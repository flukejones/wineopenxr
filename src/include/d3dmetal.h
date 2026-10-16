/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __WINE_OPENXR_D3DMETAL_H
#define __WINE_OPENXR_D3DMETAL_H

#include <stdint.h>

enum d3dmetal_op
{
    D3DMETAL_BEGIN_TEXTURE = 1,
    D3DMETAL_BEGIN_FENCE,
    D3DMETAL_END,
    D3DMETAL_RELEASE_EVENT,
};

enum d3dmetal_result
{
    D3DMETAL_SUCCESS,
    D3DMETAL_INVALID_ARGUMENT = -1,
    D3DMETAL_UNSUPPORTED = -2,
    D3DMETAL_BUSY = -3,
    D3DMETAL_FOREIGN_DEVICE = -4,
    D3DMETAL_INCOMPATIBLE_TEXTURE = -5,
    D3DMETAL_ALLOCATION_FAILED = -6,
    D3DMETAL_CREATION_COUNT = -7,
};

/* Pair successful BEGIN/END on one thread around one D3D resource creation.
 * Textures require Private storage. END clears the operation and transfers
 * a separately retained event on fence success; release it with RELEASE_EVENT. */
struct d3dmetal_params
{
    uint32_t op;
    int32_t result;
    uint64_t device;
    uint64_t texture;
    uint64_t event;
    uint32_t seen;
    uint32_t reserved;
};

#endif
