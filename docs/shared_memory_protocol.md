# Haywire Shared Memory Protocol

## Overview

The Haywire shared memory protocol enables multiple host-side Haywire instances to communicate with a single guest VM companion process through QEMU's memory-backend-file. This replaces the external memory introspection approach with a cooperative "through channels" design.

## Architecture

### Memory Layout

The protocol uses a 64MB shared memory region starting at the beacon offset (typically 0x0a670000):

```
Page 0:      Beacon (4KB) - Contains magic numbers and metadata
Pages 1-16:  Request slots (16 slots × 256 bytes each)
Pages 17-32: Response slots (16 slots × 4KB each)  
Page 33:     Iterator table (8 iterators)
Pages 34+:   Reserved for future use
```

### Key Components

1. **Guest Companion** (`companion_handler.c`)
   - Runs inside the VM
   - Creates `/dev/shm/vm-pagebeacon` shared memory
   - Processes requests and writes responses
   - Manages iterator state for chunked responses

2. **Haywire Client** (`haywire_client.cpp`)
   - Runs on host
   - Finds beacon pattern in `/tmp/haywire-vm-mem`
   - Claims request slots using PID-based ownership
   - Handles chunked responses via iterators

3. **Beacon Discovery** (`beacon_discovery.cpp`)
   - Scans memory file for 0x3142FACE magic pattern
   - Validates beacon structure
   - Maps shared memory region

## Protocol Flow

### 1. Connection Phase
```
Haywire → Scan memory for beacon (0x3142FACE)
        → Map shared memory at beacon offset
        → Initialize with PID as station ID
```

### 2. Request Phase
```
Haywire → Claim request slot (atomic test-and-set)
        → Fill request structure
        → Set magic number to signal ready
        → Wait for response
```

### 3. Response Phase
```
Companion → Poll request slots
          → Process request
          → Fill response slot
          → Set response magic
          → Clear request magic
```

### 4. Iterator Management
For large result sets:
```
Haywire → Send initial request
        → Receive partial response + iterator_id
        → Send REQ_CONTINUE_ITERATION with iterator_id
        → Repeat until RESP_COMPLETE
```

## Multi-Client Support

Multiple Haywire instances can operate simultaneously using:

1. **PID-based Station IDs**: Each Haywire uses its process ID as unique identifier
2. **Atomic Slot Claims**: Test-and-set operations prevent race conditions
3. **LRU Iterator Management**: Fixed-size table with automatic eviction

### Slot Claiming Algorithm
```c
int claim_request_slot(Request* slots, uint32_t my_pid) {
    for (int i = 0; i < MAX_REQUEST_SLOTS; i++) {
        if (__sync_bool_compare_and_swap(&slots[i].owner_pid, 0, my_pid)) {
            slots[i].magic = 0x3142FACE;
            return i;
        }
    }
    return -1;  // No slots available
}
```

## Request Types

- `REQ_LIST_PROCESSES` - Get process list (chunked)
- `REQ_GET_PROCESS_INFO` - Get single process details
- `REQ_CONTINUE_ITERATION` - Get next chunk
- `REQ_CANCEL_ITERATION` - Cancel chunked operation
- `REQ_GET_MEMORY_MAP` - Get process memory map (future)
- `REQ_READ_MEMORY` - Read process memory (future)

## Building and Testing

### Build Everything
```bash
make all
```

### Deploy to VM
```bash
# Copy and compile companion in VM
ssh -p 2222 jff@localhost  # password: p
# Inside VM:
gcc -o /tmp/companion_handler /tmp/companion_handler.c
/tmp/companion_handler &
```

### Test Multi-Client
```bash
make test  # Runs multiple concurrent Haywire instances
```

## Performance Characteristics

- **Latency**: ~1-10ms per request/response cycle
- **Throughput**: Up to 50 processes per chunk
- **Concurrency**: 16 simultaneous requests
- **Iterator Capacity**: 8 concurrent iterations
- **Memory Overhead**: 64MB shared memory region

## Security Considerations

This protocol requires trust between guest and host:
- Guest companion has full `/proc` access
- No authentication between Haywire and companion
- Shared memory is world-readable if permissions not restricted
- Should only be used in controlled environments

## Future Enhancements

1. **Authentication**: Add HMAC-based request signing
2. **Compression**: Compress large responses
3. **Notifications**: Push updates from guest to host
4. **Memory Operations**: Direct memory read/write support
5. **Windows Support**: EPROCESS walking for Windows guests