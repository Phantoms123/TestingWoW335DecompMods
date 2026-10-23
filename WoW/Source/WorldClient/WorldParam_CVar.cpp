#include "WorldParam.h"
#include <cstdio>
#include <new>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Services/GfxSingletonManager.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/ShaderEffect/ShadowCache.h"
#include "Engine/Source/ShaderEffect/SunShafts.h"
#include "WoW/Source/WorldClient/ShadowQuery.h"
#include "WoW/Source/LiquidSystem/CataWater.h"
#include "WoW/Common/AaBsp.h"
#include "WoW/Source/UIUtil/PortraitButton.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/Types.h"
#include "WoW/Source/WorldClient/World.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ShaderEffect/PostEffects.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/ConsoleDevice/ConsoleDevice.h"

// STATIC: 0x00D1C414
static uint32_t s_mapObjLightLOD;

// STATIC: 0x00CD856C
CVar* CWorldParam::cvar_maxLights;

// STATIC: 0x00CD8570
CVar* CWorldParam::cvar_lod;

// STATIC: 0x00CD8574
CVar* CWorldParam::cvar_mapShadows;

// STATIC: 0x00CD8578
CVar* CWorldParam::cvar_shadowLevel;

// STATIC: 0x00CD857C
CVar* CWorldParam::cvar_texLodBias;

// STATIC: 0x00CD8580
CVar* CWorldParam::cvar_farClip;

// STATIC: 0x00CD8584
CVar* CWorldParam::cvar_nearClip;

// STATIC: 0x00CD858C
CVar* CWorldParam::cvar_specular;

// STATIC: 0x00CD8590
CVar* CWorldParam::cvar_mapObjLightLOD;

// STATIC: 0x00CD8594
CVar* CWorldParam::cvar_particleDensity;

// STATIC: 0x00CD8598
CVar* CWorldParam::cvar_waterLOD;

// STATIC: 0x00CD859C
CVar* CWorldParam::cvar_baseMip;

// STATIC: 0x00CD85A0
CVar* CWorldParam::cvar_horizonFarClip;

// STATIC: 0x00CD85A4
CVar* CWorldParam::cvar_horizonNearClip;

// STATIC: 0x00CD85A8
CVar* CWorldParam::cvar_showFootprints;

// STATIC: 0x00CD85AC
CVar* CWorldParam::cvar_bspCache;

// STATIC: 0x00CD85B0
CVar* CWorldParam::cvar_footstepBias;

// STATIC: 0x00CD85B4
CVar* CWorldParam::cvar_occlusion;

// STATIC: 0x00CD85B8
CVar* CWorldParam::cvar_worldPoolUsage;

// STATIC: 0x00CD85BC
CVar* CWorldParam::cvar_groundEffectDensity;

// STATIC: 0x00CD85C0
CVar* CWorldParam::cvar_groundEffectDist;

// STATIC: 0x00CD85C4
CVar* CWorldParam::cvar_terrainAlphaBitDepth;

// STATIC: 0x00CD85C8
CVar* CWorldParam::cvar_farClipOverride;

// STATIC: 0x00CD85CC
CVar* CWorldParam::cvar_objectFade;

// STATIC: 0x00CD85D0
CVar* CWorldParam::cvar_objectFadeZFill;

// STATIC: 0x00CD85D4
CVar* CWorldParam::cvar_environmentDetail;

// STATIC: 0x00CD85D8
CVar* CWorldParam::cvar_hwPCF;

// STATIC: 0x00CD85DC
CVar* CWorldParam::cvar_extShadowQuality;

// STATIC: 0x00CD85E0
CVar* CWorldParam::cvar_projectedTextures;

// STATIC: 0x00CD85E4
CVar* CWorldParam::cvar_gxTextureCacheSize;

// VERIFY: 54f5415a manual
// OFFSET: 0x004023C0
bool CWorldParam::TextureFilteringCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    int32_t mode = SStrToInt(value);

    if (mode >= 0 && mode < 6) {
        return true;
    }

    char buffer[256];
    SStrPrintf(buffer, sizeof(buffer), "Texture filtering mode must be in range 0 to %d.", 6);
    ConsoleWrite(buffer, DEFAULT_COLOR);

    return false;
}

// VERIFY: 14adfded auto
// OFFSET: 0x0078D610
bool CWorldParam::LodCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        ConsoleWrite("Terrain LOD enabled.", DEFAULT_COLOR);
        CWorld::enables |= Enable_Lod;
    } else {
        ConsoleWrite("Terrain LOD disabled.", DEFAULT_COLOR);
        CWorld::enables &= ~Enable_Lod;
    }

    return true;
}

// VERIFY: fb93d195 auto
// OFFSET: 0x0078D660
bool CWorldParam::MapShadowsCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        ConsoleWrite("Terrain shadows enabled.", DEFAULT_COLOR);
        CWorld::enables |= Enable_Shadow;
    } else {
        ConsoleWrite("Terrain shadows disabled.", DEFAULT_COLOR);
        CWorld::enables &= ~Enable_Shadow;
    }

    return true;
}

// VERIFY: 4ef1e46f auto
// OFFSET: 0x0078D6B0
bool CWorldParam::MaxLightsCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    uint32_t lights = SStrToInt(value);

    if (lights < 1 || lights > 4) {
        ConsoleWriteA("MaxLights must be in range 1 - %i.", DEFAULT_COLOR, 4);

        return false;
    }

    CWorld::maxLights = lights;

    return true;
}

// VERIFY: bbb3c156 auto
// OFFSET: 0x0078D6F0
bool CWorldParam::ShadowLevelCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value) > 1) {
        ConsoleWrite("Shadow mip level must be in range 0 - 1.", DEFAULT_COLOR);

        return false;
    }

    ConsoleWrite("Shadow mip level changed upon restart.", DEFAULT_COLOR);

    return true;
}

// VERIFY: 964fa6bd auto
// OFFSET: 0x0078D730
bool CWorldParam::TexLodBiasCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float bias = SStrToFloat(value);

    if (bias < -1.0f || bias > 1.0f) {
        ConsoleWrite("TexLodBias must be in range -1.0 - 1.0.", DEFAULT_COLOR);

        return false;
    }

    World::SetTexLodBias(bias);

    return true;
}

// VERIFY: b4a11982 auto
// OFFSET: 0x0078D780
bool CWorldParam::FarClipCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    World::SetFarClip(SStrToFloat(value));

    return true;
}

// VERIFY: b99f7609 auto
// OFFSET: 0x0078D7A0
bool CWorldParam::NearClipCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    World::SetNearClip(SStrToFloat(value));

    return true;
}

// VERIFY: 452fa715 auto
// OFFSET: 0x0078D7C0
bool CWorldParam::HorizonFarClipScaleCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float scale = SStrToFloat(value);

    if (scale < 3.0f) {
        scale = 3.0f;
    } else if (scale > 6.0f) {
        scale = 6.0f;
    }

    World::SetHorizonFarClipScale(scale);

    return true;
}

// VERIFY: bd34982b auto
// OFFSET: 0x0078D810
bool CWorldParam::HorizonNearClipScaleCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float scale = SStrToFloat(value);

    if (scale < 0.01f) {
        scale = 0.01f;
    } else if (scale > 1.0f) {
        scale = 1.0f;
    }

    World::SetHorizonNearClipScale(scale);

    return true;
}

// VERIFY: 7db07d04 auto
// OFFSET: 0x0078D860
bool CWorldParam::ParticleDensityCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float density = SStrToFloat(value);

    // CHANGED: also takes the percent 4.3.4's video options write (10-100).
    if (density > 1.0f) {
        density *= 0.01f;
    }

    if (density < 0.1f - 0.0001f || density > 1.0f + 0.0001f) {
        ConsoleWrite("Value must be between 0.1 and 1.0 (or 10 and 100).", DEFAULT_COLOR);

        return false;
    }

    ParticleSystemManager::SetScaler(density);

    return true;
}

// VERIFY: aafd8987 auto
// OFFSET: 0x0078D8B0
bool CWorldParam::WaterLodCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    World::s_waterLOD = 0;

    if (SStrToInt(value)) {
        ConsoleWrite("waterLOD fixed to 0", DEFAULT_COLOR);

        return false;
    }

    return true;
}

// VERIFY: 6626e5c9 auto
// OFFSET: 0x0078D8F0
bool CWorldParam::ShowFootprintsCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        ConsoleWrite("Showing foot prints.", DEFAULT_COLOR);
        CWorld::enables |= 0x400;
    } else {
        ConsoleWrite("Hiding foot prints.", DEFAULT_COLOR);
        CWorld::enables &= ~0x400;
    }

    return true;
}

// VERIFY: 127cd82e manual
// OFFSET: 0x0078D940
bool CWorldParam::FootstepBiasCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float bias;
    sscanf(value, "%f", &bias);

    if (bias < 0.0f || bias > 1.0f) {
        ConsoleWrite("Footstep bias must be in the range (0, 1)", DEFAULT_COLOR);

        return false;
    }

    char buffer[256];
    sprintf(buffer, "Footstep bias set to %f", bias);
    ConsoleWrite(buffer, DEFAULT_COLOR);

    return true;
}

// VERIFY: a8041b4a manual
// OFFSET: 0x0078D9D0
bool CWorldParam::HardwareOcclusionCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        ConsoleWrite("Using hardware occlusion test.", DEFAULT_COLOR);
    } else {
        ConsoleWrite("Disabled hardware occlusion test.", DEFAULT_COLOR);
    }

    return true;
}

// VERIFY: 8f56c0f0 auto
// OFFSET: 0x0078DA10
bool CWorldParam::WorldPoolUsageCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrCmpI(value, "Dynamic", STORM_MAX_STR)) {
        ConsoleWrite("WorldPoolUsage must be Stream, Dynamic or Static.", DEFAULT_COLOR);

        return false;
    }

    ConsoleWrite("WorldPoolUsage set on restart.", DEFAULT_COLOR);

    return true;
}

// VERIFY: b80c920b auto
// OFFSET: 0x0078DA50
bool CWorldParam::TerrainAlphaBitDepthCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    uint32_t depth = SStrToInt(value);

    if (depth == 4 || depth == 8) {
        char buffer[256];
        sprintf(buffer, "Alpha map bit depth set to %dbit on restart.", depth);
        ConsoleWrite(buffer, DEFAULT_COLOR);

        return true;
    }

    ConsoleWrite("Alpha map bit depth must be 4 or 8.", DEFAULT_COLOR);

    return false;
}

// VERIFY: 38a0f503 auto
// OFFSET: 0x0078DAB0
bool CWorldParam::GroundEffectDensityCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    int32_t density = SStrToInt(value);

    if (density < 16 || density > 256) {
        char buffer[256];
        sprintf(buffer, "Ground effect density must be in range %d to %d.", 16, 256);
        ConsoleWriteA(buffer, DEFAULT_COLOR);

        return false;
    }

    World::SetDetailDoodadDensity(density);

    return true;
}

// VERIFY: 7685b115 auto
// OFFSET: 0x0078DB10
bool CWorldParam::GroundEffectDistCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float dist = SStrToFloat(value);

    // CHANGED: up to 260 yards, as 4.3.4's video options allow (was 140).
    if (dist < 0.0f || dist > 260.0f) {
        char buffer[256];
        sprintf(buffer, "Ground effect distance must be in range %0f to %0f.", 0.0, 260.0);
        ConsoleWriteA(buffer, DEFAULT_COLOR);

        return false;
    }

    World::SetDetailDoodadDist(dist);

    return true;
}

// VERIFY: 4551a2de auto
// OFFSET: 0x0078DB90
bool CWorldParam::ObjectFadeCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        ConsoleWrite("Object distance fade enabled.", DEFAULT_COLOR);
        CWorld::enables |= 0x4000;
    } else {
        ConsoleWrite("Object distance fade disabled.", DEFAULT_COLOR);
        CWorld::enables &= ~0x4000;
    }

    return true;
}

// VERIFY: 2278f1b3 auto
// OFFSET: 0x0078DBE0
bool CWorldParam::ObjectFadeZFillCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        ConsoleWrite("Object distance fade ZFill pass enabled.", DEFAULT_COLOR);
        CWorld::enables |= 0x8000;
    } else {
        ConsoleWrite("Object distance fade ZFill pass disabled.", DEFAULT_COLOR);
        CWorld::enables &= ~0x8000;
    }

    return true;
}

// VERIFY: 1c43c03c manual
// OFFSET: 0x0078DC30
bool CWorldParam::FarClipOverrideCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value) > 1) {
        ConsoleWrite("farClipOverride must be 0 or 1.", DEFAULT_COLOR);

        return false;
    }

    return true;
}

// VERIFY: 82f0dd4a auto
// OFFSET: 0x0078DC60
bool CWorldParam::EnvironmentDetailCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float detail = SStrToFloat(value);

    // CHANGED: also takes the percent 4.3.4's video options write (50-150).
    if (detail > 1.5f) {
        detail *= 0.01f;
    }

    if (detail < 0.5f) {
        detail = 0.5f;
    } else if (detail > 1.5f) {
        detail = 1.5f;
    }

    CWorldScene::ScaleFadeDist(detail);

    return true;
}

// CHANGED: 4.3.4's shadowMode (see RegisterCataShadows).
static CVar* s_shadowModeVar;
static bool s_applyingShadowMode;

// VERIFY: 782f4c26 manual
// OFFSET: 0x0078DCB0
bool CWorldParam::ExtShadowQualityCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    uint32_t mode = SStrToInt(value);
    ConsoleWrite(CShadowCache::GetShadowModeDesc(mode), DEFAULT_COLOR);

    if (!CShadowCache::ValidateShadowMode(static_cast<EShadowMode>(mode))) {
        return false;
    }

    CShadowCache::SetShadowMode(static_cast<EShadowMode>(mode));

    // CHANGED: setting extShadowQuality itself (the video options do) hands shadows back from shadowMode to it.
    if (!s_applyingShadowMode && s_shadowModeVar && s_shadowModeVar->m_intValue >= 0) {
        s_shadowModeVar->Set("-1", true, false, false, true);
    }

    return true;
}

// VERIFY: 8b8fa7e3 auto
// OFFSET: 0x0078DCF0
bool CWorldParam::ProjectedTexturesCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        ConsoleWrite("Projected textures enabled.", DEFAULT_COLOR);
        CWorld::enables2 |= 4;
    } else {
        ConsoleWrite("Projected textures disabled.", DEFAULT_COLOR);
        CWorld::enables2 &= ~4;
    }

    return true;
}

// VERIFY: c1590fd6 manual
// OFFSET: 0x0078DE60
bool CWorldParam::SpecularCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (!SStrToInt(value)) {
        ConsoleWrite("Specular disabled on restart.", DEFAULT_COLOR);
        CWorld::enables &= ~Enable_Specular;

        return true;
    }

    if (g_theGxDevicePtr->Caps().m_shaderTargets[GxSh_Pixel] <= 0) {
        ConsoleWrite("Specular not enabled.  Requires pixel shaders.", DEFAULT_COLOR);

        return false;
    }

    ConsoleWrite("Specular enabled on restart.", DEFAULT_COLOR);
    CWorld::enables |= Enable_Specular;

    return true;
}

// VERIFY: 677c9e7a manual
// OFFSET: 0x0078DED0
bool CWorldParam::MapObjLightLODCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    uint32_t lod = SStrToInt(value);

    if (lod > 2) {
        ConsoleWrite("MapObjLightLOD must be 0-2", DEFAULT_COLOR);

        return false;
    }

    s_mapObjLightLOD = lod;

    return true;
}

// VERIFY: b09da230 auto
// OFFSET: 0x0078DF00
bool CWorldParam::BaseMipCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    uint32_t level = SStrToInt(value);

    if (level == g_theGxDevicePtr->DeviceBaseMipLevel()) {
        return true;
    }

    if (level <= 1) {
        g_theGxDevicePtr->DeviceSetBaseMipLevel(level);
        TextureNotifyGxRestart();
        UpdateAllPortraits();
        World::s_texCacheDirty = 1;

        char buffer[256];
        SStrPrintf(buffer, sizeof(buffer), "BaseMip level changed to %d", level);
        ConsoleWrite(buffer, DEFAULT_COLOR);

        return true;
    }

    ConsoleWrite("BaseMip must be 0 or 1", DEFAULT_COLOR);

    return false;
}

// VERIFY: b0d9b0b5 manual
// OFFSET: 0x0078DF90
bool CWorldParam::BspCacheCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        if (g_BspDigestCache) {
            ConsoleWrite("Enabling BSP node cache (already enabled, so clearing content.)", DEFAULT_COLOR);
            g_BspDigestCache->Reset();
        } else {
            ConsoleWrite("Enabling BSP node cache (first time - starting up)", DEFAULT_COLOR);
            void* mem = SMemAlloc(sizeof(CAaBspDigestCache), __FILE__, __LINE__, 0);
            g_BspDigestCache = mem ? new (mem) CAaBspDigestCache() : nullptr;
        }
    } else if (g_BspDigestCache) {
        ConsoleWrite("Disabling BSP node cache.", DEFAULT_COLOR);
        CAaBspDigestCache* cache = g_BspDigestCache;

        if (cache) {
            cache->Reset();
            delete cache;
        }

        g_BspDigestCache = nullptr;
    } else {
        ConsoleWrite("BSP node cache already disabled.", DEFAULT_COLOR);
    }

    return true;
}

// VERIFY: 6a5bcdbc manual
// OFFSET: 0x0078E070
bool CWorldParam::HWPCFCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    uint32_t enable = SStrToInt(value);
    uint32_t previous = CWorld::enables2;

    if (enable) {
        if (!g_theGxDevicePtr->Caps().m_rttFormat[12]) {
            ConsoleWrite("Hardware PCF not supported by this graphics card.", DEFAULT_COLOR);

            return false;
        }

        ConsoleWrite("Hardware PCF enabled.", DEFAULT_COLOR);
        CWorld::enables2 |= Enable_HwPcf;
    } else {
        ConsoleWrite("Hardware PCF disabled.", DEFAULT_COLOR);
        CWorld::enables2 &= ~Enable_HwPcf;
    }

    if (previous != CWorld::enables2) {
        CShaderEffect::SetUsePcfFiltering((CWorld::enables2 & Enable_HwPcf) != 0);
        CShadowCache::NotifyTexturesRecreate();
        CMap::InitializePCFShaders();
    }

    return true;
}

// VERIFY: 685d5b59 auto
// OFFSET: 0x0078E110
bool CWorldParam::GxTextureCacheSizeCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    uint32_t size = SStrToInt(value);

    if (size) {
        World::s_texCacheOverride = size << 20;
        g_theGxDevicePtr->TexSetCacheSize(size << 20);

        char buffer[256];
        sprintf(buffer, "Texture cache size set to %dMB.", size);
        ConsoleWrite(buffer, DEFAULT_COLOR);
    } else {
        World::s_texCacheOverride = 0;
        World::s_texCacheDirty = 1;
        ConsoleWrite("Texture cache size set to default.", DEFAULT_COLOR);
    }

    return true;
}

// CHANGED: not in the original client. 4.3.4's sunShafts setting: 0 off, 1 low, 2 high.
// CHANGED: 4.3.4's water settings (see WoW/Source/LiquidSystem/CataWater.h).
static bool WaterDetailCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    int32_t detail = SStrToInt(value);

    if (detail < 0 || detail > 3) {
        ConsoleWrite("waterDetail: 0 and 1 the 3.3.5 water, 2 the 4.3.4 water, 3 the 4.3.4 water with real reflections (see reflectionMode)", DEFAULT_COLOR);
        return false;
    }

    Liquid::Cata::SetWaterDetail(static_cast<uint32_t>(detail));
    return true;
}

static bool RippleDetailCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    int32_t detail = SStrToInt(value);

    if (detail < 0 || detail > 2) {
        return false;
    }

    Liquid::Cata::SetRippleDetail(static_cast<uint32_t>(detail));
    return true;
}

static bool ReflectionModeCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    int32_t mode = SStrToInt(value);

    if (mode < 0 || mode > 3) {
        return false;
    }

    Liquid::Cata::SetReflectionMode(static_cast<uint32_t>(mode));
    return true;
}

static bool ReflectionDownscaleCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    int32_t downscale = SStrToInt(value);

    if (downscale < 0 || downscale > 4) {
        return false;
    }

    Liquid::Cata::SetReflectionDownscale(static_cast<uint32_t>(downscale));
    return true;
}

// CHANGED: the Advanced Effects settings (see Engine/Source/ShaderEffect/PostEffects.h), with Waystone's names.
static bool PostToggleCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    bool enable = SStrToInt(value) != 0;

    switch (reinterpret_cast<uintptr_t>(arg)) {
        case 0:
            PostEffects::SetAmbientOcclusion(enable);
            break;

        case 1:
            PostEffects::SetBloom(enable);
            break;

        case 2:
            PostEffects::SetAntiAliasing(enable);
            break;

        case 3:
            PostEffects::SetAerialFog(enable);
            break;
    }

    return true;
}

static bool PostParamCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    PostEffects::SetParam(static_cast<PostEffects::EParam>(reinterpret_cast<uintptr_t>(arg)), SStrToFloat(value));
    return true;
}

static void RegisterPostEffects() {
    static const struct {
        const char* name;
        const char* help;
        uintptr_t which;
    } toggles[] = {
        { "ambientOcclusion", "Screen space ambient occlusion (0 off, 1 on)", 0 },
        { "postBloom", "Bloom and filmic tone mapping (0 off, 1 on)", 1 },
        { "postAA", "FXAA edge anti-aliasing with contrast adaptive sharpening (0 off, 1 on)", 2 },
        { "aerialFog", "Height based distance haze (0 off, 1 on)", 3 },
    };

    for (const auto& toggle : toggles) {
        void* arg = reinterpret_cast<void*>(toggle.which);
        CVar* var = CVar::Register(toggle.name, toggle.help, 1, "0", PostToggleCallback, CATEGORY_GRAPHICS, false, arg, false);

        if (var) {
            PostToggleCallback(var, nullptr, var->m_stringValue.m_str, arg);
        }
    }

    static const struct {
        const char* name;
        const char* help;
        const char* value;
        PostEffects::EParam param;
    } params[] = {
        { "aoRadius", "Ambient occlusion radius in yards", "4.0", PostEffects::Param_AoRadius },
        { "aoStrength", "Ambient occlusion strength", "1.8", PostEffects::Param_AoStrength },
        { "aoDebug", "Show the ambient occlusion buffer", "0", PostEffects::Param_AoDebug },
        { "bloomIntensity", "Bloom intensity", "0.45", PostEffects::Param_BloomIntensity },
        { "bloomThreshold", "Bloom brightness threshold", "0.6", PostEffects::Param_BloomThreshold },
        { "toneMapping", "Highlight roll-off amount (0-1)", "1.0", PostEffects::Param_ToneMapping },
        { "toneExposure", "Tone mapping exposure", "1.0", PostEffects::Param_ToneExposure },
        { "colorSaturation", "Color saturation", "1.05", PostEffects::Param_ColorSaturation },
        { "colorContrast", "Contrast around mid grey (1 = unchanged)", "1.0", PostEffects::Param_ColorContrast },
        { "sharpen", "Contrast adaptive sharpening amount (0 off, up to 1)", "0.4", PostEffects::Param_Sharpen },
        { "aerialFogDensity", "Distance haze density", "0.0015", PostEffects::Param_AerialFogDensity },
        { "aerialFogFalloff", "Distance haze height falloff (1/yards)", "0.0067", PostEffects::Param_AerialFogFalloff },
        { "aerialFogStart", "Distance haze start distance in yards", "80", PostEffects::Param_AerialFogStart },
        { "aerialFogMax", "Distance haze maximum opacity", "0.7", PostEffects::Param_AerialFogMax },
    };

    for (const auto& param : params) {
        void* arg = reinterpret_cast<void*>(static_cast<uintptr_t>(param.param));
        CVar* var = CVar::Register(param.name, param.help, 1, param.value, PostParamCallback, CATEGORY_GRAPHICS, false, arg, false);

        if (var) {
            PostParamCallback(var, nullptr, var->m_stringValue.m_str, arg);
        }
    }
}

static bool ReflectionStrengthCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    Liquid::Cata::SetReflectionStrength(SStrToFloat(value));
    return true;
}

static bool ReflectionBlendCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    Liquid::Cata::SetReflectionPerPixel(SStrToInt(value) != 0);
    return true;
}

static bool ReflectionPlanesCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    Liquid::Cata::SetReflectionPlanes(static_cast<uint32_t>(SStrToInt(value)));
    return true;
}

static bool ReflectionDebugCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    Liquid::Cata::SetReflectionDebug(SStrToInt(value) != 0);
    return true;
}

static bool ShorelineSoftnessCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    Liquid::Cata::SetShorelineSoftness(SStrToFloat(value));
    return true;
}

static bool ReflectionModelsCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    Liquid::Cata::SetReflectionModels(SStrToInt(value) != 0);
    return true;
}

static bool ReflectionCullHeightCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    Liquid::Cata::SetReflectionCullHeight(SStrToFloat(value));
    return true;
}

static bool ReflectionModelSizeCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    Liquid::Cata::SetReflectionModelSize(SStrToFloat(value));
    return true;
}

static bool ReflectionDistanceCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    Liquid::Cata::SetReflectionDistance(SStrToFloat(value));
    return true;
}

// CHANGED: 4.3.4's shadow settings. 4.3.4 kept 3.3.5's shadow cache and renamed its settings: shadowMode 0-3
// with shadowTextureSize 1024 or 2048 picks one of extShadowQuality's six levels. shadowMode -1 (the default)
// leaves extShadowQuality (the 3.3.5 setting, and the video options) in charge.
static const char* s_cataShadowModeDescs[4] = {
    "0 - Precomputed terrain shadows, blob shadows.",
    "1 - Precomputed terrain shadows, dynamic shadows near player.",
    "2 - Static environment shadows, dynamic shadows near player.",
    "3 - Full dynamic shadows.",
};

static bool ApplyCataShadowMode(int32_t mode) {
    if (mode < 0 || mode > 3 || !CWorldParam::cvar_extShadowQuality) {
        return mode < 0;
    }

    bool lowRes = CShadowCache::s_sizeOverride == 1024;
    int32_t quality = 0;

    switch (mode) {
        case 1:
            quality = lowRes ? SHADOW_LOW : SHADOW_MEDIUM;
            break;
        case 2:
            quality = lowRes ? SHADOW_MEDIUM_HIGH : SHADOW_HIGH;
            break;
        case 3:
            quality = SHADOW_VERY_HIGH;
            break;
    }

    if (!CShadowCache::ValidateShadowMode(static_cast<EShadowMode>(quality))) {
        return false;
    }

    s_applyingShadowMode = true;
    CWorldParam::cvar_extShadowQuality->Set(quality, true, false, false, true);
    s_applyingShadowMode = false;

    return true;
}

static bool ShadowModeCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    int32_t mode = SStrToInt(value);

    if (mode < 0) {
        return true;
    }

    if (mode > 3 || !ApplyCataShadowMode(mode)) {
        ConsoleWrite("Unsupported shadow mode.", DEFAULT_COLOR);
        return false;
    }

    char buffer[256];
    SStrPrintf(buffer, sizeof(buffer), "Shadow mode changed to %s", s_cataShadowModeDescs[mode]);
    ConsoleWrite(buffer, DEFAULT_COLOR);

    return true;
}

static bool ShadowTextureSizeCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    int32_t size = SStrToInt(value);

    if (size != 0 && size != 1024 && size != 2048 && size != 4096) {
        ConsoleWrite("Unsupported shadow texture size.", DEFAULT_COLOR);
        return false;
    }

    CShadowCache::SetTextureSizeOverride(static_cast<uint32_t>(size));

    if (s_shadowModeVar && s_shadowModeVar->m_intValue >= 0) {
        ApplyCataShadowMode(s_shadowModeVar->m_intValue);
    }

    char buffer[256];
    SStrPrintf(buffer, sizeof(buffer), "Shadow texture size changed to %d.", size);
    ConsoleWrite(buffer, DEFAULT_COLOR);

    return true;
}

static bool ShadowSoftnessCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    CShadowCache::SetSoftness(SStrToFloat(value));
    return true;
}

static bool ShadowFollowSunCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    CShadowQuery::s_followSun = SStrToInt(value) != 0;
    CShadowCache::s_needsRebuild = 1;
    return true;
}

static bool ShadowStrengthCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float strength = SStrToFloat(value);
    CGxDevice::s_shadowStrength = strength < 0.1f ? 0.1f : (strength > 0.8f ? 0.8f : strength);
    CGxDevice::s_shadowStrengthChanged = true;
    return true;
}

static void RegisterCataShadows() {
    CVar* strength = CVar::Register("shadowStrength", "How dark shadows and unlit sides get (0.3 as 3.3.5, 0.45 default, 0.1-0.8; Direct3D 11)", 1, "0.45", ShadowStrengthCallback, CATEGORY_GRAPHICS, false, nullptr, false);

    if (strength) {
        float value = strength->m_floatValue;
        CGxDevice::s_shadowStrength = value < 0.1f ? 0.1f : (value > 0.8f ? 0.8f : value);
    }

    CVar* size = CVar::Register("shadowTextureSize", "Shadow texture size (1024, 2048 or 4096; 0 by shadow quality)", 1, "0", ShadowTextureSizeCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    int32_t sizeValue = size ? size->m_intValue : 0;
    CShadowCache::SetTextureSizeOverride(sizeValue == 1024 || sizeValue == 2048 || sizeValue == 4096 ? static_cast<uint32_t>(sizeValue) : 0);

    CVar* softness = CVar::Register("shadowSoftness", "Shadow edge softness (1 = original, 0.25-6)", 1, "1.0", ShadowSoftnessCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CShadowCache::SetSoftness(softness ? softness->m_floatValue : 1.0f);

    CVar* followSun = CVar::Register("shadowFollowSun", "Shadows follow the sun and moon position (0 off as 3.3.5, 1 on)", 1, "0", ShadowFollowSunCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CShadowQuery::s_followSun = followSun && followSun->m_intValue != 0;

    s_shadowModeVar = CVar::Register("shadowMode", "Quality of shadows, as 4.3.4 (0-3; -1 extShadowQuality decides)", 1, "-1", ShadowModeCallback, CATEGORY_GRAPHICS, false, nullptr, false);

    if (s_shadowModeVar && s_shadowModeVar->m_intValue >= 0) {
        ApplyCataShadowMode(s_shadowModeVar->m_intValue);
    }
}

static bool SunShaftsCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    int32_t mode = SStrToInt(value);
    SunShafts::SetMode(mode > 0 ? static_cast<uint32_t>(mode) : 0);
    return true;
}

// VERIFY: 9acf82a1 auto
// OFFSET: 0x0078E400
void CWorldParam::Initialize() {
    CWorldParam::cvar_farClipOverride = CVar::Register("farClipOverride", "Override old world graphic settings", 1, "0", CWorldParam::FarClipOverrideCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_lod = CVar::Register("lod", "Video option: Toggle Lod", 1, "0", CWorldParam::LodCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_mapShadows = CVar::Register("mapShadows", "Video option: Toggle map shadows", 1, "1", CWorldParam::MapShadowsCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_maxLights = CVar::Register("MaxLights", "Max number of hardware lights", 1, "4", CWorldParam::MaxLightsCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_shadowLevel = CVar::Register("shadowLevel", "Terrain shadow map mip level", 1, "1", CWorldParam::ShadowLevelCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_texLodBias = CVar::Register("texLodBias", "Texture LOD Bias", 1, "0.0", CWorldParam::TexLodBiasCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_farClip = CVar::Register("farclip", "Far clip plane distance", 1, "350", CWorldParam::FarClipCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_nearClip = CVar::Register("nearclip", "Near clip plane distance", 1, "0.2", CWorldParam::NearClipCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_specular = CVar::Register("specular", "Specularity", 1, "0", CWorldParam::SpecularCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_mapObjLightLOD = CVar::Register("mapObjLightLOD", "Map object light LOD", 1, "0", CWorldParam::MapObjLightLODCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_particleDensity = CVar::Register("particleDensity", "Video option: Particle density", 1, "1.0", CWorldParam::ParticleDensityCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_waterLOD = CVar::Register("waterLOD", "Water geometry LOD", 1, "0", CWorldParam::WaterLodCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_baseMip = CVar::Register("baseMip", "base mipmap level", 1, "0", CWorldParam::BaseMipCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_horizonFarClip = CVar::Register("horizonFarclipScale", "Far clip plane scale for horizon", 1, "4.0", CWorldParam::HorizonFarClipScaleCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_horizonNearClip = CVar::Register("horizonNearclipScale", "Near clip plane scale for horizon", 1, "0.7", CWorldParam::HorizonNearClipScaleCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_showFootprints = CVar::Register("showfootprints", "toggles rendering of footprints", 1, "1", CWorldParam::ShowFootprintsCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    // CHANGED: sun shafts from 4.3.4; needs its shaders in Shaders\\Cata (see COMBINED.md).
    CVar* sunShafts = CVar::Register("sunShafts", "Sun shafts (0 off, 1 low, 2 high); needs the 4.x shaders in Shaders\\Cata", 1, "0", SunShaftsCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    SunShafts::SetMode(sunShafts && sunShafts->m_intValue > 0 ? static_cast<uint32_t>(sunShafts->m_intValue) : 0);
    // CHANGED: 4.3.4's water settings; needs the 4.x shaders in Shaders\Cata and 4.3.4's LiquidType.dbc values.
    CVar* waterDetail = CVar::Register("waterDetail", "Water detail (0, 1 the 3.3.5 water; 2, 3 the 4.3.4 water)", 1, "2", WaterDetailCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    Liquid::Cata::SetWaterDetail(waterDetail ? static_cast<uint32_t>(waterDetail->m_intValue) : 2);
    CVar* rippleDetail = CVar::Register("rippleDetail", "Water ripple detail with the 4.3.4 water (0 the 3.3.5 ripples, 1 4.3.4 ripples, 2 finer 4.3.4 ripples)", 1, "1", RippleDetailCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    Liquid::Cata::SetRippleDetail(rippleDetail ? static_cast<uint32_t>(rippleDetail->m_intValue) : 1);
    CVar* reflectionMode = CVar::Register("reflectionMode", "Water reflections with waterDetail 3 (0, 1 sky; 2 sky and terrain; 3 sky, terrain and buildings)", 1, "3", ReflectionModeCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    Liquid::Cata::SetReflectionMode(reflectionMode ? static_cast<uint32_t>(reflectionMode->m_intValue) : 3);
    CVar* reflectionStrength = CVar::Register("reflectionStrength", "How strongly the water's real reflections show (0-1)", 1, "0.75", ReflectionStrengthCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    Liquid::Cata::SetReflectionStrength(reflectionStrength ? reflectionStrength->m_floatValue : 0.75f);
    CVar* shorelineSoftness = CVar::Register("shorelineSoftness", "How much softer the 4.3.4 water's shorelines get with distance (0 as 4.3.4, 1 default, up to 4)", 1, "1", ShorelineSoftnessCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    Liquid::Cata::SetShorelineSoftness(shorelineSoftness ? shorelineSoftness->m_floatValue : 1.0f);
    CVar* reflectionPlanes = CVar::Register("reflectionPlanes", "Real water reflections drawn a frame with reflectionBlend 1: 1, or 2 (a second for the nearest water at another level)", 1, "2", ReflectionPlanesCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    Liquid::Cata::SetReflectionPlanes(reflectionPlanes ? static_cast<uint32_t>(reflectionPlanes->m_intValue) : 2u);
    CVar* reflectionBlend = CVar::Register("reflectionBlend", "Experimental per-pixel water reflection blend (0 off as before, 1 on)", 1, "0", ReflectionBlendCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    Liquid::Cata::SetReflectionPerPixel(reflectionBlend && reflectionBlend->m_intValue != 0);
    CVar::Register("reflectionDebug", "Print the water reflection's level to the terminal (0 off, 1 on)", 1, "0", ReflectionDebugCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CVar* reflectionModels = CVar::Register("reflectionModels", "Reflect trees, characters and creatures in the water with reflectionMode 3 (0 off, 1 on)", 1, "1", ReflectionModelsCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    Liquid::Cata::SetReflectionModels(!reflectionModels || reflectionModels->m_intValue != 0);
    CVar* reflectionDistance = CVar::Register("reflectionDistance", "How far the water's real reflections show, in yards (they fade out from a third of it on; 0 no fade)", 1, "200", ReflectionDistanceCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    Liquid::Cata::SetReflectionDistance(reflectionDistance ? reflectionDistance->m_floatValue : 200.0f);
    CVar* reflectionModelSize = CVar::Register("reflectionModelSize", "Models smaller than this part of their distance are left out of the water's reflections (0 none, default 0.004)", 1, "0.004", ReflectionModelSizeCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    Liquid::Cata::SetReflectionModelSize(reflectionModelSize ? reflectionModelSize->m_floatValue : 0.004f);
    CVar* reflectionCullHeight = CVar::Register("reflectionCullHeight", "How many screens' height above the view the world is also drawn for the water's reflections (0-4, default 2)", 1, "2", ReflectionCullHeightCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    Liquid::Cata::SetReflectionCullHeight(reflectionCullHeight ? reflectionCullHeight->m_floatValue : 2.0f);
    CVar* reflectionDownscale = CVar::Register("reflectionDownscale", "Water reflection texture downscale (0-4)", 1, "0", ReflectionDownscaleCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    Liquid::Cata::SetReflectionDownscale(reflectionDownscale ? static_cast<uint32_t>(reflectionDownscale->m_intValue) : 0);
    RegisterPostEffects();
    CWorldParam::cvar_bspCache = CVar::Register("bspcache", "BSP node caching", 1, "1", CWorldParam::BspCacheCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_footstepBias = CVar::Register("footstepBias", "Unit footstep depth bias", 1, "0.125", CWorldParam::FootstepBiasCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_occlusion = CVar::Register("occlusion", "Use hardware occlusion test", 1, "1", CWorldParam::HardwareOcclusionCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_worldPoolUsage = CVar::Register("worldPoolUsage", "CGxPool usage static/dynamic", 1, "Dynamic", CWorldParam::WorldPoolUsageCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_terrainAlphaBitDepth = CVar::Register("terrainAlphaBitDepth", "Terrain alpha map bit depth", 1, "8", CWorldParam::TerrainAlphaBitDepthCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_groundEffectDensity = CVar::Register("groundEffectDensity", "Ground effect density", 1, "16", CWorldParam::GroundEffectDensityCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_groundEffectDist = CVar::Register("groundEffectDist", "Ground effect dist", 1, "70.0", CWorldParam::GroundEffectDistCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_objectFade = CVar::Register("objectFade", "Fade objects into view", 1, "1", CWorldParam::ObjectFadeCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_objectFadeZFill = CVar::Register("objectFadeZFill", "Fade objects using ZFill pass ", 1, "0", CWorldParam::ObjectFadeZFillCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_environmentDetail = CVar::Register("environmentDetail", "Environment detail", 1, "1.0", CWorldParam::EnvironmentDetailCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_hwPCF = CVar::Register("hwPCF", "Hardware PCF Filtering", 1, "1", CWorldParam::HWPCFCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_extShadowQuality = CVar::Register("extShadowQuality", "Quality of exterior shadows (0-5)", 1, "0", CWorldParam::ExtShadowQualityCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_projectedTextures = CVar::Register("projectedTextures", "Projected Textures", 1, "0", CWorldParam::ProjectedTexturesCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_gxTextureCacheSize = CVar::Register("gxTextureCacheSize", "GX Texture Cache Size", 1, "0", CWorldParam::GxTextureCacheSizeCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CVar::Register("spellEffectLevel", "Video Option: Spell Effects", 1, "9", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    RegisterCataShadows();

    AddConsoleDeviceDefaultCallback(CWorldParam::SetDefaults);

    if (ConsoleDeviceHardwareChanged()) {
        for (uint32_t page = OPTION_PAGE_0; page < NUM_OPTION_PAGES; page++) {
            CWorldParam::SetDefaults(static_cast<OPTION_PAGE>(page));
        }
    }

    CVar* videoOptionsVersion = CVar::Lookup("videoOptionsVersion");

    if (!videoOptionsVersion || videoOptionsVersion->m_intValue < 2) {
        CWorldParam::SetDefaultParticleDensity();
        CWorldParam::SetDefaultProjectedTextures();
    }
}
