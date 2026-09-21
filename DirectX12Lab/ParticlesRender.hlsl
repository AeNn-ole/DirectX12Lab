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
    float3   gCameraRight; float _p0; // не используются для меш-инстансинга,
    float3   gCameraUp;    float _p1; // оставлены для совместимости layout'а CB с CPU
    float3   gEyePosW;     float _p2;
    float3   gLightDirW;   float _p3;
    float4   gAmbientColor;
};

StructuredBuffer<uint>      gAliveIndices : register(t0);
StructuredBuffer<Particle>  gPool         : register(t1);

// ── VS ───────────────────────────────────────────────────────────────────
struct VSIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
};

struct VSOut
{
    float4 PosH    : SV_POSITION;
    float3 PosW    : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float4 Color   : COLOR0;
};

// Поворот вокруг оси Y — используется, чтобы каждый "чайник" крутился
// по мере полёта; угол берётся из Age (детерминированно, без доп. состояния)
float3x3 RotateY(float a)
{
    float s = sin(a), c = cos(a);
    return float3x3(
        c, 0.0f,  s,
        0.0f, 1.0f, 0.0f,
       -s, 0.0f,  c);
}

VSOut VSMain_Particle(VSIn vin, uint instanceId : SV_InstanceID)
{
    uint idx = gAliveIndices[instanceId];
    Particle p = gPool[idx];

    float3x3 rot = RotateY(p.Age * 3.0f);
    // 0.6 — подгонка масштаба под прежний визуальный размер billboard-частиц;
    // если p.Size == 0 (вырожденный слот, см. пояснение про safe OOB Consume
    // в ParticlesSim.hlsl) — меш схлопывается в точку и просто не рисуется.
    float scale = p.Size * 0.6f;

    float3 posL = mul(vin.PosL, rot) * scale;
    float3 posW = posL + p.Position;

    VSOut o;
    o.PosW    = posW;
    o.NormalW = mul(vin.NormalL, rot); // без неоднородного масштаба обычная матрица поворота годится и для нормалей
    o.PosH    = mul(float4(posW, 1.0f), gViewProj);
    o.Color   = p.Color;
    return o;
}

// ── PS ───────────────────────────────────────────────────────────────────
float4 PSMain_Particle(VSOut i) : SV_TARGET
{
    float3 N = normalize(i.NormalW);
    float3 L = normalize(-gLightDirW);
    float  ndotl = saturate(dot(N, L));

    float3 lit = i.Color.rgb * (gAmbientColor.rgb + ndotl * (1.0f - gAmbientColor.rgb));
    return float4(lit, 1.0f);
}
