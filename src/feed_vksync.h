// feed_vksync.h -- vk_sync=1 (dlss5-feed.cfg): the frame handoff on the GPU timeline under Wine/Proton,
// where D3D12 fences cannot be shared between processes (#121) and the no-fence fallback hands every frame
// through the CPU (a device drain on the game's side, a CPU signal on the host's).
//
// Two plain Vulkan TIMELINE semaphores, made by the host on its vkd3d-proton device and exported as opaque
// Win32 handles, carry the two edges the shared fences did:
//   in:  the game's copies of frame n have landed  (the game's DXVK queue signals n; the host's queue waits)
//   out: the host has finished frame n             (the host's queue signals n; the game's queue waits)
// Wine shares an opaque semaphore as the host driver's file descriptor (a d3dkmt sync object), which works
// across processes; only its D3D12-fence emulation is process-local, and an opaque semaphore never uses it.
//
// Each side submits onto its own API's Vulkan queue through that API's interop interface, with the queue
// locked against the API's own submissions:
//   game: DXVK's IDXGIVkInteropDevice -- FlushRenderingCommands first, which returns once the frame's work
//         is on the Vulkan queue, so a signal submitted after it is ordered after that work
//   host: vkd3d-proton's ID3D12DXVKInteropDevice -- LockCommandQueue drains what the D3D12 queue has
//         pending first, so a wait submitted under it comes before anything the host queues afterwards
// Compiled for both architectures: the 32-bit add-on (DXVK) and the 64-bit host (vkd3d-proton).
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>
#include <unknwn.h>
#include <cstdint>

// DXVK's IDXGIVkInteropDevice (dxgi_interfaces.h), queried from its ID3D11Device.
MIDL_INTERFACE("e2ef5fa5-dc21-4af7-90c4-f67ef6a09323")
IFeedDxvkInterop : public IUnknown
{
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance *instance, VkPhysicalDevice *phys, VkDevice *device) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue *queue, uint32_t *family) = 0;
    virtual void STDMETHODCALLTYPE TransitionSurfaceLayout(IUnknown *surface, const VkImageSubresourceRange *ranges,
                                                           VkImageLayout from, VkImageLayout to) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
};

// vkd3d-proton's ID3D12DXVKInteropDevice (vkd3d_device_vkd3d_ext.h), queried from its ID3D12Device.
struct ID3D12CommandQueue;
struct ID3D12Resource;
MIDL_INTERFACE("39da4e09-bd1c-4198-9fae-86bbe3be41fd")
IFeedVkd3dInterop : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetDXGIAdapter(REFIID iid, void **object) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInstanceExtensions(UINT *count, const char **names) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceExtensions(UINT *count, const char **names) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFeatures(const VkPhysicalDeviceFeatures2 **features) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanHandles(VkInstance *instance, VkPhysicalDevice *phys, VkDevice *device) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanQueueInfo(ID3D12CommandQueue *queue, VkQueue *vk_queue, UINT32 *family) = 0;
    virtual void STDMETHODCALLTYPE GetVulkanImageLayout(ID3D12Resource *resource, UINT state, VkImageLayout *layout) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanResourceInfo(ID3D12Resource *resource, UINT64 *handle, UINT64 *offset) = 0;
    virtual HRESULT STDMETHODCALLTYPE LockCommandQueue(ID3D12CommandQueue *queue) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnlockCommandQueue(ID3D12CommandQueue *queue) = 0;
};

// The few Vulkan entry points the handoff needs, from the vulkan-1.dll the API itself loaded.
struct FeedVkSync
{
    VkDevice dev = nullptr;
    VkQueue  queue = nullptr;
    VkSemaphore in = VK_NULL_HANDLE, out = VK_NULL_HANDLE;
    PFN_vkCreateSemaphore                 CreateSemaphore_ = nullptr;
    PFN_vkDestroySemaphore                DestroySemaphore_ = nullptr;
    PFN_vkGetSemaphoreWin32HandleKHR      GetSemaphoreWin32Handle = nullptr;
    PFN_vkImportSemaphoreWin32HandleKHR   ImportSemaphoreWin32Handle = nullptr;
    PFN_vkQueueSubmit                     QueueSubmit = nullptr;
    PFN_vkSignalSemaphore                 SignalSemaphore = nullptr;
    PFN_vkGetSemaphoreCounterValue        GetSemaphoreCounterValue = nullptr;
};

static bool FeedVkSyncLoad(FeedVkSync *s, VkInstance instance, VkDevice device, VkQueue queue)
{
    HMODULE m = GetModuleHandleA("vulkan-1.dll");
    if (m == nullptr) m = LoadLibraryA("vulkan-1.dll");
    if (m == nullptr || instance == nullptr || device == nullptr) return false;
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(m, "vkGetInstanceProcAddr"));
    const auto gdpa = gipa != nullptr ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr")) : nullptr;
    if (gdpa == nullptr) return false;
    auto get = [&](const char *name, const char *khr) {
        PFN_vkVoidFunction f = gdpa(device, name);
        return f != nullptr || khr == nullptr ? f : gdpa(device, khr);
    };
    s->dev = device;
    s->queue = queue;
    s->CreateSemaphore_           = reinterpret_cast<PFN_vkCreateSemaphore>(get("vkCreateSemaphore", nullptr));
    s->DestroySemaphore_          = reinterpret_cast<PFN_vkDestroySemaphore>(get("vkDestroySemaphore", nullptr));
    s->GetSemaphoreWin32Handle    = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(get("vkGetSemaphoreWin32HandleKHR", nullptr));
    s->ImportSemaphoreWin32Handle = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(get("vkImportSemaphoreWin32HandleKHR", nullptr));
    s->QueueSubmit                = reinterpret_cast<PFN_vkQueueSubmit>(get("vkQueueSubmit", nullptr));
    s->SignalSemaphore            = reinterpret_cast<PFN_vkSignalSemaphore>(get("vkSignalSemaphore", "vkSignalSemaphoreKHR"));
    s->GetSemaphoreCounterValue   = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(get("vkGetSemaphoreCounterValue", "vkGetSemaphoreCounterValueKHR"));
    return s->CreateSemaphore_ && s->DestroySemaphore_ && s->GetSemaphoreWin32Handle && s->ImportSemaphoreWin32Handle &&
           s->QueueSubmit && s->SignalSemaphore && s->GetSemaphoreCounterValue;
}

// A timeline semaphore at 0, exportable as an opaque Win32 handle (export) or ready to import one.
static VkSemaphore FeedVkSyncNewTimeline(FeedVkSync *s, bool exportable)
{
    VkExportSemaphoreCreateInfo ex = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
    ex.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkSemaphoreTypeCreateInfo tci = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    tci.pNext = exportable ? &ex : nullptr;
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    sci.pNext = &tci;
    VkSemaphore sem = VK_NULL_HANDLE;
    return s->CreateSemaphore_(s->dev, &sci, nullptr, &sem) == VK_SUCCESS ? sem : VK_NULL_HANDLE;
}

// Host: the opaque NT handle of an exportable semaphore, owned by the caller (null on failure).
static HANDLE FeedVkSyncExport(FeedVkSync *s, VkSemaphore sem)
{
    VkSemaphoreGetWin32HandleInfoKHR gi = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
    gi.semaphore = sem;
    gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    HANDLE h = nullptr;
    return s->GetSemaphoreWin32Handle(s->dev, &gi, &h) == VK_SUCCESS ? h : nullptr;
}

// Game: the host's semaphore, from the handle it duplicated in (the handle is not consumed).
static VkSemaphore FeedVkSyncImport(FeedVkSync *s, HANDLE h)
{
    VkSemaphore sem = FeedVkSyncNewTimeline(s, false);
    if (sem == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    VkImportSemaphoreWin32HandleInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
    imp.semaphore = sem;
    imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    imp.handle = h;
    if (s->ImportSemaphoreWin32Handle(s->dev, &imp) != VK_SUCCESS)
    {
        s->DestroySemaphore_(s->dev, sem, nullptr);
        return VK_NULL_HANDLE;
    }
    return sem;
}

// One GPU-side wait for (wait = true) or signal of `value` on `sem`, alone in a submission on s->queue.
// The caller holds the API's submission lock.
static bool FeedVkSyncSubmit(FeedVkSync *s, VkSemaphore sem, uint64_t value, bool wait)
{
    VkTimelineSemaphoreSubmitInfo ti = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext = &ti;
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (wait)
    {
        ti.waitSemaphoreValueCount = 1;
        ti.pWaitSemaphoreValues = &value;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &sem;
        si.pWaitDstStageMask = &stage;
    }
    else
    {
        ti.signalSemaphoreValueCount = 1;
        ti.pSignalSemaphoreValues = &value;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sem;
    }
    return s->QueueSubmit(s->queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS;
}

// The semaphore's value now (0 on failure).
static uint64_t FeedVkSyncValue(FeedVkSync *s, VkSemaphore sem)
{
    uint64_t v = 0;
    return sem != VK_NULL_HANDLE && s->GetSemaphoreCounterValue(s->dev, sem, &v) == VK_SUCCESS ? v : 0;
}

// A CPU signal, for releasing a wait nothing else will: only ever forward (a timeline cannot go back).
// `cap` keeps the jump within the device's maxTimelineSemaphoreValueDifference (2^31 is the minimum).
static void FeedVkSyncCpuSignal(FeedVkSync *s, VkSemaphore sem, uint64_t value)
{
    if (sem == VK_NULL_HANDLE) return;
    const uint64_t now = FeedVkSyncValue(s, sem);
    if (value <= now) return;
    const uint64_t cap = now + (1ull << 30);
    VkSemaphoreSignalInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
    si.semaphore = sem;
    si.value = value < cap ? value : cap;
    s->SignalSemaphore(s->dev, &si);
}
