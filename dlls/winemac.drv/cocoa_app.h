/*
 * MACDRV Cocoa application class declaration
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

#include "macdrv_cocoa.h"

#define ERR(...) do { if (macdrv_err_on) LogError(__func__, __VA_ARGS__); } while (false)

/* Internal notification sent on NSApp for display configuration changes. The
   userInfo contains the two keys, NSNumbers for the effected CGDirectDisplayID
   and the CGDisplayChangeSummaryFlags from the underlying CG callback. */
static NSString* const WineDisplayConfigurationChangedNotification = @"WineDisplayConfigurationChanged";
static NSString* const WineDisplayConfigurationNotificationDisplayIDKey = @"DisplayID";
static NSString* const WineDisplayConfigurationNotificationFlagsKey = @"Flags";

enum {
    WineApplicationEventWakeQuery,
};


@class WineEventQueue;
@class WineWindow;
@protocol WineClipCursorHandler;


@interface WineApplicationController : NSObject <NSApplicationDelegate>
{
    NSAlert* unansweredAlert;   /* the "not responding" sheet, while it is up */
    CFRunLoopSourceRef requestSource;
    NSMutableArray* requests;
    dispatch_queue_t requestsManipQueue;

    NSMutableArray* eventQueues;
    NSLock*         eventQueuesLock;

    NSTimeInterval eventTimeAdjustment;

    NSMutableArray* keyWindows;
    NSMutableSet* triedWindows;
    unsigned long windowFocusSerial;

    TISInputSourceRef lastKeyboardInputSource;
    TISInputSourceRef lastKeyboardLayoutInputSource;
    CGEventSourceKeyboardType keyboardType;
    NSEvent* lastFlagsChanged;
    BOOL inputSourceIsInputMethod;
    uint32_t pressedKeyCodes[128 / 32];

    CGFloat primaryScreenHeight;
    BOOL primaryScreenHeightValid;
    NSMutableData* screenFrameCGRects;

    WineWindow* lastTargetWindow;
    WineWindow* mouseCaptureWindow;
    BOOL forceNextMouseMoveAbsolute;
    double mouseMoveDeltaX, mouseMoveDeltaY;
    NSUInteger unmatchedMouseDowns;

    NSTimeInterval lastScrollTime;
    double accumScrollX, accumScrollY;

    NSMutableDictionary* originalDisplayModes;
    NSMutableDictionary* latentDisplayModes;
    BOOL displaysCapturedForFullscreen;

    NSArray*    cursorFrames;
    int         cursorFrame;
    NSTimer*    cursorTimer;
    NSCursor*   cursor;
    BOOL        cursorIsCurrent;
    BOOL        cursorHidden;
    /* A clip a game asked for while its window is shown in a window and the
       cursor is visible: held until the cursor hides. */
    CGRect      deferredClipRect;
    BOOL        hasDeferredClip;
    /* The game clipped the cursor to the whole screen its fullscreen window
       covers. Held like a windowed clip while the cursor is visible, and
       applied inset from the edges while it is hidden, where the menu bar
       and the Dock would otherwise reveal. */
    BOOL        screenClip;
    CGRect      screenClipRect;
    BOOL        clientWantsCursorHidden;

    NSTimeInterval lastSetCursorPositionTime;

    id<WineClipCursorHandler> clipCursorHandler;

    NSImage* applicationIcon;

    BOOL beenActive;

    NSMutableSet* windowsBeingDragged;
}

@property (nonatomic) CGEventSourceKeyboardType keyboardType;
@property (readonly, copy, nonatomic) NSEvent* lastFlagsChanged;
@property (readonly, nonatomic) BOOL areDisplaysCaptured;

@property (readonly) BOOL clippingCursor;
@property (nonatomic) NSTimeInterval lastSetCursorPositionTime;

    + (WineApplicationController*) sharedController;

    - (void) transformProcessToForeground:(BOOL)activateIfTransformed;
    - (void) tryToActivateIgnoringOtherApps:(BOOL)ignore;

    - (BOOL) registerEventQueue:(WineEventQueue*)queue;
    - (void) unregisterEventQueue:(WineEventQueue*)queue;

    - (void) computeEventTimeAdjustmentFromTicks:(unsigned long long)tickcount uptime:(uint64_t)uptime_ns;
    - (double) ticksForEventTime:(NSTimeInterval)eventTime;

    - (void) windowGotFocus:(WineWindow*)window;
    - (void) watchRequest:(macdrv_event*)event forWindow:(WineWindow*)window;

    - (void) applyDeferredClip;

    - (BOOL) waitUntilQueryDone:(bool*)done timeout:(NSDate*)timeout processEvents:(BOOL)processEvents;

    - (void) noteKey:(uint16_t)keyCode pressed:(BOOL)pressed;

    - (void) window:(WineWindow*)window isBeingDragged:(BOOL)dragged;
    - (void) windowWillOrderOut:(WineWindow*)window;

    - (void) flipRect:(NSRect*)rect;
    - (NSPoint) flippedMouseLocation:(NSPoint)point;

    /* Presentation-scaled windows draw Wine's coordinates somewhere else on
       screen; these move a point or rect between the two spaces, both in
       top-left-origin screen points. */
    - (CGPoint) screenPointFromWinePoint:(CGPoint)point;
    - (CGRect) screenRectFromWineRect:(CGRect)rect;
    - (CGPoint) winePointFromScreenPoint:(CGPoint)point;

    - (WineWindow*) frontWineWindow;
    - (void) adjustWindowLevels;
    - (void) updateFullscreenWindows;
    - (void) updatePresentationOptions;

    - (BOOL) handleEvent:(NSEvent*)anEvent;
    - (void) didSendEvent:(NSEvent*)anEvent;

@end


@interface WineApplication : NSApplication
{
    WineApplicationController* wineController;
}

@property (readwrite, assign, nonatomic) WineApplicationController* wineController;

@end


void OnMainThreadAsync(dispatch_block_t block);

void LogError(const char* func, NSString* format, ...);
void LogErrorv(const char* func, NSString* format, va_list args);
