/*
 * The presenter's C surface: what the driver calls into swift/Presenter.swift.
 *
 * Copyright 2026 kageroumado
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

#ifndef __WINE_SEVO_PRESENTER_H
#define __WINE_SEVO_PRESENTER_H

#include <stddef.h>

/* Brings the presenter up for the process. `upscaler` is the Upscaler option
   (off, lanczos, metalfx, or a shader package name), `final_filter` the
   FinalFilter option (nearest, bilinear, lanczos), `shader_dirs` the
   colon-separated package search path or NULL, `trace` whether frames are
   traced to stderr, `debug` the PresenterDebug option ("clear" paints the
   drawable red instead of the frame). Returns 1 when frames can be presented,
   0 when the driver is to behave as if the option were off. */
extern int sevo_presenter_init(const char *upscaler, const char *final_filter, const char *shader_dirs,
                               int trace, const char *debug);

/* Takes over one Metal view. `renderer_layer` is the CAMetalLayer the
   renderer draws into, which stays off screen; the returned handle owns the
   on-screen layer. NULL when the presenter is not running. */
extern void *sevo_presenter_attach(void *renderer_layer);
/* Takes over one GDI window surface. `bits` is the DIB the program draws
   into (BGRA, top-down, `stride` bytes per row, `width` x `height` pixels),
   in an allocation of `size` bytes. The presenter reads the DIB only inside
   sevo_presenter_surface_flush, and calls `release(context)` once the handle
   is gone. The returned handle owns the on-screen layer; NULL when the
   presenter is not running, in which case `release` is never called. */
extern void *sevo_presenter_attach_surface(void *bits, size_t size, int stride, int width, int height,
                                           void (*release)(void *context), void *context);
/* The program finished drawing this rectangle of the DIB, in DIB pixels.
   Called with the surface lock held: the rectangle is copied out of the DIB
   before this returns. Returns 1 when it was taken, 0 when the presenter's
   staging is all in flight and the caller keeps the rectangle dirty for
   its next flush. */
extern int sevo_presenter_surface_flush(void *presenter, int left, int top, int right, int bottom);
/* The window wants the surface on screen again: presenting the last frame
   afresh gives a layer the system emptied behind a covered window its
   picture back. */
extern void sevo_presenter_surface_refresh(void *presenter);
extern void *sevo_presenter_onscreen_layer(void *presenter);       /* CAMetalLayer*, unretained */
/* The view's geometry: device pixels per point (the presentation scale
   included), the source's pixels per point (the renderer layer's
   contentsScale, or the DIB's density), and the bounds in points. */
extern void sevo_presenter_layout(void *presenter, double device_scale, double renderer_scale,
                                  double width, double height);
/* id<CAMetalDrawable>, autoreleased, or NULL; NULL for a surface's presenter. */
extern void *sevo_presenter_next_drawable(void *presenter);
/* Lets the handle go. A surface's presenter stops its display link here,
   on the calling thread, before the handle is released. */
extern void sevo_presenter_detach(void *presenter);

#endif
