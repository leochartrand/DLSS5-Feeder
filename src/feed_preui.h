// preui=1 (dlss5-feed.cfg, D3D11): feed the neural pass the frame as it was BEFORE the game drew its UI, and
// keep the game's own pixels wherever the UI changed them when the result comes home -- the UI is neither
// re-lit by the model nor painted over by an answer from a frame without it.
//
// Where the UI starts: games draw the finished scene into the surface that ends up on screen with one
// full-screen pass (3, 4 or 6 vertices), then the UI into the same surface with many small draws. The
// surface is the swapchain backbuffer, or (dgVoodoo and other wrappers) a surface of their own that is
// copied to the backbuffer just before Present -- learnt here from those copies. Just before the first
// non-full-screen draw into it after a full-screen one, the surface is copied aside: that copy is the
// pre-UI frame. A frame with no such draw has no UI, and the backbuffer is fed as before.
// ui_probe=N (feed_uiprobe.h) logs the draw sequence this relies on.
#pragma once

#include <d3d11.h>
#include <reshade.hpp>

namespace preui {

static bool     g_on = false;
static uint64_t g_backbuffers[4] = {};     // swapchain backbuffer handles seen at present
static uint64_t g_finals[4] = {};          // surfaces copied onto a backbuffer: where the frame is finished
static uint64_t g_rt = 0;                  // the render target bound now
static uint64_t g_scene_rt = 0;            // a final surface that just received a full-screen pass
static bool     g_captured = false;        // the pre-UI copy of this frame exists
static ID3D11Texture2D          *g_tex = nullptr;   // the pre-UI frame
static ID3D11ShaderResourceView *g_srv = nullptr;
static D3D11_TEXTURE2D_DESC      g_desc = {};
static uint64_t g_captures = 0;
static void (*g_log)(const char *, ...) = nullptr;

static bool In(const uint64_t (&set)[4], uint64_t h)
{
    for (uint64_t v : set) if (v != 0 && v == h) return true;
    return false;
}

static void Add(uint64_t (&set)[4], uint64_t h)
{
    if (h == 0 || In(set, h)) return;
    for (int i = 3; i > 0; --i) set[i] = set[i - 1];
    set[0] = h;
}

// A typeless 8-bit surface is viewed as UNORM, the way the game's own colour is read.
static DXGI_FORMAT ViewFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:    return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:    return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                               return f;
    }
}

static void Capture(ID3D11DeviceContext *ctx, ID3D11Resource *src)
{
    ID3D11Texture2D *t = nullptr;
    if (FAILED(src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&t)))) return;
    D3D11_TEXTURE2D_DESC d = {};
    t->GetDesc(&d);
    if (g_tex == nullptr || d.Width != g_desc.Width || d.Height != g_desc.Height || d.Format != g_desc.Format)
    {
        if (g_srv) { g_srv->Release(); g_srv = nullptr; }
        if (g_tex) { g_tex->Release(); g_tex = nullptr; }
        ID3D11Device *dev = nullptr;
        ctx->GetDevice(&dev);
        D3D11_TEXTURE2D_DESC n = d;
        n.MipLevels = 1;
        n.ArraySize = 1;
        n.SampleDesc = {1, 0};
        n.Usage = D3D11_USAGE_DEFAULT;
        n.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        n.CPUAccessFlags = 0;
        n.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = ViewFormat(d.Format);
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        const bool ok = dev != nullptr && d.SampleDesc.Count == 1 && SUCCEEDED(dev->CreateTexture2D(&n, nullptr, &g_tex)) &&
                        SUCCEEDED(dev->CreateShaderResourceView(g_tex, &sv, &g_srv));
        if (dev) dev->Release();
        if (!ok)
        {
            if (g_srv) { g_srv->Release(); g_srv = nullptr; }
            if (g_tex) { g_tex->Release(); g_tex = nullptr; }
            g_log("[preui] could not create the pre-UI surface (%ux%u fmt %u, %u samples): feeding the backbuffer",
                  d.Width, d.Height, (unsigned)d.Format, d.SampleDesc.Count);
            g_on = false;
            t->Release();
            return;
        }
        g_desc = d;
        g_log("[preui] pre-UI surface %ux%u fmt %u", d.Width, d.Height, (unsigned)d.Format);
    }
    ctx->CopyResource(g_tex, t);
    t->Release();
    g_captured = true;
    if (++g_captures == 1) g_log("[preui] first pre-UI capture (the UI starts after a full-screen pass into the final surface)");
}

static void OnBind(reshade::api::command_list *cmd, uint32_t count, const reshade::api::resource_view *rtvs,
                   reshade::api::resource_view)
{
    g_rt = count > 0 && rtvs[0].handle != 0 ? cmd->get_device()->get_resource_from_view(rtvs[0]).handle : 0;
}

static void OnAnyDraw(reshade::api::command_list *cmd, bool fullscreen)
{
    if (!g_on || g_rt == 0 || !In(g_finals, g_rt)) return;
    if (fullscreen)
    {
        g_scene_rt = g_rt;     // the finished scene (again): any earlier capture is stale
        g_captured = false;
        return;
    }
    if (g_scene_rt == g_rt && !g_captured)
    {
        auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
        if (ctx != nullptr) Capture(ctx, reinterpret_cast<ID3D11Resource *>(g_rt));
    }
}

static bool OnDraw(reshade::api::command_list *cmd, uint32_t vertices, uint32_t, uint32_t, uint32_t)
{
    OnAnyDraw(cmd, vertices <= 6);
    return false;
}

static bool OnDrawIndexed(reshade::api::command_list *cmd, uint32_t indices, uint32_t, uint32_t, int32_t, uint32_t)
{
    OnAnyDraw(cmd, indices <= 6);
    return false;
}

static bool OnCopy(reshade::api::command_list *, reshade::api::resource src, reshade::api::resource dst)
{
    if (In(g_backbuffers, dst.handle) && !In(g_finals, src.handle))
    {
        Add(g_finals, src.handle);
        g_log("[preui] the frame is finished in %08llx (copied to the backbuffer)", (unsigned long long)src.handle);
    }
    return false;
}

static void OnPresent(reshade::api::command_queue *, reshade::api::swapchain *sc, const reshade::api::rect *,
                      const reshade::api::rect *, uint32_t, const reshade::api::rect *)
{
    const uint64_t bb = sc->get_current_back_buffer().handle;
    if (!In(g_backbuffers, bb)) { Add(g_backbuffers, bb); Add(g_finals, bb); }
}

// The UI composite for the blit home: a copy of the backbuffer as the game finished it (UI included), made
// just before the result is drawn over it, and the pixel shader that keeps the game's pixel wherever that
// copy differs from the pre-UI frame.
static ID3D11Texture2D          *g_ui_tex = nullptr;
static ID3D11ShaderResourceView *g_ui_srv = nullptr;
static ID3D11PixelShader        *g_ui_ps  = nullptr;
static bool                      g_used   = false;   // this frame was fed the pre-UI frame

static const char kUiSrc[] =
    "Texture2D nr_tex : register(t0);\n"
    "Texture2D ui_tex : register(t1);\n"
    "Texture2D pre_tex : register(t2);\n"
    "SamplerState smp : register(s0);\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 ps_ui(VSOut i) : SV_Target {\n"
    "  float3 nr = nr_tex.Sample(smp, i.uv).rgb;\n"
    "  int3 p = int3(i.pos.xy, 0);\n"
    "  float3 ui = ui_tex.Load(p).rgb;\n"
    "  float3 d = abs(ui - pre_tex.Load(p).rgb);\n"
    "  float m = saturate((max(d.r, max(d.g, d.b)) - 1.5 / 255.0) * 24.0);\n"
    "  return float4(lerp(nr, ui, m), 1.0); }\n";

// The composite's shader, from the add-on's D3DCompile (null: no composite, the blit stays plain).
template <typename Compile>
static void CreateShader(ID3D11Device *dev, Compile compile)
{
    if (g_ui_ps != nullptr || compile == nullptr) return;   // built with the blit, before preui may be armed
    ID3DBlob *ps = nullptr, *err = nullptr;
    const HRESULT hr = compile(kUiSrc, sizeof(kUiSrc) - 1, "feedpreui", nullptr, nullptr, "ps_ui", "ps_4_0", 0, 0, &ps, &err);
    if (SUCCEEDED(hr)) dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ui_ps);
    else if (g_log) g_log("[preui] composite shader compile failed 0x%08X: %s", hr, err ? (const char *)err->GetBufferPointer() : "");
    if (ps) ps->Release();
    if (err) err->Release();
}

// Copy the finished backbuffer aside for the composite. False: no composite this frame.
static bool CopyFinished(ID3D11DeviceContext *ctx, ID3D11Resource *bb)
{
    if (!g_used || g_ui_ps == nullptr || g_srv == nullptr) return false;
    if (g_ui_tex == nullptr)
    {
        ID3D11Device *dev = nullptr;
        ctx->GetDevice(&dev);
        D3D11_TEXTURE2D_DESC n = g_desc;
        n.MipLevels = 1; n.ArraySize = 1; n.SampleDesc = {1, 0}; n.Usage = D3D11_USAGE_DEFAULT;
        n.BindFlags = D3D11_BIND_SHADER_RESOURCE; n.CPUAccessFlags = 0; n.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = ViewFormat(g_desc.Format);
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        const bool ok = dev != nullptr && SUCCEEDED(dev->CreateTexture2D(&n, nullptr, &g_ui_tex)) &&
                        SUCCEEDED(dev->CreateShaderResourceView(g_ui_tex, &sv, &g_ui_srv));
        if (dev) dev->Release();
        if (!ok) { g_log("[preui] could not create the composite copy"); g_ui_ps->Release(); g_ui_ps = nullptr; return false; }
    }
    ID3D11Texture2D *t = nullptr;
    if (FAILED(bb->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&t)))) return false;
    D3D11_TEXTURE2D_DESC d = {};
    t->GetDesc(&d);
    const bool same = d.Width == g_desc.Width && d.Height == g_desc.Height && d.Format == g_desc.Format;
    if (same) ctx->CopyResource(g_ui_tex, t);
    t->Release();
    return same;
}

// The pre-UI frame for this frame's feed, or null (no UI drawn, or off). Consumed: the next frame needs
// its own capture.
static ID3D11Texture2D *Take(const D3D11_TEXTURE2D_DESC &want)
{
    static uint64_t asked = 0, fed = 0, none = 0;
    if (g_on && (++asked % 1800) == 0)
        g_log("[preui] last 1800 frames: %llu fed the pre-UI frame, %llu had no capture",
              (unsigned long long)fed, (unsigned long long)none), fed = none = 0;
    g_used = false;
    if (!g_on || !g_captured || g_tex == nullptr) { if (g_on) ++none; return nullptr; }
    g_captured = false;
    g_scene_rt = 0;
    if (want.Width != g_desc.Width || want.Height != g_desc.Height || want.Format != g_desc.Format)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            g_log("[preui] the backbuffer (%ux%u fmt %u) is not the surface the frame was finished in (%ux%u fmt %u): "
                  "feeding the backbuffer", want.Width, want.Height, (unsigned)want.Format, g_desc.Width,
                  g_desc.Height, (unsigned)g_desc.Format);
        }
        return nullptr;
    }
    ++fed;
    g_used = true;
    return g_tex;
}

static void Register(void (*log)(const char *, ...))
{
    if (g_on) return;
    g_on = true;
    g_log = log;
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBind);
    reshade::register_event<reshade::addon_event::draw>(OnDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
    reshade::register_event<reshade::addon_event::copy_resource>(OnCopy);
    reshade::register_event<reshade::addon_event::present>(OnPresent);
    log("[preui] on: the neural pass gets the frame before the UI; UI pixels keep the game's own colour");
}

}  // namespace preui
