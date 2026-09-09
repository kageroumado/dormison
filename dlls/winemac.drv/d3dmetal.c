/*
 * Mac graphics driver hooks used by D3DMetal (part of the Apple Game Porting Toolkit)
 *
 * Copyright 2023 Brendan Shanks for CodeWeavers, Inc.
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

#if 0
#pragma makedep unix
#endif

#if defined(__x86_64__)

#include "config.h"

#include <dispatch/dispatch.h>
#include <dlfcn.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "macdrv.h"
#include "sevo_provenance.h"

WINE_DEFAULT_DEBUG_CHANNEL(macdrv_d3dmtl);

typedef LONG LSTATUS;

void OnMainThread(dispatch_block_t block);

/* The layout D3DMetal expects. libd3dshared.dylib resolves this table with
 * dlsym(RTLD_DEFAULT, "macdrv_functions") and asserts it is non-NULL before
 * touching Metal, so every slot must exist even where the body is a stub. */
struct macdrv_functions_t
{
    void (*macdrv_init_display_devices)(BOOL);
    struct d3dmetal_macdrv_win_data* (*get_win_data)(HWND hwnd);
    void (*release_win_data)(struct d3dmetal_macdrv_win_data *data);
    macdrv_window (*macdrv_get_cocoa_window)(HWND hwnd, BOOL require_on_screen);
    macdrv_metal_device (*macdrv_create_metal_device)(void);
    void (*macdrv_release_metal_device)(macdrv_metal_device d);
    macdrv_metal_view (*macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d);
    macdrv_metal_layer (*macdrv_view_get_metal_layer)(macdrv_metal_view v);
    void (*macdrv_view_release_metal_view)(macdrv_metal_view v);
    void (*on_main_thread)(dispatch_block_t b);
    LSTATUS(WINAPI*RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, BYTE*, LPDWORD);
    LSTATUS(WINAPI*RegSetValueExA)(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD);
    LSTATUS(WINAPI*RegOpenKeyExA)(HKEY, LPCSTR, DWORD, DWORD, HKEY*);
    LSTATUS(WINAPI*RegCreateKeyExA)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, HKEY*, LPDWORD);
    LSTATUS(WINAPI*RegCloseKey)(HKEY);
    BOOL(WINAPI*EnumDisplayMonitors)(HDC,LPRECT,MONITORENUMPROC,LPARAM);
    BOOL(WINAPI*GetMonitorInfoA)(HMONITOR,LPMONITORINFO);
    BOOL(WINAPI*AdjustWindowRectEx)(LPRECT,DWORD,BOOL,DWORD);
    LONG_PTR(WINAPI*GetWindowLongPtrW)(HWND,int);
    BOOL(WINAPI*GetWindowRect)(HWND,LPRECT);
    BOOL(WINAPI*MoveWindow)(HWND,int,int,int,int,BOOL);
    BOOL(WINAPI*SetWindowPos)(HWND,HWND,int,int,int,int,UINT);
    INT(WINAPI*GetSystemMetrics)(INT);
    LONG_PTR(WINAPI*SetWindowLongPtrW)(HWND,INT,LONG_PTR);
};
C_ASSERT(sizeof(struct macdrv_functions_t) == 192);

/* The shape of macdrv's private window data as D3DMetal reads it. It is a
 * frozen copy of an older struct macdrv_win_data, so the fields are filled in
 * from the current one rather than shared with it. */
struct d3dmetal_macdrv_win_data
{
    HWND                hwnd;
    macdrv_window       cocoa_window;
    macdrv_view         cocoa_view;
    macdrv_view         client_cocoa_view;
    RECT                window_rect;
    RECT                whole_rect;
    RECT                client_rect;
    int                 pixel_format;
    COLORREF            color_key;
    HANDLE              drag_event;
    unsigned int        on_screen : 1;
    unsigned int        shaped : 1;
    unsigned int        layered : 1;
    unsigned int        ulw_layered : 1;
    unsigned int        per_pixel_alpha : 1;
    unsigned int        minimized : 1;
    void *              padding[2];  /* padding[0] carries the real macdrv_win_data */
};
C_ASSERT(sizeof(struct d3dmetal_macdrv_win_data) == 120);

static void cf_client_surface_release(CFAllocatorRef allocator, const void *client_surface)
{
    client_surface_release((struct client_surface *)client_surface);
}

void macdrv_release_d3dmetal_client_surfaces(struct macdrv_win_data *data)
{
    if (!data->d3dmetal_client_surfaces) return;
    CFRelease(data->d3dmetal_client_surfaces);
    data->d3dmetal_client_surfaces = NULL;
}

/* Posted from WineMetalLayer's nextDrawable: D3DMetal has produced a frame,
 * so the client surface can be composited into the Cocoa window. */
void macdrv_client_surface_presented(const macdrv_event *event)
{
    TRACE("client_surface %p\n", event->client_surface_presented.client_surface);
    sevo_provenance_note_presented_event();
    client_surface_present(event->client_surface_presented.client_surface);
}

static void my_macdrv_init_display_devices(BOOL p1)
{
    TRACE("%d - no-op\n", p1);
}

static struct d3dmetal_macdrv_win_data *my_get_win_data(HWND hwnd)
{
    struct macdrv_win_data *data;
    struct d3dmetal_macdrv_win_data *d3dm_data;
    struct macdrv_client_surface *surface;
    struct client_surface *client;

    TRACE("%p\n", hwnd);

    /* Making a client surface on every call costs idempotence, but D3DMetal
     * calls this only when creating a swapchain:
     * get_win_data -> create_metal_device -> create_metal_view -> get_metal_layer -> release_win_data
     * The surface must be created before the window data is held, or the two
     * lock orders deadlock. */
    if (!(client = macdrv_CreateClientSurface(hwnd, 0))) return NULL;
    surface = impl_from_client_surface(client);

    if (!(data = get_win_data(hwnd)))
    {
        client_surface_release(client);
        return NULL;
    }

    if (!data->d3dmetal_client_surfaces)
    {
        static const CFArrayCallBacks callbacks = { .release = cf_client_surface_release };
        data->d3dmetal_client_surfaces = CFArrayCreateMutable(NULL, 0, &callbacks);
    }
    CFArrayAppendValue(data->d3dmetal_client_surfaces, client);
    macdrv_set_view_d3dmetal_client_surface(surface->cocoa_view, client);

    if (!(d3dm_data = calloc(1, sizeof(*d3dm_data))))
    {
        release_win_data(data);
        return NULL;
    }

    d3dm_data->hwnd = data->hwnd;
    d3dm_data->cocoa_window = data->cocoa_window;
    d3dm_data->client_cocoa_view = surface->cocoa_view;
    d3dm_data->window_rect = data->rects.window;
    d3dm_data->whole_rect = data->rects.visible;
    d3dm_data->client_rect = data->rects.client;
    d3dm_data->pixel_format = data->pixel_format;
    d3dm_data->drag_event = data->drag_event;
    d3dm_data->on_screen = data->on_screen;
    d3dm_data->shaped = data->shaped;
    d3dm_data->layered = data->layered;
    d3dm_data->ulw_layered = data->ulw_layered;
    d3dm_data->per_pixel_alpha = data->per_pixel_alpha;
    d3dm_data->minimized = data->minimized;
    d3dm_data->padding[0] = data;

    return d3dm_data;
}

static void my_release_win_data(struct d3dmetal_macdrv_win_data *data)
{
    TRACE("%p\n", data);

    if (!data) return;
    release_win_data(data->padding[0]);
    free(data);
}

static macdrv_window my_macdrv_get_cocoa_window(HWND hwnd, BOOL require_on_screen)
{
    TRACE("%p %d\n", hwnd, require_on_screen);
    return macdrv_get_cocoa_window(hwnd, require_on_screen);
}

static macdrv_metal_device my_macdrv_create_metal_device(void)
{
    TRACE("\n");
    return macdrv_create_metal_device();
}

static void my_macdrv_release_metal_device(macdrv_metal_device d)
{
    TRACE("%p\n", d);
    macdrv_release_metal_device(d);
}

static macdrv_metal_view my_macdrv_view_create_metal_view(macdrv_view v, macdrv_metal_device d)
{
    TRACE("%p %p\n", v, d);
    return macdrv_view_create_metal_view(v, d);
}

static macdrv_metal_layer my_macdrv_view_get_metal_layer(macdrv_metal_view v)
{
    TRACE("%p\n", v);
    return macdrv_view_get_metal_layer(v);
}

static void my_macdrv_view_release_metal_view(macdrv_metal_view v)
{
    TRACE("%p\n", v);
    macdrv_view_release_metal_view(v);
}

static void my_OnMainThread(dispatch_block_t b)
{
    TRACE("%p\n", b);
    OnMainThread(b);
}

static LSTATUS WINAPI my_RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type,
                                          BYTE *data, LPDWORD count)
{
    LSTATUS result;
    void *ret_ptr;
    ULONG ret_len;
    struct regqueryvalueexa_params params =
    {
        .dispatch = {.callback = regqueryvalueexa_callback},
        .hkey = HandleToUlong(key),
        .name = (UINT_PTR)name,
        .reserved = (UINT_PTR)reserved,
        .type = (UINT_PTR)type,
        .data = (UINT_PTR)data,
        .count = (UINT_PTR)count,
        .result = (UINT_PTR)&result,
    };

    TRACE("%p %s %p %p %p %p\n", key, debugstr_a(name), reserved, type, data, count);
    KeUserDispatchCallback(&params.dispatch, sizeof(params), &ret_ptr, &ret_len);
    return result;
}

static LSTATUS WINAPI my_RegSetValueExA(HKEY key, LPCSTR name, DWORD reserved, DWORD type,
                                        const BYTE *data, DWORD count)
{
    LSTATUS result;
    void *ret_ptr;
    ULONG ret_len;
    struct regsetvalueexa_params params =
    {
        .dispatch = {.callback = regsetvalueexa_callback},
        .hkey = HandleToUlong(key),
        .name = (UINT_PTR)name,
        .reserved = reserved,
        .type = type,
        .data = (UINT_PTR)data,
        .count = count,
        .result = (UINT_PTR)&result,
    };

    TRACE("%p %s %u %p %u\n", key, debugstr_a(name), (unsigned)type, data, (unsigned)count);
    KeUserDispatchCallback(&params.dispatch, sizeof(params), &ret_ptr, &ret_len);
    return result;
}

static LSTATUS WINAPI my_RegOpenKeyExA(HKEY key, LPCSTR name, DWORD options, DWORD access, HKEY *retkey)
{
    LSTATUS result;
    void *ret_ptr;
    ULONG ret_len;
    struct regcreateopenkeyexa_params params =
    {
        .dispatch = {.callback = regcreateopenkeyexa_callback},
        .create = 0,
        .hkey = HandleToUlong(key),
        .name = (UINT_PTR)name,
        .options = options,
        .access = access,
        .retkey = (UINT_PTR)retkey,
        .result = (UINT_PTR)&result,
    };

    TRACE("%p %s\n", key, debugstr_a(name));
    KeUserDispatchCallback(&params.dispatch, sizeof(params), &ret_ptr, &ret_len);
    return result;
}

static LSTATUS WINAPI my_RegCreateKeyExA(HKEY key, LPCSTR name, DWORD reserved, LPSTR class, DWORD options,
                                         DWORD access, LPSECURITY_ATTRIBUTES security, HKEY *retkey,
                                         LPDWORD disposition)
{
    LSTATUS result;
    void *ret_ptr;
    ULONG ret_len;
    struct regcreateopenkeyexa_params params =
    {
        .dispatch = {.callback = regcreateopenkeyexa_callback},
        .create = 1,
        .hkey = HandleToUlong(key),
        .name = (UINT_PTR)name,
        .reserved = reserved,
        .class = (UINT_PTR)class,
        .options = options,
        .access = access,
        .security = (UINT_PTR)security,
        .retkey = (UINT_PTR)retkey,
        .disposition = (UINT_PTR)disposition,
        .result = (UINT_PTR)&result,
    };

    TRACE("%p %s\n", key, debugstr_a(name));
    KeUserDispatchCallback(&params.dispatch, sizeof(params), &ret_ptr, &ret_len);
    return result;
}

static LSTATUS WINAPI my_RegCloseKey(HKEY key)
{
    TRACE("%p\n", key);
    if (!key) return ERROR_INVALID_HANDLE;
    if (key >= (HKEY)0x80000000) return ERROR_SUCCESS;
    return RtlNtStatusToDosError(NtClose(key));
}

static BOOL WINAPI my_EnumDisplayMonitors(HDC hdc, LPRECT rect, MONITORENUMPROC proc, LPARAM lparam)
{
    TRACE("%p %p %p %lx\n", hdc, rect, proc, (long)lparam);
    return NtUserEnumDisplayMonitors(hdc, rect, proc, lparam);
}

static BOOL WINAPI my_GetMonitorInfoA(HMONITOR monitor, LPMONITORINFO info)
{
    MONITORINFOEXW miW;
    BOOL ret;

    TRACE("%p %p\n", monitor, info);

    if (info->cbSize == sizeof(MONITORINFO)) return NtUserGetMonitorInfo(monitor, info);
    if (info->cbSize != sizeof(MONITORINFOEXA)) return FALSE;

    miW.cbSize = sizeof(miW);
    ret = NtUserGetMonitorInfo(monitor, (MONITORINFO *)&miW);
    if (ret)
    {
        MONITORINFOEXA *miA = (MONITORINFOEXA *)info;
        ULONG size;
        miA->rcMonitor = miW.rcMonitor;
        miA->rcWork = miW.rcWork;
        miA->dwFlags = miW.dwFlags;
        RtlUnicodeToUTF8N(miA->szDevice, sizeof(miA->szDevice), &size, miW.szDevice,
                          wcslen(miW.szDevice) * sizeof(WCHAR));
    }
    return ret;
}

static BOOL WINAPI my_AdjustWindowRectEx(LPRECT rect, DWORD style, BOOL menu, DWORD ex_style)
{
    TRACE("%p %#x %d %#x\n", rect, (unsigned int)style, menu, (unsigned int)ex_style);
    return NtUserAdjustWindowRect(rect, style, menu, ex_style, NtUserGetSystemDpiForProcess(NULL));
}

static LONG_PTR WINAPI my_GetWindowLongPtrW(HWND hwnd, int offset)
{
    TRACE("%p %d\n", hwnd, offset);
    return NtUserGetWindowLongPtrW(hwnd, offset);
}

static BOOL WINAPI my_GetWindowRect(HWND hwnd, LPRECT rect)
{
    TRACE("%p %p\n", hwnd, rect);
    return NtUserGetWindowRect(hwnd, rect, NtUserGetWinMonitorDpi(hwnd, MDT_DEFAULT));
}

static BOOL WINAPI my_MoveWindow(HWND hwnd, int x, int y, int cx, int cy, BOOL repaint)
{
    TRACE("%p %d %d %d %d %d\n", hwnd, x, y, cx, cy, repaint);
    return NtUserMoveWindow(hwnd, x, y, cx, cy, repaint);
}

static BOOL WINAPI my_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    TRACE("%p %p %d %d %d %d %#x\n", hwnd, after, x, y, cx, cy, flags);
    return NtUserSetWindowPos(hwnd, after, x, y, cx, cy, flags);
}

static INT WINAPI my_GetSystemMetrics(INT index)
{
    TRACE("%d\n", index);
    return NtUserGetSystemMetrics(index);
}

static LONG_PTR WINAPI my_SetWindowLongPtrW(HWND hwnd, INT offset, LONG_PTR newval)
{
    TRACE("%p %d %lx\n", hwnd, offset, (long)newval);
    return NtUserSetWindowLongPtr(hwnd, offset, newval, FALSE);
}

/* D3DMetal 4.0's Win32Dispatch host half lives in ntdll, which loaded the
 * toolkit and owns the table; the PE-side functions arrive here with
 * macdrv_init and are handed on. The entry points are looked up by name
 * because ntdll.so is the one image every unix library can see. */
static void (*p_set_host_callbacks)( const UINT64 *callbacks, unsigned int count );
static void (*p_set_reentry)( UINT64 dispatcher, UINT64 handle, unsigned int code );
static void (*p_monitor_enum)( void *params );

void d3dmetal_set_host_callbacks(const struct init_params *params)
{
    if (!p_set_host_callbacks)
        p_set_host_callbacks = dlsym(RTLD_DEFAULT, "__wine_d3dmetal_set_host_callbacks");
    if (!p_set_reentry)
        p_set_reentry = dlsym(RTLD_DEFAULT, "__wine_d3dmetal_set_reentry");
    if (!p_monitor_enum)
        p_monitor_enum = dlsym(RTLD_DEFAULT, "__wine_d3dmetal_monitor_enum");
    if (!p_set_host_callbacks || !p_set_reentry)
    {
        TRACE("ntdll carries no D3DMetal dispatch table\n");
        return;
    }
    p_set_reentry(params->d3dmetal_unix_call_dispatcher, params->d3dmetal_unixlib_handle,
                  unix_d3dmetal_kernel_call);
    p_set_host_callbacks(params->d3dmetal_host_callbacks, d3dm_cb_count);
}

NTSTATUS macdrv_d3dmetal_kernel_call(void *arg)
{
    struct d3dmetal_kernel_call_params *params = arg;
    UINT64 (*func)( UINT64, UINT64, UINT64, UINT64, UINT64, UINT64 ) = (void *)(UINT_PTR)params->func;

    params->result = func( params->args[0], params->args[1], params->args[2],
                           params->args[3], params->args[4], params->args[5] );
    return STATUS_SUCCESS;
}

NTSTATUS macdrv_d3dmetal_monitor_enum(void *arg)
{
    struct d3dmetal_monitor_enum_params *params = arg;

    if (!p_monitor_enum) return STATUS_NOT_IMPLEMENTED;
    p_monitor_enum(params);
    return STATUS_SUCCESS;
}

DECLSPEC_EXPORT struct macdrv_functions_t macdrv_functions =
{
    &my_macdrv_init_display_devices,
    &my_get_win_data,
    &my_release_win_data,
    &my_macdrv_get_cocoa_window,
    &my_macdrv_create_metal_device,
    &my_macdrv_release_metal_device,
    &my_macdrv_view_create_metal_view,
    &my_macdrv_view_get_metal_layer,
    &my_macdrv_view_release_metal_view,
    &my_OnMainThread,
    &my_RegQueryValueExA,
    &my_RegSetValueExA,
    &my_RegOpenKeyExA,
    &my_RegCreateKeyExA,
    &my_RegCloseKey,
    &my_EnumDisplayMonitors,
    &my_GetMonitorInfoA,
    &my_AdjustWindowRectEx,
    &my_GetWindowLongPtrW,
    &my_GetWindowRect,
    &my_MoveWindow,
    &my_SetWindowPos,
    &my_GetSystemMetrics,
    &my_SetWindowLongPtrW,
};

#else

void d3dmetal_set_host_callbacks(const struct init_params *params)
{
}

NTSTATUS macdrv_d3dmetal_monitor_enum(void *arg)
{
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS macdrv_d3dmetal_kernel_call(void *arg)
{
    return STATUS_NOT_IMPLEMENTED;
}

#endif  /* __x86_64__ */
