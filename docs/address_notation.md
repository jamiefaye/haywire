# Haywire Address Notation

## Overview

Haywire uses a unified notation system for specifying addresses across different memory spaces. This document describes the notation and expression syntax.

## Address Spaces

Haywire operates with four distinct address spaces:

- **`s:`** - **Shared** memory file offset (memory-backend-file, host perspective)
- **`p:`** - **Physical** address (guest physical address as seen by guest OS)
- **`v:`** - **Virtual** address (process virtual address)
- **`c:`** - **Crunched** address (flattened/compressed virtual address space)

## Basic Notation

### Address Space Prefixes

```
s:1000      # Shared memory file offset 0x1000
p:40000000  # Physical address 0x40000000
v:7fff8000  # Virtual address 0x7fff8000
c:8000      # Crunched address 0x8000
```

### Number Formats

By default, addresses are interpreted as hexadecimal. Other formats:

```
# Hexadecimal (default for addresses)
1000        # 0x1000 in address context
0x1000      # Explicit hex prefix
1000h       # Hex with suffix
$1000       # Alternative hex prefix

# Decimal
.256        # Decimal with leading period
256.        # Decimal with trailing period
1000d       # Decimal with suffix

# Context-dependent
256         # Decimal in dimension fields, hex in address fields
```

## Arithmetic Expressions

Basic arithmetic is supported:

```
p:40000000+100    # Physical base + 0x100
v:stack-20        # Stack pointer minus 0x20
s:1000*4          # File offset 0x1000 * 4
p:40000000+.256   # Physical base + 256 decimal
```

## PID-Qualified Virtual Addresses

Virtual addresses can be qualified with a process ID:

```
v:1234:7fff8000   # VA 0x7fff8000 in PID 1234
v:nginx:stack     # Stack address in process "nginx"
v::7fff8000       # VA in current/focused PID
```

## Variables and Symbols

Built-in variables (planned):

```
$base       # User-defined variable
$ram        # Guest RAM base (typically 0x40000000)
$sp         # Current stack pointer
$pc         # Program counter
$pid        # Current process ID
```

## Special Symbols

```
stack       # Current process stack location
heap        # Current process heap
code        # Code segment
.           # Current address
..          # Parent/containing region
```

## Examples

### Navigation Commands
```
p:40000000         # Jump to physical RAM base
v:stack            # Jump to current stack
s:0                # Jump to start of memory file
c:0                # Jump to start of crunched space
```

### Complex Expressions
```
p:$ram+.1024       # RAM base + 1KB (decimal)
v:1234:stack-100   # 100 bytes before stack in PID 1234
s:($size/2)        # Middle of memory file (planned)
```

### Range Notation (planned)
```
p:40000000..40001000   # Physical address range
v:stack-100..stack     # Stack range
c:0..c:ffff            # Full crunched space
```

## UI Integration

### Input Field Behavior
- Default interpretation is hexadecimal for addresses
- Default interpretation is decimal for dimensions (width, height)
- Address space prefix changes the current viewing mode
- Invalid addresses show warning but don't halt operation

### Status Bar Display
The status bar shows the current position in all relevant address spaces:
```
s:0x1000 | p:0x40001000 | v:0x7fff1000 | c:0x8200
```

### Drag-to-Adjust (planned)
Numeric values in expressions can be adjusted by dragging:
- Normal drag: +/- 0x10
- Shift+drag: +/- page size (0x1000)
- Ctrl+drag: +/- 1MB
- Alt+drag: decimal increments

## Error Handling

Following the "show must go on" principle:
- Invalid addresses are clamped to valid ranges
- Unmapped addresses show distinctive patterns
- Parse errors fall back to simpler interpretations
- Status bar shows warnings without blocking interaction

## Implementation Phases

### Phase 1 (Current)
- Basic prefix parsing (s:/p:/v:/c:)
- Hex and decimal number formats
- Simple arithmetic (+/-)

### Phase 2 (Next)
- PID-qualified addresses
- Basic variables ($ram, $base)
- Drag-to-adjust UI

### Phase 3 (Future)
- Full expression evaluation
- User-defined variables
- Range operations
- Memory dereferencing