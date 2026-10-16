/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __WINE_OPENXR_D3D12_BRIDGE_H
#define __WINE_OPENXR_D3D12_BRIDGE_H

struct wine_XrSession;
struct wine_XrSwapchain;

/* Caller tears down the host session and retained resources after failure. */
XrResult wine_create_d3d12_session(struct wine_XrSession *session,
                                  const XrGraphicsBindingD3D12KHR *binding,
                                  XrSystemId system_id);
XrResult wine_create_d3d12_image(struct wine_XrSwapchain *swapchain,
                                uint64_t metal_texture, ID3D12Resource **resource);
void wine_destroy_d3d12_session(struct wine_XrSession *session);

#endif
