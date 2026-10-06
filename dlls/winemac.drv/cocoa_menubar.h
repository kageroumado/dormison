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

@class WineWindow;

/* Keeps each window's menu snapshot and shows the key window's menus in the main menu,
   between the app menu and View, filling each menu as it opens. Main thread only. */
@interface WineMenuBarMirror : NSObject <NSMenuDelegate>

    + (WineMenuBarMirror*) sharedMirror;
    - (void) attachToMainMenu:(NSMenu*)menu;
    - (void) setSnapshot:(NSArray*)entries forWindow:(WineWindow*)window;

@end
