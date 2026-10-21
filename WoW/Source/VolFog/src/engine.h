#pragma once

#include <windows.h>
#include <d3d9.h>

#include "fog_data.h"
#include "engine_actors.h"
#include "engine_lights.h"
#include "water_types.h"

#include <cstdint>

struct FrameInputs
{
    float cameraRelativeView[16];
    float glProjection[16];
    float camPos[3];
    float camTarget[3];
    D3DVIEWPORT9 viewport;
    float dayFraction;
    float toLight[3];
    bool lightIsMoon;
    uint32_t fogColor;
    uint32_t sunColor;
    uint32_t directColor;
    uint32_t ambientColor;
    float fogStart;
    float fogEnd;
    float zoneFogDistance;
    float farClip;
    float clientGlowAmount;
    bool inLiquid;
    int mapId;
    LightParamsSelection lightParams;
    LocalLightInputs localLights;
};

constexpr int kSkyColorCount = 6;
constexpr int kWaterColorPair = 2;

struct WaterInputs
{
    uint32_t skyColors[kSkyColorCount];
    uint32_t riverColors[kWaterColorPair];
    uint32_t oceanColors[kWaterColorPair];
    bool stockFogApplies;
    WaterContactFrame contacts;
};

namespace engine
{

constexpr int kGlowPassLists = 2;

struct ScreenEffects
{
    uintptr_t current = 0;
    uintptr_t glow = 0;
    uintptr_t death = 0;
};

struct GlowCompositePasses
{
    uintptr_t pass[kGlowPassLists] = {};
};

struct ClassicLightInputs
{
    int mapId = -1;
    float camPos[3] = {};
    float dayFraction = 0.0f;
    LightParamsSelection lightParams;
};

struct CodeRange
{
    uintptr_t address;
    size_t size;
};

bool IsSupportedClient();
void* GameD3DDevice();
bool CameraInLiquid();

ScreenEffects ReadScreenEffects();
bool GlowScreenEffectRuns();
bool GlowPassColourLayoutMatches();
bool GradingPlacementMatches();
int ForeverLookGuardRanges(CodeRange* out, int capacity);
bool FindGlowCompositePasses(const ScreenEffects& effects, GlowCompositePasses& out);
bool ReadGlowByte(uintptr_t pass, uint8_t& value);
bool WriteGlowByte(uintptr_t pass, uint8_t value);
bool ReadClassicLightInputs(ClassicLightInputs& out);

constexpr int kDayNightFogGroupCount = 2;
constexpr int kFrameInputsFogGroup = 1;

struct StockFog
{
    float start[kDayNightFogGroupCount];
    float end[kDayNightFogGroupCount];
};
StockFog ReadStockFog();
void WriteStockFog(const StockFog& fog);

void CaptureOpaqueState(IDirect3DDevice9* device);
void CaptureOpaqueState(IDirect3DDevice9* device, const float* cameraRelativeView, const float* glProjection);
bool HasOpaqueState();
bool OpaqueViewport(D3DVIEWPORT9& out);
void ClearOpaqueState();

bool BuildFrameInputs(FrameInputs& out, bool withPointLights, const PointLightUpload& upload);
bool BuildWaterInputs(WaterInputs& out);
bool WaterClientLayoutMatches();
bool TransparentFogClientLayoutMatches();
void DescribeGlarePassEntry(char* text, size_t size);
bool TransparentLiquidsQueued(const void* liquidRenderer);
WaterClass ClassifyWaterSettings(const void* liquidSettings);
}
