#pragma once

#include <string>

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

namespace Haywire {

// Abstract interface for frame sharing (Syphon on macOS, Spout on Windows)
class FrameSharing {
public:
    virtual ~FrameSharing() = default;

    // Initialize the server/sender with a name
    virtual bool Initialize(const std::string& name) = 0;

    // Check if sharing is active
    virtual bool IsActive() const = 0;

    // Publish a texture for sharing
    // Returns true if successful
    virtual bool PublishTexture(GLuint textureId, GLenum target,
                                int width, int height, bool flipped = false) = 0;

    // Stop sharing and cleanup
    virtual void Stop() = 0;

    // Get the server/sender name
    virtual std::string GetName() const = 0;

    // Check if any clients are connected (if supported)
    virtual bool HasClients() const { return true; }  // Default: assume connected

    // Create the appropriate platform-specific implementation
    static FrameSharing* Create();

    // Check if frame sharing is available on this platform
    static bool IsAvailable();
};

#ifdef __APPLE__
// Syphon implementation for macOS
class SyphonFrameSharing : public FrameSharing {
public:
    SyphonFrameSharing();
    ~SyphonFrameSharing() override;

    bool Initialize(const std::string& name) override;
    bool IsActive() const override;
    bool PublishTexture(GLuint textureId, GLenum target,
                       int width, int height, bool flipped = false) override;
    void Stop() override;
    std::string GetName() const override;
    bool HasClients() const override;

private:
    void* server_;  // SyphonOpenGLServer* (Objective-C object)
    std::string name_;
    bool active_;
};
#endif

#ifdef _WIN32
// Spout implementation for Windows
class SpoutFrameSharing : public FrameSharing {
public:
    SpoutFrameSharing();
    ~SpoutFrameSharing() override;

    bool Initialize(const std::string& name) override;
    bool IsActive() const override;
    bool PublishTexture(GLuint textureId, GLenum target,
                       int width, int height, bool flipped = false) override;
    void Stop() override;
    std::string GetName() const override;

private:
    void* spout_;  // Spout sender object
    std::string name_;
    bool active_;
    int lastWidth_, lastHeight_;
};
#endif

// Stub implementation for platforms without frame sharing
class NullFrameSharing : public FrameSharing {
public:
    bool Initialize(const std::string&) override { return false; }
    bool IsActive() const override { return false; }
    bool PublishTexture(GLuint, GLenum, int, int, bool) override { return false; }
    void Stop() override {}
    std::string GetName() const override { return ""; }
};

}  // namespace Haywire
