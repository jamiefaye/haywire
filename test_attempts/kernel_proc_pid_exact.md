# How /proc REALLY Enumerates All PIDs

## The Actual Code Path (Linux 6.x)

### 1. /proc readdir → proc_pid_readdir()
```c
// fs/proc/base.c
static int proc_pid_readdir(struct file *file, struct dir_context *ctx)
{
    struct tgid_iter iter;
    struct pid_namespace *ns = proc_pid_ns(file_inode(file)->i_sb);
    
    iter.tgid = pos - TGID_OFFSET;
    iter.task = NULL;
    for (iter = next_tgid(ns, iter);
         iter.task;
         iter.tgid += 1, iter = next_tgid(ns, iter)) {
        // Emit each PID as directory entry
    }
}
```

### 2. next_tgid() → find_ge_pid()
```c
static struct tgid_iter next_tgid(struct pid_namespace *ns, struct tgid_iter iter)
{
    struct pid *pid;
    
    pid = find_ge_pid(iter.tgid, ns);  // <-- THE KEY!
    if (pid) {
        iter.tgid = pid_nr_ns(pid, ns);
        iter.task = pid_task(pid, PIDTYPE_TGID);
    }
    return iter;
}
```

### 3. find_ge_pid() → IDR Radix Tree
```c
// kernel/pid.c
struct pid *find_ge_pid(int nr, struct pid_namespace *ns)
{
    return idr_get_next(&ns->idr, &nr);  // <-- IDR RADIX TREE!
}
```

## THE KEY STRUCTURE: IDR (ID Radix Tree)

```c
struct pid_namespace {
    struct idr idr;  // <-- THIS IS WHERE ALL PIDS ARE TRACKED!
    // ...
};
```

The IDR is a radix tree that maps PID numbers → struct pid pointers.
EVERY process is in this tree. No exceptions.

## How PIDs Are Added to IDR

```c
// kernel/pid.c - alloc_pid()
struct pid *alloc_pid(struct pid_namespace *ns, ...)
{
    // ...
    nr = idr_alloc_cyclic(&tmp->idr, pid, ...);  // <-- ADD TO IDR
    // ...
}
```

Whenever a process is created, its PID is added to the IDR.

## What We Need to Do

### Option 1: Parse the IDR Radix Tree

1. Find `init_pid_ns` (the root PID namespace)
2. Access `init_pid_ns.idr`
3. Walk the radix tree structure
4. Extract all PIDs

### Option 2: Use the Task List (for_each_process)

```c
// include/linux/sched/signal.h
#define for_each_process(p) \
    for (p = &init_task; (p = next_task(p)) != &init_task; )

#define next_task(p) \
    list_entry_rcu((p)->tasks.next, struct task_struct, tasks)
```

This walks the circular linked list starting from init_task.

## The Problem We're Having

1. We can't find the real init_task (with valid pointers)
2. The IDR tree is complex to parse
3. We're missing the kernel symbols/addresses

## Let's Find init_pid_ns Instead!

The `init_pid_ns` is easier to find than init_task:
- It's a global variable
- Contains the IDR with ALL PIDs
- More stable structure

```c
struct pid_namespace init_pid_ns = {
    .idr = {
        .idr_rt = RADIX_TREE_INIT(IDR_RT_MARKER),
        // ...
    },
    // ...
};
```