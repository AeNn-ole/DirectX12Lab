// ═══════════════════════════════════════════════════════════════════════════
// Shadow.hlsl — Shadow Pass
//
// Рендерим сцену с точки зрения источника света.
// Нужен только VS — пишем только в depth buffer.
// PS = null (D3D12 это разрешает для shadow maps).
//
// Регистры:
//   b0 — ShadowCB (LightViewProj + World)
// ═══════════════════════════════════════════════════════════════════════════

cbuffer ShadowCB : register(b0)
{
    float4x4 gLightViewProj; // матрица каскада (ortho * view из точки света)
    float4x4 gWorld;         // мировая матрица объекта
};

// Нам нужна только позиция вершины — нормали/UV не используются
float4 VSMain_Shadow(float3 PosL : POSITION) : SV_POSITION
{
    float4 posW = mul(float4(PosL, 1.f), gWorld);
    return mul(posW, gLightViewProj);
}
