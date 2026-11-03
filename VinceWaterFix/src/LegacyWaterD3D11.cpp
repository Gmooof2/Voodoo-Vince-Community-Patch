#include "LegacyWaterD3D11.h"
#include "LegacyWaterCube.h"
#include <string.h>
#include <limits.h>

namespace vince { namespace water {
namespace {
template<class T> void Release(T*& object) {
    if (object) { object->Release(); object = nullptr; }
}
bool ValidCode(ShaderBytes code) { return code.data && code.size >= 32; }
bool PowerOfTwo(UINT value) { return value && !(value & (value - 1)); }

// Device wrappers such as ReShade return their proxy from creation but leave
// views reporting the wrapped device, so accept either identity of ours.
bool CompatibleView(ID3D11ShaderResourceView* view, ID3D11Device* device,
                    ID3D11Device* viewDevice, D3D11_SRV_DIMENSION dimension) {
    if (!view) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC description;
    view->GetDesc(&description);
    if (description.ViewDimension != dimension) return false;
    ID3D11Device* owner = nullptr;
    view->GetDevice(&owner);
    const bool matches = owner && (owner == device || owner == viewDevice);
    Release(owner);
    return matches;
}

HRESULT Buffer(ID3D11Device* device, UINT size, UINT binding, bool dynamic,
               const void* initial, ID3D11Buffer** result) {
    D3D11_BUFFER_DESC description = {};
    description.ByteWidth = size;
    description.BindFlags = binding;
    description.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    description.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = initial;
    return device->CreateBuffer(&description, initial ? &data : nullptr, result);
}
} // namespace

LegacyWaterD3D11::~LegacyWaterD3D11() { Reset(); }

void LegacyWaterD3D11::Reset() {
    for (auto& object : vertex_) Release(object);
    Release(base_);
    for (auto& object : overlay_) Release(object);
    for (auto& object : layouts_) Release(object);
    Release(vertices_); Release(indices_);
    Release(vertexConstants_); Release(pixelConstants_);
    Release(bumpView_); Release(bump_); Release(cubeView_);
    Release(wrapSampler_); Release(cubeSampler_); Release(alphaBlend_);
    for (auto& object : depth_) Release(object);
    for (auto& object : inheritedDepth_) Release(object);
    Release(stencilSource_);
    Release(rasterizer_); Release(viewDevice_); Release(device_);
    vertexCount_ = indexCount_ = width_ = height_ = 0;
    ready_ = false;
}

HRESULT LegacyWaterD3D11::Initialize(ID3D11Device* device,
    const LegacyWaterPrograms& programs, LegacyWaterMesh mesh, UINT width, UINT height,
    const uint32_t* cube, size_t cubeCount) {
    if (device_) return E_UNEXPECTED;
    if (!device || !mesh.vertices || !mesh.indices || !mesh.vertexCount ||
        !mesh.indexCount || mesh.indexCount % 3 || !PowerOfTwo(width) ||
        !PowerOfTwo(height) || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        !cube || cubeCount < kLegacyDirectionCubePixels ||
        mesh.vertexCount > UINT_MAX / sizeof(DecodedWaterVertex) ||
        mesh.indexCount > UINT_MAX / sizeof(uint32_t) ||
        !ValidCode(programs.baseVertex) || !ValidCode(programs.overlayVertex) ||
        !ValidCode(programs.basePixel)) return E_INVALIDARG;
    for (const auto& program : programs.overlayPixel)
        if (!ValidCode(program)) return E_INVALIDARG;
    for (UINT i = 0; i < mesh.indexCount; ++i)
        if (mesh.indices[i] >= mesh.vertexCount) return E_INVALIDARG;
    const HRESULT result = CreateResources(device, programs, mesh, width, height, cube);
    if (FAILED(result)) { Reset(); return result; }
    device_ = device; device_->AddRef();
    bumpView_->GetDevice(&viewDevice_);
    vertexCount_ = mesh.vertexCount; indexCount_ = mesh.indexCount;
    width_ = width; height_ = height;
    return S_OK;
}

HRESULT LegacyWaterD3D11::CreateResources(ID3D11Device* device,
    const LegacyWaterPrograms& programs, LegacyWaterMesh mesh,
    UINT width, UINT height, const uint32_t* cube) {
    HRESULT result;
#define WATER_TRY(call) do { result = (call); if (FAILED(result)) return result; } while (false)
    const ShaderBytes vertexCode[2] = {programs.baseVertex, programs.overlayVertex};
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",1,DXGI_FORMAT_R32G32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",2,DXGI_FORMAT_R32G32_FLOAT,0,40,D3D11_INPUT_PER_VERTEX_DATA,0}
    };
    for (unsigned i = 0; i < 2; ++i) {
        WATER_TRY(device->CreateVertexShader(vertexCode[i].data,vertexCode[i].size,nullptr,&vertex_[i]));
        WATER_TRY(device->CreateInputLayout(elements,5,vertexCode[i].data,vertexCode[i].size,&layouts_[i]));
    }
    WATER_TRY(device->CreatePixelShader(programs.basePixel.data,programs.basePixel.size,nullptr,&base_));
    for (unsigned i = 0; i < 8; ++i)
        WATER_TRY(device->CreatePixelShader(programs.overlayPixel[i].data,programs.overlayPixel[i].size,nullptr,&overlay_[i]));
    WATER_TRY(Buffer(device,mesh.vertexCount*sizeof(DecodedWaterVertex),D3D11_BIND_VERTEX_BUFFER,true,mesh.vertices,&vertices_));
    WATER_TRY(Buffer(device,mesh.indexCount*sizeof(uint32_t),D3D11_BIND_INDEX_BUFFER,false,mesh.indices,&indices_));
    WATER_TRY(Buffer(device,sizeof(LegacyVertexConstants),D3D11_BIND_CONSTANT_BUFFER,false,nullptr,&vertexConstants_));
    WATER_TRY(Buffer(device,sizeof(LegacyPixelConstants),D3D11_BIND_CONSTANT_BUFFER,false,nullptr,&pixelConstants_));
    D3D11_TEXTURE2D_DESC texture = {};
    texture.Width=width; texture.Height=height; texture.MipLevels=1; texture.ArraySize=1;
    texture.Format=DXGI_FORMAT_B8G8R8A8_UNORM; texture.SampleDesc.Count=1;
    texture.Usage=D3D11_USAGE_DYNAMIC; texture.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    texture.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    WATER_TRY(device->CreateTexture2D(&texture,nullptr,&bump_));
    WATER_TRY(device->CreateShaderResourceView(bump_,nullptr,&bumpView_));
    texture.Width=texture.Height=kLegacyDirectionCubeSize;
    texture.ArraySize=6; texture.Usage=D3D11_USAGE_IMMUTABLE; texture.CPUAccessFlags=0;
    texture.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;
    D3D11_SUBRESOURCE_DATA faces[6] = {};
    for (unsigned face=0;face<6;++face) {
        faces[face].pSysMem=cube+face*32*32;
        faces[face].SysMemPitch=32*sizeof(uint32_t);
    }
    ID3D11Texture2D* cubeTexture=nullptr;
    WATER_TRY(device->CreateTexture2D(&texture,faces,&cubeTexture));
    result=device->CreateShaderResourceView(cubeTexture,nullptr,&cubeView_);
    cubeTexture->Release();
    if (FAILED(result)) return result;
    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.MaxAnisotropy=1; sampler.ComparisonFunc=D3D11_COMPARISON_NEVER;
    sampler.MaxLOD=D3D11_FLOAT32_MAX;
    WATER_TRY(device->CreateSamplerState(&sampler,&wrapSampler_));
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    WATER_TRY(device->CreateSamplerState(&sampler,&cubeSampler_));
    D3D11_BLEND_DESC blend = {};
    auto& target=blend.RenderTarget[0];
    target.BlendEnable=TRUE;
    target.SrcBlend=D3D11_BLEND_SRC_ALPHA; target.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp=D3D11_BLEND_OP_ADD;
    // Original source-alpha factors also apply to alpha; do not silently use
    // premultiplied/coverage alpha arithmetic for this shared blend state.
    target.SrcBlendAlpha=D3D11_BLEND_SRC_ALPHA; target.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha=D3D11_BLEND_OP_ADD; target.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    WATER_TRY(device->CreateBlendState(&blend,&alphaBlend_));
    for (unsigned write=0;write<2;++write) {
        D3D11_DEPTH_STENCIL_DESC depth = {};
        depth.DepthEnable=TRUE; depth.DepthWriteMask=write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
        WATER_TRY(device->CreateDepthStencilState(&depth,&depth_[write]));
    }
    D3D11_RASTERIZER_DESC rasterizer = {};
    rasterizer.FillMode=D3D11_FILL_SOLID; rasterizer.CullMode=D3D11_CULL_NONE;
    rasterizer.DepthClipEnable=TRUE;
    WATER_TRY(device->CreateRasterizerState(&rasterizer,&rasterizer_));
#undef WATER_TRY
    return S_OK;
}

bool LegacyWaterD3D11::IsOurImmediateContext(ID3D11DeviceContext* context) const {
    if (!device_ || !context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    ID3D11Device* device=nullptr; context->GetDevice(&device);
    const bool matches=device && (device==device_ || device==viewDevice_); Release(device);
    return matches;
}

HRESULT LegacyWaterD3D11::PrepareInheritedStencil(ID3D11DepthStencilState* source) {
    if (source == stencilSource_) return S_OK;
    D3D11_DEPTH_STENCIL_DESC incoming = {};
    if (source) source->GetDesc(&incoming);
    // A stencil-disabled draw follows the original unmasked backend path.
    // Release the previous source as well, so switching never grows the cache.
    for (auto& object : inheritedDepth_) Release(object);
    Release(stencilSource_);
    if (!incoming.StencilEnable) return S_OK;
    for (unsigned i=0;i<4;++i) {
        D3D11_DEPTH_STENCIL_DESC derived = incoming;
        derived.DepthEnable=TRUE;
        derived.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
        derived.DepthWriteMask=(i&1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        // Native MaskWater tests EQUAL and clears stencil on every outcome.
        // Keep that test for both passes; defer consumption until the last one.
        if (i&2) derived.StencilWriteMask=0;
        const HRESULT result=device_->CreateDepthStencilState(&derived,&inheritedDepth_[i]);
        if (FAILED(result)) {
            for (auto& object : inheritedDepth_) Release(object);
            return result;
        }
    }
    stencilSource_=source;
    stencilSource_->AddRef();
    return S_OK;
}

HRESULT LegacyWaterD3D11::Upload(ID3D11DeviceContext* context,
    const DecodedWaterVertex* vertices, UINT count, const uint32_t* pixels, size_t pixelCount) {
    ready_=false;
    if (!IsOurImmediateContext(context) || !vertices || count!=vertexCount_ ||
        !pixels || pixelCount!=size_t(width_)*height_) return E_INVALIDARG;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT result=context->Map(bump_,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
    if (FAILED(result)) return result;
    for (UINT row=0;row<height_;++row)
        memcpy(static_cast<unsigned char*>(mapped.pData)+size_t(row)*mapped.RowPitch,
               pixels+size_t(row)*width_,size_t(width_)*sizeof(uint32_t));
    context->Unmap(bump_,0);
    result=context->Map(vertices_,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
    if (FAILED(result)) return result;
    memcpy(mapped.pData,vertices,size_t(count)*sizeof(DecodedWaterVertex));
    context->Unmap(vertices_,0);
    ready_=true;
    return S_OK;
}

HRESULT LegacyWaterD3D11::Draw(ID3D11DeviceContext* context, const LegacyWaterDraw& p) {
    if (!ready_ || !IsOurImmediateContext(context) ||
        !CompatibleView(p.textures.environment,device_,viewDevice_,D3D11_SRV_DIMENSION_TEXTURECUBE)) return E_INVALIDARG;
    const int variant=static_cast<int>(p.variant);
    if (p.overlay && (variant<0 || variant>=8)) return E_NOTIMPL;
    if (p.overlay && (!CompatibleView(p.textures.material,device_,viewDevice_,D3D11_SRV_DIMENSION_TEXTURE2D) ||
        (variant!=0 && !CompatibleView(p.textures.detail,device_,viewDevice_,D3D11_SRV_DIMENSION_TEXTURE2D)))) return E_INVALIDARG;
    saved_.Capture(context);
    const bool inheritStencil=!p.baseDepth && !p.overlayDepth;
    if (inheritStencil) {
        const HRESULT result=PrepareInheritedStencil(saved_.depth);
        if (FAILED(result)) { saved_.Restore(context); return result; }
    }
    ID3D11DepthStencilState* baseDepth=depth_[p.writeDepth ? 1 : 0];
    ID3D11DepthStencilState* overlayDepth=depth_[0];
    if (inheritStencil && stencilSource_) {
        baseDepth=inheritedDepth_[(p.writeDepth ? 1 : 0)+(p.overlay ? 2 : 0)];
        overlayDepth=inheritedDepth_[0];
    }
    context->UpdateSubresource(vertexConstants_,0,nullptr,&p.vertex,0,0);
    context->UpdateSubresource(pixelConstants_,0,nullptr,&p.pixel,0,0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride=sizeof(DecodedWaterVertex),offset=0;
    context->IASetVertexBuffers(0,1,&vertices_,&stride,&offset);
    context->IASetIndexBuffer(indices_,DXGI_FORMAT_R32_UINT,0);
    context->VSSetConstantBuffers(1,1,&vertexConstants_);
    context->PSSetConstantBuffers(0,1,&pixelConstants_);
    context->GSSetShader(nullptr,nullptr,0); context->HSSetShader(nullptr,nullptr,0);
    context->DSSetShader(nullptr,nullptr,0);
    context->RSSetState(p.rasterizer ? p.rasterizer : rasterizer_);
    ID3D11ShaderResourceView* resources[4]={bumpView_,nullptr,cubeView_,p.textures.environment};
    ID3D11SamplerState* samplers[4]={wrapSampler_,wrapSampler_,cubeSampler_,cubeSampler_};
    context->PSSetShaderResources(0,4,resources); context->PSSetSamplers(0,4,samplers);
    context->IASetInputLayout(layouts_[0]); context->VSSetShader(vertex_[0],nullptr,0);
    context->PSSetShader(base_,nullptr,0);
    context->OMSetBlendState(p.baseBlend ? p.baseBlend : alphaBlend_,saved_.blendFactor,saved_.sampleMask);
    context->OMSetDepthStencilState(p.baseDepth ? p.baseDepth : baseDepth,saved_.stencilReference);
    context->DrawIndexed(indexCount_,0,0);
    if (p.overlay) {
        resources[0]=p.textures.material; resources[1]=p.textures.detail;
        resources[2]=bumpView_; resources[3]=nullptr;
        samplers[2]=wrapSampler_;
        context->PSSetShaderResources(0,4,resources); context->PSSetSamplers(0,4,samplers);
        context->IASetInputLayout(layouts_[1]); context->VSSetShader(vertex_[1],nullptr,0);
        context->PSSetShader(overlay_[variant],nullptr,0);
        context->OMSetBlendState(p.overlayBlend ? p.overlayBlend : alphaBlend_,saved_.blendFactor,saved_.sampleMask);
        context->OMSetDepthStencilState(p.overlayDepth ? p.overlayDepth : overlayDepth,saved_.stencilReference);
        context->DrawIndexed(indexCount_,0,0);
    }
    saved_.Restore(context);
    return S_OK;
}

void LegacyWaterD3D11::SavedState::Capture(ID3D11DeviceContext* c) {
    c->IAGetInputLayout(&layout); c->IAGetVertexBuffers(0,1,&vertexBuffer,&stride,&offset);
    c->IAGetIndexBuffer(&indexBuffer,&indexFormat,&indexOffset); c->IAGetPrimitiveTopology(&topology);
    for (auto& count:classCounts) count=256;
    c->VSGetShader(&vertex,classes[0],&classCounts[0]);
    c->PSGetShader(&pixel,classes[1],&classCounts[1]);
    c->GSGetShader(&geometry,classes[2],&classCounts[2]);
    c->HSGetShader(&hull,classes[3],&classCounts[3]);
    c->DSGetShader(&domain,classes[4],&classCounts[4]);
    c->VSGetConstantBuffers(1,1,&vertexConstants); c->PSGetConstantBuffers(0,1,&pixelConstants);
    c->PSGetShaderResources(0,4,textures); c->PSGetSamplers(0,4,samplers);
    c->OMGetBlendState(&blend,blendFactor,&sampleMask); c->OMGetDepthStencilState(&depth,&stencilReference);
    c->RSGetState(&rasterizer);
}

void LegacyWaterD3D11::SavedState::Restore(ID3D11DeviceContext* c) {
    c->IASetInputLayout(layout); c->IASetVertexBuffers(0,1,&vertexBuffer,&stride,&offset);
    c->IASetIndexBuffer(indexBuffer,indexFormat,indexOffset); c->IASetPrimitiveTopology(topology);
    c->VSSetShader(vertex,classes[0],classCounts[0]); c->PSSetShader(pixel,classes[1],classCounts[1]);
    c->GSSetShader(geometry,classes[2],classCounts[2]); c->HSSetShader(hull,classes[3],classCounts[3]);
    c->DSSetShader(domain,classes[4],classCounts[4]);
    c->VSSetConstantBuffers(1,1,&vertexConstants); c->PSSetConstantBuffers(0,1,&pixelConstants);
    c->PSSetShaderResources(0,4,textures); c->PSSetSamplers(0,4,samplers);
    c->OMSetBlendState(blend,blendFactor,sampleMask); c->OMSetDepthStencilState(depth,stencilReference);
    c->RSSetState(rasterizer);
    Release(layout); Release(vertexBuffer); Release(indexBuffer);
    Release(vertex); Release(pixel); Release(geometry); Release(hull); Release(domain);
    for (unsigned stage=0;stage<5;++stage)
        for (UINT i=0;i<classCounts[stage];++i) Release(classes[stage][i]);
    Release(vertexConstants); Release(pixelConstants);
    for (auto& object:textures) Release(object);
    for (auto& object:samplers) Release(object);
    Release(blend); Release(depth); Release(rasterizer);
}

}} // namespace vince::water
