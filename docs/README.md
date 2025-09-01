# Haywire - Memory Visualizer

A cross-platform memory visualization tool for QEMU virtual machines, built with C++, ImGui, and OpenGL.

## Features

- Real-time memory visualization from running QEMU VMs
- Multiple pixel format interpretations (RGB, RGBA, Grayscale, Binary)
- Hex overlay display
- Pan and zoom controls
- Auto-refresh capability
- QEMU Monitor and QMP protocol support

## Prerequisites

- CMake 3.16+
- C++17 compiler
- OpenGL 3.2+
- GLFW3
- Git

### macOS
```bash
brew install cmake glfw
```

### Ubuntu/Debian
```bash
sudo apt-get install build-essential cmake git libglfw3-dev libgl1-mesa-dev
```

## Building

```bash
mkdir build
cd build
cmake ..
make -j8
```

## Running

1. Start QEMU with monitoring enabled:
```bash
qemu-system-x86_64 \
    -qmp tcp:localhost:4445,server,nowait \
    -monitor telnet:localhost:4444,server,nowait \
    [other options...]
```

2. Run Haywire:
```bash
./build/haywire
```

3. Connect to QEMU using the GUI (default ports: QMP 4445, Monitor 4444)

## Current Status

Phase 1 Complete:
- Project structure created
- CMake build system configured
- ImGui integration working
- Basic window with menus
- QEMU connection UI
- Memory visualization framework
- Hex overlay system

## Next Steps (Phase 2)

- Complete QMP protocol implementation
- Implement memory reading from QEMU
- Add memory navigation features
- Optimize rendering pipeline