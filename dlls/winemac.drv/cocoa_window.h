/*
 * MACDRV Cocoa window declarations
 *
 * Copyright 2011, 2012, 2013 Ken Thomases for CodeWeavers Inc.
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

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include "macdrv_cocoa.h"


@class WineEventQueue;
@class WineContentView;


@interface WineWindow : NSPanel <NSWindowDelegate>
{
    BOOL disabled;
    BOOL noForeground;
    BOOL preventsAppActivation;
    BOOL floating;
    BOOL resizable;
    BOOL maximized;
    BOOL fullscreen;
    BOOL pendingMinimize;
    BOOL pendingOrderOut;
    BOOL savedVisibleState;
    BOOL drawnSinceShown;
    BOOL closing;
    WineWindow* latentParentWindow;
    NSMutableArray* latentChildWindows;

    void* hwnd;
    WineEventQueue* queue;

    CGDirectDisplayID _lastDisplayID;
    NSTimeInterval _lastDisplayTime;

    NSRect wineFrame;
    NSRect roundedWineFrame;

    BOOL shapeChangedSinceLastDraw;

    BOOL usePerPixelAlpha;

    NSUInteger lastModifierFlags;

    NSRect frameAtResizeStart;
    BOOL resizingFromLeft, resizingFromTop;

    void* himc;
    BOOL commandDone;

    NSSize savedContentMinSize;
    NSSize savedContentMaxSize;

    BOOL enteringFullScreen;
    BOOL exitingFullScreen;
    NSRect nonFullscreenFrame;
    NSTimeInterval enteredFullScreenTime;

    int draggingPhase;
    NSPoint dragStartPosition;
    NSPoint dragWindowStartPosition;

    NSTimeInterval lastDockIconSnapshot;

    BOOL allowKeyRepeats;

    BOOL ignore_windowDeminiaturize;
    BOOL ignore_windowResize;
    BOOL fakingClose;

    CAShapeLayer* contentViewMaskLayer;

    /* Presentation scaling. The window's content view is a stage; Wine's own
       content view sits in it, scaled to fit whenever the real frame and the
       frame Wine believes in (wineFrame) differ in size. */
    BOOL presentationScalable;
    /* The program calls its window resizable and then puts back the size it had: seen once,
       the window is presented through the scaler from then on. */
    BOOL programRefusesResize;
    NSTimeInterval liveResizeEndTime;
    /* A borderless window covering a screen, shown in a titled window at a
       smaller size (RESIZABLE_WINDOWS_WINDOW). Placed once, at the default
       windowed frame; after that the real frame is the user's alone. */
    BOOL presentationWindowed;
    BOOL presentationPlaced;
    struct macdrv_window_features presentationFeatures;
    WineContentView* wineContentView;
    /* The content rect Wine last asked for. wineFrame is derived from it for
       the style mask of the moment, since a title bar arriving after the
       window is made changes the frame of the same content. */
    NSRect wineContentRect;
}

@property (retain, readonly, nonatomic) WineEventQueue* queue;
@property (readonly, nonatomic) BOOL disabled;
@property (readonly, nonatomic) BOOL noForeground;
@property (readonly, nonatomic) BOOL preventsAppActivation;
@property (readonly, nonatomic) BOOL floating;
@property (readonly, getter=isFullscreen, nonatomic) BOOL fullscreen;
@property (readonly, getter=isFakingClose, nonatomic) BOOL fakingClose;
@property (readonly, nonatomic) NSRect wine_fractionalFrame;

/* Wine's own content view: what every surface, client view and layer host
   lives in. The window's contentView is the stage around it. */
@property (readonly, nonatomic) WineContentView* wineContentView;
/* Whether the real content size differs from the size Wine draws at. */
@property (readonly, nonatomic) BOOL presentationScaled;
@property (readonly, nonatomic) BOOL presentationWindowed;
/* Real points per point of Wine's content; 1 unless scaled. */
@property (readonly, nonatomic) CGFloat presentationScale;

/* Whether this window, when ordered in and not miniaturized, would appear to
   the user on-screen. That means it has a non-zero size and is not empty-
   shaped, or has a child window that meets those criteria. */
@property (readonly, nonatomic) BOOL presentsVisibleContent;

    - (NSInteger) minimumLevelForActive:(BOOL)active;
    - (void) updateFullscreen;

    - (void) postKeyEvent:(NSEvent *)theEvent;
    - (void) postBroughtForwardEvent;

    - (WineWindow*) ancestorWineWindow;

    - (void) updateForCursorClipping;

    - (void) setRetinaMode:(BOOL)mode;

    - (void) layoutPresentation;
    /* Applies the window's features again under the current resizable_windows,
       after the View menu changed it. */
    - (void) reapplyResizableWindows;
    /* Both points in the top-left-origin screen space the controller's
       mouse handlers work in. */
    - (CGPoint) winePointFromScreenPoint:(CGPoint)point;
    - (CGPoint) screenPointFromWinePoint:(CGPoint)point;
    - (BOOL) wineContentContainsScreenPoint:(CGPoint)point;

@end
