/* SPDX-License-Identifier: LGPL-2.1-or-later */

#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wine/debug.h>
#include "bridge.h"

WINE_DEFAULT_DEBUG_CHANNEL(openxr);

struct wine_vulkan_session
{
    VkDevice device;
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
};

static PFN_vkGetInstanceProcAddr vulkan_loader(void)
{
    HMODULE module = GetModuleHandleA("winevulkan.dll");
    HMODULE application_loader;
    PFN_vkGetInstanceProcAddr get_proc;
    if (!module)
        module = LoadLibraryA("winevulkan.dll");
    if (!module)
    {
        WINE_ERR("Loading winevulkan.dll failed: Win32 error %lu\n", GetLastError());
        return NULL;
    }
    get_proc = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(module, "vkGetInstanceProcAddr");
    if (!get_proc)
    {
        WINE_ERR("winevulkan.dll lacks vkGetInstanceProcAddr: Win32 error %lu\n", GetLastError());
        return NULL;
    }
    application_loader = GetModuleHandleA("vulkan-1.dll");
    if (application_loader &&
        (void *)GetProcAddress(application_loader, "vkGetInstanceProcAddr") != (void *)get_proc)
    {
        WINE_ERR("Native vulkan-1.dll is incompatible with Wine Vulkan handle interop; "
                 "configure vulkan-1=b and restart the application\n");
        return NULL;
    }
    return get_proc;
}

static int wine_identity(const char **version, const char **build)
{
    const char * (__cdecl *get_version)(void);
    const char * (__cdecl *get_build)(void);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    get_version = (void *)GetProcAddress(ntdll, "wine_get_version");
    get_build = (void *)GetProcAddress(ntdll, "wine_get_build_id");
    if (!get_version || !get_build)
    {
        WINE_ERR("winevulkan handle ABI requires Wine version/build identification\n");
        return 0;
    }
    *version = get_version();
    *build = get_build();
    return 1;
}

XrResult wine_vulkan_api_version(XrVersion *version)
{
    PFN_vkGetInstanceProcAddr get_proc = vulkan_loader();
    PFN_vkEnumerateInstanceVersion enumerate;
    uint32_t api = VK_API_VERSION_1_0;
    VkResult result;
    if (!get_proc)
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    enumerate = (PFN_vkEnumerateInstanceVersion)get_proc(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    if (enumerate)
    {
        result = enumerate(&api);
        if (result != VK_SUCCESS)
        {
            WINE_ERR("vkEnumerateInstanceVersion failed: VkResult %d\n", result);
            return result == VK_ERROR_OUT_OF_HOST_MEMORY ? XR_ERROR_OUT_OF_MEMORY : XR_ERROR_RUNTIME_FAILURE;
        }
    }
    if (api > VK_API_VERSION_1_3)
        api = VK_API_VERSION_1_3;
    *version = XR_MAKE_VERSION(VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));
    return XR_SUCCESS;
}

static XrResult validate_system(XrInstance instance, XrSystemId system_id)
{
    XrSystemProperties properties = {.type = XR_TYPE_SYSTEM_PROPERTIES};
    struct xrGetSystemProperties_params params = {
        .instance = instance, .systemId = system_id, .properties = &properties,
    };
    NTSTATUS status = UNIX_CALL(xrGetSystemProperties, &params);
    if (status)
    {
        WINE_ERR("Vulkan system validation unix call failed: 0x%x\n", (unsigned)status);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    return params.result;
}

static XrResult extension_string(XrInstance instance, XrSystemId system_id,
                                  const char *value, uint32_t capacity,
                                  uint32_t *count, char *buffer)
{
    XrResult result = validate_system(instance, system_id);
    uint32_t size = (uint32_t)strlen(value) + 1;
    if (result != XR_SUCCESS)
        return result;
    if (!count || (capacity && !buffer))
        return XR_ERROR_VALIDATION_FAILURE;
    *count = size;
    if (!capacity)
        return XR_SUCCESS;
    if (capacity < size)
        return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(buffer, value, size);
    return XR_SUCCESS;
}

XrResult WINAPI xrGetVulkanInstanceExtensionsKHR(XrInstance instance, XrSystemId systemId,
                                                  uint32_t bufferCapacityInput,
                                                  uint32_t *bufferCountOutput, char *buffer)
{
    return extension_string(instance, systemId, "", bufferCapacityInput,
                              bufferCountOutput, buffer);
}

XrResult WINAPI xrGetVulkanDeviceExtensionsKHR(XrInstance instance, XrSystemId systemId,
                                                uint32_t bufferCapacityInput,
                                                uint32_t *bufferCountOutput, char *buffer)
{
    return extension_string(instance, systemId, "", bufferCapacityInput,
                              bufferCountOutput, buffer);
}

XrResult WINAPI xrGetVulkanGraphicsDeviceKHR(XrInstance instance, XrSystemId systemId,
                                              VkInstance vkInstance,
                                              VkPhysicalDevice *physicalDevice)
{
    PFN_vkGetInstanceProcAddr get_proc = vulkan_loader();
    PFN_vkEnumeratePhysicalDevices enumerate;
    VkPhysicalDevice *devices = NULL;
    uint32_t count = 0, i;
    VkResult vk_result;
    XrResult result = validate_system(instance, systemId);
    if (result != XR_SUCCESS)
        return result;
    if (!vkInstance || !physicalDevice)
        return XR_ERROR_VALIDATION_FAILURE;
    if (!get_proc || !(enumerate = (PFN_vkEnumeratePhysicalDevices)get_proc(vkInstance, "vkEnumeratePhysicalDevices")))
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    vk_result = enumerate(vkInstance, &count, NULL);
    if (vk_result != VK_SUCCESS)
        goto vk_failure;
    devices = calloc(count, sizeof(*devices));
    if (!devices)
        return XR_ERROR_OUT_OF_MEMORY;
    vk_result = enumerate(vkInstance, &count, devices);
    if (vk_result != VK_SUCCESS)
        goto vk_failure;
    result = XR_ERROR_GRAPHICS_DEVICE_INVALID;
    for (i = 0; i < count; i++)
    {
        struct vulkan_physical_device_params params = {
            .instance = instance, .system_id = systemId, .physical_device = devices[i],
        };
        if (!wine_identity(&params.wine_version, &params.wine_build))
        {
            result = XR_ERROR_GRAPHICS_DEVICE_INVALID;
            break;
        }
        NTSTATUS status = UNIX_CALL(vulkan_physical_device, &params);
        if (status || params.result != XR_SUCCESS)
        {
            if (status)
                WINE_ERR("Vulkan physical device unix call failed: 0x%x\n", (unsigned)status);
            result = status ? XR_ERROR_RUNTIME_FAILURE : params.result;
            break;
        }
        if (params.matches)
        {
            *physicalDevice = devices[i];
            wine_instance_from_handle(instance)->vulkan_instance = vkInstance;
            wine_instance_from_handle(instance)->vulkan_physical_device = devices[i];
            result = XR_SUCCESS;
            break;
        }
    }
    free(devices);
    return result;
vk_failure:
    WINE_ERR("vkEnumeratePhysicalDevices failed: VkResult %d\n", vk_result);
    free(devices);
    return vk_result == VK_ERROR_OUT_OF_HOST_MEMORY ? XR_ERROR_OUT_OF_MEMORY
                                                   : XR_ERROR_GRAPHICS_DEVICE_INVALID;
}

XrResult WINAPI xrGetVulkanGraphicsDevice2KHR(XrInstance instance,
                                               const XrVulkanGraphicsDeviceGetInfoKHR *getInfo,
                                               VkPhysicalDevice *physicalDevice)
{
    if (!getInfo || getInfo->type != XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR)
        return XR_ERROR_VALIDATION_FAILURE;
    return xrGetVulkanGraphicsDeviceKHR(instance, getInfo->systemId,
                                         getInfo->vulkanInstance, physicalDevice);
}

XrResult WINAPI xrCreateVulkanInstanceKHR(XrInstance instance,
                                           const XrVulkanInstanceCreateInfoKHR *createInfo,
                                           VkInstance *vulkanInstance, VkResult *vulkanResult)
{
    PFN_vkCreateInstance create;
    XrResult result;
    if (!createInfo || createInfo->type != XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR ||
        !createInfo->pfnGetInstanceProcAddr || !createInfo->vulkanCreateInfo ||
        !vulkanInstance || !vulkanResult || createInfo->createFlags)
        return XR_ERROR_VALIDATION_FAILURE;
    result = validate_system(instance, createInfo->systemId);
    if (result != XR_SUCCESS)
        return result;
    create = (PFN_vkCreateInstance)createInfo->pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create)
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    *vulkanResult = create(createInfo->vulkanCreateInfo, createInfo->vulkanAllocator, vulkanInstance);
    if (*vulkanResult == VK_SUCCESS)
    {
        wine_instance_from_handle(instance)->vulkan_instance = *vulkanInstance;
        wine_instance_from_handle(instance)->vulkan_physical_device = VK_NULL_HANDLE;
    }
    if (*vulkanResult != VK_SUCCESS)
        WINE_WARN("xrCreateVulkanInstanceKHR: vkCreateInstance returned %d\n", *vulkanResult);
    return XR_SUCCESS;
}

XrResult WINAPI xrCreateVulkanDeviceKHR(XrInstance instance,
                                         const XrVulkanDeviceCreateInfoKHR *createInfo,
                                         VkDevice *vulkanDevice, VkResult *vulkanResult)
{
    PFN_vkCreateDevice create;
    XrResult result;
    if (!createInfo || createInfo->type != XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR ||
        !createInfo->pfnGetInstanceProcAddr || !createInfo->vulkanCreateInfo ||
        !createInfo->vulkanPhysicalDevice || !vulkanDevice || !vulkanResult || createInfo->createFlags)
        return XR_ERROR_VALIDATION_FAILURE;
    result = validate_system(instance, createInfo->systemId);
    if (result != XR_SUCCESS)
        return result;
    if (wine_instance_from_handle(instance)->vulkan_physical_device != createInfo->vulkanPhysicalDevice)
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    create = (PFN_vkCreateDevice)createInfo->pfnGetInstanceProcAddr(
        wine_instance_from_handle(instance)->vulkan_instance, "vkCreateDevice");
    if (!create)
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    *vulkanResult = create(createInfo->vulkanPhysicalDevice, createInfo->vulkanCreateInfo,
                           createInfo->vulkanAllocator, vulkanDevice);
    if (*vulkanResult != VK_SUCCESS)
        WINE_WARN("xrCreateVulkanDeviceKHR: vkCreateDevice returned %d\n", *vulkanResult);
    return XR_SUCCESS;
}

XrResult wine_create_vulkan_session(struct wine_XrInstance *instance,
                                    const XrGraphicsBindingVulkanKHR *binding,
                                    XrSystemId system_id, struct wine_XrSession *session)
{
    PFN_vkGetInstanceProcAddr get_proc = vulkan_loader();
    struct wine_vulkan_session *vulkan;
    struct create_vulkan_session_params params = {
        .instance = (XrInstance)instance, .system_id = system_id,
        .binding = binding, .session = session,
    };
    NTSTATUS status;
    if (!binding->instance || !binding->physicalDevice || !binding->device || !get_proc)
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    if (!wine_identity(&params.wine_version, &params.wine_build))
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    vulkan = calloc(1, sizeof(*vulkan));
    if (!vulkan)
        return XR_ERROR_OUT_OF_MEMORY;
    vulkan->device = binding->device;
    vulkan->create_image = (PFN_vkCreateImage)(void *)GetProcAddress(GetModuleHandleA("winevulkan.dll"), "vkCreateImage");
    vulkan->destroy_image = (PFN_vkDestroyImage)(void *)GetProcAddress(GetModuleHandleA("winevulkan.dll"), "vkDestroyImage");
    if (!vulkan->create_image || !vulkan->destroy_image)
    {
        WINE_ERR("winevulkan.dll lacks core image entrypoints: Win32 error %lu\n", GetLastError());
        free(vulkan);
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    session->vulkan = vulkan;
    status = UNIX_CALL(create_vulkan_session, &params);
    if (status)
    {
        WINE_ERR("Vulkan session unix call failed: 0x%x\n", (unsigned)status);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    if (params.result != XR_SUCCESS)
        return params.result;
    return XR_SUCCESS;
}

void wine_destroy_vulkan_session(struct wine_XrSession *session)
{
    free(session->vulkan);
    session->vulkan = NULL;
}

void wine_destroy_vulkan_images(struct wine_XrSwapchain *swapchain)
{
    XrSwapchainImageVulkanKHR *images = (XrSwapchainImageVulkanKHR *)swapchain->images;
    uint32_t i;
    if (!images)
        return;
    for (i = 0; i < swapchain->image_count; i++)
        swapchain->session->vulkan->destroy_image(swapchain->session->vulkan->device, images[i].image, NULL);
}

static VkImageUsageFlags image_usage(XrSwapchainUsageFlags usage)
{
    VkImageUsageFlags flags = 0;
    if (usage & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return flags;
}

XrResult wine_enumerate_vulkan_images(struct wine_XrSwapchain *swapchain,
                                       uint32_t capacity, uint32_t *count,
                                       XrSwapchainImageBaseHeader *output)
{
    struct wine_XrSession *session = swapchain->session;
    struct wine_vulkan_session *vulkan = session->vulkan;
    XrResult result = XR_SUCCESS;
    uint32_t i;
    if (!count || (capacity && !output))
        return XR_ERROR_VALIDATION_FAILURE;
    if (!capacity)
    {
        struct xrEnumerateSwapchainImages_params params = {
            .swapchain = (XrSwapchain)swapchain, .imageCountOutput = count,
        };
        NTSTATUS status = UNIX_CALL(xrEnumerateSwapchainImages, &params);
        if (status)
            WINE_ERR("Vulkan image count unix call failed: NTSTATUS 0x%x\n", (unsigned)status);
        return status ? XR_ERROR_RUNTIME_FAILURE : params.result;
    }
    EnterCriticalSection(&session->swapchain_lock);
    *count = swapchain->image_count;
    if (!swapchain->images)
    {
        struct xrEnumerateSwapchainImages_params query = {
            .swapchain = (XrSwapchain)swapchain, .imageCountOutput = count,
        };
        NTSTATUS status = UNIX_CALL(xrEnumerateSwapchainImages, &query);
        if (status || query.result != XR_SUCCESS)
        {
            if (status)
                WINE_ERR("Vulkan image count unix call failed: NTSTATUS 0x%x\n", (unsigned)status);
            result = status ? XR_ERROR_RUNTIME_FAILURE : query.result;
            goto done;
        }
    }
    if (capacity < *count)
    {
        result = XR_ERROR_SIZE_INSUFFICIENT;
        goto done;
    }
    for (i = 0; i < *count; i++)
        if (((XrSwapchainImageVulkanKHR *)output)[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR)
        {
            result = XR_ERROR_VALIDATION_FAILURE;
            goto done;
        }
    if (!swapchain->images)
    {
        struct export_metal_textures_params params = {
            .swapchain = (XrSwapchain)swapchain, .image_count = *count,
        };
        XrSwapchainImageVulkanKHR *images;
        NTSTATUS status = 0;
        params.mtl_textures = calloc(*count, sizeof(*params.mtl_textures));
        if (!params.mtl_textures) { result = XR_ERROR_OUT_OF_MEMORY; goto done; }
        status = UNIX_CALL(export_metal_textures, &params);
        if (status || params.result != XR_SUCCESS)
        {
            if (status)
                WINE_ERR("Vulkan Metal texture export unix call failed: NTSTATUS 0x%x\n", (unsigned)status);
            *count = params.image_count;
            result = status ? XR_ERROR_RUNTIME_FAILURE : params.result;
            free(params.mtl_textures);
            goto done;
        }
        images = calloc(params.image_count, sizeof(*images));
        if (!images)
        {
            free(params.mtl_textures);
            result = XR_ERROR_OUT_OF_MEMORY;
            goto done;
        }
        for (i = 0; i < params.image_count; i++)
        {
            VkImageCreateInfo info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .flags = (swapchain->create_info.faceCount == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
                         (swapchain->create_info.usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0),
                .imageType = VK_IMAGE_TYPE_2D,
                .format = (VkFormat)swapchain->create_info.format,
                .extent = {params.width, params.height, 1},
                .mipLevels = swapchain->create_info.mipCount,
                .arrayLayers = params.array_size * swapchain->create_info.faceCount,
                .samples = (VkSampleCountFlagBits)swapchain->create_info.sampleCount,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = image_usage(swapchain->create_info.usageFlags),
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
            struct import_vulkan_texture_params import = {.metal_texture = params.mtl_textures[i]};
            VkResult vk_result = vulkan->create_image(vulkan->device, &info, NULL, &images[i].image);
            if (vk_result == VK_SUCCESS)
            {
                import.image = images[i].image;
                status = UNIX_CALL(import_vulkan_texture, &import);
                vk_result = status ? VK_ERROR_INITIALIZATION_FAILED : import.result;
            }
            if (vk_result != VK_SUCCESS)
            {
                uint32_t created;
                WINE_ERR("Vulkan swapchain image %u import failed: VkResult %d NTSTATUS 0x%x\n", i, vk_result, (unsigned)status);
                for (created = 0; created <= i; created++)
                    if (images[created].image)
                        vulkan->destroy_image(vulkan->device, images[created].image, NULL);
                free(images);
                free(params.mtl_textures);
                result = vk_result == VK_ERROR_OUT_OF_HOST_MEMORY || vk_result == VK_ERROR_OUT_OF_DEVICE_MEMORY
                    ? XR_ERROR_OUT_OF_MEMORY : vk_result == VK_ERROR_DEVICE_LOST
                    ? XR_ERROR_SESSION_LOST : XR_ERROR_RUNTIME_FAILURE;
                goto done;
            }
            images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
        }
        free(params.mtl_textures);
        swapchain->images = (XrSwapchainImageBaseHeader *)images;
        swapchain->image_count = params.image_count;
    }
    *count = swapchain->image_count;
    {
        XrSwapchainImageVulkanKHR *images = (XrSwapchainImageVulkanKHR *)swapchain->images;
        for (i = 0; i < *count; i++)
            ((XrSwapchainImageVulkanKHR *)output)[i].image = images[i].image;
    }
done:
    LeaveCriticalSection(&session->swapchain_lock);
    return result;
}
