/*
 * winemac.drv entry points
 *
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

#include <stdarg.h>
#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "ntgdi.h"
#include "macdrv_res.h"
#include "shellapi.h"
#include "winreg.h"
#include "ddk/d3dkmthk.h"
#include "unixlib.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(macdrv);


static HMODULE macdrv_module = 0;

struct quit_info {
    HWND               *wins;
    UINT                capacity;
    UINT                count;
    UINT                done;
    DWORD               flags;
    BOOL                result;
    BOOL                replied;
};


static BOOL CALLBACK get_process_windows(HWND hwnd, LPARAM lp)
{
    struct quit_info *qi = (struct quit_info*)lp;
    DWORD pid;

    NtUserGetWindowThread(hwnd, &pid);
    if (pid == GetCurrentProcessId())
    {
        if (qi->count >= qi->capacity)
        {
            UINT new_cap = qi->capacity * 2;
            HWND *new_wins = HeapReAlloc(GetProcessHeap(), 0, qi->wins, new_cap * sizeof(*qi->wins));
            if (!new_wins) return FALSE;
            qi->wins = new_wins;
            qi->capacity = new_cap;
        }

        qi->wins[qi->count++] = hwnd;
    }

    return TRUE;
}

#pragma pack(push,1)

typedef struct
{
    BYTE bWidth;
    BYTE bHeight;
    BYTE bColorCount;
    BYTE bReserved;
    WORD wPlanes;
    WORD wBitCount;
    DWORD dwBytesInRes;
    WORD nID;
} GRPICONDIRENTRY;

typedef struct
{
    WORD idReserved;
    WORD idType;
    WORD idCount;
    GRPICONDIRENTRY idEntries[1];
} GRPICONDIR;

#pragma pack(pop)

static void quit_reply(int reply)
{
    struct quit_result_params params = { .result = reply };
    MACDRV_CALL(quit_result, &params);
}


static void CALLBACK quit_callback(HWND hwnd, UINT msg, ULONG_PTR data, LRESULT result)
{
    struct quit_info *qi = (struct quit_info*)data;

    qi->done++;

    if (msg == WM_QUERYENDSESSION)
    {
        TRACE("got WM_QUERYENDSESSION result %Id from win %p (%u of %u done)\n", result,
              hwnd, qi->done, qi->count);

        if (!result && !IsWindow(hwnd))
        {
            TRACE("win %p no longer exists; ignoring apparent refusal\n", hwnd);
            result = TRUE;
        }

        if (!result && qi->result)
        {
            qi->result = FALSE;

            /* On the first FALSE from WM_QUERYENDSESSION, we already know the
               ultimate reply.  Might as well tell Cocoa now. */
            if (!qi->replied)
            {
                qi->replied = TRUE;
                TRACE("giving quit reply %d\n", qi->result);
                quit_reply(qi->result);
            }
        }

        if (qi->done >= qi->count)
        {
            UINT i;

            qi->done = 0;
            for (i = 0; i < qi->count; i++)
            {
                TRACE("sending WM_ENDSESSION to win %p result %d flags 0x%08lx\n", qi->wins[i],
                      qi->result, qi->flags);
                if (!SendMessageCallbackW(qi->wins[i], WM_ENDSESSION, qi->result, qi->flags,
                                          quit_callback, (ULONG_PTR)qi))
                {
                    DWORD error = RtlGetLastWin32Error();
                    BOOL invalid = (error == ERROR_INVALID_WINDOW_HANDLE);
                    if (invalid)
                        TRACE("failed to send WM_ENDSESSION to win %p because it's invalid; assuming success\n",
                            qi->wins[i]);
                    else
                        WARN("failed to send WM_ENDSESSION to win %p; error 0x%08lx; assuming refusal\n",
                            qi->wins[i], error);
                    quit_callback(qi->wins[i], WM_ENDSESSION, (ULONG_PTR)qi, invalid);
                }
            }
        }
    }
    else /* WM_ENDSESSION */
    {
        TRACE("finished WM_ENDSESSION for win %p (%u of %u done)\n", hwnd, qi->done, qi->count);

        if (qi->done >= qi->count)
        {
            if (!qi->replied)
            {
                TRACE("giving quit reply %d\n", qi->result);
                quit_reply(qi->result);
            }

            TRACE("%sterminating process\n", qi->result ? "" : "not ");
            if (qi->result)
                TerminateProcess(GetCurrentProcess(), 0);

            HeapFree(GetProcessHeap(), 0, qi->wins);
            HeapFree(GetProcessHeap(), 0, qi);
        }
    }
}


/***********************************************************************
 *              macdrv_app_quit_request
 */
NTSTATUS WINAPI macdrv_app_quit_request(void *arg, ULONG size)
{
    struct app_quit_request_params *params = arg;
    struct quit_info *qi;
    UINT i;

    qi = HeapAlloc(GetProcessHeap(), 0, sizeof(*qi));
    if (!qi)
        goto fail;

    qi->capacity = 32;
    qi->wins = HeapAlloc(GetProcessHeap(), 0, qi->capacity * sizeof(*qi->wins));
    qi->count = qi->done = 0;

    if (!qi->wins || !EnumWindows(get_process_windows, (LPARAM)qi))
        goto fail;

    qi->flags = params->flags;
    qi->result = TRUE;
    qi->replied = FALSE;

    for (i = 0; i < qi->count; i++)
    {
        TRACE("sending WM_QUERYENDSESSION to win %p\n", qi->wins[i]);
        if (!SendMessageCallbackW(qi->wins[i], WM_QUERYENDSESSION, 0, qi->flags,
                                  quit_callback, (ULONG_PTR)qi))
        {
            DWORD error = RtlGetLastWin32Error();
            BOOL invalid = (error == ERROR_INVALID_WINDOW_HANDLE);
            if (invalid)
                TRACE("failed to send WM_QUERYENDSESSION to win %p because it's invalid; assuming success\n",
                     qi->wins[i]);
            else
                WARN("failed to send WM_QUERYENDSESSION to win %p; error 0x%08lx; assuming refusal\n",
                     qi->wins[i], error);
            quit_callback(qi->wins[i], WM_QUERYENDSESSION, (ULONG_PTR)qi, invalid);
        }
    }

    /* quit_callback() will clean up qi */
    return STATUS_SUCCESS;

fail:
    WARN("failed to allocate window list\n");
    if (qi)
    {
        HeapFree(GetProcessHeap(), 0, qi->wins);
        HeapFree(GetProcessHeap(), 0, qi);
    }
    quit_reply(FALSE);
    return STATUS_SUCCESS;
}

/***********************************************************************
 *              get_first_resource
 *
 * Helper for create_app_icon_images().  Enum proc for EnumResourceNamesW()
 * which just gets the handle for the first resource and stops further
 * enumeration.
 */
static BOOL CALLBACK get_first_resource(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR lparam)
{
    HRSRC *res_info = (HRSRC*)lparam;

    *res_info = FindResourceW(module, name, (LPCWSTR)RT_GROUP_ICON);
    return FALSE;
}


/***********************************************************************
 *              macdrv_app_icon
 */
static NTSTATUS WINAPI macdrv_app_icon(void *arg, ULONG size)
{
    struct app_icon_entry entries[64];
    HRSRC res_info;
    HGLOBAL res_data;
    GRPICONDIR *icon_dir;
    unsigned count;
    int i;

    TRACE("()\n");

    count = 0;

    res_info = NULL;
    EnumResourceNamesW(NULL, (LPCWSTR)RT_GROUP_ICON, get_first_resource, (LONG_PTR)&res_info);
    if (!res_info)
    {
        WARN("found no RT_GROUP_ICON resource\n");
        return STATUS_SUCCESS;
    }

    if (!(res_data = LoadResource(NULL, res_info)))
    {
        WARN("failed to load RT_GROUP_ICON resource\n");
        return STATUS_SUCCESS;
    }

    if (!(icon_dir = LockResource(res_data)))
    {
        WARN("failed to lock RT_GROUP_ICON resource\n");
        goto cleanup;
    }

    for (i = 0; i < icon_dir->idCount && count < ARRAYSIZE(entries); i++)
    {
        struct app_icon_entry *entry = &entries[count];
        int width = icon_dir->idEntries[i].bWidth;
        int height = icon_dir->idEntries[i].bHeight;
        BOOL found_better_bpp = FALSE;
        int j;
        LPCWSTR name;
        HGLOBAL icon_res_data;
        BYTE *icon_bits;

        if (!width) width = 256;
        if (!height) height = 256;

        /* If there's another icon at the same size but with better
           color depth, skip this one.  We end up making CGImages that
           are all 32 bits per pixel, so Cocoa doesn't get the original
           color depth info to pick the best representation itself. */
        for (j = 0; j < icon_dir->idCount; j++)
        {
            int jwidth = icon_dir->idEntries[j].bWidth;
            int jheight = icon_dir->idEntries[j].bHeight;

            if (!jwidth) jwidth = 256;
            if (!jheight) jheight = 256;

            if (j != i && jwidth == width && jheight == height &&
                icon_dir->idEntries[j].wBitCount > icon_dir->idEntries[i].wBitCount)
            {
                found_better_bpp = TRUE;
                break;
            }
        }

        if (found_better_bpp) continue;

        name = MAKEINTRESOURCEW(icon_dir->idEntries[i].nID);
        res_info = FindResourceW(NULL, name, (LPCWSTR)RT_ICON);
        if (!res_info)
        {
            WARN("failed to find RT_ICON resource %d with ID %hd\n", i, icon_dir->idEntries[i].nID);
            continue;
        }

        icon_res_data = LoadResource(NULL, res_info);
        if (!icon_res_data)
        {
            WARN("failed to load icon %d with ID %hd\n", i, icon_dir->idEntries[i].nID);
            continue;
        }

        icon_bits = LockResource(icon_res_data);
        if (icon_bits)
        {
            HICON icon;

            entry->width = width;
            entry->height = height;

            /* dwBytesInRes from the icon_dir entry is wrong in some apps; use
               SizeofResource instead. */
            icon = CreateIconFromResourceEx(icon_bits, SizeofResource(NULL, res_info),
                                            TRUE, 0x00030000, width, height, 0);
            if (icon)
            {
                entry->icon = HandleToUlong(icon);
                count++;
            }
            else
                WARN("failed to create icon %d from resource with ID %hd\n", i, icon_dir->idEntries[i].nID);
        }
        else
            WARN("failed to lock RT_ICON resource %d with ID %hd\n", i, icon_dir->idEntries[i].nID);

        FreeResource(icon_res_data);
    }

cleanup:
    FreeResource(res_data);

    return NtCallbackReturn(entries, count * sizeof(entries[0]), 0);
}


static NTSTATUS WINAPI macdrv_regcreateopenkeyexa(void *arg, ULONG size)
{
    struct regcreateopenkeyexa_params *params = arg;
    LONG result;

    if (params->create)
    {
        result = RegCreateKeyExA(UlongToHandle(params->hkey),
                                 param_ptr(params->name),
                                 params->reserved,
                                 param_ptr(params->class),
                                 params->options,
                                 params->access,
                                 param_ptr(params->security),
                                 param_ptr(params->retkey),
                                 param_ptr(params->disposition));
    }
    else
    {
        result = RegOpenKeyExA(UlongToHandle(params->hkey),
                               param_ptr(params->name),
                               params->options,
                               params->access,
                               param_ptr(params->retkey));
    }
    *(LONG *)param_ptr(params->result) = result;
    return 0;
}

static NTSTATUS WINAPI macdrv_regqueryvalueexa(void *arg, ULONG size)
{
    struct regqueryvalueexa_params *params = arg;
    LONG result;

    result = RegQueryValueExA(UlongToHandle(params->hkey),
                              param_ptr(params->name),
                              param_ptr(params->reserved),
                              param_ptr(params->type),
                              param_ptr(params->data),
                              param_ptr(params->count));
    *(LONG *)param_ptr(params->result) = result;
    return 0;
}

static NTSTATUS WINAPI macdrv_regsetvalueexa(void *arg, ULONG size)
{
    struct regsetvalueexa_params *params = arg;
    LONG result;

    result = RegSetValueExA(UlongToHandle(params->hkey),
                            param_ptr(params->name),
                            params->reserved,
                            params->type,
                            param_ptr(params->data),
                            params->count);
    *(LONG *)param_ptr(params->result) = result;
    return 0;
}


#ifdef _WIN64

/* MARK: - D3DMetal 4.0 host callbacks
 *
 * The toolkit calls these through KeUserDispatchCallback with the parameter
 * blocks declared in unixlib.h, and reads each Win32 result through the
 * pointer in the block's last field. Module and heap handles cross the
 * boundary as 32-bit values (the toolkit's wrappers store them in a DWORD),
 * so a 64-bit HMODULE is handed out as a small token from the table below
 * and the process heap as token 1. */

#define D3DMETAL_MODULE_TOKENS 64

static HMODULE d3dmetal_modules[D3DMETAL_MODULE_TOKENS];

static UINT32 d3dmetal_module_token(HMODULE module)
{
    unsigned int i, free_slot = D3DMETAL_MODULE_TOKENS;

    if (!module) return 0;
    for (i = 0; i < D3DMETAL_MODULE_TOKENS; i++)
    {
        if (d3dmetal_modules[i] == module) return i + 1;
        if (!d3dmetal_modules[i] && free_slot == D3DMETAL_MODULE_TOKENS) free_slot = i;
    }
    if (free_slot == D3DMETAL_MODULE_TOKENS)
    {
        ERR("no token left for module %p\n", module);
        return 0;
    }
    d3dmetal_modules[free_slot] = module;
    return free_slot + 1;
}

static HMODULE d3dmetal_module_from_token(UINT32 token)
{
    if (!token || token > D3DMETAL_MODULE_TOKENS) return NULL;
    return d3dmetal_modules[token - 1];
}

static NTSTATUS WINAPI d3dmetal_regdeletekeyvaluea(void *arg, ULONG size)
{
    struct d3dmetal_regdeletekeyvaluea_params *params = arg;
    *(LONG *)param_ptr(params->result) = RegDeleteKeyValueA(UlongToHandle(params->hkey),
                                                             param_ptr(params->subkey),
                                                             param_ptr(params->value));
    return 0;
}

static NTSTATUS WINAPI d3dmetal_createthread(void *arg, ULONG size)
{
    struct d3dmetal_createthread_params *params = arg;
    HANDLE thread = CreateThread(param_ptr(params->security), params->stack_size,
                                 param_ptr(params->start), param_ptr(params->param),
                                 params->flags, param_ptr(params->thread_id));
    TRACE("start %p param %p flags %#x -> %p\n", param_ptr(params->start), param_ptr(params->param),
          (unsigned int)params->flags, thread);
    *(HANDLE *)param_ptr(params->result) = thread;
    return 0;
}

static NTSTATUS WINAPI d3dmetal_d3dkmtenumadapters2(void *arg, ULONG size)
{
    struct d3dmetal_d3dkmtenumadapters2_params *params = arg;
    *(NTSTATUS *)param_ptr(params->result) = D3DKMTEnumAdapters2(param_ptr(params->desc));
    return 0;
}

static NTSTATUS WINAPI d3dmetal_getmodulehandlea(void *arg, ULONG size)
{
    struct d3dmetal_getmodulehandlea_params *params = arg;
    UINT32 token = d3dmetal_module_token(GetModuleHandleA(param_ptr(params->name)));
    TRACE("%s -> token %u\n", debugstr_a(param_ptr(params->name)), token);
    *(UINT64 *)param_ptr(params->result) = token;
    return 0;
}

static NTSTATUS WINAPI d3dmetal_getprocaddress(void *arg, ULONG size)
{
    struct d3dmetal_getprocaddress_params *params = arg;
    HMODULE module = d3dmetal_module_from_token(params->module);
    void *proc = module ? GetProcAddress(module, param_ptr(params->name)) : NULL;
    TRACE("token %u %s -> %p\n", params->module,
          params->name > 0xffff ? debugstr_a(param_ptr(params->name)) : wine_dbg_sprintf("#%u", (unsigned int)params->name),
          proc);
    *(void **)param_ptr(params->result) = proc;
    return 0;
}

static NTSTATUS WINAPI d3dmetal_getsystemdirectoryw(void *arg, ULONG size)
{
    struct d3dmetal_getsystemdirectoryw_params *params = arg;
    *(UINT32 *)param_ptr(params->result) = GetSystemDirectoryW(param_ptr(params->buffer), params->size);
    return 0;
}

static NTSTATUS WINAPI d3dmetal_getmodulefilenamea(void *arg, ULONG size)
{
    struct d3dmetal_getmodulefilenamea_params *params = arg;
    HMODULE module = d3dmetal_module_from_token(params->module);
    *(UINT32 *)param_ptr(params->result) = GetModuleFileNameA(module, param_ptr(params->buffer), params->size);
    return 0;
}

static NTSTATUS WINAPI d3dmetal_loadlibrarya(void *arg, ULONG size)
{
    struct d3dmetal_loadlibrarya_params *params = arg;
    UINT32 token = d3dmetal_module_token(LoadLibraryA(param_ptr(params->name)));
    TRACE("%s -> token %u\n", debugstr_a(param_ptr(params->name)), token);
    *(UINT64 *)param_ptr(params->result) = token;
    return 0;
}

static NTSTATUS WINAPI d3dmetal_freelibrary(void *arg, ULONG size)
{
    struct d3dmetal_freelibrary_params *params = arg;
    HMODULE module = d3dmetal_module_from_token(params->module);
    BOOL ret = module ? FreeLibrary(module) : FALSE;
    if (ret) d3dmetal_modules[params->module - 1] = NULL;
    *(BOOL *)param_ptr(params->result) = ret;
    return 0;
}

static NTSTATUS WINAPI d3dmetal_loadlibraryexa(void *arg, ULONG size)
{
    struct d3dmetal_loadlibraryexa_params *params = arg;
    UINT32 token = d3dmetal_module_token(LoadLibraryExA(param_ptr(params->name),
                                                        UlongToHandle(params->file), params->flags));
    TRACE("%s flags %#x -> token %u\n", debugstr_a(param_ptr(params->name)), (unsigned int)params->flags, token);
    *(UINT64 *)param_ptr(params->result) = token;
    return 0;
}

static NTSTATUS WINAPI d3dmetal_heapfree(void *arg, ULONG size)
{
    struct d3dmetal_heapfree_params *params = arg;
    HANDLE heap = params->heap == 1 ? GetProcessHeap() : UlongToHandle(params->heap);
    *(BOOL *)param_ptr(params->result) = HeapFree(heap, params->flags, param_ptr(params->mem));
    return 0;
}

static NTSTATUS WINAPI d3dmetal_getprocessheap(void *arg, ULONG size)
{
    struct d3dmetal_getprocessheap_params *params = arg;
    *(UINT64 *)param_ptr(params->result) = 1;
    return 0;
}

static NTSTATUS WINAPI d3dmetal_virtualalloc(void *arg, ULONG size)
{
    struct d3dmetal_virtualalloc_params *params = arg;
    *(void **)param_ptr(params->result) = VirtualAlloc(param_ptr(params->addr), params->size,
                                                        params->type, params->protect);
    return 0;
}

static NTSTATUS WINAPI d3dmetal_virtualfree(void *arg, ULONG size)
{
    struct d3dmetal_virtualfree_params *params = arg;
    *(BOOL *)param_ptr(params->result) = VirtualFree(param_ptr(params->addr), params->size, params->type);
    return 0;
}

static NTSTATUS WINAPI d3dmetal_virtualprotect(void *arg, ULONG size)
{
    struct d3dmetal_virtualprotect_params *params = arg;
    *(BOOL *)param_ptr(params->result) = VirtualProtect(param_ptr(params->addr), params->size,
                                                         params->protect, param_ptr(params->old_protect));
    return 0;
}

/* The MONITORENUMPROC pack_EnumDisplayMonitors hands to
 * NtUserEnumDisplayMonitors. Its LPARAM carries the toolkit's own callback,
 * which has to run on the unix side of a fresh syscall frame: it asks for
 * monitor info through KeUserDispatchCallback, and a user-mode callback
 * started from PE code that is itself inside one would place its arguments
 * on the stack this function is running on. */
static BOOL CALLBACK d3dmetal_monitor_enum_proc(HMONITOR monitor, HDC hdc, RECT *rect, LPARAM lparam)
{
    const struct d3dmetal_monitor_enum_lparam *outer = (const void *)lparam;
    struct d3dmetal_monitor_enum_params params =
    {
        .monitor = (UINT_PTR)monitor,
        .hdc = (UINT_PTR)hdc,
        .rect = (UINT_PTR)rect,
        .callback = outer->callback,
        .lparam = outer->lparam,
    };

    TRACE("monitor %p hdc %p rect %p callback %#I64x lparam %#I64x\n", monitor, hdc, rect,
          outer->callback, outer->lparam);
    if (MACDRV_CALL(d3dmetal_monitor_enum, &params)) return FALSE;
    TRACE("monitor %p -> %d\n", monitor, params.result);
    return params.result;
}

static NTSTATUS WINAPI d3dmetal_getmonitorinfow(void *arg, ULONG size)
{
    struct d3dmetal_getmonitorinfow_params *params = arg;
    *(BOOL *)param_ptr(params->result) = GetMonitorInfoW(param_ptr(params->monitor), param_ptr(params->info));
    return 0;
}

static NTSTATUS WINAPI d3dmetal_adjustwindowrectex(void *arg, ULONG size)
{
    struct d3dmetal_adjustwindowrectex_params *params = arg;
    *(BOOL *)param_ptr(params->result) = AdjustWindowRectEx(param_ptr(params->rect), params->style,
                                                             params->menu, params->ex_style);
    return 0;
}

static NTSTATUS WINAPI d3dmetal_getwindowlongptrw(void *arg, ULONG size)
{
    struct d3dmetal_getwindowlongptrw_params *params = arg;
    *(LONG_PTR *)param_ptr(params->result) = GetWindowLongPtrW(param_ptr(params->hwnd), params->index);
    return 0;
}

static NTSTATUS WINAPI d3dmetal_getwindowrect(void *arg, ULONG size)
{
    struct d3dmetal_getwindowrect_params *params = arg;
    *(BOOL *)param_ptr(params->result) = GetWindowRect(param_ptr(params->hwnd), param_ptr(params->rect));
    return 0;
}

static NTSTATUS WINAPI d3dmetal_getsystemmetrics(void *arg, ULONG size)
{
    struct d3dmetal_getsystemmetrics_params *params = arg;
    *(INT *)param_ptr(params->result) = GetSystemMetrics(params->index);
    return 0;
}

static void fill_d3dmetal_host_callbacks(UINT64 *callbacks)
{
    callbacks[d3dm_cb_regqueryvalueexa] = (UINT_PTR)macdrv_regqueryvalueexa;
    callbacks[d3dm_cb_regsetvalueexa] = (UINT_PTR)macdrv_regsetvalueexa;
    callbacks[d3dm_cb_regcreateopenkeyexa] = (UINT_PTR)macdrv_regcreateopenkeyexa;
    callbacks[d3dm_cb_regdeletekeyvaluea] = (UINT_PTR)d3dmetal_regdeletekeyvaluea;
    callbacks[d3dm_cb_createthread] = (UINT_PTR)d3dmetal_createthread;
    callbacks[d3dm_cb_d3dkmtenumadapters2] = (UINT_PTR)d3dmetal_d3dkmtenumadapters2;
    callbacks[d3dm_cb_getmodulehandlea] = (UINT_PTR)d3dmetal_getmodulehandlea;
    callbacks[d3dm_cb_getprocaddress] = (UINT_PTR)d3dmetal_getprocaddress;
    callbacks[d3dm_cb_getsystemdirectoryw] = (UINT_PTR)d3dmetal_getsystemdirectoryw;
    callbacks[d3dm_cb_getmodulefilenamea] = (UINT_PTR)d3dmetal_getmodulefilenamea;
    callbacks[d3dm_cb_loadlibrarya] = (UINT_PTR)d3dmetal_loadlibrarya;
    callbacks[d3dm_cb_freelibrary] = (UINT_PTR)d3dmetal_freelibrary;
    callbacks[d3dm_cb_loadlibraryexa] = (UINT_PTR)d3dmetal_loadlibraryexa;
    callbacks[d3dm_cb_heapfree] = (UINT_PTR)d3dmetal_heapfree;
    callbacks[d3dm_cb_getprocessheap] = (UINT_PTR)d3dmetal_getprocessheap;
    callbacks[d3dm_cb_virtualalloc] = (UINT_PTR)d3dmetal_virtualalloc;
    callbacks[d3dm_cb_virtualfree] = (UINT_PTR)d3dmetal_virtualfree;
    callbacks[d3dm_cb_virtualprotect] = (UINT_PTR)d3dmetal_virtualprotect;
    callbacks[d3dm_cb_monitorenumproc] = (UINT_PTR)d3dmetal_monitor_enum_proc;
    callbacks[d3dm_cb_getmonitorinfow] = (UINT_PTR)d3dmetal_getmonitorinfow;
    callbacks[d3dm_cb_adjustwindowrectex] = (UINT_PTR)d3dmetal_adjustwindowrectex;
    callbacks[d3dm_cb_getwindowlongptrw] = (UINT_PTR)d3dmetal_getwindowlongptrw;
    callbacks[d3dm_cb_getwindowrect] = (UINT_PTR)d3dmetal_getwindowrect;
    callbacks[d3dm_cb_getsystemmetrics] = (UINT_PTR)d3dmetal_getsystemmetrics;
}

#else

static void fill_d3dmetal_host_callbacks(UINT64 *callbacks)
{
    memset(callbacks, 0, sizeof(*callbacks) * d3dm_cb_count);
}

#endif  /* _WIN64 */


static BOOL process_attach(void)
{
    struct init_params params;

    struct localized_string *str;
    struct localized_string strings[] = {
        { .id = STRING_MENU_WINE },
        { .id = STRING_MENU_ITEM_HIDE_APPNAME },
        { .id = STRING_MENU_ITEM_HIDE },
        { .id = STRING_MENU_ITEM_HIDE_OTHERS },
        { .id = STRING_MENU_ITEM_SHOW_ALL },
        { .id = STRING_MENU_ITEM_QUIT_APPNAME },
        { .id = STRING_MENU_ITEM_QUIT },

        { .id = STRING_MENU_WINDOW },
        { .id = STRING_MENU_ITEM_MINIMIZE },
        { .id = STRING_MENU_ITEM_ZOOM },
        { .id = STRING_MENU_ITEM_ENTER_FULL_SCREEN },
        { .id = STRING_MENU_ITEM_BRING_ALL_TO_FRONT },

        { .id = 0 }
    };

    if (__wine_init_unix_call()) return FALSE;

    for (str = strings; str->id; str++)
        str->len = LoadStringW(macdrv_module, str->id, (WCHAR *)&str->str, 0);
    params.strings = strings;
    params.app_icon_callback = (UINT_PTR)macdrv_app_icon;
    params.app_quit_request_callback = (UINT_PTR)macdrv_app_quit_request;
    params.regcreateopenkeyexa_callback = (UINT_PTR)macdrv_regcreateopenkeyexa;
    params.regsetvalueexa_callback = (UINT_PTR)macdrv_regsetvalueexa;
    params.regqueryvalueexa_callback = (UINT_PTR)macdrv_regqueryvalueexa;
    fill_d3dmetal_host_callbacks(params.d3dmetal_host_callbacks);
    params.d3dmetal_unix_call_dispatcher = (UINT_PTR)__wine_unix_call_dispatcher;
    params.d3dmetal_unixlib_handle = __wine_unixlib_handle;

    if (MACDRV_CALL(init, &params)) return FALSE;

    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls(instance);
    macdrv_module = instance;
    return process_attach();
}
