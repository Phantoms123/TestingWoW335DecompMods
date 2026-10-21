/* Minimal Windows types for building Wine's bundled vkd3d-shader outside Wine (as upstream vkd3d_windows.h). */
#ifndef VKD3D_SHIM_WINDOWS_H
#define VKD3D_SHIM_WINDOWS_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int HRESULT;
typedef uint16_t WCHAR;
typedef uint32_t ULONG, DWORD, UINT;
typedef int32_t LONG, BOOL, INT;
typedef uint8_t BYTE;
typedef uint64_t UINT64, ULONG64;
typedef int64_t INT64, LONG64;
typedef uint16_t WORD, UINT16;
typedef uint32_t UINT32;
typedef uintptr_t SIZE_T, ULONG_PTR, UINT_PTR;
typedef intptr_t LONG_PTR, INT_PTR;
typedef void* HANDLE;
typedef void* HMODULE;
typedef float FLOAT;
typedef char CHAR;
typedef const char* LPCSTR;
typedef const WCHAR* LPCWSTR;
typedef void* LPVOID;
typedef struct _GUID { uint32_t Data1; uint16_t Data2; uint16_t Data3; uint8_t Data4[8]; } GUID, IID, CLSID;
typedef const GUID* REFGUID;
typedef const IID* REFIID;
#define WINAPI
#define STDMETHODCALLTYPE
#define TRUE 1
#define FALSE 0
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define E_OUTOFMEMORY ((HRESULT)0x8007000e)
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_POINTER ((HRESULT)0x80004003)
#define E_ABORT ((HRESULT)0x80004004)
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887a0002)
#define DXGI_ERROR_UNSUPPORTED ((HRESULT)0x887a0004)
#define DXGI_ERROR_ALREADY_EXISTS ((HRESULT)0x887a0036)
#define DXGI_ERROR_MORE_DATA ((HRESULT)0x887a0003)
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr) ((HRESULT)(hr) < 0)
#define MAKE_HRESULT(sev, fac, code) ((HRESULT)(((unsigned long)(sev) << 31) | ((unsigned long)(fac) << 16) | ((unsigned long)(code))))
#define IsEqualGUID(a, b) (!memcmp(a, b, sizeof(GUID)))
#define MAX_PATH 260
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#define CONTAINING_RECORD(address, type, field) ((type *)((char *)(address) - offsetof(type, field)))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))
#endif
