/* WinRT Windows.Storage Implementation
 *
 * Copyright (C) 2025 Ignacy Kuchciński
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

#include "private.h"
#include "initguid.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(storage);

struct storagefile_statics
{
    IActivationFactory IActivationFactory_iface;
    IStorageFileStatics IStorageFileStatics_iface;
    LONG ref;
};

static inline struct storagefile_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct storagefile_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct storagefile_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IStorageFileStatics ))
    {
        *out = &impl->IStorageFileStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct storagefile_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct storagefile_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( storagefile_statics, IStorageFileStatics, struct storagefile_statics, IActivationFactory_iface )

static HRESULT WINAPI storagefile_statics_GetFileFromPathAsync( IStorageFileStatics *iface,
                                                                HSTRING path,
                                                                IAsyncOperation_StorageFile **operation )
{
    FIXME( "iface %p, path %p, operation %p stub!\n", iface, path, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI storagefile_statics_GetFileFromApplicationUriAsync( IStorageFileStatics *iface,
                                                                          IUriRuntimeClass *uri,
                                                                          IAsyncOperation_StorageFile **operation )
{
    FIXME( "iface %p, uri %p, operation %p stub!\n", iface, uri, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI storagefile_statics_CreateStreamedFileAsync( IStorageFileStatics *iface,
                                                                   HSTRING display_name_with_extensions,
                                                                   IStreamedFileDataRequestedHandler *data_requested,
                                                                   IRandomAccessStreamReference *thumbnail,
                                                                   IAsyncOperation_StorageFile **operation )
{
    FIXME( "iface %p, display_name_with_extensions %p, data_requested %p, thumbnail %p, operation %p stub!\n", iface, display_name_with_extensions, data_requested, thumbnail, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI storagefile_statics_ReplaceWithStreamedFileAsync( IStorageFileStatics *iface,
                                                                        IStorageFile *file_to_replace,
                                                                        IStreamedFileDataRequestedHandler *data_requested,
                                                                        IRandomAccessStreamReference *thumbnail,
                                                                        IAsyncOperation_StorageFile **operation )
{
    FIXME( "iface %p, file_to_replace %p, data_requested %p, thumbnail %p, operation %p stub!\n", iface, file_to_replace, data_requested, thumbnail, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI storagefile_statics_CreateStreamedFileFromUriAsync( IStorageFileStatics *iface,
                                                                          HSTRING display_name_with_extensions,
                                                                          IUriRuntimeClass *uri,
                                                                          IRandomAccessStreamReference *thumbnail,
                                                                          IAsyncOperation_StorageFile **operation )
{
    FIXME( "iface %p, display_name_with_extensions %p, uri %p, thumbnail %p, operation %p stub!\n", iface, display_name_with_extensions, uri, thumbnail, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI storagefile_statics_ReplaceWithStreamedFileFromUriAsync( IStorageFileStatics *iface,
                                                                               IStorageFile *file_to_replace,
                                                                               IUriRuntimeClass *uri,
                                                                               IRandomAccessStreamReference *thumbnail,
                                                                               IAsyncOperation_StorageFile **operation )
{
    FIXME( "iface %p, file_to_replace %p, uri %p, thumbnail %p, operation %p stub!\n", iface, file_to_replace, uri, thumbnail, operation );
    return E_NOTIMPL;
}

static const struct IStorageFileStaticsVtbl storagefile_statics_vtbl =
{
    storagefile_statics_QueryInterface,
    storagefile_statics_AddRef,
    storagefile_statics_Release,
    /* IInspectable methods */
    storagefile_statics_GetIids,
    storagefile_statics_GetRuntimeClassName,
    storagefile_statics_GetTrustLevel,
    /* IStorageFileStatics methods */
    storagefile_statics_GetFileFromPathAsync,
    storagefile_statics_GetFileFromApplicationUriAsync,
    storagefile_statics_CreateStreamedFileAsync,
    storagefile_statics_ReplaceWithStreamedFileAsync,
    storagefile_statics_CreateStreamedFileFromUriAsync,
    storagefile_statics_ReplaceWithStreamedFileFromUriAsync,
};

static struct storagefile_statics storagefile_statics =
{
    {&factory_vtbl},
    {&storagefile_statics_vtbl},
    0,
};

IActivationFactory *storagefile_factory = &storagefile_statics.IActivationFactory_iface;
