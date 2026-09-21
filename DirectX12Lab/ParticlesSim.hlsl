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

cbuffer ParticleSimCB : register(b0)
{
    float3 gEmitterPos;    float gDeltaTime;
    float3 gEmitterVel;    float gTotalTime;
    float3 gGravity;       uint  gEmitCount;
    float2 gLifetimeMinMax;
    float2 gSizeMinMax;
    float4 gColorStart;
    float4 gColorEnd;
    uint   gMaxParticles;
    uint   gRandomSeed;
    float2 gEmitterSpread;
    uint   gTeapotIndexCount; // размер меша чайника (константа) — для DrawIndexedInstanced args
    float3 _padSim;
};

RWStructuredBuffer<Particle>   gPool             : register(u0);
ConsumeStructuredBuffer<uint>  gAliveIn          : register(u1);
AppendStructuredBuffer<uint>   gAliveOut         : register(u2);
AppendStructuredBuffer<uint>   gDeadAppend       : register(u3);
ConsumeStructuredBuffer<uint>  gDeadConsume      : register(u4);
RWStructuredBuffer<uint>       gDrawArgs         : register(u5); // D3D12_DRAW_INDEXED_ARGUMENTS (5 uint)
RWStructuredBuffer<uint>       gDispatchArgs     : register(u6); // [GroupsX, GroupsY, GroupsZ, AliveCount]
RWStructuredBuffer<uint>       gAliveOutCounter  : register(u7); // сырое значение hidden-counter'а gAliveOut

// ── Простой хэш-генератор случайных чисел (в духе PostFx.hlsl) ─────────────
float Hash11(uint n)
{
    n = (n << 13u) ^ n;
    n = n * (n * n * 15731u + 789221u) + 1376312589u;
    return (float)(n & 0x00FFFFFFu) / (float)0x01000000u; // → [0,1)
}

float3 RandomInUnitSphere(uint seed)
{
    float a = Hash11(seed) * 6.2831853f;
    float z = Hash11(seed + 1u) * 2.f - 1.f;
    float r = sqrt(max(0.f, 1.f - z * z));
    return float3(r * cos(a), r * sin(a), z);
}

// ─────────────────────────────────────────────────────────────────────────────
// CSUpdate — обновляет ровно gDispatchArgs[3] (=AliveCount) частиц.
// Индирект-диспатч гарантирует, что лишних потоков (за пределами реального
// числа живых частиц) не запускается вообще — поэтому здесь дополнительная
// защита не нужна для u1, но оставляем bounds-check на случай, если
// последняя группа потоков не кратна 256.
// ─────────────────────────────────────────────────────────────────────────────
[numthreads(256, 1, 1)]
void CSUpdate(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gDispatchArgs[3]) return;

    uint idx = gAliveIn.Consume();
    Particle p = gPool[idx];

    // Простая физика: гравитация + затухание (лёгкое сопротивление воздуха)
    p.Velocity += gGravity * gDeltaTime;
    p.Velocity *= (1.0f - 0.05f * gDeltaTime);
    p.Position += p.Velocity * gDeltaTime;
    p.Age      += gDeltaTime;

    if (p.Age < p.Lifetime)
    {
        // Цвет затухает от ColorStart к ColorEnd по времени жизни,
        // альфа гаснет к концу — используется как множитель яркости в PS
        // (сами частицы всё равно рисуются непрозрачно, без blend).
        float t = saturate(p.Age / p.Lifetime);
        p.Color = lerp(gColorStart, gColorEnd, t);

        gPool[idx] = p;
        gAliveOut.Append(idx);
    }
    else
    {
        // Частица умерла — возвращаем её индекс в пул свободных слотов
        gDeadAppend.Append(idx);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CSEmit — пытается родить gEmitCount новых частиц из free-list.
// Дублирование в момент насыщения системы (DeadList пуст) безопасно
// по D3D12 out-of-bounds контракту — см. комментарий в шапке файла.
// ─────────────────────────────────────────────────────────────────────────────
[numthreads(256, 1, 1)]
void CSEmit(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gEmitCount) return;

    uint idx  = gDeadConsume.Consume();
    uint seed = gRandomSeed ^ (dtid.x * 2654435761u);

    float3 dir = RandomInUnitSphere(seed);

    Particle p;
    p.Position = gEmitterPos + dir * float3(gEmitterSpread.x, 0.f, gEmitterSpread.y) * Hash11(seed + 7u);
    p.Velocity = gEmitterVel + dir * 2.5f;
    p.Age      = 0.f;
    p.Lifetime = lerp(gLifetimeMinMax.x, gLifetimeMinMax.y, Hash11(seed + 13u));
    p.Color    = gColorStart;
    p.Size     = lerp(gSizeMinMax.x, gSizeMinMax.y, Hash11(seed + 19u));
    p._pad     = float3(0, 0, 0);

    gPool[idx] = p;
    gAliveOut.Append(idx);
}

// ─────────────────────────────────────────────────────────────────────────────
// CSBuildIndirectArgs — один поток, вызывается в конце кадра, когда
// и CSUpdate, и CSEmit уже дописали всё в gAliveOut. Готовит:
//   - DrawArgs      — для DrawIndexedInstancedIndirect этого же кадра: рисуем
//                     ОДИН И ТОТ ЖЕ меш чайника (IndexCountPerInstance — константа,
//                     gTeapotIndexCount), но InstanceCount — ДИНАМИЧЕСКИЙ,
//                     равен числу живых частиц (роли Vertex/Instance по
//                     сравнению с billboard-версией поменялись местами!)
//   - DispatchArgs  — для CSUpdate СЛЕДУЮЩЕГО кадра (сколько групп нужно
//                     задиспатчить, чтобы обработать именно gAliveOut)
// ─────────────────────────────────────────────────────────────────────────────
[numthreads(1, 1, 1)]
void CSBuildIndirectArgs(uint3 dtid : SV_DispatchThreadID)
{
    uint count = min(gAliveOutCounter[0], gMaxParticles);

    // D3D12_DRAW_INDEXED_ARGUMENTS: {IndexCountPerInstance, InstanceCount,
    //                                 StartIndexLocation, BaseVertexLocation, StartInstanceLocation}
    gDrawArgs[0] = gTeapotIndexCount; // геометрия чайника фиксирована — константа
    gDrawArgs[1] = count;             // а вот число ИНСТАНСОВ — динамическое (живые частицы)
    gDrawArgs[2] = 0;                 // StartIndexLocation
    gDrawArgs[3] = 0;                 // BaseVertexLocation
    gDrawArgs[4] = 0;                 // StartInstanceLocation

    gDispatchArgs[0] = (count + 255u) / 256u; // GroupsX
    gDispatchArgs[1] = 1;
    gDispatchArgs[2] = 1;
    gDispatchArgs[3] = count;                 // точное число для bounds-check в CSUpdate
}
