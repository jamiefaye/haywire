// Syphon frame sharing implementation for macOS
// This file must be compiled as Objective-C++ (.mm)

#include "frame_sharing.h"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>

// HAS_SYPHON is defined by CMake when Syphon.framework is found
#ifdef HAS_SYPHON
#import <Syphon/Syphon.h>
#endif

namespace Haywire {

SyphonFrameSharing::SyphonFrameSharing()
    : server_(nullptr), active_(false) {
}

SyphonFrameSharing::~SyphonFrameSharing() {
    Stop();
}

bool SyphonFrameSharing::Initialize(const std::string& name) {
#ifdef HAS_SYPHON
    if (server_) {
        Stop();
    }

    name_ = name;

    // Get the current CGL context
    CGLContextObj context = CGLGetCurrentContext();
    if (!context) {
        NSLog(@"SyphonFrameSharing: No current OpenGL context");
        return false;
    }

    NSString* serverName = [NSString stringWithUTF8String:name.c_str()];

    SyphonOpenGLServer* syphonServer = [[SyphonOpenGLServer alloc]
        initWithName:serverName
        context:context
        options:nil];

    if (!syphonServer) {
        NSLog(@"SyphonFrameSharing: Failed to create Syphon server");
        return false;
    }

    server_ = (void*)CFBridgingRetain(syphonServer);
    active_ = true;

    NSLog(@"SyphonFrameSharing: Server '%@' initialized", serverName);
    return true;
#else
    (void)name;
    return false;
#endif
}

bool SyphonFrameSharing::IsActive() const {
    return active_ && server_ != nullptr;
}

bool SyphonFrameSharing::PublishTexture(GLuint textureId, GLenum target,
                                         int width, int height, bool flipped) {
#ifdef HAS_SYPHON
    if (!server_ || !active_) {
        return false;
    }

    SyphonOpenGLServer* syphonServer = (__bridge SyphonOpenGLServer*)server_;

    NSRect region = NSMakeRect(0, 0, width, height);
    NSSize size = NSMakeSize(width, height);

    [syphonServer publishFrameTexture:textureId
                        textureTarget:target
                          imageRegion:region
                    textureDimensions:size
                              flipped:flipped ? YES : NO];

    return true;
#else
    (void)textureId; (void)target; (void)width; (void)height; (void)flipped;
    return false;
#endif
}

void SyphonFrameSharing::Stop() {
#ifdef HAS_SYPHON
    if (server_) {
        SyphonOpenGLServer* syphonServer = (SyphonOpenGLServer*)CFBridgingRelease(server_);
        [syphonServer stop];
        server_ = nullptr;
        active_ = false;
        NSLog(@"SyphonFrameSharing: Server stopped");
    }
#endif
}

std::string SyphonFrameSharing::GetName() const {
    return name_;
}

bool SyphonFrameSharing::HasClients() const {
#ifdef HAS_SYPHON
    if (!server_ || !active_) {
        return false;
    }
    SyphonOpenGLServer* syphonServer = (__bridge SyphonOpenGLServer*)server_;
    return [syphonServer hasClients];
#else
    return false;
#endif
}

// Static factory method
FrameSharing* FrameSharing::Create() {
#ifdef HAS_SYPHON
    return new SyphonFrameSharing();
#else
    return new NullFrameSharing();
#endif
}

bool FrameSharing::IsAvailable() {
#ifdef HAS_SYPHON
    return true;
#else
    return false;
#endif
}

}  // namespace Haywire

#endif  // __APPLE__
