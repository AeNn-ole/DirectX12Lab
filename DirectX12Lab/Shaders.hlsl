// ── Текстура и сэмплер ────────────────────────────────────────────────────
Texture2D    gTexture : register(t0);
SamplerState gSampler : register(s0);

// ── Константный буфер (register b0) ──────────────────────────────────────
cbuffer ObjectCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldViewProj;

    float3   gEyePosW;   float _pad0;
    float3   gLightDirW; float _pad1;

    float4   gAmbient;
    float4   gDiffuse;
    float4   gSpecular;
    float    gSpecPower; float3 _pad2;

    float2   gUVOffset;   // анимационное смещение UV
    float2   gUVTiling;   // масштаб тайлинга
    
    float   gTime; // время для анимации
};

// ── Вход вершинного шейдера ───────────────────────────────────────────────
struct VSIn
{
    float3 PosL     : POSITION;
    float3 NormalL  : NORMAL;
    float2 TexCoord : TEXCOORD0;   // ← новое: UV из буфера вершин
};

// ── Интерполяты, передаваемые в пиксельный шейдер ────────────────────────
struct PSIn
{
    float4 PosH     : SV_POSITION;
    float3 PosW     : POSITION;
    float3 NormalW  : NORMAL;
    float2 TexCoord : TEXCOORD0;   // ← тайлинг + анимация применяются здесь
};

// ── Вершинный шейдер ──────────────────────────────────────────────────────
PSIn VSMain(VSIn vin)
{
    PSIn vout;
    
    float centerY = 0.0f; 

    float scaleY = 1.0f + sin(gTime * 35) * 0.6f;

    float3 animatedPos = vin.PosL;
    animatedPos.y *= scaleY;

    float4 posW = mul(float4(animatedPos, 1.0f), gWorld);
    vout.PosW     = posW.xyz;
    vout.NormalW  = normalize(mul(vin.NormalL, (float3x3)gWorld));
    vout.PosH = mul(float4(animatedPos, 1.0f), gWorldViewProj);

    // Применяем тайлинг и анимационный сдвиг к UV
    vout.TexCoord = vin.TexCoord * gUVTiling + gUVOffset;

    return vout;
}

// ── Пиксельный шейдер ────────────────────────────────────────────────────
float4 PSMain(PSIn pin) : SV_TARGET
{
    float3 N = normalize(pin.NormalW);
    float3 L = normalize(-gLightDirW);
    float3 V = normalize(gEyePosW - pin.PosW);

    // Базовый цвет: берём из текстуры
    float4 texSample = gTexture.Sample(gSampler, pin.TexCoord);
    float3 baseColor = texSample.rgb;

    // Освещение по Фонгу
    float ndotl = saturate(dot(N, L));
    float3 lit  = 0.18f + 0.82f * ndotl;

    float3 H    = normalize(L + V);
    float  spec = pow(saturate(dot(N, H)), gSpecPower);

    float3 color = baseColor * lit + spec.xxx * 0.35f;
    return float4(color, texSample.a);
}
