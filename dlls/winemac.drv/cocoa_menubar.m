/*
 * A window's Win32 menu bar in the macOS menu bar: the Cocoa side.
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

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "macdrv_cocoa.h"
#import "cocoa_app.h"
#import "cocoa_event.h"
#import "cocoa_menubar.h"
#import "cocoa_window.h"


/* How long a menu opening waits for the program to answer WM_INITMENUPOPUP before it shows
   the menu as last read. */
static const NSTimeInterval kMenuInitTimeout = 0.1;

static char kWindowMenuKey;
static char kMenuPathKey;


/* One item of a snapshot: a command, a separator, or a submenu with its items. */
@interface WineMenuEntry : NSObject
{
@public
    NSString* title;
    NSString* shortcut;
    unsigned int commandID;
    unsigned int flags;
    uint64_t menu;
    unsigned int position;
    uint64_t popup;
    NSMutableArray* children;
}
@end

@implementation WineMenuEntry

    - (void) dealloc
    {
        [title release];
        [shortcut release];
        [children release];
        [super dealloc];
    }

@end


CFTypeRef macdrv_create_menu_snapshot(const struct macdrv_menu_item *items, int count)
{
    NSMutableArray* bar;
    WineMenuEntry** entries;
    int i;

    if (count <= 0) return NULL;
    if (!(entries = calloc(count, sizeof(*entries)))) return NULL;

    @autoreleasepool
    {
        bar = [[NSMutableArray alloc] init];
        for (i = 0; i < count; i++)
        {
            WineMenuEntry* entry = [[[WineMenuEntry alloc] init] autorelease];
            NSMutableArray* container;

            entry->title = [(NSString*)items[i].title copy];
            entry->shortcut = [(NSString*)items[i].shortcut copy];
            entry->commandID = items[i].id;
            entry->flags = items[i].flags;
            entry->menu = items[i].menu;
            entry->position = items[i].position;
            entry->popup = items[i].popup;
            if (items[i].popup) entry->children = [[NSMutableArray alloc] init];
            entries[i] = entry;

            if (items[i].parent < 0 || items[i].parent >= i) container = bar;
            else container = entries[items[i].parent]->children;
            [container addObject:entry];
        }
    }
    free(entries);
    return bar;
}


/* Maps an accelerator's text to a key equivalent where it reads Ctrl, optionally Shift, and
   one letter or digit: Ctrl+N becomes ⌘N, the Mac's name for the same keys. Anything else
   is left to the program, which hears the keys themselves. Q, H and M stay the app menu's
   and the Window menu's. */
static BOOL key_equivalent_for_shortcut(NSString* shortcut, NSString** key, NSEventModifierFlags* modifiers)
{
    NSArray* parts = [shortcut componentsSeparatedByString:@"+"];
    NSString* last;
    BOOL control = NO;
    NSEventModifierFlags mask = NSEventModifierFlagCommand;
    unichar c;

    if ([parts count] < 2) return NO;
    for (NSString* part in [parts subarrayWithRange:NSMakeRange(0, [parts count] - 1)])
    {
        NSString* name = [[part stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]] lowercaseString];
        if ([name isEqualToString:@"ctrl"] || [name isEqualToString:@"control"] || [name isEqualToString:@"strg"])
            control = YES;
        else if ([name isEqualToString:@"shift"] || [name isEqualToString:@"umschalt"])
            mask |= NSEventModifierFlagShift;
        else
            return NO;
    }
    last = [[parts lastObject] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (!control || [last length] != 1) return NO;
    c = [[last lowercaseString] characterAtIndex:0];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) || c == 'q' || c == 'h' || c == 'm') return NO;
    *key = [NSString stringWithCharacters:&c length:1];
    *modifiers = mask;
    return YES;
}


@interface WineMenuBarMirror ()
{
    NSMenu* mainMenu;
    /* The driver's own View menu, renamed Picture while the program has a View of its own. */
    NSMenuItem* ownViewItem;
    /* The items this mirror put in the main menu, in order. */
    NSMutableArray* barItems;
    /* The window whose menu they show, and the snapshot they were built from. */
    WineWindow* shownWindow;
    NSArray* shownEntries;
    /* A menu of the main menu is open: the bar's own items wait until it closes. */
    BOOL tracking;
    BOOL barNeedsUpdate;
}
@end

@implementation WineMenuBarMirror

    + (WineMenuBarMirror*) sharedMirror
    {
        static WineMenuBarMirror* mirror;
        if (!mirror) mirror = [[WineMenuBarMirror alloc] init];
        return mirror;
    }

    - (void) attachToMainMenu:(NSMenu*)menu
    {
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];

        if (mainMenu) return;
        mainMenu = [menu retain];
        ownViewItem = [[mainMenu itemWithTitle:@"View"] retain];
        barItems = [[NSMutableArray alloc] init];
        [nc addObserver:self selector:@selector(keyWindowChanged:) name:NSWindowDidBecomeKeyNotification object:nil];
        [nc addObserver:self selector:@selector(windowWillClose:) name:NSWindowWillCloseNotification object:nil];
        [nc addObserver:self selector:@selector(trackingBegan:) name:NSMenuDidBeginTrackingNotification object:mainMenu];
        [nc addObserver:self selector:@selector(trackingEnded:) name:NSMenuDidEndTrackingNotification object:mainMenu];
        [self updateBar];
    }

    - (void) setSnapshot:(NSArray*)entries forWindow:(WineWindow*)window
    {
        objc_setAssociatedObject(window, &kWindowMenuKey, entries, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [self updateBar];
    }

    /* The key window's menu; a window with none, like a dialog, shows its owner's. With no
       key window, while the app is in the background, the frontmost window with a menu. */
    - (WineWindow*) targetWindow
    {
        WineWindow* key = (WineWindow*)[NSApp keyWindow];

        if (![key isKindOfClass:[WineWindow class]]) key = nil;
        if (key && !objc_getAssociatedObject(key, &kWindowMenuKey))
            key = [key ancestorWineWindow];
        if (key && !objc_getAssociatedObject(key, &kWindowMenuKey)) key = nil;
        if (!key && ![NSApp keyWindow])
        {
            for (NSNumber* number in [NSWindow windowNumbersWithOptions:0])
            {
                NSWindow* window = [NSApp windowWithWindowNumber:[number integerValue]];
                if ([window isKindOfClass:[WineWindow class]] && [window isVisible] &&
                    objc_getAssociatedObject(window, &kWindowMenuKey))
                    return (WineWindow*)window;
            }
        }
        return key;
    }

    - (void) keyWindowChanged:(NSNotification*)note
    {
        if ([[note object] isKindOfClass:[WineWindow class]]) [self updateBar];
    }

    - (void) windowWillClose:(NSNotification*)note
    {
        WineWindow* window = [note object];

        if (![window isKindOfClass:[WineWindow class]] || [window isFakingClose]) return;
        if (window == shownWindow)
        {
            [shownWindow release];
            shownWindow = nil;
            [self updateBar];
        }
    }

    - (void) trackingBegan:(NSNotification*)note
    {
        tracking = YES;
    }

    - (void) trackingEnded:(NSNotification*)note
    {
        tracking = NO;
        if (barNeedsUpdate) [self updateBar];
    }

    /* Whether the bar would show the same menus, by title, for these entries. */
    - (BOOL) barMatches:(NSArray*)entries
    {
        NSUInteger i;

        if ([entries count] != [barItems count]) return NO;
        for (i = 0; i < [entries count]; i++)
        {
            WineMenuEntry* entry = entries[i];
            if (![[barItems[i] title] isEqualToString:entry->title ? entry->title : @""]) return NO;
        }
        return YES;
    }

    /* Puts the target window's menus in the bar between the app menu and View. The bar
       changes only while no menu of it is open; its menus fill themselves as they open. */
    - (void) updateBar
    {
        WineWindow* window = [self targetWindow];
        NSArray* entries = window ? objc_getAssociatedObject(window, &kWindowMenuKey) : nil;
        NSInteger index;

        if (tracking)
        {
            barNeedsUpdate = YES;
            return;
        }
        barNeedsUpdate = NO;

        if (window != shownWindow)
        {
            [shownWindow release];
            shownWindow = [window retain];
        }
        [shownEntries release];
        shownEntries = [entries retain];
        if ([self barMatches:entries]) return;

        for (NSMenuItem* item in barItems) [mainMenu removeItem:item];
        [barItems removeAllObjects];
        [self nameOwnViewMenuFor:entries];

        index = MIN((NSInteger)1, [mainMenu numberOfItems]);
        for (NSUInteger i = 0; i < [entries count]; i++)
        {
            WineMenuEntry* entry = entries[i];
            NSString* title = entry->title ? entry->title : @"";
            NSMenuItem* item = [[[NSMenuItem alloc] initWithTitle:title action:NULL keyEquivalent:@""] autorelease];
            NSMenu* submenu = [[[NSMenu alloc] initWithTitle:title] autorelease];

            [submenu setDelegate:self];
            [submenu setAutoenablesItems:NO];
            objc_setAssociatedObject(submenu, &kMenuPathKey, @[@(i)], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [item setSubmenu:submenu];
            [mainMenu insertItem:item atIndex:index++];
            [barItems addObject:item];
        }
    }

    - (void) nameOwnViewMenuFor:(NSArray*)entries
    {
        NSString* title = @"View";

        for (WineMenuEntry* entry in entries)
        {
            if (entry->title && [entry->title caseInsensitiveCompare:@"View"] == NSOrderedSame)
                title = @"Picture";
        }
        if (![[ownViewItem title] isEqualToString:title])
        {
            [ownViewItem setTitle:title];
            [[ownViewItem submenu] setTitle:title];
        }
    }

    /* The entry a path of indices leads to from the bar, or nil once the menu changed shape. */
    - (WineMenuEntry*) entryAtPath:(NSArray*)path
    {
        NSArray* level = shownEntries;
        WineMenuEntry* entry = nil;

        for (NSNumber* number in path)
        {
            NSUInteger i = [number unsignedIntegerValue];
            if (i >= [level count]) return nil;
            entry = level[i];
            level = entry->children;
        }
        return entry;
    }

    /* Lets the program set the menu's state the way its own menu bar would, then reads it
       again. A program that does not answer in time gets the menu as last read. */
    - (void) initializeMenuAtPath:(NSArray*)path entry:(WineMenuEntry*)entry
    {
        macdrv_query* query;

        /* A window a modal dialog disabled has no menu to open on Windows. */
        if (!shownWindow || ![shownWindow queue] || shownWindow.disabled) return;
        query = macdrv_create_query();
        query->type = QUERY_MENU_INIT;
        query->window = (macdrv_window)[shownWindow retain];
        query->menu_init.popup = entry->popup;
        query->menu_init.position = [[path lastObject] intValue];
        query->menu_init.begin = [path count] == 1;
        if ([[shownWindow queue] query:query timeout:kMenuInitTimeout] && query->menu_init.snapshot)
        {
            NSArray* entries = (NSArray*)query->menu_init.snapshot;
            objc_setAssociatedObject(shownWindow, &kWindowMenuKey, entries, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [shownEntries release];
            shownEntries = [entries retain];
            if (![self barMatches:entries]) barNeedsUpdate = YES;
        }
        else
            __atomic_store_n(&query->menu_init.abandoned, true, __ATOMIC_RELAXED);
        macdrv_release_query(query);
    }

    - (NSMenuItem*) menuItemForEntry:(WineMenuEntry*)entry path:(NSArray*)path enabled:(BOOL)enabled
    {
        NSMenuItem* item;
        NSString* key;
        NSEventModifierFlags modifiers;

        if (entry->flags & MACDRV_MENU_ITEM_SEPARATOR) return [NSMenuItem separatorItem];

        item = [[[NSMenuItem alloc] initWithTitle:entry->title action:NULL keyEquivalent:@""] autorelease];
        [item setEnabled:enabled && !(entry->flags & MACDRV_MENU_ITEM_DISABLED)];
        [item setState:(entry->flags & MACDRV_MENU_ITEM_CHECKED) ? NSControlStateValueOn : NSControlStateValueOff];
        if (entry->children)
        {
            NSMenu* submenu = [[[NSMenu alloc] initWithTitle:entry->title] autorelease];
            [submenu setDelegate:self];
            [submenu setAutoenablesItems:NO];
            objc_setAssociatedObject(submenu, &kMenuPathKey, path, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [item setSubmenu:submenu];
            return item;
        }

        [item setTarget:self];
        [item setAction:@selector(chooseItem:)];
        [item setRepresentedObject:entry];
        if (entry->shortcut && key_equivalent_for_shortcut(entry->shortcut, &key, &modifiers))
        {
            [item setKeyEquivalent:key];
            [item setKeyEquivalentModifierMask:modifiers];
        }
        else if (entry->shortcut)
            [item setToolTip:entry->shortcut];
        return item;
    }

    /* The one place a mirrored menu changes: macOS 27 keeps the menu bar in another process,
       and an NSMenu changed while that process tracks it can leave the app parked in a menu
       session that never ends. Items of the same shape are patched in place; any other
       change rebuilds the menu. */
    - (void) menuNeedsUpdate:(NSMenu*)menu
    {
        NSArray* path = objc_getAssociatedObject(menu, &kMenuPathKey);
        WineMenuEntry* entry;
        BOOL enabled;
        NSUInteger i;

        if (!path) return;
        entry = [self entryAtPath:path];
        if (entry && entry->popup) [self initializeMenuAtPath:path entry:entry];
        entry = [self entryAtPath:path];
        enabled = shownWindow && !shownWindow.disabled;

        if (entry && entry->children && [menu numberOfItems] == (NSInteger)[entry->children count])
        {
            for (i = 0; i < [entry->children count]; i++)
            {
                WineMenuEntry* child = entry->children[i];
                NSMenuItem* item = [menu itemAtIndex:i];

                if ([item isSeparatorItem] != !!(child->flags & MACDRV_MENU_ITEM_SEPARATOR) ||
                    !![item hasSubmenu] != !!child->children)
                    break;
                if ([item isSeparatorItem]) continue;
                if (![[item title] isEqualToString:child->title]) [item setTitle:child->title];
                [item setRepresentedObject:child];
                [item setEnabled:enabled && !(child->flags & MACDRV_MENU_ITEM_DISABLED)];
                [item setState:(child->flags & MACDRV_MENU_ITEM_CHECKED) ? NSControlStateValueOn : NSControlStateValueOff];
            }
            if (i == [entry->children count]) return;
        }

        [menu removeAllItems];
        if (!entry || !entry->children)
        {
            NSMenuItem* item = entry ? [self menuItemForEntry:entry path:path enabled:enabled] : nil;
            /* A command in the bar itself opens a menu holding just that command. */
            if (item) [menu addItem:item];
            return;
        }
        for (i = 0; i < [entry->children count]; i++)
            [menu addItem:[self menuItemForEntry:entry->children[i] path:[path arrayByAddingObject:@(i)] enabled:enabled]];
    }

    - (void) chooseItem:(NSMenuItem*)item
    {
        WineMenuEntry* entry = [item representedObject];
        macdrv_event* event;

        if (!entry || !shownWindow || ![shownWindow queue]) return;
        event = macdrv_create_event(WINDOW_MENU_COMMAND, shownWindow);
        event->window_menu_command.id = entry->commandID;
        event->window_menu_command.menu = entry->menu;
        event->window_menu_command.position = entry->position;
        event->window_menu_command.by_position = (entry->flags & MACDRV_MENU_ITEM_BY_POSITION) != 0;
        [[shownWindow queue] postEvent:event];
        macdrv_release_event(event);
    }

@end


void macdrv_set_cocoa_window_menu(macdrv_window w, CFTypeRef snapshot)
{
    WineWindow* window = [(WineWindow*)w retain];
    NSArray* entries = [(NSArray*)snapshot retain];

    OnMainThreadAsync(^{
        [[WineMenuBarMirror sharedMirror] setSnapshot:entries forWindow:window];
        [entries release];
        [window release];
    });
}
