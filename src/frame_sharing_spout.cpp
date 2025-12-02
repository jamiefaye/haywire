// Spout frame sharing implementation for Windows
// Requires Spout2 SDK

#include "frame_sharing.h"

#ifdef _WIN32

// Check if Spout headers are available
#if __has_include("SpoutLibrary.h") || __has_include("Spout.h")
#define HAS_SPOUT 1
#include "Spout.h"
#else
#define HAS_SPOUT 0
#endif

namespace Haywire {

SpoutFrameSharing::SpoutFrameSharing()
    : spout_(nullptr), active_(false), lastWidth_(0), lastHeight_(0) {
}

SpoutFrameSharing::~SpoutFrameSharing() {
    Stop();
}

bool SpoutFrameSharing::Initialize(const std::string& name) {
#if HAS_SPOUT
    if (spout_) {
        Stop();
    }

    name_ = name;

    // Create Spout sender
    Spout* sender = new Spout();
    if (!sender) {
        return false;
    }

    // Set the sender name
    sender->SetSenderName(name.c_str());

    // Set format to RGBA
    sender->SetSenderFormat(GL_RGBA);

    spout_ = sender;
    active_ = true;

    return true;
#else
    (void)name;
    return false;
#endif
}

bool SpoutFrameSharing::IsActive() const {
    return active_ && spout_ != nullptr;
}

bool SpoutFrameSharing::PublishTexture(GLuint textureId, GLenum target,
                                        int width, int height, bool flipped) {
#if HAS_SPOUT
    if (!spout_ || !active_) {
        return false;
    }

    Spout* sender = static_cast<Spout*>(spout_);

    // Track size changes
    lastWidth_ = width;
    lastHeight_ = height;

    // SendTexture handles sender creation/update automatically
    return sender->SendTexture(textureId, target, width, height, !flipped, 0);
#else
    (void)textureId; (void)target; (void)width; (void)height; (void)flipped;
    return false;
#endif
}

void SpoutFrameSharing::Stop() {
#if HAS_SPOUT
    if (spout_) {
        Spout* sender = static_cast<Spout*>(spout_);
        sender->ReleaseSender();
        delete sender;
        spout_ = nullptr;
        active_ = false;
    }
#endif
}

std::string SpoutFrameSharing::GetName() const {
    return name_;
}

// Static factory method
FrameSharing* FrameSharing::Create() {
#if HAS_SPOUT
    return new SpoutFrameSharing();
#else
    return new NullFrameSharing();
#endif
}

bool FrameSharing::IsAvailable() {
#if HAS_SPOUT
    return true;
#else
    return false;
#endif
}

}  // namespace Haywire

#endif  // _WIN32
