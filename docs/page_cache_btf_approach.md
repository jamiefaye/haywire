# Page Cache Discovery Using BTF

## Overview
Use BTF data to automatically find struct offsets, eliminating most reverse engineering.

## Step 1: Get BTF Data

### Option A: From Running System
```python
def get_btf_from_system():
    """Get BTF from target system via QGA"""
    # Use QEMU Guest Agent to read /sys/kernel/btf/vmlinux
    btf_data = qga_read_file("/sys/kernel/btf/vmlinux")
    return btf_data
```

### Option B: From BTFHub
```python
def get_btf_from_btfhub(kernel_version, distro):
    """Download BTF from BTFHub"""
    # Example: Ubuntu 5.15.0-88-generic
    url = f"https://github.com/aquasecurity/btfhub-archive/raw/main/{distro}/{kernel_version}.btf"
    return download(url)
```

### Option C: Pre-built Database
```python
# Ship with common offsets pre-extracted
OFFSET_DB = {
    "5.15.0-88-generic": {
        "super_block.s_list": 0x30,
        "super_block.s_inodes": 0x60,
        "inode.i_mapping": 0x48,
        "address_space.i_pages": 0x0,
        "xa_node.shift": 0x8,
        "xa_node.slots": 0x40,
    },
    # ... more kernels
}
```

## Step 2: Parse BTF for Offsets

```python
import btfparse  # Hypothetical BTF parser

def extract_offsets(btf_data):
    """Extract all needed offsets from BTF"""
    btf = btfparse.parse(btf_data)

    offsets = {}

    # Get superblock offsets
    sb = btf.find_struct("super_block")
    offsets["super_block.s_list"] = sb.find_field("s_list").offset
    offsets["super_block.s_inodes"] = sb.find_field("s_inodes").offset
    offsets["super_block.s_bdev"] = sb.find_field("s_bdev").offset

    # Get inode offsets
    inode = btf.find_struct("inode")
    offsets["inode.i_mapping"] = inode.find_field("i_mapping").offset
    offsets["inode.i_ino"] = inode.find_field("i_ino").offset
    offsets["inode.i_size"] = inode.find_field("i_size").offset

    # Get address_space offsets
    as_struct = btf.find_struct("address_space")
    offsets["address_space.i_pages"] = as_struct.find_field("i_pages").offset
    offsets["address_space.nrpages"] = as_struct.find_field("nrpages").offset

    # Get xarray offsets (for walking cached pages)
    xa = btf.find_struct("xarray")
    offsets["xarray.xa_head"] = xa.find_field("xa_head").offset

    xa_node = btf.find_struct("xa_node")
    offsets["xa_node.shift"] = xa_node.find_field("shift").offset
    offsets["xa_node.slots"] = xa_node.find_field("slots").offset

    return offsets
```

## Step 3: Find Starting Points

```python
def find_super_blocks(memory, offsets):
    """Find the superblock list head"""

    # Method 1: Look for known symbol
    # 'super_blocks' is the global list head
    sb_list = find_kernel_symbol("super_blocks")

    # Method 2: Scan for superblock signatures
    # Most superblocks have magic numbers
    # ext4: 0xEF53
    # btrfs: "_BHRfS_M"

    superblocks = []
    for sb in walk_list(sb_list, offsets["super_block.s_list"]):
        superblocks.append(sb)

    return superblocks
```

## Step 4: Walk the Page Cache

```python
def discover_page_cache(memory, offsets):
    """Main page cache discovery"""

    cached_pages = {}

    for sb in find_super_blocks(memory, offsets):
        # Get filesystem type
        fs_type = read_fs_type(sb)

        # Walk all inodes on this filesystem
        for inode_addr in walk_inode_list(sb, offsets):
            # Get inode number
            i_ino = memory.read_u64(inode_addr + offsets["inode.i_ino"])

            # Get address_space (contains cached pages)
            i_mapping = memory.read_u64(inode_addr + offsets["inode.i_mapping"])
            if not i_mapping:
                continue

            # Get number of cached pages
            nrpages = memory.read_u64(i_mapping + offsets["address_space.nrpages"])
            if nrpages == 0:
                continue

            # Walk the xarray to find actual pages
            xa_head = memory.read_u64(i_mapping + offsets["xarray.xa_head"])

            for page_addr in walk_xarray(xa_head, memory, offsets):
                pfn = page_to_pfn(page_addr)
                cached_pages[pfn] = {
                    "type": "page_cache",
                    "filesystem": fs_type,
                    "inode": i_ino,
                    "file_size": memory.read_u64(inode_addr + offsets["inode.i_size"])
                }

    return cached_pages
```

## Step 5: Walking XArray (Tricky Part)

```python
def walk_xarray(xa_head, memory, offsets):
    """Walk xarray/radix tree to find pages"""

    pages = []

    # Check if it's a single entry or node
    if xa_head & 3 == 0:  # Direct pointer to page
        if is_page_addr(xa_head):
            pages.append(xa_head)
    else:
        # It's an xa_node - need to walk it
        node = xa_head & ~0x3
        shift = memory.read_u8(node + offsets["xa_node.shift"])

        # Walk slots recursively
        slots_ptr = node + offsets["xa_node.slots"]
        for i in range(64):  # XA_CHUNK_SIZE
            slot = memory.read_u64(slots_ptr + i * 8)
            if slot:
                if shift == 0:  # Leaf level
                    pages.append(slot & ~0x3)
                else:  # Recurse
                    pages.extend(walk_xarray(slot, memory, offsets))

    return pages
```

## Minimal RE Requirements

With BTF, we only need to find:
1. **super_blocks list head** - Usually a kernel symbol
2. **Page struct layout** - To convert addresses to PFNs

Everything else comes from BTF!

## Expected Output

```python
# After discovery
{
    0x12340000: {
        "type": "page_cache",
        "filesystem": "ext4",
        "inode": 12345,  # Can map to filename via dentries
        "file_size": 65536
    },
    0x12341000: {
        "type": "page_cache",
        "filesystem": "ext4",
        "inode": 12345,
        "file_size": 65536
    },
    # ... thousands more pages
}
```

## Challenges

1. **XArray complexity** - Replaced radix tree in 4.20+
2. **Filesystem differences** - Each FS organizes inodes differently
3. **Compound pages** - Large pages need special handling
4. **Swap cache** - Similar structure but different location

## Benefits

- Shows which files are hot in memory
- Reveals I/O patterns
- Can identify memory pressure (low cache = high pressure)
- Helps understand application behavior

## Implementation Priority

1. **Get BTF working** - Essential for everything
2. **Find super_blocks** - Starting point
3. **Walk simple filesystems** - ext4, tmpfs
4. **Handle XArray** - Most complex part
5. **Add filesystem-specific logic** - btrfs, xfs, etc.