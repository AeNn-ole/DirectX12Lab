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
};

struct VSIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
};

struct PSIn
{
    float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
};

PSIn VSMain(VSIn vin)
{
    PSIn vout;

    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;
    vout.NormalW = normalize(mul(vin.NormalL, (float3x3)gWorld));
    vout.PosH = mul(float4(vin.PosL, 1.0f), gWorldViewProj);

    return vout;
}

float4 PSMain(PSIn pin) : SV_TARGET
{
    float3 N = normalize(pin.NormalW);
    float3 L = normalize(-gLightDirW);
    float3 V = normalize(gEyePosW - pin.PosW);

    float t = saturate(pin.PosW.y * 0.5f + 0.5f);

    float3 colPurple = float3(0.45f, 0.20f, 0.95f);
    float3 colBlue   = float3(0.25f, 0.75f, 1.00f);
    float3 colWhite  = float3(1.00f, 1.00f, 1.00f);

    float3 baseColor;
    if (t < 0.5f)
    {
        float u = smoothstep(0.0f, 0.5f, t);
        baseColor = lerp(colPurple, colBlue, u);
    }
    else
    {
        float u = smoothstep(0.5f, 1.0f, t);
        baseColor = lerp(colBlue, colWhite, u);
    }

    float ndotl = saturate(dot(N, L));
    float3 lit = (0.18f + 0.82f * ndotl);

    float3 H = normalize(L + V);
    float spec = pow(saturate(dot(N, H)), 64.0f);

    float3 color = baseColor * lit + spec.xxx * 0.35f;
    return float4(color, 1.0f);
}