// Null frame sharing implementation for platforms without Syphon/Spout
// or when those libraries are not available

#include "frame_sharing.h"

// Only compile this if we're not on a platform with native support
// The platform-specific files define their own Create() and IsAvailable()
#if !defined(__APPLE__) && !defined(_WIN32)

namespace Haywire {

FrameSharing* FrameSharing::Create() {
    return new NullFrameSharing();
}

bool FrameSharing::IsAvailable() {
    return false;
}

}  // namespace Haywire

#endif
