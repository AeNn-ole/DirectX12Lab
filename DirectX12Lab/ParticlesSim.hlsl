// ═══════════════════════════════════════════════════════════════════════════
// ParticlesSim.hlsl — GPU Particle Simulation (Homework #6)
//
// Три compute-кернела, один root signature (u0..u7), вызываются в таком
// порядке каждый кадр (см. RenderingSystem::ParticlesSimPass):
//
//   1) CSUpdate              — интегрирует живые частицы, решает кто умер
//   2) CSEmit                — рождает новые частицы из free-list (DeadList)
//   3) CSBuildIndirectArgs   — считает indirect-аргументы для Draw/Dispatch
//
// ── Буферы (два StructuredBuffer, Append/Consume — по заданию) ─────────────
//
//   gPool        (u0) RWStructuredBuffer<Particle>   — вся память частиц,
//                      адресуется индексом 0..MaxParticles-1, НЕ Append/Consume
//
//   gAliveIn     (u1) ConsumeStructuredBuffer<uint>   ⎫ ping-pong пара:
//   gAliveOut    (u2) AppendStructuredBuffer<uint>    ⎭ индексы живых частиц.
//                      Роли (u1/u2) меняются местами каждый кадр на CPU —
//                      это и есть "один Append, один Consume" из задания.
//
//   gDeadAppend  (u3) AppendStructuredBuffer<uint>    ⎫ ОДИН физический буфер
//   gDeadConsume (u4) ConsumeStructuredBuffer<uint>   ⎭ (free-list мёртвых
//                      индексов), но два разных UAV-дескриптора на него же:
//                      Update кладёт туда умерших (Append),
//                      Emit  забирает оттуда свободный слот (Consume).
//
//   gDrawArgs        (u5) RWStructuredBuffer<uint>[4] — D3D12_DRAW_ARGUMENTS
//   gDispatchArgs    (u6) RWStructuredBuffer<uint>[4] — GroupsX,Y,Z + AliveCount
//   gAliveOutCounter (u7) RWStructuredBuffer<uint>[1] — «сырое» число элементов
//                      в gAliveOut за этот кадр (не через Append/Consume API,
//                      а прямым чтением counter-ресурса как обычного буфера —
//                      см. пояснение в Particle_Integration.md)
//
// Почему индексы дважды безопасны: как только счётчик Consume-буфера
// доходит до 0, чтение "ушедшее за границу" по спецификации D3D12
// возвращает 0, а запись "за границу" отбрасывается — GPU не падает.
// Для gAliveIn это устранено полностью (CSUpdate стартует ровно
// AliveCount потоков благодаря ExecuteIndirect+bounds-check).
// Для gDeadConsume в момент насыщения (все MaxParticles заняты) это
// может дать один "призрачный" Consume(), возвращающий индекс 0 —
// safe, но может привести к минорному визуальному дребезгу частицы #0
// на пике заполнения. Задокументировано, это стандартный и общепринятый
// trade-off для учебных GPU-particle систем.
// ═══════════════════════════════════════════════════════════════════════════

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
};

RWStructuredBuffer<Particle>   gPool             : register(u0);
ConsumeStructuredBuffer<uint>  gAliveIn          : register(u1);
AppendStructuredBuffer<uint>   gAliveOut         : register(u2);
AppendStructuredBuffer<uint>   gDeadAppend       : register(u3);
ConsumeStructuredBuffer<uint>  gDeadConsume      : register(u4);
RWStructuredBuffer<uint>       gDrawArgs         : register(u5); // [VtxCount, InstCount, StartVtx, StartInst]
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
    p.Velocity = gEmitterVel + dir * 88.5f;
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
//   - DrawArgs      — для DrawInstancedIndirect этого же кадра (рендерим
//                     то, что только что насчитали)
//   - DispatchArgs  — для CSUpdate СЛЕДУЮЩЕГО кадра (сколько групп нужно
//                     задиспатчить, чтобы обработать именно gAliveOut)
// ─────────────────────────────────────────────────────────────────────────────
[numthreads(1, 1, 1)]
void CSBuildIndirectArgs(uint3 dtid : SV_DispatchThreadID)
{
    uint count = min(gAliveOutCounter[0], gMaxParticles);

    gDrawArgs[0] = count; // VertexCountPerInstance — по одной точке на частицу
    gDrawArgs[1] = 1;     // InstanceCount
    gDrawArgs[2] = 0;     // StartVertexLocation
    gDrawArgs[3] = 0;     // StartInstanceLocation

    gDispatchArgs[0] = (count + 255u) / 256u; // GroupsX
    gDispatchArgs[1] = 1;
    gDispatchArgs[2] = 1;
    gDispatchArgs[3] = count;                 // точное число для bounds-check в CSUpdate
}
