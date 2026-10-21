#include "Engine/Source/Model2/M2Compat.h"
#include <cmath>
#include <cstddef>
#include <cstring>

// Record widths. 4.x (265-272) appended two uint16[4] multi-texture parameter blocks to the particle
// emitter, and replaced the camera's fov float with an animated fov track appended after the roll track.
static const uint32_t PARTICLE_STRIDE_CLIENT = 0x1DC;
static const uint32_t PARTICLE_STRIDE_COMPAT = 0x1EC;
static const uint32_t CAMERA_STRIDE_CLIENT = 0x64;
static const uint32_t CAMERA_STRIDE_COMPAT = 0x74;

static_assert(sizeof(M2Particle) == PARTICLE_STRIDE_CLIENT, "client particle record");
static_assert(sizeof(M2Camera) == CAMERA_STRIDE_CLIENT, "client camera record");
static_assert(sizeof(M2Track<float>) == 0x14, "track header");
// Field offsets inside the client records (see M2Data.h).
static const uint32_t PARTICLE_OFF_FLAGS = 0x04;
static const uint32_t PARTICLE_OFF_GRAVITYTRACK = 0x84;
static const uint32_t CAMERA_OFF_TYPE = 0x00;
static const uint32_t CAMERA_OFF_FOV = 0x04;
static const uint32_t CAMERA_OFF_FARCLIP = 0x08;
static const uint32_t CAMERA_OFF_NEARCLIP = 0x0C;
static const uint32_t CAMERA_OFF_BODY = 0x10;

// Source camera layout: type, farClip, nearClip, then the same 0x54-byte track body the client keeps at
// +0x10, then the fov track.
static const uint32_t CAMERA_SRC_FARCLIP = 0x04;
static const uint32_t CAMERA_SRC_NEARCLIP = 0x08;
static const uint32_t CAMERA_SRC_BODY = 0x0C;
static const uint32_t CAMERA_SRC_FOVTRACK = 0x60;
static const uint32_t CAMERA_BODY_SIZE = 0x54;
static const float CAMERA_DEFAULT_FOV = 0.7853982f;

static_assert(CAMERA_OFF_BODY + CAMERA_BODY_SIZE == CAMERA_STRIDE_CLIENT, "client camera body");
static_assert(CAMERA_SRC_BODY + CAMERA_BODY_SIZE == CAMERA_SRC_FOVTRACK, "source camera body");
static_assert(CAMERA_SRC_FOVTRACK + 0x14 == CAMERA_STRIDE_COMPAT, "source camera width");

// Emitter flags that only newer content sets.
static const uint32_t PARTICLE_FLAG_COMPRESSED_GRAVITY = 0x00800000;
static const uint32_t PARTICLE_FLAG_MULTITEXTURE = 0x10000000;
// With PARTICLE_FLAG_MULTITEXTURE the texture index holds three packed 5-bit ids. This client draws one
// texture per emitter, so the first id is all it can use.
static const uint16_t PARTICLE_PACKED_TEXTURE_MASK = 0x1F;

// Sequence flag: the sequence's keys live in this file rather than a separate .anim file.
static const uint32_t SEQUENCE_KEYS_IN_FILE = 0x20;
static const uint16_t TRACK_NO_LOOP = 0xFFFF;

static uint32_t Read32(const uint8_t* p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static void Write32(uint8_t* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

// True when count records of stride bytes starting at offset lie inside the file.
static bool Fits(uint32_t size, uint32_t offset, uint32_t count, uint32_t stride) {
    if (offset > size) {
        return false;
    }

    return count <= (size - offset) / stride;
}

struct CompatContext {
    uint8_t* base;
    uint32_t size;
    const M2Data* data;

    // True when the keys for sequence slot s of a track live in this file.
    bool KeysInFile(uint16_t loopIndex, uint32_t s) const {
        if (loopIndex != TRACK_NO_LOOP) {
            return true;
        }

        uint32_t count = this->data->m_sequences.m_count;
        uint32_t offset = this->data->m_sequences.m_offset;

        if (s >= count || !Fits(this->size, offset, count, sizeof(M2Sequence))) {
            return false;
        }

        const M2Sequence* sequence = reinterpret_cast<const M2Sequence*>(this->base + offset) + s;
        return (sequence->m_flags & SEQUENCE_KEYS_IN_FILE) != 0;
    }
};

// Reads the first key of an animated float track, or fallback when the track has none in this file.
static float FirstTrackKey(const CompatContext& ctx, const uint8_t* track, float fallback) {
    uint16_t loopIndex;
    memcpy(&loopIndex, track + 2, sizeof(loopIndex));

    uint32_t slots = Read32(track + 0x0C);
    uint32_t offset = Read32(track + 0x10);

    if (!slots || !Fits(ctx.size, offset, slots, 8) || !ctx.KeysInFile(loopIndex, 0)) {
        return fallback;
    }

    const uint8_t* slot = ctx.base + offset;
    uint32_t keyCount = Read32(slot);
    uint32_t keyOffset = Read32(slot + 4);

    // Spline keys are value, in-tangent, out-tangent.
    if (!keyCount || !Fits(ctx.size, keyOffset, keyCount, 12)) {
        return fallback;
    }

    float value;
    memcpy(&value, ctx.base + keyOffset, sizeof(value));
    return value;
}

// Expands packed gravity vectors (int8 x, int8 y, int16 magnitude) into the plain float the client
// expects, for every sequence whose keys are in this file. Keys in .anim files stay as they are.
static bool ExpandGravityKeys(const CompatContext& ctx, uint8_t* record) {
    uint8_t* track = record + PARTICLE_OFF_GRAVITYTRACK;

    uint16_t loopIndex;
    memcpy(&loopIndex, track + 2, sizeof(loopIndex));

    uint32_t slots = Read32(track + 0x0C);
    uint32_t offset = Read32(track + 0x10);

    if (!slots) {
        return true;
    }

    if (!Fits(ctx.size, offset, slots, 8)) {
        return false;
    }

    for (uint32_t s = 0; s < slots; s++) {
        if (!ctx.KeysInFile(loopIndex, s)) {
            continue;
        }

        const uint8_t* slot = ctx.base + offset + s * 8;
        uint32_t keyCount = Read32(slot);
        uint32_t keyOffset = Read32(slot + 4);

        if (!keyCount) {
            continue;
        }

        if (!Fits(ctx.size, keyOffset, keyCount, 4)) {
            return false;
        }

        for (uint32_t k = 0; k < keyCount; k++) {
            uint8_t* key = ctx.base + keyOffset + k * 4;

            float x = static_cast<int8_t>(key[0]) / 128.0f;
            float y = static_cast<int8_t>(key[1]) / 128.0f;
            int16_t packed;
            memcpy(&packed, key + 2, sizeof(packed));

            float planar = x * x + y * y;
            float z = sqrtf(planar < 1.0f ? 1.0f - planar : 0.0f);
            float magnitude = packed * 0.04238648f;

            if (magnitude < 0.0f) {
                z = -z;
                magnitude = -magnitude;
            }

            float gravity = -(z * magnitude);
            memcpy(key, &gravity, sizeof(gravity));
        }
    }

    return true;
}

// Compacts 4.x particle records to the client width, dropping the multi-texture parameter blocks.
static bool NormalizeParticles(const CompatContext& ctx, M2Array<M2Particle>& array) {
    uint32_t count = array.m_count;
    uint32_t offset = array.m_offset;

    if (!count) {
        return true;
    }

    if (!Fits(ctx.size, offset, count, PARTICLE_STRIDE_COMPAT)) {
        return false;
    }

    // Front to back: each record moves to an address at or below its source, and never past the start
    // of the next source record, so no record is overwritten before it is read.
    for (uint32_t i = 0; i < count; i++) {
        uint8_t* src = ctx.base + offset + i * PARTICLE_STRIDE_COMPAT;
        uint8_t* dst = ctx.base + offset + i * PARTICLE_STRIDE_CLIENT;
        memmove(dst, src, PARTICLE_STRIDE_CLIENT);

        uint32_t flags = Read32(dst + PARTICLE_OFF_FLAGS);

        if (flags & PARTICLE_FLAG_COMPRESSED_GRAVITY) {
            if (!ExpandGravityKeys(ctx, dst)) {
                return false;
            }

            Write32(dst + PARTICLE_OFF_FLAGS, flags & ~PARTICLE_FLAG_COMPRESSED_GRAVITY);
        }
    }

    return true;
}

// Reshapes 4.x camera records to the client layout, taking the fixed fov from the fov track's first key.
static bool NormalizeCameras(const CompatContext& ctx, M2Array<M2Camera>& array) {
    uint32_t count = array.m_count;
    uint32_t offset = array.m_offset;

    if (!count) {
        return true;
    }

    if (!Fits(ctx.size, offset, count, CAMERA_STRIDE_COMPAT)) {
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint8_t* src = ctx.base + offset + i * CAMERA_STRIDE_COMPAT;
        uint8_t* dst = ctx.base + offset + i * CAMERA_STRIDE_CLIENT;

        // Read every scalar first: for the first record the body move overlaps the source header.
        uint32_t type = Read32(src);
        uint32_t farClip = Read32(src + CAMERA_SRC_FARCLIP);
        uint32_t nearClip = Read32(src + CAMERA_SRC_NEARCLIP);
        float fov = FirstTrackKey(ctx, src + CAMERA_SRC_FOVTRACK, CAMERA_DEFAULT_FOV);

        memmove(dst + CAMERA_OFF_BODY, src + CAMERA_SRC_BODY, CAMERA_BODY_SIZE);

        Write32(dst + CAMERA_OFF_TYPE, type);
        memcpy(dst + CAMERA_OFF_FOV, &fov, sizeof(fov));
        Write32(dst + CAMERA_OFF_FARCLIP, farClip);
        Write32(dst + CAMERA_OFF_NEARCLIP, nearClip);
    }

    return true;
}

// Brings packed or out-of-range emitter texture indices into the model's texture table. The stock
// client indexes the table with the raw value and reads past its end.
static bool ClampParticleTextures(const CompatContext& ctx, const M2Array<M2Particle>& array) {
    uint32_t count = array.m_count;
    uint32_t offset = array.m_offset;

    if (!count) {
        return true;
    }

    if (!Fits(ctx.size, offset, count, PARTICLE_STRIDE_CLIENT)) {
        return false;
    }

    uint32_t textureCount = ctx.data->m_textures.m_count;

    for (uint32_t i = 0; i < count; i++) {
        M2Particle* particle = reinterpret_cast<M2Particle*>(ctx.base + offset) + i;

        if (particle->m_flags & PARTICLE_FLAG_MULTITEXTURE) {
            particle->m_textureIndex &= PARTICLE_PACKED_TEXTURE_MASK;
        }

        if (textureCount && particle->m_textureIndex >= textureCount) {
            particle->m_textureIndex = 0;
        }
    }

    return true;
}

bool M2CompatIsNewer(const M2Data& data) {
    return data.m_version > M2_VERSION_CLIENT;
}

int32_t M2CompatNormalize(uint8_t* base, uint32_t size, M2Data& data) {
    if (reinterpret_cast<uint8_t*>(&data) < base || reinterpret_cast<uint8_t*>(&data + 1) > base + size) {
        return 0;
    }

    if (data.m_version < M2_VERSION_CLIENT || data.m_version > M2_VERSION_COMPAT_MAX) {
        return 0;
    }

    CompatContext ctx = { base, size, &data };

    // The 4.3.4 client reads the wider records for every version it accepts, so the same is done here.
    if (M2CompatIsNewer(data)) {
        if (!NormalizeParticles(ctx, data.m_particles) || !NormalizeCameras(ctx, data.m_cameras)) {
            return 0;
        }
    }

    // Also covers 3.3.5a-versioned files that a converter wrote from newer content.
    return ClampParticleTextures(ctx, data.m_particles) ? 1 : 0;
}

// The 4.3.4 client's special shader effects (the low 15 bits of a batch shader with bit 0x8000 set):
// the pixel and vertex shader each one draws with, from that client's effect table.
struct CompatShaderEffect {
    uint8_t pixelShader;
    uint8_t vertexShader;
};

static const CompatShaderEffect s_compatEffects[] = {
    { 12, 3 }, { 13, 3 }, { 14, 3 }, { 15, 6 }, { 16, 3 }, { 13, 7 }, { 16, 7 }, { 17, 3 }, { 18, 3 }, { 19, 6 },
    { 20, 7 }, { 21, 3 }, { 22, 3 }, { 23, 3 }, { 23, 7 }, { 20, 2 }, { 24, 3 }, { 25, 6 }, { 26, 8 }, { 1, 9 },
    { 27, 11 }, { 6, 12 }, { 28, 13 }, { 29, 7 }, { 25, 11 }, { 1, 14 }, { 30, 15 }, { 31, 2 }, { 32, 15 },
};

// For each 4.3.4 pixel shader, the two texture ops of the closest standard combiner this client has
// (op 0 opaque, 1 modulate, 3 add, 4 mod2x, 6 mod2x without alpha, 7 add without alpha).
static const uint8_t s_compatPixelOps[33][2] = {
    { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 4 }, { 0, 6 }, { 0, 0 }, { 1, 1 }, { 1, 4 }, { 1, 3 }, { 1, 6 }, { 1, 7 },
    { 1, 0 }, { 0, 6 }, { 0, 3 }, { 0, 3 }, { 0, 6 }, { 1, 3 }, { 1, 3 }, { 0, 1 }, { 0, 6 }, { 0, 3 }, { 1, 3 },
    { 0, 1 }, { 1, 3 }, { 0, 1 }, { 0, 6 }, { 1, 1 }, { 0, 6 }, { 1, 1 }, { 0, 1 }, { 0, 1 }, { 0, 1 }, { 0, 0 },
};

// Builds standard shader bits from a 4.3.4 effect: ops in bits 4-6 (first texture) and 0-2 (second),
// bit 3 of each nibble when that texture uses environment coordinates, bit 14 when the last texture uses
// the second coordinate set.
static uint16_t CompatEffectToShader(const CompatShaderEffect& effect) {
    const uint8_t* ops = s_compatPixelOps[effect.pixelShader];
    uint16_t op0 = ops[0];
    uint16_t op1 = ops[1];

    switch (effect.vertexShader) {
        case 1:   // Diffuse_Env
        case 4:   // Diffuse_Env_T1
        case 14:  // Diffuse_EdgeFade_Env
            op0 |= 8;
            break;
        case 5:   // Diffuse_Env_Env
            op0 |= 8;
            op1 |= 8;
            break;
        case 3:   // Diffuse_T1_Env
        case 6:   // Diffuse_T1_Env_T1
        case 11:  // Diffuse_T1_Env_T2
            op1 |= 8;
            break;
        default:
            break;
    }

    uint16_t shader = static_cast<uint16_t>((op0 << 4) | op1);

    switch (effect.vertexShader) {
        case 2:   // Diffuse_T1_T2
        case 10:  // Diffuse_T2
        case 12:  // Diffuse_EdgeFade_T1_T2
        case 13:  // Diffuse_T1_T1_T1_T2
            shader |= 0x4000;
            break;
        default:
            break;
    }

    return shader;
}

void M2CompatTranslateSkinShaders(M2SkinProfile* skin) {
    static const uint32_t effectCount = sizeof(s_compatEffects) / sizeof(s_compatEffects[0]);

    for (uint32_t i = 0; i < skin->m_batches.m_count; i++) {
        M2Batch* batch = &skin->m_batches[i];

        if (!(batch->m_shader & 0x8000)) {
            continue;
        }

        uint32_t effect = batch->m_shader & 0x7FFF;

        if (effect <= 2) {
            // 4.3.4 effects 0-2 are this client's specialized effects 1-3 (Mod2xNA_Alpha, AddAlpha and
            // AddAlpha_Alpha over Diffuse_T1_Env). This client's effect 0 means "drawn by the batch
            // before it", which a 4.x file never intends.
            batch->m_shader = static_cast<uint16_t>(0x8000 | (effect + 1));
        } else if (effect < effectCount) {
            batch->m_shader = CompatEffectToShader(s_compatEffects[effect]);
        } else {
            // Not an effect 4.3.4 knew either: draw it as a plain modulated texture.
            batch->m_shader = 0x0010;
        }
    }
}
