/*
 * A window's Win32 menu bar in the macOS menu bar: the Wine side.
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

/* The menu is read here, on a Wine thread, into a snapshot the Cocoa side keeps per window
   (cocoa_menubar.m), whenever the window's frame changes: SetMenu and DrawMenuBar both end
   in SetWindowPos with SWP_FRAMECHANGED. State an application sets only when a menu opens is
   read again then, through QUERY_MENU_INIT, which sends WM_INITMENU and WM_INITMENUPOPUP
   first. A chosen item comes back as WINDOW_MENU_COMMAND on the window's own thread and is
   posted as WM_COMMAND, as Windows does for a menu bar item. */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "macdrv.h"
#include "winuser.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(menu);

/* Bounds on what a snapshot holds: no real menu comes near them, and a menu that refers to
   itself stops at the depth. */
#define MAX_MENU_ITEMS 1024
#define MAX_MENU_DEPTH 8

struct snapshot_builder
{
    struct macdrv_menu_item items[MAX_MENU_ITEMS];
    int count;
};

/* The menu of a top-level window; a child window's GWLP_ID is its control id. */
static HMENU window_menu(HWND hwnd)
{
    if (NtUserGetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD) return 0;
    return (HMENU)NtUserGetWindowLongPtrW(hwnd, GWLP_ID);
}

/* The title without its mnemonic (`&File` reads File, `&&` is one ampersand), and the
   accelerator text after a tab as the shortcut. */
static CFStringRef copy_title(const WCHAR *text, UINT length, CFStringRef *shortcut)
{
    WCHAR title[256];
    UINT i, n = 0;

    *shortcut = NULL;
    for (i = 0; i < length && text[i]; i++)
    {
        if (text[i] == '\t' || text[i] == '\b')
        {
            UINT rest = i + 1, end;
            while (rest < length && (text[rest] == '\t' || text[rest] == '\b')) rest++;
            for (end = rest; end < length && text[end]; end++);
            if (end > rest)
                *shortcut = CFStringCreateWithCharacters(NULL, (const UniChar *)text + rest, end - rest);
            break;
        }
        if (text[i] == '&')
        {
            if (i + 1 < length && text[i + 1] == '&') i++;
            else continue;
        }
        if (n < ARRAY_SIZE(title)) title[n++] = text[i];
    }
    return CFStringCreateWithCharacters(NULL, (const UniChar *)title, n);
}

static void add_menu_items(struct snapshot_builder *builder, HMENU menu, int parent, int depth)
{
    UINT count = NtUserGetMenuItemCount(menu), i;

    for (i = 0; i < count && builder->count < MAX_MENU_ITEMS; i++)
    {
        WCHAR text[256];
        MENUITEMINFOW info = { sizeof(info) };
        struct macdrv_menu_item *item;
        int index;

        info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        info.dwTypeData = text;
        info.cch = ARRAY_SIZE(text);
        text[0] = 0;
        if (!NtUserThunkedMenuItemInfo(menu, i, MF_BYPOSITION, NtUserGetMenuItemInfoW, &info, NULL))
            continue;

        index = builder->count;
        item = &builder->items[index];
        memset(item, 0, sizeof(*item));
        item->parent = parent;
        item->id = info.wID;
        item->popup = (UINT_PTR)info.hSubMenu;
        if (info.fType & MFT_SEPARATOR)
            item->flags |= MACDRV_MENU_ITEM_SEPARATOR;
        else
        {
            /* An owner-drawn or bitmap item has no text to show, so it is left out. */
            if (!text[0]) continue;
            item->title = copy_title(text, ARRAY_SIZE(text), &item->shortcut);
        }
        if (info.fState & (MFS_DISABLED | MFS_GRAYED)) item->flags |= MACDRV_MENU_ITEM_DISABLED;
        if (info.fState & MFS_CHECKED) item->flags |= MACDRV_MENU_ITEM_CHECKED;
        if (info.fType & MFT_RADIOCHECK) item->flags |= MACDRV_MENU_ITEM_RADIO;
        builder->count++;

        if (info.hSubMenu && depth < MAX_MENU_DEPTH)
            add_menu_items(builder, info.hSubMenu, index, depth + 1);
    }
}

static CFTypeRef create_snapshot(HMENU menu)
{
    struct snapshot_builder *builder;
    CFTypeRef snapshot;
    int i;

    if (!menu || !(builder = malloc(sizeof(*builder)))) return NULL;
    builder->count = 0;
    add_menu_items(builder, menu, -1, 0);
    TRACE("menu %p: %d items\n", menu, builder->count);
    snapshot = macdrv_create_menu_snapshot(builder->items, builder->count);
    for (i = 0; i < builder->count; i++)
    {
        if (builder->items[i].title) CFRelease(builder->items[i].title);
        if (builder->items[i].shortcut) CFRelease(builder->items[i].shortcut);
    }
    free(builder);
    return snapshot;
}

/***********************************************************************
 *              macdrv_update_window_menu
 *
 * Gives the window's Cocoa side a fresh snapshot of its menu, or none.
 */
void macdrv_update_window_menu(HWND hwnd)
{
    struct macdrv_win_data *data;
    CFTypeRef snapshot;

    if (!native_menu_bar) return;
    snapshot = create_snapshot(window_menu(hwnd));
    if ((data = get_win_data(hwnd)))
    {
        if (data->cocoa_window) macdrv_set_cocoa_window_menu(data->cocoa_window, snapshot);
        release_win_data(data);
    }
    if (snapshot) CFRelease(snapshot);
}

/***********************************************************************
 *              query_menu_init
 *
 * Handler for QUERY_MENU_INIT: a menu is about to open in the macOS menu bar. The program
 * hears of it the way it would from its own menu bar, and the menu is read after it has
 * answered.
 */
BOOL query_menu_init(HWND hwnd, macdrv_query *query)
{
    HMENU menu = window_menu(hwnd), popup = (HMENU)(UINT_PTR)query->menu_init.popup;

    /* A thread busy past the menu's wait answers the query late, when the menu has long
       been shown; the program hears of nothing it did not see. */
    if (!menu || __atomic_load_n(&query->menu_init.abandoned, __ATOMIC_RELAXED)) return FALSE;
    if (query->menu_init.begin) send_message(hwnd, WM_INITMENU, (WPARAM)menu, 0);
    if (popup)
        send_message(hwnd, WM_INITMENUPOPUP, (WPARAM)popup, MAKELPARAM(query->menu_init.position, FALSE));
    query->menu_init.snapshot = create_snapshot(menu);
    return query->menu_init.snapshot != NULL;
}

/***********************************************************************
 *              macdrv_window_menu_command
 *
 * Handler for WINDOW_MENU_COMMAND events.
 */
void macdrv_window_menu_command(HWND hwnd, const macdrv_event *event)
{
    if (!hwnd || (NtUserGetWindowLongW(hwnd, GWL_STYLE) & WS_DISABLED)) return;
    TRACE("hwnd %p command %u\n", hwnd, event->window_menu_command.id);
    NtUserPostMessage(hwnd, WM_COMMAND, MAKEWPARAM(event->window_menu_command.id, 0), 0);
}
