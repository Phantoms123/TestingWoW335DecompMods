set(DXVK_NATIVE_ROOT "" CACHE PATH "DXVK Native installation prefix")
find_path(DXVKNative_INCLUDE_DIR d3d9.h
    HINTS "${DXVK_NATIVE_ROOT}/include/dxvk")
find_library(DXVKNative_D3D9_LIBRARY NAMES dxvk_d3d9
    HINTS "${DXVK_NATIVE_ROOT}/lib" "${DXVK_NATIVE_ROOT}/lib64")
# CHANGED: Direct3D 11 and DXGI too, for the Direct3D 11 device (gxApi D3D11).
find_library(DXVKNative_D3D11_LIBRARY NAMES dxvk_d3d11
    HINTS "${DXVK_NATIVE_ROOT}/lib" "${DXVK_NATIVE_ROOT}/lib64")
find_library(DXVKNative_DXGI_LIBRARY NAMES dxvk_dxgi
    HINTS "${DXVK_NATIVE_ROOT}/lib" "${DXVK_NATIVE_ROOT}/lib64")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DXVKNative DEFAULT_MSG
    DXVKNative_INCLUDE_DIR DXVKNative_D3D9_LIBRARY)

if (DXVKNative_FOUND AND NOT TARGET DXVKNative::D3D9)
    add_library(DXVKNative::D3D9 UNKNOWN IMPORTED GLOBAL)
    set_target_properties(DXVKNative::D3D9 PROPERTIES
        IMPORTED_LOCATION "${DXVKNative_D3D9_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DXVKNative_INCLUDE_DIR}"
    )
endif ()
if (DXVKNative_FOUND AND DXVKNative_D3D11_LIBRARY AND DXVKNative_DXGI_LIBRARY AND NOT TARGET DXVKNative::D3D11)
    add_library(DXVKNative::D3D11 UNKNOWN IMPORTED GLOBAL)
    set_target_properties(DXVKNative::D3D11 PROPERTIES
        IMPORTED_LOCATION "${DXVKNative_D3D11_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DXVKNative_INCLUDE_DIR}"
    )
    add_library(DXVKNative::DXGI UNKNOWN IMPORTED GLOBAL)
    set_target_properties(DXVKNative::DXGI PROPERTIES
        IMPORTED_LOCATION "${DXVKNative_DXGI_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DXVKNative_INCLUDE_DIR}"
    )
endif ()
mark_as_advanced(DXVKNative_INCLUDE_DIR DXVKNative_D3D9_LIBRARY DXVKNative_D3D11_LIBRARY DXVKNative_DXGI_LIBRARY)
