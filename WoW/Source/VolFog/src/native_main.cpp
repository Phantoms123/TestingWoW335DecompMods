#include "client_ripple_sprites.h"
#include "config.h"
#include "d3d9_wrap.h"
#include "fog_data.h"
#include "forever_look.h"
#include "hooks.h"
#include "log.h"
#include "noise_volume.h"
#include "water_data.h"

#include "platform.h"
#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <string>

namespace
{
bool g_started = false;
bool g_loaded = false;
bool g_requestedActive = true;
bool g_frameActive = true;

std::string ClientDirectory()
{
    char path[MAX_PATH] = {};
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
#else
    // CHANGED (wowdecomp): the Linux client's own path.
    ssize_t r = readlink("/proc/self/exe", path, MAX_PATH - 1);
    DWORD n = r > 0 ? static_cast<DWORD>(r) : 0;
#endif
    std::string dir(path, n);
    size_t slash = dir.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
}
}

extern "C" void VolFog_Startup()
{
    if (g_started)
        return;
    g_started = true;
    std::string dir = ClientDirectory();
    LogOpen((dir + "CoAVolFog.log").c_str());
    GlobalConfig().Load(dir + "CoAVolFog.ini");
    const Config& cfg = GlobalConfig().Get();
    VF_LOG_INFO("CoAVolFog built into the client, data from %s", dir.c_str());
    GlobalFogData().Load(dir + "fogdata.bin");

    if (!cfg.enable)
    {
        VF_LOG_INFO("Enable=0; the client renders unmodified");
        return;
    }
    if (!cfg.hooks)
    {
        VF_LOG_INFO("EngineHooks=0; the client renders unmodified");
        return;
    }
    const bool engineHooks = InstallEngineHooks();
    AllowFogOnNewDevices(engineHooks);
    PrepareAuthoredNoise();
    InstallTransparentFogHooks();
    EnableForeverLookOnHookedClient();
    if (InstallWaterHooks())
        GlobalWaterData().Load(dir + "waterdata.bin");
    InstallFarClipHooks();
}

extern "C" void VolFog_Shutdown()
{
    GlobalClientRippleSprites().Restore();
}

bool VolFogFrameActive()
{
    return g_frameActive;
}

void VolFogBeginFrameSwitch()
{
    // The switch takes effect between frames, so a frame never runs half its hooks.
    g_frameActive = g_requestedActive;
}

extern "C" void VolFog_SetActive(int active)
{
    if (g_requestedActive != (active != 0) && g_started)
        VF_LOG_INFO("coaVolFog %d: CoAVolFog %s", active, active ? "on" : "off; the client renders unmodified");
    g_requestedActive = active != 0;
}

extern "C" int VolFog_IsActive()
{
    return g_requestedActive ? 1 : 0;
}

extern "C" int VolFog_IsLoaded()
{
    return g_loaded ? 1 : 0;
}

extern "C" IDirect3D9* VolFog_CreateDirect3D9(Direct3DCreate9Fn create, UINT sdkVersion)
{
    // CHANGED (wowdecomp): with the switch off, or CoAVolFog's files removed, the client gets the real Direct3D 9
    // object and CoAVolFog stays unloaded.
    if (!g_started && !g_requestedActive)
        return create(sdkVersion);
    if (!g_started)
    {
        WIN32_FILE_ATTRIBUTE_DATA ini;
        if (!GetFileAttributesExA((ClientDirectory() + "CoAVolFog.ini").c_str(), GetFileExInfoStandard, &ini))
            return create(sdkVersion);
    }
    VolFog_Startup();
    g_loaded = true;
    SetRealDirect3DCreate9(create);
    return WrappedDirect3DCreate9(sdkVersion);
}
