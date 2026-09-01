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

#include <assert.h>
#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "dcomp_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

static HRESULT STDMETHODCALLTYPE visual_QueryInterface(IDCompositionVisualUnknown *iface, REFIID iid,
        void **out)
{
    struct composition_visual *visual = impl_from_IDCompositionVisualUnknown(iface);

    TRACE("iface %p, iid %s, out %p stub!\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionVisual)
            || (visual->version >= 2 && IsEqualGUID(iid, &IID_IDCompositionVisual2))
            || IsEqualGUID(iid, &IID_IDCompositionVisualUnknown))
    {
        IUnknown_AddRef(&visual->IDCompositionVisualUnknown_iface);
        *out = &visual->IDCompositionVisualUnknown_iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE visual_AddRef(IDCompositionVisualUnknown *iface)
{
    struct composition_visual *visual = impl_from_IDCompositionVisualUnknown(iface);
    ULONG ref = InterlockedIncrement(&visual->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE visual_Release(IDCompositionVisualUnknown *iface)
{
    struct composition_visual *visual = impl_from_IDCompositionVisualUnknown(iface);
    ULONG ref = InterlockedDecrement(&visual->ref);
    struct composition_visual *child;

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        if (visual->interop)
            ID2D1GdiInteropRenderTarget_Release(visual->interop);
        if (visual->device_context)
            ID2D1DeviceContext_Release(visual->device_context);
        if (visual->content)
            IUnknown_Release(visual->content);
        if (visual->shared_visual_handle)
            CloseHandle(visual->shared_visual_handle);
        dcomp_lock();
        if (visual->is_child)
            list_remove(&visual->entry);
        LIST_FOR_EACH_ENTRY(child, &visual->child_visuals, struct composition_visual, entry)
        {
            list_remove(&child->entry);
            child->is_child = FALSE;
            child->parent = NULL;
        }
        dcomp_unlock();
        free(visual);
    }

    return ref;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetXAnimation(IDCompositionVisualUnknown *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetX(IDCompositionVisualUnknown *iface, float offset_x)
{
    struct composition_visual *visual = impl_from_IDCompositionVisualUnknown(iface);

    TRACE("iface %p, offset_x %f.\n", iface, offset_x);

    visual->offset_x = offset_x;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetYAnimation(IDCompositionVisualUnknown *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetY(IDCompositionVisualUnknown *iface, float offset_y)
{
    FIXME("iface %p, offset_y %f stub!\n", iface, offset_y);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetTransformObject(IDCompositionVisualUnknown *iface,
        IDCompositionTransform *transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetTransform(IDCompositionVisualUnknown *iface,
        const D2D_MATRIX_3X2_F *matrix)
{
    FIXME("iface %p, matrix %p stub!\n", iface, matrix);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetTransformParent(IDCompositionVisualUnknown *iface,
        IDCompositionVisual *visual)
{
    FIXME("iface %p, visual %p stub!\n", iface, visual);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetEffect(IDCompositionVisualUnknown *iface,
        IDCompositionEffect *effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetBitmapInterpolationMode(IDCompositionVisualUnknown *iface,
        enum DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpolation_mode)
{
    struct composition_visual *visual = impl_from_IDCompositionVisualUnknown(iface);

    TRACE("iface %p, interpolation_mode %d.\n", iface, interpolation_mode);

    if (interpolation_mode != DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
        && interpolation_mode != DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR
        && interpolation_mode != DCOMPOSITION_BITMAP_INTERPOLATION_MODE_INHERIT)
        return E_INVALIDARG;

    visual->interpolation_mode = interpolation_mode;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetBorderMode(IDCompositionVisualUnknown *iface,
        enum DCOMPOSITION_BORDER_MODE border_mode)
{
    struct composition_visual *visual = impl_from_IDCompositionVisualUnknown(iface);

    TRACE("iface %p, border_mode %d.\n", iface, border_mode);

    if (border_mode != DCOMPOSITION_BORDER_MODE_SOFT
        && border_mode != DCOMPOSITION_BORDER_MODE_HARD
        && border_mode != DCOMPOSITION_BORDER_MODE_INHERIT)
        return E_INVALIDARG;

    visual->border_mode = border_mode;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetClipObject(IDCompositionVisualUnknown *iface,
        IDCompositionClip *clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
}

/* C++ method: SetClip(THIS_ const D2D_RECT_F &rect).  Use pointer instead of reference */
static HRESULT STDMETHODCALLTYPE visual_SetClip(IDCompositionVisualUnknown *iface, const D2D_RECT_F *rect)
{
    FIXME("iface %p, rect %p stub!\n", iface, rect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetContent(IDCompositionVisualUnknown *iface, IUnknown *content)
{
    struct composition_visual *visual = impl_from_IDCompositionVisualUnknown(iface);
    ID2D1GdiInteropRenderTarget *interop;
    ID2D1DeviceContext *device_context;
    IDXGISwapChain1 *dxgi_swapchain;
    IDCompositionSurface *suface;
    IDXGIDevice *dxgi_device;
    ID2D1Device *d2d_device;
    const GUID *iid = NULL;
    HRESULT hr;

    FIXME("iface %p, content %p semi-stub!\n", iface, content);

    dcomp_lock();

    if (content)
    {
        if (SUCCEEDED(IUnknown_QueryInterface(content, &IID_IDXGISwapChain1,
                (void **)&dxgi_swapchain)))
        {
            iid = &IID_IDXGISwapChain1;

            hr = IDXGISwapChain1_GetDevice(dxgi_swapchain, &IID_IDXGIDevice, (void **)&dxgi_device);
            IDXGISwapChain1_Release(dxgi_swapchain);
            if (FAILED(hr))
            {
                ERR("Failed to get the swapchain device, hr %#lx.\n", hr);
                dcomp_unlock();
                return hr;
            }
        }
        else if (SUCCEEDED(IUnknown_QueryInterface(content, &IID_IDCompositionSurface, (void **)&suface)))
        {
            struct composition_surface *dcomp_surface = unsafe_impl_from_IDCompositionSurface(suface);

            if (IsEqualGUID(&dcomp_surface->physical_surface_iid, &IID_IDXGISurface))
            {
                iid = &IID_IDCompositionSurface;

                hr = IDXGISurface_GetDevice((IDXGISurface *)dcomp_surface->physical_surface,
                        &IID_IDXGIDevice, (void **)&dxgi_device);
                IDCompositionSurface_Release(suface);
                if (FAILED(hr))
                {
                    ERR("Failed to get the dxgi surface device, hr %#lx.\n", hr);
                    dcomp_unlock();
                    return hr;
                }
            }
            else
            {
                FIXME("Only IDCompositionSurface from IDXGISurface is currently supported.\n");
                IDCompositionSurface_Release(suface);
                dcomp_unlock();
                return E_INVALIDARG;
            }
        }
        else
        {
            FIXME("Only IDXGISwapChain1 or IDCompositionSurface are currently supported.\n");
            dcomp_unlock();
            return E_INVALIDARG;
        }

        hr = D2D1CreateDevice(dxgi_device, NULL, &d2d_device);
        IDXGIDevice_Release(dxgi_device);
        if (FAILED(hr))
        {
            ERR("Failed to create a D2D device, hr %#lx.\n", hr);
            dcomp_unlock();
            return hr;
        }

        hr = ID2D1Device_CreateDeviceContext(d2d_device, D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                &device_context);
        ID2D1Device_Release(d2d_device);
        if (FAILED(hr))
        {
            ERR("Failed to create a D2D device context, hr %#lx.\n", hr);
            dcomp_unlock();
            return hr;
        }

        if (FAILED(hr = ID2D1DeviceContext_QueryInterface(device_context,
                &IID_ID2D1GdiInteropRenderTarget, (void **)&interop)))
        {
            ERR("Failed to get a ID2D1GdiInteropRenderTarget, hr %#lx.\n", hr);
            ID2D1DeviceContext_Release(device_context);
            dcomp_unlock();
            return hr;
        }
    }

    if (visual->interop)
    {
        ID2D1GdiInteropRenderTarget_Release(visual->interop);
        visual->interop = NULL;
    }
    if (visual->device_context)
    {
        ID2D1DeviceContext_Release(visual->device_context);
        visual->device_context = NULL;
    }
    if (visual->content)
    {
        IUnknown_Release(visual->content);
        visual->content = NULL;
    }

    if (content)
    {
        IUnknown_AddRef(content);
        visual->content = content;
        visual->content_iid = *iid;
        visual->device_context = device_context;
        visual->interop = interop;
    }

    dcomp_unlock();
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_AddVisual(IDCompositionVisualUnknown *iface,
        IDCompositionVisual *visual, BOOL insert_above, IDCompositionVisual *reference_visual)
{
    struct composition_visual *parent_visual_impl = impl_from_IDCompositionVisualUnknown(iface);
    struct composition_visual *child_visual_impl, *ref_visual_impl = NULL;

    FIXME("iface %p, visual %p, insert_above %d, reference_visual %p.\n", iface, visual,
          insert_above, reference_visual);

    if (!visual)
        return E_INVALIDARG;

    child_visual_impl = impl_from_IDCompositionVisual(visual);
    if (child_visual_impl->is_root || child_visual_impl->is_child)
        return E_INVALIDARG;

    dcomp_lock();
    if (reference_visual)
    {
        ref_visual_impl = impl_from_IDCompositionVisual(reference_visual);
        if (!(ref_visual_impl->is_child && ref_visual_impl->parent == parent_visual_impl))
        {
            dcomp_unlock();
            return E_INVALIDARG;
        }

        if (insert_above)
            list_add_after(&ref_visual_impl->entry, &child_visual_impl->entry);
        else
            list_add_before(&ref_visual_impl->entry, &child_visual_impl->entry);
    }
    else
    {
        if (insert_above)
            list_add_tail(&parent_visual_impl->child_visuals, &child_visual_impl->entry);
        else
            list_add_head(&parent_visual_impl->child_visuals, &child_visual_impl->entry);
    }

    child_visual_impl->is_child = TRUE;
    child_visual_impl->parent = parent_visual_impl;
    dcomp_unlock();
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_RemoveVisual(IDCompositionVisualUnknown *iface,
        IDCompositionVisual *visual)
{
    struct composition_visual *parent_visual_impl = impl_from_IDCompositionVisualUnknown(iface);
    struct composition_visual *child_visual_impl;

    FIXME("iface %p, visual %p.\n", iface, visual);

    if (!visual)
        return E_INVALIDARG;

    child_visual_impl = impl_from_IDCompositionVisual(visual);
    dcomp_lock();
    if (!(child_visual_impl->is_child && child_visual_impl->parent == parent_visual_impl))
    {
        dcomp_unlock();
        return E_INVALIDARG;
    }

    list_remove(&child_visual_impl->entry);
    child_visual_impl->is_child = FALSE;
    child_visual_impl->parent = NULL;
    dcomp_unlock();
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_RemoveAllVisuals(IDCompositionVisualUnknown *iface)
{
    struct composition_visual *parent_visual_impl = impl_from_IDCompositionVisualUnknown(iface);
    struct composition_visual *child_visual_impl;

    FIXME("iface %p.\n", iface);

    dcomp_lock();
    LIST_FOR_EACH_ENTRY(child_visual_impl, &parent_visual_impl->child_visuals, struct composition_visual, entry)
    {
        list_remove(&child_visual_impl->entry);
        child_visual_impl->is_child = FALSE;
        child_visual_impl->parent = NULL;
    }
    dcomp_unlock();
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetCompositeMode(IDCompositionVisualUnknown *iface,
        enum DCOMPOSITION_COMPOSITE_MODE composite_mode)
{
    FIXME("iface %p, composite_mode %d stub!\n", iface, composite_mode);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetOpacityMode(IDCompositionVisualUnknown *iface,
        enum DCOMPOSITION_OPACITY_MODE opacity_mode)
{
    FIXME("iface %p, opacity_mode %d stub!\n", iface, opacity_mode);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetBackFaceVisibility(IDCompositionVisualUnknown *iface,
        enum DCOMPOSITION_BACKFACE_VISIBILITY visibility)
{
    struct composition_visual *visual = impl_from_IDCompositionVisualUnknown(iface);

    TRACE("iface %p, visibility %d.\n", iface, visibility);

    if (visibility != DCOMPOSITION_BACKFACE_VISIBILITY_VISIBLE
        && visibility != DCOMPOSITION_BACKFACE_VISIBILITY_HIDDEN
        && visibility != DCOMPOSITION_BACKFACE_VISIBILITY_INHERIT)
        return E_INVALIDARG;

    visual->visibility = visibility;
    return S_OK;
}

static HRESULT WINAPI visual_unknown_method1(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method2(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method3(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method4(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method5(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method6(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method7(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method8(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method9(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method10(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method11(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method12(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method13(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

/* parameter1 is from the second parameter of NtCreateCompositionInputSink() */
static HRESULT WINAPI visual_unknown_method14(IDCompositionVisualUnknown *iface, void *parameter1)
{
    FIXME("iface %p parameter1 %p stub!\n", iface, parameter1);
    return S_OK;
}

static HRESULT WINAPI visual_unknown_method15(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method16(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method17(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method18(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method19(IDCompositionVisualUnknown *iface, float parameter1)
{
    FIXME("iface %p parameter1 %f stub!\n", iface, parameter1);

    return S_OK;
}

static HRESULT WINAPI visual_unknown_method20(IDCompositionVisualUnknown *iface, float parameter1)
{
    FIXME("iface %p parameter1 %f stub!\n", iface, parameter1);

    return S_OK;
}

static HRESULT WINAPI visual_unknown_method21(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method22(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method23(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method24(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method25(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method26(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method27(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method28(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method29(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method30(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method31(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method32(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method33(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method34(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method35(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method36(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method37(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method38(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method39(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method40(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method41(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method42(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method43(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method44(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method45(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method46(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method47(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method48(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method49(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method50(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method51(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method52(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method53(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method54(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method55(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method56(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method57(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method58(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method59(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method60(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method61(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method62(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method63(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method64(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method65(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method66(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method67(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method68(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method69(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method70(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method71(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method72(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method73(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method74(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method75(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method76(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method77(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method78(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method79(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method80(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method81(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method82(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method83(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method84(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method85(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method86(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method87(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method88(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method89(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method90(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method91(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method92(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method93(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method94(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method95(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method96(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method97(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method98(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI visual_unknown_method99(IDCompositionVisualUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static const struct IDCompositionVisualUnknownVtbl visual_unknown_vtbl =
{
    /* IUnknown methods */
    visual_QueryInterface,
    visual_AddRef,
    visual_Release,
    /* IDCompositionVisual methods */
    visual_SetOffsetXAnimation,
    visual_SetOffsetX,
    visual_SetOffsetYAnimation,
    visual_SetOffsetY,
    visual_SetTransformObject,
    visual_SetTransform,
    visual_SetTransformParent,
    visual_SetEffect,
    visual_SetBitmapInterpolationMode,
    visual_SetBorderMode,
    visual_SetClipObject,
    visual_SetClip,
    visual_SetContent,
    visual_AddVisual,
    visual_RemoveVisual,
    visual_RemoveAllVisuals,
    visual_SetCompositeMode,
    /* IDCompositionVisual2 methods */
    visual_SetOpacityMode,
    visual_SetBackFaceVisibility,
    /* IDCompositionVisualUnknown methods */
    visual_unknown_method1,
    visual_unknown_method2,
    visual_unknown_method3,
    visual_unknown_method4,
    visual_unknown_method5,
    visual_unknown_method6,
    visual_unknown_method7,
    visual_unknown_method8,
    visual_unknown_method9,
    visual_unknown_method10,
    visual_unknown_method11,
    visual_unknown_method12,
    visual_unknown_method13,
    visual_unknown_method14,
    visual_unknown_method15,
    visual_unknown_method16,
    visual_unknown_method17,
    visual_unknown_method18,
    visual_unknown_method19,
    visual_unknown_method20,
    visual_unknown_method21,
    visual_unknown_method22,
    visual_unknown_method23,
    visual_unknown_method24,
    visual_unknown_method25,
    visual_unknown_method26,
    visual_unknown_method27,
    visual_unknown_method28,
    visual_unknown_method29,
    visual_unknown_method30,
    visual_unknown_method31,
    visual_unknown_method32,
    visual_unknown_method33,
    visual_unknown_method34,
    visual_unknown_method35,
    visual_unknown_method36,
    visual_unknown_method37,
    visual_unknown_method38,
    visual_unknown_method39,
    visual_unknown_method40,
    visual_unknown_method41,
    visual_unknown_method42,
    visual_unknown_method43,
    visual_unknown_method44,
    visual_unknown_method45,
    visual_unknown_method46,
    visual_unknown_method47,
    visual_unknown_method48,
    visual_unknown_method49,
    visual_unknown_method50,
    visual_unknown_method51,
    visual_unknown_method52,
    visual_unknown_method53,
    visual_unknown_method54,
    visual_unknown_method55,
    visual_unknown_method56,
    visual_unknown_method57,
    visual_unknown_method58,
    visual_unknown_method59,
    visual_unknown_method60,
    visual_unknown_method61,
    visual_unknown_method62,
    visual_unknown_method63,
    visual_unknown_method64,
    visual_unknown_method65,
    visual_unknown_method66,
    visual_unknown_method67,
    visual_unknown_method68,
    visual_unknown_method69,
    visual_unknown_method70,
    visual_unknown_method71,
    visual_unknown_method72,
    visual_unknown_method73,
    visual_unknown_method74,
    visual_unknown_method75,
    visual_unknown_method76,
    visual_unknown_method77,
    visual_unknown_method78,
    visual_unknown_method79,
    visual_unknown_method80,
    visual_unknown_method81,
    visual_unknown_method82,
    visual_unknown_method83,
    visual_unknown_method84,
    visual_unknown_method85,
    visual_unknown_method86,
    visual_unknown_method87,
    visual_unknown_method88,
    visual_unknown_method89,
    visual_unknown_method90,
    visual_unknown_method91,
    visual_unknown_method92,
    visual_unknown_method93,
    visual_unknown_method94,
    visual_unknown_method95,
    visual_unknown_method96,
    visual_unknown_method97,
    visual_unknown_method98,
    visual_unknown_method99,
};

HRESULT create_visual(int version, REFIID iid, void **new_visual)
{
    struct composition_visual *visual;
    HRESULT hr;

    if (!new_visual)
        return E_INVALIDARG;

    visual = calloc(1, sizeof(*visual));
    if (!visual)
        return E_OUTOFMEMORY;

    visual->IDCompositionVisualUnknown_iface.lpVtbl = &visual_unknown_vtbl;
    visual->version = version;
    visual->ref = 1;
    list_init(&visual->child_visuals);
    hr = IUnknown_QueryInterface(&visual->IDCompositionVisualUnknown_iface, iid, new_visual);
    IUnknown_Release(&visual->IDCompositionVisualUnknown_iface);
    return hr;
}

HRESULT create_visual_from_shared_visual_handle(HANDLE shared_visual_handle, void **new_visual)
{
    struct composition_visual *visual_impl;
    HANDLE handle;
    HRESULT hr;

    if (!DuplicateHandle(GetCurrentProcess(), shared_visual_handle, GetCurrentProcess(), &handle, 0,
                         FALSE, DUPLICATE_SAME_ACCESS))
    {
        ERR("Cannot duplicate handle, last error %lu.\n", GetLastError());
        return E_FAIL;
    }

    hr = create_visual(2, &IID_IDCompositionVisual, (void **)new_visual);
    if (FAILED(hr))
    {
        CloseHandle(handle);
        return hr;
    }

    visual_impl = impl_from_IDCompositionVisualUnknown(*new_visual);
    visual_impl->shared_visual_handle = handle;
    return S_OK;
}
