#pragma once

#include <cstdint>

struct IDirect3D9;

namespace client
{
struct FogGroup
{
    uint32_t colour;
    float start;
    float end;
};

struct LightBand
{
    uint32_t ambient;
    uint32_t diffuse;
    uint32_t sun;
    uint32_t sky[6];
    uint32_t oceanClose;
    uint32_t oceanFar;
    uint32_t riverClose;
    uint32_t riverFar;
    float zoneFogDistance;
    float glow;
};

struct DayNightState
{
    float dayFraction;
    float skyCentre[3];
    float sun[3];
    float moon[3];
    float stormBlend;
    int32_t screenEffectLightSlot;
    float interiorFogBlend;
    LightBand band;
};

constexpr float kSunRisesAt = 0.2013889f;
constexpr float kSunSetsAt = 0.9236111f;

struct ScreenEffects
{
    uintptr_t current;
    uintptr_t glow;
    uintptr_t death;
    bool ffxEnabled;
    bool currentEnabled;
};

struct LiquidType
{
    uint32_t flags;
    uint32_t soundBank;
    uint32_t materialId;
};

struct PointLight
{
    uintptr_t id;
    uint32_t type;
    uint32_t enabled;
    float position[3];
    float colour[3];
    float attenuation[3];
};

struct Unit
{
    uint64_t guid;
    bool onTransport;
    float position[3];
    float entityPosition[3];
    uint32_t entityLiquidFlags;
    float liquidSurface;
    float collisionRadius;
    float collisionHeight;
    uint32_t movementFlags;
};

using PointLightVisitor = bool (*)(const PointLight& light, void* context);
using UnitVisitor = void (*)(const Unit& unit, void* context);

void* D3DDevice();
bool GxViewMatrix(float out[16]);
bool GxProjectionMatrix(float out[16]);
bool GxViewportDepthRange(float out[2]);
void WorldSceneMatrices(float view[16], float projection[16]);
void CameraPosition(float out[3]);
void CameraTarget(float out[3]);
bool CameraInLiquid();
int32_t CurrentMapId();
float WorldFrameFarClip();

DayNightState ReadDayNight();
FogGroup ReadFogGroup(int group);
void WriteFogRange(int group, float start, float end);

ScreenEffects ReadScreenEffects();
bool GlowCompositePass(uintptr_t glowEffect, int list, uintptr_t& pass);
uint8_t* GlowPassColourAlpha(uintptr_t pass);

uint32_t LiquidSettingsCount();
const void* LiquidSettingsAt(uint32_t index);
bool LiquidTypeRow(uint32_t id, LiquidType& out);
bool TransparentLiquidsQueued(const void* liquidRenderer);

bool VisitPointLights(PointLightVisitor visit, void* context);
bool CameraInsideInterior(bool& interior);

bool VisitVisibleUnits(UnitVisitor visit, void* context);
bool UnitIsSwimming(uint32_t movementFlags);

void RenderGlares();
volatile int32_t* WaterRipplesValue();
}
