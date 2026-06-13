// ═══════════════════════════════════════════════════════════════════════════
// GBuffer.hlsl — Geometry Pass с тесселяцией, displacement и normal mapping
//
// Pipeline: VS → HS → DS → PS
//
// Регистры:
//   b0 — PerInstanceCB  (World, WVP, EyePos, LightDir)
//   b1 — PerMaterialCB  (материал + параметры тесселяции)
//   t0 — albedo texture
//   t1 — normal map
//   t2 — displacement map
//   s0 — wrap sampler   (VS/HS/DS/PS — geometry pass)
//   s1 — point sampler  (DS — SampleLevel для displacement)
// ═══════════════════════════════════════════════════════════════════════════

// ── Текстуры ─────────────────────────────────────────────────────────────────
Texture2D gAlbedoTex : register(t0);
Texture2D gNormalMap : register(t1);
Texture2D gDispMap   : register(t2);

SamplerState gWrapSampler  : register(s0);  // для albedo / normal
SamplerState gPointSampler : register(s1);  // для displacement (SampleLevel)

// ── Константные буферы ────────────────────────────────────────────────────────
cbuffer PerInstanceCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldViewProj;
    float3   gEyePosW;   float _pad0;
    float3   gLightDirW; float _pad1;
};

cbuffer PerMaterialCB : register(b1)
{
    float4 gAmbient;
    float4 gDiffuse;
    float4 gSpecular;
    float  gSpecPower; float3 _pad2;

    float  gTime;
    float3 _padTime;

    float2 gUVOffset;
    float2 gUVTiling;

    // Параметры тесселяции
    float gTessFactorNear;    // максимальный фактор (рядом)
    float gTessFactorFar;     // минимальный фактор (далеко)
    float gTessDistNear;      // дистанция «рядом»
    float gTessDistFar;       // дистанция «далеко»

    float gDisplacementScale; // амплитуда смещения
    int   gEnableNormalMap;   // 1 = применять normal map
    float2 _padTess;
};

// ════════════════════════════════════════════════════════════════════════════
// VERTEX SHADER
// В tessellation pipeline VS только преобразует вершины в мировое пространство
// (world-space) и передаёт дальше. Перспективную проекцию делает Domain Shader.
// ════════════════════════════════════════════════════════════════════════════
struct VSIn
{
    float3 PosL     : POSITION;
    float3 NormalL  : NORMAL;
    float3 TangentL : TANGENT;
    float2 TexCoord : TEXCOORD0;
};

// Данные патча (то что VS → HS передаёт на каждую control point)
struct VSOut
{
    float3 PosW     : POSITION;    // позиция в world space
    float3 NormalW  : NORMAL;      // нормаль в world space
    float3 TangentW : TANGENT;     // касательная в world space
    float2 TexCoord : TEXCOORD0;
};

VSOut VSMain(VSIn vin)
{
    VSOut o;

    float4 posW  = mul(float4(vin.PosL, 1.f), gWorld);
    o.PosW       = posW.xyz;

    // Трансформируем нормаль и тангент через обратно-транспонированную матрицу
    // (для равномерного масштаба — просто (float3x3)gWorld)
    float3x3 W3  = (float3x3)gWorld;
    o.NormalW    = normalize(mul(vin.NormalL,  W3));
    o.TangentW   = normalize(mul(vin.TangentL, W3));

    o.TexCoord   = vin.TexCoord * gUVTiling + gUVOffset;
    return o;
}

// ════════════════════════════════════════════════════════════════════════════
// HULL SHADER
//
// Два компонента:
//   1. ConstantHS  — вычисляет Tessellation Factors для патча
//   2. HSMain      — проходит control points без изменений (pass-through)
//
// Tessellation Factor зависит от расстояния: рядом — высокий, далеко — низкий.
// Это distance-adaptive tessellation из лекции.
// ════════════════════════════════════════════════════════════════════════════

struct HSOut
{
    float3 PosW     : POSITION;
    float3 NormalW  : NORMAL;
    float3 TangentW : TANGENT;
    float2 TexCoord : TEXCOORD0;
};

struct PatchTess
{
    float EdgeTess[3]  : SV_TessFactor;
    float InsideTess   : SV_InsideTessFactor;
};

// ── Вспомогательная функция: тесс-фактор по дистанции ────────────────────
float TessFactorByDist(float dist)
{
    float t = saturate((dist - gTessDistNear) / max(gTessDistFar - gTessDistNear, 0.001f));
    return lerp(gTessFactorNear, gTessFactorFar, t);
}

// ── Constant Hull Shader — вызывается один раз на патч ───────────────────
PatchTess ConstantHS(InputPatch<VSOut, 3> patch, uint patchID : SV_PrimitiveID)
{
    PatchTess pt;

    // Центр патча в мировом пространстве
    float3 center = (patch[0].PosW + patch[1].PosW + patch[2].PosW) / 3.f;

    // Back-face culling: нормаль патча vs вектор на камеру
    // Если весь патч смотрит от камеры — тесселяция = 0 (патч отбрасывается)
    float3 patchNormal = normalize(patch[0].NormalW + patch[1].NormalW + patch[2].NormalW);
    float3 toEye       = normalize(gEyePosW - center);
    // Небольшой epsilon (-0.25) чтобы избежать popping на силуэтах
    if (dot(patchNormal, toEye) < -0.25f)
    {
        pt.EdgeTess[0] = pt.EdgeTess[1] = pt.EdgeTess[2] = 0.f;
        pt.InsideTess  = 0.f;
        return pt;
    }

    // Distance-adaptive: тесс-фактор по расстоянию от каждого ребра
    // Ребро 0: вершины 1-2, ребро 1: вершины 2-0, ребро 2: вершины 0-1
    float3 mid0 = (patch[1].PosW + patch[2].PosW) * 0.5f;
    float3 mid1 = (patch[2].PosW + patch[0].PosW) * 0.5f;
    float3 mid2 = (patch[0].PosW + patch[1].PosW) * 0.5f;

    pt.EdgeTess[0] = TessFactorByDist(distance(mid0, gEyePosW));
    pt.EdgeTess[1] = TessFactorByDist(distance(mid1, gEyePosW));
    pt.EdgeTess[2] = TessFactorByDist(distance(mid2, gEyePosW));
    pt.InsideTess  = TessFactorByDist(distance(center, gEyePosW));

    return pt;
}

// ── Hull Shader — pass-through control points ─────────────────────────────
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("ConstantHS")]
[maxtessfactor(64.f)]
HSOut HSMain(InputPatch<VSOut, 3> patch,
             uint i             : SV_OutputControlPointID,
             uint patchID       : SV_PrimitiveID)
{
    HSOut o;
    o.PosW     = patch[i].PosW;
    o.NormalW  = patch[i].NormalW;
    o.TangentW = patch[i].TangentW;
    o.TexCoord = patch[i].TexCoord;
    return o;
}

// ════════════════════════════════════════════════════════════════════════════
// DOMAIN SHADER
//
// Интерполирует атрибуты по барицентрическим координатам u,v,w.
// Сэмплирует displacement map и смещает позицию вдоль нормали.
// Применяет перспективную проекцию (WorldViewProj).
// ════════════════════════════════════════════════════════════════════════════

struct PSIn
{
    float4 PosH     : SV_POSITION;
    float3 PosW     : POSITION;
    float3 NormalW  : NORMAL;
    float3 TangentW : TANGENT;
    float2 TexCoord : TEXCOORD0;
};

[domain("tri")]
PSIn DSMain(PatchTess pt,
            float3 uvw : SV_DomainLocation,
            const OutputPatch<HSOut, 3> patch)
{
    PSIn o;

    // Барицентрическая интерполяция (u=uvw.x, v=uvw.y, w=uvw.z)
    float3 posW  = uvw.x * patch[0].PosW     + uvw.y * patch[1].PosW     + uvw.z * patch[2].PosW;
    float3 normW = uvw.x * patch[0].NormalW  + uvw.y * patch[1].NormalW  + uvw.z * patch[2].NormalW;
    float3 tanW  = uvw.x * patch[0].TangentW + uvw.y * patch[1].TangentW + uvw.z * patch[2].TangentW;
    float2 uv    = uvw.x * patch[0].TexCoord + uvw.y * patch[1].TexCoord + uvw.z * patch[2].TexCoord;

    normW = normalize(normW);
    tanW  = normalize(tanW);

    // Displacement: смещаем позицию вдоль нормали по карте высот
    // SampleLevel с mip 0 — избегаем градиентов в DS (нет ddx/ddy)
    float disp = gDispMap.SampleLevel(gPointSampler, uv, 0).r;
    posW += normW * (disp * gDisplacementScale);

    o.PosH     = mul(float4(posW, 1.f), gWorldViewProj);
    o.PosW     = posW;
    o.NormalW  = normW;
    o.TangentW = tanW;
    o.TexCoord = uv;
    return o;
}

// ════════════════════════════════════════════════════════════════════════════
// PIXEL SHADER
//
// Записывает в G-Buffer. Применяет normal mapping если включено:
// читает TBN-нормаль из normal map и преобразует в world space.
// ════════════════════════════════════════════════════════════════════════════

struct GBufferOutput
{
    float4 Albedo   : SV_TARGET0;   // RGB = albedo
    float4 Normal   : SV_TARGET1;   // XYZ = world-space normal
    float4 Specular : SV_TARGET2;   // RGB = Ks, A = Ns/255
};

GBufferOutput PSMain(PSIn pin)
{
    GBufferOutput o;

    // ── Albedo ────────────────────────────────────────────────────────────
    float4 texSample = gAlbedoTex.Sample(gWrapSampler, pin.TexCoord);
    float3 albedo    = texSample.rgb * gDiffuse.rgb;
    o.Albedo         = float4(albedo, texSample.a);

    // ── Normal mapping ────────────────────────────────────────────────────
    float3 N = normalize(pin.NormalW);

    if (gEnableNormalMap)
    {
        // Образец из normal map в tangent space: [0..1] → [-1..1]
        float3 tNormal = gNormalMap.Sample(gWrapSampler, pin.TexCoord).xyz;
        tNormal        = tNormal * 2.f - 1.f;

        // TBN матрица: T, B, N в world space
        float3 T = normalize(pin.TangentW);
        // Re-orthogonalize T vs N (на случай если после интерполяции потеряна ортогональность)
        T = normalize(T - dot(T, N) * N);
        float3 B = cross(N, T);  // bitangent

        // Трансформируем нормаль из tangent в world space
        N = normalize(tNormal.x * T + tNormal.y * B + tNormal.z * N);
    }

    o.Normal = float4(N, 0.f);

    // ── Specular ──────────────────────────────────────────────────────────
    float nsNorm = saturate(gSpecPower / 255.f);
    o.Specular   = float4(gSpecular.rgb, nsNorm);

    return o;
}
