/* SPDX-License-Identifier: LGPL-2.1-or-later */

#import <Metal/Metal.h>
#import <objc/runtime.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "d3dmetal.h"
#include "wine/debug_slim.h"

WINE_DEFAULT_DEBUG_CHANNEL(openxr);

#define MAX_HOOKS 64

struct method_hook
{
    Class owner;
    SEL selector;
    IMP original;
};

static struct method_hook hooks[MAX_HOOKS];
static _Atomic unsigned hook_count;
static pthread_once_t install_once = PTHREAD_ONCE_INIT;
static int install_result;

struct creation
{
    uint32_t op;
    int32_t result;
    unsigned seen;
    unsigned event_depth;
    id<MTLDevice> device;
    id<MTLTexture> texture;
    id<MTLSharedEvent> event;
};

static __thread struct creation creation;

int32_t wine_d3dmetal(void *args);

static void fail_creation(int32_t result)
{
    if (creation.result == D3DMETAL_SUCCESS)
        creation.result = result;
}

static IMP original_method(id object, SEL selector)
{
    unsigned count = atomic_load_explicit(&hook_count, memory_order_acquire);
    Class owner;
    unsigned i;

    for (owner = object_getClass(object); owner; owner = class_getSuperclass(owner))
        for (i = 0; i < count; i++)
            if (hooks[i].owner == owner && hooks[i].selector == selector)
                return hooks[i].original;
    WINE_ERR("missing original Metal method %s on %s\n", sel_getName(selector),
             class_getName(object_getClass(object)));
    fail_creation(D3DMETAL_UNSUPPORTED);
    return NULL;
}

static int install_method(Class cls, SEL selector, IMP replacement)
{
    unsigned count = atomic_load(&hook_count), i;
    Method method;
    Class owner;

    if (!(method = class_getInstanceMethod(cls, selector)))
        return 0;
    owner = cls;
    while (class_getSuperclass(owner) &&
           class_getInstanceMethod(class_getSuperclass(owner), selector) == method)
        owner = class_getSuperclass(owner);
    for (i = 0; i < count; i++)
        if (hooks[i].owner == owner && hooks[i].selector == selector)
            return 1;
    if (count == MAX_HOOKS || !method_getImplementation(method))
        return 0;
    hooks[count].owner = owner;
    hooks[count].selector = selector;
    hooks[count].original = method_getImplementation(method);
    atomic_store_explicit(&hook_count, count + 1, memory_order_release);
    method_setImplementation(method, replacement);
    return 1;
}

static MTLPixelFormat format_family(MTLPixelFormat format)
{
    switch (format)
    {
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB:
    case MTLPixelFormatRGBA8Snorm:
    case MTLPixelFormatRGBA8Uint:
    case MTLPixelFormatRGBA8Sint:
        return MTLPixelFormatRGBA8Unorm;
    case MTLPixelFormatRGBA16Unorm:
    case MTLPixelFormatRGBA16Snorm:
    case MTLPixelFormatRGBA16Uint:
    case MTLPixelFormatRGBA16Sint:
    case MTLPixelFormatRGBA16Float:
        return MTLPixelFormatRGBA16Float;
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB:
        return MTLPixelFormatBGRA8Unorm;
    default:
        return format;
    }
}

static id substitute_texture(id<MTLDevice> device, MTLTextureDescriptor *desc)
{
    id<MTLTexture> texture = creation.texture;
    NSUInteger layers;
    MTLTextureSwizzleChannels have_swizzle, want_swizzle;
    id view;

    if (creation.op != D3DMETAL_BEGIN_TEXTURE)
        return nil;
    if (++creation.seen != 1)
    {
        fail_creation(D3DMETAL_CREATION_COUNT);
        return nil;
    }
    if (device != creation.device)
    {
        WINE_ERR("D3DMetal texture device %p differs from runtime device %p\n",
                 (void *)device, (void *)creation.device);
        fail_creation(D3DMETAL_FOREIGN_DEVICE);
        return nil;
    }
    if (!desc)
    {
        fail_creation(D3DMETAL_INVALID_ARGUMENT);
        return nil;
    }
    layers = desc.textureType == MTLTextureType2DArray ? desc.arrayLength : 1;
    have_swizzle = texture.swizzle;
    want_swizzle = desc.swizzle;
    if ((desc.textureType != MTLTextureType2D && desc.textureType != MTLTextureType2DArray) ||
        (texture.textureType != MTLTextureType2D && texture.textureType != MTLTextureType2DArray) ||
        desc.width != texture.width || desc.height != texture.height || desc.depth != texture.depth ||
        desc.sampleCount != texture.sampleCount || desc.mipmapLevelCount != texture.mipmapLevelCount ||
        layers != texture.arrayLength || (desc.usage & ~texture.usage) ||
        memcmp(&have_swizzle, &want_swizzle, sizeof(have_swizzle)) ||
        format_family(texture.pixelFormat) != format_family(desc.pixelFormat))
    {
        WINE_ERR("D3DMetal texture descriptor does not match runtime image "
                 "(requested %lux%lu/%lu/%lu/%lu format %lu usage %#lx; "
                 "runtime %lux%lu/%lu/%lu/%lu format %lu usage %#lx)\n",
                 (unsigned long)desc.width, (unsigned long)desc.height,
                 (unsigned long)layers, (unsigned long)desc.mipmapLevelCount,
                 (unsigned long)desc.sampleCount, (unsigned long)desc.pixelFormat,
                 (unsigned long)desc.usage,
                 (unsigned long)texture.width, (unsigned long)texture.height,
                 (unsigned long)texture.arrayLength, (unsigned long)texture.mipmapLevelCount,
                 (unsigned long)texture.sampleCount, (unsigned long)texture.pixelFormat,
                 (unsigned long)texture.usage);
        fail_creation(D3DMETAL_INCOMPATIBLE_TEXTURE);
        return nil;
    }
    if (desc.pixelFormat == texture.pixelFormat && desc.textureType == texture.textureType)
        return [texture retain];
    if (!(texture.usage & MTLTextureUsagePixelFormatView))
    {
        fail_creation(D3DMETAL_INCOMPATIBLE_TEXTURE);
        return nil;
    }
    view = [texture newTextureViewWithPixelFormat:desc.pixelFormat textureType:desc.textureType
                                         levels:NSMakeRange(0, texture.mipmapLevelCount)
                                         slices:NSMakeRange(0, layers)];
    if (!view)
    {
        WINE_ERR("D3DMetal texture view allocation failed for format %lu from runtime format %lu\n",
                 (unsigned long)desc.pixelFormat, (unsigned long)texture.pixelFormat);
        fail_creation(D3DMETAL_ALLOCATION_FAILED);
    }
    return view;
}

static id hook_device_texture(id self, SEL selector, MTLTextureDescriptor *desc)
{
    IMP original = original_method(self, selector);
    id texture = substitute_texture(self, desc);

    if (creation.op == D3DMETAL_BEGIN_TEXTURE)
        WINE_TRACE("D3DMetal import hit -[%s %s], storage %lu\n",
                   class_getName(object_getClass(self)), sel_getName(selector),
                   (unsigned long)desc.storageMode);
    return texture ? texture : original ?
        ((id (*)(id, SEL, MTLTextureDescriptor *))original)(self, selector, desc) : nil;
}

static id hook_heap_texture(id self, SEL selector, MTLTextureDescriptor *desc)
{
    IMP original = original_method(self, selector);
    id texture = creation.op == D3DMETAL_BEGIN_TEXTURE ?
                 substitute_texture([(id<MTLHeap>)self device], desc) : nil;

    if (creation.op == D3DMETAL_BEGIN_TEXTURE)
        WINE_TRACE("D3DMetal import hit -[%s %s], storage %lu\n",
                   class_getName(object_getClass(self)), sel_getName(selector),
                   (unsigned long)desc.storageMode);
    return texture ? texture : original ?
        ((id (*)(id, SEL, MTLTextureDescriptor *))original)(self, selector, desc) : nil;
}

static id hook_heap_placed_texture(id self, SEL selector, MTLTextureDescriptor *desc, NSUInteger offset)
{
    IMP original = original_method(self, selector);
    id texture = creation.op == D3DMETAL_BEGIN_TEXTURE ?
                 substitute_texture([(id<MTLHeap>)self device], desc) : nil;

    if (creation.op == D3DMETAL_BEGIN_TEXTURE)
        WINE_TRACE("D3DMetal import hit -[%s %s], storage %lu\n",
                   class_getName(object_getClass(self)), sel_getName(selector),
                   (unsigned long)desc.storageMode);
    return texture ? texture : original ?
        ((id (*)(id, SEL, MTLTextureDescriptor *, NSUInteger))original)(
            self, selector, desc, offset) : nil;
}

static id capture_event(id<MTLDevice> device, id event)
{
    if (creation.op != D3DMETAL_BEGIN_FENCE || creation.event_depth)
        return event;
    if (++creation.seen != 1)
        fail_creation(D3DMETAL_CREATION_COUNT);
    else if (device != creation.device)
        fail_creation(D3DMETAL_FOREIGN_DEVICE);
    else if (!event || ![event conformsToProtocol:@protocol(MTLSharedEvent)])
        fail_creation(D3DMETAL_ALLOCATION_FAILED);
    else
        creation.event = [event retain];
    return event;
}

static id hook_shared_event(id self, SEL selector)
{
    IMP original = original_method(self, selector);
    id event;

    creation.event_depth++;
    event = original ? ((id (*)(id, SEL))original)(self, selector) : nil;
    creation.event_depth--;
    return capture_event(self, event);
}

static id hook_shared_event_options(id self, SEL selector, NSUInteger options)
{
    IMP original = original_method(self, selector);
    id event;

    creation.event_depth++;
    event = original ? ((id (*)(id, SEL, NSUInteger))original)(self, selector, options) : nil;
    creation.event_depth--;
    return capture_event(self, event);
}

static id hook_event(id self, SEL selector)
{
    IMP original = original_method(self, selector);
    IMP shared;
    id event;

    creation.event_depth++;
    if (creation.op == D3DMETAL_BEGIN_FENCE && self == creation.device)
    {
        shared = original_method(self, @selector(newSharedEvent));
        event = shared ? ((id (*)(id, SEL))shared)(self, @selector(newSharedEvent)) : nil;
        if (event)
        {
            creation.event_depth--;
            return capture_event(self, event);
        }
        fail_creation(D3DMETAL_ALLOCATION_FAILED);
    }
    event = original ? ((id (*)(id, SEL))original)(self, selector) : nil;
    creation.event_depth--;
    return capture_event(self, event);
}

static void install_hooks(void)
{
    Dl_info image;
    NSArray<id<MTLDevice>> *devices;
    Class classes[8];
    unsigned class_count = 0, i;
    int valid = 1;

    /* Objective-C keeps these IMPs after the Wine DLL unloads. */
    if (!dladdr((void *)wine_d3dmetal, &image) ||
        !dlopen(image.dli_fname, RTLD_NOW | RTLD_NODELETE))
    {
        WINE_ERR("cannot pin D3DMetal interop callbacks: %s\n", dlerror());
        install_result = D3DMETAL_UNSUPPORTED;
        return;
    }
    devices = MTLCopyAllDevices();
    for (id<MTLDevice> device in devices)
    {
        Class cls = object_getClass(device);
        MTLHeapDescriptor *heap_desc;

        for (i = 0; i < class_count && classes[i] != cls; i++)
            ;
        if (i != class_count)
            continue;
        if (class_count == sizeof(classes) / sizeof(classes[0]))
        {
            valid = 0;
            break;
        }
        classes[class_count++] = cls;
        valid &= install_method(cls, @selector(newTextureWithDescriptor:), (IMP)hook_device_texture);
        valid &= install_method(cls, @selector(newSharedEvent), (IMP)hook_shared_event);
        install_method(cls, @selector(newSharedEventWithOptions:), (IMP)hook_shared_event_options);
        install_method(cls, @selector(newEvent), (IMP)hook_event);

        heap_desc = [[MTLHeapDescriptor alloc] init];
        heap_desc.size = 65536;
        for (unsigned storage = 0; storage < 2; storage++)
        {
            id<MTLHeap> heap;
            heap_desc.storageMode = storage ? MTLStorageModePrivate : MTLStorageModeShared;
            heap = [device newHeapWithDescriptor:heap_desc];
            if (heap)
            {
                Class heap_cls = object_getClass(heap);
                install_method(heap_cls, @selector(newTextureWithDescriptor:), (IMP)hook_heap_texture);
                install_method(heap_cls, @selector(newTextureWithDescriptor:offset:),
                               (IMP)hook_heap_placed_texture);
                [heap release];
            }
        }
        [heap_desc release];
    }
    [devices release];
    install_result = valid && class_count ? D3DMETAL_SUCCESS : D3DMETAL_UNSUPPORTED;
}

int32_t wine_d3dmetal(void *args)
{
    struct d3dmetal_params *params = args;

    params->result = D3DMETAL_SUCCESS;
    @autoreleasepool {
        switch (params->op)
        {
        case D3DMETAL_BEGIN_TEXTURE:
        case D3DMETAL_BEGIN_FENCE:
            if (creation.op)
            {
                params->result = D3DMETAL_BUSY;
                break;
            }
            pthread_once(&install_once, install_hooks);
            if (install_result)
            {
                params->result = install_result;
                break;
            }
            creation.texture = params->op == D3DMETAL_BEGIN_TEXTURE ?
                               (id<MTLTexture>)(void *)(uintptr_t)params->texture : nil;
            creation.device = params->op == D3DMETAL_BEGIN_TEXTURE ? creation.texture.device :
                              (id<MTLDevice>)(void *)(uintptr_t)params->device;
            if (!creation.device || (params->op == D3DMETAL_BEGIN_TEXTURE && !creation.texture))
            {
                memset(&creation, 0, sizeof(creation));
                params->result = D3DMETAL_INVALID_ARGUMENT;
                break;
            }
            /* D3DMetal initializes Shared textures through their heap's CPU backing. */
            if (creation.texture && creation.texture.storageMode != MTLStorageModePrivate)
            {
                WINE_ERR("D3DMetal interop requires Private runtime texture storage, got %lu\n",
                         (unsigned long)creation.texture.storageMode);
                memset(&creation, 0, sizeof(creation));
                params->result = D3DMETAL_INCOMPATIBLE_TEXTURE;
                break;
            }
            creation.op = params->op;
            break;
        case D3DMETAL_END:
            params->seen = creation.seen;
            params->event = 0;
            params->result = creation.result;
            if (!creation.op)
                params->result = D3DMETAL_INVALID_ARGUMENT;
            else if (!params->result && creation.seen != 1)
                params->result = D3DMETAL_CREATION_COUNT;
            if (params->result)
                [creation.event release];
            else
                params->event = (uint64_t)(uintptr_t)creation.event;
            memset(&creation, 0, sizeof(creation));
            break;
        case D3DMETAL_RELEASE_EVENT:
            [(id<MTLSharedEvent>)(void *)(uintptr_t)params->event release];
            break;
        default:
            params->result = D3DMETAL_INVALID_ARGUMENT;
            break;
        }
    }
    return 0;
}
