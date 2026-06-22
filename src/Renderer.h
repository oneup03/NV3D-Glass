#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>

namespace nv3dg {

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Init(HWND control_panel_hwnd);
    void Shutdown();

    void HandleResize(UINT w, UINT h);
    void BeginImGuiFrame();              // ImGui_ImplDX11_NewFrame + ImGui_ImplWin32_NewFrame + ImGui::NewFrame
    void EndImGuiFrame();                // ImGui::Render + ImGui_ImplDX11_RenderDrawData + Present(1,0)

    ID3D11Device*        Device()       { return d3d_.Get(); }
    ID3D11DeviceContext* Context()      { return ctx_.Get(); }

    // Capture-pipeline staging texture (MISC_SHARED + BGRA8). Allocated once
    // per session via CreateFixedStaging — we deliberately don't recreate it
    // on source-dim changes mid-stream, because changing the texture the
    // NV3DLib backend has cached forces a re-import of the shared D3D9 view
    // and causes visible artifacts on some configs. Caller is expected to
    // pick a staging size at Start time and live with it.
    bool             CreateFixedStaging(UINT w, UINT h);
    ID3D11Texture2D* Staging() const { return staging_.Get(); }
    UINT             StagingWidth()  const { return staging_w_; }
    UINT             StagingHeight() const { return staging_h_; }
    // Copy src into staging via CopyResource (matching dims) or a centered
    // sub-region copy (smaller src). Sources larger than staging are clipped.
    // Returns false on a dim mismatch we can't reconcile.
    bool             CopyCaptureToStaging(ID3D11Texture2D* src);

    // As above, but copy only a sub-rect of src (e.g. the client area of a
    // captured window, stripped of title bar / borders). Src box dims must
    // equal staging dims for the fast CopySubresourceRegion path; otherwise
    // falls through to the scaler shader. Pass an empty RECT (or a rect
    // covering the full src) to behave like CopyCaptureToStaging.
    bool             CopyCaptureRegionToStaging(ID3D11Texture2D* src, const RECT& src_box);
    // Fill staging with the DX11Sample-style stereo test pattern (red+quad in
    // the left eye half, green+quad in the right; a parallax-shifted white
    // square between them so stereo fusion is obvious under shutter glasses).
    // `frame` is the animation counter — pass 0 for a static frame, or
    // increment per tick to animate.
    void             FillTestPattern(uint32_t frame);
    void             ReleaseStaging();

private:
    bool CreateD3D();
    bool CreateSwapChain(HWND hwnd);
    bool CreateBackBufferRTV();

    HWND                                            hwnd_       = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device>            d3d_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>     ctx_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1>         swap_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>  rtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>         staging_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>  staging_rtv_;
    UINT                                            staging_w_  = 0;
    UINT                                            staging_h_  = 0;
    UINT                                            swap_w_     = 0;
    UINT                                            swap_h_     = 0;
    bool                                            imgui_init_ = false;

    // GPU scaler pipeline — compiled on first call, shared across calls.
    bool InitScaler();
    bool ScaleCaptureToStaging(ID3D11Texture2D* src);
    Microsoft::WRL::ComPtr<ID3D11VertexShader>      scale_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       scale_ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      scale_sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   scale_rs_;
    Microsoft::WRL::ComPtr<ID3D11BlendState>        scale_blend_;
    bool                                            scaler_ready_ = false;
};

}
