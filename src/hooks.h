#pragma once

namespace hooks {
// Hooks IDXGISwapChain::Present / ResizeBuffers so the overlay draws every frame.
bool InstallD3D11();
}  // namespace hooks
