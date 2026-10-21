// The Direct3D 9 fixed-function pipeline, as the client's Gx layer uses it, for the Direct3D 11 device (which has
// none). One vertex and one pixel shader; the state the Direct3D 9 device sent as render and texture stage states
// arrives in cbuffer FixedFunction (filled by CGxDeviceD3d11::IFfpSync, same layout) and is branched on.
//
// Follows Direct3D 9's rules (as DXVK implements them): lighting in view space with a local viewer, material
// colours from the material or the vertex colours, linear fog from the view-space depth, texture coordinates
// generated from the view-space position, normal or reflection vector and put through the stage matrices, the
// six texture operations the client uses, alpha test (greater or equal), world-space clip planes.

cbuffer FixedFunction : register(b1)
{
    // The first three registers are also what the shader model 4 builds of the client's pixel shaders read
    // (the 4.3.4 port's "Classic" set): fog colour, fog on (shaders older than shader model 3 had Direct3D 9's
    // fixed-function fog applied after them), alpha test on and its reference (0-255).
    float4 g_fogColor;
    float4 g_fogEnable;
    float4 g_alphaTest;
    row_major float4x4 g_world;
    row_major float4x4 g_view;
    row_major float4x4 g_proj;
    row_major float4x4 g_tex[8];
    // x: colour op (-1 stage off), y: alpha op, z: texcoord index | generation << 4, w: transform count | 0x100 projected
    int4 g_stage[8];
    float4 g_matDiffuse;
    float4 g_matEmissive;
    float4 g_matSpecular;     // w: power
    float4 g_ambient;         // the device's ambient (D3DRS_AMBIENT)
    float4 g_lightPos[4];     // view space; w 1 point light, 0 directional (xyz the direction the light travels)
    float4 g_lightDiffuse[4];
    float4 g_lightAmbient[4];
    float4 g_lightSpecular[4];
    float4 g_lightAtten[4];   // xyz constant, linear and quadratic attenuation; w 1 enabled
    int4 g_flags;             // x lighting, y fog, z unused, w normalize normals
    int4 g_flags2;            // x vertex colour 0 given, y colour 1 given, z material sources, w specular
    float4 g_fog;             // x start, y end
    float4 g_clip[6];         // world space
    int4 g_clipMask;          // x mask, y stage is a cube map (bit per stage)
};

struct VsIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float4 color0 : COLOR0;
    float4 color1 : COLOR1;
    float4 tc0 : TEXCOORD0;
    float4 tc1 : TEXCOORD1;
    float4 tc2 : TEXCOORD2;
    float4 tc3 : TEXCOORD3;
    float4 tc4 : TEXCOORD4;
    float4 tc5 : TEXCOORD5;
    float4 tc6 : TEXCOORD6;
    float4 tc7 : TEXCOORD7;
};

struct VsOut
{
    float4 pos : SV_Position;
    float4 diffuse : COLOR0;
    float4 specular : COLOR1;
    float4 tc0 : TEXCOORD0;
    float4 tc1 : TEXCOORD1;
    float4 tc2 : TEXCOORD2;
    float4 tc3 : TEXCOORD3;
    float4 tc4 : TEXCOORD4;
    float4 tc5 : TEXCOORD5;
    float4 tc6 : TEXCOORD6;
    float4 tc7 : TEXCOORD7;
    float4 fog : FOG;         // x; a whole register, the layout the client's shader model 4 shaders use
    float4 clip0 : SV_ClipDistance0;
    float2 clip1 : SV_ClipDistance1;
};

float4 PickSource(int source, float4 material, float4 c0, float4 c1)
{
    if (source == 1 && g_flags2.x != 0) {
        return c0;
    }

    if (source == 2 && g_flags2.y != 0) {
        return c1;
    }

    return material;
}

float4 StageCoord(int stage, float4 raw[8], float3 viewPos, float3 viewNormal)
{
    int coord = g_stage[stage].z;
    int index = coord & 7;
    int gen = (coord >> 4) & 3;
    int count = g_stage[stage].w & 0xFF;
    bool transform = count >= 2 && count <= 4;
    float4 tc;

    if (gen == 1) {
        tc = float4(viewPos, 1.0);
    } else if (gen == 2) {
        tc = float4(reflect(normalize(viewPos), viewNormal), 1.0);
    } else if (gen == 3) {
        tc = float4(viewNormal, 1.0);
    } else {
        tc = raw[index];
        // The coordinates are two-component; the first missing one is 1 under a transform, as Direct3D 9 pads them.
        tc.zw = transform ? float2(1.0, 0.0) : float2(0.0, 0.0);
    }

    if (transform) {
        tc = mul(tc, g_tex[stage]);
    }

    if (g_stage[stage].w & 0x100) {
        // Projected: the last counted component divides, in the pixel shader.
        float q = count == 4 ? tc.w : (count == 3 ? tc.z : tc.y);
        tc.w = q;
    } else {
        tc.w = 1.0;
    }

    return tc;
}

VsOut VsMain(VsIn input)
{
    VsOut o;
    float4 world = mul(float4(input.pos, 1.0), g_world);
    float4 view = mul(world, g_view);
    o.pos = mul(view, g_proj);

    float3 normal = mul(mul(float4(input.normal, 0.0), g_world), g_view).xyz;

    if (g_flags.w != 0) {
        normal = normalize(normal);
    }

    float4 c0 = g_flags2.x != 0 ? input.color0 : float4(1.0, 1.0, 1.0, 1.0);
    float4 c1 = g_flags2.y != 0 ? input.color1 : float4(0.0, 0.0, 0.0, 0.0);

    if (g_flags.x != 0) {
        float3 ambient = float3(0.0, 0.0, 0.0);
        float3 diffuse = float3(0.0, 0.0, 0.0);
        float3 specular = float3(0.0, 0.0, 0.0);
        float3 eye = normalize(view.xyz);

        [unroll]
        for (int i = 0; i < 4; i++) {
            if (g_lightAtten[i].w != 0.0) {
                float3 toLight;
                float atten = 1.0;

                if (g_lightPos[i].w != 0.0) {
                    float3 delta = g_lightPos[i].xyz - view.xyz;
                    float d = length(delta);
                    toLight = delta / max(d, 0.0001);
                    atten = 1.0 / max(g_lightAtten[i].x + d * (g_lightAtten[i].y + d * g_lightAtten[i].z), 0.0000001);
                } else {
                    toLight = normalize(-g_lightPos[i].xyz);
                }

                float nDotL = saturate(dot(normal, toLight));
                ambient += g_lightAmbient[i].rgb * atten;
                diffuse += g_lightDiffuse[i].rgb * nDotL * atten;

                if (g_flags2.w != 0 && nDotL > 0.0) {
                    float nDotH = saturate(dot(normal, normalize(toLight - eye)));

                    if (nDotH > 0.0) {
                        specular += g_lightSpecular[i].rgb * pow(nDotH, g_matSpecular.w) * atten;
                    }
                }
            }
        }

        int sources = g_flags2.z;
        float4 matAmbient = PickSource(sources & 3, g_matDiffuse, c0, c1);
        float4 matDiffuse = PickSource((sources >> 2) & 3, g_matDiffuse, c0, c1);
        float4 matSpecular = PickSource((sources >> 4) & 3, g_matSpecular, c0, c1);
        float4 matEmissive = PickSource((sources >> 6) & 3, g_matEmissive, c0, c1);

        float3 lit = matEmissive.rgb + matAmbient.rgb * (g_ambient.rgb + ambient) + matDiffuse.rgb * diffuse;
        o.diffuse = saturate(float4(lit, matDiffuse.a));
        o.specular = saturate(float4(matSpecular.rgb * specular, 0.0));
    } else {
        o.diffuse = c0;
        o.specular = c1;
    }

    float4 raw[8] = { input.tc0, input.tc1, input.tc2, input.tc3, input.tc4, input.tc5, input.tc6, input.tc7 };
    o.tc0 = StageCoord(0, raw, view.xyz, normal);
    o.tc1 = StageCoord(1, raw, view.xyz, normal);
    o.tc2 = StageCoord(2, raw, view.xyz, normal);
    o.tc3 = StageCoord(3, raw, view.xyz, normal);
    o.tc4 = StageCoord(4, raw, view.xyz, normal);
    o.tc5 = StageCoord(5, raw, view.xyz, normal);
    o.tc6 = StageCoord(6, raw, view.xyz, normal);
    o.tc7 = StageCoord(7, raw, view.xyz, normal);

    o.fog = float4(1.0, 0.0, 0.0, 0.0);

    if (g_flags.y != 0) {
        float range = g_fog.y - g_fog.x;
        o.fog.x = range != 0.0 ? saturate((g_fog.y - abs(view.z)) / range) : 1.0;
    }

    float c[6];

    [unroll]
    for (int p = 0; p < 6; p++) {
        c[p] = (g_clipMask.x & (1 << p)) ? dot(world, g_clip[p]) : 1.0;
    }

    o.clip0 = float4(c[0], c[1], c[2], c[3]);
    o.clip1 = float2(c[4], c[5]);
    return o;
}

Texture2D g_tex2d0 : register(t0);
Texture2D g_tex2d1 : register(t1);
Texture2D g_tex2d2 : register(t2);
Texture2D g_tex2d3 : register(t3);
Texture2D g_tex2d4 : register(t4);
Texture2D g_tex2d5 : register(t5);
Texture2D g_tex2d6 : register(t6);
Texture2D g_tex2d7 : register(t7);
TextureCube g_texCube0 : register(t16);
TextureCube g_texCube1 : register(t17);
TextureCube g_texCube2 : register(t18);
TextureCube g_texCube3 : register(t19);
TextureCube g_texCube4 : register(t20);
TextureCube g_texCube5 : register(t21);
TextureCube g_texCube6 : register(t22);
TextureCube g_texCube7 : register(t23);
SamplerState g_sampler0 : register(s0);
SamplerState g_sampler1 : register(s1);
SamplerState g_sampler2 : register(s2);
SamplerState g_sampler3 : register(s3);
SamplerState g_sampler4 : register(s4);
SamplerState g_sampler5 : register(s5);
SamplerState g_sampler6 : register(s6);
SamplerState g_sampler7 : register(s7);

float4 Combine(int op, float4 tex, float4 current, float4 diffuse)
{
    // GxTexOp: modulate, modulate 2x, add, current, blend by the current alpha, blend by the diffuse alpha.
    if (op == 0) {
        return tex * current;
    }

    if (op == 1) {
        return saturate(tex * current * 2.0);
    }

    if (op == 2) {
        return saturate(tex + current);
    }

    if (op == 3) {
        return current;
    }

    if (op == 4) {
        return current * current.a + tex * (1.0 - current.a);
    }

    return tex * diffuse.a + current * (1.0 - diffuse.a);
}

float4 Fetch2d(Texture2D t, SamplerState s, float4 tc)
{
    return t.Sample(s, tc.xy / tc.w);
}

float4 FetchCube(TextureCube t, SamplerState s, float4 tc)
{
    return t.Sample(s, tc.xyz);
}

#define STAGE(n) \
    if (active && g_stage[n].x >= 0) { \
        float4 tex = (g_clipMask.y & (1 << n)) ? FetchCube(g_texCube##n, g_sampler##n, input.tc##n) : Fetch2d(g_tex2d##n, g_sampler##n, input.tc##n); \
        float4 colorResult = Combine(g_stage[n].x, tex, current, input.diffuse); \
        float4 alphaResult = Combine(g_stage[n].y, tex, current, input.diffuse); \
        current = float4(colorResult.rgb, alphaResult.a); \
    } else { \
        active = false; \
    }

float4 PsMain(VsOut input) : SV_Target
{
    float4 current = input.diffuse;
    bool active = true;
    STAGE(0)
    STAGE(1)
    STAGE(2)
    STAGE(3)
    STAGE(4)
    STAGE(5)
    STAGE(6)
    STAGE(7)

    if (g_flags2.w != 0) {
        current.rgb = saturate(current.rgb + input.specular.rgb);
    }

    if (g_fogEnable.x != 0.0) {
        current.rgb = lerp(g_fogColor.rgb, current.rgb, input.fog.x);
    }

    // As the client's shaders test: the alpha rounded to 0-255 against the reference (greater or equal passes).
    if (g_alphaTest.x != 0.0 && floor(saturate(current.a) * 255.0 + 0.5) < g_alphaTest.y) {
        discard;
    }

    return current;
}
