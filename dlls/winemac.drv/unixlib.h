/*
 * Copyright 2022 Jacek Caban for CodeWeavers
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

#include "ntuser.h"
#include "wine/unixlib.h"

enum macdrv_funcs
{
    unix_init,
    unix_quit_result,
    unix_d3dmetal_monitor_enum,
    unix_d3dmetal_kernel_call,
    unix_funcs_count
};

#define MACDRV_CALL(func, params) WINE_UNIX_CALL(unix_ ## func, params)

/* D3DMetal 4.0's Win32Dispatch host half: the toolkit reaches Win32 through
 * PE-side functions it calls with KeUserDispatchCallback, reading each from
 * this slot order (libd3dshared 4.0b2, PatchWin32DispatchFunctions: the
 * callbacks region starts at table + 0x148). The three registry callbacks
 * use the parameter structs declared below, shared with the 3.0 table. */
enum d3dmetal_host_callback
{
    d3dm_cb_regqueryvalueexa,
    d3dm_cb_regsetvalueexa,
    d3dm_cb_regcreateopenkeyexa,
    d3dm_cb_regdeletekeyvaluea,
    d3dm_cb_createthread,
    d3dm_cb_d3dkmtenumadapters2,
    d3dm_cb_getmodulehandlea,
    d3dm_cb_getprocaddress,
    d3dm_cb_getsystemdirectoryw,
    d3dm_cb_getmodulefilenamea,
    d3dm_cb_loadlibrarya,
    d3dm_cb_freelibrary,
    d3dm_cb_loadlibraryexa,
    d3dm_cb_heapfree,
    d3dm_cb_getprocessheap,
    d3dm_cb_virtualalloc,
    d3dm_cb_virtualfree,
    d3dm_cb_virtualprotect,
    d3dm_cb_monitorenumproc,   /* a MONITORENUMPROC, not a dispatch callback */
    d3dm_cb_getmonitorinfow,
    d3dm_cb_adjustwindowrectex,
    d3dm_cb_getwindowlongptrw,
    d3dm_cb_getwindowrect,
    d3dm_cb_getsystemmetrics,
    d3dm_cb_count
};

/* macdrv_init params */
struct localized_string
{
    UINT id;
    UINT len;
    UINT64 str;
};

struct init_params
{
    struct localized_string *strings;
    UINT64 app_icon_callback;
    UINT64 app_quit_request_callback;
    UINT64 regcreateopenkeyexa_callback;
    UINT64 regqueryvalueexa_callback;
    UINT64 regsetvalueexa_callback;
    UINT64 d3dmetal_host_callbacks[d3dm_cb_count];
    UINT64 d3dmetal_unix_call_dispatcher;
    UINT64 d3dmetal_unixlib_handle;
};

/* macdrv_d3dmetal_kernel_call params: `func` is called with the six
 * arguments from inside a unix call, so that native code the toolkit runs on
 * the PE user stack can reach anything that needs a syscall frame. */
struct d3dmetal_kernel_call_params
{
    UINT64 func;
    UINT64 args[6];
    UINT64 result;
};

/* macdrv_d3dmetal_monitor_enum params: the argument block of libd3dshared's
 * MonitorEnumCallbackHandler, which calls `callback` (ms_abi) with the four
 * values and stores what it answers. */
struct d3dmetal_monitor_enum_params
{
    UINT64 monitor;
    UINT64 hdc;
    UINT64 rect;
    UINT64 callback;
    UINT64 lparam;
    INT32 result;
};

/* What pack_EnumDisplayMonitors passes as the LPARAM of the host's
 * MONITORENUMPROC. */
struct d3dmetal_monitor_enum_lparam
{
    UINT64 callback;
    UINT64 lparam;
};

/* The remaining host-callback parameter blocks, laid out as the toolkit
 * builds them; the last field of each receives the Win32 result. */
struct d3dmetal_regdeletekeyvaluea_params
{
    struct dispatch_callback_params dispatch;
    UINT32 hkey;
    UINT64 subkey;
    UINT64 value;
    UINT64 result;
};

struct d3dmetal_createthread_params
{
    struct dispatch_callback_params dispatch;
    UINT64 security;
    UINT64 stack_size;
    UINT64 start;
    UINT64 param;
    UINT32 flags;
    UINT64 thread_id;
    UINT64 result;
};

struct d3dmetal_d3dkmtenumadapters2_params
{
    struct dispatch_callback_params dispatch;
    UINT64 desc;
    UINT64 result;
};

struct d3dmetal_getmodulehandlea_params
{
    struct dispatch_callback_params dispatch;
    UINT64 name;
    UINT64 result;
};

struct d3dmetal_getprocaddress_params
{
    struct dispatch_callback_params dispatch;
    UINT32 module;
    UINT64 name;
    UINT64 result;
};

struct d3dmetal_getsystemdirectoryw_params
{
    struct dispatch_callback_params dispatch;
    UINT64 buffer;
    UINT32 size;
    UINT64 result;
};

struct d3dmetal_getmodulefilenamea_params
{
    struct dispatch_callback_params dispatch;
    UINT32 module;
    UINT64 buffer;
    UINT32 size;
    UINT64 result;
};

struct d3dmetal_loadlibrarya_params
{
    struct dispatch_callback_params dispatch;
    UINT64 name;
    UINT64 result;
};

struct d3dmetal_freelibrary_params
{
    struct dispatch_callback_params dispatch;
    UINT32 module;
    UINT64 result;
};

struct d3dmetal_loadlibraryexa_params
{
    struct dispatch_callback_params dispatch;
    UINT64 name;
    UINT32 file;
    UINT32 flags;
    UINT64 result;
};

struct d3dmetal_heapfree_params
{
    struct dispatch_callback_params dispatch;
    UINT32 heap;
    UINT32 flags;
    UINT64 mem;
    UINT64 result;
};

struct d3dmetal_getprocessheap_params
{
    struct dispatch_callback_params dispatch;
    UINT64 result;
};

struct d3dmetal_virtualalloc_params
{
    struct dispatch_callback_params dispatch;
    UINT64 addr;
    UINT64 size;
    UINT32 type;
    UINT32 protect;
    UINT64 result;
};

struct d3dmetal_virtualfree_params
{
    struct dispatch_callback_params dispatch;
    UINT64 addr;
    UINT64 size;
    UINT32 type;
    UINT64 result;
};

struct d3dmetal_virtualprotect_params
{
    struct dispatch_callback_params dispatch;
    UINT64 addr;
    UINT64 size;
    UINT32 protect;
    UINT64 old_protect;
    UINT64 result;
};

struct d3dmetal_getmonitorinfow_params
{
    struct dispatch_callback_params dispatch;
    UINT64 monitor;
    UINT64 info;
    UINT64 result;
};

struct d3dmetal_adjustwindowrectex_params
{
    struct dispatch_callback_params dispatch;
    UINT64 rect;
    UINT32 style;
    UINT32 menu;
    UINT32 ex_style;
    UINT64 result;
};

struct d3dmetal_getwindowlongptrw_params
{
    struct dispatch_callback_params dispatch;
    UINT64 hwnd;
    UINT32 index;
    UINT64 result;
};

struct d3dmetal_getwindowrect_params
{
    struct dispatch_callback_params dispatch;
    UINT64 hwnd;
    UINT64 rect;
    UINT64 result;
};

struct d3dmetal_getsystemmetrics_params
{
    struct dispatch_callback_params dispatch;
    UINT32 index;
    UINT64 result;
};

C_ASSERT(sizeof(struct d3dmetal_monitor_enum_params) == 0x30);
C_ASSERT(sizeof(struct d3dmetal_regdeletekeyvaluea_params) == 0x28);
C_ASSERT(sizeof(struct d3dmetal_createthread_params) == 0x40);
C_ASSERT(sizeof(struct d3dmetal_getprocaddress_params) == 0x20);
C_ASSERT(sizeof(struct d3dmetal_getmodulefilenamea_params) == 0x28);
C_ASSERT(sizeof(struct d3dmetal_loadlibraryexa_params) == 0x20);
C_ASSERT(sizeof(struct d3dmetal_heapfree_params) == 0x20);
C_ASSERT(sizeof(struct d3dmetal_virtualalloc_params) == 0x28);
C_ASSERT(sizeof(struct d3dmetal_virtualprotect_params) == 0x30);
C_ASSERT(sizeof(struct d3dmetal_adjustwindowrectex_params) == 0x28);
C_ASSERT(sizeof(struct d3dmetal_getwindowlongptrw_params) == 0x20);
C_ASSERT(sizeof(struct d3dmetal_getsystemmetrics_params) == 0x18);

/* macdrv_quit_result params */
struct quit_result_params
{
    int result;
};

/* macdrv_app_icon result */
struct app_icon_entry
{
    UINT32 width;
    UINT32 height;
    UINT32 icon;
};

/* macdrv_app_quit_request params */
struct app_quit_request_params
{
    struct dispatch_callback_params dispatch;
    UINT flags;
};

/* macdrv_dnd_query_exited params */
struct dnd_query_exited_params
{
    struct dispatch_callback_params dispatch;
    UINT32 hwnd;
};

/* macdrv_regcreateopenkeyexa params */
struct regcreateopenkeyexa_params
{
    struct dispatch_callback_params dispatch;
    UINT32 create;
    UINT32 hkey;
    UINT64 name;
    UINT32 reserved;
    UINT64 class;
    UINT32 options;
    UINT32 access;
    UINT64 security;
    UINT64 retkey;
    UINT64 disposition;
    UINT64 result;
};

/* macdrv_regqueryvalueexa params */
struct regqueryvalueexa_params
{
    struct dispatch_callback_params dispatch;
    UINT32 hkey;
    UINT64 name;
    UINT64 reserved;
    UINT64 type;
    UINT64 data;
    UINT64 count;
    UINT64 result;
};

/* macdrv_regsetvalueexa params */
struct regsetvalueexa_params
{
    struct dispatch_callback_params dispatch;
    UINT32 hkey;
    UINT64 name;
    UINT32 reserved;
    UINT32 type;
    UINT64 data;
    UINT32 count;
    UINT64 result;
};

static inline void *param_ptr(UINT64 param)
{
    return (void *)(UINT_PTR)param;
}
