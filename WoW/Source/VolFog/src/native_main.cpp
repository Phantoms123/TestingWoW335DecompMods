#include "client_ripple_sprites.h"
#include "config.h"
#include "d3d9_wrap.h"
#include "fog_data.h"
#include "forever_look.h"
#include "hooks.h"
#include "log.h"
#include "noise_volume.h"
#include "water_data.h"

#include <windows.h>

#include <string>

namespace
{
bool g_started = false;

std::string ClientDirectory()
{
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
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

extern "C" IDirect3D9* VolFog_CreateDirect3D9(Direct3DCreate9Fn create, UINT sdkVersion)
{
    VolFog_Startup();
    SetRealDirect3DCreate9(create);
    return WrappedDirect3DCreate9(sdkVersion);
}
