/* SPDX-License-Identifier: LGPL-2.1-or-later */

#import <Metal/Metal.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "wine/debug_slim.h"

#include "openxr/openxr.h"

WINE_DEFAULT_DEBUG_CHANNEL(openxr);

#define STATUS_SUCCESS ((NTSTATUS)0)

/* Stubs for types the shared bridge.h forward-declares but this TU
 * never dereferences. They keep header chains from pulling in D3D11 or Win32
 * content that clashes with Metal.h */
typedef struct { long long QuadPart; } LARGE_INTEGER;
#define XR_USE_GRAPHICS_API_D3D11 1
#define XR_USE_GRAPHICS_API_D3D12 1
#define XR_USE_GRAPHICS_API_METAL 1
#define XR_USE_PLATFORM_WIN32 1
#define XR_USE_TIMESPEC 1
typedef int D3D_FEATURE_LEVEL;
typedef struct { unsigned int LowPart; int HighPart; } LUID;
typedef struct ID3D11Device ID3D11Device;
typedef struct ID3D11DeviceContext ID3D11DeviceContext;
typedef struct ID3D11Texture2D ID3D11Texture2D;
typedef struct ID3D11DeviceContext4 ID3D11DeviceContext4;
typedef struct ID3D11Fence ID3D11Fence;
typedef struct IMTLD3D11InteropDevice IMTLD3D11InteropDevice;
typedef struct IUnknown IUnknown;

#include "bridge.h"
#include "unixcall.h"
#include "dispatch.h"

extern struct openxr_instance_funcs g_xr_host_instance_dispatch_table;

enum { MIN_D3D_FEATURE_LEVEL = 0xb000 };

NTSTATUS wine_create_metal_session(void *args)
{
    @autoreleasepool {
        struct create_metal_session_params *params = args;
        struct wine_XrInstance *wine_instance = wine_instance_from_handle(params->instance);
        XrInstance host_instance = wine_instance->host_instance;
        struct openxr_instance_funcs *funcs = &g_xr_host_instance_dispatch_table;
        XrGraphicsRequirementsMetalKHR metal_reqs = {
            .type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR,
        };
        XrResult res = XR_SUCCESS;
        id<MTLDevice> device = nil;
        id<MTLCommandQueue> queue = nil;

        if (!funcs->p_xrGetMetalGraphicsRequirementsKHR)
        {
            WINE_ERR("XR_KHR_metal_enable not supported by runtime\n");
            res = XR_ERROR_FUNCTION_UNSUPPORTED;
            goto out;
        }

        res = funcs->p_xrGetMetalGraphicsRequirementsKHR(
            host_instance, params->system_id, &metal_reqs);
        if (res != XR_SUCCESS)
        {
            WINE_ERR("xrGetMetalGraphicsRequirementsKHR failed: %d\n", res);
            goto out;
        }

        device = (id<MTLDevice>)metal_reqs.metalDevice;
        if (!device)
        {
            WINE_ERR("metalDevice is NULL\n");
            res = XR_ERROR_RUNTIME_FAILURE;
            goto out;
        }
        [device retain];

        queue = [device newCommandQueue];
        if (!queue)
        {
            WINE_ERR("newCommandQueue failed\n");
            res = XR_ERROR_RUNTIME_FAILURE;
            goto out;
        }

        {
            XrGraphicsBindingMetalKHR metal_binding = {
                .type = XR_TYPE_GRAPHICS_BINDING_METAL_KHR,
                .next = NULL,
                .commandQueue = (void *)queue,
            };
            XrSessionCreateInfo session_info = {
                .type = XR_TYPE_SESSION_CREATE_INFO,
                .next = &metal_binding,
                .systemId = params->system_id,
            };

            res = funcs->p_xrCreateSession(
                host_instance, &session_info, params->session);
        }

        if (res != XR_SUCCESS)
        {
            WINE_ERR("xrCreateSession (Metal) failed: %d\n", res);
            goto out;
        }

        params->mtl_device = (void *)device;
        params->mtl_command_queue = (void *)queue;
        device = nil;
        queue = nil;

out:
        [queue release];
        [device release];
        params->result = res;
        return STATUS_SUCCESS;
    }
}

NTSTATUS wine_release_metal_session(void *args)
{
    @autoreleasepool {
        struct release_metal_session_params *params = args;

        [(id<MTLCommandQueue>)params->mtl_command_queue release];
        [(id<MTLDevice>)params->mtl_device release];

        return STATUS_SUCCESS;
    }
}

static XrResult d3d_adapter_luid(XrInstance instance, XrSystemId system_id, LUID *luid)
{
    struct wine_XrInstance *wine_instance = wine_instance_from_handle(instance);
    struct openxr_instance_funcs *funcs = &g_xr_host_instance_dispatch_table;
    XrGraphicsRequirementsMetalKHR metal_reqs = {
        .type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR,
    };
    uint64_t swapped;
    XrResult result;

    if (!funcs->p_xrGetMetalGraphicsRequirementsKHR)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    result = funcs->p_xrGetMetalGraphicsRequirementsKHR(
        wine_instance->host_instance, system_id, &metal_reqs);
    if (result != XR_SUCCESS)
        return result;
    if (!metal_reqs.metalDevice)
    {
        WINE_ERR("metalDevice is NULL from requirements\n");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    /* DXMT's LUID encodes the byte-swapped Metal registry ID. */
    swapped = __builtin_bswap64([(id<MTLDevice>)metal_reqs.metalDevice registryID]);
    memcpy(luid, &swapped, sizeof(swapped));
    return XR_SUCCESS;
}

NTSTATUS wine_xrGetD3D11GraphicsRequirementsKHR(void *args)
{
    @autoreleasepool {
        struct xrGetD3D11GraphicsRequirementsKHR_params *params = args;
        params->result = d3d_adapter_luid(params->instance, params->systemId,
                                        &params->graphicsRequirements->adapterLuid);
        if (params->result == XR_SUCCESS)
        {
            params->graphicsRequirements->type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
            params->graphicsRequirements->minFeatureLevel = MIN_D3D_FEATURE_LEVEL;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS wine_xrGetD3D12GraphicsRequirementsKHR(void *args)
{
    @autoreleasepool {
        struct xrGetD3D12GraphicsRequirementsKHR_params *params = args;
        params->result = d3d_adapter_luid(params->instance, params->systemId,
                                        &params->graphicsRequirements->adapterLuid);
        if (params->result == XR_SUCCESS)
        {
            params->graphicsRequirements->type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR;
            params->graphicsRequirements->minFeatureLevel = MIN_D3D_FEATURE_LEVEL;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS wine_export_metal_textures(void *args)
{
    @autoreleasepool {
        struct export_metal_textures_params *params = args;
        struct wine_XrSwapchain *wine_swapchain = wine_swapchain_from_handle(params->swapchain);
        struct openxr_instance_funcs *funcs = &g_xr_host_instance_dispatch_table;
        XrSwapchain host_swapchain = wine_swapchain->host_swapchain;
        XrSwapchainImageMetalKHR *metal_images = NULL;
        uint32_t count = 0, i;
        XrResult res;

        if (!funcs->p_xrEnumerateSwapchainImages)
        {
            params->result = XR_ERROR_FUNCTION_UNSUPPORTED;
            return STATUS_SUCCESS;
        }

        res = funcs->p_xrEnumerateSwapchainImages(host_swapchain, 0, &count, NULL);
        if (res != XR_SUCCESS)
        {
            params->result = res;
            return STATUS_SUCCESS;
        }

        if (count > params->image_count)
        {
            params->image_count = count;
            params->result = XR_ERROR_SIZE_INSUFFICIENT;
            return STATUS_SUCCESS;
        }

        metal_images = calloc(count, sizeof(*metal_images));
        if (!metal_images)
        {
            params->result = XR_ERROR_OUT_OF_MEMORY;
            return STATUS_SUCCESS;
        }

        for (i = 0; i < count; i++)
        {
            metal_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
            metal_images[i].next = NULL;
        }

        res = funcs->p_xrEnumerateSwapchainImages(
            host_swapchain, count, &count, (XrSwapchainImageBaseHeader *)metal_images);
        if (res != XR_SUCCESS)
        {
            free(metal_images);
            params->result = res;
            return STATUS_SUCCESS;
        }

        for (i = 0; i < count; i++)
            params->mtl_textures[i] = (uint64_t)(uintptr_t)metal_images[i].texture;

        free(metal_images);

        params->image_count = count;
        params->result = XR_SUCCESS;
        return STATUS_SUCCESS;
    }
}
