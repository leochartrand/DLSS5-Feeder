// ui_probe=N (dlss5-feed.cfg): log N frames of the game's render-target binds, clears, draws and copies,
// to find where the scene ends and the UI begins -- the point a pre-UI colour capture has to hook.
// Diagnostics only; nothing is changed. Starts after ui_probe_after presents (default 600, past loading).
#pragma once

#include <cstdio>
#include <reshade.hpp>

namespace uiprobe {

static int      g_frames_left = 0;     // frames still to log
static uint64_t g_present_count = 0;
static uint64_t g_start_at = 600;
static uint32_t g_events = 0;          // in the current frame
static reshade::api::viewport g_vp = {};
static uint64_t g_rt = 0;              // current RTV's resource handle
static void (*g_log)(const char *, ...) = nullptr;

static void Describe(reshade::api::device *dev, reshade::api::resource_view v, char *out, size_t n)
{
    if (v.handle == 0) { snprintf(out, n, "-"); return; }
    const reshade::api::resource r = dev->get_resource_from_view(v);
    const reshade::api::resource_desc d = dev->get_resource_desc(r);
    snprintf(out, n, "res %08llx %ux%u fmt %u", (unsigned long long)r.handle, d.texture.width, d.texture.height,
             (unsigned)d.texture.format);
}

static bool Active() { return g_frames_left > 0 && g_present_count >= g_start_at && g_events < 4000; }

static void OnBind(reshade::api::command_list *cmd, uint32_t count, const reshade::api::resource_view *rtvs,
                   reshade::api::resource_view dsv)
{
    if (count > 0 && rtvs[0].handle != 0)
        g_rt = cmd->get_device()->get_resource_from_view(rtvs[0]).handle;
    else
        g_rt = 0;
    if (!Active()) return;
    char rt[96], ds[96];
    Describe(cmd->get_device(), count > 0 ? rtvs[0] : reshade::api::resource_view{0}, rt, sizeof(rt));
    Describe(cmd->get_device(), dsv, ds, sizeof(ds));
    g_log("[uiprobe] f%llu #%u bind %u rt [%s] ds [%s]", (unsigned long long)g_present_count, g_events++, count, rt, ds);
}

static void OnViewports(reshade::api::command_list *, uint32_t first, uint32_t count, const reshade::api::viewport *vps)
{
    if (first == 0 && count > 0) g_vp = vps[0];
}

static bool OnDraw(reshade::api::command_list *, uint32_t vertices, uint32_t instances, uint32_t, uint32_t)
{
    if (Active())
        g_log("[uiprobe] f%llu #%u draw %u x%u vp %.0fx%.0f+%.0f,%.0f rt %08llx", (unsigned long long)g_present_count,
              g_events++, vertices, instances, g_vp.width, g_vp.height, g_vp.x, g_vp.y, (unsigned long long)g_rt);
    return false;
}

static bool OnDrawIndexed(reshade::api::command_list *, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t)
{
    if (Active())
        g_log("[uiprobe] f%llu #%u drawi %u x%u vp %.0fx%.0f+%.0f,%.0f rt %08llx", (unsigned long long)g_present_count,
              g_events++, indices, instances, g_vp.width, g_vp.height, g_vp.x, g_vp.y, (unsigned long long)g_rt);
    return false;
}

static bool OnClearRt(reshade::api::command_list *cmd, reshade::api::resource_view rtv, const float c[4], uint32_t, const reshade::api::rect *)
{
    if (Active())
    {
        char rt[96];
        Describe(cmd->get_device(), rtv, rt, sizeof(rt));
        g_log("[uiprobe] f%llu #%u clear rt [%s] (%.2f %.2f %.2f %.2f)", (unsigned long long)g_present_count, g_events++, rt,
              c[0], c[1], c[2], c[3]);
    }
    return false;
}

static bool OnClearDs(reshade::api::command_list *cmd, reshade::api::resource_view dsv, const float *, const uint8_t *, uint32_t, const reshade::api::rect *)
{
    if (Active())
    {
        char ds[96];
        Describe(cmd->get_device(), dsv, ds, sizeof(ds));
        g_log("[uiprobe] f%llu #%u clear ds [%s]", (unsigned long long)g_present_count, g_events++, ds);
    }
    return false;
}

static bool OnCopy(reshade::api::command_list *, reshade::api::resource src, reshade::api::resource dst)
{
    if (Active())
        g_log("[uiprobe] f%llu #%u copy %08llx -> %08llx", (unsigned long long)g_present_count, g_events++,
              (unsigned long long)src.handle, (unsigned long long)dst.handle);
    return false;
}

static void OnPresentProbe(reshade::api::command_queue *, reshade::api::swapchain *sc, const reshade::api::rect *,
                           const reshade::api::rect *, uint32_t, const reshade::api::rect *)
{
    if (Active())
        g_log("[uiprobe] f%llu present backbuffer %08llx (%u events)", (unsigned long long)g_present_count,
              (unsigned long long)sc->get_current_back_buffer().handle, g_events);
    if (g_present_count >= g_start_at && g_frames_left > 0) --g_frames_left;
    ++g_present_count;
    g_events = 0;
}

static void Register(int frames, int after, void (*log)(const char *, ...))
{
    if (frames <= 0) return;
    g_frames_left = frames;
    g_start_at = after > 0 ? (uint64_t)after : 600;
    g_log = log;
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBind);
    reshade::register_event<reshade::addon_event::bind_viewports>(OnViewports);
    reshade::register_event<reshade::addon_event::draw>(OnDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
    reshade::register_event<reshade::addon_event::clear_render_target_view>(OnClearRt);
    reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(OnClearDs);
    reshade::register_event<reshade::addon_event::copy_resource>(OnCopy);
    reshade::register_event<reshade::addon_event::present>(OnPresentProbe);
    log("[uiprobe] logging %d frame(s) of binds, clears, draws and copies from present %d", frames, (int)g_start_at);
}

}  // namespace uiprobe
