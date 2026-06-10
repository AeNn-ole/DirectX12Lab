// ═══════════════════════════════════════════════════════════════════════════
// Billboard.hlsl — Forward pass для далёких объектов.
//
// Идея: вместо тысяч треугольников 3D-сетки рисуем простой квад,
// который всегда повёрнут к камере (spherical billboard).
//
// Вершинный шейдер строит квад из SV_VertexID без VB:
//   DrawInstanced(6, numFarInstances, 0, 0)
//   SV_InstanceID → центр из StructuredBuffer
//   SV_VertexID   → угол квада (0-5, два треугольника)
//
// Пиксельный шейдер семплирует текстуру и делает alpha-test.
// ═══════════════════════════════════════════════════════════════════════════

Texture2D    gBillboardTex : register(t0);
SamplerState gSampler      : register(s0);

// Центры далёких экземпляров (xyz = world position, w = не используется)
// Заполняется CPU каждый кадр перед BillboardPass
StructuredBuffer<float4> gCenters : register(t1);

cbuffer BillboardFrameCB : register(b0)
{
    float4x4 gViewProj;
    float3   gCamRight; float _p0;   // правый вектор камеры в мировых координатах
    float3   gCamUp;    float _p1;   // верхний вектор камеры
    float2   gSize;                  // размер квада в мировых единицах (ширина, высота)
    float2   _p2;
};

// ── Два треугольника квада, собранные из 6 вершин ─────────────────────────
//
//   3 ── 2        Нумерация углов:
//   │ ╲  │        0 = нижний левый  (-x, -y)
//   │  ╲ │        1 = нижний правый (+x, -y)
//   0 ── 1        2 = верхний правый (+x, +y)
//                 3 = верхний левый  (-x, +y)
//
// tri0: 0,1,2   tri1: 0,2,3  (counter-clockwise, left-handed)
static const float2 kOffsets[6] = {
    {-1.f, -1.f}, {+1.f, -1.f}, {+1.f, +1.f},
    {-1.f, -1.f}, {+1.f, +1.f}, {-1.f, +1.f}
};
static const float2 kUVs[6] = {
    {0.f, 1.f}, {1.f, 1.f}, {1.f, 0.f},
    {0.f, 1.f}, {1.f, 0.f}, {0.f, 0.f}
};

// ─────────────────────────────────────────────────────────────────────────────
struct PSIn
{
    float4 PosH : SV_POSITION;
    float2 UV   : TEXCOORD0;
};

// ─────────────────────────────────────────────────────────────────────────────
PSIn VSMain(uint vertexId   : SV_VertexID,
            uint instanceId : SV_InstanceID)
{
    PSIn o;

    // Центр этого билборда из структурированного буфера
    float3 center = gCenters[instanceId].xyz;

    // Смещение угла в мировых координатах:
    // CamRight и CamUp — ортонормированные оси камеры в мировом пространстве.
    // Умножаем на половину размера (kOffsets уже в [-1..+1]).
    float2 off    = kOffsets[vertexId];
    float3 worldPos = center
        + gCamRight * off.x * gSize.x * 0.5f
        + gCamUp    * off.y * gSize.y * 0.5f;

    o.PosH = mul(float4(worldPos, 1.f), gViewProj);
    o.UV   = kUVs[vertexId];
    return o;
}

// ─────────────────────────────────────────────────────────────────────────────
float4 PSMain(PSIn pin) : SV_TARGET
{
    float4 color = gBillboardTex.Sample(gSampler, pin.UV);

    // Alpha-test: отбрасываем прозрачные пиксели (фон текстуры)
    // Порог 0.1 — достаточно мягкий чтобы не резать края спрайта
    clip(color.a - 0.1f);

    return color;
}
