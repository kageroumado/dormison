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
#include "sevo_provenance.h"
#include "sevo_stats.h"
#import <Metal/Metal.h>
#include <os/lock.h>

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"


/* The runtime's weak references, which ARC code reaches through __weak; this file is not
   ARC. NSView supports them. */
extern id objc_loadWeakRetained(id *location);
extern id objc_storeWeak(id *location, id obj);

@implementation WineMetalLayer
{
    os_unfair_lock stateLock;
    void* _presenter;
    id _wineView;
}

    - (void) dealloc
    {
        objc_storeWeak(&_wineView, nil);
        [super dealloc];
    }

    - (void*) presenter
    {
        void* value;

        os_unfair_lock_lock(&stateLock);
        value = _presenter;
        os_unfair_lock_unlock(&stateLock);
        return value;
    }

    - (void) setPresenter:(void*)value
    {
        os_unfair_lock_lock(&stateLock);
        _presenter = value;
        os_unfair_lock_unlock(&stateLock);
    }

    - (NSView*) wineView
    {
        return [objc_loadWeakRetained(&_wineView) autorelease];
    }

    - (void) setWineView:(NSView*)view
    {
        objc_storeWeak(&_wineView, view);
    }

    /* D3DMetal draws into this layer and never tells the driver, so the only
     * moment winemac can learn a frame is ready is when a drawable is handed
     * out. Extending nextDrawable posts the event that makes
     * client_surface_present() run for the matching client_surface.
     *
     * It runs on the renderer's thread while the main thread can take the view and its
     * presenter away: the view comes through a weak reference and the presenter with a
     * reference of this call's own, taken under the lock the view's dealloc clears it
     * under. */
    - (id<CAMetalDrawable>) nextDrawable
    {
        NSView* owner = [objc_loadWeakRetained(&_wineView) autorelease];
        void* presenter;
        id<CAMetalDrawable> drawable;

        /* CAMetalLayer's delegate is the WineMetalView holding it; that view's
         * superview is the client_surface's WineContentView, which carries the
         * client_surface pointer. */
        if (!owner) owner = (NSView*)self.delegate;
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
                    sevo_provenance_note_drawable();
                    sevo_stats_note_window((unsigned long long)view.window.windowNumber);
                }
            }
        }

        sevo_stats_note_drawable();

        os_unfair_lock_lock(&stateLock);
        presenter = _presenter;
        if (presenter) sevo_presenter_retain(presenter);
        os_unfair_lock_unlock(&stateLock);

        /* With the presenter the drawable is one of its textures: this
           layer is off screen, so its own drawables would never be shown. */
        if (presenter)
        {
            drawable = (id<CAMetalDrawable>)sevo_presenter_next_drawable(presenter);
            sevo_presenter_release(presenter);
            return drawable;
        }

        return [super nextDrawable];
    }

@end

#endif
