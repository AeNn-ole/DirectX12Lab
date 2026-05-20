// ═══════════════════════════════════════════════════════════════════════════
// GBuffer.hlsl — Geometry Pass (проход 1 из 2)
//
// Задача: для каждого пикселя геометрии записать «сырые» данные поверхности
// в три текстуры G-Buffer ОДНОВРЕМЕННО через MRT (Multiple Render Targets).
//
// Никакого освещения здесь нет — только сбор данных.
// Освещение считается отдельно в Lighting.hlsl.
// ═══════════════════════════════════════════════════════════════════════════

// ── Текстура диффуза материала + сэмплер ─────────────────────────────────
Texture2D    gTexture : register(t0);
SamplerState gSampler : register(s0);

// ── Константный буфер (register b0) ──────────────────────────────────────
// Структура ИДЕНТИЧНА оригинальному Shaders.hlsl — никаких изменений
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
    // RT0: Albedo — RGB базовый цвет, A свободен
    float4 Albedo   : SV_TARGET0;

    // RT1: Normal — XYZ нормаль в world space.
    //   Нормаль лежит в [-1,1], а текстура R16G16B16A16_FLOAT хранит float без
    //   упаковки — просто пишем as-is. Если бы использовали UNORM, надо было бы
    //   ремапировать: n * 0.5 + 0.5 → [0,1]. С float — не нужно.
    float4 Normal   : SV_TARGET1;

    // RT2: Specular — RGB = Ks, A = Ns/255 (упакованный glossiness)
    float4 Specular : SV_TARGET2;
};

// ─────────────────────────────────────────────────────────────────────────────
// Вершинный шейдер
// Идентичен оригинальному VSMain — трансформируем позицию, нормаль, UV
// ─────────────────────────────────────────────────────────────────────────────
PSIn VSMain(VSIn vin)
{
    PSIn vout;

   
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW        = posW.xyz;
    vout.NormalW     = normalize(mul(vin.NormalL, (float3x3)gWorld));
    vout.PosH = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
    vout.TexCoord    = vin.TexCoord * gUVTiling + gUVOffset;

    return vout;
}

// ─────────────────────────────────────────────────────────────────────────────
// Пиксельный шейдер — записывает данные в три RT одновременно
// ─────────────────────────────────────────────────────────────────────────────
GBufferOutput PSMain(PSIn pin)
{
    GBufferOutput o;

    // ── RT0: Albedo ───────────────────────────────────────────────────────
    // Диффузная текстура * цвет материала Kd
    float4 texSample = gTexture.Sample(gSampler, pin.TexCoord);
    float3 albedo    = texSample.rgb * gDiffuse.rgb;
    o.Albedo         = float4(albedo, texSample.a);

    // ── RT1: Normal ───────────────────────────────────────────────────────
    // Нормализуем: интерполяция между вершинами нарушает единичную длину
    float3 N = normalize(pin.NormalW);
    // Записываем в float16 — без ремапинга, диапазон [-1,1] поддерживается
    o.Normal = float4(N, 0.0f);

    // ── RT2: Specular ─────────────────────────────────────────────────────
    // RGB = Ks (цвет блика), A = Ns/255 (shininess в [0,1])
    // 255 выбрана как практический максимум Ns в .mtl файлах
    float nsNorm = saturate(gSpecPower / 255.0f);
    o.Specular   = float4(gSpecular.rgb, nsNorm);

    return o;
}
