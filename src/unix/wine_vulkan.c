/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

#include "wine/vulkan_driver.h"
#include "wine_vulkan.h"

static int read_object(uint64_t address, void *output, size_t size)
{
    mach_vm_size_t read_size = 0;
    if (!address || address % _Alignof(uint64_t)) return 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size,
                                  (mach_vm_address_t)(uintptr_t)output, &read_size)
               == KERN_SUCCESS && read_size == size;
}

static int unwrap(uint64_t handle, struct vulkan_object *object, uint64_t *unix_handle)
{
    struct vulkan_client_object client;
    if (!read_object(handle, &client, sizeof(client)) || !client.loader_magic ||
        !read_object(client.unix_handle, object, sizeof(*object)) ||
        !object->host_handle || object->client_handle != handle)
        return 0;
    *unix_handle = client.unix_handle;
    return 1;
}

int wine_vk_abi_supported(const char *version, const char *build_id)
{
    return version && build_id && !strcmp(version, "11.0") &&
           !strcmp(build_id, "wine-11.0-8726-g2e2f5fca349");
}

uint64_t wine_vk_native_dispatchable(uint64_t handle)
{
    struct vulkan_object object;
    uint64_t unix_handle;
    return unwrap(handle, &object, &unix_handle) ? object.host_handle : 0;
}

int wine_vk_binding_valid(uint64_t instance, uint64_t physical_device,
                          uint64_t device,
                          uint32_t queue_family, uint32_t queue_index)
{
    struct vulkan_object object;
    uint64_t native_instance, native_physical_device, native_device;
    uint64_t parent, queue_count, index;
    struct vulkan_queue queue;

    if (!unwrap(instance, &object, &native_instance) ||
        !unwrap(physical_device, &object, &native_physical_device) ||
        !unwrap(device, &object, &native_device))
        return 0;
    if (!read_object(native_physical_device + offsetof(struct vulkan_physical_device, instance),
                     &parent, sizeof(parent)) || parent != native_instance ||
        !read_object(native_device + offsetof(struct vulkan_device, physical_device),
                     &parent, sizeof(parent)) || parent != native_physical_device ||
        !read_object(native_device + offsetof(struct vulkan_device, queue_count),
                     &queue_count, sizeof(queue_count)) || queue_count > UINT32_MAX)
        return 0;
    for (index = 0; index < queue_count; index++)
    {
        if (!read_object(native_device + offsetof(struct vulkan_device, queues) +
                         index * sizeof(queue), &queue, sizeof(queue)) ||
            (uint64_t)(uintptr_t)queue.device != native_device)
            return 0;
        if (!queue.info.flags && queue.info.queueFamilyIndex == queue_family &&
            queue.info.queueIndex == queue_index)
            return 1;
    }
    return 0;
}
