#include "client.h"

#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/FFX/EffectGlow.h"
#include "Engine/Source/FFX/FFX.h"
#include "Engine/Source/FFX/PassGlow.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDeviceD3d/CGxDeviceD3d.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/LiquidTypeRec.h"
#include "WoW/Source/LiquidSystem/Liquid.h"
#include "WoW/Source/Object/ObjectClient/Object_C.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapEntity.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/MapObjGroup.h"
#include "WoW/Source/WorldClient/World.h"
#include "WoW/Source/WorldClient/WorldScene.h"

#include <cstring>

namespace client
{
namespace
{
constexpr uint32_t kPointLightGridSize = 4096;
constexpr uint32_t kMaxPointLightsPerCell = 512;
constexpr uint32_t kWmoExteriorFogFlags = 0x48;
constexpr uint32_t kWmoGroupLoaded = 0x1;
constexpr uint32_t kSwimmingMovementFlag = 0x200000;
constexpr uint32_t kTransparentLiquidPass = 1;
constexpr int kGlowPassLists = 2;
constexpr uint32_t kGlowCompositePassIndex = 2;

void CopyVector(const C3Vector& v, float out[3])
{
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

uint32_t Packed(const CImVector& colour)
{
    static_assert(sizeof(CImVector) == sizeof(uint32_t), "CImVector layout");
    uint32_t value;
    std::memcpy(&value, &colour, sizeof(value));
    return value;
}

void CopyMatrix(const C44Matrix& m, float out[16])
{
    static_assert(sizeof(C44Matrix) == 16 * sizeof(float), "C44Matrix layout");
    std::memcpy(out, &m, sizeof(C44Matrix));
}

DNFogInfo& FogGroupInfo(int group)
{
    return group == 0 ? DayNight::g_dnInfo.m_fog : DayNight::g_dnInfo.m_fogInterior;
}

const TSGrowableArray<FFX::Pass*>* GlowPassList(EffectGlow* effect, int list)
{
    return list == 0 ? &effect->m_passes : &effect->m_altPasses;
}

struct UnitWalk
{
    UnitVisitor visit;
    void* context;
};

int32_t VisitVisibleUnit(WOWGUID guid, void* param)
{
    UnitWalk* walk = static_cast<UnitWalk*>(param);
    CGObject_C* object = ClntObjMgrObjectPtr(guid, TYPE_UNIT, __FILE__, __LINE__);
    CGUnit_C* unit = static_cast<CGUnit_C*>(object);
    if (!unit || !object->m_worldObject)
        return 1;
    const CMovement_C& movement = unit->m_movementData;
    const CMapEntity* entity = object->m_worldObject;
    Unit u = {};
    u.guid = guid.m_guid;
    u.onTransport = movement.m_transportGuid.m_guid != 0;
    CopyVector(movement.m_position, u.position);
    CopyVector(entity->m_position, u.entityPosition);
    u.entityLiquidFlags = entity->m_staticFlags;
    u.liquidSurface = entity->m_liquidHeight;
    u.collisionRadius = movement.m_collisionRadius;
    u.collisionHeight = movement.m_collisionHeight;
    u.movementFlags = movement.m_flags;
    walk->visit(u, walk->context);
    return 1;
}
}

void* D3DDevice()
{
    CGxDevice* device = g_theGxDevicePtr;
    if (!device || device->DeviceApi() != GxApi_D3d9)
        return nullptr;
    return static_cast<CGxDeviceD3d*>(device)->m_d3dDevice;
}

bool GxViewMatrix(float out[16])
{
    if (!g_theGxDevicePtr)
        return false;
    C44Matrix view;
    GxXformView(view);
    CopyMatrix(view, out);
    return true;
}

bool GxProjectionMatrix(float out[16])
{
    if (!g_theGxDevicePtr)
        return false;
    CopyMatrix(g_theGxDevicePtr->m_projection, out);
    return true;
}

bool GxViewportDepthRange(float out[2])
{
    if (!g_theGxDevicePtr)
        return false;
    out[0] = g_theGxDevicePtr->m_viewport.m_z.m_l;
    out[1] = g_theGxDevicePtr->m_viewport.m_z.m_h;
    return true;
}

void WorldSceneMatrices(float view[16], float projection[16])
{
    CopyMatrix(CWorldScene::viewMatrix, view);
    CopyMatrix(CWorldScene::projMatrix, projection);
}

void CameraPosition(float out[3])
{
    CopyVector(CWorldScene::s_activeWorldView, out);
}

void CameraTarget(float out[3])
{
    CopyVector(CWorldScene::camTarget, out);
}

bool CameraInLiquid()
{
    return s_liquidTypeId != 0;
}

int32_t CurrentMapId()
{
    return s_mapId;
}

float WorldFrameFarClip()
{
    return World::s_farClip;
}

DayNightState ReadDayNight()
{
    const DayNight::DNInfo& info = DayNight::g_dnInfo;
    const DayNight::CurrentLight& band = info.m_bands;
    DayNightState s = {};
    s.dayFraction = info.m_dayProgression;
    CopyVector(info.m_cameraPos, s.skyCentre);
    CopyVector(DayNight::g_planets.m_sun.m_position, s.sun);
    CopyVector(DayNight::g_planets.m_moon.m_position, s.moon);
    s.stormBlend = info.m_stormBlend;
    s.screenEffectLightSlot = static_cast<int32_t>(info.m_overrideLightParamsId);
    s.interiorFogBlend = info.m_interiorFogBlend;
    s.band.ambient = Packed(band.m_ambient);
    s.band.diffuse = Packed(band.m_diffuse);
    s.band.sun = Packed(band.m_sunCloudColors[0]);
    for (int i = 0; i < 6; ++i)
        s.band.sky[i] = Packed(band.m_skyColors[i]);
    s.band.oceanClose = Packed(band.m_waterColors[0]);
    s.band.oceanFar = Packed(band.m_waterColors[1]);
    s.band.riverClose = Packed(band.m_waterColors[2]);
    s.band.riverFar = Packed(band.m_waterColors[3]);
    s.band.zoneFogDistance = band.m_fogEnd;
    s.band.glow = band.m_glow;
    return s;
}

FogGroup ReadFogGroup(int group)
{
    const DNFogInfo& fog = FogGroupInfo(group);
    return {Packed(fog.m_color), fog.m_start, fog.m_end};
}

void WriteFogRange(int group, float start, float end)
{
    DNFogInfo& fog = FogGroupInfo(group);
    fog.m_start = start;
    fog.m_end = end;
}

ScreenEffects ReadScreenEffects()
{
    ScreenEffects e = {};
    e.current = reinterpret_cast<uintptr_t>(FFX::s_effect);
    e.glow = reinterpret_cast<uintptr_t>(CGWorldFrame::ffxEffectGlow);
    e.death = reinterpret_cast<uintptr_t>(CGWorldFrame::ffxEffectDeath);
    e.ffxEnabled = FFX::s_cvarFFX && FFX::s_cvarFFX->m_intValue != 0;
    e.currentEnabled = FFX::s_effect && FFX::s_effect->m_cvar && FFX::s_effect->m_cvar->m_intValue != 0;
    return e;
}

bool GlowCompositePass(uintptr_t glowEffect, int list, uintptr_t& pass)
{
    if (!glowEffect || glowEffect != reinterpret_cast<uintptr_t>(CGWorldFrame::ffxEffectGlow) || list < 0 ||
        list >= kGlowPassLists)
        return false;
    EffectGlow* effect = reinterpret_cast<EffectGlow*>(glowEffect);
    const TSGrowableArray<FFX::Pass*>* passes = GlowPassList(effect, list);
    if (passes->m_count <= kGlowCompositePassIndex || !passes->m_data[kGlowCompositePassIndex])
        return false;
    pass = reinterpret_cast<uintptr_t>(static_cast<PassGlowBase*>(passes->m_data[kGlowCompositePassIndex]));
    return true;
}

uint8_t* GlowPassColourAlpha(uintptr_t pass)
{
    return pass ? &reinterpret_cast<PassGlowBase*>(pass)->m_color.a : nullptr;
}

uint32_t LiquidSettingsCount()
{
    return Liquid::CSettingsBank::s_settings.Count();
}

const void* LiquidSettingsAt(uint32_t index)
{
    return index < LiquidSettingsCount() ? Liquid::CSettingsBank::s_settings.m_data[index] : nullptr;
}

bool LiquidTypeRow(uint32_t id, LiquidType& out)
{
    const LiquidTypeRec* rec = g_liquidTypeDB.GetRecord(static_cast<int32_t>(id));
    if (!rec)
        return false;
    out.flags = static_cast<uint32_t>(rec->m_flags);
    out.soundBank = static_cast<uint32_t>(rec->m_soundBank);
    out.materialId = static_cast<uint32_t>(rec->m_materialID);
    return true;
}

bool TransparentLiquidsQueued(const void* liquidRenderer)
{
    if (!liquidRenderer)
        return false;
    const Liquid::CRenderer* renderer = static_cast<const Liquid::CRenderer*>(liquidRenderer);
    return renderer->m_instances[kTransparentLiquidPass].m_count != 0;
}

bool VisitPointLights(PointLightVisitor visit, void* context)
{
    CM2Scene* scene = World::s_m2Scene;
    if (!scene || !scene->m_lightGrid)
        return true;
    for (uint32_t cell = 0; cell < kPointLightGridSize; ++cell)
    {
        uint32_t nodes = 0;
        for (CM2Light* light = scene->m_lightGrid[cell]; light; light = light->m_lightNext)
        {
            if (++nodes > kMaxPointLightsPerCell)
                return false;
            PointLight p = {};
            p.id = reinterpret_cast<uintptr_t>(light);
            p.type = static_cast<uint32_t>(light->m_type);
            p.enabled = light->m_visible;
            CopyVector(light->m_pos, p.position);
            CopyVector(light->m_dirColor, p.colour);
            p.attenuation[0] = light->m_constantAttenuation;
            p.attenuation[1] = light->m_linearAttenuation;
            p.attenuation[2] = light->m_quadraticAttenuation;
            if (!visit(p, context))
                return true;
        }
    }
    return true;
}

bool CameraInsideInterior(bool& interior)
{
    interior = false;
    CMapObjDef* def = CWorldScene::s_viewerMapObjDef;
    const TSGrowableArray<uint32_t>& groups = CWorldScene::s_viewerMapObjGroups;
    if (!def || !groups.m_count)
        return true;
    CMapObj* root = def->m_owner;
    if (!root || !root->m_isGroupLoaded)
        return false;
    for (uint32_t i = 0; i < groups.m_count; ++i)
    {
        const uint32_t id = groups.m_data[i];
        if (id >= static_cast<uint32_t>(root->m_mapObjGroupCount))
            return false;
        CMapObjGroup* group = root->m_mapObjGroupArray[id];
        if (!group)
            return false;
        if ((group->m_unkLoadedFlag & kWmoGroupLoaded) && !(group->m_flags & kWmoExteriorFogFlags))
            interior = true;
    }
    return true;
}

bool VisitVisibleUnits(UnitVisitor visit, void* context)
{
    UnitWalk walk = {visit, context};
    ClntObjMgrEnumVisibleObjects(&VisitVisibleUnit, &walk);
    return true;
}

bool UnitIsSwimming(uint32_t movementFlags)
{
    return (movementFlags & kSwimmingMovementFlag) != 0;
}

void RenderGlares()
{
    DayNight::RenderGlares();
}

volatile int32_t* WaterRipplesValue()
{
    return &CWorld::s_waterRipples;
}
}
