/*
 * This file is part of NV3D-Glass.
 *
 * NV3D-Glass is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * NV3D-Glass is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with NV3D-Glass. If not, see <http://www.gnu.org/licenses/>.
 */

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

    // Tear down all D3D11 state (device, context, swap chain, scaler shaders,
    // ImGui DX11 backend) and rebuild it from scratch. Called when we detect
    // DXGI_ERROR_DEVICE_REMOVED — the existing device is dead, the swap
    // chain backbuffer renders nothing (the control panel goes white), and
    // ImGui's cached resources point at a removed device. After RecreateDevice
    // the panel paints again and the next Start() builds a fresh capture
    // pipeline on the new device. Returns false on rebuild failure (rare —
    // would indicate the GPU is permanently gone).
    bool RecreateDevice();

    void HandleResize(UINT w, UINT h);
    void BeginImGuiFrame();              // ImGui_ImplDX11_NewFrame + ImGui_ImplWin32_NewFrame + ImGui::NewFrame
    void EndImGuiFrame();                // ImGui::Render + ImGui_ImplDX11_RenderDrawData + Present(1,0)

    ID3D11Device*        Device()       { return d3d_.Get(); }
    ID3D11DeviceContext* Context()      { return ctx_.Get(); }

    // Capture-pipeline staging textures (MISC_SHARED + BGRA8), a fixed ring
    // of kStagingRing. Allocated once per session via CreateFixedStaging —
    // dims are deliberately not recreated on source-dim changes mid-stream.
    //
    // Why a ring: NV3DLib's D3D9 side reads the staging through a legacy
    // KMT shared handle, which has ZERO implicit cross-API synchronization.
    // With a single staging, tick N+1's CopyResource/scaler-draw overwrites
    // the very allocation the D3D9 StretchRect for tick N may still be
    // reading — a permanent write/read race on the exact driver path that
    // is fragile. Each new frame is written into the NEXT ring slot instead;
    // Staging() returns the last-written slot (what should be presented).
    // Ring depth 3 (not 2): if NV3DLib's worker drops a frame, a 2-deep ring
    // wraps back onto a slot whose present may still be in flight after
    // only one tick (~8ms); 3-deep gives the GPU ≥3 ticks (~24ms) to retire
    // a read before the slot is rewritten.
    bool             CreateFixedStaging(UINT w, UINT h);
    ID3D11Texture2D* Staging() const { return staging_[staging_idx_].Get(); }
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
    // Composite a stereo software cursor (a small reticle) into the CURRENT
    // staging slot, on top of whatever capture content was just written. u/v
    // are the mouse position normalized within the captured content [0,1]; the
    // reticle is drawn into both SbS eye halves at that position. parallax_frac
    // splits the two eyes horizontally (fraction of per-eye width) so the
    // reticle can sit at, in front of, or behind the screen plane. Must be
    // called AFTER a CopyCapture*/FillTestPattern for the same tick (those
    // overwrite the whole slot) and BEFORE the frame is submitted. No-op if the
    // cursor pipeline can't be built.
    bool             DrawStereoCursor(float u, float v, float parallax_frac);

    // Fill staging with the DX11Sample-style stereo test pattern (red+quad in
    // the left eye half, green+quad in the right; a parallax-shifted white
    // square between them so stereo fusion is obvious under shutter glasses).
    // `frame` is the animation counter — pass 0 for a static frame, or
    // increment per tick to animate.
    void             FillTestPattern(uint32_t frame);
    void             ReleaseStaging();

    // Drop the cached scaler SRV (and its reference to the last scaler
    // source). MUST be called whenever the capture source's shared texture
    // is being released deliberately — e.g. CaptureKatanga::ReleaseSharedHold
    // before an FSE minimize, or ResetForReconnect on producer loss. The SRV
    // holds the cross-process allocation open on our device; keeping it
    // across the SW_MINIMIZE stereo-teardown defeats the entire point of
    // releasing the capture-side hold (observed TDR surface).
    void             InvalidateScalerCache();

private:
    bool CreateD3D();
    bool CreateSwapChain(HWND hwnd);
    bool CreateBackBufferRTV();

    // Rotate to the next ring slot; every frame-producing entry point
    // (CopyCaptureToStaging / CopyCaptureRegionToStaging / FillTestPattern)
    // calls this exactly once before writing.
    void AdvanceStaging() { staging_idx_ = (staging_idx_ + 1) % kStagingRing; }

    static constexpr UINT kStagingRing = 3;

    HWND                                            hwnd_       = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device>            d3d_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>     ctx_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1>         swap_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>  rtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>         staging_[kStagingRing];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>  staging_rtv_[kStagingRing];
    UINT                                            staging_idx_ = 0;
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
    // Cached SRV onto the last scaler source — Katanga's cross-process
    // shared texture stays at the same pointer for many frames, so
    // re-creating an SRV every call burns 100+ create/release cycles per
    // second against shared GPU memory. Cache by source pointer identity;
    // recreate only when the source changes.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> scale_cached_srv_;
    ID3D11Texture2D*                                scale_cached_src_ = nullptr;
    DXGI_FORMAT                                     scale_cached_fmt_ = DXGI_FORMAT_UNKNOWN;

    // Stereo-cursor pipeline — compiled on first DrawStereoCursor call. A
    // vertexless quad (SV_VertexID) per eye samples a small baked arrow bitmap
    // (white fill + black outline, straight alpha) so the 3D cursor reads as a
    // normal mouse pointer. The dynamic constant buffer holds the quad's clip-
    // space rectangle per draw.
    bool InitCursor();
    Microsoft::WRL::ComPtr<ID3D11VertexShader>       cursor_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>        cursor_ps_;
    Microsoft::WRL::ComPtr<ID3D11BlendState>         cursor_blend_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>    cursor_rs_;
    Microsoft::WRL::ComPtr<ID3D11Buffer>             cursor_cb_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          cursor_tex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursor_srv_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>       cursor_samp_;
    bool                                             cursor_ready_ = false;
};

}
