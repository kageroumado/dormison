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

#include "config.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include "macdrv_cocoa.h"
#import "cocoa_app.h"
#import "cocoa_event.h"
#import "cocoa_window.h"
#import "d3dmetal_objc.h"
#include "sevo_presenter.h"
#import <Metal/Metal.h>

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"


@implementation WineMetalLayer

    @synthesize presenter, wineView;

    /* D3DMetal draws into this layer and never tells the driver, so the only
     * moment winemac can learn a frame is ready is when a drawable is handed
     * out. Extending nextDrawable posts the event that makes
     * client_surface_present() run for the matching client_surface. */
    - (id<CAMetalDrawable>) nextDrawable
    {
        /* CAMetalLayer's delegate is the WineMetalView holding it; that view's
         * superview is the client_surface's WineContentView, which carries the
         * client_surface pointer. */
        NSView* owner = self.wineView ? self.wineView : (NSView*)self.delegate;
        if ([owner isKindOfClass:NSClassFromString(@"WineMetalView")])
        {
            NSView* view = owner;
            if ([view.superview isKindOfClass:NSClassFromString(@"WineContentView")] &&
                [view.window    isKindOfClass:NSClassFromString(@"WineWindow")])
            {
                void *client_surface = macdrv_get_view_d3dmetal_client_surface((macdrv_view)view.superview);
                if (client_surface)
                {
                    macdrv_event* event;
                    event = macdrv_create_event(CLIENT_SURFACE_PRESENTED, (WineWindow*)view.window);
                    event->client_surface_presented.client_surface = client_surface;

                    WineEventQueue *queue = [(WineWindow*)view.window queue];
                    [queue postEvent:event];
                    macdrv_release_event(event);
                }
            }
        }

        /* With the presenter the drawable is one of its textures: this
           layer is off screen, so its own drawables would never be shown. */
        if (self.presenter)
            return (id<CAMetalDrawable>)sevo_presenter_next_drawable(self.presenter);

        return [super nextDrawable];
    }

@end

#endif
