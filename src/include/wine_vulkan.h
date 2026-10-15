/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef WINE_OPENXR_WINE_VULKAN_H
#define WINE_OPENXR_WINE_VULKAN_H

#include <stdint.h>

/* This adapter accepts only the Wine build whose private handle ABI is verified. */
int wine_vk_abi_supported(const char *version, const char *build_id);
uint64_t wine_vk_native_dispatchable(uint64_t handle);
int wine_vk_binding_valid(uint64_t instance, uint64_t physical_device,
                          uint64_t device,
                          uint32_t queue_family, uint32_t queue_index);

#endif
