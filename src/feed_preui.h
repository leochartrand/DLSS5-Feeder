// preui=1 (dlss5-feed.cfg, D3D11): the neural pass runs on the game's frame just BEFORE the game draws its
// UI, and the game then draws its UI over the result itself, with its own blending -- the UI is never
// processed by the model, and nothing about it is copied or estimated.
//
// Where the UI starts: games draw the finished scene into the surface that ends up on screen with one
// full-screen pass (3, 4 or 6 vertices), then the UI into the same surface with many small draws. The
// surface is the swapchain backbuffer, or (dgVoodoo and other wrappers) a surface of their own that is
// copied to the backbuffer just before Present -- learnt here from those copies. Just before the first
// non-full-screen draw into it after a full-screen one, the whole effect chain runs on that surface
// (effect_runtime::render_effects: the motion estimator, then DLSS5_Feed, which feeds the frame and draws
// the result into it). ReShade skips its usual effect pass at Present on such a frame; a frame with no UI
// renders at Present as before. The device state around it is saved and restored whole by the add-on
// (SwapDeviceContextState).
// ui_probe=N (feed_uiprobe.h) logs the draw sequence this relies on.
#pragma once

#include <d3d11.h>
#include <reshade.hpp>

namespace preui {

static bool     g_on = false;
static uint64_t g_backbuffers[4] = {};     // swapchain backbuffer handles seen at present
static uint64_t g_finals[4] = {};          // surfaces copied onto a backbuffer: where the frame is finished
static uint64_t g_rt = 0;                  // the render target bound now
static reshade::api::resource_view g_rtv = {};   // and its view
static uint64_t g_scene_rt = 0;            // a final surface that just received a full-screen pass
static uint64_t g_presents = 0;            // presents seen (the frame clock)
static uint64_t g_ran_present = ~0ull;     // g_presents when the effects last ran before the UI
static uint64_t g_ran = 0, g_failed = 0;
// Runs the effect chain on `rtv` (set by the add-on); false when it could not.
static bool (*g_render_here)(reshade::api::command_list *, reshade::api::resource_view) = nullptr;
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

static void OnBind(reshade::api::command_list *cmd, uint32_t count, const reshade::api::resource_view *rtvs,
                   reshade::api::resource_view)
{
    g_rt = count > 0 && rtvs[0].handle != 0 ? cmd->get_device()->get_resource_from_view(rtvs[0]).handle : 0;
    g_rtv = count > 0 ? rtvs[0] : reshade::api::resource_view{};
}

static void OnAnyDraw(reshade::api::command_list *cmd, bool fullscreen)
{
    if (!g_on || g_rt == 0 || !In(g_finals, g_rt)) return;
    // Once per frame, at the FIRST UI draw: UI elements can be full-screen quads too (a backdrop, a fade).
    if (g_ran_present == g_presents) return;
    if (fullscreen)
    {
        g_scene_rt = g_rt;     // the finished scene (again)
        return;
    }
    if (g_scene_rt != g_rt) return;
    g_ran_present = g_presents;
    // The effects bind their own targets; the game's are restored with its state, so is our view of them.
    const uint64_t rt = g_rt;
    const reshade::api::resource_view rtv = g_rtv;
    if (g_render_here != nullptr && g_render_here(cmd, rtv)) ++g_ran;
    else ++g_failed;
    g_rt = rt;
    g_rtv = rtv;
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

// RGBA8 rows of `src` to `path`.
static void WriteRaw(ID3D11DeviceContext *ctx, ID3D11Texture2D *src, const char *path)
{
    D3D11_TEXTURE2D_DESC d = {};
    src->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Device *dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11Texture2D *st = nullptr;
    if (dev && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st)))
    {
        ctx->CopyResource(st, src);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
        {
            if (FILE *f = fopen(path, "wb"))
            {
                for (UINT y = 0; y < d.Height; ++y) fwrite(static_cast<const char *>(m.pData) + (size_t)y * m.RowPitch, 4, d.Width, f);
                fclose(f);
            }
            ctx->Unmap(st, 0);
        }
        st->Release();
    }
    if (dev) dev->Release();
}

static void OnPresent(reshade::api::command_queue *queue, reshade::api::swapchain *sc, const reshade::api::rect *,
                      const reshade::api::rect *, uint32_t, const reshade::api::rect *)
{
    // preui-dump.request beside the exe writes the next 6 finished frames (UI included) as
    // preui-dump-native-<k>.raw (RGBA8 rows) -- the picture as shown.
    static int dump_left = 0;
    if (g_on && (dump_left > 0 || (g_presents % 30 == 0 && GetFileAttributesA("preui-dump.request") != INVALID_FILE_ATTRIBUTES)))
    {
        if (dump_left == 0) { DeleteFileA("preui-dump.request"); dump_left = 6; }
        auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(queue->get_immediate_command_list()->get_native());
        ID3D11Texture2D *t = nullptr;
        auto *bb = reinterpret_cast<ID3D11Resource *>(sc->get_current_back_buffer().handle);
        if (ctx != nullptr && bb != nullptr && SUCCEEDED(bb->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&t))))
        {
            char name[64];
            snprintf(name, sizeof(name), "preui-dump-native-%d.raw", 6 - dump_left);
            WriteRaw(ctx, t, name);
            t->Release();
        }
        if (--dump_left == 0) g_log("[preui] wrote preui-dump-native-{0..5}.raw");
    }
    ++g_presents;
    if (g_on && g_presents % 1800 == 0)
    {
        g_log("[preui] last 1800 frames: %llu ran before the UI, %llu could not", (unsigned long long)g_ran,
              (unsigned long long)g_failed);
        g_ran = g_failed = 0;
    }
    const uint64_t bb = sc->get_current_back_buffer().handle;
    if (!In(g_backbuffers, bb)) { Add(g_backbuffers, bb); Add(g_finals, bb); }
}

// On (1) or off (0), from the cfg or the page, applied from the next draw. The events stay registered once
// armed; off makes them return at once.
static void SetMode(int mode, void (*log)(const char *, ...),
                    bool (*render_here)(reshade::api::command_list *, reshade::api::resource_view))
{
    const bool on = mode != 0 && render_here != nullptr;
    if (on == g_on) return;
    static bool registered = false;
    g_log = log;
    g_render_here = render_here;
    g_on = on;
    g_scene_rt = 0;
    if (on && !registered)
    {
        registered = true;
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBind);
        reshade::register_event<reshade::addon_event::draw>(OnDraw);
        reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
        reshade::register_event<reshade::addon_event::copy_resource>(OnCopy);
        reshade::register_event<reshade::addon_event::present>(OnPresent);
    }
    log(on ? "[preui] on: the neural pass runs before the game's UI, which the game draws over the result"
           : "[preui] off: the neural pass gets the finished frame, UI included");
}

}  // namespace preui
