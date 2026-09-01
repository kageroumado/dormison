/*
 * Copyright 2023 Zhiyi Zhang for CodeWeavers
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

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "dcomp_private.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

static const WCHAR *wine_window_topmost_composed = L"wine_window_topmost_composed";
static const WCHAR *wine_window_non_topmost_composed = L"wine_window_non_topmost_composed";

static HRESULT STDMETHODCALLTYPE target_QueryInterface(IDCompositionTarget *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p!\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionTarget))
    {
        IUnknown_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE target_AddRef(IDCompositionTarget *iface)
{
    struct composition_target *target = impl_from_IDCompositionTarget(iface);
    ULONG ref = InterlockedIncrement(&target->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE target_Release(IDCompositionTarget *iface)
{
    struct composition_target *target = impl_from_IDCompositionTarget(iface);
    ULONG ref = InterlockedDecrement(&target->ref);
    struct composition_visual *root_visual;
    const WCHAR *prop;

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        prop = target->topmost ? wine_window_topmost_composed : wine_window_non_topmost_composed;
        RemovePropW(target->hwnd, prop);

        dcomp_lock();
        list_remove(&target->entry);
        dcomp_unlock();
        IDCompositionDevice_Release(target->device);
        if (target->root)
        {
            root_visual = impl_from_IDCompositionVisual(target->root);
            root_visual->is_root = FALSE;
            IDCompositionVisual_Release(target->root);
        }
        if (target->shared_visual_handle)
            CloseHandle(target->shared_visual_handle);
        free(target);
    }

    return ref;
}

static void shared_visual_target_set_root(HANDLE shared_visual_handle, IDCompositionVisual *root)
{
    NTSTATUS status;

    SERVER_START_REQ(dcomp_set_shared_visual_info)
    {
        req->handle = wine_server_obj_handle(shared_visual_handle);
        req->target_root = wine_server_client_ptr(root);
        status = wine_server_call(req);
    }
    SERVER_END_REQ;

    if (status)
        ERR("dcomp_set_shared_visual_info failed, not in the same process? status %#lx.\n", status);
}

static HRESULT STDMETHODCALLTYPE target_SetRoot(IDCompositionTarget *iface,
        IDCompositionVisual *visual)
{
    struct composition_target *target = impl_from_IDCompositionTarget(iface);
    struct composition_visual *composition_visual;

    TRACE("iface %p, visual %p\n", iface, visual);

    dcomp_lock();

    if (visual)
    {
        composition_visual = impl_from_IDCompositionVisual(visual);
        if (composition_visual->is_root)
        {
            dcomp_unlock();
            return E_INVALIDARG;
        }

        composition_visual->is_root = TRUE;
        IDCompositionVisual_AddRef(visual);
    }

    if (target->root)
    {
        composition_visual = impl_from_IDCompositionVisual(target->root);
        composition_visual->is_root = FALSE;
        IDCompositionVisual_Release(target->root);
    }
    target->root = visual;

    if (target->shared_visual_handle)
        shared_visual_target_set_root(target->shared_visual_handle, target->root);

    dcomp_unlock();
    return S_OK;
}

static const struct IDCompositionTargetVtbl target_vtbl =
{
    /* IUnknown methods */
    target_QueryInterface,
    target_AddRef,
    target_Release,
    /* IDCompositionTarget methods */
    target_SetRoot,
};

HRESULT create_target(struct composition_device *device, HWND hwnd, BOOL topmost,
        IDCompositionTarget **new_target)
{
    struct composition_target *target;
    const WCHAR *prop;
    DWORD pid = 0;

    if (!hwnd || hwnd == GetDesktopWindow() || !new_target)
        return E_INVALIDARG;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId())
        return E_ACCESSDENIED;

    if ((topmost && GetPropW(hwnd, wine_window_topmost_composed))
            || (!topmost && GetPropW(hwnd, wine_window_non_topmost_composed)))
        return DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED;

    target = calloc(1, sizeof(*target));
    if (!target)
        return E_OUTOFMEMORY;

    IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);
    dcomp_lock();
    list_add_tail(&device->targets, &target->entry);
    dcomp_unlock();
    target->IDCompositionTarget_iface.lpVtbl = &target_vtbl;
    target->ref = 1;
    target->hwnd = hwnd;
    target->topmost = topmost;
    target->device = &device->IDCompositionDevice_iface;
    *new_target = &target->IDCompositionTarget_iface;

    prop = target->topmost ? wine_window_topmost_composed : wine_window_non_topmost_composed;
    SetPropW(target->hwnd, prop, (HANDLE)1);
    return S_OK;
}


HRESULT create_target_from_shared_visual_handle(struct composition_device *device, HANDLE shared_visual_handle, void **new_target)
{
    struct composition_target *target;
    HANDLE handle;

    if (!DuplicateHandle(GetCurrentProcess(), shared_visual_handle, GetCurrentProcess(), &handle, 0,
                         FALSE, DUPLICATE_SAME_ACCESS))
    {
        ERR("Cannot duplicate handle, last error %lu.\n", GetLastError());
        return E_FAIL;
    }

    target = calloc(1, sizeof(*target));
    if (!target)
    {
        CloseHandle(handle);
        return E_OUTOFMEMORY;
    }

    IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);
    dcomp_lock();
    list_add_tail(&device->targets, &target->entry);
    dcomp_unlock();
    target->IDCompositionTarget_iface.lpVtbl = &target_vtbl;
    target->ref = 1;
    target->device = &device->IDCompositionDevice_iface;
    target->shared_visual_handle = handle;
    *new_target = &target->IDCompositionTarget_iface;
    return S_OK;
}
