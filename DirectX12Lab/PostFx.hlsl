// ═══════════════════════════════════════════════════════════════════════════
// PostFx.hlsl — Post-process pass
//
// VS: fullscreen triangle без вершинного буфера (SV_VertexID)
// PS: два эффекта переключаемых через gMode:
//   0 = нет эффекта (passthrough)
//   1 = Fish-eye (бочкообразное искажение)
//   2 = VHS (хроматическая аберрация + шум + scanlines + jitter)
//   3 = оба одновременно
// ═══════════════════════════════════════════════════════════════════════════

Texture2D gHdrTex : register(t0);   // результат lighting pass
SamplerState gLinear : register(s0); // linear clamp

cbuffer PostFxCB : register(b0)
{
    float gTime;        // секунды с запуска (для анимации шума/jitter)
    int   gMode;        // битовая маска: бит0 = fisheye, бит1 = VHS
    float gFishStrength;// сила эффекта рыбьего глаза (0.3 = умеренно, 1.0 = сильно)
    float gVhsStrength; // общая интенсивность VHS (0..1)
};

// ─────────────────────────────────────────────────────────────────────────────
// Vertex Shader — генерирует fullscreen triangle из воздуха.
// Три вызова с vertexID 0,1,2 покрывают весь NDC [-1..1].
// Никакого вершинного буфера не нужно.
// ─────────────────────────────────────────────────────────────────────────────
struct VSOut
{
    float4 PosH : SV_POSITION;
    float2 UV   : TEXCOORD0;
};

VSOut VSMain_Post(uint vertexID : SV_VertexID)
{
    // Огромный треугольник: вершины в NDC
    float2 pos[3] = { float2(-1.f, 3.f), float2(-1.f,-1.f), float2(3.f,-1.f) };
    // UV [0..1] для выборки текстуры
    float2 uv[3]  = { float2( 0.f,-1.f), float2( 0.f, 1.f), float2(2.f, 1.f) };

    VSOut o;
    o.PosH = float4(pos[vertexID], 0.f, 1.f);
    o.UV   = uv[vertexID];
    return o;
}

// ─────────────────────────────────────────────────────────────────────────────
// Утилиты
// ─────────────────────────────────────────────────────────────────────────────

// Простой hash без текстуры — для псевдослучайного шума в VHS
float Hash(float2 p)
{
    p = frac(p * float2(443.897f, 441.423f));
    p += dot(p, p.yx + 19.19f);
    return frac((p.x + p.y) * p.x);
}

// ─────────────────────────────────────────────────────────────────────────────
// Эффект 1: Fish-Eye (бочкообразная дисторсия)
//
// Алгоритм:
//   1. Перевести UV в центрированные координаты [-1..1]
//   2. Найти полярный радиус r = length(centered_uv)
//   3. Применить нелинейное растяжение: r' = r + strength * r^3
//      (кубический член — классическая бочкообразная дисторсия)
//   4. Перевести обратно в [0..1] UV
//
// При strength > 0 — бочка (fisheye), при strength < 0 — подушка (pincushion)
// ─────────────────────────────────────────────────────────────────────────────
float2 ApplyFishEye(float2 uv, float strength)
{
    // Центрируем: [0,1] → [-1,1]
    float2 centered = uv * 2.f - 1.f;

    float r2 = dot(centered, centered);          // r²
    float r  = sqrt(r2);                         // r

    // Дисторсия: новый радиус = r * (1 + strength * r²)
    // Делим на (1 + strength) чтобы нормировать масштаб при r=1
    float distort = 1.f + strength * r2;
    float2 distorted = centered * distort / (1.f + strength);

    // Обратно в [0,1]
    return distorted * 0.5f + 0.5f;
}

// ─────────────────────────────────────────────────────────────────────────────
// Эффект 2: VHS
//
// Составные части:
//   a) Хроматическая аберрация — R/G/B каналы читаются со смещением по X
//      (имитация расхождения цветов на старых лентах)
//   b) Scanlines — горизонтальные полосы с небольшим затемнением
//      (имитация строчной развёртки кинескопа)
//   c) Временной шум — случайные яркие/тёмные пиксели меняются каждый кадр
//   d) Горизонтальный jitter — строки изображения "дрожат" по X
//      (имитация нестабильности магнитной головки)
//   e) Вертикальная полоса помех — иногда проходит сверху вниз
// ─────────────────────────────────────────────────────────────────────────────
float3 ApplyVHS(float2 uv, float time, float strength, Texture2D tex, SamplerState samp)
{
    // ── d) Горизонтальный jitter ─────────────────────────────────────────
    // Каждая строка немного смещена по X — случайно и меняется со временем
    float jitterFreq   = 8.f;          // сколько "горизонтальных зон" дрожат
    float jitterAmp    = 0.025f * strength;
    float jitterPhase  = floor(uv.y * jitterFreq + time * 15.f);
    float jitterOffset = (Hash(float2(jitterPhase, time * 0.3f)) - 0.5f) * jitterAmp;

    // Иногда — сильный jitter на отдельных строках (артефакт "перемотки")
    float glitchLine   = Hash(float2(floor(time * 2.f), 7.3f)); // случайная строка
    float glitchAmt    = step(0.85f, Hash(float2(time * 3.f, 1.f)));
    float isGlitchRow  = step(abs(uv.y - glitchLine), 0.04f);
    jitterOffset      += isGlitchRow * glitchAmt * 0.06f * strength;

    float2 jitteredUV = uv + float2(jitterOffset, 0.f);

    // ── a) Хроматическая аберрация ───────────────────────────────────────
    // R смещён влево, B — вправо, G читается из центра
    float aberration = 0.025f * strength;
    float r = tex.Sample(samp, jitteredUV + float2(-aberration, 0.f)).r;
    float g = tex.Sample(samp, jitteredUV).g;
    float b = tex.Sample(samp, jitteredUV + float2( aberration, 0.f)).b;
    float3 color = float3(r, g, b);

    // ── b) Scanlines ──────────────────────────────────────────────────────
    // Синус по Y создаёт горизонтальные полосы с периодом ~4px
    float scanFreq    = 300.f;  // количество линий
    float scanVal     = sin(uv.y * scanFreq * 3.14159f);
    float scanDim     = 1.f - 0.4f * strength * (scanVal * 0.5f + 0.5f);
    color            *= scanDim;

    // ── c) Шум ───────────────────────────────────────────────────────────
    // Случайные пиксели с мерцанием каждый кадр
    float noiseVal = Hash(uv * float2(1920.f, 1080.f) + float2(time * 100.f, 0.f));
    float noise    = (noiseVal - 0.5f) * 0.22f * strength;
    color         += noise;

    // ── e) Вертикальная полоса помех ─────────────────────────────────────
    // Медленно ползёт по экрану, появляется периодически
    float bandSpeed = time * 0.3f;
    float bandY     = frac(bandSpeed);
    float bandWidth = 0.08f;
    float bandDist  = abs(uv.y - bandY);
    float bandMask  = step(bandDist, bandWidth) * 0.7f * strength;
    // В полосе — смещение по X и осветление
    float2 bandUV   = jitteredUV + float2(Hash(float2(uv.y, time)) * 0.02f * bandMask, 0.f);
    float3 bandSample = tex.Sample(samp, bandUV).rgb;
    color = lerp(color, bandSample * 1.6f + 0.1f, bandMask);

    // Небольшое общее снижение насыщенности — плёнка выцвела
    float luma    = dot(color, float3(0.299f, 0.587f, 0.114f));
    color         = lerp(color, luma.xxx, 0.35f * strength);

    return color;
}

// ─────────────────────────────────────────────────────────────────────────────
// Pixel Shader
// ─────────────────────────────────────────────────────────────────────────────
float4 PSMain_Post(VSOut pin) : SV_TARGET
{
    float2 uv = pin.UV;

    bool doFishEye = (gMode & 1) != 0;
    bool doVHS     = (gMode & 2) != 0;

    // ── Fisheye дисторсия UV (применяем до сэмплирования) ────────────────
    float2 sampleUV = uv;
    if (doFishEye)
    {
        sampleUV = ApplyFishEye(uv, gFishStrength);

        // Пиксели за границей [0,1] — чёрная рамка
        if (any(sampleUV < 0.f) || any(sampleUV > 1.f))
            return float4(0.f, 0.f, 0.f, 1.f);
    }

    // ── VHS (включает собственное сэмплирование с аберрацией) ─────────────
    float3 color;
    if (doVHS)
    {
        // VHS работает с уже искажёнными координатами (если fisheye тоже вкл)
        // Подменяем базовый UV для хроматической аберрации
        float2 vhsUV = sampleUV;
        color = ApplyVHS(vhsUV, gTime, gVhsStrength, gHdrTex, gLinear);
    }
    else
    {
        color = gHdrTex.Sample(gLinear, sampleUV).rgb;
    }

    // ── Виньетка от рыбьего глаза — края темнее ───────────────────────────
    if (doFishEye)
    {
        float2 centered = uv * 2.f - 1.f;
        float  vign     = 1.f - smoothstep(0.6f, 1.1f, length(centered));
        color *= vign;
    }

    return float4(color, 1.f);
}
