/*
 * Tell a game its memory is what the hardware says it is.
 *
 * D3DMetal answers D3D12_FEATURE_ARCHITECTURE1 with UMA = 0, so every engine takes the
 * discrete path: write into an upload heap, then copy into a second allocation that is
 * the same physical memory. The device supports the unified path — a CUSTOM heap in pool
 * L0 with write-back pages takes CPU writes the GPU then reads — and skipping the copy is
 * worth 27-107% of a bandwidth-bound frame, measured.
 *
 * The answer comes from one vtable entry that every device of the process shares, so one
 * device of our own is enough to reach it. This lives here because winemac.drv is loaded
 * by every process that draws, which is the only place a patch can sit without putting a
 * file next to a game's executable.
 *
 * Off unless SEVO_FORCE_UMA says otherwise: 1 reports UMA, 2 also reports
 * CacheCoherentUMA. A game that believes the second one may write into memory expecting
 * no flush, so it is deliberately a separate step.
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

#define COBJMACROS
#define INITGUID

#include <stdarg.h>
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "initguid.h"
#include "d3d12.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(macdrv_uma);

/* Said once per process, like the `sevo:gfx` provenance line: a run that claims a
 * memory model has to be able to prove which one it asked for. */
static void note(const char *format, ...)
{
    char line[256];
    va_list args;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    fprintf(stderr, "sevo:uma pid=%u %s\n", (unsigned)GetCurrentProcessId(), line);
    fflush(stderr);
}

static LONG force_uma;
static LONG patched;
static void *notification_cookie;
static HRESULT (STDMETHODCALLTYPE *real_check_feature_support)(ID3D12Device *, D3D12_FEATURE,
                                                               void *, UINT);

static HRESULT STDMETHODCALLTYPE uma_check_feature_support(ID3D12Device *device,
                                                           D3D12_FEATURE feature,
                                                           void *data, UINT size)
{
    HRESULT hr = real_check_feature_support(device, feature, data, size);

    if (FAILED(hr) || !data) return hr;

    if (feature == D3D12_FEATURE_ARCHITECTURE && size >= sizeof(D3D12_FEATURE_DATA_ARCHITECTURE))
    {
        D3D12_FEATURE_DATA_ARCHITECTURE *architecture = data;
        architecture->UMA = TRUE;
        architecture->CacheCoherentUMA = force_uma >= 2;
        TRACE("ARCHITECTURE node %u -> UMA 1, coherent %u\n",
              architecture->NodeIndex, architecture->CacheCoherentUMA);
    }
    else if (feature == D3D12_FEATURE_ARCHITECTURE1 &&
             size >= sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1))
    {
        D3D12_FEATURE_DATA_ARCHITECTURE1 *architecture = data;
        architecture->UMA = TRUE;
        architecture->CacheCoherentUMA = force_uma >= 2;
        TRACE("ARCHITECTURE1 node %u -> UMA 1, coherent %u\n",
              architecture->NodeIndex, architecture->CacheCoherentUMA);
    }
    return hr;
}

/* One device of our own, released immediately: the vtable it carries is the one every
 * device in the process uses, so the entry only has to be reached once. */
static void patch_device_vtable(void)
{
    HRESULT (WINAPI *create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    ID3D12Device *device = NULL;
    HMODULE module;
    void **slot;
    DWORD old;
    HRESULT hr;

    if (InterlockedCompareExchange(&patched, 1, 0)) return;

    if (!(module = LoadLibraryW(L"d3d12.dll")))
    {
        note("no d3d12.dll in this process");
        return;
    }
    if (!(create_device = (void *)GetProcAddress(module, "D3D12CreateDevice")))
    {
        WARN("d3d12.dll exports no D3D12CreateDevice\n");
        return;
    }
    if (FAILED(hr = create_device(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device)))
    {
        note("no device to read the vtable from (hr %#lx); nothing changed", hr);
        return;
    }

    slot = (void **)&device->lpVtbl->CheckFeatureSupport;
    if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old))
    {
        real_check_feature_support = device->lpVtbl->CheckFeatureSupport;
        *slot = uma_check_feature_support;
        VirtualProtect(slot, sizeof(*slot), old, &old);
        note("reporting UMA=1 coherent=%d to this process", force_uma >= 2);
    }
    else note("the vtable is not writable (error %lu); nothing changed", GetLastError());

    ID3D12Device_Release(device);
}

static DWORD CALLBACK patch_work_item(void *context)
{
    patch_device_vtable();
    return 0;
}

/* The notification arrives under the loader lock, where creating a device would
 * deadlock against D3D12's own initialization, so the work is queued off it. */
static void CALLBACK dll_loaded(ULONG reason, LDR_DLL_NOTIFICATION_DATA *data, void *context)
{
    static const WCHAR d3d12W[] = L"d3d12.dll";

    if (reason != LDR_DLL_NOTIFICATION_REASON_LOADED || !data) return;
    if (!data->Loaded.BaseDllName || !data->Loaded.BaseDllName->Buffer) return;
    if (wcsicmp(data->Loaded.BaseDllName->Buffer, d3d12W)) return;

    RtlQueueWorkItem(patch_work_item, NULL, WT_EXECUTELONGFUNCTION);
}

void sevo_uma_init(void)
{
    char value[16];
    DWORD length = GetEnvironmentVariableA("SEVO_FORCE_UMA", value, sizeof(value));

    if (!length || length >= sizeof(value)) return;   /* not asked for: say nothing */
    force_uma = atoi(value);
    /* Said even when the answer is "off", so a log that carries no line at all means the
     * variable never reached this process — which is a different problem from a patch
     * that did not take. */
    if (force_uma <= 0)
    {
        note("asked for %s: leaving the device's own answer alone", value);
        return;
    }

    /* d3d12 may already be in: a game that links it statically loads it before this
     * driver. Both routes end in the same one-time patch. */
    if (GetModuleHandleW(L"d3d12.dll"))
        RtlQueueWorkItem(patch_work_item, NULL, WT_EXECUTELONGFUNCTION);

    if (LdrRegisterDllNotification(0, dll_loaded, NULL, &notification_cookie))
        WARN("cannot watch for d3d12.dll; only a module already loaded will be patched\n");
    else
        TRACE("watching for d3d12.dll, force %ld\n", force_uma);
}
