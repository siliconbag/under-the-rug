#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include "Cloth.h"

using Microsoft::WRL::ComPtr;

struct Texture {
    ComPtr<ID3D11ShaderResourceView> srv;
    int width = 0, height = 0;
};

// Hardware device with BGRA support (needed by DirectComposition), WARP if there is no usable GPU.
HRESULT createDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& ctx);
// 4x MSAA when the GPU supports it for the window format.
int pickSampleCount(ID3D11Device* device);

class Renderer {
public:
    // Throws std::runtime_error with a readable message.
    Renderer(ID3D11Device* device, ID3D11DeviceContext* ctx, int sampleCount, const std::wstring& texturePath);

    static Texture loadTexture(ID3D11Device* device, ID3D11DeviceContext* ctx, const std::wstring& path);
    void setTexture(const std::wstring& path);
    const Texture& rugTexture() const { return rug; }
    void setMesh(const Cloth& cloth);

    // Uploads vertices, fills the uniforms and draws all passes at the given pixel size,
    // then resolves into target (B8G8R8A8, single sample). Returns false when there is nothing to draw.
    bool render(const Cloth& cloth, const Camera& camera, int pixelW, int pixelH, ID3D11Texture2D* target,
                ID3D11ShaderResourceView* background);

    // Drops the render targets while the rug sleeps, they're recreated on the next frame.
    void releaseTargets();

    // Frame into BGRA pixels (demo mode), over the given background.
    bool snapshot(const Cloth& cloth, const Camera& camera, int scale, ID3D11ShaderResourceView* background,
                  std::vector<uint8_t>& out, int& w, int& h);

private:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    int sampleCount;
    ComPtr<ID3D11VertexShader> rugVS, floorVS, shadowVS, bgVS;
    ComPtr<ID3D11PixelShader> rugPS, floorPS, shadowPS, bgPS;
    ComPtr<ID3D11BlendState> premulBlend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depthLess, depthAlways;
    ComPtr<ID3D11SamplerState> texSampler, nearestSampler;
    ComPtr<ID3D11Buffer> uniforms;
    Texture rug;

    ComPtr<ID3D11Texture2D> shadowTex;
    ComPtr<ID3D11RenderTargetView> shadowRTV;
    ComPtr<ID3D11ShaderResourceView> shadowSRV;
    ComPtr<ID3D11DepthStencilView> shadowDSV;
    int shadowW = 0, shadowH = 0;

    ComPtr<ID3D11Texture2D> colorTex;
    ComPtr<ID3D11RenderTargetView> colorRTV;
    ComPtr<ID3D11DepthStencilView> depthDSV;
    int colorW = 0, colorH = 0;

    ComPtr<ID3D11Buffer> vbuf, ibuf, uvbuf;
    ComPtr<ID3D11ShaderResourceView> vsrv, uvsrv;
    int vertexCount = 0, indexCount = 0;

    void ensureShadowTargets(float2 size);
    void ensureColorTargets(int w, int h);
};
