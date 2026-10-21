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
// Not ported yet: real reflections of terrain and buildings (waterDetail 3 draws like 2), and the ripples
// 4.3.4 makes from the collision meshes of moving objects (boats and the like).

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
        // with real reflections (not ported, draws as 2). The others are kept for when those parts are ported.
        void SetWaterDetail(uint32_t detail);
        void SetRippleDetail(uint32_t detail);
        void SetReflectionMode(uint32_t mode);
        void SetReflectionDownscale(uint32_t downscale);
        uint32_t GetWaterDetail();

        // Whether the ripple simulation runs (water 4.3.4, rippleDetail 1 or 2, and its maps and shaders made).
        bool RipplesActive();
        // A unit's own disturbance of the water, renewed every frame it is in water (4.3.4's CGUnit_C::CreateRipple
        // with ripples on); force keeps it past the limit (the active player).
        void SetUnitRipple(const void* owner, const C3Vector& position, float radius, float strength, bool force);
        // Once a frame before the liquids draw: moves the ripple maps with the camera, adds rain and units, steps
        // the waves.
        void UpdateRipples(const C3Vector& camera);

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
