/*
 * MACDRV Cocoa application class
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

#import "cocoa_app.h"
#import "cocoa_cursorclipping.h"
#import "cocoa_event.h"
#import "cocoa_window.h"
#include "sevo_presenter.h"
#include "sevo_stats.h"

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"


static NSString* const WineAppWaitQueryResponseMode = @"WineAppWaitQueryResponseMode";

// Private notifications that are reliably dispatched when a window is moved by dragging its titlebar.
// The object of the notification is the window being dragged.
// Available in macOS 10.12+
static NSString* const NSWindowWillStartDraggingNotification = @"NSWindowWillStartDraggingNotification";
static NSString* const NSWindowDidEndDraggingNotification = @"NSWindowDidEndDraggingNotification";

// Internal distributed notification to handle cooperative app activation in Sonoma.
static NSString* const WineAppWillActivateNotification = @"WineAppWillActivateNotification";
static NSString* const WineActivatingAppPIDKey = @"ActivatingAppPID";
static NSString* const WineActivatingAppPrefixKey = @"ActivatingAppPrefix";
static NSString* const WineActivatingAppConfigDirKey = @"ActivatingAppConfigDir";


bool macdrv_err_on;


#if !defined(MAC_OS_VERSION_14_0) || MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_VERSION_14_0
@interface NSApplication (CooperativeActivationSelectorsForOldSDKs)

    - (void)activate;
    - (void)yieldActivationToApplication:(NSRunningApplication *)application;
    - (void)yieldActivationToApplicationWithBundleIdentifier:(NSString *)bundleIdentifier;

@end

@interface NSRunningApplication (CooperativeActivationSelectorsForOldSDKs)

    - (BOOL)activateFromApplication:(NSRunningApplication *)application
                            options:(NSApplicationActivationOptions)options;

@end
#endif


/***********************************************************************
 *              WineLocalizedString
 *
 * Look up a localized string by its ID in the dictionary.
 */
static NSString* WineLocalizedString(unsigned int stringID)
{
    return ((NSDictionary*)localized_strings)[@(stringID)];
}


@implementation WineApplication

@synthesize wineController;

    - (void) sendEvent:(NSEvent*)anEvent
    {
        if (![wineController handleEvent:anEvent])
        {
            [super sendEvent:anEvent];
            [wineController didSendEvent:anEvent];
        }
    }

    - (void) setWineController:(WineApplicationController*)newController
    {
        wineController = newController;
        [self setDelegate:wineController];
    }

@end


@interface WineApplicationController ()

@property (readwrite, copy, nonatomic) NSEvent* lastFlagsChanged;
@property (copy, nonatomic) NSArray* cursorFrames;
@property (retain, nonatomic) NSTimer* cursorTimer;
@property (retain, nonatomic) NSCursor* cursor;
@property (retain, nonatomic) NSImage* applicationIcon;
@property (readonly, nonatomic) BOOL inputSourceIsInputMethod;
@property (retain, nonatomic) WineWindow* mouseCaptureWindow;

    - (void) setupObservations;
    - (void) applicationDidBecomeActive:(NSNotification *)notification;

    static void PerformRequest(void *info);

@end


/* Whether the View menu has the presenter's readout switched on. */
static BOOL sevoReadoutShown;


/* The frame-rate counter: a small capsule at the top right of the window the
   game presents into, in a child window of it, so it sits over a Metal layer,
   an OpenGL surface and a GDI picture alike and goes where the window goes.
   It reads the process's own present counter twice a second. With the graph
   on it becomes a card: the number, the 1 % low and the slowest frame of the
   last ten seconds, and every frame of the last five seconds drawn at its
   frame time, read from the stats page's ring ten times a second. */
@interface WineFrameTimeGraph : NSView
{
    /* Frame times in ms and when each frame ended, in seconds before now. */
    float times[SEVO_STATS_RING_CAPACITY];
    float ages[SEVO_STATS_RING_CAPACITY];
    unsigned int count;
    float top;
}
    - (void) setTimes:(const float*)frameTimes ages:(const float*)frameAges count:(unsigned int)n top:(float)ceiling;
@end

@interface WineFrameRateCounter : NSObject
{
    NSPanel* panel;
    NSTextField* label;
    NSTextField* detail;
    WineFrameTimeGraph* graph;
    NSTimer* timer;
    NSWindow* host;
    unsigned long long lastFrames;
    CFAbsoluteTime lastTime;
    BOOL graphShown;
    unsigned int ticks;
}
    - (void) show;
    - (void) hide;
    - (void) setGraphShown:(BOOL)shown;
@end

static const CGFloat kFrameRateInset = 12;
static const CGFloat kFrameRateHeight = 22;
static const NSTimeInterval kFrameRateInterval = 0.5;
/* The graph's card: the number's row, the detail row, the plot. */
static const CGFloat kFrameGraphWidth = 220;
static const CGFloat kFrameGraphPlotHeight = 48;
static const CGFloat kFrameGraphHeight = 22 + 16 + 48 + 10;
static const NSTimeInterval kFrameGraphInterval = 0.1;
/* Seconds of frames the plot spans, and the window the 1 % low is taken over. */
static const float kFrameGraphSpan = 5;
static const float kFrameStatsSpan = 10;
/* The plot's ceiling is at least a 30 fps frame, so a steady 60 sits a little above the
   middle and a hitch reaches for the top. */
static const float kFrameGraphMinimumTop = 1000.0f / 30;

static int compare_floats(const void* a, const void* b)
{
    float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}

@implementation WineFrameTimeGraph

    - (BOOL) isOpaque { return NO; }

    - (void) setTimes:(const float*)frameTimes ages:(const float*)frameAges count:(unsigned int)n top:(float)ceiling
    {
        count = MIN(n, (unsigned int)SEVO_STATS_RING_CAPACITY);
        memcpy(times, frameTimes, count * sizeof(float));
        memcpy(ages, frameAges, count * sizeof(float));
        top = ceiling;
        [self setNeedsDisplay:YES];
    }

    - (void) drawRect:(NSRect)dirty
    {
        NSRect bounds = [self bounds];
        CGFloat width = NSWidth(bounds), height = NSHeight(bounds);
        NSBezierPath *area, *line;
        float guides[] = { 1000.0f / 60, 1000.0f / 30 };
        unsigned int i;

        if (top <= 0) return;

        /* Guides at 60 and 30 fps, where they fall inside the plot. */
        [[NSColor colorWithWhite:1 alpha:0.18] setFill];
        for (i = 0; i < sizeof(guides) / sizeof(guides[0]); i++)
        {
            CGFloat y = floor(guides[i] / top * height);
            if (y < height) NSRectFill(NSMakeRect(0, y, width, 1));
        }

        /* Each frame is a step as wide as it lasted and as tall as its frame time,
           the newest at the right edge: a line along the steps over a faint fill. */
        area = [NSBezierPath bezierPath];
        line = [NSBezierPath bezierPath];
        for (i = count; i-- > 0;)
        {
            CGFloat right = width - ages[i] / kFrameGraphSpan * width;
            CGFloat left = width - (ages[i] + times[i] / 1000) / kFrameGraphSpan * width;
            CGFloat y = MIN(height, times[i] / top * height);

            if (right < 0) break;
            if ([line isEmpty])
            {
                [area moveToPoint:NSMakePoint(right, 0)];
                [line moveToPoint:NSMakePoint(right, y)];
            }
            else
                [line lineToPoint:NSMakePoint(right, y)];
            [area lineToPoint:NSMakePoint(right, y)];
            [line lineToPoint:NSMakePoint(MAX(0, left), y)];
            [area lineToPoint:NSMakePoint(MAX(0, left), y)];
            if (left <= 0) break;
        }
        if ([line isEmpty]) return;
        [area lineToPoint:NSMakePoint([area currentPoint].x, 0)];
        [area closePath];
        [[NSColor colorWithWhite:1 alpha:0.14] setFill];
        [area fill];
        [[NSColor colorWithWhite:1 alpha:0.92] setStroke];
        [line setLineWidth:1.5];
        [line setLineJoinStyle:NSLineJoinStyleRound];
        [line stroke];
    }

@end

@implementation WineFrameRateCounter

    - (void) dealloc
    {
        [self hide];
        [label release];
        [detail release];
        [graph release];
        [panel release];
        [super dealloc];
    }

    - (void) show
    {
        if (timer) return;
        lastFrames = sevo_stats_frame_count(NULL);
        lastTime = CFAbsoluteTimeGetCurrent();
        [self startTimer];
    }

    - (void) startTimer
    {
        timer = [[NSTimer timerWithTimeInterval:graphShown ? kFrameGraphInterval : kFrameRateInterval
                                         target:self selector:@selector(tick:)
                                       userInfo:nil repeats:YES] retain];
        [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    }

    - (void) hide
    {
        [timer invalidate];
        [timer release];
        timer = nil;
        [self detach];
    }

    - (void) setGraphShown:(BOOL)shown
    {
        if (graphShown == shown) return;
        graphShown = shown;
        if (panel) [self layoutPanel];
        if (timer)
        {
            [timer invalidate];
            [timer release];
            [self startTimer];
        }
    }

    - (void) detach
    {
        if (!host) return;
        [[NSNotificationCenter defaultCenter] removeObserver:self name:nil object:host];
        [host removeChildWindow:panel];
        [panel orderOut:nil];
        [host release];
        host = nil;
    }

    - (NSTextField*) makeLabelOfSize:(CGFloat)size weight:(NSFontWeight)weight
    {
        NSTextField* field = [[NSTextField labelWithString:@""] retain];
        [field setFont:[NSFont monospacedDigitSystemFontOfSize:size weight:weight]];
        [field setTextColor:[NSColor whiteColor]];
        return field;
    }

    - (void) makePanel
    {
        NSVisualEffectView* backing;

        panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 64, kFrameRateHeight)
                                           styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                             backing:NSBackingStoreBuffered defer:YES];
        [panel setOpaque:NO];
        [panel setBackgroundColor:[NSColor clearColor]];
        [panel setHasShadow:NO];
        [panel setIgnoresMouseEvents:YES];
        [panel setReleasedWhenClosed:NO];
        [panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorTransient];
        /* Dark whatever the system is set to: white digits over a game's picture. */
        [panel setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameVibrantDark]];

        backing = [[[NSVisualEffectView alloc] initWithFrame:[[panel contentView] bounds]] autorelease];
        [backing setMaterial:NSVisualEffectMaterialHUDWindow];
        [backing setBlendingMode:NSVisualEffectBlendingModeBehindWindow];
        [backing setState:NSVisualEffectStateActive];
        [backing setWantsLayer:YES];
        [[backing layer] setMasksToBounds:YES];
        [backing setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [panel setContentView:backing];

        label = [self makeLabelOfSize:12 weight:NSFontWeightSemibold];
        detail = [self makeLabelOfSize:10 weight:NSFontWeightRegular];
        graph = [[WineFrameTimeGraph alloc] initWithFrame:NSZeroRect];
        [backing addSubview:label];
        [backing addSubview:detail];
        [backing addSubview:graph];
        [self layoutPanel];
    }

    /* The capsule fits its number; the card is fixed, the number and detail left-aligned
       over the plot. */
    - (void) layoutPanel
    {
        NSView* backing = [panel contentView];
        NSRect frame = [panel frame];
        CGFloat inset = 10;

        [label sizeToFit];
        if (graphShown)
        {
            frame.size = NSMakeSize(kFrameGraphWidth, kFrameGraphHeight);
            [[backing layer] setCornerRadius:12];
            [label setAlignment:NSTextAlignmentLeft];
            [label setFrame:NSMakeRect(inset, kFrameGraphHeight - 6 - 16, kFrameGraphWidth - 2 * inset, 16)];
            [detail sizeToFit];
            [detail setFrame:NSMakeRect(inset, kFrameGraphHeight - 6 - 16 - 14, kFrameGraphWidth - 2 * inset, 14)];
            [graph setFrame:NSMakeRect(inset, 6, kFrameGraphWidth - 2 * inset, kFrameGraphPlotHeight)];
        }
        else
        {
            frame.size = NSMakeSize(ceil(NSWidth([label frame])) + kFrameRateHeight, kFrameRateHeight);
            [[backing layer] setCornerRadius:kFrameRateHeight / 2];
            [label setAlignment:NSTextAlignmentCenter];
            [label setFrame:NSMakeRect(0, (kFrameRateHeight - NSHeight([label frame])) / 2,
                                       frame.size.width, NSHeight([label frame]))];
        }
        [detail setHidden:!graphShown];
        [graph setHidden:!graphShown];
        [panel setFrame:frame display:NO];
        [self place];
    }

    /* The window the process presents into, once a present has named it;
       the frontmost game window until then. */
    - (NSWindow*) presentedWindow
    {
        unsigned long long number = 0;
        NSWindow* window;

        sevo_stats_frame_count(&number);
        window = number ? [NSApp windowWithWindowNumber:(NSInteger)number] : nil;
        if (![window isKindOfClass:[WineWindow class]] || ![window isVisible])
            window = [[WineApplicationController sharedController] frontWineWindow];
        return window;
    }

    - (void) attachTo:(NSWindow*)window
    {
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];

        if (window == host) return;
        [self detach];
        if (!window) return;
        if (!panel) [self makePanel];
        host = [window retain];
        [host addChildWindow:panel ordered:NSWindowAbove];
        [nc addObserver:self selector:@selector(hostChanged:) name:NSWindowDidResizeNotification object:host];
        [nc addObserver:self selector:@selector(hostClosing:) name:NSWindowWillCloseNotification object:host];
        [self place];
    }

    - (void) hostChanged:(NSNotification*)note
    {
        [self place];
    }

    - (void) hostClosing:(NSNotification*)note
    {
        [self detach];
    }

    /* Top right of the content, inside the title bar's lower edge. */
    - (void) place
    {
        NSRect content, frame = [panel frame];

        if (!host) return;
        content = [host contentRectForFrameRect:[host frame]];
        frame.origin.x = NSMaxX(content) - NSWidth(frame) - kFrameRateInset;
        frame.origin.y = NSMaxY(content) - NSHeight(frame) - kFrameRateInset;
        [panel setFrame:frame display:YES];
    }

    /* The rate since the last reading, from the counter the stats page keeps. A count that
       went backwards switched source (to D3DMetal's drawables or the presenter's frames)
       and starts over. */
    - (NSString*) rateText
    {
        unsigned long long frames = sevo_stats_frame_count(NULL);
        CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
        double elapsed = now - lastTime;
        NSString* text = nil;

        if (elapsed > 0 && frames >= lastFrames)
            text = [NSString stringWithFormat:@"%.0f fps", (frames - lastFrames) / elapsed];
        lastFrames = frames;
        lastTime = now;
        return text;
    }

    /* Reads the ring: fills the plot and writes the detail row. */
    - (void) updateGraph
    {
        static unsigned int stamps[SEVO_STATS_RING_CAPACITY];
        static float times[SEVO_STATS_RING_CAPACITY], ages[SEVO_STATS_RING_CAPACITY];
        static float recent[SEVO_STATS_RING_CAPACITY];
        unsigned int n = sevo_stats_recent_frames(stamps, SEVO_STATS_RING_CAPACITY), frames = 0, kept = 0, i;
        float top = kFrameGraphMinimumTop, slowest = 0, sum = 0;
        unsigned int lows;
        NSString* text;

        if (n < 2)
        {
            [graph setTimes:times ages:ages count:0 top:top];
            [detail setStringValue:@"no frames yet"];
            return;
        }
        /* Ages count back from the newest frame rather than the clock, which keeps the plot
           still while the game is paused rather than scrolling it empty. */
        for (i = 1; i < n; i++)
        {
            float frame = (uint32_t)(stamps[i] - stamps[i - 1]) / 1000.0f;
            float age = (uint32_t)(stamps[n - 1] - stamps[i]) / 1e6f;

            if (age > kFrameStatsSpan) continue;
            recent[kept++] = frame;
            if (age <= kFrameGraphSpan)
            {
                times[frames] = frame;
                ages[frames] = age;
                frames++;
                if (frame > top) top = frame;
            }
        }
        [graph setTimes:times ages:ages count:frames top:MIN(top * 1.1f, 250)];

        if (!kept)
        {
            [detail setStringValue:@""];
            return;
        }
        /* The 1 % low is the rate of the slowest one per cent of frames, averaged. */
        qsort(recent, kept, sizeof(float), compare_floats);
        slowest = recent[kept - 1];
        lows = MAX(1u, kept / 100);
        for (i = kept - lows; i < kept; i++) sum += recent[i];
        text = [NSString stringWithFormat:@"1%% low %.0f · slowest %.1f ms", 1000 * lows / sum, slowest];
        [detail setStringValue:text];
    }

    - (void) tick:(NSTimer*)unused
    {
        NSString* text = nil;
        CGFloat before;

        [self attachTo:[self presentedWindow]];
        if (!host) return;

        /* The number keeps its half-second cadence when the graph runs faster. */
        if (!graphShown || ++ticks % 5 == 0) text = [self rateText];
        if (graphShown) [self updateGraph];
        if (!text || [text isEqualToString:[label stringValue]]) return;

        before = NSWidth([label frame]);
        [label setStringValue:text];
        if (!graphShown) [self layoutPanel];
        else if (before == 0) [self place];
    }

@end

static WineFrameRateCounter* sevoFrameRateCounter;

/* How long a close or a Quit may go untaken before the user is asked. Windows
   calls a window hung after five seconds without a message read. */
static const NSTimeInterval kUnansweredRequestSeconds = 5;

@implementation WineApplicationController

    @synthesize keyboardType, lastFlagsChanged;
    @synthesize applicationIcon;
    @synthesize cursorFrames, cursorTimer, cursor;
    @synthesize mouseCaptureWindow;
    @synthesize lastSetCursorPositionTime;

    + (void) initialize
    {
        if (self == [WineApplicationController class])
        {
            NSDictionary<NSString *, id> *defaults =
            @{
                @"NSQuotedKeystrokeBinding" : @"",
                    @"NSRepeatCountBinding" : @"",
                @"ApplePressAndHoldEnabled" : @NO
            };

            [[NSUserDefaults standardUserDefaults] registerDefaults:defaults];

            [NSWindow setAllowsAutomaticWindowTabbing:NO];
        }
    }

    + (WineApplicationController*) sharedController
    {
        static WineApplicationController* sharedController;
        static dispatch_once_t once;

        dispatch_once(&once, ^{
            sharedController = [[self alloc] init];
        });

        return sharedController;
    }

    - (id) init
    {
        self = [super init];
        if (self != nil)
        {
            CFRunLoopSourceContext context = { 0 };
            context.perform = PerformRequest;
            requestSource = CFRunLoopSourceCreate(NULL, 0, &context);
            if (!requestSource)
            {
                [self release];
                return nil;
            }
            CFRunLoopAddSource(CFRunLoopGetMain(), requestSource, kCFRunLoopCommonModes);
            CFRunLoopAddSource(CFRunLoopGetMain(), requestSource, (CFStringRef)WineAppWaitQueryResponseMode);

            requests =  [[NSMutableArray alloc] init];
            requestsManipQueue = dispatch_queue_create("org.winehq.WineAppRequestManipQueue", NULL);

            eventQueues = [[NSMutableArray alloc] init];
            eventQueuesLock = [[NSLock alloc] init];

            keyWindows = [[NSMutableArray alloc] init];

            originalDisplayModes = [[NSMutableDictionary alloc] init];
            latentDisplayModes = [[NSMutableDictionary alloc] init];

            windowsBeingDragged = [[NSMutableSet alloc] init];

            if (!requests || !requestsManipQueue || !eventQueues || !eventQueuesLock ||
                !keyWindows || !originalDisplayModes || !latentDisplayModes)
            {
                [self release];
                return nil;
            }

            [self setupObservations];

            keyboardType = LMGetKbdType();

            if ([NSApp isActive])
                [self applicationDidBecomeActive:nil];
        }
        return self;
    }

    - (void) dealloc
    {
        [windowsBeingDragged release];
        [cursor release];
        [screenFrameCGRects release];
        [applicationIcon release];
        [clipCursorHandler release];
        [cursorTimer release];
        [cursorFrames release];
        [latentDisplayModes release];
        [originalDisplayModes release];
        [keyWindows release];
        [eventQueues release];
        [eventQueuesLock release];
        if (requestsManipQueue) dispatch_release(requestsManipQueue);
        [requests release];
        if (requestSource)
        {
            CFRunLoopSourceInvalidate(requestSource);
            CFRelease(requestSource);
        }
        CGDisplayRemoveReconfigurationCallback(DisplayReconfigCallback, NULL);
        [super dealloc];
    }

    - (void) transformProcessToForeground:(BOOL)activateIfTransformed
    {
        /* A game started through its own loader bundle is a regular app from its first
           instruction, and still needs the menus: without them it has no Window menu, no
           Enter Full Screen, and no View menu. */
        static BOOL mainMenuBuilt;
        BOOL becomesRegular = [NSApp activationPolicy] != NSApplicationActivationPolicyRegular;

        if (becomesRegular || !mainMenuBuilt)
        {
            NSMenu* mainMenu;
            NSMenu* submenu;
            NSString* bundleName;
            NSString* title;
            NSMenuItem* item;

            mainMenuBuilt = YES;
            if (becomesRegular) [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

            if (activateIfTransformed)
                [self tryToActivateIgnoringOtherApps:YES];

            if (!enable_app_nap)
            {
                [[[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                                                                reason:@"Running Windows program"] retain]; // intentional leak
            }

            mainMenu = [[[NSMenu alloc] init] autorelease];

            // Application menu
            submenu = [[[NSMenu alloc] initWithTitle:WineLocalizedString(STRING_MENU_WINE)] autorelease];
            bundleName = [[NSBundle mainBundle] objectForInfoDictionaryKey:(NSString*)kCFBundleNameKey];

            if ([bundleName length])
                title = [NSString stringWithFormat:WineLocalizedString(STRING_MENU_ITEM_HIDE_APPNAME), bundleName];
            else
                title = WineLocalizedString(STRING_MENU_ITEM_HIDE);
            item = [submenu addItemWithTitle:title action:@selector(hide:) keyEquivalent:@""];

            item = [submenu addItemWithTitle:WineLocalizedString(STRING_MENU_ITEM_HIDE_OTHERS)
                                      action:@selector(hideOtherApplications:)
                               keyEquivalent:@"h"];
            [item setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];

            item = [submenu addItemWithTitle:WineLocalizedString(STRING_MENU_ITEM_SHOW_ALL)
                                      action:@selector(unhideAllApplications:)
                               keyEquivalent:@""];

            [submenu addItem:[NSMenuItem separatorItem]];

            if ([bundleName length])
                title = [NSString stringWithFormat:WineLocalizedString(STRING_MENU_ITEM_QUIT_APPNAME), bundleName];
            else
                title = WineLocalizedString(STRING_MENU_ITEM_QUIT);
            item = [submenu addItemWithTitle:title action:@selector(terminate:) keyEquivalent:@"q"];
            [item setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
            item = [[[NSMenuItem alloc] init] autorelease];
            [item setTitle:WineLocalizedString(STRING_MENU_WINE)];
            [item setSubmenu:submenu];
            [mainMenu addItem:item];

            [self addSevoViewMenuTo:mainMenu];

            // Window menu
            submenu = [[[NSMenu alloc] initWithTitle:WineLocalizedString(STRING_MENU_WINDOW)] autorelease];
            [submenu addItemWithTitle:WineLocalizedString(STRING_MENU_ITEM_MINIMIZE)
                               action:@selector(performMiniaturize:)
                        keyEquivalent:@""];
            [submenu addItemWithTitle:WineLocalizedString(STRING_MENU_ITEM_ZOOM)
                               action:@selector(performZoom:)
                        keyEquivalent:@""];
            item = [submenu addItemWithTitle:WineLocalizedString(STRING_MENU_ITEM_ENTER_FULL_SCREEN)
                                      action:@selector(toggleFullScreen:)
                               keyEquivalent:@"f"];
            [item setKeyEquivalentModifierMask:NSEventModifierFlagCommand |
                                               NSEventModifierFlagOption |
                                               NSEventModifierFlagControl];
            [submenu addItem:[NSMenuItem separatorItem]];
            [submenu addItemWithTitle:WineLocalizedString(STRING_MENU_ITEM_BRING_ALL_TO_FRONT)
                               action:@selector(arrangeInFront:)
                        keyEquivalent:@""];
            item = [[[NSMenuItem alloc] init] autorelease];
            [item setTitle:WineLocalizedString(STRING_MENU_WINDOW)];
            [item setSubmenu:submenu];
            [mainMenu addItem:item];

            [NSApp setMainMenu:mainMenu];
            [NSApp setWindowsMenu:submenu];

            /* The bundle's own icon stands where there is one. */
            if (becomesRegular) [NSApp setApplicationIconImage:self.applicationIcon];
        }
    }

    - (BOOL) waitUntilQueryDone:(bool*)done timeout:(NSDate*)timeout processEvents:(BOOL)processEvents
    {
        PerformRequest(NULL);

        do
        {
            if (processEvents)
            {
                @autoreleasepool
                {
                    NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                        untilDate:timeout
                                                           inMode:NSDefaultRunLoopMode
                                                          dequeue:YES];
                    if (event)
                        [NSApp sendEvent:event];
                }
            }
            else
                [[NSRunLoop currentRunLoop] runMode:WineAppWaitQueryResponseMode beforeDate:timeout];
        } while (!*done && [timeout timeIntervalSinceNow] >= 0);

        return *done;
    }

    - (BOOL) registerEventQueue:(WineEventQueue*)queue
    {
        [eventQueuesLock lock];
        [eventQueues addObject:queue];
        [eventQueuesLock unlock];
        return TRUE;
    }

    - (void) unregisterEventQueue:(WineEventQueue*)queue
    {
        [eventQueuesLock lock];
        [eventQueues removeObjectIdenticalTo:queue];
        [eventQueuesLock unlock];
    }

    - (void) computeEventTimeAdjustmentFromTicks:(unsigned long long)tickcount uptime:(uint64_t)uptime_ns
    {
        eventTimeAdjustment = (tickcount / 1000.0) - (uptime_ns / (double)NSEC_PER_SEC);
    }

    - (double) ticksForEventTime:(NSTimeInterval)eventTime
    {
        return (eventTime + eventTimeAdjustment) * 1000;
    }

    /* Invalidate old focus offers across all queues. */
    - (void) invalidateGotFocusEvents
    {
        WineEventQueue* queue;

        windowFocusSerial++;

        [eventQueuesLock lock];
        for (queue in eventQueues)
        {
            [queue discardEventsMatchingMask:event_mask_for_type(WINDOW_GOT_FOCUS)
                                   forWindow:nil];
        }
        [eventQueuesLock unlock];
    }

    - (void) windowGotFocus:(WineWindow*)window
    {
        macdrv_event* event;

        [self invalidateGotFocusEvents];
        [self applyDeferredClip];

        event = macdrv_create_event(WINDOW_GOT_FOCUS, window);
        event->window_got_focus.serial = windowFocusSerial;
        if (triedWindows)
            event->window_got_focus.tried_windows = [triedWindows retain];
        else
            event->window_got_focus.tried_windows = [[NSMutableSet alloc] init];
        [window.queue postEvent:event];
        macdrv_release_event(event);
    }

    - (void) windowRejectedFocusEvent:(const macdrv_event*)event
    {
        if (event->window_got_focus.serial == windowFocusSerial)
        {
            NSMutableArray* windows = [keyWindows mutableCopy];
            NSNumber* windowNumber;
            WineWindow* window;

            for (windowNumber in [NSWindow windowNumbersWithOptions:NSWindowNumberListAllSpaces])
            {
                window = (WineWindow*)[NSApp windowWithWindowNumber:[windowNumber integerValue]];
                if ([window isKindOfClass:[WineWindow class]] && [window screen] &&
                    ![windows containsObject:window])
                    [windows addObject:window];
            }

            triedWindows = (NSMutableSet*)event->window_got_focus.tried_windows;
            [triedWindows addObject:(WineWindow*)event->window];
            for (window in windows)
            {
                if (![triedWindows containsObject:window] && [window canBecomeKeyWindow])
                {
                    [window makeKeyWindow];
                    break;
                }
            }
            triedWindows = nil;
            [windows release];
        }
    }

    static BOOL EqualInputSource(TISInputSourceRef source1, TISInputSourceRef source2)
    {
        if (!source1 && !source2)
            return TRUE;
        if (!source1 || !source2)
            return FALSE;
        return CFEqual(source1, source2);
    }

    - (void) keyboardSelectionDidChange:(BOOL)force
    {
        TISInputSourceRef inputSource, inputSourceLayout;

        if (!force)
        {
            NSTextInputContext* context = [NSTextInputContext currentInputContext];
            if (!context || ![context client])
                return;
        }

        inputSource = TISCopyCurrentKeyboardInputSource();
        inputSourceLayout = TISCopyCurrentKeyboardLayoutInputSource();
        if (!force && EqualInputSource(inputSource, lastKeyboardInputSource) &&
            EqualInputSource(inputSourceLayout, lastKeyboardLayoutInputSource))
        {
            if (inputSource) CFRelease(inputSource);
            if (inputSourceLayout) CFRelease(inputSourceLayout);
            return;
        }

        if (lastKeyboardInputSource)
            CFRelease(lastKeyboardInputSource);
        lastKeyboardInputSource = inputSource;
        if (lastKeyboardLayoutInputSource)
            CFRelease(lastKeyboardLayoutInputSource);
        lastKeyboardLayoutInputSource = inputSourceLayout;

        if (inputSourceLayout)
        {
            CFDataRef uchr;
            uchr = TISGetInputSourceProperty(inputSourceLayout,
                    kTISPropertyUnicodeKeyLayoutData);
            if (uchr)
            {
                macdrv_event* event;
                WineEventQueue* queue;

                event = macdrv_create_event(KEYBOARD_CHANGED, nil);
                event->keyboard_changed.keyboard_type = self.keyboardType;
                event->keyboard_changed.iso_keyboard = (KBGetLayoutType(self.keyboardType) == kKeyboardISO);
                event->keyboard_changed.uchr = CFDataCreateCopy(NULL, uchr);
                event->keyboard_changed.input_source = (TISInputSourceRef)CFRetain(inputSource);

                if (event->keyboard_changed.uchr)
                {
                    [eventQueuesLock lock];

                    for (queue in eventQueues)
                        [queue postEvent:event];

                    [eventQueuesLock unlock];
                }

                macdrv_release_event(event);
            }
        }
    }

    - (void) keyboardSelectionDidChange
    {
        [self keyboardSelectionDidChange:NO];
    }

    - (void) setKeyboardType:(CGEventSourceKeyboardType)newType
    {
        if (newType != keyboardType)
        {
            keyboardType = newType;
            [self keyboardSelectionDidChange:YES];
        }
    }

    - (void) enabledKeyboardInputSourcesChanged
    {
        macdrv_layout_list_needs_update = TRUE;
    }

    - (CGFloat) primaryScreenHeight
    {
        if (!primaryScreenHeightValid)
        {
            NSArray* screens = [NSScreen screens];
            NSUInteger count = [screens count];
            if (count)
            {
                NSUInteger size;
                CGRect* rect;
                NSScreen* screen;

                primaryScreenHeight = NSHeight([screens[0] frame]);
                primaryScreenHeightValid = TRUE;

                size = count * sizeof(CGRect);
                if (!screenFrameCGRects)
                    screenFrameCGRects = [[NSMutableData alloc] initWithLength:size];
                else
                    [screenFrameCGRects setLength:size];

                rect = [screenFrameCGRects mutableBytes];
                for (screen in screens)
                {
                    CGRect temp = NSRectToCGRect([screen frame]);
                    temp.origin.y = primaryScreenHeight - CGRectGetMaxY(temp);
                    *rect++ = temp;
                }
            }
            else
                return 1280; /* arbitrary value */
        }

        return primaryScreenHeight;
    }

    - (NSPoint) flippedMouseLocation:(NSPoint)point
    {
        /* This relies on the fact that Cocoa's mouse location points are
           actually off by one (precisely because they were flipped from
           Quartz screen coordinates using this same technique). */
        point.y = [self primaryScreenHeight] - point.y;
        return point;
    }

    /* The presentation-scaled window a Wine point maps through: a visible
       one whose Wine rectangle holds the point, the key window before any
       other and a higher level before a lower, as winePointFromScreenPoint:
       settles the reverse mapping. A hidden or minimized window keeps the
       Wine rectangle and scale it last had, and a game's own windows share
       one rectangle, so without the order a cursor warp or clip could map
       through a window that is not on screen. */
    - (WineWindow*) scaledWindowContainingWinePoint:(CGPoint)point
    {
        WineWindow* best = nil;

        for (NSWindow* candidate in [NSApp windows])
        {
            WineWindow* window = (WineWindow*)candidate;

            if (![window isKindOfClass:[WineWindow class]] || ![window isVisible] ||
                ![window wineContentContainsScreenPoint:point])
                continue;
            if (!best || [window isKeyWindow] ||
                (![best isKeyWindow] && [window level] > [best level]))
                best = window;
        }

        return best;
    }

    - (CGPoint) screenPointFromWinePoint:(CGPoint)point
    {
        WineWindow* window = [self scaledWindowContainingWinePoint:point];
        return window ? [window screenPointFromWinePoint:point] : point;
    }

    - (CGRect) screenRectFromWineRect:(CGRect)rect
    {
        CGPoint center = CGPointMake(CGRectGetMidX(rect), CGRectGetMidY(rect));
        WineWindow* window = [self scaledWindowContainingWinePoint:center];
        CGPoint origin, corner;

        if (!window) return rect;
        origin = [window screenPointFromWinePoint:rect.origin];
        corner = [window screenPointFromWinePoint:CGPointMake(CGRectGetMaxX(rect), CGRectGetMaxY(rect))];
        return CGRectMake(origin.x, origin.y, corner.x - origin.x, corner.y - origin.y);
    }

    /* A screen point maps through the presentation-scaled window under it,
       found in this process's own window list. Asking the window server
       which window is under a point costs a round trip per call, and
       GetCursorPos is polled every frame by games and by Steam's overlay
       thread, so the lookup stays in this process. Two scaled windows
       overlapping at the point is settled by the key window, then the
       higher level. */
    - (CGPoint) winePointFromScreenPoint:(CGPoint)point
    {
        NSPoint cocoaPoint = [self flippedMouseLocation:NSPointFromCGPoint(point)];
        WineWindow* best = nil;

        for (NSWindow* candidate in [NSApp windows])
        {
            WineWindow* window = (WineWindow*)candidate;

            if (![window isKindOfClass:[WineWindow class]] || ![window isVisible] ||
                !window.presentationScaled || !NSMouseInRect(cocoaPoint, [window frame], NO))
                continue;
            if (!best || [window isKeyWindow] ||
                (![best isKeyWindow] && [window level] > [best level]))
                best = window;
        }

        return best ? [best winePointFromScreenPoint:point] : point;
    }

    - (void) flipRect:(NSRect*)rect
    {
        // We don't use -primaryScreenHeight here so there's no chance of having
        // out-of-date cached info.  This method is called infrequently enough
        // that getting the screen height each time is not prohibitively expensive.
        rect->origin.y = NSMaxY([[NSScreen screens][0] frame]) - NSMaxY(*rect);
    }

    - (WineWindow*) frontWineWindow
    {
        NSNumber* windowNumber;
        for (windowNumber in [NSWindow windowNumbersWithOptions:NSWindowNumberListAllSpaces])
        {
            NSWindow* window = [NSApp windowWithWindowNumber:[windowNumber integerValue]];
            if ([window isKindOfClass:[WineWindow class]] && [window screen])
                return (WineWindow*)window;
        }

        return nil;
    }

    /* The View menu: how the program's picture reaches the screen. A choice is applied at
       once where the presenter is running, and handed to `sevo` either way, which writes it
       into the game's own settings so the next launch starts with it. */
    - (void) addSevoViewMenuTo:(NSMenu*)mainMenu
    {
        NSMenu* view = [[[NSMenu alloc] initWithTitle:@"View"] autorelease];
        NSMenu* upscalers = [[[NSMenu alloc] initWithTitle:@"Upscaler"] autorelease];
        NSMenu* filters = [[[NSMenu alloc] initWithTitle:@"Final Filter"] autorelease];
        NSMenuItem* item;
        char* packages = sevo_presenter_package_names();

        for (NSArray* pair in @[@[@"Off", @"off"], @[@"Final Filter Only", @"lanczos"], @[@"MetalFX", @"metalfx"]])
        {
            item = [upscalers addItemWithTitle:pair[0] action:@selector(sevoChooseUpscaler:) keyEquivalent:@""];
            [item setTarget:self];
            [item setRepresentedObject:pair[1]];
        }
        if (packages && *packages)
        {
            [upscalers addItem:[NSMenuItem separatorItem]];
            for (NSString* name in [[NSString stringWithUTF8String:packages] componentsSeparatedByString:@"\n"])
            {
                item = [upscalers addItemWithTitle:name action:@selector(sevoChooseUpscaler:) keyEquivalent:@""];
                [item setTarget:self];
                [item setRepresentedObject:name];
            }
        }
        free(packages);

        for (NSArray* pair in @[@[@"Nearest", @"nearest"], @[@"Bilinear", @"bilinear"], @[@"Lanczos", @"lanczos"]])
        {
            item = [filters addItemWithTitle:pair[0] action:@selector(sevoChooseFilter:) keyEquivalent:@""];
            [item setTarget:self];
            [item setRepresentedObject:pair[1]];
        }

        item = [view addItemWithTitle:@"Upscaler" action:NULL keyEquivalent:@""];
        [item setSubmenu:upscalers];
        item = [view addItemWithTitle:@"Final Filter" action:NULL keyEquivalent:@""];
        [item setSubmenu:filters];
        [view addItem:[NSMenuItem separatorItem]];
        item = [view addItemWithTitle:@"Resizable Windows" action:@selector(sevoToggleResizableWindows:) keyEquivalent:@"r"];
        [item setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
        [item setTarget:self];
        [view addItem:[NSMenuItem separatorItem]];
        item = [view addItemWithTitle:@"Show Frame Rate" action:@selector(sevoToggleFrameRate:) keyEquivalent:@"f"];
        [item setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
        [item setTarget:self];
        item = [view addItemWithTitle:@"Show Frame Time Graph" action:@selector(sevoToggleFrameGraph:) keyEquivalent:@"g"];
        [item setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
        [item setTarget:self];
        item = [view addItemWithTitle:@"Show Picture Details" action:@selector(sevoToggleReadout:) keyEquivalent:@"i"];
        [item setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
        [item setTarget:self];

        if (frame_rate_on) [self sevoSetFrameRateShown:YES];

        item = [[[NSMenuItem alloc] init] autorelease];
        [item setTitle:@"View"];
        [item setSubmenu:view];
        [mainMenu addItem:item];
    }

    - (BOOL) validateMenuItem:(NSMenuItem*)menuItem
    {
        SEL action = [menuItem action];

        if (action == @selector(sevoChooseUpscaler:))
        {
            /* The presenter stays in the picture's path until the game ends. */
            if (presenter_on && [[menuItem representedObject] isEqual:@"off"])
                [menuItem setTitle:@"Off (Next Launch)"];
            [menuItem setState:!strcasecmp(upscaler_option, [[menuItem representedObject] UTF8String]) ? NSControlStateValueOn : NSControlStateValueOff];
        }
        else if (action == @selector(sevoChooseFilter:))
            [menuItem setState:!strcasecmp(final_filter_option, [[menuItem representedObject] UTF8String]) ? NSControlStateValueOn : NSControlStateValueOff];
        else if (action == @selector(sevoToggleFrameRate:))
            [menuItem setState:frame_rate_on ? NSControlStateValueOn : NSControlStateValueOff];
        else if (action == @selector(sevoToggleFrameGraph:))
            [menuItem setState:frame_rate_on && frame_graph_on ? NSControlStateValueOn : NSControlStateValueOff];
        else if (action == @selector(sevoToggleResizableWindows:))
        {
            NSWindow* key = [NSApp keyWindow];
            [menuItem setState:resizable_windows != RESIZABLE_WINDOWS_OFF ? NSControlStateValueOn : NSControlStateValueOff];
            return !([key styleMask] & NSWindowStyleMaskFullScreen);
        }
        else if (action == @selector(sevoToggleReadout:))
        {
            [menuItem setState:sevoReadoutShown ? NSControlStateValueOn : NSControlStateValueOff];
            return presenter_on;
        }
        return YES;
    }

    /* Hands a per-game setting to `sevo app config`, the one writer of the game's settings.
       SEVO_CLI names it where the app set it; the symlink the app installs is the fallback. */
    - (void) sevoStoreSetting:(NSString*)key value:(NSString*)value
    {
        unsigned int appid = sevo_stats_appid();
        const char* cli = getenv("SEVO_CLI");
        NSString* path = cli && *cli ? [NSString stringWithUTF8String:cli] : @"/usr/local/bin/sevo";
        NSTask* task;

        if (!appid || ![[NSFileManager defaultManager] isExecutableFileAtPath:path]) return;
        task = [[[NSTask alloc] init] autorelease];
        [task setExecutableURL:[NSURL fileURLWithPath:path]];
        [task setArguments:@[@"app", @"config", [NSString stringWithFormat:@"%u", appid], key, value]];
        [task setStandardOutput:[NSFileHandle fileHandleWithNullDevice]];
        [task setStandardError:[NSFileHandle fileHandleWithNullDevice]];
        [task launchAndReturnError:NULL];
    }

    - (void) sevoChooseUpscaler:(NSMenuItem*)sender
    {
        NSString* name = [sender representedObject];
        BOOL wasOff = !strcasecmp(upscaler_option, "off");
        BOOL nowOff = ![name caseInsensitiveCompare:@"off"];

        snprintf(upscaler_option, sizeof(upscaler_option), "%s", [name UTF8String]);
        [self sevoStoreSetting:@"upscaler" value:name];

        if (presenter_on && !nowOff)
        {
            sevo_presenter_set_options([name UTF8String], NULL);
            return;
        }
        if (presenter_on && nowOff)
        {
            /* The presenter stays in the picture's path until the program ends; plain
               resampling is the nearest thing to off it can do meanwhile. */
            sevo_presenter_set_options("lanczos", NULL);
        }
        if (wasOff != nowOff)
        {
            NSAlert* alert = [[[NSAlert alloc] init] autorelease];
            [alert setMessageText:nowOff ? @"The upscaler turns off the next time this game starts."
                                         : @"The upscaler turns on the next time this game starts."];
            [alert setInformativeText:nowOff ? @"Until then the picture is resampled with Lanczos."
                                             : @"This game started without it, and it cannot join a running game."];
            [alert addButtonWithTitle:@"Quit Game"];
            [alert addButtonWithTitle:@"Later"];
            if ([alert runModal] == NSAlertFirstButtonReturn)
                [NSApp terminate:nil];
        }
    }

    - (void) sevoChooseFilter:(NSMenuItem*)sender
    {
        NSString* name = [sender representedObject];

        snprintf(final_filter_option, sizeof(final_filter_option), "%s", [name UTF8String]);
        [self sevoStoreSetting:@"filter" value:name];
        if (presenter_on) sevo_presenter_set_options(NULL, [name UTF8String]);
    }

    - (void) sevoSetFrameRateShown:(BOOL)shown
    {
        frame_rate_on = shown;
        if (shown && !sevoFrameRateCounter) sevoFrameRateCounter = [[WineFrameRateCounter alloc] init];
        [sevoFrameRateCounter setGraphShown:frame_graph_on];
        if (shown) [sevoFrameRateCounter show];
        else [sevoFrameRateCounter hide];
    }

    - (void) sevoToggleFrameRate:(NSMenuItem*)sender
    {
        [self sevoSetFrameRateShown:!frame_rate_on];
        [self sevoStoreSetting:@"fps" value:frame_rate_on ? @"on" : @"off"];
    }

    /* The graph comes with the counter: turning it on shows both, turning it off leaves the
       number. */
    - (void) sevoToggleFrameGraph:(NSMenuItem*)sender
    {
        BOOL on = !(frame_rate_on && frame_graph_on);

        frame_graph_on = on;
        if (on && !frame_rate_on)
        {
            [self sevoSetFrameRateShown:YES];
            [self sevoStoreSetting:@"fps" value:@"on"];
        }
        else
            [sevoFrameRateCounter setGraphShown:on];
        [self sevoStoreSetting:@"fps-graph" value:on ? @"on" : @"off"];
    }

    /* Off, or back to the level the game started with; a game that started with it off
       gets the window mode, which also puts a full-screen game in a window. */
    - (void) sevoToggleResizableWindows:(NSMenuItem*)sender
    {
        static int levelWhenOn = RESIZABLE_WINDOWS_OFF;
        static NSString* const names[] = {
            [RESIZABLE_WINDOWS_OFF] = @"off", [RESIZABLE_WINDOWS_FIXED] = @"fixed",
            [RESIZABLE_WINDOWS_ALL] = @"all", [RESIZABLE_WINDOWS_WINDOW] = @"window",
        };

        if (resizable_windows != RESIZABLE_WINDOWS_OFF)
        {
            levelWhenOn = resizable_windows;
            resizable_windows = RESIZABLE_WINDOWS_OFF;
        }
        else
            resizable_windows = levelWhenOn != RESIZABLE_WINDOWS_OFF ? levelWhenOn : RESIZABLE_WINDOWS_WINDOW;

        for (NSWindow* window in [NSApp windows])
        {
            if ([window isKindOfClass:[WineWindow class]])
                [(WineWindow*)window reapplyResizableWindows];
        }
        [self updatePresentationOptions];
        [self sevoStoreSetting:@"windows" value:names[resizable_windows]];
    }

    - (void) sevoToggleReadout:(NSMenuItem*)sender
    {
        sevoReadoutShown = !sevoReadoutShown;
        sevo_presenter_set_readout(sevoReadoutShown);
    }

    - (void) adjustWindowLevels:(BOOL)active
    {
        NSArray* windowNumbers;
        NSMutableArray* wineWindows;
        NSNumber* windowNumber;
        NSUInteger nextFloatingIndex = 0;
        __block NSInteger maxLevel = NSIntegerMin;
        __block NSInteger maxNonfloatingLevel = NSNormalWindowLevel;
        /* Windows with WS_EX_TOPMOST should have a window level higher than the macOS dock */
        __block NSInteger minFloatingLevel = kCGDockWindowLevel + 1;
        __block WineWindow* prev = nil;
        WineWindow* window;

        if ([NSApp isHidden]) return;

        windowNumbers = [NSWindow windowNumbersWithOptions:0];
        wineWindows = [[NSMutableArray alloc] initWithCapacity:[windowNumbers count]];

        // For the most part, we rely on the window server's ordering of the windows
        // to be authoritative.  The one exception is if the "floating" property of
        // one of the windows has been changed, it may be in the wrong level and thus
        // in the order.  This method is what's supposed to fix that up.  So build
        // a list of Wine windows sorted first by floating-ness and then by order
        // as indicated by the window server.
        for (windowNumber in windowNumbers)
        {
            window = (WineWindow*)[NSApp windowWithWindowNumber:[windowNumber integerValue]];
            if ([window isKindOfClass:[WineWindow class]])
            {
                if (window.floating)
                    [wineWindows insertObject:window atIndex:nextFloatingIndex++];
                else
                    [wineWindows addObject:window];
            }
        }

        NSDisableScreenUpdates();

        // Go from back to front so that all windows in front of one which is
        // elevated for full-screen are also elevated.
        [wineWindows enumerateObjectsWithOptions:NSEnumerationReverse
                                      usingBlock:^(id obj, NSUInteger idx, BOOL *stop){
            WineWindow* window = (WineWindow*)obj;
            NSInteger origLevel = [window level];
            NSInteger newLevel = [window minimumLevelForActive:active];

            if (window.floating)
            {
                if (minFloatingLevel <= maxNonfloatingLevel)
                    minFloatingLevel = maxNonfloatingLevel + 1;
                if (newLevel < minFloatingLevel)
                    newLevel = minFloatingLevel;
            }

            if (newLevel < maxLevel)
                newLevel = maxLevel;
            else
                maxLevel = newLevel;

            if (!window.floating && maxNonfloatingLevel < newLevel)
                maxNonfloatingLevel = newLevel;

            if (newLevel != origLevel)
            {
                [window setLevel:newLevel];

                if (origLevel < newLevel)
                {
                    // If we increased the level, the window should be toward the
                    // back of its new level (but still ahead of the previous
                    // windows we did this to).
                    if (prev)
                        [window orderWindow:NSWindowAbove relativeTo:[prev windowNumber]];
                    else
                        [window orderBack:nil];
                }
                else
                {
                    // If we decreased the level, we want the window at the top
                    // of its new level. -setLevel: is documented to do that on
                    // its own, but that's buggy on Ventura. Since we're looping
                    // back-to-front here, -orderFront: will do the right thing.
                    [window orderFront:nil];
                }
            }

            prev = window;
        }];

        NSEnableScreenUpdates();

        [wineWindows release];

        // The above took care of the visible windows on the current space.  That
        // leaves windows on other spaces, minimized windows, and windows which
        // are not ordered in.  We want to leave windows on other spaces alone
        // so the space remains just as they left it (when viewed in Exposé or
        // Mission Control, for example).  We'll adjust the window levels again
        // after we switch to another space, anyway.  Windows which aren't
        // ordered in will be handled when we order them in.  Minimized windows
        // on the current space should be set to the level they would have gotten
        // if they were at the front of the windows with the same floating-ness,
        // because that's where they'll go if/when they are unminimized.  Again,
        // for good measure we'll adjust window levels again when a window is
        // unminimized, too.
        for (window in [NSApp windows])
        {
            if ([window isKindOfClass:[WineWindow class]] && [window isMiniaturized] &&
                [window isOnActiveSpace])
            {
                NSInteger origLevel = [window level];
                NSInteger newLevel = [window minimumLevelForActive:YES];
                NSInteger maxLevelForType = window.floating ? maxLevel : maxNonfloatingLevel;

                if (newLevel < maxLevelForType)
                    newLevel = maxLevelForType;

                if (newLevel != origLevel)
                    [window setLevel:newLevel];
            }
        }

        [self updatePresentationOptionsForActive:active];
    }

    - (void) adjustWindowLevels
    {
        [self adjustWindowLevels:[NSApp isActive]];
    }

    /* A fullscreen-style window sits at the normal level while the displays
       are uncaptured, where the menu bar would lie over its top strip. While
       such a window is key on the active space and the app is active, the bar
       and the Dock auto-hide: the bar is under the game and slides in over it
       when the pointer reaches the top edge, so View and the rest stay
       reachable. While the game holds the cursor for mouse-look they are
       hidden outright: auto-hide reveals them when a pinned pointer pushes
       against the edge, however far inside the clip keeps it. Only while the
       displays are uncaptured: a captured display puts the window above the
       menu bar already, and switching the options under it leaves a black
       strip across the top with every click that far off. AppKit raises on AutoHideMenuBar without
       AutoHideDock, and on HideMenuBar without HideDock. */
    - (void) updatePresentationOptionsForActive:(BOOL)active
    {
        NSWindow* key = [NSApp keyWindow];
        NSApplicationPresentationOptions options = NSApplicationPresentationDefault;

        /* Native full screen (the green button) sets its own options, and
           the title bar and menu bar slide in at the top edge only while
           AppKit keeps them. Replacing them there — as every cursor show,
           hide and clip change asks — leaves no way out of full screen. */
        if (([NSApp presentationOptions] & NSApplicationPresentationFullScreen) ||
            ([key styleMask] & NSWindowStyleMaskFullScreen))
            return;

        if (active && [key isKindOfClass:[WineWindow class]] && [(WineWindow*)key isFullscreen] &&
            [key isOnActiveSpace])
        {
            if ([self cursorHeldForMouseLook] && ![self areDisplaysCaptured])
                options = NSApplicationPresentationHideMenuBar | NSApplicationPresentationHideDock;
            else
                options = NSApplicationPresentationAutoHideMenuBar | NSApplicationPresentationAutoHideDock;
        }

        if ([NSApp presentationOptions] == options) return;
        [NSApp setPresentationOptions:options];
        if (presentation_log_on)
            fprintf(stderr, "sevo:presentation %p menubar=%s\n", key,
                    options == NSApplicationPresentationDefault ? "shown" :
                    (options & NSApplicationPresentationHideMenuBar) ? "hidden" : "autohide");
    }

    - (void) updatePresentationOptions
    {
        [self updatePresentationOptionsForActive:[NSApp isActive]];
    }

    - (void) updateFullscreenWindows
    {
        if (capture_displays_for_fullscreen && [NSApp isActive])
        {
            BOOL anyFullscreen = FALSE;
            NSNumber* windowNumber;
            for (windowNumber in [NSWindow windowNumbersWithOptions:0])
            {
                WineWindow* window = (WineWindow*)[NSApp windowWithWindowNumber:[windowNumber integerValue]];
                if ([window isKindOfClass:[WineWindow class]] && window.fullscreen)
                {
                    anyFullscreen = TRUE;
                    break;
                }
            }

            if (anyFullscreen)
            {
                if ([self areDisplaysCaptured] || CGCaptureAllDisplays() == CGDisplayNoErr)
                    displaysCapturedForFullscreen = TRUE;
            }
            else if (displaysCapturedForFullscreen)
            {
                if ([originalDisplayModes count] || CGReleaseAllDisplays() == CGDisplayNoErr)
                    displaysCapturedForFullscreen = FALSE;
            }
        }
    }

    - (void) activeSpaceDidChange
    {
        [self updateFullscreenWindows];
        [self adjustWindowLevels];
    }

    - (void) sendDisplaysChanged:(BOOL)activating
    {
        macdrv_event* event;
        WineEventQueue* queue;

        event = macdrv_create_event(DISPLAYS_CHANGED, nil);
        event->displays_changed.activating = activating;

        [eventQueuesLock lock];

        // If we're activating, then we just need one of our threads to get the
        // event, so it can send it directly to the desktop window.  Otherwise,
        // we need all of the threads to get it because we don't know which owns
        // the desktop window and only that one will do anything with it.
        if (activating) event->deliver = 1;

        for (queue in eventQueues)
            [queue postEvent:event];
        [eventQueuesLock unlock];

        macdrv_release_event(event);
    }

    // We can compare two modes directly using CFEqual, but that may require that
    // they are identical to a level that we don't need.  In particular, when the
    // OS switches between the integrated and discrete GPUs, the set of display
    // modes can change in subtle ways.  We're interested in whether two modes
    // match in their most salient features, even if they aren't identical.
    - (BOOL) mode:(CGDisplayModeRef)mode1 matchesMode:(CGDisplayModeRef)mode2
    {
        NSString *encoding1, *encoding2;
        uint32_t ioflags1, ioflags2, different;
        double refresh1, refresh2;

        if (CGDisplayModeGetWidth(mode1) != CGDisplayModeGetWidth(mode2)) return FALSE;
        if (CGDisplayModeGetHeight(mode1) != CGDisplayModeGetHeight(mode2)) return FALSE;
        if (CGDisplayModeGetPixelWidth(mode1) != CGDisplayModeGetPixelWidth(mode2)) return FALSE;
        if (CGDisplayModeGetPixelHeight(mode1) != CGDisplayModeGetPixelHeight(mode2)) return FALSE;

        encoding1 = [(NSString*)CGDisplayModeCopyPixelEncoding(mode1) autorelease];
        encoding2 = [(NSString*)CGDisplayModeCopyPixelEncoding(mode2) autorelease];
        if (![encoding1 isEqualToString:encoding2]) return FALSE;

        ioflags1 = CGDisplayModeGetIOFlags(mode1);
        ioflags2 = CGDisplayModeGetIOFlags(mode2);
        different = ioflags1 ^ ioflags2;
        if (different & (kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeStretchedFlag |
                         kDisplayModeInterlacedFlag | kDisplayModeTelevisionFlag))
            return FALSE;

        refresh1 = CGDisplayModeGetRefreshRate(mode1);
        if (refresh1 == 0) refresh1 = 60;
        refresh2 = CGDisplayModeGetRefreshRate(mode2);
        if (refresh2 == 0) refresh2 = 60;
        if (fabs(refresh1 - refresh2) > 0.1) return FALSE;

        return TRUE;
    }

    - (NSArray*)modesMatchingMode:(CGDisplayModeRef)mode forDisplay:(CGDirectDisplayID)displayID
    {
        NSMutableArray* ret = [NSMutableArray array];
        NSDictionary* options = @{ (NSString*)kCGDisplayShowDuplicateLowResolutionModes: @YES };

        NSArray *modes = [(NSArray*)CGDisplayCopyAllDisplayModes(displayID, (CFDictionaryRef)options) autorelease];
        for (id candidateModeObject in modes)
        {
            CGDisplayModeRef candidateMode = (CGDisplayModeRef)candidateModeObject;
            if ([self mode:candidateMode matchesMode:mode])
                [ret addObject:candidateModeObject];
        }
        return ret;
    }

    - (BOOL) setMode:(CGDisplayModeRef)mode forDisplay:(CGDirectDisplayID)displayID
    {
        BOOL ret = FALSE;
        NSNumber* displayIDKey = [NSNumber numberWithUnsignedInt:displayID];
        CGDisplayModeRef originalMode;

        originalMode = (CGDisplayModeRef)originalDisplayModes[displayIDKey];

        if (originalMode && [self mode:mode matchesMode:originalMode])
        {
            if ([originalDisplayModes count] == 1) // If this is the last changed display, do a blanket reset
            {
                CGRestorePermanentDisplayConfiguration();
                if (!displaysCapturedForFullscreen)
                    CGReleaseAllDisplays();
                [originalDisplayModes removeAllObjects];
                ret = TRUE;
            }
            else // ... otherwise, try to restore just the one display
            {
                for (id modeObject in [self modesMatchingMode:mode forDisplay:displayID])
                {
                    mode = (CGDisplayModeRef)modeObject;
                    if (CGDisplaySetDisplayMode(displayID, mode, NULL) == CGDisplayNoErr)
                    {
                        [originalDisplayModes removeObjectForKey:displayIDKey];
                        ret = TRUE;
                        break;
                    }
                }
            }
        }
        else
        {
            CGDisplayModeRef currentMode;
            NSArray* modes;

            currentMode = CGDisplayModeRetain((CGDisplayModeRef)latentDisplayModes[displayIDKey]);
            if (!currentMode)
                currentMode = CGDisplayCopyDisplayMode(displayID);
            if (!currentMode) // Invalid display ID
                return FALSE;

            if ([self mode:mode matchesMode:currentMode]) // Already there!
            {
                CGDisplayModeRelease(currentMode);
                return TRUE;
            }

            CGDisplayModeRelease(currentMode);
            currentMode = NULL;

            modes = [self modesMatchingMode:mode forDisplay:displayID];
            if (!modes.count)
                return FALSE;

            [self transformProcessToForeground:YES];

            BOOL active = [NSApp isActive];

            if ([originalDisplayModes count] || displaysCapturedForFullscreen ||
                !active || CGCaptureAllDisplays() == CGDisplayNoErr)
            {
                if (active)
                {
                    // If we get here, we have the displays captured.  If we don't
                    // know the original mode of the display, the current mode must
                    // be the original.  We should re-query the current mode since
                    // another process could have changed it between when we last
                    // checked and when we captured the displays.
                    if (!originalMode)
                        originalMode = currentMode = CGDisplayCopyDisplayMode(displayID);

                    if (originalMode)
                    {
                        for (id modeObject in modes)
                        {
                            mode = (CGDisplayModeRef)modeObject;
                            if (CGDisplaySetDisplayMode(displayID, mode, NULL) == CGDisplayNoErr)
                            {
                                ret = TRUE;
                                break;
                            }
                        }
                    }
                    if (ret && !(currentMode && [self mode:mode matchesMode:currentMode]))
                        [originalDisplayModes setObject:(id)originalMode forKey:displayIDKey];
                    else if (![originalDisplayModes count])
                    {
                        CGRestorePermanentDisplayConfiguration();
                        if (!displaysCapturedForFullscreen)
                            CGReleaseAllDisplays();
                    }

                    if (currentMode)
                        CGDisplayModeRelease(currentMode);
                }
                else
                {
                    [latentDisplayModes setObject:(id)mode forKey:displayIDKey];
                    ret = TRUE;
                }
            }
        }

        if (ret)
            [self adjustWindowLevels];

        return ret;
    }

    - (BOOL) areDisplaysCaptured
    {
        return ([originalDisplayModes count] > 0 || displaysCapturedForFullscreen);
    }

    - (void) updateCursor:(BOOL)force
    {
        if (force || lastTargetWindow)
        {
            if (clientWantsCursorHidden && !cursorHidden)
            {
                [NSCursor hide];
                cursorHidden = TRUE;
            }

            if (!cursorIsCurrent)
            {
                [cursor set];
                cursorIsCurrent = TRUE;
            }

            if (!clientWantsCursorHidden && cursorHidden)
            {
                [NSCursor unhide];
                cursorHidden = FALSE;
            }
        }
        else
        {
            if (cursorIsCurrent)
            {
                [[NSCursor arrowCursor] set];
                cursorIsCurrent = FALSE;
            }
            if (cursorHidden)
            {
                [NSCursor unhide];
                cursorHidden = FALSE;
            }
        }
    }

    /* A clip that is being held: because the cursor is visible over a window
       shown in a window, or because the handler had no window of ours to tie
       a confinement rect to. Both change with activation or with the cursor
       hiding, and this is what those moments call. */
    - (void) applyDeferredClip
    {
        CGRect rect = deferredClipRect;

        if (!hasDeferredClip) return;
        hasDeferredClip = FALSE;
        [self startClippingCursor:rect];
    }

    - (void) hideCursor
    {
        if (!clientWantsCursorHidden)
        {
            clientWantsCursorHidden = TRUE;
            [self updateCursor:TRUE];
            [self applyDeferredClip];
            [self updatePresentationOptions];
        }
    }

    - (void) unhideCursor
    {
        if (clientWantsCursorHidden)
        {
            clientWantsCursorHidden = FALSE;
            [self updateCursor:FALSE];
            if (self.clippingCursor && ([self keyWindowIsPresentedInWindow] || screenClip))
            {
                CGRect rect = screenClip ? screenClipRect : clipCursorHandler.cursorClipRect;
                [self stopClippingCursor];
                deferredClipRect = rect;
                hasDeferredClip = TRUE;
            }
            [self updatePresentationOptions];
        }
    }

    - (void) setCursor:(NSCursor*)newCursor
    {
        if (newCursor != cursor)
        {
            [cursor release];
            cursor = [newCursor retain];
            cursorIsCurrent = FALSE;
            [self updateCursor:FALSE];
        }
    }

    - (void) setCursor
    {
        NSDictionary* frame = cursorFrames[cursorFrame];
        CGImageRef cgimage = (CGImageRef)frame[@"image"];
        CGSize size = CGSizeMake(CGImageGetWidth(cgimage), CGImageGetHeight(cgimage));
        NSImage* image = [[NSImage alloc] initWithCGImage:cgimage size:NSSizeFromCGSize(cgsize_mac_from_win(size))];
        CFDictionaryRef hotSpotDict = (CFDictionaryRef)frame[@"hotSpot"];
        CGPoint hotSpot;

        if (!CGPointMakeWithDictionaryRepresentation(hotSpotDict, &hotSpot))
            hotSpot = CGPointZero;
        hotSpot = cgpoint_mac_from_win(hotSpot);
        self.cursor = [[[NSCursor alloc] initWithImage:image hotSpot:NSPointFromCGPoint(hotSpot)] autorelease];
        [image release];
        [self unhideCursor];
    }

    - (void) nextCursorFrame:(NSTimer*)theTimer
    {
        NSDictionary* frame;
        NSTimeInterval duration;
        NSDate* date;

        cursorFrame++;
        if (cursorFrame >= [cursorFrames count])
            cursorFrame = 0;
        [self setCursor];

        frame = cursorFrames[cursorFrame];
        duration = [frame[@"duration"] doubleValue];
        date = [[theTimer fireDate] dateByAddingTimeInterval:duration];
        [cursorTimer setFireDate:date];
    }

    - (void) setCursorWithFrames:(NSArray*)frames
    {
        if (self.cursorFrames == frames || [self.cursorFrames isEqualToArray:frames])
            return;

        self.cursorFrames = frames;
        cursorFrame = 0;
        [cursorTimer invalidate];
        self.cursorTimer = nil;

        if ([frames count])
        {
            if ([frames count] > 1)
            {
                NSDictionary* frame = frames[0];
                NSTimeInterval duration = [frame[@"duration"] doubleValue];
                NSDate* date = [NSDate dateWithTimeIntervalSinceNow:duration];
                self.cursorTimer = [[[NSTimer alloc] initWithFireDate:date
                                                             interval:1000000
                                                               target:self
                                                             selector:@selector(nextCursorFrame:)
                                                             userInfo:nil
                                                              repeats:YES] autorelease];
                [[NSRunLoop currentRunLoop] addTimer:cursorTimer forMode:NSRunLoopCommonModes];
            }

            [self setCursor];
        }
    }

    - (void) setApplicationIconFromCGImageArray:(NSArray*)images
    {
        NSImage* nsimage = nil;

        if ([images count])
        {
            NSSize bestSize = NSZeroSize;
            id image;

            nsimage = [[[NSImage alloc] initWithSize:NSZeroSize] autorelease];

            for (image in images)
            {
                CGImageRef cgimage = (CGImageRef)image;
                NSBitmapImageRep* imageRep = [[NSBitmapImageRep alloc] initWithCGImage:cgimage];
                if (imageRep)
                {
                    NSSize size = [imageRep size];

                    [nsimage addRepresentation:imageRep];
                    [imageRep release];

                    if (MIN(size.width, size.height) > MIN(bestSize.width, bestSize.height))
                        bestSize = size;
                }
            }

            if ([[nsimage representations] count] && bestSize.width && bestSize.height)
                [nsimage setSize:bestSize];
            else
                nsimage = nil;
        }

        self.applicationIcon = nsimage;
    }

    - (void) handleCommandTab
    {
        if ([NSApp isActive])
        {
            NSRunningApplication* thisApp = [NSRunningApplication currentApplication];
            NSRunningApplication* app;
            NSRunningApplication* otherValidApp = nil;

            if ([originalDisplayModes count] || displaysCapturedForFullscreen)
            {
                NSNumber* displayID;
                for (displayID in originalDisplayModes)
                {
                    CGDisplayModeRef mode = CGDisplayCopyDisplayMode([displayID unsignedIntValue]);
                    [latentDisplayModes setObject:(id)mode forKey:displayID];
                    CGDisplayModeRelease(mode);
                }

                CGRestorePermanentDisplayConfiguration();
                CGReleaseAllDisplays();
                [originalDisplayModes removeAllObjects];
                displaysCapturedForFullscreen = FALSE;
            }

            for (app in [[NSWorkspace sharedWorkspace] runningApplications])
            {
                if (![app isEqual:thisApp] && !app.terminated &&
                    app.activationPolicy == NSApplicationActivationPolicyRegular)
                {
                    if (!app.hidden)
                    {
                        // There's another visible app.  Just hide ourselves and let
                        // the system activate the other app.
                        [NSApp hide:self];
                        return;
                    }

                    if (!otherValidApp)
                        otherValidApp = app;
                }
            }

            // Didn't find a visible GUI app.  Try the Finder or, if that's not
            // running, the first hidden GUI app.  If even that doesn't work, we
            // just fail to switch and remain the active app.
            app = [[NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.finder"] lastObject];
            if (!app) app = otherValidApp;
            [app unhide];
            [app activateWithOptions:0];
        }
    }

    - (BOOL) setCursorPosition:(CGPoint)pos
    {
        BOOL ret;

        if ([windowsBeingDragged count])
            return FALSE;

        /* The clip decides where the cursor ends up, and the queued events
           below are rewritten to that point. */
        if (self.clippingCursor)
            [clipCursorHandler clipCursorLocation:&pos];

        if (self.clippingCursor && [clipCursorHandler respondsToSelector:@selector(setCursorPosition:)])
            ret = [clipCursorHandler setCursorPosition:pos];
        else
        {
            // Annoyingly, CGWarpMouseCursorPosition() effectively disassociates
            // the mouse from the cursor position for 0.25 seconds.  This means
            // that mouse movement during that interval doesn't move the cursor
            // and events carry a constant location (the warped-to position)
            // even though they have delta values.  For apps which warp the
            // cursor frequently (like after every mouse move), this makes
            // cursor movement horribly laggy and jerky, as only a fraction of
            // mouse move events have any effect.
            //
            // On some versions of OS X, it's sufficient to forcibly reassociate
            // the mouse and cursor position.  On others, it's necessary to set
            // the local events suppression interval to 0 for the warp.  That's
            // deprecated, but I'm not aware of any other way.  For good
            // measure, we do both.
            CGSetLocalEventsSuppressionInterval(0);
            ret = (CGWarpMouseCursorPosition(pos) == kCGErrorSuccess);
            CGSetLocalEventsSuppressionInterval(0.25);
            if (ret)
            {
                lastSetCursorPositionTime = [[NSProcessInfo processInfo] systemUptime];

                CGAssociateMouseAndMouseCursorPosition(true);
            }
        }

        if (ret)
        {
            WineEventQueue* queue;

            // Discard all pending mouse move events.
            [eventQueuesLock lock];
            for (queue in eventQueues)
            {
                [queue discardEventsMatchingMask:event_mask_for_type(MOUSE_MOVED_RELATIVE) |
                                                 event_mask_for_type(MOUSE_MOVED_ABSOLUTE)
                                       forWindow:nil];
                [queue resetMouseEventPositions:pos];
            }
            [eventQueuesLock unlock];
        }

        return ret;
    }

    - (void) updateWindowsForCursorClipping
    {
        WineWindow* window;
        for (window in [NSApp windows])
        {
            if ([window isKindOfClass:[WineWindow class]])
                [window updateForCursorClipping];
        }
    }

    /* How far a whole-screen clip keeps the cursor from the screen's edges,
       in points. The menu bar and the Dock are hidden while the cursor is
       held (updatePresentationOptionsForActive:), since a pointer pushing
       against the edge reveals them at any inset. */
    static const CGFloat screenClipInset = 1;

    /* Whether a clip that covers every screen is the game holding the cursor
       inside its own fullscreen window: the app is active and its key window
       is a fullscreen Wine window whose frame contains the rect. */
    - (BOOL) isScreenClip:(NSRect)rect
    {
        NSWindow* key = [NSApp keyWindow];
        return [NSApp isActive] && [key isKindOfClass:[WineWindow class]]
            && [(WineWindow*)key isFullscreen] && NSContainsRect([key frame], rect);
    }

    - (void) setScreenClip:(BOOL)clip rect:(CGRect)rect
    {
        screenClip = clip;
        screenClipRect = rect;
    }

    /* Whether the key window is a fullscreen-style window shown in a window. */
    - (BOOL) keyWindowIsPresentedInWindow
    {
        NSWindow* key = [NSApp keyWindow];
        return [key isKindOfClass:[WineWindow class]] && [(WineWindow*)key presentationWindowed];
    }

    - (BOOL) startClippingCursor:(CGRect)rect
    {
        if (!clipCursorHandler) {
            if ([WineCursorRestrictionClipCursorHandler isAvailable])
                clipCursorHandler = [[WineCursorRestrictionClipCursorHandler alloc] init];
            else if (use_confinement_cursor_clipping && [WineConfinementClipCursorHandler isAvailable])
                clipCursorHandler = [[WineConfinementClipCursorHandler alloc] init];
            else
                clipCursorHandler = [[WineEventTapClipCursorHandler alloc] init];
        }

        /* A game shown in a window clips the cursor to its client area the
           moment it activates, and that area is the picture: the title bar
           and the buttons beside it become unreachable. While the cursor is
           visible the clip is held back; a hidden cursor — mouse look — gets
           it, and a cursor shown again is let go. */
        if (!clientWantsCursorHidden && ([self keyWindowIsPresentedInWindow] || screenClip))
        {
            if (self.clippingCursor) [self stopClippingCursor];
            deferredClipRect = rect;
            hasDeferredClip = TRUE;
            return TRUE;
        }
        hasDeferredClip = FALSE;

        if (screenClip)
            rect = CGRectInset(rect, screenClipInset, screenClipInset);

        if (self.clippingCursor && CGRectEqualToRect(rect, clipCursorHandler.cursorClipRect))
            return TRUE;

        if (![clipCursorHandler startClippingCursor:rect])
        {
            /* A confinement rect has to be tied to a window of ours that is in
               front, and a game that calls ClipCursor before its window
               activates has none — the first thing many of them do. The rect
               is kept so the activation applies it: such a game never asks
               again. */
            deferredClipRect = rect;
            hasDeferredClip = TRUE;
            return FALSE;
        }

        [self setCursorPosition:NSPointToCGPoint([self flippedMouseLocation:[NSEvent mouseLocation]])];

        [self updateWindowsForCursorClipping];
        [self updatePresentationOptions];

        return TRUE;
    }

    - (BOOL) stopClippingCursor
    {
        hasDeferredClip = FALSE;
        if (!self.clippingCursor)
            return TRUE;

        if (![clipCursorHandler stopClippingCursor])
            return FALSE;

        lastSetCursorPositionTime = [[NSProcessInfo processInfo] systemUptime];

        [self updateWindowsForCursorClipping];
        [self updatePresentationOptions];

        return TRUE;
    }

    - (BOOL) clippingCursor
    {
        return clipCursorHandler.clippingCursor;
    }

    /* A game holds the cursor for mouse-look: clipped so it cannot leave and
       hidden because the camera is the pointer. */
    - (BOOL) cursorHeldForMouseLook
    {
        return self.clippingCursor && clientWantsCursorHidden;
    }

    /* An NSEvent's delta is the hand's motion run through the system's
       pointer-acceleration curve: the same sweep covered fast turns a camera
       further than the same sweep covered slowly. The CGEvent also carries
       the device's own displacement, which a camera wants. Synthetic events
       — accessibility tools, remote play, a tablet — carry no such
       displacement, so a zero pair means the event did not come from a
       mouse and the delta stands. Returns TRUE when the pair is the
       device's own displacement. */
    - (BOOL) mouseDeltaOfEvent:(NSEvent*)anEvent x:(CGFloat*)dx y:(CGFloat*)dy
    {
        if (linear_mouse && [self cursorHeldForMouseLook])
        {
            CGEventRef cgevent = [anEvent CGEvent];
            if (cgevent)
            {
                double rawX = CGEventGetDoubleValueField(cgevent, kCGEventUnacceleratedPointerMovementX);
                double rawY = CGEventGetDoubleValueField(cgevent, kCGEventUnacceleratedPointerMovementY);

                if (rawX || rawY)
                {
                    *dx = rawX;
                    *dy = rawY;
                    return TRUE;
                }
            }
        }

        *dx = [anEvent deltaX];
        *dy = [anEvent deltaY];
        return FALSE;
    }


    - (BOOL) isKeyPressed:(uint16_t)keyCode
    {
        int bits = sizeof(pressedKeyCodes[0]) * 8;
        int index = keyCode / bits;
        uint32_t mask = 1 << (keyCode % bits);
        return (pressedKeyCodes[index] & mask) != 0;
    }

    - (void) noteKey:(uint16_t)keyCode pressed:(BOOL)pressed
    {
        int bits = sizeof(pressedKeyCodes[0]) * 8;
        int index = keyCode / bits;
        uint32_t mask = 1 << (keyCode % bits);
        if (pressed)
            pressedKeyCodes[index] |= mask;
        else
            pressedKeyCodes[index] &= ~mask;
    }

    - (void) window:(WineWindow*)window isBeingDragged:(BOOL)dragged
    {
        if (dragged)
            [windowsBeingDragged addObject:window];
        else
            [windowsBeingDragged removeObject:window];
    }

    - (void) windowWillOrderOut:(WineWindow*)window
    {
        if ([windowsBeingDragged containsObject:window])
        {
            [self window:window isBeingDragged:NO];

            macdrv_event* event = macdrv_create_event(WINDOW_DRAG_END, window);
            [window.queue postEvent:event];
            macdrv_release_event(event);
        }
    }

    - (BOOL) isAnyWineWindowVisible
    {
        for (WineWindow* w in [NSApp windows])
        {
            if ([w isKindOfClass:[WineWindow class]] && ![w isMiniaturized] && [w isVisible] && [w presentsVisibleContent])
                return YES;
        }

        return NO;
    }

    - (void) handleWindowDrag:(WineWindow*)window begin:(BOOL)begin
    {
        macdrv_event* event;
        int eventType;

        if (begin)
        {
            [windowsBeingDragged addObject:window];
            eventType = WINDOW_DRAG_BEGIN;
        }
        else
        {
            [windowsBeingDragged removeObject:window];
            eventType = WINDOW_DRAG_END;
        }

        event = macdrv_create_event(eventType, window);
        if (eventType == WINDOW_DRAG_BEGIN)
            event->window_drag_begin.no_activate = [NSEvent wine_commandKeyDown];
        [window.queue postEvent:event];
        macdrv_release_event(event);
    }

    - (void) handleMouseMove:(NSEvent*)anEvent
    {
        WineWindow* targetWindow;
        BOOL drag = [anEvent type] != NSEventTypeMouseMoved;

        if ([windowsBeingDragged count])
            targetWindow = nil;
        else if (mouseCaptureWindow)
            targetWindow = mouseCaptureWindow;
        else if (drag)
            targetWindow = (WineWindow*)[anEvent window];
        else
        {
            /* Because of the way -[NSWindow setAcceptsMouseMovedEvents:] works, the
               event indicates its window is the main window, even if the cursor is
               over a different window.  Find the actual WineWindow that is under the
               cursor and post the event as being for that window. */
            CGPoint cgpoint = CGEventGetLocation([anEvent CGEvent]);
            NSPoint point = [self flippedMouseLocation:NSPointFromCGPoint(cgpoint)];
            NSInteger windowUnderNumber;

            windowUnderNumber = [NSWindow windowNumberAtPoint:point
                                  belowWindowWithWindowNumber:0];
            targetWindow = (WineWindow*)[NSApp windowWithWindowNumber:windowUnderNumber];
            if (!NSMouseInRect(point, [targetWindow contentRectForFrameRect:[targetWindow frame]], NO))
                targetWindow = nil;
        }

        if ([targetWindow isKindOfClass:[WineWindow class]])
        {
            CGPoint point = CGEventGetLocation([anEvent CGEvent]);
            macdrv_event* event;
            BOOL absolute;

            // If we recently warped the cursor (other than in our cursor-clipping
            // event tap), discard mouse move events until we see an event which is
            // later than that time.
            if (lastSetCursorPositionTime)
            {
                if ([anEvent timestamp] <= lastSetCursorPositionTime)
                    return;

                lastSetCursorPositionTime = 0;
                forceNextMouseMoveAbsolute = TRUE;
            }

            if (forceNextMouseMoveAbsolute || targetWindow != lastTargetWindow)
            {
                absolute = TRUE;
                forceNextMouseMoveAbsolute = FALSE;
            }
            else
            {
                // Send absolute move events if the cursor is in the interior of
                // its range.  Only send relative moves if the cursor is pinned to
                // the boundaries of where it can go.  We compute the position
                // that's one additional point in the direction of movement.  If
                // that is outside of the clipping rect or desktop region (the
                // union of the screen frames), then we figure the cursor would
                // have moved outside if it could but it was pinned.
                CGPoint computedPoint = point;
                CGFloat deltaX = [anEvent deltaX];
                CGFloat deltaY = [anEvent deltaY];

                if (deltaX > 0.001)
                    computedPoint.x++;
                else if (deltaX < -0.001)
                    computedPoint.x--;

                if (deltaY > 0.001)
                    computedPoint.y++;
                else if (deltaY < -0.001)
                    computedPoint.y--;

                // Assume cursor is pinned for now
                absolute = FALSE;
                if (!self.clippingCursor || CGRectContainsPoint(clipCursorHandler.cursorClipRect, computedPoint))
                {
                    const CGRect* rects;
                    NSUInteger count, i;

                    // Caches screenFrameCGRects if necessary
                    [self primaryScreenHeight];

                    rects = [screenFrameCGRects bytes];
                    count = [screenFrameCGRects length] / sizeof(rects[0]);

                    for (i = 0; i < count; i++)
                    {
                        if (CGRectContainsPoint(rects[i], computedPoint))
                        {
                            absolute = TRUE;
                            break;
                        }
                    }
                }
            }

            if (absolute)
            {
                if (self.clippingCursor)
                    [clipCursorHandler clipCursorLocation:&point];
                point = [targetWindow winePointFromScreenPoint:point];
                point = cgpoint_win_from_mac(point);

                event = macdrv_create_event(MOUSE_MOVED_ABSOLUTE, targetWindow);
                event->mouse_moved.x = floor(point.x);
                event->mouse_moved.y = floor(point.y);

                mouseMoveDeltaX = 0;
                mouseMoveDeltaY = 0;
            }
            else
            {
                double scale;
                CGFloat moveX, moveY;
                BOOL raw;

                raw = [self mouseDeltaOfEvent:anEvent x:&moveX y:&moveY];

                /* A scaled window's point is worth less than one of Wine's.
                   Device displacement is already in the units a camera
                   reads, whatever the window's size. */
                scale = raw ? 1 : (retina_on ? 2 : 1) / [targetWindow presentationScale];

                /* Add event delta to accumulated delta error */
                /* deltaY is already flipped */
                mouseMoveDeltaX += moveX;
                mouseMoveDeltaY += moveY;

                event = macdrv_create_event(MOUSE_MOVED_RELATIVE, targetWindow);
                event->mouse_moved.x = mouseMoveDeltaX * scale;
                event->mouse_moved.y = mouseMoveDeltaY * scale;

                /* Keep the remainder after integer truncation. */
                mouseMoveDeltaX -= event->mouse_moved.x / scale;
                mouseMoveDeltaY -= event->mouse_moved.y / scale;
            }

            if (event->type == MOUSE_MOVED_ABSOLUTE || event->mouse_moved.x || event->mouse_moved.y)
            {
                event->mouse_moved.time_ms = [self ticksForEventTime:[anEvent timestamp]];
                event->mouse_moved.drag = drag;

                [targetWindow.queue postEvent:event];
            }

            macdrv_release_event(event);

            lastTargetWindow = targetWindow;
        }
        else
            lastTargetWindow = nil;

        [self updateCursor:FALSE];
    }

    - (void) handleMouseButton:(NSEvent*)theEvent
    {
        WineWindow* window = (WineWindow*)[theEvent window];
        NSEventType type = [theEvent type];
        WineWindow* windowBroughtForward = nil;
        BOOL process = FALSE;

        if ([window isKindOfClass:[WineWindow class]] &&
            type == NSEventTypeLeftMouseDown &&
            ![theEvent wine_commandKeyDown])
        {
            NSWindowButton windowButton;

            windowBroughtForward = window;

            /* Any left-click on our window anyplace other than the close or
               minimize buttons will bring it forward. */
            for (windowButton = NSWindowCloseButton;
                 windowButton <= NSWindowMiniaturizeButton;
                 windowButton++)
            {
                NSButton* button = [window standardWindowButton:windowButton];
                if (button)
                {
                    NSPoint point = [button convertPoint:[theEvent locationInWindow] fromView:nil];
                    if ([button mouse:point inRect:[button bounds]])
                    {
                        windowBroughtForward = nil;
                        break;
                    }
                }
            }
        }

        if ([windowsBeingDragged count])
            window = nil;
        else if (mouseCaptureWindow)
            window = mouseCaptureWindow;

        if ([window isKindOfClass:[WineWindow class]])
        {
            BOOL pressed = (type == NSEventTypeLeftMouseDown ||
                            type == NSEventTypeRightMouseDown ||
                            type == NSEventTypeOtherMouseDown);
            CGPoint pt = CGEventGetLocation([theEvent CGEvent]);

            if (self.clippingCursor)
                [clipCursorHandler clipCursorLocation:&pt];

            if (pressed)
            {
                if (mouseCaptureWindow)
                    process = TRUE;
                else
                {
                    // Test if the click was in the window's content area.
                    NSPoint nspoint = [self flippedMouseLocation:NSPointFromCGPoint(pt)];
                    NSRect contentRect = [window contentRectForFrameRect:[window frame]];
                    process = NSMouseInRect(nspoint, contentRect, NO);
                    if (process && [window styleMask] & NSWindowStyleMaskResizable)
                    {
                        // Ignore clicks in the grow box (resize widget).
                        HIPoint origin = { 0, 0 };
                        HIThemeGrowBoxDrawInfo info = { 0 };
                        HIRect bounds;
                        OSStatus status;

                        info.kind = kHIThemeGrowBoxKindNormal;
                        info.direction = kThemeGrowRight | kThemeGrowDown;
                        if ([window styleMask] & NSWindowStyleMaskUtilityWindow)
                            info.size = kHIThemeGrowBoxSizeSmall;
                        else
                            info.size = kHIThemeGrowBoxSizeNormal;

                        status = HIThemeGetGrowBoxBounds(&origin, &info, &bounds);
                        if (status == noErr)
                        {
                            NSRect growBox = NSMakeRect(NSMaxX(contentRect) - bounds.size.width,
                                                        NSMinY(contentRect),
                                                        bounds.size.width,
                                                        bounds.size.height);
                            process = !NSMouseInRect(nspoint, growBox, NO);
                        }
                    }
                }
                if (process)
                    unmatchedMouseDowns |= NSEventMaskFromType(type);
            }
            else
            {
                NSEventType downType = type - 1;
                NSUInteger downMask = NSEventMaskFromType(downType);
                process = (unmatchedMouseDowns & downMask) != 0;
                unmatchedMouseDowns &= ~downMask;
            }

            if (process)
            {
                macdrv_event* event;

                pt = [window winePointFromScreenPoint:pt];
                pt = cgpoint_win_from_mac(pt);

                event = macdrv_create_event(MOUSE_BUTTON, window);
                event->mouse_button.button = [theEvent buttonNumber];
                event->mouse_button.pressed = pressed;
                event->mouse_button.x = floor(pt.x);
                event->mouse_button.y = floor(pt.y);
                event->mouse_button.time_ms = [self ticksForEventTime:[theEvent timestamp]];

                [window.queue postEvent:event];

                macdrv_release_event(event);
            }
        }

        if (windowBroughtForward)
        {
            WineWindow* ancestor = [windowBroughtForward ancestorWineWindow];
            NSInteger ancestorNumber = [ancestor windowNumber];
            NSInteger ancestorLevel = [ancestor level];

            for (NSNumber* windowNumberObject in [NSWindow windowNumbersWithOptions:0])
            {
                NSInteger windowNumber = [windowNumberObject integerValue];
                if (windowNumber == ancestorNumber)
                    break;
                WineWindow* otherWindow = (WineWindow*)[NSApp windowWithWindowNumber:windowNumber];
                if ([otherWindow isKindOfClass:[WineWindow class]] && [otherWindow screen] &&
                    [otherWindow level] <= ancestorLevel && otherWindow == [otherWindow ancestorWineWindow])
                {
                    [ancestor postBroughtForwardEvent];
                    break;
                }
            }
            if (!process && ![windowBroughtForward isKeyWindow] && !windowBroughtForward.disabled && !windowBroughtForward.noForeground)
                [self windowGotFocus:windowBroughtForward];
        }

        // Since mouse button events deliver absolute cursor position, the
        // accumulating delta from move events is invalidated.  Make sure
        // next mouse move event starts over from an absolute baseline.
        // Also, it's at least possible that the title bar widgets (e.g. close
        // button, etc.) could enter an internal event loop on a mouse down that
        // wouldn't exit until a mouse up.  In that case, we'd miss any mouse
        // dragged events and, after that, any notion of the cursor position
        // computed from accumulating deltas would be wrong.
        forceNextMouseMoveAbsolute = TRUE;
    }

    - (void) handleScrollWheel:(NSEvent*)theEvent
    {
        WineWindow* window;

        if (mouseCaptureWindow)
            window = mouseCaptureWindow;
        else
            window = (WineWindow*)[theEvent window];

        if ([window isKindOfClass:[WineWindow class]])
        {
            CGEventRef cgevent = [theEvent CGEvent];
            CGPoint pt = CGEventGetLocation(cgevent);
            BOOL process;

            if (self.clippingCursor)
                [clipCursorHandler clipCursorLocation:&pt];

            if (mouseCaptureWindow)
                process = TRUE;
            else
            {
                // Only process the event if it was in the window's content area.
                NSPoint nspoint = [self flippedMouseLocation:NSPointFromCGPoint(pt)];
                NSRect contentRect = [window contentRectForFrameRect:[window frame]];
                process = NSMouseInRect(nspoint, contentRect, NO);
            }

            if (process)
            {
                macdrv_event* event;
                double x, y;
                BOOL continuous = FALSE;

                pt = [window winePointFromScreenPoint:pt];
                pt = cgpoint_win_from_mac(pt);

                event = macdrv_create_event(MOUSE_SCROLL, window);
                event->mouse_scroll.x = floor(pt.x);
                event->mouse_scroll.y = floor(pt.y);
                event->mouse_scroll.time_ms = [self ticksForEventTime:[theEvent timestamp]];

                if (CGEventGetIntegerValueField(cgevent, kCGScrollWheelEventIsContinuous))
                {
                    continuous = TRUE;

                    /* Continuous scroll wheel events come from high-precision scrolling
                       hardware like Apple's Magic Mouse, Mighty Mouse, and trackpads.
                       For these, we can get more precise data from the CGEvent API. */
                    /* Axis 1 is vertical, axis 2 is horizontal. */
                    x = CGEventGetDoubleValueField(cgevent, kCGScrollWheelEventPointDeltaAxis2);
                    y = CGEventGetDoubleValueField(cgevent, kCGScrollWheelEventPointDeltaAxis1);
                }
                else
                {
                    double pixelsPerLine = 10;
                    CGEventSourceRef source;

                    /* The non-continuous values are in units of "lines", not pixels. */
                    if ((source = CGEventCreateSourceFromEvent(cgevent)))
                    {
                        pixelsPerLine = CGEventSourceGetPixelsPerLine(source);
                        CFRelease(source);
                    }

                    x = pixelsPerLine * [theEvent deltaX];
                    y = pixelsPerLine * [theEvent deltaY];
                }

                /* Mac: negative is right or down, positive is left or up.
                   Win32: negative is left or down, positive is right or up.
                   So, negate the X scroll value to translate. */
                x = -x;

                /* The x,y values so far are in pixels.  Win32 expects to receive some
                   fraction of WHEEL_DELTA == 120.  By my estimation, that's roughly
                   6 times the pixel value. */
                x *= 6;
                y *= 6;

                if (use_precise_scrolling)
                {
                    event->mouse_scroll.x_scroll = x;
                    event->mouse_scroll.y_scroll = y;

                    if (!continuous)
                    {
                        /* For non-continuous "clicky" wheels, if there was any motion, make
                           sure there was at least WHEEL_DELTA motion.  This is so, at slow
                           speeds where the system's acceleration curve is actually reducing the
                           scroll distance, the user is sure to get some action out of each click.
                           For example, this is important for rotating though weapons in a
                           first-person shooter. */
                        if (0 < event->mouse_scroll.x_scroll && event->mouse_scroll.x_scroll < 120)
                            event->mouse_scroll.x_scroll = 120;
                        else if (-120 < event->mouse_scroll.x_scroll && event->mouse_scroll.x_scroll < 0)
                            event->mouse_scroll.x_scroll = -120;

                        if (0 < event->mouse_scroll.y_scroll && event->mouse_scroll.y_scroll < 120)
                            event->mouse_scroll.y_scroll = 120;
                        else if (-120 < event->mouse_scroll.y_scroll && event->mouse_scroll.y_scroll < 0)
                            event->mouse_scroll.y_scroll = -120;
                    }
                }
                else
                {
                    /* If it's been a while since the last scroll event or if the scrolling has
                       reversed direction, reset the accumulated scroll value. */
                    if ([theEvent timestamp] - lastScrollTime > 1)
                        accumScrollX = accumScrollY = 0;
                    else
                    {
                        /* The accumulated scroll value is in the opposite direction/sign of the last
                           scroll.  That's because it's the "debt" resulting from over-scrolling in
                           that direction.  We accumulate by adding in the scroll amount and then, if
                           it has the same sign as the scroll value, we subtract any whole or partial
                           WHEEL_DELTAs, leaving it 0 or the opposite sign.  So, the user switched
                           scroll direction if the accumulated debt and the new scroll value have the
                           same sign. */
                        if ((accumScrollX < 0 && x < 0) || (accumScrollX > 0 && x > 0))
                            accumScrollX = 0;
                        if ((accumScrollY < 0 && y < 0) || (accumScrollY > 0 && y > 0))
                            accumScrollY = 0;
                    }
                    lastScrollTime = [theEvent timestamp];

                    accumScrollX += x;
                    accumScrollY += y;

                    if (accumScrollX > 0 && x > 0)
                        event->mouse_scroll.x_scroll = 120 * ceil(accumScrollX / 120);
                    if (accumScrollX < 0 && x < 0)
                        event->mouse_scroll.x_scroll = 120 * -ceil(-accumScrollX / 120);
                    if (accumScrollY > 0 && y > 0)
                        event->mouse_scroll.y_scroll = 120 * ceil(accumScrollY / 120);
                    if (accumScrollY < 0 && y < 0)
                        event->mouse_scroll.y_scroll = 120 * -ceil(-accumScrollY / 120);

                    accumScrollX -= event->mouse_scroll.x_scroll;
                    accumScrollY -= event->mouse_scroll.y_scroll;
                }

                if (event->mouse_scroll.x_scroll || event->mouse_scroll.y_scroll)
                    [window.queue postEvent:event];

                macdrv_release_event(event);

                // Since scroll wheel events deliver absolute cursor position, the
                // accumulating delta from move events is invalidated.  Make sure next
                // mouse move event starts over from an absolute baseline.
                forceNextMouseMoveAbsolute = TRUE;
            }
        }
    }

    // Returns TRUE if the event was handled and caller should do nothing more
    // with it.  Returns FALSE if the caller should process it as normal and
    // then call -didSendEvent:.
    - (BOOL) handleEvent:(NSEvent*)anEvent
    {
        BOOL ret = FALSE;
        NSEventType type = [anEvent type];

        if (type == NSEventTypeFlagsChanged)
            self.lastFlagsChanged = anEvent;
        else if (type == NSEventTypeMouseMoved || type == NSEventTypeLeftMouseDragged ||
                 type == NSEventTypeRightMouseDragged || type == NSEventTypeOtherMouseDragged)
        {
            [self handleMouseMove:anEvent];
            ret = mouseCaptureWindow && ![windowsBeingDragged count];
        }
        else if (type == NSEventTypeLeftMouseDown || type == NSEventTypeLeftMouseUp ||
                 type == NSEventTypeRightMouseDown || type == NSEventTypeRightMouseUp ||
                 type == NSEventTypeOtherMouseDown || type == NSEventTypeOtherMouseUp)
        {
            [self handleMouseButton:anEvent];
            ret = mouseCaptureWindow && ![windowsBeingDragged count];
        }
        else if (type == NSEventTypeScrollWheel)
        {
            [self handleScrollWheel:anEvent];
            ret = mouseCaptureWindow != nil;
        }
        else if (type == NSEventTypeKeyDown)
        {
            // -[NSApplication sendEvent:] seems to consume presses of the Help
            // key (Insert key on PC keyboards), so we have to bypass it and
            // send the event directly to the window.
            if (anEvent.keyCode == kVK_Help)
            {
                [anEvent.window sendEvent:anEvent];
                ret = TRUE;
            }
        }
        else if (type == NSEventTypeKeyUp)
        {
            uint16_t keyCode = [anEvent keyCode];
            if ([self isKeyPressed:keyCode])
            {
                WineWindow* window = (WineWindow*)[anEvent window];
                [self noteKey:keyCode pressed:FALSE];
                if ([window isKindOfClass:[WineWindow class]])
                    [window postKeyEvent:anEvent];
            }
        }

        return ret;
    }

    - (void) didSendEvent:(NSEvent*)anEvent
    {
        NSEventType type = [anEvent type];

        if (type == NSEventTypeKeyDown && ![anEvent isARepeat] && [anEvent keyCode] == kVK_Tab)
        {
            NSUInteger modifiers = [anEvent modifierFlags];
            if ((modifiers & NSEventModifierFlagCommand) &&
                !(modifiers & (NSEventModifierFlagControl | NSEventModifierFlagOption)))
            {
                // Command-Tab and Command-Shift-Tab would normally be intercepted
                // by the system to switch applications.  If we're seeing it, it's
                // presumably because we've captured the displays, preventing
                // normal application switching.  Do it manually.
                [self handleCommandTab];
            }
        }
    }

    - (void) setupObservations
    {
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        NSNotificationCenter* wsnc = [[NSWorkspace sharedWorkspace] notificationCenter];
        NSDistributedNotificationCenter* dnc = [NSDistributedNotificationCenter defaultCenter];

        [nc addObserverForName:NSWindowDidBecomeKeyNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification *note){
            NSWindow* window = [note object];
            [keyWindows removeObjectIdenticalTo:window];
            [keyWindows insertObject:window atIndex:0];
        }];

        [nc addObserverForName:NSWindowWillCloseNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *note){
            NSWindow* window = [note object];
            if ([window isKindOfClass:[WineWindow class]] && [(WineWindow*)window isFakingClose])
                return;
            [keyWindows removeObjectIdenticalTo:window];
            if (window == lastTargetWindow)
                lastTargetWindow = nil;
            if (window == self.mouseCaptureWindow)
                self.mouseCaptureWindow = nil;
            if ([window isKindOfClass:[WineWindow class]] && [(WineWindow*)window isFullscreen])
            {
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 0), dispatch_get_main_queue(), ^{
                    [self updateFullscreenWindows];
                });
            }
            [windowsBeingDragged removeObject:window];
        }];

        [nc addObserverForName:NSWindowWillStartDraggingNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *note){
            NSWindow* window = [note object];
            if ([window isKindOfClass:[WineWindow class]])
                [self handleWindowDrag:(WineWindow *)window begin:YES];
        }];

        [nc addObserverForName:NSWindowDidEndDraggingNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *note){
            NSWindow* window = [note object];
            if ([window isKindOfClass:[WineWindow class]])
                [self handleWindowDrag:(WineWindow *)window begin:NO];
        }];

        [nc addObserver:self
               selector:@selector(keyboardSelectionDidChange)
                   name:NSTextInputContextKeyboardSelectionDidChangeNotification
                 object:nil];

        /* The above notification isn't sent unless the NSTextInputContext
           class has initialized itself.  Poke it. */
        [NSTextInputContext self];

        [wsnc addObserver:self
                 selector:@selector(activeSpaceDidChange)
                     name:NSWorkspaceActiveSpaceDidChangeNotification
                   object:nil];

        [nc addObserver:self
               selector:@selector(releaseMouseCapture)
                   name:NSMenuDidBeginTrackingNotification
                 object:nil];

        [dnc        addObserver:self
                       selector:@selector(releaseMouseCapture)
                           name:@"com.apple.HIToolbox.beginMenuTrackingNotification"
                         object:nil
             suspensionBehavior:NSNotificationSuspensionBehaviorDrop];

        [dnc addObserver:self
                selector:@selector(enabledKeyboardInputSourcesChanged)
                    name:(NSString*)kTISNotifyEnabledKeyboardInputSourcesChanged
                  object:nil];

        if ([NSApplication instancesRespondToSelector:@selector(yieldActivationToApplication:)])
        {
            /* App activation cooperation, starting in macOS 14 Sonoma. */
            [dnc addObserver:self
                    selector:@selector(otherWineAppWillActivate:)
                        name:WineAppWillActivateNotification
                      object:nil
          suspensionBehavior:NSNotificationSuspensionBehaviorDeliverImmediately];
        }
    }

    - (void) otherWineAppWillActivate:(NSNotification *)note
    {
        NSProcessInfo *ourProcess;
        pid_t otherPID;
        NSString *ourConfigDir, *otherConfigDir, *ourPrefix, *otherPrefix;
        NSRunningApplication *otherApp;

        /* No point in yielding if we're not the foreground app. */
        if (![NSApp isActive]) return;

        /* Ignore requests from ourself, dead processes, and other prefixes. */
        ourProcess = [NSProcessInfo processInfo];
        otherPID = [note.userInfo[WineActivatingAppPIDKey] integerValue];
        if (otherPID == ourProcess.processIdentifier) return;

        otherApp = [NSRunningApplication runningApplicationWithProcessIdentifier:otherPID];
        if (!otherApp) return;

        ourConfigDir = ourProcess.environment[@"WINECONFIGDIR"];
        otherConfigDir = note.userInfo[WineActivatingAppConfigDirKey];
        if (ourConfigDir.length && otherConfigDir.length &&
            ![ourConfigDir isEqualToString:otherConfigDir])
        {
            return;
        }

        ourPrefix = ourProcess.environment[@"WINEPREFIX"];
        otherPrefix = note.userInfo[WineActivatingAppPrefixKey];
        if (ourPrefix.length && otherPrefix.length &&
            ![ourPrefix isEqualToString:otherPrefix])
        {
            return;
        }

        /* There's a race condition here. The requesting app sends out
           WineAppWillActivateNotification and then activates itself, but since
           distributed notifications are asynchronous, we may not have yielded
           in time. So we call activateFromApplication: on the other app here,
           which will work around that race if it happened. If we didn't hit the
           race, the activateFromApplication: call will be a no-op. */

        /* We only add this observer if NSApplication responds to the yield
           methods, so they're safe to call without checking here. */
        [NSApp yieldActivationToApplication:otherApp];
        [otherApp activateFromApplication:[NSRunningApplication currentApplication]
                                  options:0];
    }

    - (void) tryToActivateIgnoringOtherApps:(BOOL)ignore
    {
        NSProcessInfo *processInfo;
        NSString *configDir, *prefix;
        NSDictionary *userInfo;

        if ([NSApp isActive]) return;  /* Nothing to do. */

        if (!ignore ||
            ![NSApplication instancesRespondToSelector:@selector(yieldActivationToApplication:)])
        {
            /* Either we don't need to force activation, or the OS is old enough
               that this is our only option. */
            [NSApp activateIgnoringOtherApps:ignore];
            return;
        }

        /* Ask other Wine apps to yield activation to us. */
        processInfo = [NSProcessInfo processInfo];
        configDir = processInfo.environment[@"WINECONFIGDIR"];
        prefix = processInfo.environment[@"WINEPREFIX"];
        userInfo = @{
            WineActivatingAppPIDKey: @(processInfo.processIdentifier),
            WineActivatingAppPrefixKey: prefix ? prefix : @"",
            WineActivatingAppConfigDirKey: configDir ? configDir : @""
        };

        [[NSDistributedNotificationCenter defaultCenter]
            postNotificationName:WineAppWillActivateNotification
                          object:nil
                        userInfo:userInfo
              deliverImmediately:YES];

        /* This is racy. See the note in otherWineAppWillActivate:. */
        [NSApp activate];
     }

    static BOOL InputSourceShouldBeIgnored(TISInputSourceRef inputSource)
    {
        /* Certain system utilities are technically input sources, but we
           shouldn't consider them as such for our purposes. */
        static CFStringRef ignoredIDs[] = {
            /* The "Emoji & Symbols" palette. */
            CFSTR("com.apple.CharacterPaletteIM"),
            /* The on-screen keyboard and accessibility panel. */
            CFSTR("com.apple.inputmethod.AssistiveControl"),
            /* The popup for accented characters when you hold down a key. */
            CFSTR("com.apple.PressAndHold"),
            /* Emoji list on MacBooks with the Touch Bar. */
            CFSTR("com.apple.inputmethod.EmojiFunctionRowItem"),
            /* Dictation. Ideally this would actually receive key events, since
               escape cancels it, but it remains a "selected" input source even
               when not active, so we need to ignore it to avoid incorrectly
               sending input to it. */
            CFSTR("com.apple.inputmethod.ironwood"),
        };

        CFStringRef sourceID = TISGetInputSourceProperty(inputSource, kTISPropertyInputSourceID);
        for (int i = 0; i < sizeof(ignoredIDs) / sizeof(CFStringRef); i++)
        {
            if (CFEqual(sourceID, ignoredIDs[i]))
                return YES;
        }

        return NO;
    }

    - (BOOL) inputSourceIsInputMethod
    {
        static dispatch_once_t onceToken;
        static CFDictionaryRef filterDict;
        CFArrayRef enabledSources;
        CFIndex i;
        BOOL ret = NO;

        /* There may be multiple active ("selected") input sources, but there is
           always exactly one selected keyboard input source. For instance,
           handwriting methods are active simultaneously with a keyboard source.
           As the name implies, TISCopyCurrentKeyboardInputSource only returns
           the keyboard source, so it's not sufficient for our needs. We use
           TISCreateInputSourceList instead to find all selected sources. */
        dispatch_once(&onceToken, ^{
            filterDict = CFDictionaryCreate(NULL, (const void **)&kTISPropertyInputSourceIsSelected, (const void **)&kCFBooleanTrue, 1,
                                            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

        });
        enabledSources = TISCreateInputSourceList(filterDict, false);
        for (i = 0; i < CFArrayGetCount(enabledSources); i++)
        {
            TISInputSourceRef source = (TISInputSourceRef)CFArrayGetValueAtIndex(enabledSources, i);
            CFStringRef type = TISGetInputSourceProperty(source, kTISPropertyInputSourceType);

            /* kTISTypeKeyboardLayout is for physical keyboards. Any type other
               than that is an IME. */
            if (!CFEqual(type, kTISTypeKeyboardLayout) && !InputSourceShouldBeIgnored(source))
            {
                ret = YES;
                break;
            }
        }

        CFRelease(enabledSources);
        return ret;
     }

    - (void) releaseMouseCapture
    {
        // This might be invoked on a background thread by the distributed
        // notification center.  Shunt it to the main thread.
        if (![NSThread isMainThread])
        {
            dispatch_async(dispatch_get_main_queue(), ^{ [self releaseMouseCapture]; });
            return;
        }

        if (mouseCaptureWindow)
        {
            macdrv_event* event;

            event = macdrv_create_event(RELEASE_CAPTURE, mouseCaptureWindow);
            [mouseCaptureWindow.queue postEvent:event];
            macdrv_release_event(event);
        }
    }

    - (void) unminimizeWindowIfNoneVisible
    {
        WineWindow *bestOption = nil;

        if ([self isAnyWineWindowVisible])
            return;

        for (WineWindow *window in [NSApp windows])
        {
            if (![window isKindOfClass:[WineWindow class]] || ![window isMiniaturized])
                continue;

            bestOption = window;

            /* Prefer any window that would actually show something. */
            if ([window presentsVisibleContent])
                break;
        }

        [bestOption deminiaturize:self];
    }

    - (void) setRetinaMode:(BOOL)mode
    {
        retina_on = mode;

        [clipCursorHandler setRetinaMode:mode];

        for (WineWindow* window in [NSApp windows])
        {
            if ([window isKindOfClass:[WineWindow class]])
                [window setRetinaMode:mode];
        }
    }


    /*
     * ---------- NSApplicationDelegate methods ----------
     */
    - (void)cgDisplay:(CGDirectDisplayID)display wasReconfiguredWithFlags:(CGDisplayChangeSummaryFlags)flags
    {
        NSDictionary *userInfo;

        primaryScreenHeightValid = FALSE;
        [self sendDisplaysChanged:FALSE];
        [self adjustWindowLevels];

        /* When the display configuration changes, the cursor position may jump.
           Accumulated mouse movement deltas are invalidated.  Make sure the
           next mouse move event starts over from an absolute baseline. */
        forceNextMouseMoveAbsolute = TRUE;

        userInfo = @{
            WineDisplayConfigurationNotificationDisplayIDKey: @(display),
            WineDisplayConfigurationNotificationFlagsKey: @(flags)
        };
        [NSNotificationCenter.defaultCenter postNotificationName:WineDisplayConfigurationChangedNotification
                                                          object:NSApp
                                                        userInfo:userInfo];
    }

    static void DisplayReconfigCallback(CGDirectDisplayID display, CGDisplayChangeSummaryFlags flags, void *userInfo)
    {
        /* A callback with flag == kCGDisplayBeginConfigurationFlag is
           documented to be sent at the beginning of a change set. We should
           ignore it; the actual changes will come in separate callbacks. */
        if (flags == kCGDisplayBeginConfigurationFlag)
            return;

        /* We're called back on an internal CG thread, so kick this over to the
           main thread. */
        OnMainThreadAsync(^{
            [WineApplicationController.sharedController cgDisplay:display wasReconfiguredWithFlags:flags];
        });
    }

    - (void)applicationDidFinishLaunching:(NSNotification *)notification
    {
        CGDisplayRegisterReconfigurationCallback(DisplayReconfigCallback, NULL);
    }

    - (void)applicationDidBecomeActive:(NSNotification *)notification
    {
        NSNumber* displayID;
        NSDictionary* modesToRealize = [latentDisplayModes autorelease];

        macdrv_note_app_active(1);
        latentDisplayModes = [[NSMutableDictionary alloc] init];
        for (displayID in modesToRealize)
        {
            CGDisplayModeRef mode = (CGDisplayModeRef)modesToRealize[displayID];
            [self setMode:mode forDisplay:[displayID unsignedIntValue]];
        }

        [self updateFullscreenWindows];
        [self adjustWindowLevels:YES];
        [self applyDeferredClip];

        if (beenActive)
            [self unminimizeWindowIfNoneVisible];
        beenActive = TRUE;

        // If a Wine process terminates abruptly while it has the display captured
        // and switched to a different resolution, Mac OS X will uncapture the
        // displays and switch their resolutions back.  However, the other Wine
        // processes won't have their notion of the desktop rect changed back.
        // This can lead them to refuse to draw or acknowledge clicks in certain
        // portions of their windows.
        //
        // To solve this, we synthesize a displays-changed event whenever we're
        // activated.  This will provoke a re-synchronization of Wine's notion of
        // the desktop rect with the actual state.
        [self sendDisplaysChanged:TRUE];

        // The cursor probably moved while we were inactive.  Accumulated mouse
        // movement deltas are invalidated.  Make sure the next mouse move event
        // starts over from an absolute baseline.
        forceNextMouseMoveAbsolute = TRUE;
    }

    - (void)applicationDidResignActive:(NSNotification *)notification
    {
        macdrv_event* event;
        WineEventQueue* queue;

        macdrv_note_app_active(0);
        [self invalidateGotFocusEvents];

        event = macdrv_create_event(APP_DEACTIVATED, nil);

        [eventQueuesLock lock];
        for (queue in eventQueues)
            [queue postEvent:event];
        [eventQueuesLock unlock];

        macdrv_release_event(event);

        [self releaseMouseCapture];
    }

    - (void) applicationDidUnhide:(NSNotification*)aNotification
    {
        [self adjustWindowLevels];
    }

    - (BOOL) applicationShouldHandleReopen:(NSApplication*)theApplication hasVisibleWindows:(BOOL)flag
    {
        // Note that "flag" is often wrong.  WineWindows are NSPanels and NSPanels
        // don't count as "visible windows" for this purpose.
        [self unminimizeWindowIfNoneVisible];
        return YES;
    }

    /* A close or a Quit is a request the program's own thread answers. One that
       loads or loops without taking messages never does, and the window then
       ignores its close button for as long as that lasts. Windows says such a
       window is not responding and offers to end the program; so does this. */
    - (void) watchRequest:(macdrv_event*)event forWindow:(WineWindow*)window
    {
        if (unansweredAlert) return;

        macdrv_retain_event(event);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kUnansweredRequestSeconds * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            /* Over the window that was asked, while it is still there; a Quit names no
               window and goes over the front one. */
            WineWindow* host = window ? (window.isVisible ? window : nil) : [self frontWineWindow];

            if (!__atomic_load_n(&event->taken, __ATOMIC_RELAXED) && !unansweredAlert && host)
                [self offerToEndProgramOver:host waitingOn:event];
            macdrv_release_event(event);
        });
    }

    - (void) offerToEndProgramOver:(WineWindow*)window waitingOn:(macdrv_event*)event
    {
        NSString* name = [[NSRunningApplication currentApplication] localizedName] ?: @"The game";
        NSAlert* alert = [[[NSAlert alloc] init] autorelease];
        NSTimer* timer;

        alert.alertStyle = NSAlertStyleWarning;
        alert.messageText = [NSString stringWithFormat:@"\u201c%@\u201d is not responding", name];
        alert.informativeText = @"It has not answered for a few seconds. A game that is loading may answer "
                                 "in a while. Ending it loses anything it has not saved.";
        [alert addButtonWithTitle:@"Wait"];
        [alert addButtonWithTitle:@"End Game"].hasDestructiveAction = YES;

        macdrv_retain_event(event);
        unansweredAlert = [alert retain];

        /* The program answering is the better ending: the sheet leaves by itself, and so
           it does when its window goes away under it. */
        timer = [NSTimer timerWithTimeInterval:0.5 repeats:YES block:^(NSTimer* t){
            BOOL settled = __atomic_load_n(&event->taken, __ATOMIC_RELAXED) || !window.isVisible;
            if (settled && window.attachedSheet == alert.window)
                [window endSheet:alert.window returnCode:NSAlertFirstButtonReturn];
        }];
        [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];

        [alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse response){
            [timer invalidate];
            macdrv_release_event(event);
            [unansweredAlert release];
            unansweredAlert = nil;
            if (response == NSAlertSecondButtonReturn)
            {
                fprintf(stderr, "sevo:exit pid=%d ended by the user while not responding\n", getpid());
                _exit(1);
            }
        }];
    }

    - (NSApplicationTerminateReply) applicationShouldTerminate:(NSApplication *)sender
    {
        NSApplicationTerminateReply ret = NSTerminateNow;
        NSAppleEventManager* m = [NSAppleEventManager sharedAppleEventManager];
        NSAppleEventDescriptor* desc = [m currentAppleEvent];
        macdrv_event* event;
        WineEventQueue* queue;

        event = macdrv_create_event(APP_QUIT_REQUESTED, nil);
        event->deliver = 1;
        switch ([[desc attributeDescriptorForKeyword:kAEQuitReason] int32Value])
        {
            case kAELogOut:
            case kAEReallyLogOut:
                event->app_quit_requested.reason = QUIT_REASON_LOGOUT;
                break;
            case kAEShowRestartDialog:
                event->app_quit_requested.reason = QUIT_REASON_RESTART;
                break;
            case kAEShowShutdownDialog:
                event->app_quit_requested.reason = QUIT_REASON_SHUTDOWN;
                break;
            default:
                event->app_quit_requested.reason = QUIT_REASON_NONE;
                break;
        }

        [eventQueuesLock lock];

        if ([eventQueues count])
        {
            for (queue in eventQueues)
                [queue postEvent:event];
            ret = NSTerminateLater;
        }

        [eventQueuesLock unlock];

        if (ret == NSTerminateLater)
            [self watchRequest:event forWindow:nil];

        macdrv_release_event(event);

        return ret;
    }

    - (void)applicationWillBecomeActive:(NSNotification *)notification
    {
        macdrv_event* event = macdrv_create_event(APP_ACTIVATED, nil);
        event->deliver = 1;

        [eventQueuesLock lock];
        for (WineEventQueue* queue in eventQueues)
            [queue postEvent:event];
        [eventQueuesLock unlock];

        macdrv_release_event(event);
    }

    - (void)applicationWillResignActive:(NSNotification *)notification
    {
        [self adjustWindowLevels:NO];
    }

/***********************************************************************
 *              PerformRequest
 *
 * Run-loop-source perform callback.  Pull request blocks from the
 * array of queued requests and invoke them.
 */
static void PerformRequest(void *info)
{
@autoreleasepool
{
    WineApplicationController* controller = [WineApplicationController sharedController];

    for (;;)
    {
        @autoreleasepool
        {
            __block dispatch_block_t block;

            dispatch_sync(controller->requestsManipQueue, ^{
                if ([controller->requests count])
                {
                    block = (dispatch_block_t)[controller->requests[0] retain];
                    [controller->requests removeObjectAtIndex:0];
                }
                else
                    block = nil;
            });

            if (!block)
                break;

            block();
            [block release];
        }
    }
}
}

/***********************************************************************
 *              OnMainThreadAsync
 *
 * Run a block on the main thread asynchronously.
 */
void OnMainThreadAsync(dispatch_block_t block)
{
    WineApplicationController* controller = [WineApplicationController sharedController];

    block = [block copy];
    dispatch_sync(controller->requestsManipQueue, ^{
        [controller->requests addObject:block];
    });
    [block release];
    CFRunLoopSourceSignal(controller->requestSource);
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

@end

/***********************************************************************
 *              LogError
 */
void LogError(const char* func, NSString* format, ...)
{
    va_list args;
    va_start(args, format);
    LogErrorv(func, format, args);
    va_end(args);
}

/***********************************************************************
 *              LogErrorv
 */
void LogErrorv(const char* func, NSString* format, va_list args)
{
@autoreleasepool
{
    NSString* message = [[NSString alloc] initWithFormat:format arguments:args];
    fprintf(stderr, "err:%s:%s", func, [message UTF8String]);
    [message release];
}
}

/***********************************************************************
 *              macdrv_window_rejected_focus
 *
 * Pass focus to the next window that hasn't already rejected this same
 * WINDOW_GOT_FOCUS event.
 */
void macdrv_window_rejected_focus(const macdrv_event *event)
{
    OnMainThread(^{
        [[WineApplicationController sharedController] windowRejectedFocusEvent:event];
    });
}

/***********************************************************************
 *              macdrv_get_input_source_info
 *
 * Returns the keyboard layout uchr data, keyboard type and input source.
 */
void macdrv_get_input_source_info(CFDataRef* uchr, CGEventSourceKeyboardType* keyboard_type, bool* is_iso, TISInputSourceRef* input_source)
{
    OnMainThread(^{
        TISInputSourceRef inputSourceLayout;

        inputSourceLayout = TISCopyCurrentKeyboardLayoutInputSource();
        if (inputSourceLayout)
        {
            CFDataRef data = TISGetInputSourceProperty(inputSourceLayout,
                                kTISPropertyUnicodeKeyLayoutData);
            *uchr = CFDataCreateCopy(NULL, data);
            CFRelease(inputSourceLayout);

            *keyboard_type = [WineApplicationController sharedController].keyboardType;
            *is_iso = (KBGetLayoutType(*keyboard_type) == kKeyboardISO);
            if (input_source)
                *input_source = TISCopyCurrentKeyboardInputSource();
        }
    });
}

/***********************************************************************
 *              macdrv_beep
 *
 * Play the beep sound configured by the user in System Preferences.
 */
void macdrv_beep(void)
{
    OnMainThreadAsync(^{
        NSBeep();
    });
}

/***********************************************************************
 *              macdrv_set_display_mode
 */
int macdrv_set_display_mode(CGDirectDisplayID displayID, CGDisplayModeRef display_mode)
{
    __block int ret;

    OnMainThread(^{
        ret = [[WineApplicationController sharedController] setMode:display_mode forDisplay:displayID];
    });

    return ret;
}

/***********************************************************************
 *              macdrv_set_cursor
 *
 * Set the cursor.
 *
 * If name is non-NULL, it is a selector for a class method on NSCursor
 * identifying the cursor to set.  In that case, frames is ignored.  If
 * name is NULL, then frames is used.
 *
 * frames is an array of dictionaries.  Each dictionary is a frame of
 * an animated cursor.  Under the key "image" is a CGImage for the
 * frame.  Under the key "duration" is a CFNumber time interval, in
 * seconds, for how long that frame is presented before proceeding to
 * the next frame.  Under the key "hotSpot" is a CFDictionary encoding a
 * CGPoint, to be decoded using CGPointMakeWithDictionaryRepresentation().
 * This is the hot spot, measured in pixels down and to the right of the
 * top-left corner of the image.
 *
 * If the array has exactly 1 element, the cursor is static, not
 * animated.  If frames is NULL or has 0 elements, the cursor is hidden.
 */
void macdrv_set_cursor(CFStringRef name, CFArrayRef frames)
{
    SEL sel;

    sel = NSSelectorFromString((NSString*)name);
    if (sel)
    {
        OnMainThreadAsync(^{
            WineApplicationController* controller = [WineApplicationController sharedController];
            [controller setCursorWithFrames:nil];
            controller.cursor = [NSCursor performSelector:sel];
            [controller unhideCursor];
        });
    }
    else
    {
        NSArray* nsframes = (NSArray*)frames;
        if ([nsframes count])
        {
            OnMainThreadAsync(^{
                [[WineApplicationController sharedController] setCursorWithFrames:nsframes];
            });
        }
        else
        {
            OnMainThreadAsync(^{
                WineApplicationController* controller = [WineApplicationController sharedController];
                [controller setCursorWithFrames:nil];
                [controller hideCursor];
            });
        }
    }
}

/***********************************************************************
 *              macdrv_get_cursor_position
 *
 * Obtains the current cursor position.  Returns zero on failure,
 * non-zero on success.
 */
int macdrv_get_cursor_position(CGPoint *pos)
{
    OnMainThread(^{
        WineApplicationController* controller = [WineApplicationController sharedController];
        NSPoint location = [NSEvent mouseLocation];
        location = [controller flippedMouseLocation:location];
        *pos = cgpoint_win_from_mac([controller winePointFromScreenPoint:NSPointToCGPoint(location)]);
    });

    return TRUE;
}

/***********************************************************************
 *              macdrv_set_cursor_position
 *
 * Sets the cursor position without generating events.  Returns zero on
 * failure, non-zero on success.
 */
int macdrv_set_cursor_position(CGPoint pos)
{
    __block int ret;

    OnMainThread(^{
        WineApplicationController* controller = [WineApplicationController sharedController];
        ret = [controller setCursorPosition:[controller screenPointFromWinePoint:cgpoint_mac_from_win(pos)]];
    });

    return ret;
}

/***********************************************************************
 *              macdrv_clip_cursor
 *
 * Sets the cursor cursor clipping rectangle.  If the rectangle is equal
 * to or larger than the whole desktop region, the cursor is unclipped.
 * Returns zero on failure, non-zero on success.
 */
int macdrv_clip_cursor(CGRect r)
{
    __block int ret;

    OnMainThread(^{
        WineApplicationController* controller = [WineApplicationController sharedController];
        BOOL clipping = FALSE;
        BOOL screenClip = FALSE;
        CGRect rect = r;

        if (!CGRectIsInfinite(rect))
            rect = [controller screenRectFromWineRect:cgrect_mac_from_win(rect)];

        if (!CGRectIsInfinite(rect))
        {
            NSRect nsrect = NSRectFromCGRect(rect);
            NSScreen* screen;

            /* Convert the rectangle from top-down coords to bottom-up. */
            [controller flipRect:&nsrect];

            clipping = FALSE;
            for (screen in [NSScreen screens])
            {
                if (!NSContainsRect(nsrect, [screen frame]))
                {
                    clipping = TRUE;
                    break;
                }
            }

            /* A rect covering every screen unclips on Windows, and upstream
               follows it. A fullscreen game asks for exactly that to hold the
               cursor for mouse-look, and on a Mac the screen's edges reveal
               the menu bar and the Dock, so that one clip is kept. */
            if (!clipping && [controller isScreenClip:nsrect])
                clipping = screenClip = TRUE;
        }

        [controller setScreenClip:screenClip rect:rect];
        if (clipping)
            ret = [controller startClippingCursor:rect];
        else
            ret = [controller stopClippingCursor];
    });

    return ret;
}

/***********************************************************************
 *              macdrv_set_application_icon
 *
 * Set the application icon.  The images array contains CGImages.  If
 * there are more than one, then they represent different sizes or
 * color depths from the icon resource.  If images is NULL or empty,
 * restores the default application image.
 */
void macdrv_set_application_icon(CFArrayRef images)
{
    NSArray* imageArray = (NSArray*)images;

    OnMainThreadAsync(^{
        [[WineApplicationController sharedController] setApplicationIconFromCGImageArray:imageArray];
    });
}

/***********************************************************************
 *              macdrv_quit_reply
 */
void macdrv_quit_reply(int reply)
{
    OnMainThread(^{
        [NSApp replyToApplicationShouldTerminate:reply];
    });
}

/***********************************************************************
 *              macdrv_using_input_method
 */
bool macdrv_using_input_method(void)
{
    __block bool ret;

    OnMainThread(^{
        ret = [[WineApplicationController sharedController] inputSourceIsInputMethod];
    });

    return ret;
}

/***********************************************************************
 *              macdrv_set_mouse_capture_window
 */
void macdrv_set_mouse_capture_window(macdrv_window window)
{
    WineWindow* w = (WineWindow*)window;

    [w.queue discardEventsMatchingMask:event_mask_for_type(RELEASE_CAPTURE) forWindow:w];

    OnMainThread(^{
        [[WineApplicationController sharedController] setMouseCaptureWindow:w];
    });
}

const CFStringRef macdrv_input_source_input_key = CFSTR("input");
const CFStringRef macdrv_input_source_type_key = CFSTR("type");
const CFStringRef macdrv_input_source_lang_key = CFSTR("lang");

/***********************************************************************
 *              macdrv_create_input_source_list
 */
CFArrayRef macdrv_create_input_source_list(void)
{
    CFMutableArrayRef ret = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

    OnMainThread(^{
        CFArrayRef input_list;
        CFDictionaryRef filter_dict;
        const void *filter_keys[2] = { kTISPropertyInputSourceCategory, kTISPropertyInputSourceIsSelectCapable };
        const void *filter_values[2] = { kTISCategoryKeyboardInputSource, kCFBooleanTrue };
        int i;

        filter_dict = CFDictionaryCreate(NULL, filter_keys, filter_values, sizeof(filter_keys)/sizeof(filter_keys[0]),
                                         &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        input_list = TISCreateInputSourceList(filter_dict, false);

        for (i = 0; i < CFArrayGetCount(input_list); i++)
        {
            TISInputSourceRef input = (TISInputSourceRef)CFArrayGetValueAtIndex(input_list, i);
            CFArrayRef source_langs = TISGetInputSourceProperty(input, kTISPropertyInputSourceLanguages);
            CFDictionaryRef entry;
            const void *input_keys[3] = { macdrv_input_source_input_key,
                                          macdrv_input_source_type_key,
                                          macdrv_input_source_lang_key };
            const void *input_values[3];

            input_values[0] = input;
            input_values[1] = TISGetInputSourceProperty(input, kTISPropertyInputSourceType);
            input_values[2] = CFArrayGetValueAtIndex(source_langs, 0);

            entry = CFDictionaryCreate(NULL, input_keys, input_values, sizeof(input_keys) / sizeof(input_keys[0]),
                                       &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

            CFArrayAppendValue(ret, entry);
            CFRelease(entry);
        }
        CFRelease(input_list);
        CFRelease(filter_dict);
    });

    return ret;
}

bool macdrv_select_input_source(TISInputSourceRef input_source)
{
    __block bool ret = false;

    OnMainThread(^{
        ret = (TISSelectInputSource(input_source) == noErr);
    });

    return ret;
}

void macdrv_set_cocoa_retina_mode(bool new_mode)
{
    OnMainThread(^{
        [[WineApplicationController sharedController] setRetinaMode:new_mode];
    });
}

bool macdrv_is_any_wine_window_visible(void)
{
    __block bool ret = false;

    OnMainThread(^{
        ret = [[WineApplicationController sharedController] isAnyWineWindowVisible];
    });

    return ret;
}
