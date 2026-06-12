// ═══════════════════════════════════════════════════════════════════════════
// Lighting.hlsl — Lighting Pass
// Читает G-Buffer, считает освещение от N источников трёх типов.
// ═══════════════════════════════════════════════════════════════════════════

Texture2D gAlbedo   : register(t0);
Texture2D gNormal   : register(t1);
Texture2D gSpecular : register(t2);
Texture2D gDepth    : register(t3);

SamplerState gSampler : register(s0);

// ─────────────────────────────────────────────────────────────────────────────
// Структура источника света — БАЙТ В БАЙТ совпадает с Light в RenderingSystem.h
// ─────────────────────────────────────────────────────────────────────────────
struct Light
{
    float3 Position;   float Range;       // 16 байт
    float3 Direction;  float SpotAngle;   // 16 байт  (SpotAngle = cos(halfAngle))
    float4 Color;                         // 16 байт  (RGB = цвет, A = интенсивность)
    int    Type;       float3 _pad;       // 16 байт  (0=Dir, 1=Point, 2=Spot)
};

// ── Lighting CB ───────────────────────────────────────────────────────────────
// Источники света больше НЕ хранятся здесь — constant buffer слишком мал
// для большого количества источников (дождь из 200 точечных огней).
cbuffer LightingCB : register(b0)
{
    float4x4 gInvViewProj;
    float3   gEyePosW;    float _p0;
    float2   gScreenSize; int gNumLights; float _p1;
};

// Источники света — StructuredBuffer (t4), читается напрямую из памяти,
// размер не ограничен constant buffer'ом (16 источников было слишком мало).
StructuredBuffer<Light> gLights : register(t4);

// ─────────────────────────────────────────────────────────────────────────────
// Реконструкция мировой позиции из UV и глубины
// ─────────────────────────────────────────────────────────────────────────────
float3 ReconstructWorldPos(float2 uv, float depth)
{
    float ndcX =  uv.x * 2.0f - 1.0f;
    float ndcY = -uv.y * 2.0f + 1.0f;
    float4 clipPos  = float4(ndcX, ndcY, depth, 1.0f);
    float4 worldPos = mul(clipPos, gInvViewProj);
    worldPos /= worldPos.w;
    return worldPos.xyz;
}

// ─────────────────────────────────────────────────────────────────────────────
// Затухание точечного/прожекторного источника по дистанции:
//   1 / (1 + d²/Range²)
//   При d=0     → 1.0,  при d=Range → 0.5,  плавно гаснет за Range
// ─────────────────────────────────────────────────────────────────────────────
float Attenuation(float dist, float range)
{
    float s = dist / range;
    return saturate(1.0f / (1.0f + s * s));
}

// ─────────────────────────────────────────────────────────────────────────────
// Phong BRDF — общая функция для всех типов.
// L и V уже нормализованы снаружи.
// ─────────────────────────────────────────────────────────────────────────────
float3 PhongBRDF(float3 albedo, float3 N, float3 L, float3 V,
                 float3 Ks, float Ns, float4 lightColor)
{
    float  NdotL    = saturate(dot(N, L));
    float3 diffuse  = albedo * NdotL;

    float3 H        = normalize(L + V);
    float  spec     = (Ns > 0.0f) ? pow(saturate(dot(N, H)), Ns) : 0.0f;
    float3 specular = Ks * spec;

    return (diffuse + specular) * lightColor.rgb * lightColor.a;
}

// ─────────────────────────────────────────────────────────────────────────────
// Вклад одного источника
// ─────────────────────────────────────────────────────────────────────────────
float3 CalcLight(Light light,
                 float3 posW, float3 N, float3 V,
                 float3 albedo, float3 Ks, float Ns)
{
    if (light.Type == 0)
    {
        // ── Directional ───────────────────────────────────────────────────
        // Direction = куда светит источник (вниз = {0,-1,0}).
        // L = вектор НА источник = -Direction.
        float3 L = normalize(-light.Direction);
        return PhongBRDF(albedo, N, L, V, Ks, Ns, light.Color);
    }
    else if (light.Type == 1)
    {
        // ── Point ─────────────────────────────────────────────────────────
        float3 toLight = light.Position - posW;
        float  dist    = length(toLight);
        float3 L       = toLight / dist;

        float atten = Attenuation(dist, light.Range);
        if (atten < 0.001f) return (float3)0;

        return PhongBRDF(albedo, N, L, V, Ks, Ns, light.Color) * atten;
    }
    else // Type == 2: Spot
    {
        // ── Spot ──────────────────────────────────────────────────────────
        float3 toLight  = light.Position - posW;
        float  dist     = length(toLight);
        float3 L        = toLight / dist;

        // Угол между осью конуса и вектором к пикселю
        float3 spotDir  = normalize(light.Direction);
        float  cosTheta = dot(-L, spotDir); // -L: от источника к пикселю

        if (cosTheta < light.SpotAngle) return (float3)0; // за конусом

        // Плавный falloff на краю конуса
        float innerAngle = light.SpotAngle + (1.0f - light.SpotAngle) * 0.1f;
        float spotFactor = smoothstep(light.SpotAngle, innerAngle, cosTheta);

        float atten = Attenuation(dist, light.Range) * spotFactor;
        if (atten < 0.001f) return (float3)0;

        return PhongBRDF(albedo, N, L, V, Ks, Ns, light.Color) * atten;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Вершинный шейдер — fullscreen triangle из SV_VertexID (без VB)
// ─────────────────────────────────────────────────────────────────────────────
struct LightVSOut
{
    float4 PosH : SV_POSITION;
    float2 UV   : TEXCOORD0;
};

LightVSOut VSMain_Light(uint vertexID : SV_VertexID)
{
    float2 pos[3] = { float2(-1.f, 3.f), float2(-1.f,-1.f), float2(3.f,-1.f) };
    float2 uv[3]  = { float2( 0.f,-1.f), float2( 0.f, 1.f), float2(2.f, 1.f) };

    LightVSOut o;
    o.PosH = float4(pos[vertexID], 0.f, 1.f);
    o.UV   = uv[vertexID];
    return o;
}

// ─────────────────────────────────────────────────────────────────────────────
// Пиксельный шейдер — цикл по всем источникам
// ─────────────────────────────────────────────────────────────────────────────
float4 PSMain_Light(LightVSOut pin) : SV_TARGET
{
    float2 uv = pin.UV;

    float4 albedoSamp   = gAlbedo.Sample(gSampler, uv);
    float4 normalSamp   = gNormal.Sample(gSampler, uv);
    float4 specularSamp = gSpecular.Sample(gSampler, uv);
    float  depth        = gDepth.Sample(gSampler, uv).r;

    // Фон
    if (depth >= 1.0f)
        return float4(0.05f, 0.05f, 0.08f, 1.f);

    // Распаковка G-Buffer
    float3 albedo = albedoSamp.rgb;
    float3 N      = normalize(normalSamp.xyz);
    float3 Ks     = specularSamp.rgb;
    float  Ns     = specularSamp.a * 255.0f;

    float3 posW   = ReconstructWorldPos(uv, depth);
    float3 V      = normalize(gEyePosW - posW);

    // Ambient — чтобы тени не были абсолютно чёрными
    float3 color = albedo * 0.04f;

    // Цикл по всем источникам
    for (int i = 0; i < gNumLights; ++i)
        color += CalcLight(gLights[i], posW, N, V, albedo, Ks, Ns);

    // Reinhard tonemapping — предотвращает пересвет при суммировании источников
    color = color / (color + 1.0f);

    return float4(color, 1.0f);
}
