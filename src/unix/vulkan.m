/* SPDX-License-Identifier: LGPL-2.1-or-later */

#import <Metal/Metal.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include "wine/debug_slim.h"

typedef struct { long long QuadPart; } LARGE_INTEGER;
typedef struct { unsigned int LowPart; int HighPart; } LUID;
typedef int D3D_FEATURE_LEVEL;
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_METAL
#define XR_USE_PLATFORM_WIN32
#define XR_USE_TIMESPEC
#include "bridge.h"
#include "dispatch.h"
#include "wine_vulkan.h"

WINE_DEFAULT_DEBUG_CHANNEL(openxr);
#define STATUS_SUCCESS ((NTSTATUS)0)

extern struct openxr_instance_funcs g_xr_host_instance_dispatch_table;

struct mvk_functions
{
    void (*get_device)(VkPhysicalDevice, void **);
    void (*get_queue)(VkQueue, void **);
    VkResult (*set_texture)(VkImage, void *);
    VkResult (*configuration)(VkInstance, void *, size_t *);
    PFN_vkGetDeviceQueue get_device_queue;
    PFN_vkGetPhysicalDeviceProperties get_properties;
};

static struct mvk_functions mvk;
static pthread_once_t mvk_once = PTHREAD_ONCE_INIT;

static void load_mvk(void)
{
    uint32_t i;
    void *module = NULL;
    const char *selected = NULL;
    for (i = 0; i < _dyld_image_count(); i++)
    {
        const char *path = _dyld_get_image_name(i);
        const char *name = strrchr(path, '/');
        if (!strcmp(name ? name + 1 : path, "libMoltenVK.dylib"))
        {
            if (selected)
            {
                WINE_ERR("Ambiguous MoltenVK libraries for Wine handles: %s and %s\n", selected, path);
                return;
            }
            selected = path;
        }
    }
    if (selected)
        module = dlopen(selected, RTLD_LAZY | RTLD_NOLOAD);
    if (!module)
    {
        WINE_ERR("Cannot open winevulkan's loaded MoltenVK library: %s\n", selected ? dlerror() : "no library loaded");
        return;
    }
    mvk.get_device = dlsym(module, "vkGetMTLDeviceMVK");
    mvk.get_queue = dlsym(module, "vkGetMTLCommandQueueMVK");
    mvk.set_texture = dlsym(module, "vkSetMTLTextureMVK");
    mvk.configuration = dlsym(module, "vkGetMoltenVKConfigurationMVK");
    mvk.get_device_queue = dlsym(module, "vkGetDeviceQueue");
    mvk.get_properties = dlsym(module, "vkGetPhysicalDeviceProperties");
}

static XrResult metal_requirements(XrInstance instance, XrSystemId system_id,
                                   XrGraphicsRequirementsMetalKHR *requirements)
{
    struct openxr_instance_funcs *funcs = &g_xr_host_instance_dispatch_table;
    if (!funcs->p_xrGetMetalGraphicsRequirementsKHR)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    return funcs->p_xrGetMetalGraphicsRequirementsKHR(
        wine_instance_from_handle(instance)->host_instance, system_id, requirements);
}

NTSTATUS wine_vulkan_requirements(void *args)
{
    struct vulkan_requirements_params *params = args;
    XrGraphicsRequirementsMetalKHR requirements = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
    params->result = metal_requirements(params->instance, params->system_id, &requirements);
    if (params->result == XR_SUCCESS)
    {
        params->requirements->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    }
    return STATUS_SUCCESS;
}

NTSTATUS wine_vulkan_physical_device(void *args)
{
    @autoreleasepool {
        struct vulkan_physical_device_params *params = args;
        XrGraphicsRequirementsMetalKHR requirements = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
        void *device = NULL;
        params->matches = 0;
        params->result = metal_requirements(params->instance, params->system_id, &requirements);
        if (params->result != XR_SUCCESS)
            return STATUS_SUCCESS;
        if (!wine_vk_abi_supported(params->wine_version, params->wine_build))
        {
            WINE_ERR("Unsupported winevulkan handle ABI: Wine %s build %s\n", params->wine_version, params->wine_build);
            params->result = XR_ERROR_GRAPHICS_DEVICE_INVALID;
            return STATUS_SUCCESS;
        }
        pthread_once(&mvk_once, load_mvk);
        if (!mvk.get_device)
        {
            params->result = XR_ERROR_GRAPHICS_DEVICE_INVALID;
            return STATUS_SUCCESS;
        }
        VkPhysicalDevice native = (VkPhysicalDevice)(uintptr_t)wine_vk_native_dispatchable((uint64_t)(uintptr_t)params->physical_device);
        if (!native)
        {
            WINE_ERR("Invalid winevulkan physical device handle\n");
            params->result = XR_ERROR_GRAPHICS_DEVICE_INVALID;
            return STATUS_SUCCESS;
        }
        mvk.get_device(native, &device);
        params->matches = device && [(id<MTLDevice>)device registryID] == [(id<MTLDevice>)requirements.metalDevice registryID];
        return STATUS_SUCCESS;
    }
}

NTSTATUS wine_create_vulkan_session(void *args)
{
    @autoreleasepool {
        struct create_vulkan_session_params *params = args;
        const XrGraphicsBindingVulkanKHR *binding = params->binding;
        struct wine_XrSession *session = params->session;
        XrGraphicsRequirementsMetalKHR requirements = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
        VkDevice native_device;
        VkInstance native_instance;
        VkPhysicalDevice native_physical;
        VkPhysicalDeviceProperties properties;
        VkQueue native_queue = VK_NULL_HANDLE;
        void *metal_device = NULL, *metal_queue = NULL;
        struct { VkBool32 debug_mode, flip_vertex_y, synchronous_queue_submits; } config = {0};
        size_t config_size = sizeof(config);
        VkResult vk_result;

        params->result = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        if (!wine_vk_abi_supported(params->wine_version, params->wine_build))
        {
            WINE_ERR("Unsupported winevulkan handle ABI: Wine %s build %s\n", params->wine_version, params->wine_build);
            return STATUS_SUCCESS;
        }
        if (!wine_vk_binding_valid((uint64_t)(uintptr_t)binding->instance,
                                   (uint64_t)(uintptr_t)binding->physicalDevice,
                                   (uint64_t)(uintptr_t)binding->device,
                                   binding->queueFamilyIndex, binding->queueIndex))
        {
            WINE_ERR("Invalid winevulkan binding parents or queue: family %u index %u\n",
                     binding->queueFamilyIndex, binding->queueIndex);
            return STATUS_SUCCESS;
        }
        pthread_once(&mvk_once, load_mvk);
        if (!mvk.get_device || !mvk.get_queue || !mvk.get_device_queue ||
            !mvk.set_texture || !mvk.configuration || !mvk.get_properties)
        {
            WINE_ERR("MoltenVK lacks required device, queue, texture or configuration entrypoints\n");
            return STATUS_SUCCESS;
        }
        native_instance = (VkInstance)(uintptr_t)wine_vk_native_dispatchable((uint64_t)(uintptr_t)binding->instance);
        vk_result = mvk.configuration(native_instance, &config, &config_size);
        if ((vk_result != VK_SUCCESS && vk_result != VK_INCOMPLETE) ||
            config_size < sizeof(config) || !config.synchronous_queue_submits)
        {
            WINE_ERR("MoltenVK synchronousQueueSubmits is required for shared Metal queue ordering: VkResult %d, configuration size %zu, enabled %u\n",
                     vk_result, config_size, config.synchronous_queue_submits);
            return STATUS_SUCCESS;
        }
        params->result = metal_requirements(params->instance, params->system_id, &requirements);
        if (params->result != XR_SUCCESS)
            return STATUS_SUCCESS;
        native_device = (VkDevice)(uintptr_t)wine_vk_native_dispatchable((uint64_t)(uintptr_t)binding->device);
        native_physical = (VkPhysicalDevice)(uintptr_t)wine_vk_native_dispatchable((uint64_t)(uintptr_t)binding->physicalDevice);
        mvk.get_properties(native_physical, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_0)
        {
            params->result = XR_ERROR_GRAPHICS_DEVICE_INVALID;
            return STATUS_SUCCESS;
        }
        mvk.get_device(native_physical, &metal_device);
        mvk.get_device_queue(native_device, binding->queueFamilyIndex, binding->queueIndex, &native_queue);
        if (native_queue)
            mvk.get_queue(native_queue, &metal_queue);
        if (!metal_device || !metal_queue ||
            [(id<MTLDevice>)metal_device registryID] != [(id<MTLDevice>)requirements.metalDevice registryID])
        {
            WINE_ERR("Vulkan binding does not select Headgasket's Metal device/queue\n");
            params->result = XR_ERROR_GRAPHICS_DEVICE_INVALID;
            return STATUS_SUCCESS;
        }
        {
            XrGraphicsBindingMetalKHR metal_binding = {
                .type = XR_TYPE_GRAPHICS_BINDING_METAL_KHR, .commandQueue = metal_queue,
            };
            XrSessionCreateInfo info = {
                .type = XR_TYPE_SESSION_CREATE_INFO, .next = &metal_binding,
                .systemId = params->system_id,
            };
            params->result = g_xr_host_instance_dispatch_table.p_xrCreateSession(
                wine_instance_from_handle(params->instance)->host_instance, &info, &session->host_session);
        }
        if (params->result == XR_SUCCESS)
        {
            session->mtl_device = [(id<MTLDevice>)metal_device retain];
            session->mtl_command_queue = [(id<MTLCommandQueue>)metal_queue retain];
        }
        else
            WINE_ERR("Vulkan Metal session creation failed: XrResult %d\n", params->result);
        return STATUS_SUCCESS;
    }
}

NTSTATUS wine_import_vulkan_texture(void *args)
{
    struct import_vulkan_texture_params *params = args;
    pthread_once(&mvk_once, load_mvk);
    params->result = mvk.set_texture ? mvk.set_texture(params->image, (void *)(uintptr_t)params->metal_texture)
                                    : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (params->result != VK_SUCCESS)
        WINE_ERR("vkSetMTLTextureMVK failed: VkResult %d\n", params->result);
    return STATUS_SUCCESS;
}
