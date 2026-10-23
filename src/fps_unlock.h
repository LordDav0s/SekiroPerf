#pragma once

#include "config.h"

namespace sp::fps {

// Frame rate the unlock will target for this config.
int ResolveTargetFps(const Config& config);

// Waits for the game's code to be unpacked, removes the 60 FPS cap, then keeps the patch
// in place for the rest of the session. Blocks; run on a worker thread.
void Run(const Config& config);

}  // namespace sp::fps
