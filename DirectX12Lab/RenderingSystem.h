#pragma once
#ifndef NOMINMAX
#define NOMINMAX   // избегаем конфликта windows.h min/max с std::min/std::max
#endif
#include <windows.h>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <cstdint>
#include <vector>
#include "ObjLoader.h"
#include "GBuffer.h"

// ─────────────────────────────────────────────────────────────────────────────
// RenderingSystem — оркестратор двухпроходного deferred rendering.
//
// Проход 1 — Geometry Pass (m_geometryPSO):
//   Рисует всю сцену один раз; пиксельный шейдер записывает НЕ освещение,
//   а «сырые» данные о поверхности в три текстуры G-Buffer:
//     RT0 Albedo   R8G8B8A8   — цвет диффуза
//     RT1 Normal   R16G16_FL  — нормаль в world space (float16)
//     RT2 Specular R8G8B8A8   — Ks + Ns
//
// Проход 2 — Lighting Pass (m_lightingPSO):
//   Один fullscreen quad (3 вершины из SV_VertexID, без VB).
//   Пиксельный шейдер читает G-Buffer и глубину, реконструирует мировую позицию
//   через InvViewProj, считает Phong-освещение ОДИН РАЗ на пиксель.
//
// Раскладка CBV/SRV-кучи (shader-visible):
//   [0  .. N-1 ]  CBV — константные буферы по материалам (geometry pass)
//   [N  .. 2N-1]  SRV — текстуры материалов (geometry pass)
//   [2N .. 2N+2]  SRV — G-Buffer: albedo, normal, specular (lighting pass t0-t2)
//   [2N+3]        SRV — глубина R24_UNORM_X8_TYPELESS (lighting pass t3)
//   [2N+4]        SRV — StructuredBuffer<Light> с источниками света (lighting pass t4)
//
// Источники света хранятся в StructuredBuffer, а НЕ в constant buffer:
// constant buffer слишком мал для большого количества источников
// (дождь из ~200 точечных огней на полу Sponza).
//
// Раскладка RTV-кучи:
//   [0,1]    — swap chain back buffers
//   [2,3,4]  — G-Buffer RT0, RT1, RT2
// ─────────────────────────────────────────────────────────────────────────────
class RenderingSystem
{
public:
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    void OnResize(uint32_t width, uint32_t height);
    void Draw();
    void SetCamera(const DirectX::XMFLOAT3& eyePos, float yaw, float pitch);

private:
    // ── D3D12-инфраструктура (переехала из D3D12Context) ──────────────────
    bool CreateDevice();
    bool CreateCommandObjects();
    bool CreateSwapChain();
    bool CreateDescriptorHeaps();        // RTV(5) + DSV(1)
    bool CreateRtvForBackBuffers();
    bool CreateDepthStencil();           // R24G8_TYPELESS — чтобы SRV тоже работал

    // ── Построение ресурсов (вызываются один раз при Initialize) ─────────
    bool BuildShaders();          // компиляция 4 точек входа из 2 .hlsl файлов
    bool BuildGeometry();         // VB/IB из m_model
    bool BuildConstantBuffers();  // geometry CB (N слотов) + lighting CB (1 слот)
    bool BuildDescriptorViews();  // создаёт CBV/SRV кучу, регистрирует всё
    bool BuildRootSignatures();   // два отдельных root sig
    bool BuildPSOs();             // geometry PSO (3 RT) + lighting PSO (1 RT)
    void InitLights();            // расставляет источники света по сцене
    void InitRainDrops();         // инициализирует капли дождя
    void UpdateRain(float dt);    // обновляет падение/приземление капель

    // ── Обновление данных каждый кадр ────────────────────────────────────
    void UpdateMaterialCB(int mi);   // обновляет слот mi в geometry CB
    void UpdateLightingCB();         // обновляет InvViewProj + eye/light

    // ── Два прохода рендеринга ────────────────────────────────────────────
    void GeometryPass();    // рисует сцену → G-Buffer (3 RT + depth)
    void LightingPass();    // fullscreen triangle → back buffer

    // ── Вспомогательные ──────────────────────────────────────────────────
    bool LoadAndUploadTexture(const wchar_t* path,
                              Microsoft::WRL::ComPtr<ID3D12Resource>& outTex);
    void FlushCommandQueue();
    D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferRTV() const;
    ID3D12Resource*              CurrentBackBuffer()    const;
    void RecreateDepthSRV();   // выделено: пересоздаётся при OnResize

private:
    static constexpr uint32_t kSwapChainBufferCount = 2;

    // ─────────────────────────────────────────────────────────────────────
    // Geometry pass: константный буфер (один слот на материал)
    // Структура ПОЛНОСТЬЮ совпадает с оригиналом — не меняем шейдер объявления
    // ─────────────────────────────────────────────────────────────────────
    struct alignas(16) ObjectConstants
    {
        DirectX::XMFLOAT4X4 World;
        DirectX::XMFLOAT4X4 WorldViewProj;

        DirectX::XMFLOAT3 EyePosW;   float _pad0 = 0.f;
        DirectX::XMFLOAT3 LightDirW; float _pad1 = 0.f;

        DirectX::XMFLOAT4 Ambient;
        DirectX::XMFLOAT4 Diffuse;
        DirectX::XMFLOAT4 Specular;
        float SpecPower = 32.f;
        float _pad2[3]  = { 0, 0, 0 };

        float gTime;
        float padTime[3];

        DirectX::XMFLOAT2 UVOffset{ 0.f, 0.f };
        DirectX::XMFLOAT2 UVTiling{ 1.f, 1.f };
    };

    // ─────────────────────────────────────────────────────────────────────
    // Источник света (CPU-сторона).
    // Структура БАЙТ В БАЙТ совпадает с Light в Lighting.hlsl.
    //
    // Выравнивание в HLSL constant buffer: каждый float3 + float = 16 байт,
    // float4 = 16 байт, int + float[3] = 16 байт → итого 64 байта на свет.
    // ─────────────────────────────────────────────────────────────────────
    struct alignas(16) Light
    {
        // float3 + float = 16 байт — одна строка CB
        DirectX::XMFLOAT3 Position;   float Range      = 500.f;
        // float3 + float = 16 байт
        DirectX::XMFLOAT3 Direction;  float SpotAngle  = 0.f; // cos(halfAngle)
        // float4 = 16 байт
        DirectX::XMFLOAT4 Color       { 1.f, 1.f, 1.f, 1.f };
        // int + float[3] = 16 байт
        int               Type        = 0;   // 0=Directional 1=Point 2=Spot
        float             _pad[3]     = {};
    };

    // 200 — преподаватель сказал что 16 (предел constant buffer) слишком мало
    // для дождя. Источники теперь хранятся в StructuredBuffer, не в CB,
    // так что 200 — не проблема.
    static constexpr int kMaxLights = 200;

    // ─────────────────────────────────────────────────────────────────────
    // Lighting pass: один CB на кадр.
    // Массив источников УБРАН из этой структуры — он живёт в отдельном
    // StructuredBuffer (m_lightsBuffer), т.к. constant buffer слишком мал
    // для 200 источников (200*64 = 12800 байт, а CB ограничен 65536, но
    // главное — обновление через memcpy в structured buffer проще и
    // гибче, и снимает искусственное ограничение в 16).
    // ─────────────────────────────────────────────────────────────────────
    struct alignas(16) LightingConstants
    {
        DirectX::XMFLOAT4X4 InvViewProj;        // реконструкция pos из depth
        DirectX::XMFLOAT3   EyePosW; float _p0 = 0.f;
        DirectX::XMFLOAT2   ScreenSize;
        int                 NumLights = 0;
        float               _p1      = 0.f;
    };

    struct GpuMaterial
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    };

    // ── Состояние ─────────────────────────────────────────────────────────
    bool     m_initialized  = false;
    HWND     m_hwnd         = nullptr;
    uint32_t m_width        = 0;
    uint32_t m_height       = 0;

    // ── D3D12-объекты ─────────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<IDXGIFactory4>             m_factory;
    Microsoft::WRL::ComPtr<ID3D12Device>              m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>        m_cmdQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_cmdAlloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;
    Microsoft::WRL::ComPtr<ID3D12Fence>               m_fence;
    uint64_t m_fenceValue = 0;
    HANDLE   m_fenceEvent = nullptr;

    Microsoft::WRL::ComPtr<IDXGISwapChain>  m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12Resource>  m_swapChainBuffers[kSwapChainBufferCount];
    uint32_t m_currBackBuffer = 0;

    // ── Дескрипторные кучи ────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;    // 5 слотов
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;    // 1 слот
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_cbvSrvHeap; // 2N+5 слотов

    uint32_t m_rtvDescriptorSize    = 0;
    uint32_t m_dsvDescriptorSize    = 0;
    uint32_t m_cbvSrvDescriptorSize = 0;

    // ── Глубина (R24G8_TYPELESS — нужна для SRV в lighting pass) ─────────
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthStencilBuffer;

    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT     m_scissorRect{};

    // ── G-Buffer ──────────────────────────────────────────────────────────
    GBuffer m_gbuffer;

    // ── Pipeline ──────────────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_geometryRootSig;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_lightingRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_geometryPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_lightingPSO;

    Microsoft::WRL::ComPtr<ID3DBlob> m_gbVS, m_gbPS;       // geometry pass
    Microsoft::WRL::ComPtr<ID3DBlob> m_lightVS, m_lightPS; // lighting pass

    D3D12_INPUT_ELEMENT_DESC m_inputLayout[3]{};

    // ── Геометрия ─────────────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBufferGPU;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBufferGPU;
    D3D12_VERTEX_BUFFER_VIEW m_vbv{};
    D3D12_INDEX_BUFFER_VIEW  m_ibv{};

    // ── Константные буферы ────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12Resource> m_objectCB;        // geometry: N слотов
    uint8_t* m_mappedObjectCB    = nullptr;
    uint32_t m_objectCBByteSize  = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightingCB;      // lighting: 1 слот
    uint8_t* m_mappedLightingCB  = nullptr;

    // ── StructuredBuffer источников света (upload heap, SRV t4) ───────────
    // 200 источников * 64 байта = 12800 байт. Заменяет массив в constant
    // buffer — позволяет легко иметь сотни источников (дождь).
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightsBuffer;
    uint8_t* m_mappedLightsBuffer = nullptr;
    static constexpr uint32_t kLightStride = sizeof(Light); // = 64 байта

    // ── Данные сцены ──────────────────────────────────────────────────────
    ObjModel                 m_model;
    std::vector<GpuMaterial> m_gpuMaterials;
    uint32_t                 m_numMaterials = 0;

    DirectX::XMFLOAT4X4 m_world{};
    DirectX::XMFLOAT4X4 m_view{};
    DirectX::XMFLOAT4X4 m_proj{};
    DirectX::XMFLOAT3   m_eyePos{ 0.5f, 4.f, -5.f };
    DirectX::XMFLOAT3   m_lightDir{ 0.8f, -0.5f, 0.4f };

    float m_totalTime = 0.f;

    // ── Источники света ───────────────────────────────────────────────────
    Light    m_lights[kMaxLights]{};
    int      m_numLights = 0;
    int      m_numBaseLights = 0; // сколько источников статичны (не дождь)

    // ─────────────────────────────────────────────────────────────────────
    // Дождь из точечных источников света, падающих на пол Sponza (Y=0)
    // и остающихся там навсегда (преподаватель просил, чтобы они НЕ
    // пропадали — поэтому m_landedDrops накапливается, а не сбрасывается
    // каждый кадр; при переполнении используется кольцевой буфер).
    //
    // Бюджет источников: 200 всего
    //    8   — статичные (солнце, колоннады, спот и т.д., InitLights)
    //    40  — падающие капли (постоянно активны)
    //    152 — «лужицы» на полу (накапливаются, потом вытесняются по кругу)
    // ─────────────────────────────────────────────────────────────────────
    struct RainDrop
    {
        float x = 0.f, z = 0.f;
        float y = 0.f;
        float speed = 0.f;
        bool  landed = false;
        float r = 1.f, g = 1.f, b = 1.f;
    };

    static constexpr int   kRainDrops     = 40;
    static constexpr int   kRainLanded    = kMaxLights - 8 - kRainDrops; // = 152
    static constexpr float kSpawnHeight   = 480.f;
    static constexpr float kFloorY        = 0.f; // пол Sponza

    RainDrop              m_rainDrops[kRainDrops]{};
    std::vector<RainDrop> m_landedDrops;     // накапливается до kRainLanded
    int                   m_landedWriteIdx = 0; // позиция для кольцевой записи

    // Таймер для delta-time симуляции дождя (независим от кадрового dt)
    LARGE_INTEGER m_rainFreq{};
    LARGE_INTEGER m_rainPrev{};
    bool          m_rainTimerInited = false;
};
