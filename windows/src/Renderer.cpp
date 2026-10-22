#include "Renderer.h"
#include <d3dcompiler.h>
#include <wincodec.h>
#include <cfloat>
#include <stdexcept>
#include "Common.h"
#include "Shaders.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

struct GPUVertex {
    float pos[4];
    float nrm[4];
};

// Same layout as the HLSL cbuffer (80 bytes).
struct Uniforms {
    float lightDir[4] = {0, 0, 1, 0};
    float floorRect[4] = {0, 0, 0, 0};
    float viewSize[2] = {0, 0};
    float lightK[2] = {0, 0};
    float rugSize[2] = {0, 0};
    float shadowMapSize[2] = {0, 0};
    float camD = 1000;
    float floorZ = -4;
    float shadowStrength = 0.5f;
    float time = 0;
};
static_assert(sizeof(Uniforms) == 80, "cbuffer layout");

// Lower light for shading the pile, higher one for shadows, so shadows stay short and bumps read well.
static const float3 kShadeLight = normalize(float3(-0.5f, 0.58f, 0.64f));
static const float3 kShadowLight = normalize(float3(-0.3f, 0.38f, 0.87f));

static void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[160];
        sprintf_s(buf, "%s failed (0x%08lx)", what, (unsigned long)hr);
        throw std::runtime_error(buf);
    }
}

HRESULT createDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& ctx) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device,
                               nullptr, &ctx);
        if (hr == E_INVALIDARG)  // runtimes without 11_1
            hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels + 1, 1, D3D11_SDK_VERSION, &device, nullptr,
                                   &ctx);
        if (SUCCEEDED(hr)) {
            if (type == D3D_DRIVER_TYPE_WARP) rugLog(L"no usable GPU, rendering with WARP");
            return hr;
        }
    }
    return hr;
}

int pickSampleCount(ID3D11Device* device) {
    UINT q = 0;
    if (SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, 4, &q)) && q > 0) return 4;
    return 1;
}

static ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(kShaderSource, strlen(kShaderSource), "Rug.hlsl", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        std::string msg = std::string("shader ") + entry + ": ";
        if (errors) msg += (const char*)errors->GetBufferPointer();
        throw std::runtime_error(msg);
    }
    return code;
}

Renderer::Renderer(ID3D11Device* device_, ID3D11DeviceContext* ctx_, int sampleCount_, const std::wstring& texturePath)
    : device(device_), ctx(ctx_), sampleCount(sampleCount_) {
    auto vs = [&](const char* name, ComPtr<ID3D11VertexShader>& out) {
        ComPtr<ID3DBlob> b = compile(name, "vs_5_0");
        check(device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &out), name);
    };
    auto ps = [&](const char* name, ComPtr<ID3D11PixelShader>& out) {
        ComPtr<ID3DBlob> b = compile(name, "ps_5_0");
        check(device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &out), name);
    };
    vs("rug_vs", rugVS);
    ps("rug_fs", rugPS);
    vs("floor_vs", floorVS);
    ps("floor_fs", floorPS);
    vs("shadow_vs", shadowVS);
    ps("shadow_fs", shadowPS);
    vs("bg_vs", bgVS);
    ps("bg_fs", bgPS);

    D3D11_BLEND_DESC bd = {};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    check(device->CreateBlendState(&bd, &premulBlend), "blend state");

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = TRUE;
    rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = sampleCount > 1;
    check(device->CreateRasterizerState(&rd, &raster), "rasterizer state");

    D3D11_DEPTH_STENCIL_DESC dl = {};
    dl.DepthEnable = TRUE;
    dl.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dl.DepthFunc = D3D11_COMPARISON_LESS;
    check(device->CreateDepthStencilState(&dl, &depthLess), "depth state");
    D3D11_DEPTH_STENCIL_DESC da = {};
    da.DepthEnable = TRUE;
    da.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    da.DepthFunc = D3D11_COMPARISON_ALWAYS;
    check(device->CreateDepthStencilState(&da, &depthAlways), "depth state");

    D3D11_SAMPLER_DESC ts = {};
    ts.Filter = D3D11_FILTER_ANISOTROPIC;
    ts.MaxAnisotropy = 16;
    ts.AddressU = ts.AddressV = ts.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ts.MaxLOD = D3D11_FLOAT32_MAX;
    check(device->CreateSamplerState(&ts, &texSampler), "sampler");
    D3D11_SAMPLER_DESC ns = {};
    ns.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ns.AddressU = ns.AddressV = ns.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ns.MaxLOD = D3D11_FLOAT32_MAX;
    check(device->CreateSamplerState(&ns, &nearestSampler), "sampler");

    D3D11_BUFFER_DESC ub = {};
    ub.ByteWidth = sizeof(Uniforms);
    ub.Usage = D3D11_USAGE_DYNAMIC;
    ub.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ub.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check(device->CreateBuffer(&ub, nullptr, &uniforms), "uniform buffer");

    rug = loadTexture(device.Get(), ctx.Get(), texturePath);
}

// Decodes any image WIC can read into an RGBA texture with a full mip chain.
Texture Renderer::loadTexture(ID3D11Device* device, ID3D11DeviceContext* ctx, const std::wstring& path) {
    ComPtr<IWICImagingFactory> f;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)), "WIC");
    check(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec),
          "open image");
    check(dec->GetFrame(0, &frame), "decode image");
    ComPtr<IWICBitmapSource> src = frame;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    // large pictures are scaled down, even a big rug on a 4K screen is narrower than 4096 pixels
    const UINT maxSide = 4096;
    if (w > maxSide || h > maxSide) {
        double k = double(maxSide) / std::max(w, h);
        ComPtr<IWICBitmapScaler> scaler;
        check(f->CreateBitmapScaler(&scaler), "scaler");
        UINT sw = std::max(1u, UINT(w * k)), sh = std::max(1u, UINT(h * k));
        check(scaler->Initialize(frame.Get(), sw, sh, WICBitmapInterpolationModeFant), "scale image");
        src = scaler;
        w = sw;
        h = sh;
    }
    check(f->CreateFormatConverter(&conv), "converter");
    check(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                           WICBitmapPaletteTypeCustom),
          "convert image");
    std::vector<uint8_t> px(size_t(w) * h * 4);
    check(conv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()), "read pixels");

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ComPtr<ID3D11Texture2D> tex;
    check(device->CreateTexture2D(&td, nullptr, &tex), "texture");
    ctx->UpdateSubresource(tex.Get(), 0, nullptr, px.data(), w * 4, 0);
    Texture t;
    check(device->CreateShaderResourceView(tex.Get(), nullptr, &t.srv), "texture view");
    ctx->GenerateMips(t.srv.Get());
    t.width = (int)w;
    t.height = (int)h;
    return t;
}

void Renderer::setTexture(const std::wstring& path) { rug = loadTexture(device.Get(), ctx.Get(), path); }

void Renderer::setMesh(const Cloth& cloth) {
    indexCount = (int)cloth.indices.size();
    vertexCount = cloth.count;

    D3D11_BUFFER_DESC ib = {};
    ib.ByteWidth = UINT(cloth.indices.size() * sizeof(uint32_t));
    ib.Usage = D3D11_USAGE_IMMUTABLE;
    ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA id = {cloth.indices.data()};
    check(device->CreateBuffer(&ib, &id, &ibuf), "index buffer");

    D3D11_BUFFER_DESC ub = {};
    ub.ByteWidth = UINT(cloth.uvs.size() * sizeof(float2));
    ub.Usage = D3D11_USAGE_IMMUTABLE;
    ub.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ub.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    ub.StructureByteStride = sizeof(float2);
    D3D11_SUBRESOURCE_DATA ud = {cloth.uvs.data()};
    check(device->CreateBuffer(&ub, &ud, &uvbuf), "uv buffer");
    check(device->CreateShaderResourceView(uvbuf.Get(), nullptr, &uvsrv), "uv view");

    D3D11_BUFFER_DESC vb = {};
    vb.ByteWidth = UINT(sizeof(GPUVertex) * cloth.count);
    vb.Usage = D3D11_USAGE_DYNAMIC;
    vb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    vb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    vb.StructureByteStride = sizeof(GPUVertex);
    check(device->CreateBuffer(&vb, nullptr, &vbuf), "vertex buffer");
    check(device->CreateShaderResourceView(vbuf.Get(), nullptr, &vsrv), "vertex view");
}

void Renderer::ensureShadowTargets(float2 size) {
    int w = std::max(1, int(size.x)), h = std::max(1, int(size.y));
    if (shadowTex && w == shadowW && h == shadowH) return;
    D3D11_TEXTURE2D_DESC cd = {};
    cd.Width = w;
    cd.Height = h;
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    cd.Format = DXGI_FORMAT_R32_FLOAT;
    cd.SampleDesc.Count = 1;
    cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    check(device->CreateTexture2D(&cd, nullptr, &shadowTex), "shadow map");
    check(device->CreateRenderTargetView(shadowTex.Get(), nullptr, &shadowRTV), "shadow map");
    check(device->CreateShaderResourceView(shadowTex.Get(), nullptr, &shadowSRV), "shadow map");
    D3D11_TEXTURE2D_DESC dd = cd;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    check(device->CreateTexture2D(&dd, nullptr, &depth), "shadow depth");
    check(device->CreateDepthStencilView(depth.Get(), nullptr, &shadowDSV), "shadow depth");
    shadowW = w;
    shadowH = h;
}

void Renderer::ensureColorTargets(int w, int h) {
    if (colorTex && w == colorW && h == colorH) return;
    D3D11_TEXTURE2D_DESC cd = {};
    cd.Width = w;
    cd.Height = h;
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    cd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    cd.SampleDesc.Count = sampleCount;
    cd.BindFlags = D3D11_BIND_RENDER_TARGET;
    check(device->CreateTexture2D(&cd, nullptr, &colorTex), "color target");
    check(device->CreateRenderTargetView(colorTex.Get(), nullptr, &colorRTV), "color target");
    D3D11_TEXTURE2D_DESC dd = cd;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    check(device->CreateTexture2D(&dd, nullptr, &depth), "depth target");
    check(device->CreateDepthStencilView(depth.Get(), nullptr, &depthDSV), "depth target");
    colorW = w;
    colorH = h;
}

void Renderer::releaseTargets() {
    shadowTex.Reset();
    shadowRTV.Reset();
    shadowSRV.Reset();
    shadowDSV.Reset();
    colorTex.Reset();
    colorRTV.Reset();
    depthDSV.Reset();
    shadowW = shadowH = colorW = colorH = 0;
    ctx->ClearState();
    // D3D frees released resources lazily, once the GPU is done with them and the context is flushed again
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> done;
    if (SUCCEEDED(device->CreateQuery(&qd, &done))) {
        ctx->End(done.Get());
        BOOL ready = FALSE;
        for (int i = 0; i < 200 && ctx->GetData(done.Get(), &ready, sizeof(ready), 0) == S_FALSE; i++) Sleep(1);
    }
    ctx->Flush();
}

bool Renderer::render(const Cloth& cloth, const Camera& camera, int pixelW, int pixelH, ID3D11Texture2D* target,
                      ID3D11ShaderResourceView* background) {
    if (!ibuf || !uvbuf || !vbuf || cloth.count != vertexCount) return false;
    ensureShadowTargets(camera.size);
    ensureColorTargets(pixelW, pixelH);

    float2 k = kShadowLight.xy() / kShadowLight.z;
    float2 lo(FLT_MAX), hi(-FLT_MAX);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(vbuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    GPUVertex* ptr = (GPUVertex*)m.pData;
    for (int i = 0; i < cloth.count; i++) {
        float3 p = cloth.pos[i], n = cloth.nrm[i];
        ptr[i] = GPUVertex{{p.x, p.y, p.z, 1}, {n.x, n.y, n.z, 0}};
        float2 f = p.xy() - k * p.z;
        lo = vmin(lo, vmin(f, cloth.proj[i]));
        hi = vmax(hi, vmax(f, cloth.proj[i]));
    }
    ctx->Unmap(vbuf.Get(), 0);
    lo -= float2(48);
    hi += float2(48);

    Uniforms u;
    u.lightDir[0] = kShadeLight.x;
    u.lightDir[1] = kShadeLight.y;
    u.lightDir[2] = kShadeLight.z;
    u.floorRect[0] = lo.x;
    u.floorRect[1] = lo.y;
    u.floorRect[2] = hi.x;
    u.floorRect[3] = hi.y;
    u.viewSize[0] = u.shadowMapSize[0] = camera.size.x;
    u.viewSize[1] = u.shadowMapSize[1] = camera.size.y;
    u.lightK[0] = k.x;
    u.lightK[1] = k.y;
    u.rugSize[0] = cloth.width;
    u.rugSize[1] = cloth.height;
    u.camD = camera.D();
    if (FAILED(ctx->Map(uniforms.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, &u, sizeof(u));
    ctx->Unmap(uniforms.Get(), 0);

    ID3D11Buffer* cb = uniforms.Get();
    ID3D11ShaderResourceView* vsSRVs[2] = {vsrv.Get(), uvsrv.Get()};
    ID3D11SamplerState* samplers[2] = {texSampler.Get(), nearestSampler.Get()};
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->VSSetShaderResources(2, 2, vsSRVs);
    ctx->PSSetSamplers(0, 2, samplers);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetIndexBuffer(ibuf.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx->RSSetState(raster.Get());

    // pass 1: shadow height map
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, none);
    float clearShadow[4] = {-10000, 0, 0, 0};
    ctx->ClearRenderTargetView(shadowRTV.Get(), clearShadow);
    ctx->ClearDepthStencilView(shadowDSV.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    ctx->OMSetRenderTargets(1, shadowRTV.GetAddressOf(), shadowDSV.Get());
    D3D11_VIEWPORT sv = {0, 0, float(shadowW), float(shadowH), 0, 1};
    ctx->RSSetViewports(1, &sv);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(depthLess.Get(), 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(shadowVS.Get(), nullptr, 0);
    ctx->PSSetShader(shadowPS.Get(), nullptr, 0);
    ctx->DrawIndexed(indexCount, 0, 0);

    // pass 2 and 3: floor shadow and the rug
    float clear[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(colorRTV.Get(), clear);
    ctx->ClearDepthStencilView(depthDSV.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    ctx->OMSetRenderTargets(1, colorRTV.GetAddressOf(), depthDSV.Get());
    D3D11_VIEWPORT mv = {0, 0, float(pixelW), float(pixelH), 0, 1};
    ctx->RSSetViewports(1, &mv);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    if (background) {
        ctx->OMSetDepthStencilState(depthAlways.Get(), 0);
        ctx->PSSetShaderResources(0, 1, &background);
        ctx->VSSetShader(bgVS.Get(), nullptr, 0);
        ctx->PSSetShader(bgPS.Get(), nullptr, 0);
        ctx->Draw(4, 0);
    }
    ID3D11ShaderResourceView* psSRVs[2] = {rug.srv.Get(), shadowSRV.Get()};
    ctx->PSSetShaderResources(0, 2, psSRVs);
    ctx->OMSetBlendState(premulBlend.Get(), nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(depthAlways.Get(), 0);
    ctx->VSSetShader(floorVS.Get(), nullptr, 0);
    ctx->PSSetShader(floorPS.Get(), nullptr, 0);
    ctx->Draw(4, 0);

    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(depthLess.Get(), 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(rugVS.Get(), nullptr, 0);
    ctx->PSSetShader(rugPS.Get(), nullptr, 0);
    ctx->DrawIndexed(indexCount, 0, 0);

    ctx->PSSetShaderResources(0, 2, none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (sampleCount > 1) ctx->ResolveSubresource(target, 0, colorTex.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    else ctx->CopyResource(target, colorTex.Get());
    return true;
}

bool Renderer::snapshot(const Cloth& cloth, const Camera& camera, int scale, ID3D11ShaderResourceView* background,
                        std::vector<uint8_t>& out, int& w, int& h) {
    w = int(camera.size.x) * scale;
    h = int(camera.size.y) * scale;
    D3D11_TEXTURE2D_DESC rd = {};
    rd.Width = w;
    rd.Height = h;
    rd.MipLevels = 1;
    rd.ArraySize = 1;
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> resolve, staging;
    if (FAILED(device->CreateTexture2D(&rd, nullptr, &resolve))) return false;
    rd.BindFlags = 0;
    rd.Usage = D3D11_USAGE_STAGING;
    rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&rd, nullptr, &staging))) return false;
    if (!render(cloth, camera, w, h, resolve.Get(), background)) return false;
    ctx->CopyResource(staging.Get(), resolve.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    out.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; y++) memcpy(&out[size_t(y) * w * 4], (uint8_t*)m.pData + size_t(y) * m.RowPitch, size_t(w) * 4);
    ctx->Unmap(staging.Get(), 0);
    return true;
}
