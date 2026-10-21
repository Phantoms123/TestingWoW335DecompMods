#ifndef WOW_SOURCE_VOLFOG_VOLFOG_H
#define WOW_SOURCE_VOLFOG_VOLFOG_H

// CoAVolFog (volumetric fog and modern water, https://github.com/jealous-sound/coa-vfog, GPL-3.0) built into the
// client. The original attaches as a DLL and patches call sites in Wow.exe; here the client calls these entry points
// at the same places instead. Without WOWDECOMP_VOLFOG every entry point is an empty inline, so the call sites cost
// nothing and the client renders as before.

#include <cstdint>

struct IDirect3D9;

struct VolFogM2BatchFog {
    float start;
    float end;
    float exponent;
    const uint32_t* colour;
};

#if defined(WOWDECOMP_VOLFOG)

#if !defined(_WIN32)
#ifndef __stdcall
#define __stdcall
#endif
#ifndef __cdecl
#define __cdecl
#endif
#endif

typedef IDirect3D9* (__stdcall* VolFogDirect3DCreate9Fn)(unsigned int sdkVersion);

// Loads CoAVolFog.ini, fogdata.bin and waterdata.bin from the client folder. Call once, before the device exists.
extern "C" void VolFog_Startup();
extern "C" void VolFog_Shutdown();
// Returns the client's Direct3D 9 object wrapped so the fog sees the device it creates.
extern "C" IDirect3D9* VolFog_CreateDirect3D9(VolFogDirect3DCreate9Fn create, unsigned int sdkVersion);
// CHANGED (wowdecomp): the master switch, driven by the coaVolFog CVar. Off: every effect stops from the next frame
// and the client draws as unmodified. Off when the device is created (or with CoAVolFog.ini missing): CoAVolFog is
// not loaded at all, so turning it back on needs a restart. VolFog_IsLoaded tells which.
extern "C" void VolFog_SetActive(int active);
extern "C" int VolFog_IsActive();
extern "C" int VolFog_IsLoaded();

extern "C" void __cdecl vf_on_frame_begin();
extern "C" void __cdecl vf_on_frame_end();
extern "C" void __cdecl vf_on_opaque_done();
extern "C" void __cdecl vf_on_liquid_begin();
extern "C" void __cdecl vf_on_liquid_end();
extern "C" void __cdecl vf_on_transparents_begin();
extern "C" void __cdecl vf_on_world_done();
extern "C" void __cdecl vf_on_glare_pass();
extern "C" void __cdecl vf_on_world_text_begin();
extern "C" void __cdecl vf_on_world_text_end();
extern "C" void __cdecl vf_on_m2_batch_fog(VolFogM2BatchFog* args);
extern "C" void __cdecl vf_on_water_pass_begin(const void* liquidRenderer);
extern "C" void __cdecl vf_on_water_pass_end();
extern "C" int __cdecl vf_on_water_draw_begin(void* liquidSettings);
extern "C" void __cdecl vf_on_water_draw_end(int tagged);
extern "C" float __cdecl vf_far_clip_clamp(float clientFarClip, float farClipSetting, int mapId);
#if !defined(_WIN32)
// The overlay's input on Linux (CoAVolFog chains the window procedure on Windows): offered each SDL event before the
// client handles it; nonzero means the overlay took it.
extern "C" int vf_on_os_event(void* sdlEvent);
#endif

#define VOLFOG_ENABLED 1

#else

#define VOLFOG_ENABLED 0

inline void VolFog_Startup() {}
inline void VolFog_Shutdown() {}
inline void VolFog_SetActive(int) {}
inline int VolFog_IsActive() { return 0; }
inline int VolFog_IsLoaded() { return 0; }
inline void vf_on_frame_begin() {}
inline void vf_on_frame_end() {}
inline void vf_on_opaque_done() {}
inline void vf_on_liquid_begin() {}
inline void vf_on_liquid_end() {}
inline void vf_on_transparents_begin() {}
inline void vf_on_world_done() {}
inline void vf_on_world_text_begin() {}
inline void vf_on_world_text_end() {}
inline void vf_on_m2_batch_fog(VolFogM2BatchFog*) {}
inline void vf_on_water_pass_begin(const void*) {}
inline void vf_on_water_pass_end() {}
inline int vf_on_water_draw_begin(void*) { return 0; }
inline void vf_on_water_draw_end(int) {}
inline float vf_far_clip_clamp(float clientFarClip, float, int) { return clientFarClip; }
inline int vf_on_os_event(void*) { return 0; }

#endif

// Tags one water draw for the water renderer while it is drawn; every return path of the material render ends it.
struct VolFogWaterDrawScope {
    int m_tagged;

    explicit VolFogWaterDrawScope(void* liquidSettings) : m_tagged(vf_on_water_draw_begin(liquidSettings)) {}
    ~VolFogWaterDrawScope() { vf_on_water_draw_end(this->m_tagged); }
};

// Keeps text quads drawn in the world out of the depth the fog is integrated against.
struct VolFogWorldTextScope {
    VolFogWorldTextScope() { vf_on_world_text_begin(); }
    ~VolFogWorldTextScope() { vf_on_world_text_end(); }
};

#endif
