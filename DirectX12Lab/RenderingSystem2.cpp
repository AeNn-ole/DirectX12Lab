// ─────────────────────────────────────────────────────────────────────────────
// Этот файл — продолжение RenderingSystem.cpp.
// Он подключается к основному через единую единицу компиляции:
// просто добавьте оба .cpp в проект. Либо вставьте содержимое в конец
// RenderingSystem.cpp — оба варианта эквивалентны.
// ─────────────────────────────────────────────────────────────────────────────

#include "RenderingSystem.h"
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <wincodec.h>
#include <DirectXMath.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// (ThrowIfFailed и хелперы объявлены в RenderingSystem.cpp — здесь не нужны)
static void ThrowIfFailed2(HRESULT hr, const char* what)
{
    if (FAILED(hr)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (hr=0x%08X)", what, (unsigned)hr);
        throw std::runtime_error(buf);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// UpdateMaterialCB — обновляет один слот geometry CB для материала mi.
// Логика идентична D3D12Context::UpdateConstantBufferForMaterial.
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::UpdateMaterialCB(int mi)
{
    if (!m_mappedObjectCB) return;

    static LARGE_INTEGER freq{}, t0{};
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
    }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    m_totalTime = (float)((now.QuadPart - t0.QuadPart) / (double)freq.QuadPart);

    const auto& mat = m_model.materials[mi];

    ObjectConstants cb{};
    XMMATRIX world = XMLoadFloat4x4(&m_world);
    XMMATRIX view  = XMLoadFloat4x4(&m_view);
    XMMATRIX proj  = XMLoadFloat4x4(&m_proj);
    XMStoreFloat4x4(&cb.World,         XMMatrixTranspose(world));
    XMStoreFloat4x4(&cb.WorldViewProj, XMMatrixTranspose(world * view * proj));

    cb.EyePosW   = m_eyePos;
    XMVECTOR L   = XMVector3Normalize(XMLoadFloat3(&m_lightDir));
    XMStoreFloat3(&cb.LightDirW, L);

    cb.Ambient   = { mat.Ka.x, mat.Ka.y, mat.Ka.z, 1.f };
    cb.Diffuse   = { mat.Kd.x, mat.Kd.y, mat.Kd.z, 1.f };
    cb.Specular  = { mat.Ks.x, mat.Ks.y, mat.Ks.z, 1.f };
    cb.SpecPower = mat.Ns > 0.f ? mat.Ns : 32.f;

    cb.UVOffset  = { m_totalTime * 0.02f, 0.f };
    cb.UVTiling  = { 1.f, 1.f };
    cb.gTime     = m_totalTime;

    std::memcpy(m_mappedObjectCB + (size_t)mi * m_objectCBByteSize, &cb, sizeof(cb));
}

// ─────────────────────────────────────────────────────────────────────────────
// InitLights — расставляет источники света по сцене Sponza.
// Вызывается один раз из Initialize() после загрузки модели.
//
// Стандартная Sponza: X ≈ [-600, 600], Y ≈ [0, 500], Z ≈ [-100, 100]
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::InitLights()
{
    m_numLights = 0;
    auto& L = m_lights;

    // ── 0: Directional — солнце сверху-сбоку (единственный на всю сцену) ──
   L[0].Type      = 0;
    L[0].Direction = { 0.4f, -0.8f, 0.2f };    // нормализуем в шейдере
   L[0].Color     = { 1.0f, 0.95f, 0.85f, 0.1f }; // тёплый, умеренный

    // ── 1-4: Point 
    // Левая колоннада
    L[1].Type      = 1;
    L[1].Position  = { -350.f, 120.f, 0.f };
    L[1].Range     = 3.f;
    L[1].Color     = { 1.0f, 0.5f, 0.1f, 1.5f }; // оранжевый огонь

    L[2].Type      = 1;
    L[2].Position  = { -100.f, 120.f, 0.f };
    L[2].Range     = 3.f;
    L[2].Color     = { 1.0f, 0.5f, 0.1f, 1.5f };

    // Правая колоннада
    L[3].Type      = 1;
    L[3].Position  = {  100.f, 120.f, 0.f };
    L[3].Range     = 3.f;
    L[3].Color     = { 0.2f, 0.5f, 1.0f, 1.5f }; // холодный синий

    L[4].Type      = 1;
    L[4].Position  = {  350.f, 120.f, 0.f };
    L[4].Range     = 3.f;
    L[4].Color     = { 0.2f, 0.5f, 1.0f, 1.5f };

    // ── 5: Point — свет в центре 
    L[5].Type      = 1;
    L[5].Position  = { 0.f, 400.f, 0.f };
    L[5].Range     = 3.f;
    L[5].Color     = { 0.8f, 0.8f, 0.8f, 1.0f }; // холодный белый

    // ── 6: Spot
    L[6].Type       = 2;
    L[6].Position   = { 0.f, 450.f, 0.f };
    L[6].Direction  = { 0.f, -1.f, 0.f };       // строго вниз
    L[6].Range      = 5.f;
    L[6].SpotAngle  = cosf(XM_PI / 6.f);        // cos(30°) — угол полуконуса
    L[6].Color      = { 0.87f, 0.9f, 1.0f, 2.0f }; // яркий тёплый

    // ── 7: Spot 
    L[7].Type       = 2;
    L[7].Position   = { 500.f, 300.f, 0.f };
    L[7].Direction  = { -0.6f, -0.8f, 0.f };    // нормализуем в шейдере
    L[7].Range      = 4.f;
    L[7].SpotAngle  = cosf(XM_PI / 8.f);        // cos(22.5°) — узкий конус
    L[7].Color      = { 0.9f, 0.7f, 1.0f, 1.8f }; // фиолетовый оттенок

    // 8 статичных источников; остальные 192 слота (kMaxLights=200) отведены
    // под дождь: 40 летящих капель + 152 "лужицы" на полу, которые остаются
    // навсегда (см. UpdateRain).
    m_numBaseLights = 8;
    m_numLights     = 8;

    m_landedDrops.reserve(kRainLanded);
    InitRainDrops();
}

// ─────────────────────────────────────────────────────────────────────────────
// RandF — простой xorshift-генератор без <random>, достаточно для визуала.
// ─────────────────────────────────────────────────────────────────────────────
static float RandF(float lo, float hi)
{
    static unsigned s = 0x2F6E2B1u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return lo + (s & 0xFFFFu) / 65535.f * (hi - lo);
}

// ─────────────────────────────────────────────────────────────────────────────
// InitRainDrops — расставляет летящие капли дождя в случайных точках над
// полом Sponza (Y=0). Вызывается один раз из InitLights().
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::InitRainDrops()
{
    for (int i = 0; i < kRainDrops; ++i)
    {
        auto& d = m_rainDrops[i];
        d.x      = RandF(-500.f, 500.f);
        d.z      = RandF(-70.f,  70.f);
        // Разные стартовые высоты — чтобы капли не падали синхронно
        d.y      = RandF(kFloorY + 10.f, kSpawnHeight);
        d.speed  = RandF(80.f, 220.f);
        d.landed = false;
        d.r = RandF(0.2f, 1.0f);
        d.g = RandF(0.2f, 1.0f);
        d.b = RandF(0.4f, 1.0f);
    }
    m_landedDrops.clear();
    m_landedWriteIdx = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// UpdateRain — продвигает падающие капли на dt секунд. Когда капля достигает
// пола (Y <= kFloorY), она:
//   1) добавляется в m_landedDrops НАВСЕГДА (преподаватель просил, чтобы
//      источники не пропадали) — пока вектор не заполнится до kRainLanded,
//      затем используется кольцевой буфер (вытесняет самые старые "лужицы");
//   2) перезапускается сверху с новыми случайными x/z/цветом/скоростью.
//
// Заполняет m_lights[] начиная с m_numBaseLights:
//   m_numBaseLights .. +m_landedDrops.size()-1   — "лужицы" на полу (постоянные)
//   далее ..  +kRainDrops-1                       — летящие капли
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::UpdateRain(float dt)
{
    m_numLights = m_numBaseLights;

    auto addLight = [&](float x, float y, float z,
                         float r, float g, float b,
                         float range, float intensity)
    {
        if (m_numLights >= kMaxLights) return;
        auto& L = m_lights[m_numLights++];
        L.Type      = 1; // Point
        L.Position  = { x, y, z };
        L.Range     = range;
        L.Color     = { r, g, b, intensity };
    };

    // 1. "Лужицы" на полу — остаются навсегда (накапливаются)
    for (auto& d : m_landedDrops)
        addLight(d.x, kFloorY + 1.f, d.z,
                 d.r, d.g, d.b, /*range*/ 100.f, /*intensity*/ 0.46f);

    // 2. Летящие капли
    for (int i = 0; i < kRainDrops; ++i)
    {
        auto& d = m_rainDrops[i];

        d.y -= d.speed * dt;

        if (d.y <= kFloorY)
        {
            d.y      = kFloorY;

            // Сохраняем как постоянную "лужицу" на полу
            if ((int)m_landedDrops.size() < kRainLanded)
            {
                m_landedDrops.push_back(d);
            }
            else
            {
                // Кольцевой буфер: вытесняем самую старую лужицу
                m_landedDrops[m_landedWriteIdx] = d;
                m_landedWriteIdx = (m_landedWriteIdx + 1) % kRainLanded;
            }

            // Перезапускаем каплю сверху с новыми параметрами
            d.x     = RandF(-500.f, 500.f);
            d.z     = RandF(-330.f,  250.f);
            d.y     = kSpawnHeight;
            d.speed = RandF(80.f, 220.f);
            d.r = RandF(0.2f, 1.0f);
            d.g = RandF(0.2f, 1.0f);
            d.b = RandF(0.4f, 1.0f);
        }

        addLight(d.x, d.y, d.z,
                 d.r, d.g, d.b, /*range*/ 50.f, /*intensity*/ 0.6f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// UpdateLightingCB — обновляет LightingConstants (без массива источников)
// и записывает текущий массив источников в StructuredBuffer (m_lightsBuffer).
// Constant buffer слишком мал для 200 источников — поэтому источники
// хранятся отдельно и читаются в шейдере как StructuredBuffer<Light> t4.
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::UpdateLightingCB()
{
    if (!m_mappedLightingCB) return;

    XMMATRIX view     = XMLoadFloat4x4(&m_view);
    XMMATRIX proj     = XMLoadFloat4x4(&m_proj);
    XMMATRIX viewProj = view * proj;

    XMVECTOR det;
    XMMATRIX invVP = XMMatrixInverse(&det, viewProj);

    LightingConstants lc{};
    XMStoreFloat4x4(&lc.InvViewProj, XMMatrixTranspose(invVP));
    lc.EyePosW    = m_eyePos;
    lc.ScreenSize = { (float)m_width, (float)m_height };
    lc.NumLights  = m_numLights;

    std::memcpy(m_mappedLightingCB, &lc, sizeof(lc));

    // Записываем источники света в structured buffer (upload heap, t4)
    if (m_mappedLightsBuffer)
        std::memcpy(m_mappedLightsBuffer, m_lights, (size_t)m_numLights * sizeof(Light));
}

// ─────────────────────────────────────────────────────────────────────────────
// GeometryPass — рисует всю сцену в G-Buffer.
//
//   Состояния перед входом:  GBuffer текстуры — PIXEL_SHADER_RESOURCE
//                            Depth             — DEPTH_WRITE
//   Состояния на выходе:     GBuffer текстуры — PIXEL_SHADER_RESOURCE (возврат)
//                            Depth             — DEPTH_WRITE (не меняется)
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::GeometryPass()
{
    // 1. Переключаем G-Buffer: PSR → RT
    m_gbuffer.TransitionToRenderTarget(m_cmdList.Get());

    // 2. Собираем массив из трёх RTV-хендлов для OMSetRenderTargets
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {
        m_gbuffer.GetRTV(GBuffer::Albedo),
        m_gbuffer.GetRTV(GBuffer::Normal),
        m_gbuffer.GetRTV(GBuffer::Specular),
    };
    auto dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    // FALSE во втором параметре = хендлы НЕ идут подряд в памяти (мы дали массив)
    m_cmdList->OMSetRenderTargets(3, rtvs, FALSE, &dsv);

    // 3. Очищаем G-Buffer и глубину
    m_gbuffer.Clear(m_cmdList.Get());
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);

    // 4. Устанавливаем PSO и root sig для geometry pass
    m_cmdList->SetPipelineState(m_geometryPSO.Get());
    m_cmdList->SetGraphicsRootSignature(m_geometryRootSig.Get());

    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissorRect);
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmdList->IASetVertexBuffers(0, 1, &m_vbv);
    m_cmdList->IASetIndexBuffer(&m_ibv);

    // 5. Одна куча для всех дескрипторов
    ID3D12DescriptorHeap* heaps[] = { m_cbvSrvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase =
        m_cbvSrvHeap->GetGPUDescriptorHandleForHeapStart();
    const uint32_t N = m_numMaterials;

    // 6. Draw-call на каждый сабмеш с его материалом
    for (const auto& sub : m_model.subMeshes)
    {
        int mi = sub.materialIndex;

        // CBV: слот mi
        D3D12_GPU_DESCRIPTOR_HANDLE cbvH = gpuBase;
        cbvH.ptr += (SIZE_T)((uint32_t)mi * m_cbvSrvDescriptorSize);
        m_cmdList->SetGraphicsRootDescriptorTable(0, cbvH);

        // SRV текстуры материала: слот N+mi
        D3D12_GPU_DESCRIPTOR_HANDLE srvH = gpuBase;
        srvH.ptr += (SIZE_T)((N + (uint32_t)mi) * m_cbvSrvDescriptorSize);
        m_cmdList->SetGraphicsRootDescriptorTable(1, srvH);

        m_cmdList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexStart, 0, 0);
    }

    // 7. Возвращаем G-Buffer в PSR — lighting pass будет читать как SRV
    m_gbuffer.TransitionToShaderResource(m_cmdList.Get());
}

// ─────────────────────────────────────────────────────────────────────────────
// LightingPass — fullscreen triangle, читает G-Buffer, пишет в back buffer.
//
// Трюк с fullscreen triangle без VB:
//   DrawInstanced(3, 1, 0, 0) с SV_VertexID 0,1,2
//   Вершинный шейдер генерирует позиции и UV математически — ни байта VB не нужно.
//   Один большой треугольник покрывает весь экран и чуть выходит за его пределы,
//   что быстрее fullscreen quad (нет diagonal overdraw).
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::LightingPass()
{
    // 1. Барьер: depth DEPTH_WRITE → PIXEL_SHADER_RESOURCE (читаем глубину)
    D3D12_RESOURCE_BARRIER depthToSRV{};
    depthToSRV.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    depthToSRV.Transition = {
        m_depthStencilBuffer.Get(),
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    };
    m_cmdList->ResourceBarrier(1, &depthToSRV);

    // 2. Bind back buffer как единственный RT; DSV не нужен
    auto rtv = CurrentBackBufferRTV();
    m_cmdList->OMSetRenderTargets(1, &rtv, TRUE, nullptr);

    const float clearColor[4] = { 0.f, 0.f, 0.f, 1.f };
    m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

    // 3. PSO и root sig для lighting pass
    m_cmdList->SetPipelineState(m_lightingPSO.Get());
    m_cmdList->SetGraphicsRootSignature(m_lightingRootSig.Get());

    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissorRect);
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 4. Дескрипторная куча — та же самая
    ID3D12DescriptorHeap* heaps[] = { m_cbvSrvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);

    // param[0]: таблица из 5 SRV начиная со слота 2N
    //   (albedo, normal, specular, depth, StructuredBuffer<Light> источников)
    // GetFirstSRV() возвращает GPU-хендл на слот 2N в куче — root signature
    // расширен до 5 последовательных SRV (t0..t4), поэтому ничего менять не нужно
    m_cmdList->SetGraphicsRootDescriptorTable(0, m_gbuffer.GetFirstSRV());

    // param[1]: inline CBV — GPU-адрес lighting CB напрямую (не через таблицу!)
    m_cmdList->SetGraphicsRootConstantBufferView(1,
        m_lightingCB->GetGPUVirtualAddress());

    // 5. Рисуем 3 вершины — один большой треугольник = весь экран
    m_cmdList->DrawInstanced(3, 1, 0, 0);

    // 6. Возвращаем depth в DEPTH_WRITE — следующий кадр будет писать в него
    D3D12_RESOURCE_BARRIER depthBack = depthToSRV;
    depthBack.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    depthBack.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    m_cmdList->ResourceBarrier(1, &depthBack);
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — главная функция кадра: обновляет CB → geometry pass → lighting pass → present
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::Draw()
{
    if (!m_initialized) return;

    // ── Delta-time для симуляции дождя (независимо от geometry-таймера) ──
    if (!m_rainTimerInited)
    {
        QueryPerformanceFrequency(&m_rainFreq);
        QueryPerformanceCounter(&m_rainPrev);
        m_rainTimerInited = true;
    }
    LARGE_INTEGER rainNow;
    QueryPerformanceCounter(&rainNow);
    float rainDt = (float)((rainNow.QuadPart - m_rainPrev.QuadPart) / (double)m_rainFreq.QuadPart);
    m_rainPrev = rainNow;
    rainDt = (std::min)(rainDt, 0.1f); // clamp — избегаем скачков при паузах в отладчике

    UpdateRain(rainDt);

    // Обновляем оба CB на CPU (mapped memory — нет копирования, GPU читает напрямую)
    for (int i = 0; i < (int)m_numMaterials; ++i)
        UpdateMaterialCB(i);
    UpdateLightingCB();

    // Сбрасываем аллокатор и list
    ThrowIfFailed2(m_cmdAlloc->Reset(), "Alloc Draw");
    // Открываем list в geometry PSO — он будет первым, экономим одно SetPipelineState
    ThrowIfFailed2(m_cmdList->Reset(m_cmdAlloc.Get(), m_geometryPSO.Get()), "List Draw");

    // Барьер: back buffer PRESENT → RENDER_TARGET
    D3D12_RESOURCE_BARRIER toRT{};
    toRT.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT.Transition = {
        CurrentBackBuffer(),
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET
    };
    m_cmdList->ResourceBarrier(1, &toRT);

    // ── Проход 1: геометрия → G-Buffer ───────────────────────────────────
    GeometryPass();

    // ── Проход 2: освещение → back buffer ────────────────────────────────
    LightingPass();

    // Барьер: back buffer RENDER_TARGET → PRESENT
    D3D12_RESOURCE_BARRIER toPresent = toRT;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    m_cmdList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed2(m_cmdList->Close(), "CmdList Close Draw");
    ID3D12CommandList* ls[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, ls);

    ThrowIfFailed2(m_swapChain->Present(1, 0), "Present");
    m_currBackBuffer = (m_currBackBuffer + 1) % kSwapChainBufferCount;
    FlushCommandQueue();
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::SetCamera(const DirectX::XMFLOAT3& eyePos, float yaw, float pitch)
{
    m_eyePos = eyePos;
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    XMVECTOR fwd = XMVector3Normalize(XMVectorSet(sy * cp, sp, cy * cp, 0));
    XMVECTOR eye = XMVectorSet(eyePos.x, eyePos.y, eyePos.z, 1);
    XMStoreFloat4x4(&m_view, XMMatrixLookToLH(eye, fwd, XMVectorSet(0, 1, 0, 0)));
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::OnResize(uint32_t width, uint32_t height)
{
    if (!m_initialized || !width || !height) return;
    m_width = width; m_height = height;

    FlushCommandQueue();

    for (auto& b : m_swapChainBuffers) b.Reset();
    m_depthStencilBuffer.Reset();
    m_gbuffer.Destroy();

    ThrowIfFailed2(m_swapChain->ResizeBuffers(kSwapChainBufferCount, width, height,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0), "ResizeBuffers");
    m_currBackBuffer = 0;

    // Пересоздаём back buffer RTV-ы
    CreateRtvForBackBuffers();

    // Пересоздаём depth (с новым размером, тот же typeless формат)
    CreateDepthStencil();

    // Пересоздаём G-Buffer текстуры с новым размером;
    // RTV-дескрипторы пишутся в те же слоты RTV-кучи (2,3,4)
    // SRV-дескрипторы пишутся в те же слоты CBV/SRV-кучи (2N..2N+2)
    m_gbuffer.Create(m_device.Get(), width, height,
        m_rtvHeap.Get(), kSwapChainBufferCount, m_rtvDescriptorSize,
        m_cbvSrvHeap.Get(), 2 * m_numMaterials, m_cbvSrvDescriptorSize);

    // Обновляем SRV глубины (слот 2N+3)
    RecreateDepthSRV();

    m_viewport    = { 0, 0, (float)width, (float)height, 0, 1 };
    m_scissorRect = { 0, 0, (LONG)width,  (LONG)height };
    float asp = (float)width / height;
    XMStoreFloat4x4(&m_proj,
        XMMatrixPerspectiveFovLH(0.25f * XM_PI, asp, 0.1f, 1000.f));
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::FlushCommandQueue()
{
    const uint64_t v = ++m_fenceValue;
    ThrowIfFailed2(m_cmdQueue->Signal(m_fence.Get(), v), "Signal");
    if (m_fence->GetCompletedValue() < v) {
        ThrowIfFailed2(m_fence->SetEventOnCompletion(v, m_fenceEvent), "SetEvent");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderingSystem::CurrentBackBufferRTV() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (size_t)m_currBackBuffer * m_rtvDescriptorSize;
    return h;
}

ID3D12Resource* RenderingSystem::CurrentBackBuffer() const
{
    return m_swapChainBuffers[m_currBackBuffer].Get();
}

// ─────────────────────────────────────────────────────────────────────────────
// LoadAndUploadTexture — идентична D3D12Context; перенесена без изменений
// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::LoadAndUploadTexture(const wchar_t* path,
    ComPtr<ID3D12Resource>& outTex)
{
    constexpr DXGI_FORMAT kFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t texW = 0, texH = 0;
    std::vector<uint8_t> pixels;

    // Конвертируем wchar_t path → char для stb_image
    char pathA[512]{};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, pathA, sizeof(pathA), nullptr, nullptr);

    int w, h, channels;
    // Просим stb всегда отдавать 4 канала (RGBA)
    uint8_t* data = stbi_load(pathA, &w, &h, &channels, 4);

    if (data)
    {
        texW = (uint32_t)w;
        texH = (uint32_t)h;
        pixels.assign(data, data + texW * texH * 4);
        stbi_image_free(data);
    }
    else
    {
        // Шахматный fallback если файл не найден
        texW = texH = 64;
        pixels.resize(texW * texH * 4);
        for (uint32_t y = 0; y < texH; ++y)
            for (uint32_t x = 0; x < texW; ++x)
            {
                bool w = ((x / 8) + (y / 8)) % 2 == 0;
                uint32_t i = (y * texW + x) * 4;
                pixels[i + 0] = w ? 255u : 50u;
                pixels[i + 1] = w ? 255u : 200u;
                pixels[i + 2] = w ? 255u : 50u;
                pixels[i + 3] = 255u;
            }
    }

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = texW; td.Height = texH; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = kFmt; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    auto defH = D3D12_HEAP_PROPERTIES{ D3D12_HEAP_TYPE_DEFAULT, {}, {}, 1, 1 };
    ThrowIfFailed2(m_device->CreateCommittedResource(&defH, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outTex)), "Create Texture");

    UINT64 uploadSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT numRows = 0; UINT64 rowSz = 0;
    m_device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &numRows, &rowSz, &uploadSize);

    ComPtr<ID3D12Resource> upBuf;
    auto upH = D3D12_HEAP_PROPERTIES{ D3D12_HEAP_TYPE_UPLOAD, {}, {}, 1, 1 };
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = uploadSize;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ThrowIfFailed2(m_device->CreateCommittedResource(&upH, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upBuf)), "Tex Upload Buf");

    {
        uint8_t* p = nullptr; D3D12_RANGE rr{0,0};
        ThrowIfFailed2(upBuf->Map(0, &rr, reinterpret_cast<void**>(&p)), "Map Tex Upload");
        for (uint32_t row = 0; row < numRows; ++row)
            std::memcpy(p + fp.Offset + (UINT64)row * fp.Footprint.RowPitch,
                        pixels.data() + (size_t)row * texW * 4, (size_t)texW * 4);
        upBuf->Unmap(0, nullptr);
    }

    ThrowIfFailed2(m_cmdAlloc->Reset(), "Alloc TexUp");
    ThrowIfFailed2(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "List TexUp");

    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = outTex.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upBuf.Get();  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    m_cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition = { outTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
    m_cmdList->ResourceBarrier(1, &bar);

    ThrowIfFailed2(m_cmdList->Close(), "Close TexUp");
    ID3D12CommandList* ls[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, ls);
    FlushCommandQueue();
    return true;
}
