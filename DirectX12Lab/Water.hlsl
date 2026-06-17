// ═══════════════════════════════════════════════════════════════════════════
// Water.hlsl — Тесселированная анимированная поверхность воды
//
// Pipeline: VS → HS → DS → PS
//
// Геометрия — плоская сетка (создаётся процедурно на CPU, без UV/normal map).
// Высота волны и нормаль вычисляются АНАЛИТИЧЕСКИ в Domain Shader через
// сумму синусоид (Sum of Sines / Gerstner-style waves).
//
// Рендерится forward (не через G-Buffer) поверх уже готовой сцены,
// с alpha blending — вода прозрачная.
// ═══════════════════════════════════════════════════════════════════════════

cbuffer WaterCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldViewProj;
    float3   gEyePosW;   float gTime;
    float3   gLightDirW; float _pad0;

    // ── Параметры волн: до 4 волн, каждая = (dirX, dirZ, amplitude, frequency) ─
    float4   gWaveDir0Amp;   // (dirX, dirZ, amplitude, frequency)
    float4   gWaveDir1Amp;
    float4   gWaveDir2Amp;
    float4   gWaveDir3Amp;
    float4   gWaveSpeeds;    // speed для каждой из 4 волн

    float    gTessFactorNear;
    float    gTessFactorFar;
    float    gTessDistNear;
    float    gTessDistFar;

    float4   gShallowColor;  // цвет воды на мелководье/гребне
    float4   gDeepColor;     // цвет воды в глубине/впадине
};

// ─────────────────────────────────────────────────────────────────────────────
// VERTEX SHADER — просто передаёт позицию в мировое пространство
// ─────────────────────────────────────────────────────────────────────────────
struct VSIn
{
    float3 PosL : POSITION;   // плоская сетка в локальных координатах (Y=0)
};

struct VSOut
{
    float3 PosW : POSITION;
};

VSOut VSMain_Water(VSIn vin)
{
    VSOut o;
    float4 posW = mul(float4(vin.PosL, 1.f), gWorld);
    o.PosW = posW.xyz;
    return o;
}

// ─────────────────────────────────────────────────────────────────────────────
// HULL SHADER — тот же distance-adaptive принцип что и в GBuffer.hlsl
// ─────────────────────────────────────────────────────────────────────────────
struct HSOut
{
    float3 PosW : POSITION;
};

struct PatchTess
{
    float EdgeTess[3] : SV_TessFactor;
    float InsideTess  : SV_InsideTessFactor;
};

float WaterTessFactorByDist(float dist)
{
    float t = saturate((dist - gTessDistNear) / max(gTessDistFar - gTessDistNear, 0.001f));
    return lerp(gTessFactorNear, gTessFactorFar, t);
}

PatchTess ConstantHS_Water(InputPatch<VSOut, 3> patch, uint patchID : SV_PrimitiveID)
{
    PatchTess pt;

    float3 mid0 = (patch[1].PosW + patch[2].PosW) * 0.5f;
    float3 mid1 = (patch[2].PosW + patch[0].PosW) * 0.5f;
    float3 mid2 = (patch[0].PosW + patch[1].PosW) * 0.5f;
    float3 center = (patch[0].PosW + patch[1].PosW + patch[2].PosW) / 3.f;

    pt.EdgeTess[0] = WaterTessFactorByDist(distance(mid0, gEyePosW));
    pt.EdgeTess[1] = WaterTessFactorByDist(distance(mid1, gEyePosW));
    pt.EdgeTess[2] = WaterTessFactorByDist(distance(mid2, gEyePosW));
    pt.InsideTess  = WaterTessFactorByDist(distance(center, gEyePosW));

    return pt;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("ConstantHS_Water")]
[maxtessfactor(64.f)]
HSOut HSMain_Water(InputPatch<VSOut, 3> patch,
                    uint i       : SV_OutputControlPointID,
                    uint patchID : SV_PrimitiveID)
{
    HSOut o;
    o.PosW = patch[i].PosW;
    return o;
}

// ─────────────────────────────────────────────────────────────────────────────
// Sum-of-Sines: высота и аналитический градиент волны в точке (x, z)
//
// height(x,z,t) = Σ Ai * sin(dot(Di,(x,z)) * wi + t * speedi)
//
// Градиент нужен для вычисления нормали без соседних вершин:
//   dH/dx = Σ Ai * wi * Di.x * cos(...)
//   dH/dz = Σ Ai * wi * Di.z * cos(...)
//   N = normalize(-dH/dx, 1, -dH/dz)
// ─────────────────────────────────────────────────────────────────────────────
void EvalWaves(float2 posXZ, float time,
               float4 waveA, float4 waveB, float4 waveC, float4 waveD,
               float4 speeds,
               out float height, out float2 gradient)
{
    height   = 0.f;
    gradient = float2(0.f, 0.f);

    float4 waves[4] = { waveA, waveB, waveC, waveD };

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float2 dir  = normalize(waves[i].xy);
        float  amp  = waves[i].z;
        float  freq = waves[i].w;
        float  spd  = speeds[i];

        float phase = dot(dir, posXZ) * freq + time * spd;
        float s     = sin(phase);
        float c     = cos(phase);

        height      += amp * s;
        gradient    += amp * freq * dir * c;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DOMAIN SHADER — барицентрическая интерполяция + смещение по волне
// ─────────────────────────────────────────────────────────────────────────────
struct PSIn
{
    float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
    float  WaveH   : TEXCOORD0;   // высота волны в этой точке (для цвета)
};

[domain("tri")]
PSIn DSMain_Water(PatchTess pt,
                   float3 uvw : SV_DomainLocation,
                   const OutputPatch<HSOut, 3> patch)
{
    PSIn o;

    // Интерполируем базовую (плоскую) позицию
    float3 posW = uvw.x * patch[0].PosW + uvw.y * patch[1].PosW + uvw.z * patch[2].PosW;

    // Вычисляем волну в этой XZ точке
    float  height;
    float2 gradient;
    EvalWaves(posW.xz, gTime,
              gWaveDir0Amp, gWaveDir1Amp, gWaveDir2Amp, gWaveDir3Amp,
              gWaveSpeeds, height, gradient);

    // Смещаем по Y
    posW.y += height;

    // Аналитическая нормаль из градиента
    float3 normalW = normalize(float3(-gradient.x, 1.f, -gradient.y));

    o.PosH    = mul(float4(posW, 1.f), gWorldViewProj);
    o.PosW    = posW;
    o.NormalW = normalW;
    o.WaveH   = height;
    return o;
}

// ─────────────────────────────────────────────────────────────────────────────
// PIXEL SHADER — простое освещение + Fresnel + цвет по высоте волны
// ─────────────────────────────────────────────────────────────────────────────
float4 PSMain_Water(PSIn pin) : SV_TARGET
{
    float3 N = normalize(pin.NormalW);
    float3 V = normalize(gEyePosW - pin.PosW);
    float3 L = normalize(-gLightDirW);

    // ── Diffuse + specular (Blinn-Phong, упрощённо) ──────────────────────
    float  NdotL = saturate(dot(N, L));
    float3 H     = normalize(L + V);
    float  spec  = pow(saturate(dot(N, H)), 64.f);

    // ── Цвет воды: смешиваем deep/shallow по высоте волны ────────────────
    // Гребень волны (height > 0) — светлее (shallowColor), впадина — темнее (deepColor)
    float waveT   = saturate(pin.WaveH * 0.5f + 0.5f);
    float3 baseColor = lerp(gDeepColor.rgb, gShallowColor.rgb, waveT);

    // ── Fresnel — вода ярче/прозрачнее под острым углом обзора ───────────
    float fresnel = pow(1.f - saturate(dot(N, V)), 5.f);
    fresnel = lerp(0.05f, 1.f, fresnel); // минимум 5% даже при взгляде сверху

    float3 color = baseColor * (0.25f + NdotL * 0.6f) + spec * 0.8f;
    color = lerp(color, float3(1.f, 1.f, 1.f), fresnel * 0.3f); // блик неба на гребнях

    // Полупрозрачность — меньше альфа при взгляде сверху (видно дно),
    // больше альфа (более непрозрачно) под острым углом (Fresnel)
    float alpha = lerp(0.55f, 0.95f, fresnel);

    return float4(color, alpha);
}
