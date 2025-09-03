# VM Code Injection Techniques - No Guest Modification Required

## Overview
Methods to inject code/configuration into VMs without modifying the base image. Essential for deploying custom agents, monitoring tools, or configuration from a single master image.

## 1. Cloud-Init (Most Standard)
**Used by:** AWS, Azure, GCP, OpenStack, all cloud providers  
**Requirements:** cloud-init pre-installed (standard in Ubuntu/RHEL cloud images)

### How it works:
```bash
# Create user-data.yaml with your code
# Package as ISO
cloud-localds seed.iso user-data.yaml meta-data.yaml
# Or on macOS:
hdiutil makehybrid -o seed.iso -iso -joliet -default-volume-name cidata cidata/

# Add to QEMU
-drive file=seed.iso,if=virtio,format=raw
```

### What you can inject:
- Scripts that run on boot
- Files with any content
- System configuration
- Network settings
- SSH keys
- Users and passwords

### Example for Haywire:
```yaml
#cloud-config
runcmd:
  - |
    # This runs automatically on boot
    python3 -c "
    import mmap
    # Write process list to shared memory at 0x13FFF0000
    with open('/dev/mem', 'r+b') as f:
        mem = mmap.mmap(f.fileno(), 4096, offset=0x13FFF0000)
        # Write process data here
    "
```

## 2. QEMU Firmware Config (fw_cfg)
**Built into QEMU, no guest requirements**

```bash
# Add to QEMU command
-fw_cfg name=opt/haywire/script.sh,file=/path/to/your/script.sh

# Guest can read it from:
/sys/firmware/qemu_fw_cfg/by_name/opt/haywire/script.sh/raw
```

## 3. Kernel Command Line + initrd
**Runs before init system starts**

```bash
# Create custom initrd
mkdir initrd
echo '#!/bin/sh' > initrd/init
echo 'your code here' >> initrd/init
chmod +x initrd/init
find initrd | cpio -o -H newc | gzip > custom-initrd.gz

# Add to QEMU
-initrd custom-initrd.gz
-append "init=/init"
```

## 4. 9P/VirtFS Shared Folder
**Real-time file sharing between host and guest**

```bash
# Add to QEMU
-virtfs local,path=/tmp/shared,mount_tag=hostshare,security_model=none

# Guest mounts it:
mount -t 9p -o trans=virtio hostshare /mnt/shared
```

## 5. QEMU Guest Agent Commands
**If guest agent is installed**

```python
# Execute commands in guest
qmp_command({
    "execute": "guest-exec",
    "arguments": {
        "path": "/bin/sh",
        "arg": ["-c", "your_command_here"]
    }
})
```

## 6. Memory Backend Injection
**Using existing memory-backend-file**

Since QEMU already has:
```bash
-object memory-backend-file,id=mem,size=4G,mem-path=/tmp/haywire-vm-mem,share=on
```

Guest can write to high memory addresses (e.g., 0x13FFF0000), host reads from `/tmp/haywire-vm-mem` at same offset.

## 7. SMBIOS OEM Strings
**Hide data in system information**

```bash
-smbios type=11,value="BASE64_ENCODED_SCRIPT"

# Guest reads via:
dmidecode -s oem-string | base64 -d | sh
```

## For Haywire Tomorrow

### Quick Test - Check if cloud-init works:
1. The seed.iso is already created at `/tmp/seed.iso`
2. Add to QEMU launch: `-drive file=/tmp/seed.iso,if=virtio,format=raw`
3. After boot, check: `ssh -p 2222 jff@localhost 'ls /tmp/haywire*.txt'`
4. If files exist, cloud-init works!

### Implementation Priority:
1. **Cloud-init** - Most standard, works with any cloud image
2. **Memory backend** - Fastest data transfer, you already have it set up
3. **9P/VirtFS** - Easy file sharing, good for development
4. **fw_cfg** - Simple but limited size
5. **Guest agent** - Already there but slow with JSON

### Performance Comparison:
- QMP/JSON: ~100ms per update
- Shared memory: ~0.1ms per update  
- **1000x faster!**

### Binary Format for Speed:
```c
struct process_entry {
    uint32_t pid;
    uint32_t tgid;
    uint64_t task_struct_va;
    char comm[16];
} __attribute__((packed));
// 32 bytes per process, 1000 processes = 32KB
```

## Key Insight
This is how cloud providers deploy millions of VMs from one image:
- One golden master image (never modified)
- Per-instance customization via cloud-init
- No passwords/keys/configs in the image
- Complete automation possible

## Next Steps
1. Test if your Ubuntu image accepts cloud-init
2. If yes, inject Haywire agent that writes to shared memory
3. Host reads process data instantly from memory-backend-file
4. No guest modification needed, works with any cloud image!