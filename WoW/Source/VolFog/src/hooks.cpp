#include "hooks.h"

#include "client.h"
#include "client_ripple_sprites.h"
#include "config.h"
#include "d3d9_wrap.h"
#include "engine.h"
#include "fog_model.h"
#include "forever_look.h"
#include "log.h"
#include "transparent_fog.h"
#include "water_classify.h"
#include "water_renderer.h"
#include "status_log.h"

#include "platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
constexpr float kOutOfRangeStockFogStart = 50000.0f;
constexpr DWORD kConfigReloadIntervalMs = 1000;
constexpr int kEasternKingdomsMap = 0;
constexpr int kKalimdorMap = 1;
constexpr int kOutlandMap = 530;
constexpr int kNorthrendMap = 571;
constexpr size_t kDrawnWaterClassesTextSize = 64;
constexpr unsigned kFlatWavesBit = 1u << 31;
constexpr const char* kCameraUnderLiquid = "camera under liquid";
constexpr const char* kWorldNotCaptured = "the world render inputs were not captured";
constexpr const char* kInvalidFrameInputs = "invalid frame inputs";
constexpr DWORD kTransparentFogStatsIntervalMs = 60000;
constexpr size_t kGlarePassEntryTextSize = 64;
constexpr double kPercent = 100.0;

FogDevice* g_liquidDepthWriteDevice = nullptr;
bool g_failed = false;
bool g_engineHooksInstalled = false;
bool g_farClipLifted = false;
bool g_renderedLastFrame = false;
bool g_renderedThisFrame = false;
bool g_stockFogPushed = false;
engine::StockFog g_savedStockFog = {};
bool g_deviceChecked = false;
DWORD g_lastReload = 0;
const char* g_fogNotDrawnReason = "";
StatusLog g_fogLog;
StatusLog g_godRayLog;

int g_m2BatchFogObserved = 0;
bool g_transparentFogHooksInstalled = false;
bool g_transparentFogFailed = false;
StatusLog g_transparentFogLog;

struct TransparentFogFrame
{
    bool inWorld;
    bool pastLiquid;
    bool composited;
    bool armed;
    bool glareDrawn;
    StockFogFit fit;
};

TransparentFogFrame g_transparentFog = {};

struct TransparentFogStats
{
    unsigned frames;
    unsigned earlyComposites;
    unsigned batchesBeforeLiquid;
    unsigned batchesAfterLiquid;
    unsigned batchesOutsideWorld;
    unsigned batchesRewritten;
    unsigned lightingColours;
    unsigned additiveColours;
    unsigned modulateColours;
    unsigned modulate2xColours;
    unsigned otherColours;
    unsigned linearExponents;
    unsigned lowerExponents;
    unsigned higherExponents;
    float lowestExponent;
    float highestExponent;
    unsigned glareDrawnEarly;
    unsigned glareCallsPassed;
    unsigned glareCallsSkipped;
};

TransparentFogStats g_transparentFogStats = {};
DWORD g_transparentFogStatsTicks = 0;
bool g_transparentFogStatsDue = false;

bool g_waterHooksInstalled = false;
bool g_waterFailed = false;
bool g_waterFaultLogged = false;
FogDevice* g_waterPassDevice = nullptr;
FogDevice* g_waterResourcesDevice = nullptr;
bool g_waterPassRanThisFrame = false;
bool g_waterShadedThisFrame = false;
bool g_waterRipplesAvailable = true;
unsigned g_waterClassesThisPass = 0;
WaterFrameStatus g_waterStatus = {false, "waiting for the world to render"};
StatusLog g_waterLog;
unsigned g_drawnWaterClassesTextMask = 0;
char g_drawnWaterClassesText[kDrawnWaterClassesTextSize] = "";

FogDevice* GameFogDevice()
{
    void* game = engine::GameD3DDevice();
    FogDevice* device = WrapperOrLatestFogDevice(game);
    if (!g_deviceChecked && device)
    {
        g_deviceChecked = true;
        if (!IsWrapperOf(device, game))
            VF_LOG_ERROR("the client's D3D device %p is not a fog wrapper; using the latest fog device", game);
    }
    return device;
}

struct WorldClient
{
    FogDevice* (*device)();
    void (*captureOpaqueState)(IDirect3DDevice9* device);
    bool (*worldCaptured)();
    bool (*frameInputs)(FrameInputs& in, bool withPointLights, const PointLightUpload& upload);
    bool (*cameraInLiquid)();
    engine::StockFog (*readStockFog)();
    void (*writeStockFog)(const engine::StockFog& fog);
    void (*endWorld)();
    bool (*glowScreenEffectRuns)();
};

void CaptureGameOpaqueState(IDirect3DDevice9* device)
{
    engine::CaptureOpaqueState(device);
}

const WorldClient kGameWorldClient = {&GameFogDevice,
                                      &CaptureGameOpaqueState,
                                      &engine::HasOpaqueState,
                                      &engine::BuildFrameInputs,
                                      &engine::CameraInLiquid,
                                      &engine::ReadStockFog,
                                      &engine::WriteStockFog,
                                      &engine::ClearOpaqueState,
                                      &engine::GlowScreenEffectRuns};

FrameInputs g_testWorldFrame = {};
bool g_testGlowScreenEffectRuns = false;
engine::StockFog g_testStockFog = {};

void CaptureTestOpaqueState(IDirect3DDevice9* device)
{
    engine::CaptureOpaqueState(device, g_testWorldFrame.cameraRelativeView, g_testWorldFrame.glProjection);
}

bool TestFrameInputsInCapturedViewport(FrameInputs& in, bool, const PointLightUpload&)
{
    D3DVIEWPORT9 viewport = {};
    if (!engine::OpaqueViewport(viewport))
        return false;
    in = g_testWorldFrame;
    in.viewport = viewport;
    return true;
}

bool TestWorldCaptured()
{
    return true;
}

bool TestFrameInputs(FrameInputs& in, bool, const PointLightUpload&)
{
    in = g_testWorldFrame;
    return true;
}

bool TestCameraInLiquid()
{
    return g_testWorldFrame.inLiquid;
}

engine::StockFog TestReadStockFog()
{
    return g_testStockFog;
}

void TestWriteStockFog(const engine::StockFog& fog)
{
    g_testStockFog = fog;
}

void TestEndWorld()
{
}

bool TestGlowScreenEffectRuns()
{
    return g_testGlowScreenEffectRuns;
}

const WorldClient kTestOpaqueCaptureClient = {&LatestFogDevice,
                                              &CaptureTestOpaqueState,
                                              &engine::HasOpaqueState,
                                              &TestFrameInputsInCapturedViewport,
                                              &TestCameraInLiquid,
                                              &TestReadStockFog,
                                              &TestWriteStockFog,
                                              &engine::ClearOpaqueState,
                                              &TestGlowScreenEffectRuns};
const WorldClient kTestFogClient = {&LatestFogDevice,
                                    &CaptureTestOpaqueState,
                                    &TestWorldCaptured,
                                    &TestFrameInputs,
                                    &TestCameraInLiquid,
                                    &TestReadStockFog,
                                    &TestWriteStockFog,
                                    &TestEndWorld,
                                    &TestGlowScreenEffectRuns};
const WorldClient* g_worldClient = &kGameWorldClient;

bool FogDrawsInPlaceOfStockFog()
{
    return !g_failed && g_renderedLastFrame && GlobalConfig().Get().stockFog == 1 && !g_worldClient->cameraInLiquid() &&
           g_worldClient->device();
}

void PushStockFogOutOfRange()
{
    g_savedStockFog = g_worldClient->readStockFog();
    engine::StockFog outOfRange;
    for (int group = 0; group < engine::kDayNightFogGroupCount; ++group)
    {
        outOfRange.start[group] = kOutOfRangeStockFogStart;
        outOfRange.end[group] = kOutOfRangeStockFogStart * 2.0f;
    }
    g_worldClient->writeStockFog(outOfRange);
    g_stockFogPushed = true;
}

void RestorePushedStockFog()
{
    if (!g_stockFogPushed)
        return;
    g_worldClient->writeStockFog(g_savedStockFog);
    g_stockFogPushed = false;
}

bool TransparentFogStatsCounted()
{
    return g_transparentFogHooksInstalled && LogEnabled(LogLevel::Debug);
}

void ObserveM2BatchFog()
{
    g_m2BatchFogObserved = g_transparentFog.armed || TransparentFogStatsCounted() ? 1 : 0;
}

void DisarmTransparentFog()
{
    g_transparentFog.armed = false;
    ObserveM2BatchFog();
}

void ResetTransparentFogFrame()
{
    g_transparentFog = {};
    g_transparentFog.inWorld = true;
    ObserveM2BatchFog();
}

void LeaveTransparentFogWorld()
{
    g_transparentFog.inWorld = false;
    g_transparentFog.pastLiquid = false;
    DisarmTransparentFog();
}

void OnFrameBegin()
{
    g_renderedThisFrame = false;
    ResetTransparentFogFrame();
    if (FogDrawsInPlaceOfStockFog())
        PushStockFogOutOfRange();
}

void OnLiquidSurfaceBegin()
{
    if (g_failed || !g_renderedLastFrame || !GlobalConfig().Get().liquidDepth)
        return;
    g_liquidDepthWriteDevice = GameFogDevice();
    ForceDepthWrite(g_liquidDepthWriteDevice, true);
}

void OnLiquidSurfaceEnd()
{
    ForceDepthWrite(g_liquidDepthWriteDevice, false);
    g_liquidDepthWriteDevice = nullptr;
}

const char* GlarePassEntryText()
{
    static char text[kGlarePassEntryTextSize] = "";
    engine::DescribeGlarePassEntry(text, sizeof(text));
    return text;
}

double Share(unsigned part, unsigned whole)
{
    return whole ? kPercent * part / whole : 0.0;
}

void LogTransparentFogStats(const TransparentFogStats& s)
{
    const unsigned batches = s.batchesBeforeLiquid + s.batchesAfterLiquid + s.batchesOutsideWorld;
    VF_LOG_DEBUG("transparent fog: %u frames, %u early composites; M2 batch fog %u before the liquid end, %u after, "
                 "%u outside the world render, %u rewritten; colours lighting %.1f%%, black %.1f%%, white %.1f%%, "
                 "grey %.1f%%, other %.1f%%",
                 s.frames, s.earlyComposites, s.batchesBeforeLiquid, s.batchesAfterLiquid, s.batchesOutsideWorld,
                 s.batchesRewritten,
                 Share(s.lightingColours, batches), Share(s.additiveColours, batches),
                 Share(s.modulateColours, batches), Share(s.modulate2xColours, batches),
                 Share(s.otherColours, batches));
    VF_LOG_DEBUG("  fog exponent 1: %.1f%%, below 1: %.1f%%, above 1: %.1f%% (lowest %.3f, highest %.3f); glare pass "
                 "%u drawn before the fog, %u at its own call, %u own calls skipped; glare pass %s",
                 Share(s.linearExponents, batches), Share(s.lowerExponents, batches),
                 Share(s.higherExponents, batches), batches ? s.lowestExponent : 0.0f,
                 batches ? s.highestExponent : 0.0f, s.glareDrawnEarly, s.glareCallsPassed, s.glareCallsSkipped,
                 GlarePassEntryText());
}

void LogTransparentFogStatsWhenDue()
{
    if (!TransparentFogStatsCounted())
    {
        g_transparentFogStats = {};
        g_transparentFogStatsTicks = 0;
        return;
    }
    ++g_transparentFogStats.frames;
    const DWORD now = GetTickCount();
    if (!g_transparentFogStatsTicks)
        g_transparentFogStatsTicks = now;
    if (!g_transparentFogStatsDue && now - g_transparentFogStatsTicks < kTransparentFogStatsIntervalMs)
        return;
    g_transparentFogStatsDue = false;
    g_transparentFogStatsTicks = now;
    LogTransparentFogStats(g_transparentFogStats);
    g_transparentFogStats = {};
}

void OnFrameEnd()
{
    OnLiquidSurfaceEnd();
    LeaveTransparentFogWorld();
    RestorePushedStockFog();
    g_renderedLastFrame = g_renderedThisFrame;
    LogTransparentFogStatsWhenDue();
}

void ReloadConfigAfterInterval()
{
    DWORD now = GetTickCount();
    if (now - g_lastReload > kConfigReloadIntervalMs)
    {
        g_lastReload = now;
        GlobalConfig().ReloadIfChanged();
    }
}

void UseClientFogRangeInsteadOfPushed(FrameInputs& in)
{
    in.fogStart = g_savedStockFog.start[engine::kFrameInputsFogGroup];
    in.fogEnd = g_savedStockFog.end[engine::kFrameInputsFogGroup];
}

void UseDeliveredGlow(FrameInputs& in)
{
    float delivered = 0.0f;
    if (DeliveredGlowThisFrame(delivered))
        in.clientGlowAmount = !in.inLiquid && g_worldClient->glowScreenEffectRuns() ? delivered : 0.0f;
}

struct WaterClient
{
    FogDevice* (*device)();
    bool (*frameInputs)(FrameInputs& in);
    bool (*waterInputs)(WaterInputs& water);
    void (*contacts)(const FrameInputs& in, WaterContactFrame& out);
    bool (*contactsSupported)();
    WaterClass (*classify)(const void* liquidSettings);
};

bool GameWaterFrameInputs(FrameInputs& in)
{
    if (!engine::BuildFrameInputs(in, false, {}))
        return false;
    if (g_stockFogPushed)
        UseClientFogRangeInsteadOfPushed(in);
    return true;
}

bool GameWaterInputs(WaterInputs& water)
{
    if (!engine::BuildWaterInputs(water))
        return false;
    water.stockFogApplies = !g_stockFogPushed;
    return true;
}

void GameWaterContacts(const FrameInputs& in, WaterContactFrame& out)
{
    engine::CaptureWaterContacts(in.camTarget, out);
}

const WaterClient kGameWaterClient = {&GameFogDevice,
                                      &GameWaterFrameInputs,
                                      &GameWaterInputs,
                                      &GameWaterContacts,
                                      &engine::WaterContactsSupported,
                                      &engine::ClassifyWaterSettings};

FrameInputs g_testWaterFrame = {};
WaterInputs g_testWaterInputs = {};

bool TestWaterFrameInputs(FrameInputs& in)
{
    in = g_testWaterFrame;
    return true;
}

bool TestWaterInputs(WaterInputs& water)
{
    water = g_testWaterInputs;
    return true;
}

unsigned g_testWaterContactReads = 0;
bool g_testWaterContactsRefused = false;

void TestWaterContacts(const FrameInputs&, WaterContactFrame& out)
{
    out = g_testWaterInputs.contacts;
    ++g_testWaterContactReads;
}

bool TestWaterContactsSupported()
{
    return !g_testWaterContactsRefused;
}

WaterClass TestWaterClass(const void* liquidSettings)
{
    return liquidSettings ? static_cast<WaterClass>(*static_cast<const int*>(liquidSettings)) : WaterClass::None;
}

const WaterClient kTestWaterClient = {&LatestFogDevice,   &TestWaterFrameInputs,       &TestWaterInputs,
                                      &TestWaterContacts, &TestWaterContactsSupported, &TestWaterClass};
const WaterClient* g_waterClient = &kGameWaterClient;

void RecordFogFrame(bool rendered, bool cameraUnderLiquid, const char* skip)
{
    if (rendered)
    {
        g_fogNotDrawnReason = "";
        g_fogLog.Drawn();
        return;
    }
    if (cameraUnderLiquid)
    {
        g_fogNotDrawnReason = kCameraUnderLiquid;
        const StatusLogLine line = g_fogLog.Idle(kCameraUnderLiquid);
        if (line.write)
            LogWrite(line.level, "fog idle: %s%s", kCameraUnderLiquid,
                     line.level == LogLevel::Info ? " (repeats are logged at LogLevel 2)" : "");
        return;
    }
    g_fogNotDrawnReason = skip;
    const StatusLogLine line = g_fogLog.Skip(skip);
    if (line.write)
        LogWrite(line.level, "fog skipped: %s", skip);
}

struct FogAttempt
{
    bool tried;
    bool rendered;
    bool cameraUnderLiquid;
    const char* skip;
};

struct WorldFogInputs
{
    bool valid;
    bool cameraUnderLiquid;
    FrameInputs in;
};

WorldFogInputs ReadWorldFogInputs(const Config& cfg)
{
    WorldFogInputs world = {};
    world.valid = g_worldClient->frameInputs(world.in, cfg.localLights, LocalLightUpload(cfg));
    if (g_stockFogPushed)
        UseClientFogRangeInsteadOfPushed(world.in);
    UseDeliveredGlow(world.in);
    world.cameraUnderLiquid = world.valid && world.in.inLiquid && !cfg.underwater;
    return world;
}

FogAttempt RenderWorldFogFrom(FogDevice* device, const WorldFogInputs& world, const Config& cfg, FogPass pass)
{
    const char* skip = kInvalidFrameInputs;
    const bool rendered =
        world.valid && !world.cameraUnderLiquid && RenderFog(device, world.in, cfg, pass, &skip);
    return {true, rendered, world.cameraUnderLiquid, skip};
}

FogAttempt RenderWorldFog(FogDevice* device, FogPass pass)
{
    if (!device || !g_worldClient->worldCaptured())
        return {false, false, false, ""};

    ReloadConfigAfterInterval();

    const Config& cfg = GlobalConfig().Get();
    return RenderWorldFogFrom(device, ReadWorldFogInputs(cfg), cfg, pass);
}

void RecordFogAttempt(const FogAttempt& attempt)
{
    g_renderedThisFrame = attempt.rendered;
    RecordFogFrame(attempt.rendered, attempt.cameraUnderLiquid, attempt.skip);
}

void RenderCurrentWorldFog(FogDevice* device)
{
    const FogAttempt attempt = RenderWorldFog(device, FogPass::WholeFrame);
    if (attempt.tried)
        RecordFogAttempt(attempt);
}

void RenderGodRaysOverTheWorld(FogDevice* device)
{
    const char* skip = "";
    if (RenderGodRaysAfterWorld(device, &skip))
    {
        g_godRayLog.Drawn();
        return;
    }
    const StatusLogLine line = g_godRayLog.Skip(skip);
    if (line.write)
        LogWrite(line.level, "god rays after the world skipped: %s", skip);
}

const char* EarlyCompositeBlocked(const Config& cfg)
{
    if (!cfg.transparentFog)
        return "TransparentFog=0";
    if (!g_transparentFogHooksInstalled)
        return "its hooks are not installed";
    if (g_transparentFogFailed)
        return "it stopped after an exception";
    if (!g_stockFogPushed)
        return "the stock fog is not replaced in this frame";
    if (cfg.debugView != 0)
        return "a debug view is shown";
    if (cfg.sunMarker)
        return "the sun marker is shown";
    if (g_worldClient->cameraInLiquid())
        return kCameraUnderLiquid;
    return nullptr;
}

void NoteEarlyCompositeBlocked(const Config& cfg, const char* blocked)
{
    if (!cfg.transparentFog)
        return;
    const StatusLogLine line = g_transparentFogLog.Idle(blocked);
    if (line.write)
        LogWrite(line.level, "transparent fog: the fog is drawn after the world because %s%s", blocked,
                 line.level == LogLevel::Info ? " (repeats are logged at LogLevel 2)" : "");
}

void NoteEarlyCompositeSkipped(const char* reason)
{
    const StatusLogLine line = g_transparentFogLog.Skip(reason);
    if (line.write)
        LogWrite(line.level, "transparent fog: the early composite was skipped: %s; the fog is drawn after the world",
                 reason);
}

const char* EarlyCompositeUnready(FogDevice* device, const WorldFogInputs& world)
{
    if (!world.valid)
        return kInvalidFrameInputs;
    if (world.cameraUnderLiquid)
        return kCameraUnderLiquid;
    const char* skip = "";
    return FogReadyToRender(device, world.in.viewport, &skip) ? nullptr : skip;
}

void DrawGlareBeforeTheFog()
{
    g_transparentFog.glareDrawn = true;
    ++g_transparentFogStats.glareDrawnEarly;
    client::RenderGlares();
}

void ArmTransparentFog(FogDevice* device)
{
    g_transparentFog.composited = true;
    g_transparentFog.fit = LastStockFogFit(device);
    g_transparentFog.armed = true;
    ++g_transparentFogStats.earlyComposites;
    ObserveM2BatchFog();
}

void OnTransparentsBegin()
{
    g_transparentFog.pastLiquid = true;
    if (g_failed)
        return;
    ReloadConfigAfterInterval();
    const Config& cfg = GlobalConfig().Get();
    const char* blocked = EarlyCompositeBlocked(cfg);
    FogDevice* device = blocked ? nullptr : g_worldClient->device();
    if (!blocked && !device)
        blocked = "there is no fog device";
    if (blocked)
    {
        NoteEarlyCompositeBlocked(cfg, blocked);
        return;
    }
    if (!g_worldClient->worldCaptured())
    {
        NoteEarlyCompositeSkipped(kWorldNotCaptured);
        return;
    }
    ForeverLookBeforeEarlyFog();
    const WorldFogInputs world = ReadWorldFogInputs(cfg);
    if (const char* unready = EarlyCompositeUnready(device, world))
    {
        NoteEarlyCompositeSkipped(unready);
        return;
    }
    DrawGlareBeforeTheFog();
    const FogAttempt attempt = RenderWorldFogFrom(device, world, cfg, FogPass::BeforeTransparents);
    if (!attempt.rendered)
    {
        NoteEarlyCompositeSkipped(attempt.skip);
        return;
    }
    g_transparentFogLog.Drawn();
    RecordFogAttempt(attempt);
    ArmTransparentFog(device);
}

void CountM2BatchFog(const M2BatchFogArgs& args)
{
    TransparentFogStats& s = g_transparentFogStats;
    const unsigned batches = s.batchesBeforeLiquid + s.batchesAfterLiquid + s.batchesOutsideWorld;
    if (!g_transparentFog.inWorld)
        ++s.batchesOutsideWorld;
    else
        ++(g_transparentFog.pastLiquid ? s.batchesAfterLiquid : s.batchesBeforeLiquid);
    const uint32_t colour = args.colour ? *args.colour : kAdditiveFogColour;
    if (UsesLightingFogColour(colour))
        ++s.lightingColours;
    else if (colour == kAdditiveFogColour)
        ++s.additiveColours;
    else if (colour == kModulateFogColour)
        ++s.modulateColours;
    else if (colour == kModulate2xFogColour)
        ++s.modulate2xColours;
    else
        ++s.otherColours;
    if (args.exponent == kLinearStockFogExponent)
        ++s.linearExponents;
    else if (args.exponent < kLinearStockFogExponent)
        ++s.lowerExponents;
    else
        ++s.higherExponents;
    s.lowestExponent = batches ? std::min(s.lowestExponent, args.exponent) : args.exponent;
    s.highestExponent = batches ? std::max(s.highestExponent, args.exponent) : args.exponent;
}

void OnM2BatchFog(M2BatchFogArgs& args)
{
    if (TransparentFogStatsCounted())
        CountM2BatchFog(args);
    if (!g_transparentFog.armed)
        return;
    ApplyStockFogFit(g_transparentFog.fit, args);
    ++g_transparentFogStats.batchesRewritten;
}

int TransparentFogGuardFilter(unsigned code, const char* where)
{
    VF_LOG_ERROR("exception 0x%08X in %s; transparent fog disabled for this session, the fog is drawn after the world",
                 code, where);
    return EXCEPTION_EXECUTE_HANDLER;
}

void FailTransparentFog()
{
    g_transparentFogFailed = true;
    DisarmTransparentFog();
}

void OnOpaqueDone()
{
    FogDevice* device = g_worldClient->device();
    if (!device)
        return;
    g_worldClient->captureOpaqueState(RealDevice(device));
}

void OnWorldDone()
{
    if (!g_failed && g_transparentFog.composited)
        RenderGodRaysOverTheWorld(g_worldClient->device());
    else if (!g_failed)
        RenderCurrentWorldFog(g_worldClient->device());
    DisarmTransparentFog();
    g_worldClient->endWorld();
}

int GuardFilter(unsigned code, const char* where)
{
    VF_LOG_ERROR("exception 0x%08X in %s; fog disabled for this session", code, where);
    return EXCEPTION_EXECUTE_HANDLER;
}

void RecordWaterSkip(const char* reason)
{
    g_waterStatus = {false, reason};
    const StatusLogLine line = g_waterLog.Skip(reason);
    if (line.write)
        LogWrite(line.level, "water skipped: %s", reason);
}

void RecordWaterIdle(const char* state)
{
    g_waterStatus = {false, state};
    const StatusLogLine line = g_waterLog.Idle(state);
    if (line.write)
        LogWrite(line.level, "water idle: %s%s", state,
                 line.level == LogLevel::Info ? " (repeats are logged at LogLevel 2)" : "");
}

unsigned WaterClassBit(WaterClass waterClass)
{
    return 1u << static_cast<unsigned>(waterClass);
}

const char* DrawnWaterClassesText(unsigned classMask, bool flatWaves)
{
    const unsigned key = classMask | (flatWaves ? kFlatWavesBit : 0u);
    if (key == g_drawnWaterClassesTextMask)
        return g_drawnWaterClassesText;
    g_drawnWaterClassesTextMask = key;
    g_drawnWaterClassesText[0] = 0;
    for (WaterClass waterClass : {WaterClass::Lake, WaterClass::River, WaterClass::Ocean, WaterClass::Interior})
    {
        if (!(classMask & WaterClassBit(waterClass)))
            continue;
        const size_t used = std::strlen(g_drawnWaterClassesText);
        std::snprintf(g_drawnWaterClassesText + used, sizeof(g_drawnWaterClassesText) - used, "%s%s",
                      used ? ", " : "", WaterClassLabel(waterClass));
    }
    if (flatWaves)
    {
        const size_t used = std::strlen(g_drawnWaterClassesText);
        std::snprintf(g_drawnWaterClassesText + used, sizeof(g_drawnWaterClassesText) - used, "; flat waves");
    }
    return g_drawnWaterClassesText;
}

void RecordWaterDrawn(unsigned classMask, bool flatWaves)
{
    g_waterStatus = {true, DrawnWaterClassesText(classMask, flatWaves)};
    g_waterLog.Drawn();
}

enum class WaterArming
{
    Armed,
    Idle,
    Failed,
};

WaterArming ArmWaterPass(FogDevice* device, const Config& cfg, const char** reason)
{
    FrameInputs in = {};
    if (!g_waterClient->frameInputs(in))
    {
        *reason = "invalid frame inputs";
        return WaterArming::Failed;
    }
    if (in.inLiquid)
    {
        *reason = "camera under water";
        return WaterArming::Idle;
    }
    WaterInputs water = {};
    if (!g_waterClient->waterInputs(water))
    {
        *reason = "the client's water colours are unavailable";
        return WaterArming::Failed;
    }
    water.contacts = {};
    if (cfg.waterRipples > 0.0f)
        g_waterClient->contacts(in, water.contacts);
    const char* skip = "";
    if (BeginWaterPass(device, in, water, cfg, &skip))
        return WaterArming::Armed;
    *reason = skip && *skip ? skip : "the water pass could not start";
    return WaterArming::Failed;
}

void OnWaterPassBegin(const void* liquidRenderer)
{
    g_waterPassRanThisFrame = true;
    g_waterPassDevice = nullptr;
    g_waterClassesThisPass = 0;
    const Config& cfg = GlobalConfig().Get();
    if (g_waterFailed || !cfg.water)
        return;
    if (g_failed)
    {
        RecordWaterSkip("the fog stopped after an exception");
        return;
    }
    if (!engine::TransparentLiquidsQueued(liquidRenderer))
    {
        RecordWaterIdle("no water in view");
        return;
    }
    FogDevice* device = g_waterClient->device();
    if (!device)
    {
        RecordWaterSkip("no fog device");
        return;
    }
    const char* reason = "the water pass could not start";
    g_waterPassDevice = device;
    g_waterResourcesDevice = device;
    const WaterArming arming = ArmWaterPass(device, cfg, &reason);
    if (arming == WaterArming::Armed)
        return;
    g_waterPassDevice = nullptr;
    if (arming == WaterArming::Idle)
        RecordWaterIdle(reason);
    else
        RecordWaterSkip(reason);
}

void OnWaterPassEnd()
{
    if (!g_waterPassDevice)
        return;
    const WaterPassEnd end = EndWaterPass(g_waterPassDevice);
    g_waterPassDevice = nullptr;
    g_waterShadedThisFrame = g_waterShadedThisFrame || end.shaded;
    if (end.shaded)
    {
        g_waterRipplesAvailable = end.ripplesAvailable;
        RecordWaterDrawn(end.shadedClasses, end.flatWaves);
    }
    else if (!g_waterClassesThisPass)
        RecordWaterIdle("no water in view");
    else
        RecordWaterSkip(end.skipReason && *end.skipReason ? end.skipReason : "the water pass shaded nothing");
}

void TagWaterDrawUnsafe(const void* liquidSettings)
{
    const WaterClass waterClass = g_waterClient->classify(liquidSettings);
    TagWaterDraw(g_waterPassDevice, waterClass);
    if (waterClass != WaterClass::None)
        g_waterClassesThisPass |= WaterClassBit(waterClass);
}

void AbortArmedWaterPass()
{
    FogDevice* device = g_waterPassDevice;
    g_waterPassDevice = nullptr;
    if (device)
        AbortWaterPass(device);
}

void ReleaseWaterWhenOff()
{
    if (!g_waterResourcesDevice || (!g_waterFailed && GlobalConfig().Get().water))
        return;
    FogDevice* device = g_waterResourcesDevice;
    g_waterResourcesDevice = nullptr;
    if (device == g_waterClient->device())
        ReleaseWaterResources(device);
}

bool RingsReplaceClientSprites(const Config& cfg)
{
    return !g_waterFailed && cfg.water && cfg.waterRipples > 0.0f && !cfg.waterClientSplashes &&
           g_waterRipplesAvailable && g_waterClient->contactsSupported();
}

void UpdateClientRippleSprites()
{
    const bool allowed = RingsReplaceClientSprites(GlobalConfig().Get());
    GlobalClientRippleSprites().Update(allowed, g_waterShadedThisFrame, WaterClockSeconds());
    g_waterShadedThisFrame = false;
}

void EndWaterFrame()
{
    if (!g_waterHooksInstalled)
        return;
    if (g_waterPassDevice)
    {
        AbortArmedWaterPass();
        RecordWaterSkip("the water pass did not finish");
    }
    else if (!g_waterPassRanThisFrame)
        RecordWaterIdle("no liquid pass in this frame");
    g_waterPassRanThisFrame = false;
    UpdateClientRippleSprites();
    ReleaseWaterWhenOff();
}

int WaterGuardFilter(unsigned code, const char* where)
{
    if (!g_waterFaultLogged)
        VF_LOG_ERROR("exception 0x%08X in %s; water disabled for this session, the fog continues", code, where);
    g_waterFaultLogged = true;
    return EXCEPTION_EXECUTE_HANDLER;
}

void FailWater()
{
    g_waterFailed = true;
    GlobalClientRippleSprites().Restore();
    VF_TRY
    {
        AbortArmedWaterPass();
    }
    VF_EXCEPT(WaterGuardFilter(GetExceptionCode(), "water pass abort"))
    {
        g_waterPassDevice = nullptr;
    }
}

bool TagArmedWaterDraw(const void* liquidSettings)
{
    if (!g_waterPassDevice)
        return false;
    VF_TRY
    {
        TagWaterDrawUnsafe(liquidSettings);
        return true;
    }
    VF_EXCEPT(WaterGuardFilter(GetExceptionCode(), "water draw classification"))
    {
        FailWater();
        return false;
    }
}

void UntagArmedWaterDraw()
{
    if (!g_waterPassDevice)
        return;
    VF_TRY
    {
        UntagWaterDraw(g_waterPassDevice);
    }
    VF_EXCEPT(WaterGuardFilter(GetExceptionCode(), "water draw untag"))
    {
        FailWater();
    }
}

void EndWaterFrameGuarded()
{
    VF_TRY
    {
        EndWaterFrame();
    }
    VF_EXCEPT(WaterGuardFilter(GetExceptionCode(), "water frame end"))
    {
        FailWater();
    }
}

float g_loggedFarClip = -1.0f;
int g_loggedFarClipMap = -1;

bool IsContinentCappedByExtensionsDll(int mapId)
{
    return mapId == kEasternKingdomsMap || mapId == kKalimdorMap || mapId == kOutlandMap || mapId == kNorthrendMap;
}

const Config& ReloadedConfig()
{
    GlobalConfig().ReloadIfChanged();
    return GlobalConfig().Get();
}

float LiftedFarClipClamp(float clientFarClip, float farClipSetting, int mapId)
{
    float result = clientFarClip;
    const float farClipMax = ReloadedConfig().farClipMax;
    if (farClipMax > 0.0f && IsContinentCappedByExtensionsDll(mapId) && std::isfinite(farClipSetting))
        result = std::max(result,
                          std::clamp(farClipSetting, kEngineFarClipMin, std::min(farClipMax, kEngineFarClipMax)));
    if (result != g_loggedFarClip || mapId != g_loggedFarClipMap)
    {
        g_loggedFarClip = result;
        g_loggedFarClipMap = mapId;
        VF_LOG_INFO("far clip: map %d, farclip setting %.1f -> %.2f", mapId, farClipSetting, result);
    }
    return result;
}
}

extern "C" float __cdecl vf_far_clip_clamp(float clientFarClip, float farClipSetting, int mapId)
{
    if (!g_farClipLifted)
        return clientFarClip;
    VF_TRY
    {
        return LiftedFarClipClamp(clientFarClip, farClipSetting, mapId);
    }
    VF_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        return clientFarClip;
    }
}

extern "C" void __cdecl vf_on_frame_begin()
{
    if (!g_engineHooksInstalled)
        return;
    VF_TRY
    {
        OnFrameBegin();
    }
    VF_EXCEPT(GuardFilter(GetExceptionCode(), "frame begin hook"))
    {
        g_failed = true;
    }
}

extern "C" void __cdecl vf_on_frame_end()
{
    if (!g_engineHooksInstalled)
        return;
    EndWaterFrameGuarded();
    VF_TRY
    {
        OnFrameEnd();
    }
    VF_EXCEPT(GuardFilter(GetExceptionCode(), "frame end hook"))
    {
        g_failed = true;
    }
    ForeverLookAtFrameEnd();
}

extern "C" void __cdecl vf_on_water_pass_begin(const void* liquidRenderer)
{
    if (!g_waterHooksInstalled)
        return;
    VF_TRY
    {
        OnWaterPassBegin(liquidRenderer);
    }
    VF_EXCEPT(WaterGuardFilter(GetExceptionCode(), "water pass begin hook"))
    {
        FailWater();
    }
}

extern "C" void __cdecl vf_on_water_pass_end()
{
    if (!g_waterHooksInstalled)
        return;
    VF_TRY
    {
        OnWaterPassEnd();
    }
    VF_EXCEPT(WaterGuardFilter(GetExceptionCode(), "water pass end hook"))
    {
        FailWater();
    }
}

extern "C" void __cdecl vf_on_liquid_begin()
{
    if (!g_engineHooksInstalled)
        return;
    VF_TRY
    {
        OnLiquidSurfaceBegin();
    }
    VF_EXCEPT(GuardFilter(GetExceptionCode(), "liquid begin hook"))
    {
        g_failed = true;
    }
}

extern "C" void __cdecl vf_on_liquid_end()
{
    if (!g_engineHooksInstalled)
        return;
    VF_TRY
    {
        OnLiquidSurfaceEnd();
    }
    VF_EXCEPT(GuardFilter(GetExceptionCode(), "liquid end hook"))
    {
        g_failed = true;
    }
}

extern "C" void __cdecl vf_on_transparents_begin()
{
    if (!g_transparentFogHooksInstalled)
        return;
    VF_TRY
    {
        OnTransparentsBegin();
    }
    VF_EXCEPT(GuardFilter(GetExceptionCode(), "transparent fog hook"))
    {
        g_failed = true;
        DisarmTransparentFog();
    }
}

extern "C" void __cdecl vf_on_m2_batch_fog(M2BatchFogArgs* args)
{
    if (!g_transparentFogHooksInstalled || !g_m2BatchFogObserved)
        return;
    VF_TRY
    {
        OnM2BatchFog(*args);
    }
    VF_EXCEPT(TransparentFogGuardFilter(GetExceptionCode(), "M2 batch fog hook"))
    {
        FailTransparentFog();
    }
}

extern "C" void __cdecl vf_on_opaque_done()
{
    if (!g_engineHooksInstalled)
        return;
    VF_TRY
    {
        OnOpaqueDone();
    }
    VF_EXCEPT(GuardFilter(GetExceptionCode(), "opaque hook"))
    {
        g_failed = true;
    }
}

extern "C" void __cdecl vf_on_world_done()
{
    if (!g_engineHooksInstalled)
        return;
    ForeverLookAtWorldDone();
    VF_TRY
    {
        OnWorldDone();
    }
    VF_EXCEPT(GuardFilter(GetExceptionCode(), "world hook"))
    {
        g_failed = true;
    }
}

extern "C" void __cdecl vf_on_glare_pass()
{
    if (g_transparentFogHooksInstalled && g_transparentFog.glareDrawn)
    {
        ++g_transparentFogStats.glareCallsSkipped;
        return;
    }
    ++g_transparentFogStats.glareCallsPassed;
    client::RenderGlares();
}

extern "C" int __cdecl vf_on_water_draw_begin(void* liquidSettings)
{
    return g_waterHooksInstalled && TagArmedWaterDraw(liquidSettings) ? 1 : 0;
}

extern "C" void __cdecl vf_on_water_draw_end(int tagged)
{
    if (tagged)
        UntagArmedWaterDraw();
}

namespace
{
FogDevice* g_worldTextDevice = nullptr;
}

extern "C" void __cdecl vf_on_world_text_begin()
{
    g_worldTextDevice = nullptr;
    if (!g_engineHooksInstalled)
        return;
    VF_TRY
    {
        if (!g_failed && GlobalConfig().Get().enable)
            g_worldTextDevice = GameFogDevice();
        SuppressDepthWrite(g_worldTextDevice, true);
    }
    VF_EXCEPT(GuardFilter(GetExceptionCode(), "world text depth hook"))
    {
        g_failed = true;
    }
}

extern "C" void __cdecl vf_on_world_text_end()
{
    SuppressDepthWrite(g_worldTextDevice, false);
    g_worldTextDevice = nullptr;
}

bool InstallEngineHooks()
{
    g_engineHooksInstalled = true;
    VF_LOG_INFO("engine hooks: called directly by the client's world render");
    return true;
}

void EnableForeverLookOnHookedClient()
{
    EnableForeverLook(&GameFogDevice, engine::GlowPassColourLayoutMatches(), engine::GradingPlacementMatches());
}

bool InstallTransparentFogHooks()
{
    g_transparentFogHooksInstalled = true;
    VF_LOG_INFO("transparent fog: M2 batch fog and glare pass called directly by the client (TransparentFog=%d)",
                GlobalConfig().Get().transparentFog ? 1 : 0);
    return true;
}

FogFrameStatus LastFogFrameStatus()
{
    if (g_failed)
        return {false, "stopped after an exception, see CoAVolFog.log"};
    if (g_renderedLastFrame)
        return {true, ""};
    return {false, *g_fogNotDrawnReason ? g_fogNotDrawnReason : "waiting for the world to render"};
}

void InstallFarClipHooks()
{
    if (GlobalConfig().Get().farClipMax <= kFarClipMaxKeepsClientCap)
    {
        VF_LOG_INFO("far clip left to the client (FarClipMax=0)");
        return;
    }
    g_farClipLifted = true;
    VF_LOG_INFO("far clip lifted up to %.0f", GlobalConfig().Get().farClipMax);
}

bool InstallWaterHooks()
{
    g_waterHooksInstalled = true;
    VF_LOG_INFO("water: liquid pass and water materials called directly by the client");
    GlobalClientRippleSprites().Bind(client::WaterRipplesValue(), ClientCodeView());
    return true;
}

void RecordHookedFogFrame(bool rendered, bool cameraUnderLiquid, const char* skip)
{
    RecordFogFrame(rendered, cameraUnderLiquid, skip);
}

void UseTestWorldClient(const FrameInputs& in, bool glowScreenEffectRuns)
{
    g_testWorldFrame = in;
    g_testGlowScreenEffectRuns = glowScreenEffectRuns;
    g_worldClient = &kTestOpaqueCaptureClient;
}

void SimulateFogHookFailure(bool failed)
{
    g_failed = failed;
}

void UseTestFogClient(const FrameInputs& in)
{
    g_testWorldFrame = in;
    g_testGlowScreenEffectRuns = true;
    g_worldClient = &kTestFogClient;
    g_transparentFogHooksInstalled = true;
}

engine::StockFog TestClientStockFog()
{
    return g_testStockFog;
}

void LogTransparentFogStatsAtFrameEnd()
{
    g_transparentFogStatsDue = true;
}

void ClearTransparentFogFailure()
{
    g_transparentFogFailed = false;
}

void SetTestClientStockFog(const engine::StockFog& fog)
{
    g_testStockFog = fog;
}

void UseTestWaterClient(const FrameInputs& in, const WaterInputs& water)
{
    g_testWaterFrame = in;
    g_testWaterInputs = water;
    g_waterClient = &kTestWaterClient;
    g_waterHooksInstalled = true;
}

unsigned TestWaterContactReads()
{
    return g_testWaterContactReads;
}

void RefuseTestWaterContacts(bool refused)
{
    g_testWaterContactsRefused = refused;
}

bool TagHookedWaterDraw(const void* liquidSettings)
{
    return TagArmedWaterDraw(liquidSettings);
}

void UntagHookedWaterDraw()
{
    UntagArmedWaterDraw();
}

WaterFrameStatus LastWaterFrameStatus()
{
    if (!g_waterHooksInstalled)
        return {false, "hooks not installed"};
    if (g_waterFailed)
        return {false, "stopped after an exception, see CoAVolFog.log"};
    if (!GlobalConfig().Get().water)
        return {false, "off"};
    return g_waterStatus;
}
