# Kernel /proc PID Enumeration Analysis

## How Linux Kernel Lists Processes in /proc

### Entry Point: fs/proc/base.c

The `/proc` filesystem generates PID directories on-the-fly when you list `/proc`.

### Key Function: `proc_pid_readdir()`

```c
// fs/proc/base.c
static int proc_pid_readdir(struct file *file, struct dir_context *ctx)
{
    struct tgid_iter iter;
    struct pid_namespace *ns = proc_pid_ns(file_inode(file)->i_sb);
    loff_t pos = ctx->pos;

    if (pos >= PID_MAX_LIMIT + TGID_OFFSET)
        return 0;

    if (pos == TGID_OFFSET - 2) {
        struct inode *inode = d_inode(ns->proc_self);
        if (!dir_emit(ctx, "self", 4, inode->i_ino, DT_LNK))
            return 0;
        ctx->pos = pos = pos + 1;
    }
    if (pos == TGID_OFFSET - 1) {
        struct inode *inode = d_inode(ns->proc_thread_self);
        if (!dir_emit(ctx, "thread-self", 11, inode->i_ino, DT_LNK))
            return 0;
        ctx->pos = pos = pos + 1;
    }

    iter.tgid = pos - TGID_OFFSET;
    iter.task = NULL;
    for (iter = next_tgid(ns, iter);
         iter.task;
         iter.tgid += 1, iter = next_tgid(ns, iter)) {
        char name[10 + 1];
        unsigned int len;

        cond_resched();
        if (!has_pid_permissions(fs_info, iter.task, HIDEPID_INVISIBLE))
            continue;

        len = snprintf(name, sizeof(name), "%u", iter.tgid);
        ctx->pos = iter.tgid + TGID_OFFSET;
        if (!proc_fill_cache(file, ctx, name, len,
                          proc_pid_instantiate, iter.task, NULL)) {
            put_task_struct(iter.task);
            return 0;
        }
    }
    ctx->pos = PID_MAX_LIMIT + TGID_OFFSET;
    return 0;
}
```

### The Magic: `next_tgid()` Function

```c
static struct tgid_iter next_tgid(struct pid_namespace *ns, struct tgid_iter iter)
{
    struct pid *pid;

    if (iter.task)
        put_task_struct(iter.task);
    rcu_read_lock();
retry:
    iter.task = NULL;
    pid = find_ge_pid(iter.tgid, ns);  // <-- KEY FUNCTION!
    if (pid) {
        iter.tgid = pid_nr_ns(pid, ns);
        iter.task = pid_task(pid, PIDTYPE_TGID);
        if (!iter.task) {
            iter.tgid += 1;
            goto retry;
        }
        get_task_struct(iter.task);
    }
    rcu_read_unlock();
    return iter;
}
```

### The Core: PID Management (kernel/pid.c)

```c
struct pid *find_ge_pid(int nr, struct pid_namespace *ns)
{
    return idr_get_next(&ns->idr, &nr);  // <-- IDR radix tree!
}
```

## KEY DISCOVERY: Linux Uses an IDR (ID Radix Tree)

The kernel doesn't walk task_struct linked lists to enumerate PIDs!
Instead it uses:

1. **struct pid_namespace** - Contains an IDR (radix tree) of all PIDs
2. **idr_get_next()** - Efficiently finds next PID in numerical order
3. **pid_task()** - Converts PID to task_struct

### The IDR Structure

```c
struct pid_namespace {
    struct idr idr;              // <-- Radix tree of PIDs!
    struct rcu_head rcu;
    unsigned int pid_allocated;  // Number of PIDs allocated
    struct task_struct *child_reaper;
    struct kmem_cache *pid_cachep;
    unsigned int level;
    struct pid_namespace *parent;
    // ...
};
```

### How PIDs are Organized

```c
struct pid {
    refcount_t count;
    unsigned int level;
    spinlock_t lock;
    /* lists of tasks that use this pid */
    struct hlist_head tasks[PIDTYPE_MAX];
    struct hlist_head inodes;
    /* wait queue for pidfd notifications */
    wait_queue_head_t wait_pidfd;
    struct rcu_head rcu;
    struct upid numbers[1];
};
```

## The Complete Picture

1. **init_pid_ns** - Global variable pointing to root PID namespace
2. **init_pid_ns.idr** - Radix tree containing all PIDs
3. Each PID maps to a **struct pid** (not task_struct!)
4. **struct pid** has lists of tasks using that PID
5. **pid_task()** gets the task_struct from struct pid

## For Memory Introspection

To enumerate ALL processes like `/proc` does:

1. Find `init_pid_ns` (global variable)
2. Access `init_pid_ns.idr`
3. Walk the IDR radix tree
4. For each PID entry, get the associated task_struct

### Alternative: The tasklist_lock Method

```c
// kernel/fork.c
RWLOCK(tasklist_lock);
static LIST_HEAD(init_task.tasks);  // All tasks linked here

for_each_process(p) {  // Macro that walks init_task.tasks
    // Process p
}
```

## Why Our Current Approach Misses Some

1. We're scanning memory for task_structs
2. We're not using the kernel's PID tracking structures
3. Some processes might be:
   - In different memory regions
   - Allocated via different mechanisms
   - Hidden by namespace isolation

## Actionable Next Steps

1. **Find init_pid_ns address** (via kallsyms or known offset)
2. **Parse the IDR radix tree** structure
3. **Walk all PID entries** to find task_structs
4. This would give us 100% process discovery!

## Simpler Alternative: Walk init_task.tasks

Since we found init_task (PID 1), we can:
1. Start from init_task
2. Follow tasks.next pointers
3. Walk the entire circular list
4. This visits ALL task_structs!

The linked list approach is simpler than IDR parsing and should give 100% coverage.