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
#include <sys/mman.h>

#include "macdrv.h"
#include "winuser.h"

WINE_DEFAULT_DEBUG_CHANNEL(bitblt);

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
    macdrv_window           window;
    CGDataProviderRef       provider;
    /* The presenter drawing this surface, or NULL when a CGImage on the
       content view's layer does: every surface with the presenter off, and
       layered, shaped or alpha-blended surfaces always. */
    void                   *presenter;
    BOOL                    presenter_drawn;
};

static struct macdrv_window_surface *get_mac_surface(struct window_surface *surface);

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

/* The DIB of a surface the presenter reads, shared by the CGDataProvider
   and the presenter's Metal buffer: it is unmapped when the last of the two
   lets go, in whichever order they finish. The surface is destroyed on the
   program's thread while a blit the presenter encoded may still be reading
   the pages. */
struct page_bits
{
    void   *memory;
    size_t  size;
    int     refs;
};

static void page_bits_release(void *info, const void *data, size_t size)
{
    struct page_bits *bits = info;

    if (__atomic_sub_fetch(&bits->refs, 1, __ATOMIC_ACQ_REL)) return;
    munmap(bits->memory, bits->size);
    free(bits);
}

static void page_bits_release_presenter(void *info)
{
    page_bits_release(info, NULL, 0);
}

/* The DIB of a surface the presenter reads: page-aligned and a whole number
   of pages, as Metal requires of memory it wraps in a buffer without
   copying. The page is the host's 16 KB one, whatever page size this
   process is told it has. The provider holds one reference on the pages. */
static CGDataProviderRef page_data_provider_create(size_t size, void **bits, struct page_bits **pages)
{
    const size_t page = 16384;
    size_t rounded = (size + page - 1) & ~(page - 1);
    CGDataProviderRef provider;
    struct page_bits *holder;
    void *memory;

    if (!(holder = malloc(sizeof(*holder)))) return NULL;
    memory = mmap(NULL, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED)
    {
        free(holder);
        return NULL;
    }
    holder->memory = memory;
    holder->size = rounded;
    holder->refs = 1;
    if (!(provider = CGDataProviderCreateWithData(holder, memory, rounded, page_bits_release)))
    {
        munmap(memory, rounded);
        free(holder);
        return NULL;
    }
    *bits = memory;
    *pages = holder;
    return provider;
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
    BOOL flushed = TRUE;

    /* The presenter draws opaque rectangles: a shape or per-pixel alpha
       hands the surface to the CGImage path for the rest of its life. */
    if (surface->presenter && ((shape_changed && shape_bits) || window_surface->alpha_mask))
    {
        TRACE("surface %p handed back from the presenter: %s\n", surface, shape_bits ? "shaped" : "alpha");
        macdrv_window_detach_surface(surface->window, surface->presenter);
        surface->presenter = NULL;
    }

    if (surface->presenter)
    {
        /* Refused when the presenter's staging is all in flight: win32u then
           keeps the dirty bounds and hands them to the next flush. */
        flushed = sevo_presenter_surface_flush(surface->presenter, dirty->left, dirty->top, dirty->right, dirty->bottom);
        if (flushed && !surface->presenter_drawn)
        {
            surface->presenter_drawn = TRUE;
            macdrv_window_surface_drawn(surface->window);
        }
    }
    else
    {
        colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        image = CGImageCreate(color_info->bmiHeader.biWidth, abs(color_info->bmiHeader.biHeight), 8, 32,
                              color_info->bmiHeader.biSizeImage / abs(color_info->bmiHeader.biHeight), colorspace,
                              alpha_info | kCGBitmapByteOrder32Little, surface->provider, NULL, retina_on, kCGRenderingIntentDefault);
        CGColorSpaceRelease(colorspace);

        macdrv_window_set_color_image(surface->window, image, cgrect_from_rect(*rect), cgrect_from_rect(*dirty));
        CGImageRelease(image);
    }

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

            if (!(provider = data_provider_create(shape_info->bmiHeader.biSizeImage, (void **)&dst))) return flushed;
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

    return flushed;
}

/***********************************************************************
 *              macdrv_surface_destroy
 */
static void macdrv_surface_destroy(struct window_surface *window_surface)
{
    struct macdrv_window_surface *surface = get_mac_surface(window_surface);

    TRACE("freeing %p\n", surface);
    if (surface->presenter) macdrv_window_detach_surface(surface->window, surface->presenter);
    CGDataProviderRelease(surface->provider);
}

static const struct window_surface_funcs macdrv_surface_funcs =
{
    macdrv_surface_set_clip,
    macdrv_surface_flush,
    macdrv_surface_destroy,
};

static struct macdrv_window_surface *get_mac_surface(struct window_surface *surface)
{
    if (!surface || surface->funcs != &macdrv_surface_funcs) return NULL;
    return (struct macdrv_window_surface *)surface;
}

/***********************************************************************
 *              create_surface
 */
static struct window_surface *create_surface(HWND hwnd, macdrv_window window, const RECT *rect, BOOL layered)
{
    struct macdrv_window_surface *surface;
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    DWORD window_background;
    D3DKMT_CREATEDCFROMMEMORY desc = {.Format = D3DDDIFMT_A8R8G8B8};
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    struct window_surface *window_surface;
    CGDataProviderRef provider;
    HBITMAP bitmap = 0;
    UINT status;
    void *bits;
    void *presenter = NULL;
    struct page_bits *pages = NULL;

    memset(info, 0, sizeof(*info));
    info->bmiHeader.biSize        = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth       = width;
    info->bmiHeader.biHeight      = -height; /* top-down */
    info->bmiHeader.biPlanes      = 1;
    info->bmiHeader.biBitCount    = 32;
    info->bmiHeader.biSizeImage   = get_dib_image_size(info);
    info->bmiHeader.biCompression = BI_RGB;

    /* With the presenter on, an opaque surface of a window is drawn by it
       from a DIB Metal can wrap; the CGImage path keeps the CFData one. */
    if (presenter_on && !layered && window)
    {
        if (!(provider = page_data_provider_create(info->bmiHeader.biSizeImage, &bits, &pages))) return NULL;
        __atomic_add_fetch(&pages->refs, 1, __ATOMIC_ACQ_REL);
        presenter = sevo_presenter_attach_surface(bits, pages->size, get_dib_stride(width, 32), width, height,
                                                  page_bits_release_presenter, pages);
        if (!presenter) page_bits_release_presenter(pages);
        TRACE("surface %dx%d for %p: presenter %p\n", width, height, hwnd, presenter);
    }
    else if (!(provider = data_provider_create(info->bmiHeader.biSizeImage, &bits))) return NULL;
    window_background = macdrv_window_background_color();
    memset_pattern4(bits, &window_background, info->bmiHeader.biSizeImage);

    /* wrap the data in a HBITMAP so we can write to the surface pixels directly */
    desc.Width = info->bmiHeader.biWidth;
    desc.Height = abs(info->bmiHeader.biHeight);
    desc.Pitch = info->bmiHeader.biSizeImage / abs(info->bmiHeader.biHeight);
    desc.pMemory = bits;
    desc.hDeviceDc = NtUserGetDCEx(hwnd, 0, DCX_CACHE | DCX_WINDOW);
    if ((status = NtGdiDdDDICreateDCFromMemory(&desc)))
        ERR("Failed to create HBITMAP, status %#x\n", status);
    else
    {
        bitmap = desc.hBitmap;
        NtGdiDeleteObjectApp(desc.hDc);
    }
    if (desc.hDeviceDc) NtUserReleaseDC(hwnd, desc.hDeviceDc);

    if (!(window_surface = window_surface_create(sizeof(*surface), &macdrv_surface_funcs, hwnd, rect, info, bitmap)))
    {
        if (bitmap) NtGdiDeleteObjectApp(bitmap);
        if (presenter) sevo_presenter_detach(presenter);
        CGDataProviderRelease(provider);
    }
    else
    {
        surface = get_mac_surface(window_surface);
        surface->window = window;
        surface->provider = provider;
        surface->presenter = presenter;
        surface->presenter_drawn = FALSE;
        if (presenter) macdrv_window_attach_surface(window, presenter);
    }

    return window_surface;
}


/***********************************************************************
 *              CreateWindowSurface   (MACDRV.@)
 */
BOOL macdrv_CreateWindowSurface(HWND hwnd, BOOL layered, const RECT *surface_rect, struct window_surface **surface)
{
    struct window_surface *previous;
    struct macdrv_win_data *data;

    TRACE("hwnd %p, layered %u, surface_rect %s, surface %p\n", hwnd, layered, wine_dbgstr_rect(surface_rect), surface);

    if ((previous = *surface) && previous->funcs == &macdrv_surface_funcs) return TRUE;
    if (!(data = get_win_data(hwnd))) return TRUE; /* use default surface */
    if (previous) window_surface_release(previous);

    if (layered)
    {
        data->layered = TRUE;
        data->ulw_layered = TRUE;
    }

    *surface = create_surface(hwnd, data->cocoa_window, surface_rect, layered);

    release_win_data(data);
    return TRUE;
}
