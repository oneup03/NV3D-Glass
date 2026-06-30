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

#include "Renderer.h"

#include "Logging.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <Windows.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace nv3dg {

Renderer::~Renderer() { Shutdown(); }

bool Renderer::CreateD3D() {
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL fl{};
    UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    // D3D11 debug layer: ON by default. Set NV3D_GLASS_D3D11_DEBUG=0 to
    // disable. When enabled, every D3D11 driver validation warning / error
    // is routed to OutputDebugString — view with sysinternals DebugView.
    // Most useful right before a GPU TDR, where the layer usually prints
    // the underlying API misuse that the driver is about to fault on.
    // Comes with a perf cost; if missing SDK Layers package, we fall back
    // to a non-debug create automatically below.
    {
        wchar_t env_value[8]{};
        DWORD env_len = GetEnvironmentVariableW(L"NV3D_GLASS_D3D11_DEBUG", env_value, _countof(env_value));
        const bool explicitly_off = (env_len > 0 && env_len < _countof(env_value) && env_value[0] == L'0');
        if (!explicitly_off) {
            create_flags |= D3D11_CREATE_DEVICE_DEBUG;
            Log(NV3D::LogLevel::Info,
                L"Renderer::CreateD3D  D3D11_CREATE_DEVICE_DEBUG on (set NV3D_GLASS_D3D11_DEBUG=0 to disable)");
        }
    }
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        create_flags,
        fls, _countof(fls), D3D11_SDK_VERSION,
        &d3d_, &fl, &ctx_);
    // If the SDK layers DLL is missing the call fails with E_FAIL — fall
    // back to a non-debug create so we don't accidentally brick the app on
    // a machine without the SDK installed.
    if (FAILED(hr) && (create_flags & D3D11_CREATE_DEVICE_DEBUG)) {
        Log(NV3D::LogLevel::Warning,
            L"Renderer::CreateD3D  D3D11_CREATE_DEVICE_DEBUG failed hr=0x%08X — "
            L"SDK layers missing? Retrying without debug flag", (unsigned)hr);
        create_flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            create_flags,
            fls, _countof(fls), D3D11_SDK_VERSION,
            &d3d_, &fl, &ctx_);
    }
    return SUCCEEDED(hr) && d3d_ && ctx_;
}

bool Renderer::CreateSwapChain(HWND hwnd) {
    ComPtr<IDXGIDevice>  dxgi_dev;
    if (FAILED(d3d_->QueryInterface(IID_PPV_ARGS(&dxgi_dev)))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_dev->GetAdapter(&adapter)))                return false;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))    return false;

    RECT rc{}; GetClientRect(hwnd, &rc);
    swap_w_ = (UINT)(rc.right  - rc.left);
    swap_h_ = (UINT)(rc.bottom - rc.top);
    if (swap_w_ == 0) swap_w_ = 1;
    if (swap_h_ == 0) swap_h_ = 1;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width            = swap_w_;
    sd.Height           = swap_h_;
    sd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = 2;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;

    HRESULT hr = factory->CreateSwapChainForHwnd(
        d3d_.Get(), hwnd, &sd, nullptr, nullptr, &swap_);
    return SUCCEEDED(hr) && swap_;
}

bool Renderer::CreateBackBufferRTV() {
    rtv_.Reset();
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
    HRESULT hr = d3d_->CreateRenderTargetView(bb.Get(), nullptr, &rtv_);
    return SUCCEEDED(hr) && rtv_;
}

bool Renderer::Init(HWND control_panel_hwnd) {
    hwnd_ = control_panel_hwnd;
    if (!CreateD3D())                       return false;
    if (!CreateSwapChain(hwnd_))            return false;
    if (!CreateBackBufferRTV())             return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(hwnd_))               return false;
    if (!ImGui_ImplDX11_Init(d3d_.Get(), ctx_.Get())) return false;
    imgui_init_ = true;
    return true;
}

bool Renderer::RecreateDevice() {
    if (!hwnd_) return false;
    Log(NV3D::LogLevel::Warning, L"Renderer::RecreateDevice  begin");

    // Drop the ImGui DX11 backend (it caches device-bound resources). The
    // Win32 backend and ImGui::CreateContext are device-agnostic; leave them.
    if (imgui_init_) {
        ImGui_ImplDX11_Shutdown();
    }

    // Release every D3D11 resource we own. Scaler shaders + sampler/blend/
    // rasterizer state too — they're device-bound; the next scaler call will
    // rebuild them via InitScaler.
    ReleaseStaging();
    rtv_.Reset();
    scale_vs_.Reset();
    scale_ps_.Reset();
    scale_sampler_.Reset();
    scale_rs_.Reset();
    scale_blend_.Reset();
    scaler_ready_ = false;
    swap_.Reset();
    ctx_.Reset();
    d3d_.Reset();

    if (!CreateD3D()) {
        Log(NV3D::LogLevel::Error, L"Renderer::RecreateDevice  CreateD3D failed");
        imgui_init_ = false;   // ImGui DX11 backend is gone and we couldn't rebuild it
        return false;
    }
    if (!CreateSwapChain(hwnd_)) {
        Log(NV3D::LogLevel::Error, L"Renderer::RecreateDevice  CreateSwapChain failed");
        imgui_init_ = false;
        return false;
    }
    if (!CreateBackBufferRTV()) {
        Log(NV3D::LogLevel::Error, L"Renderer::RecreateDevice  CreateBackBufferRTV failed");
        imgui_init_ = false;
        return false;
    }
    if (imgui_init_ && !ImGui_ImplDX11_Init(d3d_.Get(), ctx_.Get())) {
        Log(NV3D::LogLevel::Error, L"Renderer::RecreateDevice  ImGui_ImplDX11_Init failed");
        imgui_init_ = false;
        return false;
    }
    Log(NV3D::LogLevel::Info, L"Renderer::RecreateDevice  complete");
    return true;
}

void Renderer::Shutdown() {
    // Step-by-step logs so a freeze in any one release pinpoints itself.
    // Same rationale as App::Shutdown's checkpoints — the freeze on quit
    // goes silent through this path; we need to know which step blocks.
    if (imgui_init_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imgui_init_ = false;
        Log(NV3D::LogLevel::Info, L"Renderer::Shutdown  ImGui backend torn down");
    }
    ReleaseStaging();
    rtv_.Reset();
    Log(NV3D::LogLevel::Info, L"Renderer::Shutdown  releasing swap chain");
    if (ctx_) ctx_->ClearState();
    if (ctx_) ctx_->Flush();
    swap_.Reset();
    Log(NV3D::LogLevel::Info, L"Renderer::Shutdown  swap chain released");
    ctx_.Reset();
    d3d_.Reset();
    Log(NV3D::LogLevel::Info, L"Renderer::Shutdown  D3D11 device released");
    hwnd_ = nullptr;
}

void Renderer::HandleResize(UINT w, UINT h) {
    if (!swap_ || w == 0 || h == 0) return;
    if (w == swap_w_ && h == swap_h_) return;
    rtv_.Reset();
    if (SUCCEEDED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
        swap_w_ = w;
        swap_h_ = h;
        CreateBackBufferRTV();
    }
}

void Renderer::BeginImGuiFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void Renderer::EndImGuiFrame() {
    ImGui::Render();
    if (rtv_) {
        const float clear[4] = { 0.10f, 0.11f, 0.13f, 1.0f };
        ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
        ctx_->ClearRenderTargetView(rtv_.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    // Present(0, 0) instead of Present(1, 0): no vsync block on the control-
    // panel swap chain. The 1-vsync wait per tick was eating ~8ms of CPU
    // every Tick whether or not anything moved in the GUI — that's GPU+CPU
    // time the captured source could be using.
    if (swap_) swap_->Present(0, 0);
}

bool Renderer::CreateFixedStaging(UINT w, UINT h) {
    if (!d3d_ || w == 0 || h == 0) return false;
    if (staging_ && w == staging_w_ && h == staging_h_) return true;
    staging_rtv_.Reset();
    staging_.Reset();
    staging_w_ = 0; staging_h_ = 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    // BIND_RENDER_TARGET so we can draw the scaler shader pass into staging.
    // BIND_SHADER_RESOURCE because NV3DLib opens the shared D3D9 view onto it.
    // MISC_SHARED is the NV3DLib fast path requirement.
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &staging_))) return false;
    if (FAILED(d3d_->CreateRenderTargetView(staging_.Get(), nullptr, &staging_rtv_))) {
        staging_.Reset();
        return false;
    }
    staging_w_ = w;
    staging_h_ = h;
    return true;
}

bool Renderer::CopyCaptureToStaging(ID3D11Texture2D* src) {
    if (!ctx_ || !staging_ || !src) return false;
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    // Fast path requires BOTH matching dimensions AND a format compatible
    // with our B8G8R8A8_UNORM staging. CopyResource silently no-ops on
    // cross-group format mismatches (e.g. a Geo-11 producer publishing
    // R10G10B10A2_UNORM HDR), which manifests as a black 3D panel because
    // the staging never gets written. The scaler path handles any source
    // format by sampling through an SRV into the staging RTV (OM does the
    // implicit precision conversion).
    const bool format_ok =
        sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        sd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if (sd.Width == staging_w_ && sd.Height == staging_h_ && format_ok) {
        // Dim+format-match fast path — no shader needed.
        ctx_->CopyResource(staging_.Get(), src);
        return true;
    }
    return ScaleCaptureToStaging(src);
}

bool Renderer::CopyCaptureRegionToStaging(ID3D11Texture2D* src, const RECT& src_box) {
    if (!ctx_ || !staging_ || !src) return false;
    const LONG box_w = src_box.right  - src_box.left;
    const LONG box_h = src_box.bottom - src_box.top;
    if (box_w <= 0 || box_h <= 0) return CopyCaptureToStaging(src);

    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    const bool format_ok =
        sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        sd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;

    // Fast path: requested sub-rect already matches staging dims + format is
    // BGRA. CopySubresourceRegion is a 1:1 GPU memcpy of the rectangle —
    // much cheaper than the scaler shader pass, and the typical case once
    // staging is sized to the source's client-area dim at session start.
    if (static_cast<UINT>(box_w) == staging_w_ &&
        static_cast<UINT>(box_h) == staging_h_ && format_ok) {
        D3D11_BOX box{};
        box.left   = static_cast<UINT>(src_box.left);
        box.top    = static_cast<UINT>(src_box.top);
        box.front  = 0;
        box.right  = static_cast<UINT>(src_box.right);
        box.bottom = static_cast<UINT>(src_box.bottom);
        box.back   = 1;
        ctx_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, src, 0, &box);
        return true;
    }

    // Box dim doesn't match staging — fall through to the scaler. The scaler
    // currently samples src whole-image; cropping via UV would be a nicer
    // extension but isn't needed for the common case where staging is sized
    // to match the client area at session start.
    return ScaleCaptureToStaging(src);
}

namespace {

// Vertex shader: generate a single fullscreen triangle from SV_VertexID. No
// vertex buffer needed. UVs span [0,1] across the staging RTV.
constexpr const char* kScaleVS = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    float2 p = float2((vid << 1) & 2, vid & 2);
    o.uv  = p;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

// Pixel shader: bilinear sample the source at the staging UV. The source can
// be any aspect / resolution — the sample naturally stretches it to fill the
// staging texture.
constexpr const char* kScalePS = R"(
Texture2D    g_src : register(t0);
SamplerState g_smp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return g_src.Sample(g_smp, uv);
}
)";

}  // anonymous

bool Renderer::InitScaler() {
    if (scaler_ready_) return true;
    if (!d3d_)         return false;

    ComPtr<ID3DBlob> vs_blob, ps_blob, errors;
    if (FAILED(D3DCompile(kScaleVS, std::strlen(kScaleVS), nullptr, nullptr, nullptr,
                          "main", "vs_5_0", 0, 0, &vs_blob, &errors))) return false;
    if (FAILED(D3DCompile(kScalePS, std::strlen(kScalePS), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, &ps_blob, &errors))) return false;
    if (FAILED(d3d_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                         nullptr, &scale_vs_))) return false;
    if (FAILED(d3d_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                                        nullptr, &scale_ps_))) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD         = 0;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    if (FAILED(d3d_->CreateSamplerState(&sd, &scale_sampler_))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(d3d_->CreateRasterizerState(&rd, &scale_rs_))) return false;

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable           = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(d3d_->CreateBlendState(&bd, &scale_blend_))) return false;

    scaler_ready_ = true;
    return true;
}

bool Renderer::ScaleCaptureToStaging(ID3D11Texture2D* src) {
    if (!InitScaler() || !staging_rtv_) return false;

    // Build an SRV onto the captured texture. Cache by source-pointer
    // identity: Katanga's shared texture stays at the same ID3D11Texture2D*
    // for many frames (it's reimported only on producer-side handle
    // rotation), so re-creating an SRV every Tick was burning ~125 create
    // /release cycles per second against cross-process shared GPU memory.
    // On a steady-state Katanga stream this collapses to a single SRV.
    D3D11_TEXTURE2D_DESC td{};
    src->GetDesc(&td);
    if (!scale_cached_srv_ ||
        scale_cached_src_ != src ||
        scale_cached_fmt_ != td.Format) {
        scale_cached_srv_.Reset();
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format                    = td.Format;
        sv.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MostDetailedMip = 0;
        sv.Texture2D.MipLevels       = 1;
        if (FAILED(d3d_->CreateShaderResourceView(src, &sv, &scale_cached_srv_))) return false;
        scale_cached_src_ = src;
        scale_cached_fmt_ = td.Format;
    }
    ID3D11ShaderResourceView* srv_ptr = scale_cached_srv_.Get();

    D3D11_VIEWPORT vp{};
    vp.Width    = static_cast<float>(staging_w_);
    vp.Height   = static_cast<float>(staging_h_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    ID3D11RenderTargetView* rtv = staging_rtv_.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(scale_rs_.Get());
    const float blend_factor[4] = { 1, 1, 1, 1 };
    ctx_->OMSetBlendState(scale_blend_.Get(), blend_factor, 0xFFFFFFFF);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->IASetInputLayout(nullptr);
    ctx_->VSSetShader(scale_vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(scale_ps_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[]   = { srv_ptr };
    ID3D11SamplerState*       samps[]  = { scale_sampler_.Get() };
    ctx_->PSSetShaderResources(0, 1, srvs);
    ctx_->PSSetSamplers(0, 1, samps);

    ctx_->Draw(3, 0);

    // Unbind so other pipelines (ImGui's compose pass, NV3DLib's import) don't
    // see leftover state.
    ID3D11ShaderResourceView* null_srv[] = { nullptr };
    ctx_->PSSetShaderResources(0, 1, null_srv);
    ID3D11RenderTargetView* null_rtv[] = { nullptr };
    ctx_->OMSetRenderTargets(1, null_rtv, nullptr);
    return true;
}

void Renderer::FillTestPattern(uint32_t frame) {
    if (!d3d_ || !ctx_ || !staging_) return;

    // Build the pattern in CPU-side memory, then UpdateSubresource into the
    // MISC_SHARED staging. (Map() doesn't work on MISC_SHARED + USAGE_DEFAULT.)
    const UINT W = staging_w_;
    const UINT H = staging_h_;
    const UINT per_eye_w = W / 2;
    const UINT pitch     = W * 4u;

    static thread_local std::vector<uint8_t> px;
    px.assign(static_cast<size_t>(W) * H * 4u, 0);

    const UINT cx = per_eye_w / 2;
    const UINT cy = H / 2;
    const int disparity = static_cast<int>(20 + 15 * std::sin(frame * 0.04f));
    const int quad_half = 40;

    for (UINT y = 0; y < H; ++y) {
        uint8_t* row = px.data() + y * pitch;
        for (UINT x = 0; x < W; ++x) {
            uint8_t* p = row + x * 4u;   // BGRA
            const bool right_eye = (x >= per_eye_w);
            const UINT lx = right_eye ? (x - per_eye_w) : x;
            // Red ramp in left eye, green ramp in right eye.
            if (!right_eye) {
                p[0] = 0;
                p[1] = 0;
                p[2] = static_cast<uint8_t>(lx * 255u / per_eye_w);
                p[3] = 0xFF;
            } else {
                p[0] = 0;
                p[1] = static_cast<uint8_t>(lx * 255u / per_eye_w);
                p[2] = 0;
                p[3] = 0xFF;
            }
            // White quad with mirrored disparity so it pops out under stereo.
            int qx = static_cast<int>(lx) - static_cast<int>(cx);
            int qy = static_cast<int>(y)  - static_cast<int>(cy);
            qx += right_eye ? -disparity : disparity;
            if (qx >= -quad_half && qx <= quad_half &&
                qy >= -quad_half && qy <= quad_half) {
                p[0] = p[1] = p[2] = 0xFF;
            }
        }
    }

    ctx_->UpdateSubresource(staging_.Get(), 0, nullptr, px.data(), pitch, 0);
}

void Renderer::ReleaseStaging() {
    staging_rtv_.Reset();
    staging_.Reset();
    staging_w_ = 0;
    staging_h_ = 0;
    // Drop the cached scaler SRV — the source pointer was tied to the prior
    // capture session; next session's first scaler call will recreate it.
    scale_cached_srv_.Reset();
    scale_cached_src_ = nullptr;
    scale_cached_fmt_ = DXGI_FORMAT_UNKNOWN;
}

}  // namespace nv3dg
