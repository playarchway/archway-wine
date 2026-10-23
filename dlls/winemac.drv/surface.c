/*
 * Mac driver window surface implementation
 *
 * Copyright 1993, 1994, 2011 Alexandre Julliard
 * Copyright 2006 Damjan Jovanovic
 * Copyright 2012, 2013 Ken Thomases for CodeWeavers, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"
#include <stdlib.h>
#include "macdrv.h"

WINE_DEFAULT_DEBUG_CHANNEL(bitblt);

#define SHARED_GDI_MAGIC 0x4944474d
#define SHARED_GDI_VERSION 1
#define SHARED_GDI_LIMIT (512u * 1024u * 1024u)

static const WCHAR shared_gdi_property[] = {'W','i','n','e','M','a','c','S','h','a','r','e','d','G','d','i','S','u','r','f','a','c','e',0};

struct shared_gdi_header
{
    UINT32 magic, version, owner_pid, tag;
    UINT64 hwnd;
    INT32 left, top, width, height, stride;
    UINT32 pixel_offset, allocation, retired;
    RECT dirty;
    BYTE pixels[1];
};

struct shared_gdi_mapping
{
    HANDLE section, mutant;
    struct shared_gdi_header *header;
    SIZE_T size;
    DWORD owner_pid, tag;
    BOOL foreign;
    BOOL local_only;
    LONG invalid;
    RECT rect;
    UINT width, height, stride, pixel_offset;
};

static UINT shared_gdi_offset(void)
{
    return (FIELD_OFFSET(struct shared_gdi_header, pixels) + 15) & ~15;
}

static void shared_gdi_name(WCHAR *out, DWORD pid, HWND hwnd, DWORD tag, char type)
{
    char name[160];
    snprintf(name, sizeof(name), "\\Sessions\\%u\\BaseNamedObjects\\WineMacGdi_%08x_%016llx_%08x_%c",
             RtlGetCurrentPeb()->SessionId, pid, (unsigned long long)(ULONG_PTR)hwnd, tag, type);
    asciiz_to_unicode(out, name);
}

static void shared_gdi_close(struct shared_gdi_mapping *shared)
{
    if (shared->header) NtUnmapViewOfSection(NtCurrentProcess(), shared->header);
    if (shared->section) NtClose(shared->section);
    if (shared->mutant) NtClose(shared->mutant);
    memset(shared, 0, sizeof(*shared));
}

static BOOL shared_gdi_wait(struct shared_gdi_mapping *shared)
{
    LARGE_INTEGER timeout = {.QuadPart = -10000000};
    NTSTATUS status = NtWaitForSingleObject(shared->mutant, FALSE, &timeout);

    if (status == STATUS_ABANDONED)
    {
        InterlockedExchange(&shared->invalid, TRUE);
        if (shared->header && shared->size >= FIELD_OFFSET(struct shared_gdi_header, retired) + sizeof(UINT32))
            shared->header->retired = TRUE;
        NtReleaseMutant(shared->mutant, NULL);
    }
    return status == STATUS_SUCCESS;
}

static BOOL shared_gdi_map(struct shared_gdi_mapping *shared)
{
    void *base = NULL;
    SIZE_T size = 0;

    if (NtMapViewOfSection(shared->section, NtCurrentProcess(), &base, 0, 0, NULL, &size,
                           ViewUnmap, 0, PAGE_READWRITE)) return FALSE;
    shared->header = base;
    shared->size = size;
    return TRUE;
}

static BOOL shared_gdi_create(struct shared_gdi_mapping *shared, HWND hwnd, const RECT *rect)
{
    WCHAR name_buffer[160];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    LARGE_INTEGER allocation;
    NTSTATUS status;
    DWORD tag, pid = GetCurrentProcessId();
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    UINT offset = shared_gdi_offset();
    unsigned int attempt;

    if (width <= 0 || height <= 0 || width > 32768 || height > 32768 ||
        (UINT64)width * height * 4 + offset > SHARED_GDI_LIMIT) return FALSE;
    allocation.QuadPart = (UINT64)width * height * 4 + offset;

    for (attempt = 0; attempt < 32; attempt++)
    {
        tag = arc4random();
        if (!tag) continue;
        shared_gdi_name(name_buffer, pid, hwnd, tag, 'M');
        RtlInitUnicodeString(&name, name_buffer);
        InitializeObjectAttributes(&attr, &name, 0, NULL, NULL);
        status = NtCreateMutant(&shared->mutant, MUTEX_ALL_ACCESS, &attr, FALSE);
        if (status == STATUS_OBJECT_NAME_COLLISION) continue;
        if (status) break;

        shared_gdi_name(name_buffer, pid, hwnd, tag, 'S');
        RtlInitUnicodeString(&name, name_buffer);
        InitializeObjectAttributes(&attr, &name, 0, NULL, NULL);
        status = NtCreateSection(&shared->section, SECTION_ALL_ACCESS, &attr, &allocation,
                                 PAGE_READWRITE, SEC_COMMIT, NULL);
        if (status == STATUS_OBJECT_NAME_COLLISION)
        {
            NtClose(shared->mutant);
            shared->mutant = NULL;
            continue;
        }
        if (status || !shared_gdi_map(shared)) break;

        shared->owner_pid = pid;
        shared->tag = tag;
        shared->local_only = TRUE;
        shared->header->magic = SHARED_GDI_MAGIC;
        shared->header->version = SHARED_GDI_VERSION;
        shared->header->owner_pid = pid;
        shared->header->tag = tag;
        shared->header->hwnd = (ULONG_PTR)hwnd;
        shared->header->left = rect->left;
        shared->header->top = rect->top;
        shared->header->width = width;
        shared->header->height = height;
        shared->header->stride = width * 4;
        shared->header->pixel_offset = offset;
        shared->header->allocation = allocation.QuadPart;
        shared->header->retired = FALSE; /* private until the owning window publishes its tag */
        SetRectEmpty(&shared->header->dirty);
        shared->rect = *rect;
        shared->width = width;
        shared->height = height;
        shared->stride = width * 4;
        shared->pixel_offset = offset;
        return TRUE;
    }
    shared_gdi_close(shared);
    return FALSE;
}

static BOOL shared_gdi_valid(const struct shared_gdi_mapping *shared, HWND hwnd, DWORD pid, DWORD tag)
{
    const struct shared_gdi_header *h = shared->header;
    UINT64 bytes;

    if (shared->size < shared_gdi_offset() || h->magic != SHARED_GDI_MAGIC ||
        h->version != SHARED_GDI_VERSION || h->owner_pid != pid || h->hwnd != (ULONG_PTR)hwnd ||
        h->tag != tag || h->width <= 0 || h->height <= 0 || h->width > 32768 || h->height > 32768 ||
        h->stride != h->width * 4 || h->pixel_offset != shared_gdi_offset()) return FALSE;
    bytes = (UINT64)h->stride * h->height + h->pixel_offset;
    return bytes == h->allocation && bytes <= SHARED_GDI_LIMIT && bytes <= shared->size &&
           (INT64)h->left + h->width <= INT32_MAX && (INT64)h->top + h->height <= INT32_MAX;
}

static BOOL shared_gdi_open(struct shared_gdi_mapping *shared, HWND hwnd)
{
    WCHAR name_buffer[160];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    DWORD pid = 0, tag = HandleToULong(NtUserGetProp(hwnd, shared_gdi_property));

    if (!tag || !NtUserGetWindowThread(hwnd, &pid) || !pid || pid == GetCurrentProcessId()) return FALSE;
    shared_gdi_name(name_buffer, pid, hwnd, tag, 'M');
    RtlInitUnicodeString(&name, name_buffer);
    InitializeObjectAttributes(&attr, &name, 0, NULL, NULL);
    if (NtOpenMutant(&shared->mutant, MUTEX_ALL_ACCESS, &attr)) goto failed;
    shared_gdi_name(name_buffer, pid, hwnd, tag, 'S');
    RtlInitUnicodeString(&name, name_buffer);
    InitializeObjectAttributes(&attr, &name, 0, NULL, NULL);
    if (NtOpenSection(&shared->section, SECTION_MAP_READ | SECTION_MAP_WRITE, &attr) ||
        !shared_gdi_map(shared) || !shared_gdi_wait(shared)) goto failed;
    if (!shared_gdi_valid(shared, hwnd, pid, tag) || shared->header->retired ||
        HandleToULong(NtUserGetProp(hwnd, shared_gdi_property)) != tag ||
        !NtUserGetWindowThread(hwnd, &shared->owner_pid) || shared->owner_pid != pid)
    {
        NtReleaseMutant(shared->mutant, NULL);
        goto failed;
    }
    shared->tag = tag;
    shared->foreign = TRUE;
    shared->rect.left = shared->header->left;
    shared->rect.top = shared->header->top;
    shared->rect.right = shared->rect.left + shared->header->width;
    shared->rect.bottom = shared->rect.top + shared->header->height;
    shared->width = shared->header->width;
    shared->height = shared->header->height;
    shared->stride = shared->header->stride;
    shared->pixel_offset = shared->header->pixel_offset;
    NtReleaseMutant(shared->mutant, NULL);
    return TRUE;
failed:
    shared_gdi_close(shared);
    return FALSE;
}

static inline int get_dib_stride(int width, int bpp)
{
    return ((width * bpp + 31) >> 3) & ~3;
}

static inline int get_dib_image_size(const BITMAPINFO *info)
{
    return get_dib_stride(info->bmiHeader.biWidth, info->bmiHeader.biBitCount)
        * abs(info->bmiHeader.biHeight);
}


struct macdrv_window_surface
{
    struct window_surface   header;
    WineWindow             *window;
    CGDataProviderRef       provider;
    void                   *remote_context;
    HWND                    remote_root;
    struct shared_gdi_mapping shared;
};

static struct macdrv_window_surface *get_mac_surface(struct window_surface *surface);

static BOOL shared_gdi_current(struct window_surface *window_surface)
{
    struct shared_gdi_mapping *shared = &get_mac_surface(window_surface)->shared;
    DWORD pid = 0;

    if (!shared->mutant) return TRUE;
    if (InterlockedCompareExchange(&shared->invalid, 0, 0)) return FALSE;
    if (shared->local_only && !shared->foreign) return TRUE;
    return HandleToULong(NtUserGetProp(window_surface->hwnd, shared_gdi_property)) == shared->tag &&
           NtUserGetWindowThread(window_surface->hwnd, &pid) && pid == shared->owner_pid;
}

static BOOL macdrv_surface_begin_pixel_access(struct window_surface *window_surface)
{
    struct shared_gdi_mapping *shared = &get_mac_surface(window_surface)->shared;

    if (!shared->mutant) return TRUE;
    if (!shared_gdi_wait(shared)) return FALSE;
    if (shared->header->retired || !shared_gdi_current(window_surface))
    {
        if (shared->header->retired) InterlockedExchange(&shared->invalid, TRUE);
        NtReleaseMutant(shared->mutant, NULL);
        return FALSE;
    }
    return TRUE;
}

static void macdrv_surface_end_pixel_access(struct window_surface *window_surface)
{
    struct shared_gdi_mapping *shared = &get_mac_surface(window_surface)->shared;
    if (!shared->mutant) return;
    NtReleaseMutant(shared->mutant, NULL);
}

static BOOL macdrv_surface_is_current(struct window_surface *window_surface)
{
    return shared_gdi_current(window_surface);
}

static HBITMAP create_memory_bitmap(HWND hwnd, BITMAPINFO *info, void *bits, BOOL foreign)
{
    D3DKMT_CREATEDCFROMMEMORY desc = {.Format = D3DDDIFMT_A8R8G8B8};
    HWND dc_hwnd = foreign ? NtUserGetDesktopWindow() : hwnd;
    UINT status;

    desc.Width = info->bmiHeader.biWidth;
    desc.Height = abs(info->bmiHeader.biHeight);
    desc.Pitch = info->bmiHeader.biSizeImage / desc.Height;
    desc.pMemory = bits;
    desc.hDeviceDc = NtUserGetDCEx(dc_hwnd, 0, DCX_CACHE | DCX_WINDOW);
    status = NtGdiDdDDICreateDCFromMemory(&desc);
    if (status) ERR("Failed to create shared HBITMAP, status %#x\n", status);
    else NtGdiDeleteObjectApp(desc.hDc);
    if (desc.hDeviceDc) NtUserReleaseDC(dc_hwnd, desc.hDeviceDc);
    return status ? 0 : desc.hBitmap;
}

static CGDataProviderRef data_provider_create(size_t size, void **bits)
{
    CGDataProviderRef provider;
    CFMutableDataRef data;

    if (!(data = CFDataCreateMutable(kCFAllocatorDefault, size))) return NULL;
    CFDataSetLength(data, size);

    if ((provider = CGDataProviderCreateWithCFData(data)))
        *bits = CFDataGetMutableBytePtr(data);
    CFRelease(data);

    return provider;
}

static BOOL macdrv_shared_present(struct macdrv_window_surface *surface)
{
    struct shared_gdi_mapping *shared = &surface->shared;
    struct window_surface *window_surface = &surface->header;
    CGColorSpaceRef colorspace;
    CGDataProviderRef provider;
    CGImageRef image;
    struct macdrv_win_data *data;
    size_t size = (size_t)shared->stride * shared->height;
    void *copy;

    if (!shared_gdi_wait(shared)) return FALSE;
    if (shared->header->retired || !shared_gdi_current(window_surface) ||
        !(provider = data_provider_create(size, &copy)))
    {
        NtReleaseMutant(shared->mutant, NULL);
        return FALSE;
    }
    memcpy(copy, (BYTE *)shared->header + shared->pixel_offset, size);
    SetRectEmpty(&shared->header->dirty);
    NtReleaseMutant(shared->mutant, NULL);

    colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    image = CGImageCreate(shared->width, shared->height, 8, 32,
                          shared->stride, colorspace,
                          kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
                          provider, NULL, retina_on, kCGRenderingIntentDefault);
    CGColorSpaceRelease(colorspace);
    CGDataProviderRelease(provider);
    if (!image) return FALSE;
    macdrv_window_set_color_image(surface->window, image, cgrect_from_rect(window_surface->rect),
                                  cgrect_from_rect(window_surface->rect));
    CGImageRelease(image);

    if ((data = get_win_data(window_surface->hwnd)))
    {
        if (data->client_view)
        {
            macdrv_set_view_hidden(data->client_view, TRUE);
            data->client_view = NULL;
        }
        release_win_data(data);
    }
    return TRUE;
}

static BOOL macdrv_shared_flush(struct macdrv_window_surface *surface, const RECT *dirty)
{
    struct shared_gdi_mapping *shared = &surface->shared;
    RECT *bounds;

    if (!shared->foreign) return macdrv_shared_present(surface);
    if (!shared_gdi_wait(shared)) return FALSE;
    if (shared->header->retired || !shared_gdi_current(&surface->header))
    {
        NtReleaseMutant(shared->mutant, NULL);
        return FALSE;
    }
    bounds = &shared->header->dirty;
    if (IsRectEmpty(bounds)) *bounds = *dirty;
    else
    {
        bounds->left = min(bounds->left, dirty->left);
        bounds->top = min(bounds->top, dirty->top);
        bounds->right = max(bounds->right, dirty->right);
        bounds->bottom = max(bounds->bottom, dirty->bottom);
    }
    NtReleaseMutant(shared->mutant, NULL);
    if (!NtUserPostMessage(surface->header.hwnd, WM_MACDRV_SHARED_GDI_DIRTY, shared->tag, 0))
        WARN("failed to notify owner of shared GDI pixels for %p\n", surface->header.hwnd);
    return TRUE;
}

/***********************************************************************
 *              macdrv_surface_set_clip
 */
static void macdrv_surface_set_clip(struct window_surface *window_surface, const RECT *rects, UINT count)
{
}

/***********************************************************************
 *              macdrv_surface_flush
 */
static BOOL macdrv_surface_flush(struct window_surface *window_surface, const RECT *rect, const RECT *dirty,
                                 const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                                 const BITMAPINFO *shape_info, const void *shape_bits)
{
    struct macdrv_window_surface *surface = get_mac_surface(window_surface);
    CGImageAlphaInfo alpha_info = (window_surface->alpha_mask ? kCGImageAlphaPremultipliedFirst : kCGImageAlphaNoneSkipFirst);
    CGColorSpaceRef colorspace;
    CGImageRef image;
    struct macdrv_win_data *data;
    CGDataProviderRef provider = surface->provider;
    UINT image_width = color_info->bmiHeader.biWidth;
    UINT image_height = abs(color_info->bmiHeader.biHeight);
    size_t row_bytes;

    if (surface->shared.mutant) return macdrv_shared_flush(surface, dirty);

    if (!image_height) return FALSE;
    row_bytes = color_info->bmiHeader.biSizeImage / image_height;

    if (surface->remote_context)
    {
        RECT window_rect;
        BYTE *copy;
        int width, height;
        UINT y;

        if (!NtUserGetWindowRect(window_surface->hwnd, &window_rect,
                                 NtUserGetWinMonitorDpi(window_surface->hwnd, MDT_RAW_DPI))) return FALSE;
        width = window_rect.right - window_rect.left;
        height = window_rect.bottom - window_rect.top;
        if (width <= 0 || height <= 0) return FALSE;
        image_width = min(image_width, (UINT)width);
        image_height = min(image_height, (UINT)height);
        if (!image_width || !image_height || image_width > SIZE_MAX / 4 / image_height) return FALSE;
        if (!(provider = data_provider_create((size_t)image_width * image_height * 4, (void **)&copy))) return FALSE;
        for (y = 0; y < image_height; y++)
            memcpy(copy + (size_t)y * image_width * 4, (const BYTE *)color_bits + (size_t)y * row_bytes,
                   (size_t)image_width * 4);
        row_bytes = (size_t)image_width * 4;
    }

    colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    image = CGImageCreate(image_width, image_height, 8, 32, row_bytes, colorspace,
                          alpha_info | kCGBitmapByteOrder32Little, provider, NULL, retina_on, kCGRenderingIntentDefault);
    CGColorSpaceRelease(colorspace);
    if (surface->remote_context) CGDataProviderRelease(provider);
    if (!image) return FALSE;

    if (surface->remote_context) macdrv_gdi_context_set_image(surface->remote_context, image);
    else macdrv_window_set_color_image(surface->window, image, cgrect_from_rect(*rect), cgrect_from_rect(*dirty));
    CGImageRelease(image);

    if (surface->remote_context) return TRUE;

    if (shape_changed)
    {
        if (!shape_bits)
            macdrv_window_set_shape_image(surface->window, NULL);
        else
        {
            const BYTE *src = shape_bits;
            CGDataProviderRef provider;
            CGImageRef image;
            BYTE *dst;
            UINT i;

            if (!(provider = data_provider_create(shape_info->bmiHeader.biSizeImage, (void **)&dst))) return TRUE;
            for (i = 0; i < shape_info->bmiHeader.biSizeImage; i++) dst[i] = ~src[i]; /* CGImage mask bits are inverted */

            image = CGImageMaskCreate(shape_info->bmiHeader.biWidth, abs(shape_info->bmiHeader.biHeight), 1, 1,
                                      shape_info->bmiHeader.biSizeImage / abs(shape_info->bmiHeader.biHeight),
                                      provider, NULL, retina_on);
            CGDataProviderRelease(provider);

            macdrv_window_set_shape_image(surface->window, image);
            CGImageRelease(image);
        }
    }

    /* The window may have been previously drawn with client_surface, for example, when the window
     * had been a target for a D3D swapchain. Hide the client_view so that it doesn't occlude the
     * content in the window_surface */
    if ((data = get_win_data(window_surface->hwnd)))
    {
        if (data->client_view)
        {
            macdrv_set_view_hidden(data->client_view, TRUE);
            data->client_view = NULL;
        }
        release_win_data(data);
    }

    return TRUE;
}

/***********************************************************************
 *              macdrv_surface_destroy
 */
static void macdrv_surface_destroy(struct window_surface *window_surface)
{
    struct macdrv_window_surface *surface = get_mac_surface(window_surface);

    TRACE("freeing %p\n", surface);
    if (surface->remote_root)
        NtUserPostMessage(surface->remote_root, WM_MACDRV_RELEASE_GDI_LAYER,
                          (WPARAM)window_surface->hwnd, macdrv_gdi_context_id(surface->remote_context));
    if (surface->remote_context) macdrv_destroy_gdi_context(surface->remote_context);
    if (surface->provider) CGDataProviderRelease(surface->provider);
    shared_gdi_close(&surface->shared);
}

static const struct window_surface_funcs macdrv_surface_funcs =
{
    .size = sizeof(struct macdrv_window_surface),
    .set_clip = macdrv_surface_set_clip,
    .flush = macdrv_surface_flush,
    .destroy = macdrv_surface_destroy,
    .begin_pixel_access = macdrv_surface_begin_pixel_access,
    .end_pixel_access = macdrv_surface_end_pixel_access,
    .is_current = macdrv_surface_is_current,
};

static struct macdrv_window_surface *get_mac_surface(struct window_surface *surface)
{
    if (!surface || surface->funcs != &macdrv_surface_funcs) return NULL;
    return (struct macdrv_window_surface *)surface;
}

struct window_surface *macdrv_GetForeignWindowSurface(HWND hwnd, struct window_surface *previous, BOOL *retry)
{
    struct shared_gdi_mapping shared = {0};
    struct macdrv_window_surface *surface = get_mac_surface(previous);
    struct window_surface *window_surface;
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    HBITMAP bitmap;

    *retry = TRUE;
    if (surface && surface->shared.foreign && previous->hwnd == hwnd &&
        !surface->shared.header->retired && shared_gdi_current(previous))
    {
        window_surface_add_ref(previous);
        return previous;
    }
    if (!shared_gdi_open(&shared, hwnd)) return NULL;
    memset(info, 0, sizeof(*info));
    info->bmiHeader.biSize = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth = shared.width;
    info->bmiHeader.biHeight = -(INT)shared.height;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 32;
    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biSizeImage = (UINT)shared.stride * shared.height;
    if (!(bitmap = create_memory_bitmap(hwnd, info, (BYTE *)shared.header + shared.pixel_offset, TRUE)))
    {
        shared_gdi_close(&shared);
        return NULL;
    }
    if (!(window_surface = window_surface_create_existing(&macdrv_surface_funcs, hwnd, &shared.rect, info, bitmap)))
    {
        NtGdiDeleteObjectApp(bitmap);
        shared_gdi_close(&shared);
        return NULL;
    }
    surface = get_mac_surface(window_surface);
    surface->shared = shared;
    window_surface->shared_id[0] = shared.owner_pid;
    window_surface->shared_id[1] = (ULONG_PTR)hwnd;
    window_surface->shared_id[2] = shared.tag;
    window_surface->flush_immediately = TRUE;
    return window_surface;
}

BOOL macdrv_WindowSurfaceChanged(HWND hwnd, struct window_surface *old_surface, struct window_surface *new_surface)
{
    struct macdrv_window_surface *old = get_mac_surface(old_surface), *next = get_mac_surface(new_surface);
    struct macdrv_win_data *data;
    struct window_surface *previous = NULL;
    BOOL old_retired = TRUE;

    if (old && old->shared.mutant && !old->shared.foreign)
    {
        pthread_mutex_lock(&old_surface->mutex);
        if (shared_gdi_wait(&old->shared))
        {
            old->shared.header->retired = TRUE;
            NtReleaseMutant(old->shared.mutant, NULL);
        }
        else old_retired = FALSE;
        if (HandleToULong(NtUserGetProp(hwnd, shared_gdi_property)) == old->shared.tag)
            NtUserRemoveProp(hwnd, shared_gdi_property);
        pthread_mutex_unlock(&old_surface->mutex);
    }
    if (next && next->shared.mutant && !next->shared.foreign)
    {
        pthread_mutex_lock(&new_surface->mutex);
        if (shared_gdi_wait(&next->shared))
        {
            next->shared.header->retired = FALSE;
            if (NtUserSetProp(hwnd, shared_gdi_property, ULongToHandle(next->shared.tag)))
                next->shared.local_only = FALSE;
            else
                WARN("failed to publish shared GDI surface for %p\n", hwnd);
            NtReleaseMutant(next->shared.mutant, NULL);
        }
        pthread_mutex_unlock(&new_surface->mutex);
    }
    if ((data = get_win_data(hwnd)))
    {
        previous = data->gdi_surface;
        data->gdi_surface = next && next->shared.mutant && !next->shared.foreign ? new_surface : NULL;
        if (data->gdi_surface) window_surface_add_ref(data->gdi_surface);
        release_win_data(data);
    }
    if (previous) window_surface_release(previous);
    return old_retired;
}

void macdrv_PresentSharedWindowSurface(HWND hwnd, ULONG tag)
{
    struct macdrv_win_data *data = get_win_data(hwnd);
    struct macdrv_window_surface *surface;
    struct window_surface *window_surface = NULL;

    if (data)
    {
        if ((window_surface = data->gdi_surface)) window_surface_add_ref(window_surface);
        release_win_data(data);
    }
    if (!window_surface) return;
    surface = get_mac_surface(window_surface);
    if (surface && surface->shared.mutant && surface->shared.tag == tag)
    {
        pthread_mutex_lock(&window_surface->mutex);
        macdrv_shared_present(surface);
        pthread_mutex_unlock(&window_surface->mutex);
    }
    window_surface_release(window_surface);
}

/***********************************************************************
 *              create_surface
 */
static struct window_surface *create_surface(HWND hwnd, WineWindow *window, const RECT *rect, BOOL share)
{
    struct macdrv_window_surface *surface;
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    DWORD window_background;
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    struct window_surface *window_surface;
    CGDataProviderRef provider = NULL;
    struct shared_gdi_mapping shared = {0};
    HBITMAP bitmap = 0;
    void *bits;

    if (width <= 0 || height <= 0 || width > 32768 || height > 32768 ||
        (UINT64)width * height * 4 > UINT32_MAX) return NULL;

    memset(info, 0, sizeof(*info));
    info->bmiHeader.biSize        = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth       = width;
    info->bmiHeader.biHeight      = -height; /* top-down */
    info->bmiHeader.biPlanes      = 1;
    info->bmiHeader.biBitCount    = 32;
    info->bmiHeader.biSizeImage   = get_dib_image_size(info);
    info->bmiHeader.biCompression = BI_RGB;

    if (share && shared_gdi_create(&shared, hwnd, rect))
        bits = (BYTE *)shared.header + shared.header->pixel_offset;
    else
    {
        share = FALSE;
        if (!(provider = data_provider_create(info->bmiHeader.biSizeImage, &bits))) return NULL;
    }
    window_background = macdrv_window_background_color();
    memset_pattern4(bits, &window_background, info->bmiHeader.biSizeImage);

    /* wrap the data in a HBITMAP so we can write to the surface pixels directly */
    bitmap = create_memory_bitmap(hwnd, info, bits, FALSE);
    if (share && !bitmap)
    {
        shared_gdi_close(&shared);
        return create_surface(hwnd, window, rect, FALSE);
    }

    if (!(window_surface = window_surface_create(&macdrv_surface_funcs, hwnd, rect, info, bitmap)))
    {
        if (bitmap) NtGdiDeleteObjectApp(bitmap);
        if (provider) CGDataProviderRelease(provider);
        shared_gdi_close(&shared);
    }
    else
    {
        surface = get_mac_surface(window_surface);
        surface->window = window;
        surface->provider = provider;
        surface->shared = shared;
        if (share)
        {
            window_surface->shared_id[0] = shared.owner_pid;
            window_surface->shared_id[1] = (ULONG_PTR)hwnd;
            window_surface->shared_id[2] = shared.tag;
        }
        if (!window && !(surface->remote_context = macdrv_create_gdi_context(CGRectMake(0, 0, width, height))))
        {
            window_surface_release(window_surface);
            return NULL;
        }
    }

    return window_surface;
}

void macdrv_publish_gdi_surface(struct window_surface *window_surface)
{
    struct macdrv_window_surface *surface = get_mac_surface(window_surface);
    HWND root;
    DWORD pid = 0;

    if (!surface || !surface->remote_context) return;
    root = NtUserGetAncestor(window_surface->hwnd, GA_ROOT);
    if (root) NtUserGetWindowThread(root, &pid);
    if (root == window_surface->hwnd || !pid || pid == GetCurrentProcessId()) root = 0;
    if (surface->remote_root && surface->remote_root != root)
        NtUserPostMessage(surface->remote_root, WM_MACDRV_RELEASE_GDI_LAYER,
                          (WPARAM)window_surface->hwnd, macdrv_gdi_context_id(surface->remote_context));
    surface->remote_root = root;
    if (root)
        NtUserPostMessage(root, WM_MACDRV_CREATE_GDI_LAYER,
                          (WPARAM)window_surface->hwnd, macdrv_gdi_context_id(surface->remote_context));
}


/***********************************************************************
 *              CreateWindowSurface   (MACDRV.@)
 */
BOOL macdrv_CreateWindowSurface(HWND hwnd, BOOL layered, const RECT *surface_rect, struct window_surface **surface)
{
    struct window_surface *previous;
    struct macdrv_window_surface *old_mac_surface;
    struct macdrv_win_data *data;
    BOOL share;

    TRACE("hwnd %p, layered %u, surface_rect %s, surface %p\n", hwnd, layered, wine_dbgstr_rect(surface_rect), surface);

    previous = *surface;
    if (!(data = get_win_data(hwnd))) return TRUE; /* use default surface */
    old_mac_surface = get_mac_surface(previous);
    share = !!data->cocoa_window && !layered && !data->shaped && !data->per_pixel_alpha &&
            !(NtUserGetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED);
    if (old_mac_surface && !!old_mac_surface->remote_context == !data->cocoa_window &&
        !!old_mac_surface->shared.mutant == share)
    {
        release_win_data(data);
        return TRUE;
    }
    if (previous) window_surface_release(previous);

    if (layered)
    {
        data->layered = TRUE;
        data->ulw_layered = TRUE;
    }

    *surface = create_surface(hwnd, data->cocoa_window, surface_rect, share);

    release_win_data(data);
    return TRUE;
}
