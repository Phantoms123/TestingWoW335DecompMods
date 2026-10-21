#include "engine.h"

#include "client.h"
#include "log.h"
#include "water_classify.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine
{
namespace
{
constexpr float kMinWorldDepthRangeSpan = 0.01f;
constexpr int kClassifiedSettingsCacheSize = 32;
constexpr int kMaxLoggedLiquidTypes = 64;
constexpr size_t kLiquidSettingsTextureSlot = 0x80;

struct OpaqueState
{
    bool valid;
    D3DVIEWPORT9 viewport;
    float cameraRelativeView[16];
    float glProjection[16];
};

OpaqueState g_opaque = {};

bool Finite(const float* v, int count)
{
    for (int i = 0; i < count; ++i)
        if (!std::isfinite(v[i]))
            return false;
    return true;
}

bool IsPerspective(const float* p)
{
    return Finite(p, 16) && std::fabs(p[11] - 1.0f) < 1e-3f && std::fabs(p[15]) < 1e-3f &&
           std::fabs(p[0]) > 1e-4f && std::fabs(p[5]) > 1e-4f;
}

bool ReadGxMatrices(float* view, float* proj)
{
    return client::GxViewMatrix(view) && client::GxProjectionMatrix(proj) && Finite(view, 16) && IsPerspective(proj);
}

bool IsWorldDepthRange(const float* minMaxZ)
{
    return Finite(minMaxZ, 2) && minMaxZ[0] >= 0.0f && minMaxZ[1] <= 1.0f &&
           minMaxZ[1] - minMaxZ[0] > kMinWorldDepthRangeSpan;
}

void ApplyPendingGxViewportDepthRange(D3DVIEWPORT9& viewport)
{
    float minMaxZ[2];
    if (client::GxViewportDepthRange(minMaxZ) && IsWorldDepthRange(minMaxZ))
    {
        viewport.MinZ = minMaxZ[0];
        viewport.MaxZ = minMaxZ[1];
    }
}

bool UsableOpaqueState(const OpaqueState& state)
{
    return state.viewport.Width > 0 && state.viewport.Height > 0 && IsPerspective(state.glProjection) &&
           Finite(state.cameraRelativeView, 16);
}

bool CaptureOpaqueStateFrom(IDirect3DDevice9* device, OpaqueState& state)
{
    if (FAILED(device->GetViewport(&state.viewport)))
        return false;
    ApplyPendingGxViewportDepthRange(state.viewport);
    if (!ReadGxMatrices(state.cameraRelativeView, state.glProjection))
        client::WorldSceneMatrices(state.cameraRelativeView, state.glProjection);
    return UsableOpaqueState(state);
}

void Normalize(float* v)
{
    float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len < 1e-6f)
    {
        v[0] = 0.0f;
        v[1] = 0.0f;
        v[2] = 1.0f;
        return;
    }
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
}

bool GlowRunsIn(const client::ScreenEffects& e)
{
    return e.ffxEnabled && e.current && e.current == e.glow && e.currentEnabled;
}

float GlowScreenEffectAmount(float glow)
{
    if (!GlowRunsIn(client::ReadScreenEffects()) || !std::isfinite(glow))
        return 0.0f;
    return std::clamp(glow, 0.0f, 1.0f);
}

LightParamsSelection LightParamsSelectionOf(const client::DayNightState& dn)
{
    LightParamsSelection selection;
    selection.stormBlend = std::isfinite(dn.stormBlend) ? std::clamp(dn.stormBlend, 0.0f, 1.0f) : 0.0f;
    if (dn.screenEffectLightSlot >= 0 && dn.screenEffectLightSlot < FogData::kLightParamsSlots)
        selection.screenEffectSlot = dn.screenEffectLightSlot;
    return selection;
}

bool LightIsMoon(float dayFraction)
{
    return !(dayFraction >= client::kSunRisesAt && dayFraction <= client::kSunSetsAt);
}

float FarClipOf(const float* glProjection)
{
    float farClip = glProjection[14] / (1.0f - glProjection[10]);
    if (!(farClip > 10.0f && farClip < 100000.0f))
        farClip = client::WorldFrameFarClip();
    if (!(farClip > 10.0f && farClip < 100000.0f))
        farClip = 1000.0f;
    return farClip;
}

struct LiquidDescription
{
    bool inBank;
    bool hasRow;
    uint32_t id;
    client::LiquidType row;
    char texture0[kLiquidSettingsTextureSlot];
};

struct ClassifiedSettings
{
    const void* settings;
    WaterClass waterClass;
};

struct ClassifiedSettingsCache
{
    uint32_t bankCount;
    int count;
    int next;
    ClassifiedSettings entries[kClassifiedSettingsCacheSize];
};

ClassifiedSettingsCache g_classified = {};
uint32_t g_loggedLiquidTypes[kMaxLoggedLiquidTypes] = {};
int g_loggedLiquidTypeCount = 0;
bool g_loggedUnbankedSettings = false;

bool FindLiquidTypeId(const void* settings, uint32_t& id)
{
    const uint32_t count = client::LiquidSettingsCount();
    for (uint32_t i = 0; i < count; ++i)
        if (client::LiquidSettingsAt(i) == settings)
        {
            id = i;
            return true;
        }
    return false;
}

void ReadTexture0(const void* settings, char* out)
{
    std::memcpy(out, settings, kLiquidSettingsTextureSlot);
    if (!std::memchr(out, 0, kLiquidSettingsTextureSlot))
        out[0] = 0;
}

void DescribeLiquid(const void* settings, LiquidDescription& d)
{
    d.inBank = FindLiquidTypeId(settings, d.id);
    if (!d.inBank)
        return;
    d.hasRow = client::LiquidTypeRow(d.id, d.row);
    ReadTexture0(settings, d.texture0);
}

WaterClass ClassOf(const LiquidDescription& d)
{
    if (!d.inBank || !d.hasRow)
        return WaterClass::None;
    return ClassifyLiquid(d.id, d.row.soundBank, d.row.materialId, d.texture0);
}

void ForgetClassifiedSettings(uint32_t bankCount)
{
    g_classified = {};
    g_classified.bankCount = bankCount;
}

const ClassifiedSettings* FindClassified(const void* settings)
{
    for (int i = 0; i < g_classified.count; ++i)
        if (g_classified.entries[i].settings == settings)
            return &g_classified.entries[i];
    return nullptr;
}

void RememberClassified(const void* settings, WaterClass waterClass)
{
    const int slot = g_classified.count < kClassifiedSettingsCacheSize ? g_classified.count++ : g_classified.next;
    g_classified.next = (slot + 1) % kClassifiedSettingsCacheSize;
    g_classified.entries[slot] = {settings, waterClass};
}

bool MarkLiquidTypeLogged(uint32_t id)
{
    for (int i = 0; i < g_loggedLiquidTypeCount; ++i)
        if (g_loggedLiquidTypes[i] == id)
            return false;
    if (g_loggedLiquidTypeCount < kMaxLoggedLiquidTypes)
        g_loggedLiquidTypes[g_loggedLiquidTypeCount++] = id;
    return true;
}

void LogNewLiquid(const void* settings, const LiquidDescription& d, WaterClass waterClass)
{
    if (!d.inBank)
    {
        if (!g_loggedUnbankedSettings)
            VF_LOG_INFO("water: liquid settings %p are not in the client's settings bank; left to the client",
                        settings);
        g_loggedUnbankedSettings = true;
        return;
    }
    if (!MarkLiquidTypeLogged(d.id))
        return;
    if (!d.hasRow)
        VF_LOG_INFO("water: liquid type %u -> %s (no LiquidType row)", d.id, WaterClassLabel(waterClass));
    else
        VF_LOG_INFO("water: liquid type %u -> %s (sound bank %u, material %u, flags 0x%X, texture %s)", d.id,
                    WaterClassLabel(waterClass), d.row.soundBank, d.row.materialId, d.row.flags, d.texture0);
}
}

bool IsSupportedClient()
{
    return true;
}

void* GameD3DDevice()
{
    return client::D3DDevice();
}

bool CameraInLiquid()
{
    return client::CameraInLiquid();
}

ScreenEffects ReadScreenEffects()
{
    const client::ScreenEffects e = client::ReadScreenEffects();
    ScreenEffects effects;
    effects.current = e.current;
    effects.glow = e.glow;
    effects.death = e.death;
    return effects;
}

bool GlowScreenEffectRuns()
{
    return GlowRunsIn(client::ReadScreenEffects());
}

bool GlowPassColourLayoutMatches()
{
    return true;
}

bool GradingPlacementMatches()
{
    return true;
}

int ForeverLookGuardRanges(CodeRange*, int)
{
    return 0;
}

bool FindGlowCompositePasses(const ScreenEffects& effects, GlowCompositePasses& out)
{
    if (!effects.glow || effects.current != effects.glow)
        return false;
    for (int list = 0; list < kGlowPassLists; ++list)
        if (!client::GlowCompositePass(effects.glow, list, out.pass[list]))
            return false;
    return true;
}

bool ReadGlowByte(uintptr_t pass, uint8_t& value)
{
    const uint8_t* alpha = client::GlowPassColourAlpha(pass);
    if (!alpha)
        return false;
    value = *alpha;
    return true;
}

bool WriteGlowByte(uintptr_t pass, uint8_t value)
{
    uint8_t* alpha = client::GlowPassColourAlpha(pass);
    if (!alpha)
        return false;
    *alpha = value;
    return true;
}

bool ReadClassicLightInputs(ClassicLightInputs& out)
{
    const client::DayNightState dn = client::ReadDayNight();
    out.mapId = client::CurrentMapId();
    client::CameraPosition(out.camPos);
    out.dayFraction = dn.dayFraction;
    out.lightParams = LightParamsSelectionOf(dn);
    return Finite(out.camPos, 3) && std::isfinite(out.dayFraction);
}

StockFog ReadStockFog()
{
    StockFog fog;
    for (int group = 0; group < kDayNightFogGroupCount; ++group)
    {
        const client::FogGroup g = client::ReadFogGroup(group);
        fog.start[group] = g.start;
        fog.end[group] = g.end;
    }
    return fog;
}

void WriteStockFog(const StockFog& fog)
{
    for (int group = 0; group < kDayNightFogGroupCount; ++group)
        client::WriteFogRange(group, fog.start[group], fog.end[group]);
}

void CaptureOpaqueState(IDirect3DDevice9* device)
{
    OpaqueState state = {};
    state.valid = CaptureOpaqueStateFrom(device, state);
    g_opaque = state;
}

void CaptureOpaqueState(IDirect3DDevice9* device, const float* cameraRelativeView, const float* glProjection)
{
    OpaqueState state = {};
    std::memcpy(state.cameraRelativeView, cameraRelativeView, sizeof(state.cameraRelativeView));
    std::memcpy(state.glProjection, glProjection, sizeof(state.glProjection));
    state.valid = SUCCEEDED(device->GetViewport(&state.viewport)) && UsableOpaqueState(state);
    g_opaque = state;
}

bool HasOpaqueState()
{
    return g_opaque.valid;
}

bool OpaqueViewport(D3DVIEWPORT9& out)
{
    if (!g_opaque.valid)
        return false;
    out = g_opaque.viewport;
    return true;
}

void ClearOpaqueState()
{
    g_opaque.valid = false;
}

bool BuildFrameInputs(FrameInputs& out, bool withPointLights, const PointLightUpload& upload)
{
    if (!g_opaque.valid)
        return false;
    std::memcpy(out.cameraRelativeView, g_opaque.cameraRelativeView, sizeof(out.cameraRelativeView));
    std::memcpy(out.glProjection, g_opaque.glProjection, sizeof(out.glProjection));
    out.viewport = g_opaque.viewport;
    client::CameraPosition(out.camPos);
    client::CameraTarget(out.camTarget);

    const client::DayNightState dn = client::ReadDayNight();
    out.dayFraction = dn.dayFraction;
    out.lightIsMoon = LightIsMoon(out.dayFraction);
    const float* body = out.lightIsMoon ? dn.moon : dn.sun;
    for (int i = 0; i < 3; ++i)
        out.toLight[i] = body[i] - dn.skyCentre[i];
    Normalize(out.toLight);

    const client::FogGroup fog = client::ReadFogGroup(kFrameInputsFogGroup);
    out.fogColor = fog.colour;
    out.fogStart = fog.start;
    out.fogEnd = fog.end;
    out.sunColor = dn.band.sun;
    out.directColor = dn.band.diffuse;
    out.ambientColor = dn.band.ambient;
    out.inLiquid = CameraInLiquid();
    out.mapId = client::CurrentMapId();
    out.lightParams = LightParamsSelectionOf(dn);
    CaptureLocalLightInputs(out.camPos, withPointLights, upload, out.localLights);

    out.zoneFogDistance = dn.band.zoneFogDistance;
    out.clientGlowAmount = out.inLiquid ? 0.0f : GlowScreenEffectAmount(dn.band.glow);
    out.farClip = FarClipOf(out.glProjection);

    return Finite(out.camPos, 3) && Finite(out.toLight, 3) && std::isfinite(out.fogEnd) &&
           std::isfinite(out.fogStart);
}

bool BuildWaterInputs(WaterInputs& out)
{
    out = {};
    const client::LightBand band = client::ReadDayNight().band;
    for (int i = 0; i < kSkyColorCount; ++i)
        out.skyColors[i] = band.sky[i];
    out.riverColors[0] = band.riverClose;
    out.riverColors[1] = band.riverFar;
    out.oceanColors[0] = band.oceanClose;
    out.oceanColors[1] = band.oceanFar;
    return true;
}

bool TransparentLiquidsQueued(const void* liquidRenderer)
{
    return client::TransparentLiquidsQueued(liquidRenderer);
}

bool WaterClientLayoutMatches()
{
    return true;
}

bool TransparentFogClientLayoutMatches()
{
    return true;
}

void DescribeGlarePassEntry(char* text, size_t size)
{
    std::snprintf(text, size, "called directly by the client");
}

WaterClass ClassifyWaterSettings(const void* liquidSettings)
{
    if (!liquidSettings)
        return WaterClass::None;
    const uint32_t bankCount = client::LiquidSettingsCount();
    if (bankCount != g_classified.bankCount)
        ForgetClassifiedSettings(bankCount);
    if (const ClassifiedSettings* known = FindClassified(liquidSettings))
        return known->waterClass;
    LiquidDescription description = {};
    DescribeLiquid(liquidSettings, description);
    const WaterClass waterClass = ClassOf(description);
    RememberClassified(liquidSettings, waterClass);
    LogNewLiquid(liquidSettings, description, waterClass);
    return waterClass;
}
}
