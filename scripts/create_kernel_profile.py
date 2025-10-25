#!/usr/bin/env python3
"""
Create a kernel profile JSON from pahole output.
Works without SSH - just paste pahole output or pipe it in.

Usage:
  1. Inside VM console: Run pahole commands, copy output
  2. On host: python3 create_kernel_profile.py < pahole_output.txt

  OR paste interactively:
  python3 create_kernel_profile.py
  (paste output, then Ctrl-D)
"""

import sys
import re
import json
from datetime import date

def parse_pahole_output(text):
    """Parse pahole output and extract field offsets."""
    offsets = {}
    current_struct = None

    for line in text.split('\n'):
        # Detect struct start: "struct task_struct {"
        struct_match = re.match(r'struct\s+(\w+)\s*\{', line)
        if struct_match:
            current_struct = struct_match.group(1)
            offsets[current_struct] = {}
            continue

        # Parse field: "pid_t pid; /* 888 4 */"
        # Or: "char comm[16]; /* 1144 16 */"
        field_match = re.search(r'(\w+(?:\s+\w+)?)\s+(\w+)(?:\[(\d+)\])?;\s*/\*\s+(\d+)\s+(\d+)\s*\*/', line)
        if field_match and current_struct:
            field_type = field_match.group(1).strip()
            field_name = field_match.group(2)
            array_size = field_match.group(3)
            offset = int(field_match.group(4))
            size = int(field_match.group(5))

            # Build type string
            type_str = field_type
            if array_size:
                type_str += f"[{array_size}]"

            offsets[current_struct][field_name] = {
                'offset': offset,
                'size': size,
                'type': type_str
            }

    return offsets

def create_profile_json(offsets, kernel_version="unknown", arch="unknown"):
    """Create kernel profile JSON structure."""

    profile = {
        "profile": {
            "name": f"Custom Kernel Profile - {kernel_version}",
            "kernel_version": kernel_version,
            "architecture": arch,
            "os": "Linux",
            "os_version": "unknown",
            "date_created": str(date.today()),
            "verified": False,
            "source": "pahole /sys/kernel/btf/vmlinux"
        },
        "offsets": {},
        "notes": [
            "Profile created from pahole output",
            "Verify offsets before using in production",
            "Run: pahole -C <struct> /sys/kernel/btf/vmlinux to verify"
        ]
    }

    # Add task_struct
    if 'task_struct' in offsets:
        ts = offsets['task_struct']
        profile['offsets']['task_struct'] = {
            "size": 0,  # TODO: extract from pahole if available
            "fields": {}
        }

        for field in ['pid', 'tgid', 'comm', 'mm', 'tasks']:
            if field in ts:
                profile['offsets']['task_struct']['fields'][field] = ts[field]

                # Add derived fields for list_head
                if field == 'tasks':
                    profile['offsets']['task_struct']['fields']['tasks_next'] = {
                        'offset': ts[field]['offset'],
                        'size': 8,
                        'type': 'struct list_head *'
                    }
                    profile['offsets']['task_struct']['fields']['tasks_prev'] = {
                        'offset': ts[field]['offset'] + 8,
                        'size': 8,
                        'type': 'struct list_head *'
                    }

    # Add mm_struct
    if 'mm_struct' in offsets:
        ms = offsets['mm_struct']
        profile['offsets']['mm_struct'] = {
            "size": 0,
            "fields": {}
        }

        for field in ['pgd', 'mm_mt', 'mm_users']:
            if field in ms:
                profile['offsets']['mm_struct']['fields'][field] = ms[field]

    # Add vm_area_struct
    if 'vm_area_struct' in offsets:
        vma = offsets['vm_area_struct']
        profile['offsets']['vm_area_struct'] = {
            "size": 0,
            "fields": {}
        }

        for field in ['vm_start', 'vm_end', 'vm_next', 'vm_flags', 'vm_file']:
            if field in vma:
                profile['offsets']['vm_area_struct']['fields'][field] = vma[field]

    return profile

def main():
    print("=== Kernel Profile Creator ===", file=sys.stderr)
    print("Paste pahole output below (Ctrl-D when done):", file=sys.stderr)
    print("", file=sys.stderr)

    # Read from stdin
    text = sys.stdin.read()

    if not text.strip():
        print("Error: No input received", file=sys.stderr)
        print("", file=sys.stderr)
        print("Run these commands in VM console:", file=sys.stderr)
        print("  pahole -C task_struct /sys/kernel/btf/vmlinux", file=sys.stderr)
        print("  pahole -C mm_struct /sys/kernel/btf/vmlinux", file=sys.stderr)
        print("  pahole -C vm_area_struct /sys/kernel/btf/vmlinux", file=sys.stderr)
        sys.exit(1)

    # Parse pahole output
    offsets = parse_pahole_output(text)

    if not offsets:
        print("Error: Could not parse any structure offsets", file=sys.stderr)
        sys.exit(1)

    print(f"Found structures: {', '.join(offsets.keys())}", file=sys.stderr)

    # Try to detect kernel version from input
    kernel_version = "unknown"
    version_match = re.search(r'Linux version ([\d\.\-\w]+)', text)
    if version_match:
        kernel_version = version_match.group(1)

    # Try to detect architecture
    arch = "unknown"
    if 'aarch64' in text.lower() or 'arm64' in text.lower():
        arch = 'aarch64'
    elif 'x86_64' in text.lower():
        arch = 'x86_64'

    # Create profile
    profile = create_profile_json(offsets, kernel_version, arch)

    # Output JSON
    print(json.dumps(profile, indent=2))

    print("", file=sys.stderr)
    print("Profile created successfully!", file=sys.stderr)
    print("Save to: profiles/<kernel-version>-<arch>.json", file=sys.stderr)

if __name__ == '__main__':
    main()
