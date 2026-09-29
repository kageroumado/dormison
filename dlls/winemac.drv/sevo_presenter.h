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
   drawable red instead of the frame), `engine` the engine's build name for
   the readout, or NULL for SEVO_ENGINE_NAME. Returns 1 when frames can be
   presented, 0 when the driver is to behave as if the option were off. */
extern int sevo_presenter_init(const char *upscaler, const char *final_filter, const char *shader_dirs,
                               int trace, const char *debug, const char *engine);

/* Takes over one Metal view. `renderer_layer` is the CAMetalLayer the
   renderer draws into, which stays off screen; the returned handle owns the
   on-screen layer. NULL when the presenter is not running. */
extern void *sevo_presenter_attach(void *renderer_layer);
/* Takes over one GDI window surface. `bits` is the DIB the program draws
   into (BGRA, top-down, `stride` bytes per row, `width` x `height` pixels),
   in an allocation of `size` bytes. The presenter reads the DIB inside
   sevo_presenter_surface_flush and when a refresh fills a frame a layout made
   new, and calls `release(context)` once the handle is gone. The returned handle owns the on-screen layer; NULL when the
   presenter is not running, in which case `release` is never called. */
extern void *sevo_presenter_attach_surface(void *bits, size_t size, int stride, int width, int height,
                                           void (*release)(void *context), void *context);
/* The program finished drawing this rectangle of the DIB, in DIB pixels.
   Called with the surface lock held: the rectangle is copied out of the DIB
   before this returns. Returns 1 when it was taken, 0 when the presenter's
   staging is all in flight and the caller keeps the rectangle dirty for
   its next flush. */
extern int sevo_presenter_surface_flush(void *presenter, int left, int top, int right, int bottom);
/* The window wants its Metal, OpenGL, or surface picture on screen
   again: presenting the last frame afresh gives a layer the system emptied
   behind a covered window its picture back. */
extern void sevo_presenter_refresh(void *presenter);

/* Takes over one OpenGL window drawable. The driver draws each frame into an
   IOSurface of the presenter's ring, top row first, and the presenter puts it
   on screen. The returned handle owns the on-screen layer; NULL when the
   presenter is not running. */
extern void *sevo_presenter_attach_gl(void);
/* A new ring of SEVO_GL_RING BGRA surfaces of this size. The driver deletes
   its textures of the old ring first. Returns 1 when the ring exists. */
#define SEVO_GL_RING 4
extern int sevo_presenter_gl_resize(void *presenter, int width, int height);
extern void *sevo_presenter_gl_surface(void *presenter, int index);  /* IOSurfaceRef, unretained */
/* The slot to draw the next frame into, or -1 when every surface is still
   being read and the frame is to be dropped. An acquired slot ends in
   sevo_presenter_gl_present, after glFlush, or sevo_presenter_gl_abandon.
   A present with `synced` waits for the display as a swap interval does. */
extern int sevo_presenter_gl_acquire(void *presenter);
extern void sevo_presenter_gl_present(void *presenter, int index, int synced);
extern void sevo_presenter_gl_abandon(void *presenter, int index);
extern void *sevo_presenter_onscreen_layer(void *presenter);       /* CAMetalLayer*, unretained */
/* The view's geometry: device pixels per point (the presentation scale
   included), the source's pixels per point (the renderer layer's
   contentsScale, or the DIB's density), and the bounds in points. */
extern void sevo_presenter_layout(void *presenter, double device_scale, double renderer_scale,
                                  double width, double height);
/* Holds output resolution through a live drag; ending it applies the latest
   layout and redraws the retained picture. Main thread. */
extern void sevo_presenter_set_live_resize(void *presenter, int resizing);
/* id<CAMetalDrawable>, autoreleased, or NULL; NULL for a surface's presenter. */
extern void *sevo_presenter_next_drawable(void *presenter);
/* Lets the handle go. A surface's presenter stops its display link here,
   on the calling thread, before the handle is released. */
extern void sevo_presenter_detach(void *presenter);
/* One more reference to the handle and its release: for a thread that uses a handle another
   thread may detach meanwhile. */
extern void sevo_presenter_retain(void *presenter);
extern void sevo_presenter_release(void *presenter);

/* The View menu's side. The upscaler and the final filter change for every presented view
   from its next frame; NULL leaves one as it is. */
extern void sevo_presenter_set_options(const char *upscaler, const char *final_filter);
/* The readout over the picture: engine, sizes, upscaler, filter, frames a second. */
extern void sevo_presenter_set_readout(int shown);
/* The shader packages that can be chosen, newline-separated; the caller frees it. */
extern char *sevo_presenter_package_names(void);

/* The archive's build id, `sevo:winemacswift=<16 hex>`: the sha256 prefix of
   its sources (swift/Makefile). Printed in the `sevo:run` line, and what
   build-macos/package-engine.sh pairs a staged winemac.so with. */
extern const char *sevo_winemacswift_build_id(void);

#endif
