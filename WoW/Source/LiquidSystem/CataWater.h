#ifndef WOW_SOURCE_LIQUIDSYSTEM_CATAWATER_H
#define WOW_SOURCE_LIQUIDSYSTEM_CATAWATER_H

// Water as the 4.3.4 client draws it (its Liquid::CMaterialWater with waterDetail 2 and up), ported into this
// client. Not part of the original 12340 client.
//
// The surface uses 4.3.4's own shaders (ProcWater, ProcWaterAbove, ProcWaterBelow and FourTapBlur, read from
// Shaders\Cata\...), refracts a copy of the frame drawn so far through animated normal maps, tints it by the water
// depth read from the scene depth, adds sun specular and foam, and reflects a blurred copy of the frame (screen-
// space) with a mirrored render of the sky behind it. Needs LiquidType.dbc with the 4.3.4 water values and
// textures (newoceanbump, waterchop7bw); without them, or with waterDetail 0, water draws as before.
//
// With rippleDetail 1 or 2 the surface is also bent by 4.3.4's ripple simulation: height maps around the camera,
// stepped with a wave equation and disturbed by units moving in water and by rain.
//
// waterDetail 3 also reflects the terrain (reflectionMode 2) and buildings (reflectionMode 3) in the water
// nearest the camera, drawn mirrored into a texture before the world.
//
// Not ported yet: the ripples 4.3.4 makes from the collision meshes of moving objects (boats and the like), and
// reflections of models (trees, characters).

#include <cstdint>

class C3Vector;
class C44Matrix;
class CAaSphere;

namespace Liquid {
    class CMaterialSettings;
    class IEnvironment;
    class IGeomFactory;
    class IWaveManager;

    namespace Cata {
        // CVar values. waterDetail: 0 = the 3.3.5 water, 1 = 3.3.5 water too, 2 = 4.3.4 water, 3 = 4.3.4 water
        // with real reflections (reflectionMode 2: terrain, 3: terrain and buildings; 0 and 1: sky only).
        void SetWaterDetail(uint32_t detail);
        void SetRippleDetail(uint32_t detail);
        void SetReflectionMode(uint32_t mode);
        void SetReflectionDownscale(uint32_t downscale);
        // How strongly the real reflection shows (0-1; below 1 it fades towards the fog colour).
        void SetReflectionStrength(float strength);
        // How much softer shorelines get with distance (0 = as 4.3.4, 1 = default, up to 4).
        void SetShorelineSoftness(float softness);
        // How far the real reflection shows (yards; it fades out over the second half, 0 = no fade).
        void SetReflectionDistance(float distance);
        uint32_t GetWaterDetail();

        // Whether the ripple simulation runs (water 4.3.4, rippleDetail 1 or 2, and its maps and shaders made).
        bool RipplesActive();
        // A unit's own disturbance of the water, renewed every frame it is in water (4.3.4's CGUnit_C::CreateRipple
        // with ripples on); force keeps it past the limit (the active player).
        void SetUnitRipple(const void* owner, const C3Vector& position, float radius, float strength, bool force);
        // Once a frame before the liquids draw: moves the ripple maps with the camera, adds rain and units, steps
        // the waves.
        void UpdateRipples(const C3Vector& camera);

        // Real reflections (waterDetail 3 with reflectionMode 2 or 3): when the water seen last frame wants one,
        // switches to the reflection texture, draws the mirrored sky and sets the mirrored view, clip plane and
        // winding for the terrain (and map objects, mode 3) the caller then draws; End puts it all back.
        bool BeginReflection(const C3Vector& worldViewOrigin, uint32_t& mode);
        void EndReflection();
        // Whether the world's culling should take in more above the view for the reflection.
        bool ReflectionNeedsWideCulling();
        // Within a reflection (mode 3): mirrors the models (trees, characters, creatures) for the caller to draw.
        bool BeginModelReflection();
        void EndModelReflection();
        void SetReflectionModels(bool enable);
        // While a reflection draws: true gives its clip plane in world space (for drawing without shaders), false
        // in clip space again.
        void SetReflectionClipForFixedFunction(bool fixedFunction);

        // Once a frame before the world draws (creates or frees what the current setting needs).
        void BeginFrame();
        // The camera's clip planes and the depth the far plane lands on, before the liquid pass.
        void SetView(float zNear, float zFar, float viewportFarClip);
        // Around the see-through liquid pass: copies and blurs the frame drawn so far and renders the sky
        // reflection the first time 4.3.4 water is drawn in the pass; the end puts back shader constants.
        void BeginLiquidPass(bool cameraUnderwater);
        void EndLiquidPass();

        // Draws one water surface the 4.3.4 way. False (nothing drawn) when that is off or unavailable, so the
        // caller draws its own water.
        bool RenderWater(IEnvironment* environment, IGeomFactory* geomFactory, const C3Vector& cameraPos, const C44Matrix* transform, const CAaSphere* bounds, CMaterialSettings* settings);
        // Draws one magma or slime surface with 4.3.4's ProcMagma (with the 4.3.4 water on); false when it is
        // off or unavailable, so the caller draws its own.
        bool RenderMagma(IEnvironment* environment, IGeomFactory* geomFactory, const C3Vector& cameraPos, const C44Matrix* transform, const CAaSphere* bounds, CMaterialSettings* settings);
    }
}

#endif
