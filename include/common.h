#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <string>

namespace Haywire {

struct MemoryBlock {
    uint64_t address;
    std::vector<uint8_t> data;
    size_t stride;
    
    MemoryBlock() : address(0), stride(0) {}
    MemoryBlock(uint64_t addr, size_t size) 
        : address(addr), data(size), stride(0) {}
};

struct PixelFormat {
    enum Type {
        RGB888,
        RGBA8888,
        RGB565,
        GRAYSCALE,
        BINARY,
        CUSTOM
    };
    
    Type type;
    int bytesPerPixel;
    
    PixelFormat(Type t = RGB888) : type(t) {
        switch(t) {
            case RGB888: bytesPerPixel = 3; break;
            case RGBA8888: bytesPerPixel = 4; break;
            case RGB565: bytesPerPixel = 2; break;
            case GRAYSCALE: bytesPerPixel = 1; break;
            case BINARY: bytesPerPixel = 1; break;
            default: bytesPerPixel = 1;
        }
    }
};

struct ViewportSettings {
    uint64_t baseAddress;
    size_t width;
    size_t height;
    size_t stride;
    PixelFormat format;
    float zoom;
    float panX, panY;
    
    ViewportSettings() 
        : baseAddress(0), width(256), height(256), stride(256),
          format(PixelFormat::RGB888), zoom(1.0f), panX(0), panY(0) {}
};

inline uint32_t PackRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

inline void UnpackRGBA(uint32_t color, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    r = color & 0xFF;
    g = (color >> 8) & 0xFF;
    b = (color >> 16) & 0xFF;
    a = (color >> 24) & 0xFF;
}

inline uint32_t ContrastColor(uint32_t background) {
    uint8_t r, g, b, a;
    UnpackRGBA(background, r, g, b, a);
    float luminance = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
    return luminance > 0.5f ? 0xFF000000 : 0xFFFFFFFF;
}

}