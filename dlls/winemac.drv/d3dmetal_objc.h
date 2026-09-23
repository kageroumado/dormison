/*
 * Mac graphics driver hooks used by D3DMetal (part of the Apple Game Porting Toolkit)
 *
 * Copyright 2025 Brendan Shanks for CodeWeavers, Inc.
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

#if defined(__x86_64__)

#import <QuartzCore/QuartzCore.h>

@interface WineMetalLayer : CAMetalLayer

/* The presenter this layer's frames go to (sevo_presenter_attach), or NULL
   when the layer is on screen itself. Set by the view that made the layer on the main
   thread, read by the renderer's thread in nextDrawable: both under the layer's lock. */
@property (nonatomic, assign) void* presenter;
/* The Metal view this layer draws for. A layer that is the view's own
   backing layer has the view as its delegate; the presenter's renderer
   layer is not in the layer tree and has no delegate, so the view is named
   here for the presented event that unhides the client surface. */
@property (nonatomic, assign) NSView* wineView;  /* held weakly */

@end

#endif
