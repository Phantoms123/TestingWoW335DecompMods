// CHANGED: not in the original 12340 client. The Direct3D 11 Gx device; see CGxDeviceD3d11.h.
//
// It is modelled on the Direct3D 9 device (CGxDeviceD3d, Native/Device.cpp): the same Gx state goes in, the same
// order of syncing it before each draw, the same window handling (SDL, through DXVK's native WSI). What Direct3D 9
// kept as device state (render states, texture stage states, the fixed-function transforms, lights and material)
// is kept here and turned into Direct3D 11 state objects, constant buffers and the fixed-function emulation shader.

#include "Engine/Source/Gx/CGxDeviceD3d11/CGxDeviceD3d11.h"
#include <SDL.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "Engine/Source/Gx/Blit.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxDevice/SDL/Display.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/SDL/Window.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Os/Window.h"
#include "Engine/Source/Storm/H/storm.h"

namespace {
#include "Engine/Source/Gx/CGxDeviceD3d11/Shaders/vkd3d/blit_ps.h"
#include "Engine/Source/Gx/CGxDeviceD3d11/Shaders/vkd3d/blit_vs.h"
#include "Engine/Source/Gx/CGxDeviceD3d11/Shaders/vkd3d/depthcopy_ps.h"
#include "Engine/Source/Gx/CGxDeviceD3d11/Shaders/vkd3d/ffp_ps.h"
#include "Engine/Source/Gx/CGxDeviceD3d11/Shaders/vkd3d/ffp_vs.h"

template <typename T>
void SafeRelease(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

// The fixed-function constant buffer; the layout of cbuffer FixedFunction in Shaders/FixedFunction.hlsl.
struct FfpConstants {
    float fogColor[4];
    float fogEnable[4];
    float alphaTest[4];
    float world[16];
    float view[16];
    float proj[16];
    float tex[8][16];
    int32_t stage[8][4];
    float matDiffuse[4];
    float matEmissive[4];
    float matSpecular[4];
    float ambient[4];
    float lightPos[4][4];
    float lightDiffuse[4][4];
    float lightAmbient[4][4];
    float lightSpecular[4][4];
    float lightAtten[4][4];
    int32_t flags[4];
    int32_t flags2[4];
    float fog[4];
    float clip[6][4];
    int32_t clipMask[4];
};

static_assert(sizeof(FfpConstants) == 89 * 16, "FfpConstants must match cbuffer FixedFunction");

// Direct3D 9's blend factors for each Gx blending mode (CGxDeviceD3d::s_srcBlend, s_dstBlend), as Direct3D 11's.
const D3D11_BLEND s_srcBlend[12] = { D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_BLEND_FACTOR };
const D3D11_BLEND s_dstBlend[12] = { D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_SRC_COLOR, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_INV_BLEND_FACTOR };

// Alpha blending factors: colour factors have no alpha counterpart for the destination colour.
D3D11_BLEND AlphaBlend(D3D11_BLEND blend) {
    switch (blend) {
        case D3D11_BLEND_SRC_COLOR:
            return D3D11_BLEND_SRC_ALPHA;
        case D3D11_BLEND_INV_SRC_COLOR:
            return D3D11_BLEND_INV_SRC_ALPHA;
        case D3D11_BLEND_DEST_COLOR:
            return D3D11_BLEND_DEST_ALPHA;
        case D3D11_BLEND_INV_DEST_COLOR:
            return D3D11_BLEND_INV_DEST_ALPHA;
        default:
            return blend;
    }
}

const D3D11_COMPARISON_FUNC s_cmpFunc[4] = { D3D11_COMPARISON_LESS_EQUAL, D3D11_COMPARISON_EQUAL, D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_LESS };
const D3D11_PRIMITIVE_TOPOLOGY s_topology[GxPrims_Last] = { D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, D3D11_PRIMITIVE_TOPOLOGY_LINELIST, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST };

// Vertex attribute types (CGxVertexAttrib::m_type): D3DCOLOR, UBYTE4, UBYTE4N, FLOAT2, FLOAT3, SHORT2, FLOAT1.
const uint32_t s_attribSize[7] = { 4, 4, 4, 8, 12, 4, 4 };
const DXGI_FORMAT s_attribFormat[7] = { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R32_FLOAT };
const char* const s_attribSemantic[GxVAs_Last] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "COLOR", "COLOR", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD" };
const uint32_t s_attribSemanticIndex[GxVAs_Last] = { 0, 0, 0, 0, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7 };

// The texture format each Gx format is stored as. 16-bit colour formats become 32-bit (DXVK's Direct3D 11 has no
// 4444 or 1555 format every driver takes), and the upload converts.
DXGI_FORMAT TextureFormat(EGxTexFormat format) {
    switch (format) {
        case GxTex_Abgr8888:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case GxTex_Argb8888:
        case GxTex_Argb4444:
        case GxTex_Argb1555:
        case GxTex_Rgb565:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case GxTex_Dxt1:
            return DXGI_FORMAT_BC1_UNORM;
        case GxTex_Dxt3:
            return DXGI_FORMAT_BC2_UNORM;
        case GxTex_Dxt5:
            return DXGI_FORMAT_BC3_UNORM;
        case GxTex_Uv88:
            return DXGI_FORMAT_R8G8_SNORM;
        case GxTex_Gr1616F:
            return DXGI_FORMAT_R16G16_FLOAT;
        case GxTex_R32F:
            return DXGI_FORMAT_R32_FLOAT;
        case GxTex_D24X8:
        case GxTex_Intz:
            return DXGI_FORMAT_R24G8_TYPELESS;
        default:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

bool IsBlockFormat(EGxTexFormat format) {
    return format >= GxTex_Dxt1 && format <= GxTex_Dxt5;
}

uint32_t BytesPerTexel(EGxTexFormat stored) {
    switch (stored) {
        case GxTex_Uv88:
            return 2;
        default:
            return 4;
    }
}

// Converts rows of the texel data the client hands over (dataFormat) to what the texture holds (stored).
bool ConvertTexels(const C2iVector& size, const uint8_t* in, uint32_t inStride, EGxTexFormat dataFormat, uint8_t* out, uint32_t outStride, EGxTexFormat stored) {
    if (dataFormat == stored || (GxGetBlitFormat(dataFormat) == GxGetBlitFormat(stored) && GxGetBlitFormat(stored) != BlitFormat_Unknown)) {
        uint32_t rows = IsBlockFormat(stored) ? std::max(1, (size.y + 3) / 4) : size.y;
        uint32_t rowBytes = IsBlockFormat(stored) ? std::max(1, (size.x + 3) / 4) * (stored == GxTex_Dxt1 ? 8 : 16) : size.x * BytesPerTexel(stored);

        for (uint32_t y = 0; y < rows; y++) {
            memcpy(out + y * outStride, in + y * inStride, rowBytes);
        }

        return true;
    }

    if (stored == GxTex_Argb8888 && (dataFormat == GxTex_Argb4444 || dataFormat == GxTex_Argb1555 || dataFormat == GxTex_Rgb565)) {
        for (int32_t y = 0; y < size.y; y++) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(in + y * inStride);
            uint8_t* dst = out + y * outStride;

            for (int32_t x = 0; x < size.x; x++) {
                uint32_t p = src[x];
                uint32_t a, r, g, b;

                if (dataFormat == GxTex_Argb4444) {
                    a = ((p >> 12) & 0xF) * 17;
                    r = ((p >> 8) & 0xF) * 17;
                    g = ((p >> 4) & 0xF) * 17;
                    b = (p & 0xF) * 17;
                } else if (dataFormat == GxTex_Argb1555) {
                    a = (p & 0x8000) ? 255 : 0;
                    r = ((p >> 10) & 0x1F) * 255 / 31;
                    g = ((p >> 5) & 0x1F) * 255 / 31;
                    b = (p & 0x1F) * 255 / 31;
                } else {
                    a = 255;
                    r = ((p >> 11) & 0x1F) * 255 / 31;
                    g = ((p >> 5) & 0x3F) * 255 / 63;
                    b = (p & 0x1F) * 255 / 31;
                }

                dst[4 * x + 0] = static_cast<uint8_t>(b);
                dst[4 * x + 1] = static_cast<uint8_t>(g);
                dst[4 * x + 2] = static_cast<uint8_t>(r);
                dst[4 * x + 3] = static_cast<uint8_t>(a);
            }
        }

        return true;
    }

    return Blit(size, BlitAlpha_0, in, inStride, GxGetBlitFormat(dataFormat), out, outStride, GxGetBlitFormat(stored)) != 0;
}

uint32_t Rgba(uint32_t argb, float* out) {
    out[0] = ((argb >> 16) & 0xFF) / 255.0f;
    out[1] = ((argb >> 8) & 0xFF) / 255.0f;
    out[2] = (argb & 0xFF) / 255.0f;
    out[3] = ((argb >> 24) & 0xFF) / 255.0f;
    return argb;
}

void Matrix(const C44Matrix& m, float* out) {
    memcpy(out, &m, sizeof(float) * 16);
}

uint64_t Hash(uint64_t hash, uint64_t value) {
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return hash;
}

// What the device needs to know of a shader model 4 shader: its input signature (vertex shaders), the samplers it
// declares with comparison mode (shadow map lookups) and the size of constant buffer 0.
void ParseDxbc(const uint8_t* code, size_t size, CGxDeviceD3d11::Shader& shader) {
    if (size < 32 || memcmp(code, "DXBC", 4)) {
        return;
    }

    uint32_t chunkCount;
    memcpy(&chunkCount, code + 28, 4);

    for (uint32_t i = 0; i < chunkCount && 32 + 4 * i + 4 <= size; i++) {
        uint32_t offset;
        memcpy(&offset, code + 32 + 4 * i, 4);

        if (offset + 8 > size) {
            continue;
        }

        const uint8_t* chunk = code + offset;
        uint32_t chunkSize;
        memcpy(&chunkSize, chunk + 4, 4);
        const uint8_t* data = chunk + 8;

        if (offset + 8 + chunkSize > size) {
            continue;
        }

        if (!memcmp(chunk, "ISGN", 4)) {
            uint32_t count;
            memcpy(&count, data, 4);

            for (uint32_t e = 0; e < count && 8 + 24 * (e + 1) <= chunkSize; e++) {
                uint32_t element[6];
                memcpy(element, data + 8 + 24 * e, 24);
                CGxDeviceD3d11::Shader::Input input = {};

                if (element[0] < chunkSize) {
                    SStrCopy(input.m_name, reinterpret_cast<const char*>(data + element[0]), sizeof(input.m_name));
                }

                input.m_index = element[1];
                input.m_componentType = element[3];

                // System values (vertex and instance IDs) are not fed from vertex buffers.
                if (element[2] == 0) {
                    shader.m_inputs.push_back(input);
                }
            }
        } else if (!memcmp(chunk, "SHDR", 4) || !memcmp(chunk, "SHEX", 4)) {
            const uint32_t* tokens = reinterpret_cast<const uint32_t*>(data);
            uint32_t tokenCount = chunkSize / 4;
            uint32_t position = 2;

            while (position < tokenCount) {
                uint32_t token = tokens[position];
                uint32_t opcode = token & 0x7FF;
                uint32_t length = (token >> 24) & 0x7F;

                if (opcode == 0x35) {
                    // customdata: the length is in the next token.
                    length = position + 1 < tokenCount ? tokens[position + 1] : 0;
                }

                if (!length) {
                    break;
                }

                if (opcode == 0x5A && length >= 3) {
                    // dcl_sampler: mode in bits 11-14, the register in the operand's index.
                    uint32_t mode = (token >> 11) & 0xF;
                    uint32_t reg = tokens[position + 2];

                    if (mode == 1 && reg < 32) {
                        shader.m_comparisonSamplers |= 1u << reg;
                    }
                } else if (opcode == 0x59 && length >= 4) {
                    // dcl_constantbuffer: cb index then size in registers.
                    uint32_t index = tokens[position + 2];
                    uint32_t registers = tokens[position + 3];

                    if (index == 0) {
                        shader.m_constants = registers;
                    }
                } else if (opcode < 0x5B && opcode != 0x35 && opcode < 0x58) {
                    // Declarations come first; stop at the first instruction.
                    break;
                }

                position += length;
            }
        }
    }
}

// Window events for the device's SDL window (the Direct3D 9 device's SDL/Platform.cpp, for this device).
void SetClientRect(CGxDeviceD3d11& device, CGxFormat* format = nullptr) {
    int width, height;
    SDL_GetWindowSize(static_cast<SDL_Window*>(device.m_hwnd), &width, &height);
    CRect rect;
    rect.minX = rect.minY = 0.0f;
    rect.maxX = static_cast<float>(width);
    rect.maxY = static_cast<float>(height);
    device.DeviceSetDefWindow(rect);

    if (format) {
        format->m_size = { width, height };
    }
}

void WindowEvent(void* context, const SDL_WindowEvent& event) {
    auto& device = *static_cast<CGxDeviceD3d11*>(context);

    switch (event.event) {
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            device.DeviceWM(GxWM_SetFocus, 0, 0);
            break;

        case SDL_WINDOWEVENT_FOCUS_LOST:
            device.DeviceWM(GxWM_KillFocus, 0, 0);
            device.ISetWindowFocus(0);
            break;

        case SDL_WINDOWEVENT_HIDDEN:
        case SDL_WINDOWEVENT_MINIMIZED:
            device.DeviceWM(GxWM_Size, 0, 1);
            break;

        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_SHOWN:
        case SDL_WINDOWEVENT_RESTORED:
        case SDL_WINDOWEVENT_DISPLAY_CHANGED: {
            int width, height;
            SDL_GetWindowSize(static_cast<SDL_Window*>(device.m_hwnd), &width, &height);

            if (width <= 0 || height <= 0) {
                break;
            }

            const CRect& current = device.DeviceDefWindow();

            if (device.m_windowVisible && current.maxX == width && current.maxY == height) {
                break;
            }

            CRect rect;
            rect.minX = rect.minY = 0.0f;
            rect.maxX = static_cast<float>(width);
            rect.maxY = static_cast<float>(height);
            device.DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 0);
            break;
        }

        default:
            break;
    }
}

void DetachWindow(CGxDeviceD3d11& device) {
    if (OsGuiGetWindow(0) == device.m_hwnd) {
        OsGuiSetGxWindow(nullptr);
    }

    if (device.m_hwnd && device.m_ownhwnd) {
        SDL_DestroyWindow(static_cast<SDL_Window*>(device.m_hwnd));
    }

    device.m_hwnd = nullptr;
}
}

CGxDevice* CreateCGxDeviceD3d11() {
    return new CGxDeviceD3d11();
}

int32_t D3d11Available() {
    SDL_setenv("DXVK_WSI_DRIVER", "SDL2", 1);
    IDXGIFactory1* factory = nullptr;

    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
        return 0;
    }

    IDXGIAdapter1* adapter = nullptr;
    bool available = SUCCEEDED(factory->EnumAdapters1(0, &adapter)) && adapter;
    SafeRelease(adapter);
    factory->Release();
    return available;
}

int32_t D3d11AdapterFormats(TSGrowableArray<CGxFormat>& formats) {
    SDL_setenv("DXVK_WSI_DRIVER", "SDL2", 1);
    IDXGIFactory1* factory = nullptr;

    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
        return 0;
    }

    // The window modes SDL reports, each with the depth formats and sample counts Direct3D 11 always has.
    const int display = GxSdlDisplay();
    const int count = SDL_GetNumDisplayModes(display);
    const CGxFormat::Format depths[] = { CGxFormat::Fmt_Ds24X, CGxFormat::Fmt_Ds248 };
    const uint32_t samples[] = { 1, 2, 4, 8 };

    for (int i = 0; i < count; i++) {
        SDL_DisplayMode mode;

        if (SDL_GetDisplayMode(display, i, &mode) != 0 || mode.w < 640 || mode.h < 480) {
            continue;
        }

        CGxFormat format;
        memset(&format, 0, sizeof(format));
        format.m_size = { mode.w, mode.h };
        format.m_refreshRate = mode.refresh_rate;

        for (CGxFormat::Format color : { CGxFormat::Fmt_ArgbX888, CGxFormat::Fmt_Argb8888 }) {
            format.m_colorFormat = color;

            for (CGxFormat::Format depth : depths) {
                format.m_depthFormat = depth;

                for (uint32_t sample : samples) {
                    format.m_multisampleCount = sample;

                    if (!CGxDevice::FormatExists(formats, format)) {
                        *formats.New() = format;
                    }
                }
            }
        }
    }

    factory->Release();
    return formats.Count() != 0;
}

CGxDeviceD3d11::CGxDeviceD3d11() {
    this->m_api = GxApi_D3d11;
    this->m_caps.m_colorFormat = GxCF_argb;
    this->m_caps.unk_0018 = 1;
    this->DeviceCreatePools();
    this->DeviceCreateStreamBufs();
}

CGxDeviceD3d11::~CGxDeviceD3d11() {
}

// Window.

int32_t CGxDeviceD3d11::ICreateWindow(CGxFormat& format) {
    if (format.m_size.x < 0 || format.m_size.y < 0) {
        return 0;
    }

    uint32_t flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI;

    if (!format.m_window) {
        flags |= SDL_WINDOW_FULLSCREEN;
    } else if (format.m_maximize == 1) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    } else if (format.m_maximize == 2) {
        flags |= SDL_WINDOW_BORDERLESS;
    } else {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    const int display = GxSdlDisplay();
    SDL_Rect bounds = { 0, 0, 0, 0 };
    SDL_GetDisplayBounds(display, &bounds);
    int x = SDL_WINDOWPOS_CENTERED_DISPLAY(display);
    int y = SDL_WINDOWPOS_CENTERED_DISPLAY(display);

    if (format.m_window && !format.m_maximize && (format.m_pos.x > 0 || format.m_pos.y > 0)) {
        x = bounds.x + format.m_pos.x;
        y = bounds.y + format.m_pos.y;
    }

    SDL_Window* window = SDL_CreateWindow("World of Warcraft", x, y, format.m_size.x ? format.m_size.x : 1024, format.m_size.y ? format.m_size.y : 768, flags);

    if (!window) {
        CGxDevice::Log("SDL_CreateWindow: %s", SDL_GetError());
        return 0;
    }

    this->m_hwnd = window;
    SDL_SetWindowMinimumSize(window, 320, 240);
    CGxDevice::s_clientAdjust = { 0, 0 };
    OsGuiSetGxWindow(window);
    SetClientRect(*this, &format);

    if (SDL_GetWindowGammaRamp(window, this->m_systemGammaRamp.m_red, this->m_systemGammaRamp.m_green, this->m_systemGammaRamp.m_blue) == 0) {
        this->m_gammaRamp = this->m_systemGammaRamp;
    }

    return 1;
}

void CGxDeviceD3d11::ISetWindowFocus(int32_t focus) {
    this->m_windowFocus = focus != 0;

    if (this->m_hwnd) {
        SDL_SetWindowMouseGrab(static_cast<SDL_Window*>(this->m_hwnd), focus && !this->m_format.m_window ? SDL_TRUE : SDL_FALSE);
    }
}

void* CGxDeviceD3d11::DeviceWindow() {
    return this->m_hwnd;
}

void CGxDeviceD3d11::CapsWindowSize(CRect& rect) {
    rect = this->DeviceCurWindow();
}

void CGxDeviceD3d11::CapsWindowSizeInScreenCoords(CRect& rect) {
    rect = this->DeviceCurWindow();

    if (this->IDevIsWindowed() && this->m_hwnd) {
        int x, y;
        SDL_GetWindowPosition(static_cast<SDL_Window*>(this->m_hwnd), &x, &y);
        rect.minX += x;
        rect.maxX += x;
        rect.minY += y;
        rect.maxY += y;
    }
}

// The cursor. With the hardware cursor (the usual case) SDL shows the client's cursor image; otherwise the client
// draws it into the frame and SDL's own cursor is hidden, so only one ever shows.
void CGxDeviceD3d11::CursorSetVisible(int32_t visible) {
    CGxDevice::CursorSetVisible(visible);
    SDL_ShowCursor(this->m_hwCursor && this->m_cursorVisible ? SDL_ENABLE : SDL_DISABLE);
}

void CGxDeviceD3d11::CursorUnlock(uint32_t x, uint32_t y) {
    CGxDevice::CursorUnlock(x, y);
    this->m_hwCursorDirty = 1;
}

void CGxDeviceD3d11::ICursorCreate(const CGxFormat& format) {
    CGxDevice::ICursorCreate(format);
    this->m_hwCursorDirty = 1;
    SDL_ShowCursor(this->m_hwCursor && this->m_cursorVisible ? SDL_ENABLE : SDL_DISABLE);
}

void CGxDeviceD3d11::ICursorDestroy() {
    CGxDevice::ICursorDestroy();

    if (this->m_sdlCursor) {
        SDL_SetCursor(SDL_GetDefaultCursor());
        SDL_FreeCursor(static_cast<SDL_Cursor*>(this->m_sdlCursor));
        this->m_sdlCursor = nullptr;
    }

    this->m_hwCursorDirty = 1;
}

void CGxDeviceD3d11::ICursorDraw() {
    if (!this->m_hwCursor) {
        SDL_ShowCursor(SDL_DISABLE);
        CGxDevice::ICursorDraw();
        return;
    }

    if (!this->m_hwCursorDirty) {
        return;
    }

    // m_cursor: 32x32 ARGB.
    SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(this->m_cursor, 32, 32, 32, 32 * 4, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);

    if (surface) {
        SDL_Cursor* cursor = SDL_CreateColorCursor(surface, static_cast<int>(this->m_cursorHotspotX), static_cast<int>(this->m_cursorHotspotY));
        SDL_FreeSurface(surface);

        if (cursor) {
            SDL_SetCursor(cursor);

            if (this->m_sdlCursor) {
                SDL_FreeCursor(static_cast<SDL_Cursor*>(this->m_sdlCursor));
            }

            this->m_sdlCursor = cursor;
        }
    }

    SDL_ShowCursor(this->m_cursorVisible ? SDL_ENABLE : SDL_DISABLE);
    this->m_hwCursorDirty = 0;
}

int32_t CGxDeviceD3d11::DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2) {
    switch (wm) {
        case GxWM_Size:
            if (param2 == 1 || param2 == 2) {
                this->m_windowVisible = 0;
                break;
            }

            this->m_windowVisible = 1;
            this->DeviceSetDefWindow(*reinterpret_cast<const CRect*>(param1));

            if (this->m_swapChain && this->m_context) {
                const CRect& window = this->DeviceDefWindow();
                uint32_t width = static_cast<uint32_t>(window.maxX);
                uint32_t height = static_cast<uint32_t>(window.maxY);

                if (width && height && (width != this->m_backBufferWidth || height != this->m_backBufferHeight)) {
                    this->IReleaseBackBuffers();

                    if (SUCCEEDED(this->m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)) && this->ICreateBackBuffers()) {
                        this->IBindTargets();
                        this->NotifyOnDeviceRestored();
                    } else {
                        this->m_context = 0;
                    }
                }
            }

            this->m_viewportDirty = 1;
            this->m_scissorRectDirty = 1;
            break;

        case GxWM_DisplayChange:
            if (this->m_windowVisible) {
                this->DeviceSetDefWindow(*reinterpret_cast<const CRect*>(param1));
                this->m_viewportDirty = 1;
            }

            break;

        case GxWM_Destroy:
        case GxWM_KillFocus:
            this->m_windowFocus = 0;
            break;

        case GxWM_SetFocus:
            this->ISetWindowFocus(1);
            break;

        default:
            break;
    }

    return 0;
}

// Device.

int32_t CGxDeviceD3d11::ICreateD3d() {
    // DXVK's native WSI takes SDL windows as HWNDs.
    if (SDL_setenv("DXVK_WSI_DRIVER", "SDL2", 1) != 0) {
        return 0;
    }

    if (!this->m_factory && FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&this->m_factory)))) {
        this->m_factory = nullptr;
        CGxDevice::Log("CGxDeviceD3d11::ICreateD3d(): CreateDXGIFactory1 failed");
        return 0;
    }

    return 1;
}

int32_t CGxDeviceD3d11::ICreateBackBuffers() {
    ID3D11Texture2D* swapBuffer = nullptr;

    if (FAILED(this->m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&swapBuffer)))) {
        return 0;
    }

    D3D11_TEXTURE2D_DESC desc;
    swapBuffer->GetDesc(&desc);
    this->m_backBufferWidth = desc.Width;
    this->m_backBufferHeight = desc.Height;

    if (this->m_samples > 1) {
        // The client draws into a multisampled buffer, resolved into the swap chain's at present.
        swapBuffer->Release();
        desc.SampleDesc.Count = this->m_samples;
        desc.SampleDesc.Quality = 0;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = 0;

        if (FAILED(this->m_device->CreateTexture2D(&desc, nullptr, &this->m_backColor))) {
            return 0;
        }
    } else {
        this->m_backColor = swapBuffer;
    }

    if (FAILED(this->m_device->CreateRenderTargetView(this->m_backColor, nullptr, &this->m_backRtv))) {
        return 0;
    }

    // Typeless, so DeviceCopySampledDepth can read it.
    D3D11_TEXTURE2D_DESC depth = {};
    depth.Width = desc.Width;
    depth.Height = desc.Height;
    depth.MipLevels = 1;
    depth.ArraySize = 1;
    depth.Format = DXGI_FORMAT_R24G8_TYPELESS;
    depth.SampleDesc.Count = this->m_samples;
    depth.Usage = D3D11_USAGE_DEFAULT;
    depth.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    D3D11_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv.ViewDimension = this->m_samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;

    if (FAILED(this->m_device->CreateTexture2D(&depth, nullptr, &this->m_backDepth)) || FAILED(this->m_device->CreateDepthStencilView(this->m_backDepth, &dsv, &this->m_backDsv))) {
        return 0;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv.ViewDimension = this->m_samples > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    this->m_device->CreateShaderResourceView(this->m_backDepth, &srv, &this->m_backDepthSrv);

    return 1;
}

void CGxDeviceD3d11::IReleaseBackBuffers() {
    if (this->m_ctx) {
        this->m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }

    this->m_curRtv = nullptr;
    this->m_curDsv = nullptr;
    SafeRelease(this->m_backRtv);
    SafeRelease(this->m_backColor);
    SafeRelease(this->m_backDsv);
    SafeRelease(this->m_backDepthSrv);
    SafeRelease(this->m_resolveSrv);
    SafeRelease(this->m_resolveColor);
    SafeRelease(this->m_backDepth);
}

int32_t CGxDeviceD3d11::ICreateD3dDevice(const CGxFormat& format) {
    IDXGIAdapter1* adapter = nullptr;
    this->m_factory->EnumAdapters1(0, &adapter);

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, sizeof(levels) / sizeof(levels[0]), D3D11_SDK_VERSION, &this->m_device, &level, &this->m_ctx);
    SafeRelease(adapter);

    if (FAILED(hr) || !this->m_device) {
        CGxDevice::Log("CGxDeviceD3d11::ICreateD3dDevice(): D3D11CreateDevice failed (%08x)", static_cast<uint32_t>(hr));
        this->m_device = nullptr;
        this->m_ctx = nullptr;
        return 0;
    }

    this->m_device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&this->m_device1));
    this->m_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&this->m_ctx1));

    this->m_samples = 1;

    if (format.m_multisampleCount > 1) {
        for (uint32_t samples = std::min<uint32_t>(format.m_multisampleCount, 8); samples > 1; samples--) {
            UINT colorLevels = 0;
            UINT depthLevels = 0;
            this->m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, samples, &colorLevels);
            this->m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, samples, &depthLevels);

            if (colorLevels && depthLevels) {
                this->m_samples = samples;
                break;
            }
        }
    }

    const CRect& window = this->DeviceDefWindow();
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferDesc.Width = static_cast<UINT>(window.maxX);
    desc.BufferDesc.Height = static_cast<UINT>(window.maxY);
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = format.m_window ? 0 : format.m_refreshRate;
    desc.BufferDesc.RefreshRate.Denominator = format.m_window ? 0 : 1;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = format.m_vsync && format.m_backBufferCount > 1 ? format.m_backBufferCount : 2;
    desc.OutputWindow = reinterpret_cast<HWND>(this->m_hwnd);
    // Full screen is the SDL window's; the swap chain stays windowed.
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    hr = this->m_factory->CreateSwapChain(this->m_device, &desc, &this->m_swapChain);

    if (FAILED(hr) || !this->m_swapChain) {
        CGxDevice::Log("CGxDeviceD3d11::ICreateD3dDevice(): CreateSwapChain failed (%08x)", static_cast<uint32_t>(hr));
        this->m_swapChain = nullptr;
        return 0;
    }

    if (!this->ICreateBackBuffers()) {
        CGxDevice::Log("CGxDeviceD3d11::ICreateD3dDevice(): back buffers failed");
        return 0;
    }

    // Constant buffers: the Direct3D 9 registers of each stage, the fixed-function state, the blit rectangle.
    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = 256 * 16;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    this->m_device->CreateBuffer(&cb, nullptr, &this->m_constants[0]);
    this->m_device->CreateBuffer(&cb, nullptr, &this->m_constants[1]);
    cb.ByteWidth = sizeof(FfpConstants);
    this->m_device->CreateBuffer(&cb, nullptr, &this->m_ffpConstants);
    cb.ByteWidth = 16;
    this->m_device->CreateBuffer(&cb, nullptr, &this->m_blitConstants);

    D3D11_BUFFER_DESC zero = {};
    zero.ByteWidth = 64;
    zero.Usage = D3D11_USAGE_IMMUTABLE;
    zero.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    static const uint8_t zeroes[64] = {};
    D3D11_SUBRESOURCE_DATA zeroData = { zeroes, 0, 0 };
    this->m_device->CreateBuffer(&zero, &zeroData, &this->m_zeroBuffer);

    this->m_device->CreateVertexShader(g_ffp_vs, sizeof(g_ffp_vs), nullptr, &this->m_ffpVs);
    this->m_device->CreatePixelShader(g_ffp_ps, sizeof(g_ffp_ps), nullptr, &this->m_ffpPs);
    this->m_device->CreateVertexShader(g_blit_vs, sizeof(g_blit_vs), nullptr, &this->m_blitVs);
    this->m_device->CreatePixelShader(g_blit_ps, sizeof(g_blit_ps), nullptr, &this->m_blitPs);
    this->m_device->CreatePixelShader(g_depthcopy_ps, sizeof(g_depthcopy_ps), nullptr, &this->m_depthCopyPs);
    this->m_ffpVsInfo = Shader();
    this->m_ffpVsInfo.m_object = this->m_ffpVs;
    this->m_ffpVsInfo.m_code.assign(g_ffp_vs, g_ffp_vs + sizeof(g_ffp_vs));
    ParseDxbc(g_ffp_vs, sizeof(g_ffp_vs), this->m_ffpVsInfo);

    if (!this->m_ffpVs || !this->m_ffpPs || !this->m_constants[0] || !this->m_constants[1] || !this->m_ffpConstants) {
        CGxDevice::Log("CGxDeviceD3d11::ICreateD3dDevice(): device objects failed");
        return 0;
    }

    this->m_context = 1;
    this->ISetCaps(format);
    this->Log(this->m_caps);
    CGxDevice::Log("\tD3D11 feature level %x, %u samples", static_cast<uint32_t>(level), this->m_samples);

    this->IStateSetDefaults();
    this->ICursorCreate(format);

    this->ShaderCreate(CGxDevice::s_uiVertexShader, GxSh_Vertex, "Shaders\\Vertex", "UI", 2);
    this->ShaderCreate(&CGxDevice::s_uiPixelShader, GxSh_Pixel, "Shaders\\Pixel", "UI", 1);

    return 1;
}

void CGxDeviceD3d11::ISetCaps(const CGxFormat& format) {
    CGxCaps& caps = this->m_caps;
    caps.m_numTmus = 8;
    // Direct3D 10 and later put pixel centres half a pixel in, as OpenGL does.
    caps.m_pixelCenterOnEdge = 1;
    caps.m_texelCenterOnEdge = 1;

    for (uint32_t i = 0; i < GxTexTargets_Last; i++) {
        caps.m_texMaxSize[i] = 16384;
    }

    caps.m_maxIndex = 0xFFFFFF;
    caps.m_texFilterTrilinear = 1;
    caps.m_texFilterAnisotropic = 1;
    caps.m_maxTexAnisotropy = 16;
    caps.m_depthBias = 1;
    caps.m_numStreams = 16;
    caps.m_streamOffset = 1;

    for (uint32_t i = 0; i < GxShTargets_Last; i++) {
        caps.m_shaderTargets[i] = 0;
        caps.m_shaderConstants[i] = 0;
    }

    // The client sees shader model 3 (its paths are written for it); the shaders load from the shader model 4
    // folders (see ShaderCreate).
    caps.m_shaderTargets[GxSh_Pixel] = 4;
    caps.m_shaderTargets[GxSh_Vertex] = 3;
    caps.m_shaderConstants[GxSh_Vertex] = 256;

    if (format.m_fixedFunction[GxSh_Pixel] != -1 && format.m_fixedFunction[GxSh_Pixel] <= caps.m_shaderTargets[GxSh_Pixel]) {
        caps.m_shaderTargets[GxSh_Pixel] = format.m_fixedFunction[GxSh_Pixel] >= 4 ? 4 : 0;
    }

    if (format.m_fixedFunction[GxSh_Vertex] != -1 && format.m_fixedFunction[GxSh_Vertex] <= caps.m_shaderTargets[GxSh_Vertex]) {
        caps.m_shaderTargets[GxSh_Vertex] = format.m_fixedFunction[GxSh_Vertex] >= 3 ? 3 : 0;
    }

    if (!caps.m_shaderTargets[GxSh_Vertex]) {
        caps.m_shaderTargets[GxSh_Pixel] = 0;
    }

    for (uint32_t i = 0; i < GxTexFormats_Last; i++) {
        caps.m_texFmt[i] = i != 0;
        caps.m_rttFormat[i] = 0;
    }

    caps.m_rttFormat[GxTex_Argb8888] = 1;
    caps.m_rttFormat[GxTex_Rgb565] = 1;
    caps.m_rttFormat[GxTex_Gr1616F] = 1;
    caps.m_rttFormat[GxTex_R32F] = 1;
    caps.m_rttFormat[GxTex_D24X8] = 1;
    caps.m_rttFormat[GxTex_Intz] = 1;
    caps.m_generateMipMaps = 1;
    caps.m_colorWrite = 1;
    caps.m_hwCursor = 1;
    caps.m_maxClipPlanes = 6;
    caps.m_texTarget[GxTex_2d] = 1;
    caps.m_texTarget[GxTex_CubeMap] = 1;
    caps.m_texTarget[GxTex_Rectangle] = 0;
    caps.m_texTarget[GxTex_NonPow2] = 1;
    caps.m_texNonPow2 = 1;
    caps.m_occlusionQuery = 1;
    // No point sprites in Direct3D 11: the client draws its quads instead.
    caps.m_pointParameters = 0;
    caps.m_pointScaleMax = 1.0f;
    caps.m_pointSprite = 0;
    caps.m_blendFactor = 1;
    // Alpha test is a device state, as on Direct3D 9: every pixel shader tests against constant buffer 1.
    caps.m_shaderAlphaRef = 1;

    for (uint32_t i = 0; i < 5; i++) {
        caps.unused_0114[i] = 0;
    }

    caps.unk_0134 = caps.m_shaderTargets[GxSh_Pixel] != 4;
    caps.unk_0138 = caps.m_shaderTargets[GxSh_Pixel] != 4;
}

void CGxDeviceD3d11::IStateSetDefaults() {
    this->m_ctx->ClearState();
    this->m_hwBlend = ~0ull;
    this->m_hwDepth = ~0ull;
    this->m_hwRaster = ~0ull;
    this->m_hwVs = nullptr;
    this->m_hwPs = nullptr;
    this->m_stateDirty = true;
    this->m_texturesDirty = true;
    this->m_ffpDirty = true;
    this->m_vertexDirty = true;

    for (uint32_t i = 0; i < 8; i++) {
        this->m_texMatrix[i] = C44Matrix();
        this->m_texTransform[i] = 0;
    }

    this->IRsForceUpdate();
    this->IRsSync(0);
    this->m_primVertexDirty = 0xFFFFFFFF;
    this->m_primIndexDirty = 1;
    this->InitLights();
    this->m_ambientOnlyMode = 0;
    this->ShaderConstantsClear();
    this->IBindTargets();
}

void CGxDeviceD3d11::IReleaseResources(int32_t freeTextures) {
    this->ITexForceRecreation(freeTextures);

    if (freeTextures) {
        this->IReleaseShaders();

        for (CGxPool* pool = this->m_poolList.Head(); pool; pool = this->m_poolList.Next(pool)) {
            this->IReleasePool(pool);
        }

        for (CGxQuery* query = this->m_queryList.Head(); query; query = this->m_queryList.Next(query)) {
            if (query->m_apiSpecific) {
                static_cast<ID3D11Query*>(query->m_apiSpecific)->Release();
                query->m_apiSpecific = nullptr;
            }
        }
    }
}

void CGxDeviceD3d11::IDestroyD3d() {
    for (uint32_t i = 0; i < 2; i++) {
        if (CGxDevice::s_uiVertexShader[i]) {
            this->ShaderDestroy(CGxDevice::s_uiVertexShader[i]);
        }
    }

    if (CGxDevice::s_uiPixelShader) {
        this->ShaderDestroy(CGxDevice::s_uiPixelShader);
    }

    this->ICursorDestroy();

    if (this->m_ctx) {
        this->m_ctx->ClearState();
        this->m_ctx->Flush();
    }

    this->IReleaseResources(1);

    for (auto& entry : this->m_blendStates) entry.second->Release();
    for (auto& entry : this->m_depthStates) entry.second->Release();
    for (auto& entry : this->m_rasterStates) entry.second->Release();
    for (auto& entry : this->m_samplers) entry.second->Release();
    for (auto& entry : this->m_inputLayouts) entry.second->Release();
    this->m_blendStates.clear();
    this->m_depthStates.clear();
    this->m_rasterStates.clear();
    this->m_samplers.clear();
    this->m_inputLayouts.clear();

    SafeRelease(this->m_constants[0]);
    SafeRelease(this->m_constants[1]);
    SafeRelease(this->m_ffpConstants);
    SafeRelease(this->m_blitConstants);
    SafeRelease(this->m_zeroBuffer);
    SafeRelease(this->m_fanIndices);
    this->m_fanIndicesSize = 0;
    SafeRelease(this->m_ffpVs);
    SafeRelease(this->m_ffpPs);
    SafeRelease(this->m_blitVs);
    SafeRelease(this->m_blitPs);
    SafeRelease(this->m_stretchSampler);
    SafeRelease(this->m_depthCopyPs);
    this->m_ffpVsInfo = Shader();
    for (RtDepth& rtDepth : this->m_rtDepths) {
        SafeRelease(rtDepth.m_dsv);
        SafeRelease(rtDepth.m_texture);
        rtDepth = RtDepth();
    }

    this->m_rtDsv = nullptr;
    this->IReleaseBackBuffers();
    SafeRelease(this->m_swapChain);
    SafeRelease(this->m_ctx1);
    SafeRelease(this->m_device1);
    SafeRelease(this->m_ctx);
    SafeRelease(this->m_device);
    this->m_sampledDepth = nullptr;
    this->m_boundVs = nullptr;
    this->m_boundPs = nullptr;

    for (uint32_t i = 0; i < 16; i++) {
        this->m_boundTextures[i] = nullptr;
    }

    this->m_context = 0;
}

int32_t CGxDeviceD3d11::DeviceSetFormat(const CGxFormat& format) {
    CGxDevice::Log("CGxDeviceD3d11::DeviceSetFormat():");
    this->Log(format);

    if (OsGuiGetWindow(0) == this->m_hwnd) {
        OsGuiSetNativeWindowEventProc(nullptr, nullptr);
    }

    this->IDestroyD3d();
    this->m_context = 0;
    this->m_windowVisible = 0;

    if (this->m_ownhwnd) {
        DetachWindow(*this);
    }

    this->m_format = format;
    CGxFormat actual = format;
    bool windowReady = false;

    if (this->m_ownhwnd) {
        windowReady = this->ICreateWindow(actual) != 0;
    } else if (this->m_hwnd) {
        SetClientRect(*this, &actual);
        OsGuiSetGxWindow(this->m_hwnd);
        windowReady = true;
    }

    if (windowReady && this->ICreateD3d() && this->ICreateD3dDevice(actual) && CGxDevice::DeviceSetFormat(actual)) {
        OsGuiSetNativeWindowEventProc(WindowEvent, this);

        if (actual.m_maximize != 2) {
            SDL_ShowWindow(static_cast<SDL_Window*>(this->m_hwnd));
        }

        this->m_windowVisible = actual.m_maximize != 2;
        this->ISetWindowFocus((SDL_GetWindowFlags(static_cast<SDL_Window*>(this->m_hwnd)) & SDL_WINDOW_INPUT_FOCUS) != 0);
        return 1;
    }

    CGxDevice::Log("CGxDeviceD3d11::DeviceSetFormat(): unable to set format!");
    this->IDestroyD3d();
    this->m_context = 0;
    DetachWindow(*this);
    return 0;
}

void CGxDeviceD3d11::DeviceDestroy() {
    CGxDevice::DeviceDestroy();

    if (OsGuiGetWindow(0) == this->m_hwnd) {
        OsGuiSetNativeWindowEventProc(nullptr, nullptr);
    }

    this->IDestroyD3d();
    this->m_windowVisible = 0;
    this->m_windowFocus = 0;
    DetachWindow(*this);
    SafeRelease(this->m_factory);
}

int32_t CGxDeviceD3d11::DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) {
    this->m_ownhwnd = 1;

    if (!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO)) {
        CGxDevice::Log("SDL video must be initialized before creating the graphics device");
        return 0;
    }

    if (this->ICreateD3d() && CGxDevice::DeviceCreate(windowProc, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

int32_t CGxDeviceD3d11::DeviceCreate(void* window, const CGxFormat& format) {
    this->m_ownhwnd = 0;
    this->m_hwnd = window;

    if (!window) {
        return 0;
    }

    CGxDevice::DeviceCreate(window, format);
    SetClientRect(*this);
    OsGuiSetGxWindow(window);

    if (this->ICreateD3d() && this->ICreateD3dDevice(format)) {
        OsGuiSetNativeWindowEventProc(WindowEvent, this);
        this->m_windowVisible = !(SDL_GetWindowFlags(static_cast<SDL_Window*>(window)) & SDL_WINDOW_HIDDEN);
        this->ISetWindowFocus((SDL_GetWindowFlags(static_cast<SDL_Window*>(window)) & SDL_WINDOW_INPUT_FOCUS) != 0);
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

void CGxDeviceD3d11::DeviceSetGamma(float gamma) {
    CGxDevice::DeviceSetGamma(gamma);

    if (this->m_hwnd && !this->IDevIsWindowed()) {
        SDL_SetWindowGammaRamp(static_cast<SDL_Window*>(this->m_hwnd), this->m_gammaRamp.m_red, this->m_gammaRamp.m_green, this->m_gammaRamp.m_blue);
    }
}

void CGxDeviceD3d11::DeviceSetGamma(const CGxGammaRamp& ramp) {
    CGxDevice::DeviceSetGamma(ramp);

    if (this->m_hwnd && !this->IDevIsWindowed()) {
        SDL_SetWindowGammaRamp(static_cast<SDL_Window*>(this->m_hwnd), this->m_gammaRamp.m_red, this->m_gammaRamp.m_green, this->m_gammaRamp.m_blue);
    }
}

void CGxDeviceD3d11::DeviceSetBaseMipLevel(uint32_t level) {
    this->ITexForceRecreation(1);
    CGxDevice::DeviceSetBaseMipLevel(level);
}

void CGxDeviceD3d11::DeviceOverride(EGxOverride override, uint32_t value) {
    CGxDevice::DeviceOverride(override, value);

    if (override == GxOverride_PixelShader) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = value >= 4 ? 4 : 0;
    } else if (override == GxOverride_Unk7) {
        this->m_caps.m_texNonPow2 = value != 0;
    }
}

void CGxDeviceD3d11::LogCrashInfo(char* buffer, uint32_t size) {
    if (!this->m_factory) {
        return;
    }

    IDXGIAdapter1* adapter = nullptr;

    if (SUCCEEDED(this->m_factory->EnumAdapters1(0, &adapter)) && adapter) {
        DXGI_ADAPTER_DESC1 desc;

        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            char name[128];

            for (uint32_t i = 0; i < 127; i++) {
                name[i] = static_cast<char>(desc.Description[i]);

                if (!name[i]) {
                    break;
                }
            }

            name[127] = 0;
            char line[260];
            SStrPrintf(line, sizeof(line), "Adapter (D3D11): %s, vendor %04x device %04x\r\n", name, desc.VendorId, desc.DeviceId);
            SStrPack(buffer, line, size);
        }

        adapter->Release();
    }
}

void CGxDeviceD3d11::StereoSetConvergence(float convergence) {
}

float CGxDeviceD3d11::StereoGetConvergence() {
    return 1.0f;
}

void CGxDeviceD3d11::StereoSetSeparation(float separation) {
}

float CGxDeviceD3d11::StereoGetSeparation() {
    return 0.0f;
}

bool CGxDeviceD3d11::StereoEnabled() {
    return false;
}

void CGxDeviceD3d11::DeviceUnk81(uint32_t a, uint32_t b) {
}

void CGxDeviceD3d11::DeviceUnk82() {
}

void CGxDeviceD3d11::DeviceUnk83(uint32_t a, uint32_t b) {
}

// Vertex and index pools. Each keeps a copy of what it holds: locks hand out pointers into it and unlocks copy the
// locked range to the buffer, so locks never wait for the GPU, and triangle fans can be rebuilt from the indices.

CGxDeviceD3d11::Pool* CGxDeviceD3d11::IPool(CGxPool* pool) {
    if (pool->m_apiSpecific || !this->m_device || pool->m_size <= 0) {
        return static_cast<Pool*>(pool->m_apiSpecific);
    }

    Pool* api = new Pool();
    api->m_dynamic = pool->m_usage == GxPoolUsage_Stream || pool->m_usage == GxPoolUsage_Dynamic;
    api->m_mirror.resize(pool->m_size);

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = (static_cast<uint32_t>(pool->m_size) + 15) & ~15u;
    desc.Usage = api->m_dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    desc.BindFlags = pool->m_target == GxPoolTarget_Index ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = api->m_dynamic ? D3D11_CPU_ACCESS_WRITE : 0;

    if (FAILED(this->m_device->CreateBuffer(&desc, nullptr, &api->m_buffer))) {
        delete api;
        return nullptr;
    }

    pool->m_apiSpecific = api;
    return api;
}

void CGxDeviceD3d11::IReleasePool(CGxPool* pool) {
    if (pool->m_usage == GxPoolUsage_Stream) {
        pool->m_offset = 0;
    }

    pool->Invalidate();
    Pool* api = static_cast<Pool*>(pool->m_apiSpecific);

    if (api) {
        SafeRelease(api->m_buffer);
        delete api;
        pool->m_apiSpecific = nullptr;
    }
}

void CGxDeviceD3d11::IUploadPool(CGxPool* pool, uint32_t offset, uint32_t size) {
    Pool* api = static_cast<Pool*>(pool->m_apiSpecific);

    if (!api || !api->m_buffer || !size || offset >= api->m_mirror.size()) {
        return;
    }

    size = std::min<uint32_t>(size, static_cast<uint32_t>(api->m_mirror.size()) - offset);

    if (api->m_dynamic) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        bool discard = api->m_discard;

        if (FAILED(this->m_ctx->Map(api->m_buffer, 0, discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped))) {
            return;
        }

        if (discard && pool->m_usage != GxPoolUsage_Stream) {
            // A fresh buffer: all of it, for the other buffers in the pool.
            memcpy(mapped.pData, api->m_mirror.data(), api->m_mirror.size());
        } else {
            memcpy(static_cast<uint8_t*>(mapped.pData) + offset, api->m_mirror.data() + offset, size);
        }

        this->m_ctx->Unmap(api->m_buffer, 0);
        api->m_discard = false;
    } else {
        D3D11_BOX box = { offset, 0, 0, offset + size, 1, 1 };
        this->m_ctx->UpdateSubresource(api->m_buffer, 0, &box, api->m_mirror.data() + offset, 0, 0);
    }
}

void CGxDeviceD3d11::PoolSizeSet(CGxPool* pool, uint32_t size) {
    this->IReleasePool(pool);
    pool->m_size = size;
    this->IPool(pool);
}

void CGxDeviceD3d11::PoolDestroy(CGxPool* pool) {
    if (pool) {
        this->IReleasePool(pool);
        CGxDevice::PoolDestroy(pool);
    }
}

char* CGxDeviceD3d11::BufLock(CGxBuf* buf) {
    CGxDevice::BufLock(buf);
    CGxPool* pool = buf->m_pool;

    if (!this->m_context) {
        return static_cast<char*>(this->m_emergencyMem[pool->m_target].Lock(buf->m_size));
    }

    if (pool->m_usage == GxPoolUsage_Stream) {
        uint32_t index = (buf->m_itemSize + pool->m_offset - 1) - (buf->m_itemSize + pool->m_offset - 1) % buf->m_itemSize;

        if (index + buf->m_size > static_cast<uint32_t>(pool->m_size)) {
            pool->Discard();
            buf->m_index = 0;
            pool->m_offset = buf->m_size;

            if (Pool* api = this->IPool(pool)) {
                api->m_discard = true;
            }
        } else {
            buf->m_index = index;
            pool->m_offset = index + buf->m_size;
        }
    }

    Pool* api = this->IPool(pool);

    if (!api || buf->m_index + buf->m_size > api->m_mirror.size()) {
        return static_cast<char*>(this->m_emergencyMem[pool->m_target].Lock(buf->m_size));
    }

    return reinterpret_cast<char*>(api->m_mirror.data() + buf->m_index);
}

int32_t CGxDeviceD3d11::BufUnlock(CGxBuf* buf, uint32_t size) {
    CGxDevice::BufUnlock(buf, size);
    CGxPool* pool = buf->m_pool;
    EmergencyMem& emergencyMem = this->m_emergencyMem[pool->m_target];

    if (emergencyMem.m_lock) {
        emergencyMem.Unlock();
        buf->m_valid = 0;
        this->m_intF5C = 1;
        return 1;
    }

    this->IUploadPool(pool, buf->m_index, size ? size : buf->m_size);
    buf->m_valid = pool->m_apiSpecific != nullptr;
    return 1;
}

void CGxDeviceD3d11::BufData(CGxBuf* buf, const void* data, uint32_t size, uint32_t offset) {
    CGxDevice::BufData(buf, data, size, offset);
    char* dst = this->BufLock(buf);

    if (dst) {
        memcpy(dst + offset, data, size);
    }

    this->BufUnlock(buf, 0);
}

// Textures.

void CGxDeviceD3d11::ITexCreate(CGxTex* texId) {
    uint32_t width;
    uint32_t height;
    uint32_t baseMip;
    uint32_t mipCount;
    this->ITexWHDStartEnd(texId, width, height, baseMip, mipCount);

    if (texId->m_format == GxTex_Argb4444 || texId->m_format == GxTex_Argb1555 || texId->m_format == GxTex_Rgb565) {
        texId->m_format = GxTex_Argb8888;
    }

    Texture* tex = new Texture();
    tex->m_cube = texId->m_target == GxTex_CubeMap;
    tex->m_depth = texId->m_format == GxTex_D24X8 || texId->m_format == GxTex_Intz;
    tex->m_dxgiFormat = TextureFormat(texId->m_format);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = std::max(width, 1u);
    desc.Height = std::max(height, 1u);
    uint32_t maxLevels = 1;

    for (uint32_t size = std::max(desc.Width, desc.Height); size > 1; size >>= 1) {
        maxLevels++;
    }

    desc.MipLevels = tex->m_depth ? 1 : std::min(std::max(mipCount - baseMip, 1u), maxLevels);
    desc.ArraySize = tex->m_cube ? 6 : 1;
    desc.Format = static_cast<DXGI_FORMAT>(tex->m_dxgiFormat);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = tex->m_cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;

    if (tex->m_depth) {
        desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
    } else if (texId->m_flags.m_renderTarget || (texId->m_flags.m_generateMipMaps && !IsBlockFormat(texId->m_format))) {
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET;

        if (texId->m_flags.m_generateMipMaps) {
            desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
        }
    }

    // Block-compressed textures are at least one block wide and high at their top level.
    if (IsBlockFormat(texId->m_format)) {
        desc.Width = std::max<UINT>(desc.Width, 1);
        desc.Height = std::max<UINT>(desc.Height, 1);
    }

    if (FAILED(this->m_device->CreateTexture2D(&desc, nullptr, &tex->m_texture))) {
        CGxDevice::Log("CGxDeviceD3d11::ITexCreate(): %ux%u format %d failed", desc.Width, desc.Height, texId->m_format);
        delete tex;
        return;
    }

    tex->m_levels = desc.MipLevels;

    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = tex->m_depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : desc.Format;

    if (tex->m_cube) {
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MipLevels = desc.MipLevels;
    } else {
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = desc.MipLevels;
    }

    this->m_device->CreateShaderResourceView(tex->m_texture, &srv, &tex->m_srv);

    if (tex->m_depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        this->m_device->CreateDepthStencilView(tex->m_texture, &dsv, &tex->m_dsv);
    }

    texId->m_apiSpecificData = tex;
    texId->m_needsCreation = 0;
}

ID3D11RenderTargetView* CGxDeviceD3d11::ITexRtv(CGxTex* texId, uint32_t plane) {
    Texture* tex = static_cast<Texture*>(texId->m_apiSpecificData);

    if (!tex || tex->m_depth || plane >= 6) {
        return nullptr;
    }

    if (!tex->m_rtv[plane]) {
        D3D11_RENDER_TARGET_VIEW_DESC desc = {};
        desc.Format = static_cast<DXGI_FORMAT>(tex->m_dxgiFormat);

        if (tex->m_cube) {
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.FirstArraySlice = plane;
            desc.Texture2DArray.ArraySize = 1;
        } else {
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        }

        this->m_device->CreateRenderTargetView(tex->m_texture, &desc, &tex->m_rtv[plane]);
    }

    return tex->m_rtv[plane];
}

void CGxDeviceD3d11::ITexUpload(CGxTex* texId) {
    uint32_t texelStrideInBytes;
    const void* texels = nullptr;
    texId->m_userFunc(GxTex_Lock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);

    uint32_t width;
    uint32_t height;
    uint32_t baseMip;
    uint32_t mipCount;
    this->ITexWHDStartEnd(texId, width, height, baseMip, mipCount);

    Texture* tex = static_cast<Texture*>(texId->m_apiSpecificData);
    bool compressed = texId->m_dataFormat >= GxTex_Dxt1 && texId->m_dataFormat <= GxTex_Dxt5;
    uint32_t faceCount = texId->m_target == GxTex_CubeMap ? 6 : 1;
    std::vector<uint8_t> staging;

    for (uint32_t face = 0; face < faceCount; face++) {
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;
        uint32_t level = 0;

        for (uint32_t mip = baseMip; mip != mipCount && level < tex->m_levels; mip++, level++) {
            texels = nullptr;
            texId->m_userFunc(GxTex_Latch, mipWidth, mipHeight, face, mip, texId->m_userArg, texelStrideInBytes, texels);

            if (texels && !texId->m_flags.m_renderTarget && !tex->m_depth) {
                int32_t left = texId->m_updateRect.minX >> mip;
                int32_t top = texId->m_updateRect.minY >> mip;
                int32_t bottom = std::max(top + 1, texId->m_updateRect.maxY >> mip);
                int32_t right = std::max(left + 1, texId->m_updateRect.maxX >> mip);

                if (IsBlockFormat(texId->m_format)) {
                    left &= ~3;
                    top &= ~3;
                    right = (right + 3) & ~3;
                    bottom = (bottom + 3) & ~3;
                }

                right = std::min<int32_t>(right, IsBlockFormat(texId->m_format) ? std::max<int32_t>((mipWidth + 3) & ~3u, 4) : mipWidth);
                bottom = std::min<int32_t>(bottom, IsBlockFormat(texId->m_format) ? std::max<int32_t>((mipHeight + 3) & ~3u, 4) : mipHeight);

                const uint8_t* src = static_cast<const uint8_t*>(texels);

                if (!texId->m_flags.m_bit15) {
                    if (compressed) {
                        src += s_texFormatBytesPerBlock[texId->m_dataFormat] * (left >> 2) + texelStrideInBytes * (top >> 2);
                    } else {
                        src += ((left * s_texFormatBitDepth[texId->m_dataFormat]) >> 3) + texelStrideInBytes * top;
                    }
                }

                C2iVector size;
                size.x = right - left;
                size.y = bottom - top;
                uint32_t pitch;
                uint32_t rows;

                if (IsBlockFormat(texId->m_format)) {
                    pitch = std::max(1, (size.x + 3) / 4) * (texId->m_format == GxTex_Dxt1 ? 8 : 16);
                    rows = std::max(1, (size.y + 3) / 4);
                } else {
                    pitch = size.x * BytesPerTexel(texId->m_format);
                    rows = size.y;
                }

                staging.resize(static_cast<size_t>(pitch) * rows);

                if (size.x > 0 && size.y > 0 && ConvertTexels(size, src, texelStrideInBytes, texId->m_dataFormat, staging.data(), pitch, texId->m_format)) {
                    // A block-compressed mip smaller than a block still updates whole blocks.
                    uint32_t boxRight = static_cast<uint32_t>(right);
                    uint32_t boxBottom = static_cast<uint32_t>(bottom);

                    if (IsBlockFormat(texId->m_format)) {
                        uint32_t levelWidth = std::max(1u, static_cast<uint32_t>(width >> level));
                        uint32_t levelHeight = std::max(1u, static_cast<uint32_t>(height >> level));
                        boxRight = std::min(boxRight, (levelWidth + 3) & ~3u);
                        boxBottom = std::min(boxBottom, (levelHeight + 3) & ~3u);
                    }

                    D3D11_BOX box = { static_cast<UINT>(left), static_cast<UINT>(top), 0, boxRight, boxBottom, 1 };
                    UINT subresource = D3D11CalcSubresource(level, face, tex->m_levels);
                    this->m_ctx->UpdateSubresource(tex->m_texture, subresource, &box, staging.data(), pitch, 0);
                }
            }

            mipHeight = std::max(1u, mipHeight >> 1);
            mipWidth = std::max(1u, mipWidth >> 1);
        }
    }

    texels = nullptr;
    texId->m_userFunc(GxTex_Unlock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);

    if (texId->m_flags.m_generateMipMaps && tex->m_srv && tex->m_levels > 1 && !IsBlockFormat(texId->m_format)) {
        this->m_ctx->GenerateMips(tex->m_srv);
    }
}

void CGxDeviceD3d11::ITexMarkAsUpdated(CGxTex* texId) {
    if (!texId->m_needsUpdate || !this->m_context) {
        return;
    }

    if (texId->m_needsCreation || !texId->m_apiSpecificData) {
        this->ITexCreate(texId);
    }

    if (!texId->m_needsCreation && texId->m_apiSpecificData) {
        if (texId->m_userFunc) {
            this->ITexUpload(texId);
        }

        CGxDevice::ITexMarkAsUpdated(texId);
    }
}

static void ReleaseTexture(CGxTex* texId) {
    auto* tex = static_cast<CGxDeviceD3d11::Texture*>(texId->m_apiSpecificData);

    if (tex) {
        SafeRelease(tex->m_srv);

        for (uint32_t i = 0; i < 6; i++) {
            SafeRelease(tex->m_rtv[i]);
        }

        SafeRelease(tex->m_dsv);
        SafeRelease(tex->m_texture);
        SafeRelease(tex->m_volume);
        delete tex;
    }

    texId->m_apiSpecificData = nullptr;
}

void CGxDeviceD3d11::ITexForceRecreation(int32_t all) {
    for (CGxTex* texId = this->m_textures.Head(); texId; texId = this->m_textures.Next(texId)) {
        if (!texId->m_apiSpecificData || !(all || texId->m_flags.m_renderTarget)) {
            continue;
        }

        static CiRect s_updateRect;
        ReleaseTexture(texId);
        texId->m_needsCreation = 1;
        this->TexMarkForUpdate(texId, s_updateRect, 0);

        uint32_t texelStrideInBytes;
        const void* texels;
        texId->m_userFunc(GxTex_3, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);
    }

    for (uint32_t i = 0; i < 16; i++) {
        this->m_boundTextures[i] = nullptr;
    }

    this->m_texturesDirty = true;

    if (this->m_hwnd && this->m_device) {
        this->NotifyOnTextureRecreation();
    }
}

void CGxDeviceD3d11::TexDestroy(CGxTex* texId) {
    for (uint32_t i = 0; i < 16; i++) {
        if (this->m_boundTextures[i] == texId) {
            this->m_boundTextures[i] = nullptr;
            this->m_texturesDirty = true;
        }
    }

    for (uint32_t i = 0; i < GxBuffers_Last; i++) {
        if (this->m_textureTarget[i].m_texture == texId) {
            this->m_textureTarget[i].m_texture = nullptr;
            this->m_textureTarget[i].m_apiSpecific = nullptr;
            this->IBindTargets();
        }
    }

    if (this->m_sampledDepth == texId) {
        this->m_sampledDepth = nullptr;
        this->IBindTargets();
    }

    if (this->m_ctx && texId->m_apiSpecificData) {
        // Unbind it everywhere before it goes.
        ID3D11ShaderResourceView* none[24] = {};
        this->m_ctx->PSSetShaderResources(0, 24, none);
        this->m_ctx->VSSetShaderResources(0, 16, none);
        this->m_texturesDirty = true;
    }

    ReleaseTexture(texId);
    CGxDevice::TexDestroy(texId);
}

// Render targets.

void CGxDeviceD3d11::ICreateRtDepth(uint32_t width, uint32_t height) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    this->m_rtDepthClock++;
    RtDepth* oldest = &this->m_rtDepths[0];

    for (RtDepth& rtDepth : this->m_rtDepths) {
        if (rtDepth.m_dsv && rtDepth.m_width == width && rtDepth.m_height == height) {
            rtDepth.m_used = this->m_rtDepthClock;
            this->m_rtDsv = rtDepth.m_dsv;
            return;
        }

        if (rtDepth.m_used < oldest->m_used) {
            oldest = &rtDepth;
        }
    }

    // A new size: replaces the one used longest ago.
    SafeRelease(oldest->m_dsv);
    SafeRelease(oldest->m_texture);
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    if (SUCCEEDED(this->m_device->CreateTexture2D(&desc, nullptr, &oldest->m_texture))) {
        this->m_device->CreateDepthStencilView(oldest->m_texture, nullptr, &oldest->m_dsv);
    }

    oldest->m_width = width;
    oldest->m_height = height;
    oldest->m_used = this->m_rtDepthClock;
    this->m_rtDsv = oldest->m_dsv;
}

void CGxDeviceD3d11::IBindTargets() {
    if (!this->m_ctx) {
        return;
    }

    ID3D11RenderTargetView* rtv = this->m_backRtv;
    ID3D11DepthStencilView* dsv = this->m_backDsv;
    CGxTex* color = this->m_textureTarget[GxBuffers_Color].m_texture;
    CGxTex* depth = this->m_textureTarget[GxBuffers_Depth].m_texture;
    bool colorTexture = false;

    // The sampled depth texture stands in for the back buffer's (single-sampled back buffers only).
    if (this->m_sampledDepth && this->m_samples == 1) {
        Texture* tex = static_cast<Texture*>(this->m_sampledDepth->m_apiSpecificData);

        if (tex && tex->m_dsv) {
            D3D11_TEXTURE2D_DESC desc;
            tex->m_texture->GetDesc(&desc);

            if (desc.Width == this->m_backBufferWidth && desc.Height == this->m_backBufferHeight) {
                dsv = tex->m_dsv;
            }
        }
    }

    if (color && color->m_apiSpecificData) {
        if (ID3D11RenderTargetView* view = this->ITexRtv(color, this->m_textureTarget[GxBuffers_Color].m_plane)) {
            rtv = view;
            colorTexture = true;
        }
    } else if (color == nullptr && depth) {
        // A depth-only pass (shadow maps): no colour target.
        rtv = nullptr;
    }

    if (depth && depth->m_apiSpecificData && static_cast<Texture*>(depth->m_apiSpecificData)->m_dsv) {
        dsv = static_cast<Texture*>(depth->m_apiSpecificData)->m_dsv;
    } else if (colorTexture) {
        this->ICreateRtDepth(color->m_width, color->m_height);
        dsv = this->m_rtDsv;
    }

    // Anything about to be drawn into must not stay bound for reading.
    ID3D11ShaderResourceView* none[24] = {};
    this->m_ctx->PSSetShaderResources(0, 24, none);
    this->m_texturesDirty = true;

    this->m_ctx->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, dsv);
    this->m_curRtv = rtv;
    this->m_curDsv = dsv;
    this->m_targetIsTexture = colorTexture || depth != nullptr;
    this->m_viewportDirty = 1;
    this->m_scissorRectDirty = 1;
}

void CGxDeviceD3d11::DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane) {
    if (!this->m_context) {
        return;
    }

    CGxDevice::TextureTarget& target = this->m_textureTarget[buffer];

    if (target.m_texture == texture && target.m_plane == plane) {
        return;
    }

    CGxDevice::DeviceSetRenderTarget(buffer, texture, plane);
    target.m_apiSpecific = nullptr;

    if (texture) {
        if (texture->m_needsCreation || !texture->m_apiSpecificData) {
            this->ITexCreate(texture);
        }

        // The Direct3D 9 device marks a texture target by its surface; anything non-null will do.
        target.m_apiSpecific = texture->m_apiSpecificData;
    }

    this->IBindTargets();
    GxXformSetViewport(this->m_viewport.m_x.m_l, this->m_viewport.m_x.m_h, this->m_viewport.m_y.m_l, this->m_viewport.m_y.m_h, this->m_viewport.m_z.m_l, this->m_viewport.m_z.m_h);
}

void CGxDeviceD3d11::DeviceSaveDepthSurface() {
    this->m_savedDsv = this->m_curDsv;
}

void CGxDeviceD3d11::DeviceRestoreDepthSurface() {
    if (!this->m_context) {
        return;
    }

    this->m_ctx->OMSetRenderTargets(this->m_curRtv ? 1 : 0, this->m_curRtv ? &this->m_curRtv : nullptr, this->m_savedDsv);
    this->m_curDsv = this->m_savedDsv;
}

void CGxDeviceD3d11::DeviceResolveDepthBuffer(CGxTex* texture) {
    if (!this->m_context || !texture) {
        return;
    }

    if (texture->m_needsCreation || !texture->m_apiSpecificData) {
        this->ITexCreate(texture);
    }

    Texture* tex = static_cast<Texture*>(texture->m_apiSpecificData);

    if (tex && tex->m_dsv) {
        this->m_ctx->OMSetRenderTargets(this->m_curRtv ? 1 : 0, this->m_curRtv ? &this->m_curRtv : nullptr, tex->m_dsv);
        this->m_curDsv = tex->m_dsv;
    }
}

void CGxDeviceD3d11::DeviceSetSampledDepth(CGxTex* texture) {
    if (!this->m_context) {
        return;
    }

    if (texture && texture->m_format == GxTex_Intz && (texture->m_needsCreation || !texture->m_apiSpecificData)) {
        this->ITexCreate(texture);
    }

    CGxTex* sampled = texture && texture->m_format == GxTex_Intz && texture->m_apiSpecificData ? texture : nullptr;

    if (sampled == this->m_sampledDepth) {
        return;
    }

    this->m_sampledDepth = sampled;
    this->IBindTargets();
}

int32_t CGxDeviceD3d11::DeviceCanCopySampledDepth() {
    return this->m_context && this->m_depthCopyPs && this->m_blitVs && this->m_backDepthSrv;
}

// Copies the back buffer's depth into texture (an INTZ one), for effects that read the scene depth: drawn into
// its depth through SV_Depth. The back buffer's depth stays bound for the rest of the frame.
int32_t CGxDeviceD3d11::DeviceCopySampledDepth(CGxTex* texture) {
    if (!this->DeviceCanCopySampledDepth() || !texture || texture->m_format != GxTex_Intz) {
        return 0;
    }

    if (texture->m_needsCreation || !texture->m_apiSpecificData) {
        this->ITexCreate(texture);
    }

    Texture* tex = static_cast<Texture*>(texture->m_apiSpecificData);

    if (!tex || !tex->m_dsv) {
        return 0;
    }

    D3D11_TEXTURE2D_DESC desc;
    tex->m_texture->GetDesc(&desc);

    if (desc.Width != this->m_backBufferWidth || desc.Height != this->m_backBufferHeight) {
        return 0;
    }

    float info[4] = { this->m_samples > 1 ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
    D3D11_MAPPED_SUBRESOURCE mapped;

    if (SUCCEEDED(this->m_ctx->Map(this->m_blitConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, info, sizeof(info));
        this->m_ctx->Unmap(this->m_blitConstants, 0);
    }

    // Always passes, always writes.
    static ID3D11DepthStencilState* s_write = nullptr;

    if (!s_write) {
        D3D11_DEPTH_STENCIL_DESC depthDesc = {};
        depthDesc.DepthEnable = TRUE;
        depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        this->m_device->CreateDepthStencilState(&depthDesc, &s_write);
    }

    ID3D11ShaderResourceView* none[24] = {};
    this->m_ctx->PSSetShaderResources(0, 24, none);
    this->m_ctx->OMSetRenderTargets(0, nullptr, tex->m_dsv);

    D3D11_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0.0f, 1.0f };
    this->m_ctx->RSSetViewports(1, &viewport);
    this->m_ctx->RSSetState(nullptr);
    this->m_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    this->m_ctx->OMSetDepthStencilState(s_write, 0);
    this->m_ctx->IASetInputLayout(nullptr);
    this->m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    this->m_ctx->VSSetShader(this->m_blitVs, nullptr, 0);
    this->m_ctx->PSSetShader(this->m_depthCopyPs, nullptr, 0);
    this->m_ctx->PSSetConstantBuffers(0, 1, &this->m_blitConstants);
    ID3D11ShaderResourceView* views[2] = { this->m_samples > 1 ? nullptr : this->m_backDepthSrv, this->m_samples > 1 ? this->m_backDepthSrv : nullptr };
    this->m_ctx->PSSetShaderResources(0, 2, views);
    // The blit vertex shader's rectangle (constant buffer 0) is ignored; its corners cover the target.
    this->m_ctx->VSSetConstantBuffers(0, 1, &this->m_blitConstants);
    this->m_ctx->Draw(3, 0);
    this->m_ctx->PSSetShaderResources(0, 2, none);

    this->IStateInvalidate();
    this->IBindTargets();
    return 1;
}

// After the device drew something of its own: everything is sent again before the next draw.
void CGxDeviceD3d11::IStateInvalidate() {
    this->m_hwBlend = this->m_hwDepth = this->m_hwRaster = ~0ull;
    this->m_stateDirty = true;
    this->m_hwVs = nullptr;
    this->m_hwPs = nullptr;
    this->m_vertexDirty = true;
    this->m_texturesDirty = true;
    this->m_ffpDirty = true;
    this->m_viewportDirty = 1;
    this->m_scissorRectDirty = 1;
}

// The texture's resource becomes a 3D texture (ARGB texels), for shaders that read a volume (4.3.4 magma noise).
int32_t CGxDeviceD3d11::DeviceTexSetVolume(CGxTex* texture, uint32_t width, uint32_t height, uint32_t depth, const void* argb) {
    if (!this->m_context || !texture || !argb || !width || !height || !depth) {
        return 0;
    }

    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Depth = depth;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = { argb, width * 4, width * height * 4 };
    ID3D11Texture3D* volume = nullptr;

    if (FAILED(this->m_device->CreateTexture3D(&desc, &data, &volume))) {
        return 0;
    }

    Texture* tex = new Texture();
    tex->m_volume = volume;
    tex->m_dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    this->m_device->CreateShaderResourceView(volume, nullptr, &tex->m_srv);

    if (texture->m_apiSpecificData) {
        Texture* old = static_cast<Texture*>(texture->m_apiSpecificData);
        SafeRelease(old->m_srv);

        for (uint32_t i = 0; i < 6; i++) {
            SafeRelease(old->m_rtv[i]);
        }

        SafeRelease(old->m_dsv);
        SafeRelease(old->m_texture);
        SafeRelease(old->m_volume);
        delete old;
    }

    texture->m_apiSpecificData = tex;
    texture->m_needsCreation = 0;
    texture->m_needsUpdate = 0;
    this->m_texturesDirty = true;
    return 1;
}

void CGxDeviceD3d11::DeviceSetCullFlip(int32_t flip) {
    if (this->m_cullFlip != (flip != 0)) {
        this->m_cullFlip = flip != 0;
        this->m_stateDirty = true;
    }
}

void CGxDeviceD3d11::DeviceCopyTex(CGxTex* source, uint32_t sourcePlane, CGxTex* dest, uint32_t destPlane) {
    if (!this->m_context || !source || !dest) {
        return;
    }

    if (source->m_needsCreation || !source->m_apiSpecificData) {
        this->ITexCreate(source);
    }

    if (dest->m_needsCreation || !dest->m_apiSpecificData) {
        this->ITexCreate(dest);
    }

    Texture* src = static_cast<Texture*>(source->m_apiSpecificData);
    Texture* dst = static_cast<Texture*>(dest->m_apiSpecificData);

    if (!src || !dst) {
        return;
    }

    if (src->m_dxgiFormat == dst->m_dxgiFormat && source->m_width == dest->m_width && source->m_height == dest->m_height) {
        this->m_ctx->CopySubresourceRegion(dst->m_texture, D3D11CalcSubresource(0, destPlane, dst->m_levels), 0, 0, 0, src->m_texture, D3D11CalcSubresource(0, sourcePlane, src->m_levels), nullptr);
    }
}

// Copies part of the current colour target into dest (the Direct3D 9 device's StretchRect from the back buffer).
int32_t CGxDeviceD3d11::TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane) {
    if (!this->m_context) {
        return 0;
    }

    if (dest->m_needsCreation || !dest->m_apiSpecificData) {
        this->ITexCreate(dest);
    }

    Texture* tex = static_cast<Texture*>(dest->m_apiSpecificData);

    if (!tex || !this->m_backColor) {
        return 0;
    }

    // Multisampled: resolved first (into the kept copy), the rectangle copied from it.
    ID3D11Resource* source = this->m_samples > 1 ? this->IResolveBack() : this->m_backColor;

    if (!source) {
        return 0;
    }

    if (tex->m_dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM) {
        D3D11_BOX box = { static_cast<UINT>(sourcePos.x), static_cast<UINT>(sourcePos.y), 0, static_cast<UINT>(sourcePos.x + size.x), static_cast<UINT>(sourcePos.y + size.y), 1 };
        this->m_ctx->CopySubresourceRegion(tex->m_texture, D3D11CalcSubresource(level, plane, tex->m_levels), destPos.x, destPos.y, 0, source, 0, &box);
    }

    return 1;
}

// The back buffer, resolved or copied into a single-sampled texture made once (not per copy: making a full-screen
// texture for every copy is slow).
ID3D11Texture2D* CGxDeviceD3d11::IResolveBack() {
    if (!this->m_backColor) {
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc;
    this->m_backColor->GetDesc(&desc);

    if (!this->m_resolveColor) {
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;

        if (FAILED(this->m_device->CreateTexture2D(&desc, nullptr, &this->m_resolveColor))) {
            this->m_resolveColor = nullptr;
            return nullptr;
        }

        this->m_device->CreateShaderResourceView(this->m_resolveColor, nullptr, &this->m_resolveSrv);
    }

    if (this->m_samples > 1) {
        this->m_ctx->ResolveSubresource(this->m_resolveColor, 0, this->m_backColor, 0, desc.Format);
    } else {
        this->m_ctx->CopyResource(this->m_resolveColor, this->m_backColor);
    }

    return this->m_resolveColor;
}

// Draws the current colour target, scaled, into dest (the Direct3D 9 device's filtered StretchRect).
int32_t CGxDeviceD3d11::TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face) {
    if (!this->m_context) {
        return 0;
    }

    if (dest->m_needsCreation || !dest->m_apiSpecificData) {
        this->ITexCreate(dest);
    }

    Texture* tex = static_cast<Texture*>(dest->m_apiSpecificData);

    if (!tex || !this->m_backColor || level || !this->m_blitVs || !this->m_blitPs) {
        return 0;
    }

    D3D11_TEXTURE2D_DESC desc;
    this->m_backColor->GetDesc(&desc);

    if (!this->IResolveBack()) {
        return 0;
    }

    ID3D11ShaderResourceView* copyView = this->m_resolveSrv;
    ID3D11RenderTargetView* rtv = this->ITexRtv(dest, face);

    if (copyView && rtv) {
        float source[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

        if (sourceRect) {
            source[0] = sourceRect->minX / static_cast<float>(desc.Width);
            source[1] = sourceRect->minY / static_cast<float>(desc.Height);
            source[2] = sourceRect->maxX / static_cast<float>(desc.Width);
            source[3] = sourceRect->maxY / static_cast<float>(desc.Height);
        }

        D3D11_MAPPED_SUBRESOURCE mapped;

        if (SUCCEEDED(this->m_ctx->Map(this->m_blitConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, source, sizeof(source));
            this->m_ctx->Unmap(this->m_blitConstants, 0);
        }

        D3D11_VIEWPORT viewport;
        viewport.TopLeftX = destRect ? static_cast<float>(destRect->minX) : 0.0f;
        viewport.TopLeftY = destRect ? static_cast<float>(destRect->minY) : 0.0f;
        viewport.Width = destRect ? static_cast<float>(destRect->maxX - destRect->minX) : static_cast<float>(dest->m_width);
        viewport.Height = destRect ? static_cast<float>(destRect->maxY - destRect->minY) : static_cast<float>(dest->m_height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        if (!this->m_stretchSampler) {
            D3D11_SAMPLER_DESC samplerDesc = {};
            samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
            this->m_device->CreateSamplerState(&samplerDesc, &this->m_stretchSampler);
        }

        ID3D11SamplerState* sampler = this->m_stretchSampler;

        this->m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        this->m_ctx->RSSetViewports(1, &viewport);
        this->m_ctx->RSSetState(nullptr);
        this->m_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        this->m_ctx->OMSetDepthStencilState(nullptr, 0);
        this->m_ctx->IASetInputLayout(nullptr);
        this->m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        this->m_ctx->VSSetShader(this->m_blitVs, nullptr, 0);
        this->m_ctx->VSSetConstantBuffers(0, 1, &this->m_blitConstants);
        this->m_ctx->PSSetShader(this->m_blitPs, nullptr, 0);
        this->m_ctx->PSSetShaderResources(0, 1, &copyView);
        this->m_ctx->PSSetSamplers(0, 1, &sampler);
        this->m_ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        this->m_ctx->PSSetShaderResources(0, 1, &none);

        this->IStateInvalidate();
        this->IBindTargets();
    }

    return 1;
}

void CGxDeviceD3d11::DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels) {
    this->ClampRectToWindow(rect);

    uint32_t width = rect.maxX - rect.minX;
    uint32_t height = rect.maxY - rect.minY;
    pixels.SetCount(width * height);

    if (!this->m_context || !this->m_backColor || !width || !height) {
        return;
    }

    D3D11_TEXTURE2D_DESC desc;
    this->m_backColor->GetDesc(&desc);
    ID3D11Resource* source = desc.SampleDesc.Count > 1 ? this->IResolveBack() : this->m_backColor;

    if (!source) {
        return;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Width = width;
    stagingDesc.Height = height;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;

    if (SUCCEEDED(this->m_device->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
        D3D11_BOX box = { static_cast<UINT>(rect.minX), static_cast<UINT>(rect.minY), 0, static_cast<UINT>(rect.maxX), static_cast<UINT>(rect.maxY), 1 };
        this->m_ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped;

        if (SUCCEEDED(this->m_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            for (uint32_t y = 0; y < height; y++) {
                memcpy(pixels.Ptr() + y * width, static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch, width * 4);
            }

            this->m_ctx->Unmap(staging, 0);
        }

        staging->Release();
    }
}

void CGxDeviceD3d11::DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths) {
    this->ClampRectToWindow(rect);
    uint32_t count = (rect.maxY - rect.minY) * (rect.maxX - rect.minX);
    depths.SetCount(count);
    memset(depths.Ptr(), 0, sizeof(float) * count);
}

// Scenes.

void CGxDeviceD3d11::SceneClear(uint32_t mask, CImVector color) {
    if (!this->m_context) {
        return;
    }

    if (this->m_viewportDirty) {
        this->IXformSetViewport();
    }

    if ((mask & GxBufferBit_Color) && this->m_curRtv) {
        float rgba[4] = { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f };
        D3D11_VIEWPORT viewport;
        UINT viewports = 1;
        this->m_ctx->RSGetViewports(&viewports, &viewport);
        const CRect& window = this->DeviceCurWindow();
        bool full = viewports == 0 || (viewport.TopLeftX <= 0.0f && viewport.TopLeftY <= 0.0f && viewport.Width >= window.maxX && viewport.Height >= window.maxY);

        // Direct3D 9 clears the viewport only.
        if (!full && this->m_ctx1) {
            D3D11_RECT clearRect = { static_cast<LONG>(viewport.TopLeftX), static_cast<LONG>(viewport.TopLeftY), static_cast<LONG>(viewport.TopLeftX + viewport.Width), static_cast<LONG>(viewport.TopLeftY + viewport.Height) };
            this->m_ctx1->ClearView(this->m_curRtv, rgba, &clearRect, 1);
        } else {
            this->m_ctx->ClearRenderTargetView(this->m_curRtv, rgba);
        }
    }

    if ((mask & GxBufferBit_Depth) && this->m_curDsv) {
        this->m_ctx->ClearDepthStencilView(this->m_curDsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }
}

void CGxDeviceD3d11::ScenePresent() {
    if (this->m_context) {
        int32_t scrShotClick = this->m_scrShotClick;
        CGxDevice::ScenePresent();
        this->ICursorDraw();

        if (scrShotClick) {
            this->DeviceScreenShot();
        }

        if (this->m_samples > 1 && this->m_backColor) {
            ID3D11Texture2D* swapBuffer = nullptr;

            if (SUCCEEDED(this->m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&swapBuffer)))) {
                this->m_ctx->ResolveSubresource(swapBuffer, 0, this->m_backColor, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
                swapBuffer->Release();
            }
        }

        this->WaitForFPSCap();
        UINT interval = this->m_format.m_vsync ? std::min<uint32_t>(this->m_format.m_vsync, 4) : 0;

        if (this->m_format.m_window && interval > 1) {
            interval = 1;
        }

        HRESULT hr = this->m_swapChain->Present(interval, 0);
        static uint32_t s_presentFailures;

        if (FAILED(hr) && s_presentFailures++ < 8) {
            CGxDevice::Log("CGxDeviceD3d11::ScenePresent(): Present failed (%08x)", static_cast<uint32_t>(hr));
        }

        if (this->m_format.m_fixLag) {
            this->m_ctx->Flush();
        }

        // FLIP model swap chains unbind the back buffer at present.
        this->IBindTargets();
    }

    this->ShaderConstantsClear();
}

// Queries.

void CGxDeviceD3d11::QueryCreate(CGxQuery*& query, EGxQueryType type) {
    CGxDevice::QueryCreate(query, type);

    if (query && !query->m_apiSpecific && this->m_device) {
        D3D11_QUERY_DESC desc = { D3D11_QUERY_OCCLUSION, 0 };
        ID3D11Query* d3dQuery = nullptr;

        if (SUCCEEDED(this->m_device->CreateQuery(&desc, &d3dQuery))) {
            query->m_apiSpecific = d3dQuery;
        }
    }
}

void CGxDeviceD3d11::QueryDestroy(CGxQuery*& query) {
    if (query->m_apiSpecific) {
        static_cast<ID3D11Query*>(query->m_apiSpecific)->Release();
        query->m_apiSpecific = nullptr;
    }

    CGxDevice::QueryDestroy(query);
}

bool CGxDeviceD3d11::QueryBegin(CGxQuery* query) {
    if (!this->m_context) {
        return false;
    }

    if (!query->m_apiSpecific) {
        D3D11_QUERY_DESC desc = { D3D11_QUERY_OCCLUSION, 0 };
        ID3D11Query* d3dQuery = nullptr;

        if (FAILED(this->m_device->CreateQuery(&desc, &d3dQuery))) {
            return false;
        }

        query->m_apiSpecific = d3dQuery;
    }

    this->m_ctx->Begin(static_cast<ID3D11Query*>(query->m_apiSpecific));
    return true;
}

bool CGxDeviceD3d11::QueryEnd(CGxQuery* query) {
    if (!this->m_context || !query->m_apiSpecific) {
        return false;
    }

    this->m_ctx->End(static_cast<ID3D11Query*>(query->m_apiSpecific));
    return true;
}

bool CGxDeviceD3d11::QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data) {
    if (!this->m_context || !query->m_apiSpecific) {
        return false;
    }

    switch (param) {
        case GxQueryParam_Unk0:
            data = sizeof(UINT64);
            return true;

        case GxQueryParam_Unk1:
            data = this->m_ctx->GetData(static_cast<ID3D11Query*>(query->m_apiSpecific), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            return false;

        default:
            return false;
    }
}

bool CGxDeviceD3d11::QueryGetData(CGxQuery* query, uint32_t& data) {
    if (!this->m_context || !query->m_apiSpecific) {
        return false;
    }

    UINT64 samples = 0;
    HRESULT hr;

    do {
        hr = this->m_ctx->GetData(static_cast<ID3D11Query*>(query->m_apiSpecific), &samples, sizeof(samples), 0);
    } while (hr == S_FALSE);

    data = static_cast<uint32_t>(std::min<UINT64>(samples, 0xFFFFFFFFull));
    return hr == S_OK;
}

// Shaders.

void CGxDeviceD3d11::ShaderCreate(CGxShader** shaders, EGxShTarget target, const char* path, const char* name, uint32_t permutations) {
    // The client asks at shader model 3; the files come from the shader model 4 folders (vs_4_0, ps_4_0).
    int32_t vertex = this->m_caps.m_shaderTargets[GxSh_Vertex];
    int32_t pixel = this->m_caps.m_shaderTargets[GxSh_Pixel];

    if (vertex) {
        this->m_caps.m_shaderTargets[GxSh_Vertex] = 4;
    }

    if (pixel) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = 5;
    }

    CGxDevice::ShaderCreate(shaders, target, path, name, permutations);
    this->m_caps.m_shaderTargets[GxSh_Vertex] = vertex;
    this->m_caps.m_shaderTargets[GxSh_Pixel] = pixel;

    // Says once which shaders have no shader model 4 build (their draws fall back to the fixed-function shader).
    // (Shaders\Post ones have no files: PostEffects gives them their code.)
    if ((target == GxSh_Vertex ? vertex : pixel) && permutations && shaders[0] && !shaders[0]->m_code.Count() && SStrCmpI(path, "Shaders\\Post", STORM_MAX_STR)) {
        static char s_logged[64][64];
        static uint32_t s_loggedCount = 0;
        bool seen = false;

        for (uint32_t i = 0; i < s_loggedCount; i++) {
            if (!SStrCmpI(s_logged[i], name, STORM_MAX_STR)) {
                seen = true;
                break;
            }
        }

        if (!seen && s_loggedCount < 64) {
            SStrCopy(s_logged[s_loggedCount++], name, sizeof(s_logged[0]));
            CGxDevice::Log("CGxDeviceD3d11: no shader model 4 %s shader %s\\%s", target == GxSh_Vertex ? "vertex" : "pixel", path, name);
        }
    }

    if (permutations == 1 && shaders[0] && !shaders[0]->m_loaded) {
        this->IShaderCreate(shaders[0]);
    }
}

void CGxDeviceD3d11::IShaderReload(CGxShader* shader, const char* path, const char* name) {
    if (shader->m_refCount == 1) {
        this->IShaderRelease(shader);
    }

    int32_t vertex = this->m_caps.m_shaderTargets[GxSh_Vertex];
    int32_t pixel = this->m_caps.m_shaderTargets[GxSh_Pixel];
    this->m_caps.m_shaderTargets[GxSh_Vertex] = vertex ? 4 : 0;
    this->m_caps.m_shaderTargets[GxSh_Pixel] = pixel ? 5 : 0;
    this->IShaderLoad(&shader, static_cast<EGxShTarget>(shader->m_target), path, name, 1);
    this->m_caps.m_shaderTargets[GxSh_Vertex] = vertex;
    this->m_caps.m_shaderTargets[GxSh_Pixel] = pixel;
    this->IShaderCreate(shader);
}

void CGxDeviceD3d11::IShaderCreate(CGxShader* shader) {
    if (shader->m_target == GxSh_Vertex) {
        this->IShaderCreateVertex(shader);
    } else if (shader->m_target == GxSh_Pixel) {
        this->IShaderCreatePixel(shader);
    }
}

void CGxDeviceD3d11::IShaderCreateVertex(CGxShader* shader) {
    shader->m_valid = 0;

    if (!this->m_context) {
        return;
    }

    shader->m_loaded = 1;
    uint32_t size = shader->m_code.Count();

    // Only DXBC: a Direct3D 9 shader the client built itself (CataWater, PostEffects) stays invalid here.
    if (size < 32 || memcmp(shader->m_code.m_data, "DXBC", 4)) {
        return;
    }

    ID3D11VertexShader* vs = nullptr;

    if (FAILED(this->m_device->CreateVertexShader(shader->m_code.m_data, size, nullptr, &vs))) {
        return;
    }

    Shader* info = new Shader();
    info->m_object = vs;
    info->m_code.assign(shader->m_code.m_data, shader->m_code.m_data + size);
    ParseDxbc(shader->m_code.m_data, size, *info);
    shader->m_apiSpecific = info;
    shader->m_valid = 1;
}

void CGxDeviceD3d11::IShaderCreatePixel(CGxShader* shader) {
    shader->m_valid = 0;

    if (!this->m_context) {
        return;
    }

    shader->m_loaded = 1;
    uint32_t size = shader->m_code.Count();

    if (size < 32 || memcmp(shader->m_code.m_data, "DXBC", 4)) {
        return;
    }

    ID3D11PixelShader* ps = nullptr;

    if (FAILED(this->m_device->CreatePixelShader(shader->m_code.m_data, size, nullptr, &ps))) {
        return;
    }

    Shader* info = new Shader();
    info->m_object = ps;
    ParseDxbc(shader->m_code.m_data, size, *info);
    shader->m_apiSpecific = info;
    shader->m_valid = 1;
}

void CGxDeviceD3d11::IShaderRelease(CGxShader* shader) {
    Shader* info = static_cast<Shader*>(shader->m_apiSpecific);

    if (info) {
        if (info->m_object) {
            static_cast<IUnknown*>(info->m_object)->Release();
        }

        // Input layouts made for this vertex shader go with it.
        for (auto it = this->m_inputLayouts.begin(); it != this->m_inputLayouts.end();) {
            if ((it->first >> 32) == (reinterpret_cast<uintptr_t>(info) & 0xFFFFFFFFull)) {
                it->second->Release();
                it = this->m_inputLayouts.erase(it);
            } else {
                ++it;
            }
        }

        delete info;
        shader->m_apiSpecific = nullptr;
    }

    if (this->m_boundVs == shader) {
        this->m_boundVs = nullptr;
    }

    if (this->m_boundPs == shader) {
        this->m_boundPs = nullptr;
    }

    this->m_hwVs = nullptr;
    this->m_hwPs = nullptr;
    this->m_vertexDirty = true;
    shader->m_loaded = 0;
}

void CGxDeviceD3d11::IReleaseShaders() {
    for (uint32_t target = 0; target < GxShTargets_Last; target++) {
        TSHashTable<CGxShader, HASHKEY_STRI>& shaders = this->m_shaderList[target];

        for (CGxShader* shader = shaders.m_fulllist.Head(); shader; shader = shaders.m_fulllist.Next(shader)) {
            if (shader->m_apiSpecific) {
                this->IShaderRelease(shader);
            }
        }
    }
}

void CGxDeviceD3d11::ShaderDestroy(CGxShader*& shader) {
    if (shader->m_refCount == 1) {
        this->IShaderRelease(shader);
    }

    CGxDevice::ShaderDestroy(shader);
}

// Render states. Direct3D 9 set most of these straight on the device; here they are kept and turned into state
// objects and the fixed-function constants before the next draw.

void CGxDeviceD3d11::XformSetProjection(const C44Matrix& matrix) {
    CGxDevice::XformSetProjection(matrix);

    // Direct3D's depth range (0 to 1), as the Direct3D 9 device converts it.
    C44Matrix projNative = matrix;

    if (!(fabs(projNative.c3 - 1.0f) < 0.00000023841858f) && !(fabs(projNative.c3) < 0.00000023841858f)) {
        projNative *= 1.0f / projNative.c3;
    }

    if (projNative.d3 == 0.0f) {
        float minZ = -(projNative.d2 / (projNative.c2 + 1.0f));
        float maxZ = -(projNative.d2 / (projNative.c2 - 1.0f));
        projNative.c2 = maxZ / (maxZ - minZ);
        projNative.d2 = maxZ * minZ / (minZ - maxZ);
    } else {
        float invC2 = 1.0f / projNative.c2;
        float minZ = (-1.0f - projNative.d2) * invC2;
        float maxZ = (1.0f - projNative.d2) * invC2;
        projNative.c2 = 1.0f / (maxZ - minZ);
        projNative.d2 = minZ / (minZ - maxZ);
    }

    if (!(this->m_appMasterEnables & (1 << GxMasterEnable_NormalProjection)) && projNative.d3 != 1.0f) {
        C44Matrix scale;
        scale.a0 = 0.2f;
        scale.b1 = 0.2f;
        scale.c2 = 0.2f;
        projNative = projNative * scale;
    }

    this->m_xforms[GxXform_Projection].m_dirty = 1;
    this->m_projNative = projNative;
    this->m_ffpDirty = true;
}

void CGxDeviceD3d11::ISetTexGen(uint32_t tmu, int32_t texGen) {
    if (tmu >= 8) {
        return;
    }

    this->m_texGenMode[tmu] = texGen;
    this->m_ffpDirty = true;
    CGxMatrixStack& stack = this->m_texGen[tmu];
    C44Matrix matrix;

    if (texGen == GxTexGen_ObjectLinear || texGen == GxTexGen_EyeLinear) {
        CGxMatrixStack& view = this->m_xforms[GxXform_View];
        matrix = view.m_mtx[view.m_level];
        matrix = matrix.AffineInverse();

        if (texGen == GxTexGen_ObjectLinear) {
            CGxMatrixStack& world = this->m_xforms[GxXform_World];
            matrix = matrix * world.m_mtx[world.m_level].Inverse();
        }
    } else if (texGen == GxTexGen_SphereMap) {
        matrix.a0 = 0.5f;
        matrix.b1 = 0.5f;
        matrix.d0 = 0.5f;
        matrix.d1 = 0.5f;
    } else {
        stack.Identity();
        return;
    }

    stack.m_dirty = 1;
    stack.m_flags[stack.m_level] &= ~CGxMatrixStack::F_Identity;
    stack.m_mtx[stack.m_level] = matrix;
}

void CGxDeviceD3d11::IXformSetTex(uint32_t tmu) {
    // The Direct3D 9 device's texture transform (IXformSetTex): count 0 none, 2 or 3 components, 0x100 projected.
    uint32_t flags = 0;
    CGxMatrixStack& texGen = this->m_texGen[tmu];
    CGxMatrixStack& xform = this->m_xforms[tmu];
    C44Matrix matrix = texGen.m_mtx[texGen.m_level];

    switch (this->m_appRenderStates[GxRs_TextureShader0 + tmu].m_value.m_data.i[0]) {
        case 0:
            if (!(texGen.m_flags[texGen.m_level] & CGxMatrixStack::F_Identity)) {
                flags = 3;
            }

            break;

        case 1: {
            C44Matrix product = texGen.m_mtx[texGen.m_level] * xform.m_mtx[xform.m_level];
            matrix = product;

            if (this->m_appRenderStates[GxRs_TexGen0 + tmu].m_value.m_data.i[0]) {
                flags = 3;
            } else {
                flags = 2;
                matrix.c0 = product.d0;
                matrix.c1 = product.d1;
            }

            break;
        }

        case 2:
            matrix = texGen.m_mtx[texGen.m_level] * xform.m_mtx[xform.m_level];
            flags = 3 | 0x100;
            break;

        default:
            break;
    }

    this->m_texMatrix[tmu] = matrix;
    this->m_texTransform[tmu] = flags;
    xform.m_dirty = 0;
    texGen.m_dirty = 0;
    this->m_ffpDirty = true;
}

void CGxDeviceD3d11::IRsSendToHw(EGxRenderState which) {
    CGxStateBom& value = this->m_appRenderStates[which].m_value;

    switch (which) {
        case GxRs_PolygonOffset:
            this->m_depthBias = -value.m_data.f[0];
            this->m_stateDirty = true;
            break;

        case GxRs_MatDiffuse:
        case GxRs_MatEmissive:
        case GxRs_MatSpecular:
        case GxRs_MatSpecularExp:
            this->m_specularEnable = this->m_appRenderStates[GxRs_MatSpecularExp].m_value.m_data.f[0] > 0.0f && this->m_appRenderStates[GxRs_MatSpecular].m_value.m_data.i[0];
            this->m_ffpDirty = true;
            break;

        case GxRs_NormalizeNormals:
            this->m_normalizeNormals = value.m_data.i[0] != 0;
            this->m_ffpDirty = true;
            break;

        case GxRs_BlendingMode:
            this->m_blendMode = std::min<uint32_t>(value.m_data.u[0], 11);
            this->m_stateDirty = true;
            break;

        case GxRs_AlphaRef:
            this->m_alphaRef = value.m_data.i[0];
            this->m_ffpDirty = true;
            break;

        case GxRs_FogStart:
            this->m_fogStart = value.m_data.f[0];
            this->m_ffpDirty = true;
            break;

        case GxRs_FogEnd:
            this->m_fogEnd = value.m_data.f[0];
            this->m_ffpDirty = true;
            break;

        case GxRs_FogColor:
            this->m_fogColor = value.m_data.u[0];
            this->m_ffpDirty = true;
            break;

        case GxRs_Lighting:
            this->m_lighting = (this->m_appMasterEnables & 0x1) ? value.m_data.i[0] != 0 : 0;
            this->m_ffpDirty = true;
            break;

        case GxRs_Fog:
            this->m_fog = (this->m_appMasterEnables & 0x2) ? value.m_data.i[0] != 0 : 0;
            this->m_ffpDirty = true;
            break;

        case GxRs_DepthTest:
        case GxRs_DepthFunc:
            if ((this->m_appMasterEnables & 0x4) && this->m_appRenderStates[GxRs_DepthTest].m_value.m_data.i[0]) {
                this->m_depthFunc = 1 + std::min<uint32_t>(this->m_appRenderStates[GxRs_DepthFunc].m_value.m_data.u[0], 3);
            } else {
                this->m_depthFunc = 0;
            }

            this->m_appRenderStates[GxRs_DepthTest].m_dirty = 0;
            this->m_appRenderStates[GxRs_DepthFunc].m_dirty = 0;
            this->m_stateDirty = true;
            break;

        case GxRs_DepthWrite:
            this->m_depthWrite = (this->m_appMasterEnables & 0x8) ? value.m_data.i[0] != 0 : false;
            this->m_stateDirty = true;
            break;

        case GxRs_ColorWrite:
            this->m_colorMask = (this->m_appMasterEnables & 0x10) ? value.m_data.u[0] : 0;
            this->m_stateDirty = true;
            break;

        case GxRs_Culling: {
            uint32_t cull = 0;

            if (this->m_appMasterEnables & 0x20) {
                cull = value.m_data.i[0] > 2 ? 2 : static_cast<uint32_t>(std::max(value.m_data.i[0], 0));
            }

            this->m_cull = cull;
            this->m_stateDirty = true;
            break;
        }

        case GxRs_ClipPlaneMask:
            this->m_clipEnable = value.m_data.u[0];
            this->m_ffpDirty = true;
            break;

        case GxRs_Multisample:
            this->m_multisample = value.m_data.i[0] != 0;
            this->m_stateDirty = true;
            break;

        case GxRs_ScissorTest:
            this->m_scissor = value.m_data.i[0] != 0;
            this->m_stateDirty = true;
            break;

        case GxRs_Texture0:
        case GxRs_Texture1:
        case GxRs_Texture2:
        case GxRs_Texture3:
        case GxRs_Texture4:
        case GxRs_Texture5:
        case GxRs_Texture6:
        case GxRs_Texture7:
        case GxRs_Texture8:
        case GxRs_Texture9:
        case GxRs_Texture10:
        case GxRs_Texture11:
        case GxRs_Texture12:
        case GxRs_Texture13:
        case GxRs_Texture14:
        case GxRs_Texture15: {
            uint32_t tmu = which - GxRs_Texture0;
            CGxTex* texId = static_cast<CGxTex*>(value.m_data.p);

            if (texId) {
                this->ITexMarkAsUpdated(texId);
            }

            this->m_boundTextures[tmu] = texId;
            this->m_texturesDirty = true;

            if (tmu < 8) {
                this->m_ffpDirty = true;
            }

            break;
        }

        case GxRs_ColorOp0:
        case GxRs_ColorOp1:
        case GxRs_ColorOp2:
        case GxRs_ColorOp3:
        case GxRs_ColorOp4:
        case GxRs_ColorOp5:
        case GxRs_ColorOp6:
        case GxRs_ColorOp7:
            this->m_colorOp[which - GxRs_ColorOp0] = value.m_data.u[0];
            this->m_ffpDirty = true;
            break;

        case GxRs_AlphaOp0:
        case GxRs_AlphaOp1:
        case GxRs_AlphaOp2:
        case GxRs_AlphaOp3:
        case GxRs_AlphaOp4:
        case GxRs_AlphaOp5:
        case GxRs_AlphaOp6:
        case GxRs_AlphaOp7:
            this->m_alphaOp[which - GxRs_AlphaOp0] = value.m_data.u[0];
            this->m_ffpDirty = true;
            break;

        case GxRs_TexGen0:
        case GxRs_TexGen1:
        case GxRs_TexGen2:
        case GxRs_TexGen3:
        case GxRs_TexGen4:
        case GxRs_TexGen5:
        case GxRs_TexGen6:
        case GxRs_TexGen7:
            this->ISetTexGen(which - GxRs_TexGen0, value.m_data.i[0]);
            break;

        case GxRs_TextureCoord0:
        case GxRs_TextureCoord1:
        case GxRs_TextureCoord2:
        case GxRs_TextureCoord3:
        case GxRs_TextureCoord4:
        case GxRs_TextureCoord5:
        case GxRs_TextureCoord6:
        case GxRs_TextureCoord7:
            this->m_texCoord[which - GxRs_TextureCoord0] = value.m_data.u[0];
            this->m_ffpDirty = true;
            break;

        case GxRs_TextureShader0:
        case GxRs_TextureShader1:
        case GxRs_TextureShader2:
        case GxRs_TextureShader3:
        case GxRs_TextureShader4:
        case GxRs_TextureShader5:
        case GxRs_TextureShader6:
        case GxRs_TextureShader7:
            this->m_xforms[which - GxRs_TextureShader0].m_dirty = 1;
            break;

        case GxRs_VertexShader:
        case GxRs_PixelShader: {
            CGxShader* shader = static_cast<CGxShader*>(value.m_data.p);

            // Permutations are created when first bound, as on the Direct3D 9 device.
            if (shader && !shader->m_loaded) {
                this->IShaderCreate(shader);
            }

            if (which == GxRs_VertexShader) {
                this->m_boundVs = shader;
                this->m_vertexDirty = true;
            } else {
                this->m_boundPs = shader;
            }

            break;
        }

        case Unk84:
            this->m_blendFactor = value.m_data.f[0];
            this->m_stateDirty = true;
            break;

        default:
            break;
    }
}

ID3D11SamplerState* CGxDeviceD3d11::ISampler(CGxTex* texId, bool comparison) {
    uint32_t filter = texId->m_flags.m_filter;
    uint32_t anisotropy = std::max<uint32_t>(texId->m_flags.m_maxAnisotropy, 1);
    uint64_t key = filter | (texId->m_flags.m_wrapU << 4) | (texId->m_flags.m_wrapV << 5) | (static_cast<uint64_t>(anisotropy) << 8) | (comparison ? 1ull << 16 : 0);
    auto it = this->m_samplers.find(key);

    if (it != this->m_samplers.end()) {
        return it->second;
    }

    D3D11_SAMPLER_DESC desc = {};
    // Gx filters: nearest, linear, nearest mip nearest, linear mip nearest, trilinear, anisotropic.
    bool mips = filter >= GxTex_NearestMipNearest;

    switch (filter) {
        case GxTex_Nearest:
        case GxTex_NearestMipNearest:
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            break;

        case GxTex_Linear:
        case GxTex_LinearMipNearest:
            desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            break;

        case GxTex_LinearMipLinear:
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            break;

        default:
            desc.Filter = anisotropy > 1 ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            break;
    }

    if (comparison) {
        desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    } else {
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    }

    desc.AddressU = texId->m_flags.m_wrapU ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressV = texId->m_flags.m_wrapV ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.MaxAnisotropy = std::min<uint32_t>(anisotropy, 16);
    desc.MinLOD = 0.0f;
    desc.MaxLOD = mips ? D3D11_FLOAT32_MAX : 0.0f;

    ID3D11SamplerState* sampler = nullptr;
    this->m_device->CreateSamplerState(&desc, &sampler);
    this->m_samplers[key] = sampler;
    return sampler;
}

bool CGxDeviceD3d11::IShadersActive() {
    return this->m_boundVs && this->m_boundVs->m_apiSpecific && this->m_boundVs->m_valid;
}

void CGxDeviceD3d11::IStateSyncStates() {
    if (!this->m_stateDirty) {
        return;
    }

    this->m_stateDirty = false;
    uint32_t mode = this->m_blendMode;
    uint64_t blendKey = mode | (static_cast<uint64_t>(this->m_colorMask & 0xF) << 4);
    auto blendIt = this->m_blendStates.find(blendKey);
    ID3D11BlendState* blendState = nullptr;

    if (blendIt != this->m_blendStates.end()) {
        blendState = blendIt->second;
    } else {
        D3D11_BLEND_DESC desc = {};
        D3D11_RENDER_TARGET_BLEND_DESC& rt = desc.RenderTarget[0];
        rt.BlendEnable = mode >= 2;
        rt.SrcBlend = s_srcBlend[mode];
        rt.DestBlend = s_dstBlend[mode];
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = AlphaBlend(s_srcBlend[mode]);
        rt.DestBlendAlpha = AlphaBlend(s_dstBlend[mode]);
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        // Gx colour write bits: 1 red, 2 blue, 4 green, 8 alpha (the Direct3D 9 device's mapping).
        uint32_t mask = this->m_colorMask;
        rt.RenderTargetWriteMask = ((mask & 1) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) | ((mask & 4) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) | ((mask & 2) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) | ((mask & 8) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
        this->m_device->CreateBlendState(&desc, &blendState);
        this->m_blendStates[blendKey] = blendState;
    }

    uint32_t factorBits;
    memcpy(&factorBits, &this->m_blendFactor, 4);
    uint64_t hwBlend = blendKey | (static_cast<uint64_t>(factorBits) << 32);

    if (hwBlend != this->m_hwBlend) {
        float factor[4] = { this->m_blendFactor, this->m_blendFactor, this->m_blendFactor, this->m_blendFactor };
        this->m_ctx->OMSetBlendState(blendState, factor, 0xFFFFFFFF);
        this->m_hwBlend = hwBlend;
    }

    uint64_t depthKey = this->m_depthFunc | (this->m_depthWrite ? 0x10 : 0);

    if (depthKey != this->m_hwDepth) {
        auto it = this->m_depthStates.find(depthKey);
        ID3D11DepthStencilState* state = nullptr;

        if (it != this->m_depthStates.end()) {
            state = it->second;
        } else {
            D3D11_DEPTH_STENCIL_DESC desc = {};
            desc.DepthEnable = TRUE;
            desc.DepthWriteMask = this->m_depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            desc.DepthFunc = this->m_depthFunc ? s_cmpFunc[this->m_depthFunc - 1] : D3D11_COMPARISON_ALWAYS;
            desc.StencilEnable = FALSE;
            this->m_device->CreateDepthStencilState(&desc, &state);
            this->m_depthStates[depthKey] = state;
        }

        this->m_hwDepth = depthKey;
        this->m_ctx->OMSetDepthStencilState(state, 0);
    }

    uint32_t cull = this->m_cull;

    // A mirrored view (water reflections) turns the winding around.
    if (this->m_cullFlip && cull) {
        cull = 3 - cull;
    }

    // The Direct3D 9 depth bias is in depth units; Direct3D 11's in steps of a 24-bit depth buffer.
    int32_t bias = static_cast<int32_t>(this->m_depthBias * 16777215.0f);
    uint64_t rasterKey = cull | (this->m_scissor ? 4 : 0) | (this->m_multisample ? 8 : 0) | (this->m_wireframe ? 16 : 0) | (static_cast<uint64_t>(static_cast<uint32_t>(bias)) << 32);

    if (rasterKey != this->m_hwRaster) {
        auto it = this->m_rasterStates.find(rasterKey);
        ID3D11RasterizerState* state = nullptr;

        if (it != this->m_rasterStates.end()) {
            state = it->second;
        } else {
            D3D11_RASTERIZER_DESC desc = {};
            desc.FillMode = this->m_wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
            // Direct3D 9: 1 culls clockwise faces, 2 counter-clockwise ones. Front faces are clockwise here.
            desc.CullMode = cull == 1 ? D3D11_CULL_FRONT : (cull == 2 ? D3D11_CULL_BACK : D3D11_CULL_NONE);
            desc.FrontCounterClockwise = FALSE;
            desc.DepthBias = bias;
            desc.DepthClipEnable = TRUE;
            desc.ScissorEnable = this->m_scissor;
            desc.MultisampleEnable = this->m_multisample;
            this->m_device->CreateRasterizerState(&desc, &state);
            this->m_rasterStates[rasterKey] = state;
        }

        this->m_hwRaster = rasterKey;
        this->m_ctx->RSSetState(state);
    }
}

void CGxDeviceD3d11::IXformSetViewport() {
    const CRect& window = this->DeviceCurWindow();
    D3D11_VIEWPORT viewport;
    float x = floorf(this->m_viewport.m_x.m_l * window.maxX + 0.5f);
    float y = floorf((1.0f - this->m_viewport.m_y.m_h) * window.maxY + 0.5f);

    viewport.TopLeftX = x;
    viewport.TopLeftY = y;
    viewport.Width = floorf(this->m_viewport.m_x.m_h * window.maxX - x + 0.5f);
    viewport.Height = floorf((1.0f - this->m_viewport.m_y.m_l) * window.maxY - y + 0.5f);
    viewport.MinDepth = this->m_viewport.m_z.m_l;
    viewport.MaxDepth = this->m_viewport.m_z.m_h;

    if (this->m_targetIsTexture) {
        viewport.TopLeftY = floorf(window.maxY * this->m_viewport.m_y.m_l);
    }

    this->m_ctx->RSSetViewports(1, &viewport);
    this->m_viewportDirty = 0;
}

void CGxDeviceD3d11::IStateSyncScissorRect() {
    if (!this->m_scissorRectDirty) {
        return;
    }

    const CRect& window = this->DeviceCurWindow();
    D3D11_RECT rect;
    rect.left = static_cast<LONG>(this->m_scissorRect.minX * window.maxX + 0.5f);
    rect.right = static_cast<LONG>(this->m_scissorRect.maxX * window.maxX + 1.0f);

    if (this->m_targetIsTexture) {
        rect.top = static_cast<LONG>(this->m_scissorRect.minY * window.maxY + 0.5f);
        rect.bottom = static_cast<LONG>(1.0f + this->m_scissorRect.maxY * window.maxY);
    } else {
        rect.top = static_cast<LONG>((1.0f - this->m_scissorRect.maxY) * window.maxY + 0.5f);
        rect.bottom = static_cast<LONG>(1.0f + (1.0f - this->m_scissorRect.minY) * window.maxY);
    }

    rect.left = std::max<LONG>(rect.left, 0);
    rect.top = std::max<LONG>(rect.top, 0);
    rect.right = std::min<LONG>(rect.right, static_cast<LONG>(window.maxX));
    rect.bottom = std::min<LONG>(rect.bottom, static_cast<LONG>(window.maxY));
    this->m_ctx->RSSetScissorRects(1, &rect);
    this->m_scissorRectDirty = 0;
}

void CGxDeviceD3d11::IShaderConstantsFlush() {
    // Both stages' registers go up whole (4 KB each) when any changed.
    for (uint32_t stage = 0; stage < 2; stage++) {
        // s_shadowConstants[1] holds the vertex registers, [0] the pixel ones.
        ShadowConstants& shadow = CGxDevice::s_shadowConstants[stage == 0 ? 1 : 0];

        if (shadow.dirtyMin <= shadow.dirtyMax) {
            D3D11_MAPPED_SUBRESOURCE mapped;

            if (SUCCEEDED(this->m_ctx->Map(this->m_constants[stage], 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                memcpy(mapped.pData, shadow.constants, sizeof(shadow.constants));
                this->m_ctx->Unmap(this->m_constants[stage], 0);
            }
        }

        shadow.dirtyMin = 255;
        shadow.dirtyMax = 0;
    }
}

void CGxDeviceD3d11::IFfpSync() {
    // The lights, as the Direct3D 9 device's IStateSyncLights would set them: lit draws use the lights, unlit
    // draws without vertex colours but with a material colour light it by an ambient of 1.
    int32_t lighting = 0;
    uint32_t ambient = 0;

    if (this->m_appRenderStates[GxRs_Lighting].m_value.m_data.i[0]) {
        lighting = this->m_appMasterEnables & 0x1;
        this->m_ambientOnlyMode = 0;
        // The lights have no dirty flag of their own.
        this->m_ffpDirty = true;
    } else if ((this->m_primVertexMask & (1 << GxVA_Color0)) || this->m_appRenderStates[GxRs_MatDiffuse].m_value.m_data.u[0] == 0xFFFFFFFF) {
        this->m_ambientOnlyMode = 0;
    } else {
        this->m_ambientOnlyMode = 1;
        lighting = 1;
        ambient = 0xFFFFFFFF;
    }

    for (uint32_t i = 0; i < 8; i++) {
        if (this->m_xforms[i].m_dirty || this->m_texGen[i].m_dirty) {
            this->IXformSetTex(i);
        }
    }

    if (this->m_xforms[GxXform_World].m_dirty || this->m_xforms[GxXform_View].m_dirty || this->m_xforms[GxXform_Projection].m_dirty) {
        this->m_ffpDirty = true;
    }

    // The colour vertex attribute decides the material sources (IStateSyncMaterial).
    if (!this->m_ffpDirty && !(this->m_primVertexDirty & ((1 << GxVA_Color0) | (1 << GxVA_Color1)))) {
        return;
    }

    FfpConstants c;
    memset(&c, 0, sizeof(c));
    CGxMatrixStack& world = this->m_xforms[GxXform_World];
    CGxMatrixStack& view = this->m_xforms[GxXform_View];
    Matrix(world.m_mtx[world.m_level], c.world);
    Matrix(view.m_mtx[view.m_level], c.view);
    Matrix(this->m_projNative, c.proj);
    world.m_dirty = 0;
    view.m_dirty = 0;
    this->m_xforms[GxXform_Projection].m_dirty = 0;

    // Stages run until the first without a texture or switched off.
    uint32_t cubes = 0;

    for (uint32_t i = 0; i < 8; i++) {
        Matrix(this->m_texMatrix[i], c.tex[i]);
        CGxTex* tex = this->m_boundTextures[i];
        c.stage[i][0] = tex ? static_cast<int32_t>(std::min<uint32_t>(this->m_colorOp[i], 5)) : -1;
        c.stage[i][1] = static_cast<int32_t>(std::min<uint32_t>(this->m_alphaOp[i], 5));
        int32_t gen = 0;

        switch (this->m_texGenMode[i]) {
            case GxTexGen_ObjectLinear:
            case GxTexGen_EyeLinear:
            case GxTexGen_Unk3:
                gen = 1;
                break;

            case GxTexGen_ReflectionMap:
            case GxTexGen_SphereMap:
                gen = 2;
                break;

            case GxTexGen_Normal:
                gen = 3;
                break;

            default:
                break;
        }

        c.stage[i][2] = static_cast<int32_t>(this->m_texCoord[i] & 7) | (gen << 4);
        c.stage[i][3] = static_cast<int32_t>(this->m_texTransform[i]);

        if (tex && tex->m_target == GxTex_CubeMap) {
            cubes |= 1u << i;
        }
    }

    CImVector diffuse;
    CImVector emissive;
    CImVector specular;
    *reinterpret_cast<uint32_t*>(&diffuse) = this->m_appRenderStates[GxRs_MatDiffuse].m_value.m_data.u[0];
    *reinterpret_cast<uint32_t*>(&emissive) = this->m_appRenderStates[GxRs_MatEmissive].m_value.m_data.u[0];
    *reinterpret_cast<uint32_t*>(&specular) = this->m_appRenderStates[GxRs_MatSpecular].m_value.m_data.u[0];
    Rgba(*reinterpret_cast<uint32_t*>(&diffuse), c.matDiffuse);
    Rgba(*reinterpret_cast<uint32_t*>(&emissive), c.matEmissive);
    Rgba(*reinterpret_cast<uint32_t*>(&specular), c.matSpecular);
    c.matSpecular[3] = this->m_appRenderStates[GxRs_MatSpecularExp].m_value.m_data.f[0];
    Rgba(ambient, c.ambient);

    const C44Matrix& viewMatrix = view.m_mtx[view.m_level];

    for (uint32_t i = 0; i < 4; i++) {
        CGxApiLight& light = this->m_lights[i];

        if (!light.m_enable || this->m_ambientOnlyMode) {
            continue;
        }

        // Lights are given in world space; the shader lights in view space.
        float x = light.m_dir.x;
        float y = light.m_dir.y;
        float z = light.m_dir.z;
        float w = light.m_dir.w == 1.0f ? 1.0f : 0.0f;
        c.lightPos[i][0] = x * viewMatrix.a0 + y * viewMatrix.b0 + z * viewMatrix.c0 + w * viewMatrix.d0;
        c.lightPos[i][1] = x * viewMatrix.a1 + y * viewMatrix.b1 + z * viewMatrix.c1 + w * viewMatrix.d1;
        c.lightPos[i][2] = x * viewMatrix.a2 + y * viewMatrix.b2 + z * viewMatrix.c2 + w * viewMatrix.d2;
        c.lightPos[i][3] = w;
        c.lightDiffuse[i][0] = light.m_dirColor.x;
        c.lightDiffuse[i][1] = light.m_dirColor.y;
        c.lightDiffuse[i][2] = light.m_dirColor.z;
        c.lightAmbient[i][0] = light.m_ambColor.x;
        c.lightAmbient[i][1] = light.m_ambColor.y;
        c.lightAmbient[i][2] = light.m_ambColor.z;
        c.lightSpecular[i][0] = light.m_specColor.x;
        c.lightSpecular[i][1] = light.m_specColor.y;
        c.lightSpecular[i][2] = light.m_specColor.z;
        c.lightAtten[i][0] = light.m_constantAttenuation;
        c.lightAtten[i][1] = light.m_linearAttenuation;
        c.lightAtten[i][2] = light.m_quadraticAttenuation;
        c.lightAtten[i][3] = 1.0f;
    }

    bool color0 = (this->m_primVertexMask & (1 << GxVA_Color0)) != 0;
    bool color1 = (this->m_primVertexMask & (1 << GxVA_Color1)) != 0;
    // Material sources (D3DMCS): 0 the material, 1 vertex colour 0, 2 vertex colour 1 (IStateSyncMaterial).
    int32_t sources = 0;

    if (color0) {
        int32_t colorMaterial = this->m_appRenderStates[GxRs_ColorMaterial].m_value.m_data.i[0];
        int32_t ambientSource = colorMaterial == 0;
        int32_t diffuseSource = colorMaterial == 0;
        int32_t specularSource = colorMaterial == 1;
        int32_t emissiveSource = colorMaterial == 2;
        sources = ambientSource | (diffuseSource << 2) | (specularSource << 4) | (emissiveSource << 6);
    }

    c.flags[0] = lighting;
    c.flags[1] = this->m_fog;
    c.flags[2] = 0;
    c.fogEnable[0] = this->m_fog ? 1.0f : 0.0f;
    c.alphaTest[0] = this->m_alphaRef > 0 ? 1.0f : 0.0f;
    c.alphaTest[1] = static_cast<float>(std::min(std::max(this->m_alphaRef, 0), 255));
    c.flags[3] = this->m_normalizeNormals;
    c.flags2[0] = color0;
    c.flags2[1] = color1;
    c.flags2[2] = sources;
    c.flags2[3] = this->m_specularEnable && lighting;
    c.fog[0] = this->m_fogStart;
    c.fog[1] = this->m_fogEnd;
    Rgba(this->m_fogColor, c.fogColor);

    for (uint32_t i = 0; i < 6; i++) {
        memcpy(c.clip[i], &this->m_clipPlane[i], sizeof(float) * 4);
    }

    c.clipMask[0] = static_cast<int32_t>(this->m_clipEnable & 0x3F);
    c.clipMask[1] = static_cast<int32_t>(cubes);

    D3D11_MAPPED_SUBRESOURCE mapped;

    if (SUCCEEDED(this->m_ctx->Map(this->m_ffpConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &c, sizeof(c));
        this->m_ctx->Unmap(this->m_ffpConstants, 0);
    }

    this->m_ffpDirty = false;
}

void CGxDeviceD3d11::IStateSyncShaders() {
    Shader* vs = this->IShadersActive() ? static_cast<Shader*>(this->m_boundVs->m_apiSpecific) : nullptr;
    Shader* ps = this->m_boundPs && this->m_boundPs->m_valid ? static_cast<Shader*>(this->m_boundPs->m_apiSpecific) : nullptr;
    void* hwVs = vs ? vs->m_object : this->m_ffpVs;
    void* hwPs = ps ? ps->m_object : this->m_ffpPs;

    if (hwVs != this->m_hwVs) {
        this->m_ctx->VSSetShader(static_cast<ID3D11VertexShader*>(hwVs), nullptr, 0);
        this->m_hwVs = hwVs;
        this->m_vertexDirty = true;
    }

    if (hwPs != this->m_hwPs) {
        this->m_ctx->PSSetShader(static_cast<ID3D11PixelShader*>(hwPs), nullptr, 0);
        this->m_hwPs = hwPs;
        this->m_texturesDirty = true;
    }

    ID3D11Buffer* vsBuffers[2] = { this->m_constants[0], this->m_ffpConstants };
    ID3D11Buffer* psBuffers[2] = { this->m_constants[1], this->m_ffpConstants };
    this->m_ctx->VSSetConstantBuffers(0, 2, vsBuffers);
    this->m_ctx->PSSetConstantBuffers(0, 2, psBuffers);
}

void CGxDeviceD3d11::IStateSyncTextures() {
    if (!this->m_texturesDirty) {
        return;
    }

    this->m_texturesDirty = false;
    bool ffp = this->m_hwPs == this->m_ffpPs;
    Shader* ps = !ffp && this->m_boundPs ? static_cast<Shader*>(this->m_boundPs->m_apiSpecific) : nullptr;
    uint32_t comparison = ps ? ps->m_comparisonSamplers : 0;
    ID3D11ShaderResourceView* views[24] = {};
    ID3D11SamplerState* samplers[16] = {};

    for (uint32_t i = 0; i < 16; i++) {
        CGxTex* texId = this->m_boundTextures[i];

        if (!texId) {
            continue;
        }

        if (texId->m_needsCreation || !texId->m_apiSpecificData) {
            this->ITexMarkAsUpdated(texId);
        }

        Texture* tex = static_cast<Texture*>(texId->m_apiSpecificData);

        if (!tex || !tex->m_srv) {
            continue;
        }

        // Nothing being drawn into is read.
        if ((tex->m_rtv[0] && this->m_curRtv == tex->m_rtv[0]) || (tex->m_dsv && this->m_curDsv == tex->m_dsv)) {
            continue;
        }

        // The fixed-function shader reads cube maps from slots 16-23.
        if (ffp && tex->m_cube && i < 8) {
            views[16 + i] = tex->m_srv;
        } else {
            views[i] = tex->m_srv;
        }

        samplers[i] = this->ISampler(texId, (comparison >> i) & 1);
    }

    this->m_ctx->PSSetShaderResources(0, 24, views);
    this->m_ctx->PSSetSamplers(0, 16, samplers);
}

void CGxDeviceD3d11::IStateSyncVertexPtrs() {
    if (!this->m_vertexDirty && !this->m_primVertexDirty) {
        return;
    }

    this->m_vertexDirty = false;
    Shader* vs = this->m_hwVs == this->m_ffpVs ? &this->m_ffpVsInfo : static_cast<Shader*>(this->m_boundVs->m_apiSpecific);

    // Streams: one per distinct buffer, with the attributes' sizes summed for the stride (as Direct3D 9's).
    CGxBuf* streamBufs[GxVAs_Last] = {};
    uint32_t streamStrides[GxVAs_Last] = {};
    uint32_t streamCount = 0;
    uint32_t attribStream[GxVAs_Last] = {};

    for (uint32_t i = 0; i < GxVAs_Last; i++) {
        if (!(this->m_primVertexMask & (1 << i))) {
            continue;
        }

        uint32_t stream;

        for (stream = 0; stream < streamCount; stream++) {
            if (streamBufs[stream] == this->m_primVertexFormatBuf[i]) {
                break;
            }
        }

        if (stream == streamCount) {
            streamBufs[streamCount++] = this->m_primVertexFormatBuf[i];
        }

        attribStream[i] = stream;
        streamStrides[stream] += s_attribSize[std::min<uint32_t>(this->m_primVertexFormatAttrib[i].m_type, 6)];
    }

    // The layout: each input the shader reads, from the matching attribute, or zeroes when there is none.
    D3D11_INPUT_ELEMENT_DESC elements[32];
    uint32_t elementCount = 0;
    uint64_t key = Hash(0, reinterpret_cast<uintptr_t>(vs));

    for (const Shader::Input& input : vs->m_inputs) {
        if (elementCount >= 32) {
            break;
        }

        D3D11_INPUT_ELEMENT_DESC& element = elements[elementCount++];
        element.SemanticName = input.m_name;
        element.SemanticIndex = input.m_index;
        element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        element.InstanceDataStepRate = 0;
        int32_t attrib = -1;

        for (uint32_t i = 0; i < GxVAs_Last; i++) {
            if ((this->m_primVertexMask & (1 << i)) && s_attribSemanticIndex[i] == input.m_index && !SStrCmpI(s_attribSemantic[i], input.m_name, STORM_MAX_STR)) {
                attrib = i;
                break;
            }
        }

        if (attrib >= 0) {
            const CGxVertexAttrib& gx = this->m_primVertexFormatAttrib[attrib];
            uint32_t type = std::min<uint32_t>(gx.m_type, 6);
            DXGI_FORMAT format = s_attribFormat[type];

            // Integer inputs read the byte attributes as integers.
            if (type == 2 && input.m_componentType == 1) {
                format = DXGI_FORMAT_R8G8B8A8_UINT;
            } else if (type == 1 && input.m_componentType == 3) {
                format = DXGI_FORMAT_R8G8B8A8_UNORM;
            } else if (type == 5 && input.m_componentType == 3) {
                format = DXGI_FORMAT_R16G16_SNORM;
            }

            element.Format = format;
            element.InputSlot = attribStream[attrib];
            element.AlignedByteOffset = gx.m_offset;
            key = Hash(key, (static_cast<uint64_t>(attrib) << 40) | (static_cast<uint64_t>(format) << 32) | (attribStream[attrib] << 16) | gx.m_offset);
        } else {
            element.Format = input.m_componentType == 1 ? DXGI_FORMAT_R32G32B32A32_UINT : (input.m_componentType == 2 ? DXGI_FORMAT_R32G32B32A32_SINT : DXGI_FORMAT_R32G32B32A32_FLOAT);
            element.InputSlot = 15;
            element.AlignedByteOffset = 0;
            key = Hash(key, 0xFFFF0000ull | elementCount);
        }
    }

    key = (key & 0xFFFFFFFFull) | (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vs) & 0xFFFFFFFFull) << 32);
    auto it = this->m_inputLayouts.find(key);
    ID3D11InputLayout* layout = nullptr;

    if (it != this->m_inputLayouts.end()) {
        layout = it->second;
    } else {
        if (FAILED(this->m_device->CreateInputLayout(elements, elementCount, vs->m_code.data(), vs->m_code.size(), &layout))) {
            static uint32_t s_failures;

            if (s_failures++ < 8) {
                CGxDevice::Log("CGxDeviceD3d11: input layout failed (%u inputs, vertex mask %x)", elementCount, this->m_primVertexMask);
            }

            layout = nullptr;
        }

        this->m_inputLayouts[key] = layout;
    }

    this->m_ctx->IASetInputLayout(layout);

    ID3D11Buffer* buffers[16] = {};
    UINT strides[16] = {};
    UINT offsets[16] = {};

    for (uint32_t stream = 0; stream < streamCount && stream < 15; stream++) {
        CGxBuf* buf = streamBufs[stream];
        Pool* pool = buf && buf->m_pool ? static_cast<Pool*>(buf->m_pool->m_apiSpecific) : nullptr;
        buffers[stream] = pool ? pool->m_buffer : nullptr;
        strides[stream] = streamStrides[stream];
        offsets[stream] = buf ? buf->m_index : 0;
    }

    buffers[15] = this->m_zeroBuffer;
    strides[15] = 0;
    offsets[15] = 0;
    this->m_ctx->IASetVertexBuffers(0, 16, buffers, strides, offsets);
    this->m_primVertexDirty = 0;
}

void CGxDeviceD3d11::IStateSync() {
    this->IShaderConstantsFlush();
    this->IRsSync(0);

    bool wireframe = !(this->m_appMasterEnables & (1 << GxMasterEnable_PolygonFill));

    if (wireframe != this->m_wireframe) {
        this->m_wireframe = wireframe;
        this->m_stateDirty = true;
    }

    this->m_hwMasterEnables = this->m_appMasterEnables;

    if (this->m_clipPlaneMask) {
        this->m_clipPlaneMask = 0;
        this->m_ffpDirty = true;
    }

    this->IStateSyncShaders();
    this->IFfpSync();
    this->IStateSyncStates();
    this->IStateSyncTextures();
    this->IStateSyncScissorRect();
    this->IStateSyncVertexPtrs();

    if (this->m_viewportDirty) {
        this->IXformSetViewport();
    }
}

void CGxDeviceD3d11::Draw(CGxBatch* batch, int32_t count) {
    if (!this->m_context || this->m_intF5C) {
        return;
    }

    this->IStateSync();

    D3D11_PRIMITIVE_TOPOLOGY topology = s_topology[batch->m_primType];
    this->m_ctx->IASetPrimitiveTopology(topology);

    if (batch->m_primType == GxPrim_TriangleFan) {
        // No fans in Direct3D 11: the same triangles as a list, from the index copy when indexed.
        uint32_t triangles = batch->m_count >= 3 ? batch->m_count - 2 : 0;

        if (!triangles) {
            return;
        }

        std::vector<uint16_t> indices(triangles * 3);
        const uint16_t* source = nullptr;

        if (count) {
            Pool* pool = this->m_primIndexBuf && this->m_primIndexBuf->m_pool ? static_cast<Pool*>(this->m_primIndexBuf->m_pool->m_apiSpecific) : nullptr;

            if (!pool) {
                return;
            }

            size_t start = this->m_primIndexBuf->m_index + static_cast<size_t>(batch->m_start) * 2;

            if (start + static_cast<size_t>(batch->m_count) * 2 > pool->m_mirror.size()) {
                return;
            }

            source = reinterpret_cast<const uint16_t*>(pool->m_mirror.data() + start);
        }

        for (uint32_t i = 0; i < triangles; i++) {
            indices[3 * i + 0] = source ? source[0] : 0;
            indices[3 * i + 1] = source ? source[i + 1] : static_cast<uint16_t>(i + 1);
            indices[3 * i + 2] = source ? source[i + 2] : static_cast<uint16_t>(i + 2);
        }

        uint32_t bytes = static_cast<uint32_t>(indices.size() * 2);

        if (bytes > this->m_fanIndicesSize) {
            SafeRelease(this->m_fanIndices);
            D3D11_BUFFER_DESC desc = {};
            desc.ByteWidth = std::max<uint32_t>(bytes, 4096);
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            if (FAILED(this->m_device->CreateBuffer(&desc, nullptr, &this->m_fanIndices))) {
                this->m_fanIndicesSize = 0;
                return;
            }

            this->m_fanIndicesSize = desc.ByteWidth;
        }

        D3D11_MAPPED_SUBRESOURCE mapped;

        if (FAILED(this->m_ctx->Map(this->m_fanIndices, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return;
        }

        memcpy(mapped.pData, indices.data(), bytes);
        this->m_ctx->Unmap(this->m_fanIndices, 0);
        this->m_ctx->IASetIndexBuffer(this->m_fanIndices, DXGI_FORMAT_R16_UINT, 0);
        this->m_ctx->DrawIndexed(triangles * 3, 0, 0);
        this->m_primIndexDirty = 1;
        return;
    }

    if (count) {
        {
            Pool* pool = this->m_primIndexBuf && this->m_primIndexBuf->m_pool ? static_cast<Pool*>(this->m_primIndexBuf->m_pool->m_apiSpecific) : nullptr;
            this->m_ctx->IASetIndexBuffer(pool ? pool->m_buffer : nullptr, DXGI_FORMAT_R16_UINT, this->m_primIndexBuf ? this->m_primIndexBuf->m_index : 0);
            this->m_primIndexDirty = 0;
        }

        this->m_ctx->DrawIndexed(batch->m_count, batch->m_start, 0);
    } else {
        this->m_ctx->Draw(batch->m_count, 0);
    }
}
