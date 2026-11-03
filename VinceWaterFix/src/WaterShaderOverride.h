#pragma once

#include <stddef.h>

namespace vince { namespace water {

struct ShaderBytes {
    const unsigned char* data;
    size_t size;
};

// A host-owned CreatePixelShader hook calls this BEFORE forwarding to D3D11.
// The host owns both buffers and keeps the selected buffer alive through the
// CreatePixelShader call. This function allocates nothing and changes no state.
// Unknown input is returned unchanged. An empty replacement disables the fix.
ShaderBytes SelectWaterPixelShader(ShaderBytes incoming, ShaderBytes replacement);

}} // namespace vince::water
