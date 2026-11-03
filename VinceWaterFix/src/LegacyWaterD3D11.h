#pragma once

#include <d3d11.h>
#include <stdint.h>
#include "LegacyWaterContract.h"
#include "WaterShaderOverride.h"

namespace vince { namespace water {

struct LegacyWaterPrograms {
    ShaderBytes baseVertex;
    ShaderBytes overlayVertex;
    ShaderBytes basePixel;
    ShaderBytes overlayPixel[8]; // LegacyOverlayVariant order, not game material IDs.
};

struct LegacyWaterMesh {
    const DecodedWaterVertex* vertices;
    UINT vertexCount;
    const uint32_t* indices;
    UINT indexCount;
};

struct LegacyWaterTextures {
    ID3D11ShaderResourceView* environment;
    ID3D11ShaderResourceView* material;
    ID3D11ShaderResourceView* detail;
};

struct LegacyWaterDraw {
    LegacyVertexConstants vertex;
    LegacyPixelConstants pixel;
    LegacyWaterTextures textures;
    bool overlay;
    LegacyOverlayVariant variant;
    bool writeDepth;
    // Null uses alpha blend, <= depth test and no culling. With BOTH depth
    // overrides null, inherit the caller's stencil test and consume its stencil
    // writes only on the final pass (base when overlay is disabled).
    // The native adapter can supply the original camera/material-specific states.
    ID3D11BlendState* baseBlend;
    ID3D11BlendState* overlayBlend;
    ID3D11DepthStencilState* baseDepth;
    ID3D11DepthStencilState* overlayDepth;
    ID3D11RasterizerState* rasterizer;
};

// One instance per water surface, called on the immediate render thread.
// Owns its GPU resources; all input buffers and material SRVs remain host-owned.
// No game offsets, loader, simulation clock or shared global state live here.
class LegacyWaterD3D11 {
public:
    LegacyWaterD3D11() = default;
    ~LegacyWaterD3D11();
    LegacyWaterD3D11(const LegacyWaterD3D11&) = delete;
    LegacyWaterD3D11& operator=(const LegacyWaterD3D11&) = delete;

    // directionCube is GenerateLegacyDirectionCube's six row-major BGRA faces.
    // Initialize an empty instance; on failure all partial resources are released.
    HRESULT Initialize(ID3D11Device* device, const LegacyWaterPrograms& programs,
                       LegacyWaterMesh mesh, UINT fieldWidth, UINT fieldHeight,
                       const uint32_t* directionCube, size_t cubePixelCount);
    void Reset();

    // Both uploads must succeed before Draw is allowed. Generated normal pixels
    // are numeric ARGB / little-endian BGRA, not RGBA. Counts must match Init.
    HRESULT Upload(ID3D11DeviceContext* context, const DecodedWaterVertex* vertices,
                   UINT vertexCount, const uint32_t* bumpPixels, size_t pixelCount);
    // Leaves render targets/viewports unchanged; restores every binding/state it
    // changes, including shader class instances. It never clears or flushes.
    HRESULT Draw(ID3D11DeviceContext* context, const LegacyWaterDraw& parameters);

private:
    struct SavedState {
        ID3D11InputLayout* layout = nullptr;
        ID3D11Buffer* vertexBuffer = nullptr;
        ID3D11Buffer* indexBuffer = nullptr;
        UINT stride = 0, offset = 0, indexOffset = 0;
        DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ID3D11VertexShader* vertex = nullptr;
        ID3D11PixelShader* pixel = nullptr;
        ID3D11GeometryShader* geometry = nullptr;
        ID3D11HullShader* hull = nullptr;
        ID3D11DomainShader* domain = nullptr;
        ID3D11ClassInstance* classes[5][256] = {};
        UINT classCounts[5] = {};
        ID3D11Buffer* vertexConstants = nullptr;
        ID3D11Buffer* pixelConstants = nullptr;
        ID3D11ShaderResourceView* textures[4] = {};
        ID3D11SamplerState* samplers[4] = {};
        ID3D11BlendState* blend = nullptr;
        FLOAT blendFactor[4] = {};
        UINT sampleMask = 0, stencilReference = 0;
        ID3D11DepthStencilState* depth = nullptr;
        ID3D11RasterizerState* rasterizer = nullptr;
        void Capture(ID3D11DeviceContext* context);
        void Restore(ID3D11DeviceContext* context);
    } saved_;

    bool IsOurImmediateContext(ID3D11DeviceContext* context) const;
    HRESULT CreateResources(ID3D11Device* device, const LegacyWaterPrograms& programs,
                            LegacyWaterMesh mesh, UINT width, UINT height,
                            const uint32_t* cube);
    HRESULT PrepareInheritedStencil(ID3D11DepthStencilState* source);
    ID3D11Device* device_ = nullptr;
    ID3D11Device* viewDevice_ = nullptr; // Owner reported by our views; differs under wrappers.
    ID3D11VertexShader* vertex_[2] = {};
    ID3D11PixelShader* base_ = nullptr;
    ID3D11PixelShader* overlay_[8] = {};
    ID3D11InputLayout* layouts_[2] = {};
    ID3D11Buffer* vertices_ = nullptr;
    ID3D11Buffer* indices_ = nullptr;
    ID3D11Buffer* vertexConstants_ = nullptr;
    ID3D11Buffer* pixelConstants_ = nullptr;
    ID3D11Texture2D* bump_ = nullptr;
    ID3D11ShaderResourceView* bumpView_ = nullptr;
    ID3D11ShaderResourceView* cubeView_ = nullptr;
    ID3D11SamplerState* wrapSampler_ = nullptr;
    ID3D11SamplerState* cubeSampler_ = nullptr;
    ID3D11BlendState* alphaBlend_ = nullptr;
    ID3D11DepthStencilState* depth_[2] = {};
    ID3D11DepthStencilState* stencilSource_ = nullptr;
    // [depth writes + 2 * defer stencil writes]. Bounded to one source state.
    ID3D11DepthStencilState* inheritedDepth_[4] = {};
    ID3D11RasterizerState* rasterizer_ = nullptr;
    UINT vertexCount_ = 0, indexCount_ = 0, width_ = 0, height_ = 0;
    bool ready_ = false;
};

}} // namespace vince::water
