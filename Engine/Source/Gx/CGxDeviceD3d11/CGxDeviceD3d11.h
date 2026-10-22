#ifndef ENGINE_SOURCE_GX_CGXDEVICED3D11_CGXDEVICED3D11_H
#define ENGINE_SOURCE_GX_CGXDEVICED3D11_CGXDEVICED3D11_H

// CHANGED: not in the original 12340 client. A Direct3D 11 Gx device (gxApi D3D11), as 4.3.4 added one, for the
// Linux client through DXVK's native Direct3D 11. The Direct3D 9 device stays the default and the fallback.
//
// It runs the client's shaders from their shader model 4 builds (Shaders\Vertex\vs_4_0, Shaders\Pixel\ps_4_0:
// the 3.3.5 shaders compiled for Direct3D 11, the same permutations and constant registers, from the 4.3.4 port's
// data), with the Direct3D 9 constant registers in constant buffer 0 of each stage. Everything the client draws
// without shaders goes through one fixed-function emulation shader (Shaders/FixedFunction.hlsl). The rest of the
// client sees the same capabilities as on the Direct3D 9 device (shader model 3), so it takes the same paths.

#include <cstdint>
#include <unordered_map>
#include <vector>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"

struct ID3D11Device;
struct ID3D11Device1;
struct ID3D11DeviceContext;
struct ID3D11DeviceContext1;
struct ID3D11Buffer;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11ShaderResourceView;
struct ID3D11SamplerState;
struct ID3D11BlendState;
struct ID3D11DepthStencilState;
struct ID3D11RasterizerState;
struct ID3D11InputLayout;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11Query;
struct IDXGIFactory1;
struct IDXGISwapChain;

class CGxDeviceD3d11 : public CGxDevice {
    public:
    // What a CGxTex's m_apiSpecificData points to.
    struct Texture {
        ID3D11Texture2D* m_texture = nullptr;
        struct ID3D11Texture3D* m_volume = nullptr;   // DeviceTexSetVolume
        ID3D11ShaderResourceView* m_srv = nullptr;
        ID3D11RenderTargetView* m_rtv[6] = {};
        ID3D11DepthStencilView* m_dsv = nullptr;
        uint32_t m_dxgiFormat = 0;
        uint32_t m_levels = 1;
        bool m_cube = false;
        bool m_depth = false;
    };

    // What a CGxShader's m_apiSpecific points to.
    struct Shader {
        void* m_object = nullptr;             // ID3D11VertexShader or ID3D11PixelShader
        uint32_t m_comparisonSamplers = 0;    // samplers declared with comparison mode (shadow maps)
        uint32_t m_constants = 0;             // registers of constant buffer 0 the shader reads
        struct Input {
            char m_name[24];
            uint32_t m_index;
            uint32_t m_componentType;         // 1 uint, 2 sint, 3 float
            int32_t m_attrib;                 // the Gx vertex attribute it reads (-1: none), found once
        };
        std::vector<Input> m_inputs;          // vertex shaders: the input signature
        std::vector<uint8_t> m_code;          // vertex shaders: kept for input layouts
        bool m_clipOutputs = false;           // vertex shaders: writes the outputs ClipPlanes.hlsl takes
    };

    // What a CGxPool's m_apiSpecific points to.
    struct Pool {
        ID3D11Buffer* m_buffer = nullptr;
        std::vector<uint8_t> m_mirror;        // what the buffer holds, kept for locks and triangle fans
        bool m_dynamic = false;
        bool m_discard = true;
    };

    CGxDeviceD3d11();
    virtual ~CGxDeviceD3d11();

    virtual void ITexMarkAsUpdated(CGxTex* texId);
    virtual void IRsSendToHw(EGxRenderState which);
    virtual int32_t DeviceCreate(GxWindowProc windowProc, const CGxFormat& format);
    virtual int32_t DeviceCreate(void* window, const CGxFormat& format);
    virtual void DeviceDestroy();
    virtual int32_t DeviceSetFormat(const CGxFormat& format);
    virtual void DeviceSetBaseMipLevel(uint32_t level);
    virtual void DeviceSetGamma(float gamma);
    virtual void DeviceSetGamma(const CGxGammaRamp& ramp);
    virtual void* DeviceWindow();
    virtual void DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels);
    virtual void DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths);
    virtual int32_t DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2);
    virtual void DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane);
    virtual void DeviceSaveDepthSurface();
    virtual void DeviceRestoreDepthSurface();
    virtual void DeviceResolveDepthBuffer(CGxTex* texture);
    virtual void DeviceSetSampledDepth(CGxTex* texture);
    virtual int32_t DeviceCanCopySampledDepth();
    virtual int32_t DeviceCopySampledDepth(CGxTex* texture);
    virtual void DeviceSetCullFlip(int32_t flip);
    virtual int32_t DeviceTexSetVolume(CGxTex* texture, uint32_t width, uint32_t height, uint32_t depth, const void* argb);
    virtual void DeviceCopyTex(CGxTex* source, uint32_t sourcePlane, CGxTex* dest, uint32_t destPlane);
    virtual void DeviceOverride(EGxOverride override, uint32_t value);
    virtual void CapsWindowSize(CRect& rect);
    virtual void CapsWindowSizeInScreenCoords(CRect& rect);
    virtual void LogCrashInfo(char* buffer, uint32_t size);
    virtual void ScenePresent();
    virtual void SceneClear(uint32_t mask, CImVector color);
    virtual void XformSetProjection(const C44Matrix& matrix);
    virtual void Draw(CGxBatch* batch, int32_t count);
    virtual void PoolSizeSet(CGxPool* pool, uint32_t size);
    virtual void PoolDestroy(CGxPool* pool);
    virtual char* BufLock(CGxBuf* buf);
    virtual int32_t BufUnlock(CGxBuf* buf, uint32_t size);
    virtual void BufData(CGxBuf* buf, const void* data, uint32_t size, uint32_t offset);
    virtual void TexDestroy(CGxTex* texId);
    virtual int32_t TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane);
    virtual int32_t TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face);
    virtual void QueryCreate(CGxQuery*& query, EGxQueryType type);
    virtual void QueryDestroy(CGxQuery*& query);
    virtual bool QueryBegin(CGxQuery* query);
    virtual bool QueryEnd(CGxQuery* query);
    virtual bool QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data);
    virtual bool QueryGetData(CGxQuery* query, uint32_t& data);
    virtual void ShaderCreate(CGxShader** shaders, EGxShTarget target, const char* path, const char* name, uint32_t permutations);
    virtual void ShaderDestroy(CGxShader*& shader);
    virtual void IShaderReload(CGxShader* shader, const char* path, const char* name);
    virtual void IShaderCreate(CGxShader* shader);
    virtual void CursorSetVisible(int32_t visible);
    virtual void CursorUnlock(uint32_t x, uint32_t y);
    virtual void ICursorCreate(const CGxFormat& format);
    virtual void ICursorDestroy();
    virtual void ICursorDraw();
    virtual void StereoSetConvergence(float convergence);
    virtual float StereoGetConvergence();
    virtual void StereoSetSeparation(float separation);
    virtual float StereoGetSeparation();
    virtual bool StereoEnabled();
    virtual void DeviceUnk81(uint32_t a, uint32_t b);
    virtual void DeviceUnk82();
    virtual void DeviceUnk83(uint32_t a, uint32_t b);

    // Window and device.
    void* m_hwnd = nullptr;
    // The hardware cursor: an SDL colour cursor made from the client's cursor image.
    void* m_sdlCursor = nullptr;
    int32_t m_hwCursorDirty = 1;
    int32_t m_ownhwnd = 0;
    IDXGIFactory1* m_factory = nullptr;
    ID3D11Device* m_device = nullptr;
    ID3D11Device1* m_device1 = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;
    ID3D11DeviceContext1* m_ctx1 = nullptr;
    IDXGISwapChain* m_swapChain = nullptr;
    uint32_t m_backBufferWidth = 0;
    uint32_t m_backBufferHeight = 0;
    uint32_t m_samples = 1;

    // The back buffer the client draws to (multisampled when asked for, resolved into the swap chain's buffer).
    ID3D11Texture2D* m_backColor = nullptr;
    ID3D11RenderTargetView* m_backRtv = nullptr;
    ID3D11Texture2D* m_backDepth = nullptr;
    ID3D11DepthStencilView* m_backDsv = nullptr;
    ID3D11ShaderResourceView* m_backDepthSrv = nullptr;
    // A single-sampled copy of the back buffer, kept for the copies that read it (IResolveBack).
    ID3D11Texture2D* m_resolveColor = nullptr;
    ID3D11ShaderResourceView* m_resolveSrv = nullptr;
    ID3D11SamplerState* m_stretchSampler = nullptr;
    ID3D11PixelShader* m_depthCopyPs = nullptr;
    ID3D11PixelShader* m_clearDepthPs = nullptr;
    ID3D11DepthStencilState* m_clearDepthState = nullptr;
    // Clip planes for shader draws (Shaders/ClipPlanes.hlsl): bound while planes are on and a client shader draws
    // triangles.
    struct ID3D11GeometryShader* m_clipGs = nullptr;
    ID3D11Buffer* m_clipGsConstants = nullptr;
    void* m_hwGs = nullptr;
    bool m_clipGsDirty = true;
    bool m_drawTriangles = true;
    // A depth buffer for drawing into textures without their own.
    // Depth buffers for texture targets without one, one per size (the effects switch sizes many times a frame).
    struct RtDepth {
        ID3D11Texture2D* m_texture = nullptr;
        ID3D11DepthStencilView* m_dsv = nullptr;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        uint32_t m_used = 0;
    };
    RtDepth m_rtDepths[8];
    // Frame statistics: draws and the time spent in them and in presenting, printed every 5 s.
    // The registers as the GPU has them (constant buffer 0 of each stage): only the dirty ones are copied in from
    // CGxDevice::s_shadowConstants, which ShaderConstantsClear fills with FLT_MAX after every frame.
    float m_gpuConstants[2][256 * 4] = {};
    uint64_t m_statDraws = 0;
    uint64_t m_statDrawNs = 0;
    uint64_t m_statPresentNs = 0;
    uint64_t m_statBufNs = 0;            // locking and unlocking buffers (uploads included)
    uint64_t m_statUploadBytes = 0;
    uint64_t m_statTexNs = 0;            // making and filling textures
    uint64_t m_statTexCreates = 0;
    uint64_t m_statTexUploads = 0;
    uint64_t m_statTargetNs = 0;         // binding render targets
    uint32_t m_statFrames = 0;
    uint64_t m_statStartNs = 0;
    uint32_t m_rtDepthClock = 0;
    ID3D11DepthStencilView* m_rtDsv = nullptr;
    // DeviceSetSampledDepth: stands in for the back buffer's depth buffer.
    CGxTex* m_sampledDepth = nullptr;
    ID3D11RenderTargetView* m_curRtv = nullptr;
    ID3D11DepthStencilView* m_curDsv = nullptr;
    ID3D11DepthStencilView* m_savedDsv = nullptr;
    bool m_targetIsTexture = false;
    bool m_cullFlip = false;

    // Shaders and constants.
    ID3D11Buffer* m_constants[2] = {};        // vertex, pixel: the Direct3D 9 constant registers
    ID3D11Buffer* m_ffpConstants = nullptr;
    ID3D11Buffer* m_blitConstants = nullptr;
    ID3D11VertexShader* m_ffpVs = nullptr;
    ID3D11PixelShader* m_ffpPs = nullptr;
    ID3D11VertexShader* m_blitVs = nullptr;
    ID3D11PixelShader* m_blitPs = nullptr;
    Shader m_ffpVsInfo;
    ID3D11Buffer* m_zeroBuffer = nullptr;     // inputs a vertex format lacks read zeroes from it (stride 0)
    ID3D11Buffer* m_fanIndices = nullptr;     // triangle fans become lists
    uint32_t m_fanIndicesSize = 0;
    CGxShader* m_boundVs = nullptr;
    CGxShader* m_boundPs = nullptr;
    void* m_hwVs = nullptr;
    void* m_hwPs = nullptr;
    bool m_ffpDirty = true;
    bool m_vertexDirty = true;
    int32_t m_ambientOnlyMode = 0;

    // State objects, cached by their description.
    std::unordered_map<uint64_t, ID3D11BlendState*> m_blendStates;
    std::unordered_map<uint64_t, ID3D11DepthStencilState*> m_depthStates;
    std::unordered_map<uint64_t, ID3D11RasterizerState*> m_rasterStates;
    std::unordered_map<uint64_t, ID3D11SamplerState*> m_samplers;
    std::unordered_map<uint64_t, ID3D11InputLayout*> m_inputLayouts;
    uint32_t m_blendMode = 0;
    uint32_t m_colorMask = 0xF;
    float m_blendFactor = 1.0f;
    uint32_t m_depthFunc = 0;
    bool m_depthWrite = true;
    uint32_t m_cull = 0;
    float m_depthBias = 0.0f;
    bool m_scissor = false;
    bool m_multisample = true;
    bool m_wireframe = false;
    bool m_stateDirty = true;
    uint64_t m_hwBlend = ~0ull;
    uint64_t m_hwDepth = ~0ull;
    uint64_t m_hwRaster = ~0ull;

    // Textures as bound.
    CGxTex* m_boundTextures[16] = {};
    bool m_texturesDirty = true;

    // Fixed-function state, as the Direct3D 9 device kept it in its render states.
    uint32_t m_colorOp[8] = {};
    uint32_t m_alphaOp[8] = {};
    int32_t m_texGenMode[8] = {};
    uint32_t m_texCoord[8] = {};
    uint32_t m_texTransform[8] = {};
    C44Matrix m_texMatrix[8];
    int32_t m_lighting = 0;
    int32_t m_fog = 0;
    int32_t m_alphaRef = 0;
    int32_t m_specularEnable = 0;
    int32_t m_normalizeNormals = 0;
    uint32_t m_ambient = 0;
    uint32_t m_fogColor = 0;
    float m_fogStart = 0.0f;
    float m_fogEnd = 1.0f;
    uint32_t m_clipEnable = 0;

    int32_t ICreateWindow(CGxFormat& format);
    int32_t ICreateD3d();
    int32_t ICreateD3dDevice(const CGxFormat& format);
    void IDestroyD3d();
    int32_t ICreateBackBuffers();
    void IReleaseBackBuffers();
    void ISetCaps(const CGxFormat& format);
    void ISetWindowFocus(int32_t focus);
    void IStateSetDefaults();
    void IReleaseResources(int32_t freeTextures);
    void ITexCreate(CGxTex* texId);
    void ITexUpload(CGxTex* texId);
    void ITexForceRecreation(int32_t all);
    void ICreateRtDepth(uint32_t width, uint32_t height);
    void IBindTargets();
    ID3D11RenderTargetView* ITexRtv(CGxTex* texId, uint32_t plane);
    void IStateSync();
    void IStateInvalidate();
    ID3D11Texture2D* IResolveBack();
    void IStateSyncStates();
    void IStateSyncShaders();
    void IStateSyncTextures();
    void IStateSyncVertexPtrs();
    void IStateSyncScissorRect();
    void IXformSetViewport();
    void IShaderConstantsFlush();
    void IFfpSync();
    void ISetTexGen(uint32_t tmu, int32_t texGen);
    void IXformSetTex(uint32_t tmu);
    ID3D11SamplerState* ISampler(CGxTex* texId, bool comparison);
    void IShaderCreateVertex(CGxShader* shader);
    void IShaderCreatePixel(CGxShader* shader);
    void IShaderRelease(CGxShader* shader);
    void IReleaseShaders();
    void IReleasePool(CGxPool* pool);
    Pool* IPool(CGxPool* pool);
    void IUploadPool(CGxPool* pool, uint32_t offset, uint32_t size);
    bool IShadersActive();
};

CGxDevice* CreateCGxDeviceD3d11();
int32_t D3d11AdapterFormats(TSGrowableArray<CGxFormat>& formats);
int32_t D3d11Available();

#endif
