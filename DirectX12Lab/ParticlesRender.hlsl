// ═══════════════════════════════════════════════════════════════════════════
// ParticlesRender.hlsl — рендер частиц геометрическим шейдером (Homework #6)
//
// Draw без VB/IB: DrawInstancedIndirect(POINTLIST), SV_VertexID = 0..N-1,
// N берётся с GPU из gDrawArgs (см. ParticlesSim.hlsl::CSBuildIndirectArgs).
//
// VS   — по SV_VertexID достаёт реальный индекс частицы из gAliveIndices (t0)
// GS   — по индексу читает Particle из gPool (t1), строит billboard-квад,
//        развёрнутый к камере через CameraRight/CameraUp
// PS   — простое forward-освещение (Ambient + N·L), N = направление на
//        камеру (billboard), плюс круглая alpha-test маска через clip()
//        (это НЕ alpha-blend: частицы остаются полностью непрозрачными —
//        пишут в depth, PSO без blend, по условию задания)
// ═══════════════════════════════════════════════════════════════════════════

struct Particle
{
    float3 Position;
    float  Age;
    float3 Velocity;
    float  Lifetime;
    float4 Color;
    float  Size;
    float3 _pad;
};

cbuffer ParticleRenderCB : register(b0)
{
    float4x4 gViewProj;
    float3   gCameraRight; float _p0;
    float3   gCameraUp;    float _p1;
    float3   gEyePosW;     float _p2;
    float3   gLightDirW;   float _p3;
    float4   gAmbientColor;
};

StructuredBuffer<uint>      gAliveIndices : register(t0);
StructuredBuffer<Particle>  gPool         : register(t1);

// ── VS ───────────────────────────────────────────────────────────────────
struct VSOut
{
    uint ParticleIdx : PARTICLE_INDEX;
};

VSOut VSMain_Particle(uint vid : SV_VertexID)
{
    VSOut o;
    o.ParticleIdx = gAliveIndices[vid];
    return o;
}

// ── GS ───────────────────────────────────────────────────────────────────
struct GSOut
{
    float4 PosH  : SV_POSITION;
    float2 UV    : TEXCOORD0;
    float4 Color : COLOR0;
    float3 PosW  : TEXCOORD1;
};

// point-list — на вход один вертекс на примитив
[maxvertexcount(4)]
void GSMain_Particle(point VSOut input[1], inout TriangleStream<GSOut> stream)
{
    Particle p = gPool[input[0].ParticleIdx];

    // Частица уже мертва / слот пуст (Age==Lifetime==0, см. пояснение
    // про safe out-of-bounds Consume в ParticlesSim.hlsl) — не рисуем.
    if (p.Size <= 0.0f)
        return;

    float half_ = p.Size * 0.5f;
    float3 right = gCameraRight * half_;
    float3 up    = gCameraUp    * half_;

    float3 corners[4] =
    {
        p.Position - right - up,  // bottom-left
        p.Position - right + up,  // top-left
        p.Position + right - up,  // bottom-right
        p.Position + right + up,  // top-right
    };
    float2 uvs[4] = { float2(0, 1), float2(0, 0), float2(1, 1), float2(1, 0) };

    GSOut o;
    o.Color = p.Color;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        o.PosW = corners[i];
        o.PosH = mul(float4(corners[i], 1.0f), gViewProj);
        o.UV   = uvs[i];
        stream.Append(o);
    }
}

// ── PS ───────────────────────────────────────────────────────────────────
float4 PSMain_Particle(GSOut i) : SV_TARGET
{
    // Круглая alpha-test маска (не blend!) — билборд остаётся квадратом
    // геометрически, но "дырявые" углы отбрасываются через clip().
    float2 c = i.UV * 2.0f - 1.0f;
    float  d = dot(c, c);
    clip(1.0f - d);

    // Billboard всегда обращён к камере → нормаль = направление к глазу
    float3 N = normalize(gEyePosW - i.PosW);
    float3 L = normalize(-gLightDirW);
    float  ndotl = saturate(dot(N, L));

    float3 lit = i.Color.rgb * (gAmbientColor.rgb + ndotl * (1.0f - gAmbientColor.rgb));

    // Мягкое затемнение к краю диска — чисто косметика
    float rim = saturate(1.0f - d);
    lit *= lerp(0.55f, 1.0f, rim);

    return float4(lit, 1.0f);
}
