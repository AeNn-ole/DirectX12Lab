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
    float3   gCameraRight; float _p0;
    float3   gCameraUp;    float _p1;
    float3   gEyePosW;     float _p2;
    float3   gLightDirW;   float _p3;
    float4   gAmbientColor;
};

StructuredBuffer<uint>      gAliveIndices : register(t0);
StructuredBuffer<Particle>  gPool         : register(t1);
Texture2D                  gDiffuse      : register(t2);
SamplerState               gSampler      : register(s0);

struct VSIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
    float2 UV      : TEXCOORD0;
};

// VS passes an OBJ triangle and its particle index to GS.
struct VSOut
{
    float3 PosL       : POSITION;
    float3 NormalL    : NORMAL;
    float2 UV         : TEXCOORD0;
    nointerpolation uint InstanceId : TEXCOORD1;
};

struct GSOut
{
    float4 PosH    : SV_POSITION;
    float3 NormalW : TEXCOORD0;
    float2 UV      : TEXCOORD1;
    float4 Color   : COLOR0;
};

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
    VSOut o;
    o.PosL = vin.PosL;
    o.NormalL = vin.NormalL;
    o.UV = vin.UV;
    o.InstanceId = instanceId;
    return o;
}

[maxvertexcount(3)]
void GSMain_Particle(triangle VSOut input[3], inout TriangleStream<GSOut> stream)
{
    uint idx = gAliveIndices[input[0].InstanceId];
    Particle p = gPool[idx];

    float3x3 rot = RotateY(p.Age * 3.0f);
    float scale = p.Size * 0.6f;

    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        float3 posW = mul(input[i].PosL, rot) * scale + p.Position;

        GSOut o;
        o.PosH = mul(float4(posW, 1.0f), gViewProj);
        o.NormalW = mul(input[i].NormalL, rot);
        o.UV = input[i].UV;
        o.Color = p.Color;
        stream.Append(o);
    }
    stream.RestartStrip();
}

float4 PSMain_Particle(GSOut i) : SV_TARGET
{
    float3 N = normalize(i.NormalW);
    float3 L = normalize(-gLightDirW);
    float ndotl = saturate(dot(N, L));

    float3 albedo = gDiffuse.Sample(gSampler, i.UV).rgb * i.Color.rgb;
    float3 lit = albedo * (gAmbientColor.rgb + ndotl * (1.0f - gAmbientColor.rgb));
    return float4(lit, 1.0f);
}
