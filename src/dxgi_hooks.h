#pragma once

#include "config.h"

namespace sp::dxgi {

// Hooks swap chain creation. Depending on config this wraps the swap chain in the FSR upscaling
// proxy, raises Sekiro's forced 60 Hz fullscreen to the monitor's highest refresh rate, and/or
// turns off forced VSync. Must run before the game creates its swap chain; call from a worker
// thread, not DllMain.
void Install(const Config& config);

}  // namespace sp::dxgi
