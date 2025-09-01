# Building QEMU with Custom Patches

## Quick Build (15-20 minutes on M1/M2 Mac)

```bash
# 1. Clone QEMU
git clone https://gitlab.com/qemu-project/qemu.git
cd qemu
git checkout v9.1.0  # Use stable version

# 2. Install dependencies (you probably have these)
brew install ninja pixman glib pkg-config

# 3. Configure (minimal build, just ARM64)
mkdir build
cd build
../configure --target-list=aarch64-softmmu \
             --enable-hvf \
             --enable-cocoa \
             --prefix=/usr/local/qemu-custom

# 4. Build (fast on Apple Silicon)
make -j8

# 5. Test without installing
./qemu-system-aarch64 --version

# 6. Optional: Install
sudo make install
```

## Adding a TTBR Exposure Patch

The patch would be simple - add a QMP command to read system registers:

**File: qmp-commands.hx**
```c
{
    .name       = "query-arm-sysregs",
    .args_type  = "cpu:i",
    .mhandler.cmd_new = qmp_marshal_query_arm_sysregs,
},
```

**File: target/arm/monitor.c**
```c
SysRegInfo *qmp_query_arm_sysregs(int64_t cpu, Error **errp)
{
    CPUState *cs = qemu_get_cpu(cpu);
    ARMCPU *armcpu = ARM_CPU(cs);
    CPUARMState *env = &armcpu->env;
    
    SysRegInfo *info = g_malloc0(sizeof(*info));
    
    // Read TTBR0_EL1 and TTBR1_EL1
    info->ttbr0_el1 = env->cp15.ttbr0_el[1];
    info->ttbr1_el1 = env->cp15.ttbr1_el[1];
    info->tcr_el1 = env->cp15.tcr_el[1].raw_tcr;
    
    return info;
}
```

## Even Simpler: Use Existing GDB Stub

Actually, QEMU's GDB stub already has access to system registers! We just need to use the right commands:

```gdb
# In GDB connected to QEMU:
(gdb) maintenance packet qqemu.sstep
(gdb) info all-registers
```

Or via monitor commands through QMP:
```bash
echo '{"execute": "human-monitor-command", "arguments": {"command-line": "info registers -a"}}' | nc localhost 4445
```

## Build Time

- Full QEMU build: ~20-30 minutes first time
- Incremental builds: ~2-3 minutes
- Just ARM target: ~10-15 minutes

## Alternative: Use QEMU Plugin API

Instead of patching QEMU, we could write a plugin that exposes TTBR:

```c
// ttbr_plugin.c
#include <qemu-plugin.h>

static void vcpu_sysreg_read(unsigned int cpu_index, void *udata)
{
    // Read TTBR0_EL1, TTBR1_EL1
    // Write to shared memory for Haywire to read
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    // Register callbacks for system register access
    return 0;
}
```

Compile with:
```bash
gcc -shared -fPIC -o ttbr_plugin.so ttbr_plugin.c
```

Use with:
```bash
qemu-system-aarch64 -plugin ./ttbr_plugin.so ...
```

## Recommendation

1. **Try QMP monitor commands first** - might already expose what we need
2. **Plugin approach** - no QEMU rebuild needed
3. **Patch QEMU** - most flexible but requires maintenance

The build itself is easy, the main work is understanding QEMU's internals (which are well documented).