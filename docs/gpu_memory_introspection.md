# GPU Memory Introspection - Technical Analysis

## Overview

This document explores the challenges and possibilities of GPU memory introspection, particularly in the context of extending Haywire to capture GPU-resident data. It covers native GPU debugging, web-based approaches, and the fundamental architectural barriers.

## Table of Contents

1. [Memory Architecture](#memory-architecture)
2. [Native GPU Debugging](#native-gpu-debugging)
3. [Web Platform (WebGL/WebGPU)](#web-platform-webglwebgpu)
4. [Electron as a Solution](#electron-as-a-solution)
5. [Practical Implications](#practical-implications)

## Memory Architecture

### Traditional vs Unified Memory

**Traditional PC Architecture:**
```
CPU ← PCIe bus → GPU
 ↓                 ↓
System RAM      Video RAM
(DDR5)          (GDDR6)
[Separate]      [Separate]
```

**Apple M3 Unified Memory:**
```
CPU ← Same die → GPU
     ↓        ↓
  Shared LPDDR5 RAM
  [Single pool]
```

Key points:
- M3 has genuinely unified memory - single physical RAM pool
- No copying needed between CPU↔GPU on M3
- However, memory protection still applies via page-level flags
- QEMU guests don't inherit unified memory benefits

### Why GPU Memory is "Invisible"

1. **Memory Protection**: GPU pages marked with special protection bits
2. **Address Space Isolation**: GPU uses separate virtual address space
3. **Driver Control**: Kernel drivers mediate all GPU memory access
4. **Hardware Features**: IOMMUs provide hardware-enforced isolation

## Linux Graphics Stack (DRM/KMS)

### DRM (Direct Rendering Manager)
- Kernel subsystem managing GPU access
- Controls GPU memory allocation via GEM (Graphics Execution Manager)
- Provides userspace API via `/dev/dri/card*`

### KMS (Kernel Mode Setting)
- Handles display configuration (resolution, refresh rate, multi-monitor)
- Manages display pipeline (framebuffers → displays)
- Replaced old userspace X.org mode setting

### Impact on Memory Visibility

Modern Linux graphics keeps pixels invisible to Haywire:
- **GEM Objects**: Allocated in kernel space, not normal RAM
- **DMA-BUF**: Shared buffers stay in GPU/kernel space
- **Framebuffers**: Managed by KMS in GPU memory
- **Wayland**: Direct DRM/KMS usage bypasses system RAM

Where pixels might still appear:
- Software rendering fallback (llvmpipe/swrast)
- Image decoding (PNG/JPEG → raw pixels temporarily)
- Legacy X11 applications
- VNC/remote desktop buffers

## Finding Images in Memory

### Image Asset Lifecycle

```
Compressed (PNG/JPEG) → Decoder → Raw Pixels → GPU Upload → GPU Memory
     [.data section]      [heap]     [heap]      [brief]     [invisible]
         ✅                ✅          ✅           ✅            ❌
```

### Common Formats on ARM Linux

1. **Framebuffer formats:**
   - RGB888/BGR888 (24-bit, most common)
   - RGB565 (16-bit, embedded systems)
   - XRGB8888 (32-bit with ignored alpha)

2. **Why fewer images than Windows:**
   - Linux uses server-side rendering (X11/Wayland)
   - Framebuffers in video memory, not system RAM
   - DRM/KMS keeps buffers in kernel space
   - Shared memory (`/dev/shm`) not visible via memory-backend-file

### Search Strategies

Look for:
- PNG headers: `89 50 4E 47`
- JPEG headers: `FF D8 FF`
- Large contiguous regions (e.g., 1920×1080×4 bytes)
- Heap allocations near image decoder libraries

## Native GPU Debugging

### How GPU Debuggers Work

GPU debuggers bypass normal protection through:

1. **Driver Integration**: Direct hooks into graphics drivers
2. **API Interception**: Library injection (LD_PRELOAD) or API layers
3. **Debug Contexts**: Special GPU modes for debugging
4. **Kernel Privileges**: Driver-level memory access

### Vendor Fragmentation

The GPU debugging landscape is extremely fragmented:

**NVIDIA:**
- Graphics: NSight Graphics (proprietary)
- Compute: CUDA-GDB (different tool)
- Windows: NvAPI (requires NDA)

**AMD:**
- Radeon GPU Profiler (Vulkan/D3D12)
- ROCm debugger (compute only)
- Different tools per API

**Intel:**
- Intel Graphics Performance Analyzers
- Platform-specific (Linux uses debugfs, Windows uses ETW)

**Apple:**
- Metal debugger only (Xcode exclusive)
- No OpenGL/Vulkan debugging support

### Why Haywire Can't Use These Methods

Haywire operates at VM guest memory level:
- No driver access in guest
- No API hooks possible
- No kernel privileges in guest
- Can't inject into guest processes

## Web Platform (WebGL/WebGPU)

### The Standardized Layer

Web GPU APIs are surprisingly more portable than native:

```javascript
// This works on ANY browser, ANY GPU!
const spector = new SPECTOR.Spector();
spector.captureCanvas(canvas);  // Universal WebGL capture

// WebGPU has debugging in the spec
const device = await adapter.requestDevice({
    requiredFeatures: ['timestamp-query']
});
```

### Browser Security Barriers

However, browser security prevents Haywire-like introspection:

1. **Process Isolation**: Each tab/origin in separate process
2. **CORS/COEP/COOP**: Cross-origin restrictions
3. **Canvas Tainting**: Can't read cross-origin textures
4. **Permission Model**: Requires user consent for access
5. **Fingerprinting Protection**: Browsers add noise, hide GPU info

### WebGL Memory Restrictions

```javascript
gl.readPixels(...);  // Throttled for cross-origin
// Returns zeros for security
// Cannot access other contexts
// Cannot read uninitialized memory
```

## Electron as a Solution

### What Electron Enables

Electron removes browser security restrictions:

```javascript
// Main process
app.commandLine.appendSwitch('disable-web-security');
app.commandLine.appendSwitch('disable-gpu-sandbox');
app.commandLine.appendSwitch('enable-unsafe-webgpu');

// Renderer with nodeIntegration
const memwatch = require('memwatch-next');
const nativeMemory = require('./build/Release/memory_reader.node');
```

### Electron Advantages

✅ **Can do:**
- Read all JavaScript heap memory
- Intercept all WebGL/WebGPU calls
- Access any origin's canvases
- Use native modules for memory scanning
- Disable all browser security
- Debug other Electron apps

❌ **Still can't:**
- Read actual GPU VRAM
- Bypass GPU driver isolation
- Access unmapped WebGPU buffers
- Use vendor-specific GPU debugging

### Practical Electron-Haywire Architecture

```javascript
class ElectronHaywire {
    constructor() {
        // Native memory access
        this.memoryReader = require('./native/memory_reader');

        // WebGL interception
        this.hookWebGL();

        // GPU debugging flags
        app.commandLine.appendSwitch('enable-gpu-debugging');
    }

    hookWebGL() {
        const originalGetContext = HTMLCanvasElement.prototype.getContext;
        HTMLCanvasElement.prototype.getContext = function(type, ...args) {
            const ctx = originalGetContext.call(this, type, ...args);
            if (type.includes('webgl')) {
                interceptAllCalls(ctx);
            }
            return ctx;
        };
    }
}
```

## Practical Implications

### For Haywire Development

1. **Current Approach is Optimal**: Reading guest RAM directly sidesteps vendor fragmentation
2. **GPU Memory Remains Inaccessible**: Without guest agent with driver privileges
3. **Focus on CPU-Visible Data**: Images during decode, software rendering, cached textures

### For Finding Images

**Most likely to find:**
- Decoded images in heap (temporary during loading)
- Software-rendered content
- Image thumbnails and icons
- VNC/remote desktop buffers

**Unlikely to find:**
- Active framebuffers (in GPU)
- OpenGL/Vulkan textures (uploaded to GPU)
- Wayland compositor buffers (DRM/KMS managed)
- Hardware video decode output (DMA buffers)

### Platform-Specific Considerations

**Linux Guest (ARM/x86):**
- Look for software rendering fallbacks
- Check for X11 legacy applications
- Monitor image decoder output

**Windows Guest:**
- More likely to find GDI bitmaps in RAM
- Desktop Window Manager buffers sometimes visible
- More software rendering in legacy apps

**macOS Guest:**
- Least likely to find images (Metal everywhere)
- Look for image decode caches
- PDF/Preview app buffers sometimes visible

## Optimization Results

Recent optimizations to Haywire's memory scanning:

1. **Zero-copy scanning**: Using `TestPageNonZero` instead of copying data
2. **Performance improvements:**
   - PA mode: 45ms → 7.8ms (5.8x speedup)
   - VA mode: 60ms → 7ms (8.5x speedup)
3. **Scan coverage increased:**
   - PA mode: 10k → 30k pages (40MB → 120MB)
   - VA mode: 1k → 3k pages (4MB → 12MB)

## Conclusion

GPU memory introspection faces fundamental architectural barriers:
- Hardware-enforced isolation
- Vendor fragmentation
- Security models (especially in browsers)

Haywire's approach of reading system RAM remains the most practical and portable solution, even though it misses GPU-resident data. For web-based approaches, Electron provides the best compromise between access and portability.

## Future Possibilities

1. **Guest Agent with Driver Privileges**: Could access GPU memory from inside guest
2. **QEMU virtio-gpu Modifications**: Intercept at virtualization layer
3. **Electron-based Companion**: For web app memory introspection
4. **Machine Learning**: Detect image patterns in compressed/encrypted data

---

*Last updated: September 2024*
*Related: CLAUDE.md, docs/qemu_memory_introspection.md*