// ═══════════════════════════════════════════════════════════════════════════
// GBuffer.hlsl — Geometry Pass (проход 1 из 2)
//
// ИЗМЕНЕНИЕ (задание 1): единый cbuffer ObjectCB разбит на два:
//   PerInstanceCB (b0) — меняется на каждый экземпляр объекта
//   PerMaterialCB (b1) — меняется на каждый материал (N раз за кадр)
// ═══════════════════════════════════════════════════════════════════════════

Texture2D    gTexture : register(t0);
SamplerState gSampler : register(s0);

// ── b0: данные экземпляра (меняются на каждый draw call внешнего цикла) ──
// Соответствует RenderingSystem::PerInstanceCB
cbuffer PerInstanceCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldViewProj;

    float3   gEyePosW;   float _pad0;
    float3   gLightDirW; float _pad1;
};

// ── b1: свойства материала (меняются на каждый draw call внутреннего цикла) ──
// Соответствует RenderingSystem::PerMaterialCB
cbuffer PerMaterialCB : register(b1)
{
    float4   gAmbient;
    float4   gDiffuse;
    float4   gSpecular;
    float    gSpecPower; float3 _pad2;

    float    gTime;
    float3   _padTime;

    float2   gUVOffset;
    float2   gUVTiling;
};

// ── Вход вершинного шейдера ───────────────────────────────────────────────
struct VSIn
{
    float3 PosL     : POSITION;
    float3 NormalL  : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

// ── Интерполяты ───────────────────────────────────────────────────────────
struct PSIn
{
    float4 PosH     : SV_POSITION;
    float3 PosW     : POSITION;
    float3 NormalW  : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

// ── Выход пиксельного шейдера: три SV_TARGET = три RT G-Buffer ───────────
struct GBufferOutput
{
    float4 Albedo   : SV_TARGET0;
    float4 Normal   : SV_TARGET1;
    float4 Specular : SV_TARGET2;
};

// ─────────────────────────────────────────────────────────────────────────────
// Вершинный шейдер — без изменений по логике
// ─────────────────────────────────────────────────────────────────────────────
PSIn VSMain(VSIn vin)
{
    PSIn vout;
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW        = posW.xyz;
    vout.NormalW     = normalize(mul(vin.NormalL, (float3x3)gWorld));
    vout.PosH        = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
    vout.TexCoord    = vin.TexCoord * gUVTiling + gUVOffset;
    return vout;
}

// ─────────────────────────────────────────────────────────────────────────────
// Пиксельный шейдер — без изменений по логике
// ─────────────────────────────────────────────────────────────────────────────
GBufferOutput PSMain(PSIn pin)
{
    GBufferOutput o;

    float4 texSample = gTexture.Sample(gSampler, pin.TexCoord);
    float3 albedo    = texSample.rgb * gDiffuse.rgb;
    o.Albedo         = float4(albedo, texSample.a);

    float3 N = normalize(pin.NormalW);
    o.Normal = float4(N, 0.0f);

    float nsNorm = saturate(gSpecPower / 255.0f);
    o.Specular   = float4(gSpecular.rgb, nsNorm);

    return o;
}
