// ═══════════════════════════════════════════════════════════════════════════
// Lighting.hlsl — Lighting Pass с Cascaded Shadow Maps + PCF
//
// G-Buffer входы:  t0=Albedo, t1=Normal, t2=Specular, t3=Depth
// Shadow Map:      t4=Texture2DArray (kCsmCascades слоёв)
// ═══════════════════════════════════════════════════════════════════════════

Texture2D       gAlbedo    : register(t0);
Texture2D       gNormal    : register(t1);
Texture2D       gSpecular  : register(t2);
Texture2D       gDepth     : register(t3);
Texture2DArray  gShadowMap : register(t4); // массив shadow map по каскадам

SamplerState           gSampler       : register(s0); // point clamp (G-Buffer)
SamplerComparisonState gShadowSampler : register(s1); // comparison sampler для PCF

// ─────────────────────────────────────────────────────────────────────────────
// Структура источника света — БАЙТ В БАЙТ совпадает с Light в RenderingSystem.h
// ─────────────────────────────────────────────────────────────────────────────
struct Light
{
    float3 Position;   float Range;
    float3 Direction;  float SpotAngle;
    float4 Color;
    int    Type;       float3 _pad;
};

#define MAX_CASCADES 4

// ── Lighting CB ───────────────────────────────────────────────────────────────
cbuffer LightingCB : register(b0)
{
    float4x4 gInvViewProj;
    float3   gEyePosW;    float _p0;
    float2   gScreenSize; int gNumLights; float _p1;
    float3   gCameraForward; float _p1b; // forward вектор камеры (для view-Z каскадов)
    Light    gLights[16];

    // ── CSM данные ────────────────────────────────────────────────────────
    float4x4 gLightViewProj[MAX_CASCADES]; // матрицы каскадов (light space)
    float4   gCascadeFarPlanes;            // дистанции разбиения (view space Z)
    int      gNumCascades;                 // реальное число каскадов
    int      gDebugCascades;               // 1 = цветовая маркировка каскадов
    float2   _p2;
    float    gShadowMapSize;               // ширина/высота shadow map (для PCF offset)
    float    gShadowBias;                  // depth bias против acne
    float2   _p3;
};

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
// PCF — Percentage Closer Filtering
//
// Сэмплируем shadow map в сетке kernelSize×kernelSize вокруг точки.
// SampleCmpLevelZero возвращает 0 или 1 для каждого теселя, затем
// линейно интерполирует — это и есть PCF от GPU hardware.
// Мы дополнительно усредняем по сетке для большего размытия краёв.
//
// cascade — индекс слоя Texture2DArray
// shadowUV — UV в [0..1] в shadow map
// compareDepth — глубина пикселя в light space (с bias уже вычтенным)
// ─────────────────────────────────────────────────────────────────────────────
float SampleShadowPCF(int cascade, float2 shadowUV, float compareDepth)
{
    float shadow    = 0.f;
    float texelSize = 1.f / gShadowMapSize;

    // 3×3 ядро
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texelSize;
            shadow += gShadowMap.SampleCmpLevelZero(
                gShadowSampler,
                float3(shadowUV + offset, (float)cascade),
                compareDepth);
        }
    }
    return shadow / 9.f; // среднее по 9 сэмплам
}

// ─────────────────────────────────────────────────────────────────────────────
// CSM lookup — выбор каскада и вычисление shadow factor [0..1]
//   0 = полностью в тени, 1 = полностью освещён
// ─────────────────────────────────────────────────────────────────────────────
float CalcShadow(float3 posW, float viewDepth, out int cascadeIndex)
{
    // Выбираем каскад по глубине в view space
    cascadeIndex = gNumCascades - 1; // fallback — самый дальний
    [unroll]
    for (int i = 0; i < MAX_CASCADES; ++i)
    {
        if (i < gNumCascades && viewDepth < gCascadeFarPlanes[i])
        {
            cascadeIndex = i;
            break;
        }
    }

    // Трансформируем позицию в light clip space данного каскада
    float4 lightClip = mul(float4(posW, 1.f), gLightViewProj[cascadeIndex]);

    // Перспективное деление (для ortho = noop, но оставим для корректности)
    float3 ndcL = lightClip.xyz / lightClip.w;

    // NDC → UV: X: [-1,1]→[0,1], Y: [1,-1]→[0,1] (Y перевёрнут в D3D)
    float2 shadowUV = float2(ndcL.x * 0.5f + 0.5f,
                            -ndcL.y * 0.5f + 0.5f);

    // Пиксели вне shadow map — считаем освещёнными (нет тени за фрустумом)
    if (any(shadowUV < 0.f) || any(shadowUV > 1.f))
        return 1.f;

    // Глубина пикселя в light space минус bias (против shadow acne)
    float compareDepth = ndcL.z - gShadowBias;

    return SampleShadowPCF(cascadeIndex, shadowUV, compareDepth);
}

// ─────────────────────────────────────────────────────────────────────────────
// Затухание точечного/прожекторного источника
// ─────────────────────────────────────────────────────────────────────────────
float Attenuation(float dist, float range)
{
    float s = dist / range;
    return saturate(1.0f / (1.0f + s * s));
}

// ─────────────────────────────────────────────────────────────────────────────
// Phong BRDF
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
        float3 L = normalize(-light.Direction);
        return PhongBRDF(albedo, N, L, V, Ks, Ns, light.Color);
    }
    else if (light.Type == 1)
    {
        float3 toLight = light.Position - posW;
        float  dist    = length(toLight);
        float3 L       = toLight / dist;
        float atten = Attenuation(dist, light.Range);
        if (atten < 0.001f) return (float3)0;
        return PhongBRDF(albedo, N, L, V, Ks, Ns, light.Color) * atten;
    }
    else
    {
        float3 toLight  = light.Position - posW;
        float  dist     = length(toLight);
        float3 L        = toLight / dist;
        float3 spotDir  = normalize(light.Direction);
        float  cosTheta = dot(-L, spotDir);
        if (cosTheta < light.SpotAngle) return (float3)0;
        float innerAngle = light.SpotAngle + (1.0f - light.SpotAngle) * 0.1f;
        float spotFactor = smoothstep(light.SpotAngle, innerAngle, cosTheta);
        float atten = Attenuation(dist, light.Range) * spotFactor;
        if (atten < 0.001f) return (float3)0;
        return PhongBRDF(albedo, N, L, V, Ks, Ns, light.Color) * atten;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Vertex Shader — fullscreen triangle без VB
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
// Pixel Shader
// ─────────────────────────────────────────────────────────────────────────────

// Цвета для debug-визуализации каскадов
static const float3 kCascadeColors[4] = {
    float3(1.f, 0.3f, 0.3f), // каскад 0 — красный  (ближний)
    float3(0.3f, 1.f, 0.3f), // каскад 1 — зелёный
    float3(0.3f, 0.5f, 1.f), // каскад 2 — синий
    float3(1.f, 1.f, 0.3f),  // каскад 3 — жёлтый   (дальний)
};

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

    // ── View-space глубина для выбора каскада ─────────────────────────────
    // Проецируем posW через InvViewProj обратно: нам нужна view-Z.
    // Проще взять dot(posW - eyePos, forwardVec), но forward нам неизвестен.
    // Используем gInvViewProj.transpose для получения view матрицы:
    // viewPos = mul(float4(posW,1), gViewMatrix) — но view матрицы нет в CB.
    // Обходим: вычисляем abs(z) в clip space разделённый на w даст нормализованную
    // глубину, а реальная view-Z = near*far / (far - depth*(far-near)) — сложно.
    // Простейший подход: расстояние от камеры (хорошо работает для направленного света)
    // View-space Z: проекция вектора (posW - eye) на forward вектор камеры
    // Это линейная глубина — совпадает с splits[] из UpdateCsmCascades
    float viewDepth = dot(posW - gEyePosW, gCameraForward);

    // ── Shadow ────────────────────────────────────────────────────────────
    int cascadeIdx = 0;
    float shadowFactor = 1.f;
    // ВРЕМЕННО ОТКЛЮЧЕНО ДЛЯ ДИАГНОСТИКИ — раскомментировать после проверки
    // if (gNumCascades > 0)
    //     shadowFactor = CalcShadow(posW, viewDepth, cascadeIdx);

    // ── Ambient ───────────────────────────────────────────────────────────
    float3 color = albedo * 0.04f;

    // ── Directional light (индекс 0) — применяем тень только к нему ──────
    // Остальные источники (point/spot) — без теней (упрощение)
    for (int i = 0; i < gNumLights; ++i)
    {
        float3 contrib = CalcLight(gLights[i], posW, N, V, albedo, Ks, Ns);

        // Тень применяем только к направленному свету (Type == 0)
        if (gLights[i].Type == 0)
            contrib *= shadowFactor;

        color += contrib;
    }

    // Reinhard tonemapping
    color = color / (color + 1.0f);

    // ── Debug: цветовая маркировка каскадов ──────────────────────────────
    if (gDebugCascades && gNumCascades > 0)
        color = lerp(color, kCascadeColors[cascadeIdx], 0.35f);

    return float4(color, 1.0f);
}
