// The runtime's single Direct3D 11 device (ADR 0014), shared by the presenter (window thread) and the GPU renderer of the GS
// (GS thread). Backend files only: D3D types do not leave them.
#pragma once
#include <mutex>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace gfx {

// The device and its immediate context, created on the first call; both AddRef'd (the caller releases them). False when
// Direct3D 11 is unavailable.
bool Device(ID3D11Device** dev, ID3D11DeviceContext** ctx);
// Held around every sequence of calls on the immediate context (a presenter frame, a renderer batch): the two threads set
// their own pipeline state, which the other must not change between their calls.
std::recursive_mutex& Lock();

}  // namespace gfx
