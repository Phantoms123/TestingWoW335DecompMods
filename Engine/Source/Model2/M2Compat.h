#ifndef ENGINE_SOURCE_MODEL2_M2COMPAT_H
#define ENGINE_SOURCE_MODEL2_M2COMPAT_H

// Loads models authored for 4.x clients (M2 versions 265-272) into the 3.3.5a model runtime.
//
// Not part of the original 12340 client. The record layouts come from the 4.3.4 (15595) client, and the
// conversions follow WarcraftXL's wxl-modern-m2 (GPL-3.0-or-later, https://github.com/WarcraftXL),
// rewritten against this tree's own types instead of hooking the original binary.

#include <cstdint>
#include "Engine/Source/Model2/M2Data.h"

// The one inner version the stock loader understood.
static const uint32_t M2_VERSION_CLIENT = 0x108;
// The newest inner version the converter accepts (4.3.4).
static const uint32_t M2_VERSION_COMPAT_MAX = 0x110;

// Rewrites records whose on-disk shape changed after 3.3.5a into the shape this client steps, in place,
// while the arrays still hold file offsets. Runs once per model, before M2Init. Returns 0 when a record
// points outside the file, which fails the load instead of leaving a bad offset behind.
int32_t M2CompatNormalize(uint8_t* base, uint32_t size, M2Data& data);

// True when the model was authored for a newer client than this one.
bool M2CompatIsNewer(const M2Data& data);

// Translates the shader effect ids a 4.x skin carries in its file into this client's effect ids, and
// clears the ones it has no shader for so SubstituteSimpleShaders picks a standard combiner instead.
void M2CompatTranslateSkinShaders(M2SkinProfile* skin);

#endif
