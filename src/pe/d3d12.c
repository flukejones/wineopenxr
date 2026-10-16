/* SPDX-License-Identifier: LGPL-2.1-or-later */

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_PLATFORM_WIN32
#define WIDL_C_INLINE_WRAPPERS

#include <stdint.h>

#include <windows.h>
#include <initguid.h>
#include <d3d12.h>

#include <wine/debug.h>

#include "bridge.h"
#include "d3d12_bridge.h"
#include "d3dmetal.h"
#include "formats.h"

WINE_DEFAULT_DEBUG_CHANNEL(openxr);

static XrResult validate_queue(const XrGraphicsBindingD3D12KHR *binding)
{
    ID3D12Device *queue_device = NULL;
    D3D12_COMMAND_QUEUE_DESC desc;
    HRESULT hr;
    XrResult result = XR_ERROR_GRAPHICS_DEVICE_INVALID;

    if (!binding->device || !binding->queue)
    {
        WINE_ERR("D3D12 binding has NULL device or queue\n");
        return result;
    }
    desc = ID3D12CommandQueue_GetDesc(binding->queue);
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        WINE_ERR("D3D12 binding queue type %u is not DIRECT\n", (unsigned)desc.Type);
        return result;
    }
    hr = ID3D12CommandQueue_GetDevice(binding->queue, &IID_ID3D12Device, (void **)&queue_device);
    if (FAILED(hr))
    {
        WINE_ERR("D3D12 queue device query failed: 0x%08x\n", (unsigned)hr);
        goto out;
    }
    if (binding->device != queue_device)
    {
        WINE_ERR("D3D12 binding queue belongs to another device\n");
        goto out;
    }
    result = XR_SUCCESS;
out:
    if (queue_device) ID3D12Device_Release(queue_device);
    return result;
}

XrResult wine_create_d3d12_session(struct wine_XrSession *session,
                                  const XrGraphicsBindingD3D12KHR *binding,
                                  XrSystemId system_id)
{
    struct create_metal_session_params native = {
        .instance = (XrInstance)session->instance,
        .system_id = system_id,
        .session = &session->host_session,
    };
    struct d3dmetal_params capture = {.op = D3DMETAL_BEGIN_FENCE};
    XrResult result = validate_queue(binding);
    NTSTATUS status;
    HRESULT hr;

    if (result != XR_SUCCESS)
        return result;
    session->d3d12_device = binding->device;
    ID3D12Device_AddRef(session->d3d12_device);
    session->d3d12_queue = binding->queue;
    ID3D12CommandQueue_AddRef(session->d3d12_queue);

    status = UNIX_CALL(create_metal_session, &native);
    if (status)
    {
        WINE_ERR("create Metal session failed: status=0x%x\n", (unsigned)status);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    if (native.result != XR_SUCCESS)
        return native.result;
    session->mtl_device = native.mtl_device;
    session->mtl_command_queue = native.mtl_command_queue;

    capture.device = (uint64_t)(uintptr_t)session->mtl_device;
    status = UNIX_CALL(d3dmetal, &capture);
    if (status || capture.result)
    {
        WINE_ERR("D3D12 fence capture begin failed: status=0x%x result=%d\n",
                 (unsigned)status, capture.result);
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    hr = ID3D12Device_CreateFence(session->d3d12_device, 0, D3D12_FENCE_FLAG_NONE,
                                 &IID_ID3D12Fence, (void **)&session->d3d12_fence);
    capture.op = D3DMETAL_END;
    status = UNIX_CALL(d3dmetal, &capture);
    session->mtl_shared_event = capture.event;
    if (FAILED(hr))
    {
        WINE_ERR("D3D12 CreateFence failed: hr=0x%08x capture status=0x%x result=%d\n",
                 (unsigned)hr, (unsigned)status, capture.result);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    if (status || capture.result || !capture.event || !session->d3d12_fence)
    {
        WINE_ERR("D3D12 fence capture failed: status=0x%x result=%d seen=%u event=%#llx\n",
                 (unsigned)status, capture.result, capture.seen,
                 (unsigned long long)capture.event);
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    return XR_SUCCESS;
}

XrResult wine_create_d3d12_image(struct wine_XrSwapchain *swapchain,
                                uint64_t metal_texture, ID3D12Resource **resource)
{
    struct wine_XrSession *session = swapchain->session;
    XrSwapchainUsageFlags usage = swapchain->create_info.usageFlags;
    int depth = is_depth_dxgi_format(swapchain->create_info.format);
    D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = swapchain->create_info.width,
        .Height = swapchain->create_info.height,
        .Format = (DXGI_FORMAT)dxgi_typeless_parent(swapchain->create_info.format),
        .SampleDesc = {.Count = 1},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
    };
    D3D12_RESOURCE_STATES state = depth
        ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    struct d3dmetal_params capture = {
        .op = D3DMETAL_BEGIN_TEXTURE, .texture = metal_texture,
    };
    NTSTATUS status;
    HRESULT hr;

    *resource = NULL;
    desc.DepthOrArraySize = (UINT16)swapchain->create_info.arraySize;
    desc.MipLevels = (UINT16)swapchain->create_info.mipCount;
    if (usage & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (usage & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (usage & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (depth && !(usage & XR_SWAPCHAIN_USAGE_SAMPLED_BIT))
        desc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    status = UNIX_CALL(d3dmetal, &capture);
    if (status || capture.result)
    {
        WINE_ERR("D3D12 image capture begin failed: status=0x%x result=%d\n",
                 (unsigned)status, capture.result);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    hr = ID3D12Device_CreateCommittedResource(session->d3d12_device, &heap,
        D3D12_HEAP_FLAG_NONE, &desc, state, NULL, &IID_ID3D12Resource, (void **)resource);
    capture.op = D3DMETAL_END;
    status = UNIX_CALL(d3dmetal, &capture);
    if (FAILED(hr) || status || capture.result || !*resource)
    {
        WINE_ERR("D3D12 image creation failed: hr=0x%08x capture status=0x%x result=%d seen=%u\n",
                 (unsigned)hr, (unsigned)status, capture.result, capture.seen);
        if (*resource) ID3D12Resource_Release(*resource);
        *resource = NULL;
        return XR_ERROR_RUNTIME_FAILURE;
    }
    return XR_SUCCESS;
}

void wine_destroy_d3d12_session(struct wine_XrSession *session)
{
    if (session->d3d12_fence)
    {
        ID3D12Fence_Release(session->d3d12_fence);
        session->d3d12_fence = NULL;
    }
    if (session->d3d12_queue)
    {
        ID3D12CommandQueue_Release(session->d3d12_queue);
        session->d3d12_queue = NULL;
    }
    if (session->d3d12_device)
    {
        ID3D12Device_Release(session->d3d12_device);
        session->d3d12_device = NULL;
    }
}
