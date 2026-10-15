/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __WINE_OPENXR_BRIDGE_H
#define __WINE_OPENXR_BRIDGE_H

#include <stdint.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "openxr/openxr.h"

typedef struct IMTLD3D11InteropDevice IMTLD3D11InteropDevice;
#ifndef __d3d11_h__
typedef struct ID3D11Device ID3D11Device;
typedef struct ID3D11DeviceContext ID3D11DeviceContext;
typedef struct ID3D11Texture2D ID3D11Texture2D;
#endif
typedef struct ID3D11DeviceContext4 ID3D11DeviceContext4;
typedef struct ID3D11Fence ID3D11Fence;
#ifndef __d3dcommon_h__
typedef int D3D_FEATURE_LEVEL;
#endif
#ifndef _OBJBASE_H_
typedef struct IUnknown IUnknown;
#endif

#include "unixcall.h"

struct openxr_function
{
    const char *name;
    void *pfn;
    int extension;
    uint32_t core_minor;
    int global;
};

const struct openxr_function *wine_xr_find_function(const char *name);
int wine_xr_extension_index(const char *name);

#ifndef __WINEOPENXR_LIST_INLINE
#define __WINEOPENXR_LIST_INLINE

struct list { struct list *next, *prev; };

static inline void list_add_tail(struct list *head, struct list *entry)
{
    entry->prev = head->prev;
    entry->next = head;
    head->prev->next = entry;
    head->prev = entry;
}

static inline void list_remove(struct list *entry)
{
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
    entry->next = entry->prev = entry;
}

#endif /* __WINEOPENXR_LIST_INLINE */

struct init_params
{
    NTSTATUS result;
};

struct create_d3d11_session_params
{
    XrInstance instance;
    XrSystemId system_id;
    XrSession *session;
    void *mtl_device;             /* id<MTLDevice>, owned +1 reference returned to caller */
    void *mtl_command_queue;      /* id<MTLCommandQueue>, owned +1 reference returned to caller */
    XrResult result;
};

struct release_metal_session_params
{
    void *mtl_device;             /* id<MTLDevice>, callee consumes caller's +1 reference */
    void *mtl_command_queue;      /* id<MTLCommandQueue>, callee consumes caller's +1 reference */
};

struct export_metal_textures_params
{
    XrSwapchain swapchain;
    uint32_t image_count;         /* capacity in, actual count out */
    uint64_t *mtl_textures;       /* borrowed id<MTLTexture> pointers, valid while swapchain lives */
    uint32_t width;
    uint32_t height;
    uint32_t array_size;
    int64_t dxgi_format;
    XrResult result;
};

struct vulkan_requirements_params
{
    XrInstance instance;
    XrSystemId system_id;
    XrGraphicsRequirementsVulkanKHR *requirements;
    XrResult result;
};

struct vulkan_physical_device_params
{
    XrInstance instance;
    XrSystemId system_id;
    VkPhysicalDevice physical_device;
    const char *wine_version;
    const char *wine_build;
    int matches;
    XrResult result;
};

struct create_vulkan_session_params
{
    XrInstance instance;
    XrSystemId system_id;
    const XrGraphicsBindingVulkanKHR *binding;
    struct wine_XrSession *session;
    const char *wine_version;
    const char *wine_build;
    XrResult result;
};

struct import_vulkan_texture_params
{
    VkImage image;
    uint64_t metal_texture;
    VkResult result;
};

struct wine_XrInstance
{
    /* Keep host_instance first. wine_xrCreateInstance passes &host_instance to
     * the host xrCreateInstance and recovers the wrapper via an offset-0 cast */
    XrInstance host_instance;

    /* Set under primary_session_lock during teardown. A concurrent session
     * create observes the flag and returns XR_ERROR_INSTANCE_LOST rather
     * than producing an orphan */
    int destroying;

    /* Last systemId for which xrGetD3D11GraphicsRequirementsKHR succeeded, or
     * XR_NULL_SYSTEM_ID if never called. xrCreateSession requires the app to
     * call xrGet*GraphicsRequirements* for the graphics API and systemId it
     * binds. Monado would enforce this natively, but src/unix/session.m calls
     * xrGetMetalGraphicsRequirementsKHR inside wine_create_d3d11_session and
     * flips Monado's gotten_requirements, so the bridge must track it before
     * that call. Read and written under primary_session_lock */
    XrSystemId d3d11_requirements_queried_for;
    XrSystemId vulkan_requirements_queried_for;
    VkInstance vulkan_instance;
    VkPhysicalDevice vulkan_physical_device;

    XrVersion api_version;
    uint64_t enabled_extensions[(XR_BRIDGE_EXTENSION_COUNT + 63) / 64];
};

struct wine_XrSession
{
    XrSession host_session;
    struct wine_XrInstance *instance;
    enum { WINE_XR_D3D11, WINE_XR_VULKAN } graphics_api;
    struct wine_vulkan_session *vulkan;

    void *mtl_device;          /* id<MTLDevice>, owned +1 reference */
    void *mtl_command_queue;   /* id<MTLCommandQueue>, owned +1 reference */

    IMTLD3D11InteropDevice *dxmt_device;
    ID3D11Device         *d3d11_device;
    ID3D11DeviceContext  *d3d11_context;
    ID3D11DeviceContext4 *d3d11_context4;   /* cached to avoid per-release QueryInterface */

    ID3D11Fence *gpu_fence;                 /* keeps the MTLSharedEvent behind mtl_shared_event alive */
    int64_t gpu_fence_value;                /* monotonic, guarded by swapchain_lock */
    uint64_t mtl_shared_event;              /* borrowed id<MTLSharedEvent>, valid while gpu_fence lives */

    struct list swapchain_list;

#ifdef _WIN32
    /* Guards swapchain_list mutations and the first-fill of
     * wine_XrSwapchain::images against concurrent callers */
    CRITICAL_SECTION swapchain_lock;
#endif
};

#ifdef _WIN32
struct wine_XrSwapchain;
XrResult wine_vulkan_api_version(XrVersion *version);
XrResult wine_create_vulkan_session(struct wine_XrInstance *instance,
                                    const XrGraphicsBindingVulkanKHR *binding,
                                    XrSystemId system_id, struct wine_XrSession *session);
XrResult wine_enumerate_vulkan_images(struct wine_XrSwapchain *swapchain,
                                       uint32_t capacity, uint32_t *count,
                                       XrSwapchainImageBaseHeader *images);
void wine_destroy_vulkan_images(struct wine_XrSwapchain *swapchain);
void wine_destroy_vulkan_session(struct wine_XrSession *session);
#endif

struct wine_XrSwapchain
{
    XrSwapchain host_swapchain;
    struct wine_XrSession *session;
    struct list entry;

    /* Subset of XrSwapchainCreateInfo. Copying the whole struct would dangle
     * on the caller's next-chain pointer, which we do not own */
    struct
    {
        uint32_t width;
        uint32_t height;
        uint32_t arraySize;
        uint32_t mipCount;
        uint32_t sampleCount;
        uint32_t faceCount;
        int64_t format;
        XrSwapchainUsageFlags usageFlags;
    } create_info;

    XrSwapchainImageBaseHeader *images;
    uint32_t image_count;
};

static inline struct wine_XrInstance *wine_instance_from_handle(XrInstance handle)
{
    return (struct wine_XrInstance *)(uintptr_t)handle;
}

static inline struct wine_XrSession *wine_session_from_handle(XrSession handle)
{
    return (struct wine_XrSession *)(uintptr_t)handle;
}

static inline struct wine_XrSwapchain *wine_swapchain_from_handle(XrSwapchain handle)
{
    return (struct wine_XrSwapchain *)(uintptr_t)handle;
}

#ifdef _WIN32
#include <windows.h>

typedef uint64_t unixlib_handle_t;
extern unixlib_handle_t __wineopenxr_unixlib_handle;
extern __declspec(dllimport) NTSTATUS (WINAPI *__wine_unix_call_dispatcher)(unixlib_handle_t, unsigned int, void *);

NTSTATUS WINAPI init_unix_call(void);

#define UNIX_CALL(code, params) \
    __wine_unix_call_dispatcher(__wineopenxr_unixlib_handle, unix_##code, (params))
#endif

#endif /* __WINE_OPENXR_BRIDGE_H */
