#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <cstdint>
#include <vector>
#include <algorithm>
#include "ObjLoader.h"
#include "GBuffer.h"
#include "Octree.h"

class RenderingSystem
{
public:
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    void OnResize(uint32_t width, uint32_t height);
    void Draw();
    void SetCamera(const DirectX::XMFLOAT3& eyePos, float yaw, float pitch);

    // ── Culling toggles (сохраняем) ───────────────────────────────────────
    void ToggleFrustumCulling()  { m_frustumCullingEnabled = !m_frustumCullingEnabled; }
    void ToggleOctreeCulling()   { m_octreeCullingEnabled  = !m_octreeCullingEnabled;  }
    bool IsFrustumCullingOn()    const { return m_frustumCullingEnabled; }
    bool IsOctreeCullingOn()     const { return m_octreeCullingEnabled;  }
    int  GetVisibleCount()       const { return m_lastVisibleCount; }

    // ── Тесселяция ────────────────────────────────────────────────────────
    void  SetTessFactor(float nearF, float farF)
    { m_tessFactorNear = nearF; m_tessFactorFar = farF; }
    void  SetTessDistances(float dNear, float dFar)
    { m_tessDistNear = dNear; m_tessDistFar = dFar; }
    void  SetDisplacementScale(float s) { m_displacementScale = s; }
    void  ToggleNormalMapping()  { m_normalMappingEnabled  = !m_normalMappingEnabled; }
    void  ToggleTessellation()   { m_tessellationEnabled   = !m_tessellationEnabled;  }
    void  ToggleWireframe()      { m_wireframe = !m_wireframe; }
    void  SetPostFxMode(int m)   { m_postFxMode = m; }
    int   GetPostFxMode()        const { return m_postFxMode; }
    void  CyclePostFx()          { m_postFxMode = (m_postFxMode + 1) % 4; }
    void  ToggleDebugCascades()  { m_debugCascades = !m_debugCascades; }
    bool  IsDebugCascadesOn()    const { return m_debugCascades; }
    void  AdjustShadowTexTiling(float m) { m_shadowTexTiling = (std::max)(0.001f, m_shadowTexTiling + m); }
    float GetShadowTexTiling()   const { return m_shadowTexTiling; }

    void  ToggleWater()          { m_waterEnabled = !m_waterEnabled; }
    bool  IsWaterOn()            const { return m_waterEnabled; }
    void  SetWaterSpeedMul(float m) { m_waterSpeedMul = (std::max)(0.f, m_waterSpeedMul + m); }
    void  SetWaterAmpMul(float m)   { m_waterAmpMul   = (std::max)(0.f, m_waterAmpMul + m); }
    float GetWaterSpeedMul()     const { return m_waterSpeedMul; }
    float GetWaterAmpMul()       const { return m_waterAmpMul; }
    bool  IsTessellationOn()     const { return m_tessellationEnabled; }
    bool  IsNormalMappingOn()    const { return m_normalMappingEnabled; }
    bool  IsWireframeOn()        const { return m_wireframe; }
    float GetTessFactorNear()    const { return m_tessFactorNear; }
    float GetDisplacementScale() const { return m_displacementScale; }

private:
    bool CreateDevice();
    bool CreateCommandObjects();
    bool CreateSwapChain();
    bool CreateDescriptorHeaps();
    bool CreateRtvForBackBuffers();
    bool CreateDepthStencil();

    bool BuildShaders();
    bool BuildGeometry();
    bool BuildConstantBuffers();
    bool BuildDescriptorViews();
    bool BuildRootSignatures();
    bool BuildPSOs();
    void InitLights();

    void ComputeModelBounds();
    void ScatterInstances();
    void BuildOctree();

    void UpdateInstanceCB(int instIdx);
    void UpdateMaterialCB(int mi);
    void UpdateLightingCB();
    void CollectVisibleInstances();

    void GeometryPass();
    void LightingPass();

    bool LoadAndUploadTexture(const wchar_t* path,
                              Microsoft::WRL::ComPtr<ID3D12Resource>& outTex);
    void FlushCommandQueue();
    D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferRTV() const;
    ID3D12Resource*              CurrentBackBuffer()    const;
    void RecreateDepthSRV();
    bool CreateShadowMap();
    bool BuildShadowPSO();
    void UpdateCsmCascades();
    void ShadowPass();
    bool BuildWaterGeometry();
    bool BuildWaterPSO();
    bool CreateShadowAreaTexture();
    void UpdateWaterCB();
    void WaterPass();

    // Пересоздать PSO при смене режима тесселяции
    void RebuildGeometryPSO();
    bool CreateHdrRT();
    bool BuildPostFxPSO();
    void PostFxPass();

private:
    // ── Константы ─────────────────────────────────────────────────────────
    static constexpr uint32_t kSwapChainBufferCount = 2;
    static constexpr int      kMaxInstances         = 1;
    static constexpr int      kMaxLights            = 16;
    static constexpr int      kCsmCascades          = 4;
    static constexpr uint32_t kShadowMapSize         = 2048;

    // ─────────────────────────────────────────────────────────────────────
    // Раскладка дескрипторной кучи CBV/SRV (shader-visible):
    //
    //   [0  .. N-1 ]      CBV → PerMaterialCB слот i         (geom param[1])
    //   [N  .. 2N-1]      SRV → albedo-текстура материала i  (geom param[2])
    //   [2N .. 3N-1]      SRV → normal map материала i       (geom param[3])
    //   [3N .. 4N-1]      SRV → displacement map материала i (geom/DS param[4])
    //   [4N .. 4N+2]      SRV → G-Buffer (albedo, norm, spec) (light param[0])
    //   [4N+3]            SRV → глубина                        (light param[0]+3)
    // ─────────────────────────────────────────────────────────────────────

    // ── Вложенные структуры ───────────────────────────────────────────────
    struct InstanceData
    {
        DirectX::XMFLOAT4X4 World;
        DirectX::XMFLOAT3   Center;
        float               Radius;
    };

    struct alignas(16) PerInstanceCB
    {
        DirectX::XMFLOAT4X4 World;
        DirectX::XMFLOAT4X4 WorldViewProj;
        DirectX::XMFLOAT3   EyePosW;   float _pad0 = 0.f;
        DirectX::XMFLOAT3   LightDirW; float _pad1 = 0.f;
    };

    struct alignas(16) PerMaterialCB
    {
        DirectX::XMFLOAT4 Ambient;
        DirectX::XMFLOAT4 Diffuse;
        DirectX::XMFLOAT4 Specular;
        float SpecPower = 32.f; float _pad2[3] = {};

        float gTime;            float _padTime[3] = {};
        DirectX::XMFLOAT2 UVOffset{ 0.f, 0.f };
        DirectX::XMFLOAT2 UVTiling{ 1.f, 1.f };

        // ── Параметры тесселяции / displacement ──────────────────────────
        float TessFactorNear  = 8.f;   // макс. фактор тесселяции (рядом)
        float TessFactorFar   = 1.f;   // мин. фактор (далеко)
        float TessDistNear    = 10.f;  // дистанция «рядом»
        float TessDistFar     = 200.f; // дистанция «далеко»

        float DisplacementScale = 5.f; // амплитуда смещения по дисплейсменту
        int   EnableNormalMap   = 1;   // 1 = использовать normal map
        float _padTess[2]       = {};
    };

    struct alignas(16) Light
    {
        DirectX::XMFLOAT3 Position;   float Range     = 500.f;
        DirectX::XMFLOAT3 Direction;  float SpotAngle = 0.f;
        DirectX::XMFLOAT4 Color       { 1.f, 1.f, 1.f, 1.f };
        int               Type        = 0;
        float             _pad[3]     = {};
    };

    struct alignas(16) LightingConstants
    {
        DirectX::XMFLOAT4X4 InvViewProj;
        DirectX::XMFLOAT3   EyePosW; float _p0 = 0.f;
        DirectX::XMFLOAT2   ScreenSize;
        int                 NumLights = 0;
        float               _p1      = 0.f;
        DirectX::XMFLOAT3   CameraForward; float _p1b = 0.f;
        Light               Lights[kMaxLights];

        // CSM
        DirectX::XMFLOAT4X4 LightViewProj[kCsmCascades];
        DirectX::XMFLOAT4   CascadeFarPlanes;   // x,y,z,w = far plane 0..3
        int                 NumCascades   = 0;
        int                 DebugCascades = 0;
        float               _p2[2]        = {};
        float               ShadowMapSize = (float)kShadowMapSize;
        float               ShadowBias    = 0.005f;
        float               ShadowTexTiling = 0.1f;
        float               _p3[1]        = {};
    };

    // Данные одного каскада
    struct CsmCascade
    {
        DirectX::XMFLOAT4X4 LightViewProj;
        float                FarPlane;     // дистанция в world units (от камеры)
    };

    struct GpuMaterial
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        Microsoft::WRL::ComPtr<ID3D12Resource> normalMap;
        Microsoft::WRL::ComPtr<ID3D12Resource> dispMap;
    };

    // ── Состояние ─────────────────────────────────────────────────────────
    bool     m_initialized  = false;
    HWND     m_hwnd         = nullptr;
    uint32_t m_width        = 0;
    uint32_t m_height       = 0;

    // ── D3D12 ─────────────────────────────────────────────────────────────
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

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_cbvSrvHeap;

    uint32_t m_rtvDescriptorSize    = 0;
    uint32_t m_dsvDescriptorSize    = 0;
    uint32_t m_cbvSrvDescriptorSize = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthStencilBuffer;
    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT     m_scissorRect{};

    GBuffer m_gbuffer;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_geometryRootSig;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_lightingRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_geometryPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_lightingPSO;

    // Шейдеры geometry pass (VS/HS/DS/PS)
    Microsoft::WRL::ComPtr<ID3DBlob> m_gbVS, m_gbHS, m_gbDS, m_gbPS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_lightVS, m_lightPS;

    // 4 компонента: POSITION, NORMAL, TANGENT, TEXCOORD
    D3D12_INPUT_ELEMENT_DESC m_inputLayout[4]{};

    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBufferGPU;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBufferGPU;
    D3D12_VERTEX_BUFFER_VIEW m_vbv{};
    D3D12_INDEX_BUFFER_VIEW  m_ibv{};

    Microsoft::WRL::ComPtr<ID3D12Resource> m_instanceCB;
    uint8_t* m_mappedInstanceCB   = nullptr;
    uint32_t m_instanceCBByteSize = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_materialCB;
    uint8_t* m_mappedMaterialCB   = nullptr;
    uint32_t m_materialCBByteSize = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightingCB;
    uint8_t* m_mappedLightingCB   = nullptr;

    // ── Shadow Map (Texture2DArray, kCsmCascades слоёв) ───────────────────
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_shadowMap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_shadowDsvHeap; // 4 DSV
    CsmCascade  m_cascades[kCsmCascades]{};

    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_shadowRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_shadowPSO;
    Microsoft::WRL::ComPtr<ID3DBlob>             m_shadowVS;

    Microsoft::WRL::ComPtr<ID3D12Resource>       m_shadowCB; // per-instance light CB
    uint8_t*                                     m_mappedShadowCB = nullptr;

    bool  m_debugCascades = false;

    // ═══════════════════════════════════════════════════════════════════
    // Вода — процедурная тесселированная анимированная поверхность
    // ═══════════════════════════════════════════════════════════════════
    struct WaterVertex { DirectX::XMFLOAT3 Pos; };

    struct alignas(16) WaterCB
    {
        DirectX::XMFLOAT4X4 World;
        DirectX::XMFLOAT4X4 WorldViewProj;
        DirectX::XMFLOAT3   EyePosW;   float Time = 0.f;
        DirectX::XMFLOAT3   LightDirW; float _pad0 = 0.f;

        DirectX::XMFLOAT4   WaveDir0Amp{  1.0f, 0.3f, 1.2f, 0.04f };
        DirectX::XMFLOAT4   WaveDir1Amp{  0.4f, 1.0f, 0.7f, 0.08f };
        DirectX::XMFLOAT4   WaveDir2Amp{ -0.7f, 0.6f, 0.4f, 0.15f };
        DirectX::XMFLOAT4   WaveDir3Amp{  0.8f,-0.5f, 0.25f,0.30f };
        DirectX::XMFLOAT4   WaveSpeeds { 1.0f, 1.4f, 2.0f, 2.6f };

        float TessFactorNear = 12.f;
        float TessFactorFar  = 1.f;
        float TessDistNear   = 15.f;
        float TessDistFar    = 250.f;

        DirectX::XMFLOAT4 ShallowColor{ 0.25f, 0.55f, 0.55f, 1.f };
        DirectX::XMFLOAT4 DeepColor   { 0.02f, 0.10f, 0.18f, 1.f };
    };

    static constexpr int kWaterGridN     = 64;     // N×N вершин сетки
    static constexpr float kWaterWorldSize = 400.f; // размер плоскости в world units

    Microsoft::WRL::ComPtr<ID3D12Resource> m_waterVB;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_waterIB;
    D3D12_VERTEX_BUFFER_VIEW m_waterVbv{};
    D3D12_INDEX_BUFFER_VIEW  m_waterIbv{};
    uint32_t                 m_waterIndexCount = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_waterRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_waterPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_waterWirePSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_waterVS, m_waterHS, m_waterDS, m_waterPS;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_waterCB;
    uint8_t* m_mappedWaterCB = nullptr;

    bool  m_waterEnabled   = true;
    float m_waterSpeedMul  = 1.f;
    float m_waterAmpMul    = 1.f;
    float m_waterY         = 0.f; // высота плоскости воды (мировой Y)

    // ── Текстура для затенённых областей (доп. задание) ───────────────────
    // Полностью заменяет цвет пикселя там, где shadowFactor близок к 0.
    // SRV кладётся в cbvSrvHeap слот [4N+6], читается в Lighting.hlsl как t5.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowTex;
    float m_shadowTexTiling = 0.1f; // масштаб world-space UV для тайлинга

    ObjModel                 m_model;
    std::vector<GpuMaterial> m_gpuMaterials;
    uint32_t                 m_numMaterials = 0;

    std::vector<InstanceData> m_instances;
    DirectX::XMFLOAT3         m_modelCenter{ 0.f, 0.f, 0.f };
    float                     m_modelRadius = 1.f;

    // ── Culling ───────────────────────────────────────────────────────────
    bool             m_frustumCullingEnabled = false;
    bool             m_octreeCullingEnabled  = false;
    FrustumPlanes    m_frustumPlanes{};
    Octree           m_octree;
    std::vector<int> m_visibleIndices;
    int              m_lastVisibleCount = 0;

    // ── Камера / свет ─────────────────────────────────────────────────────
    DirectX::XMFLOAT4X4 m_view{};
    DirectX::XMFLOAT4X4 m_proj{};
    DirectX::XMFLOAT3   m_eyePos{ 0.5f, 4.f, -5.f };
    DirectX::XMFLOAT3   m_lightDir{ 0.8f, -0.5f, 0.4f };

    float m_totalTime = 0.f;
    Light m_lights[kMaxLights]{};
    int   m_numLights = 0;

    // ── Параметры тесселяции (CPU-сторона) ───────────────────────────────
    bool  m_tessellationEnabled  = true;
    bool  m_normalMappingEnabled = true;
    bool  m_wireframe            = false;
    float m_tessFactorNear       = 8.f;
    float m_tessFactorFar        = 1.f;
    float m_tessDistNear         = 10.f;
    float m_tessDistFar          = 200.f;
    float m_displacementScale    = 5.f;

    // Wireframe PSO для визуальной проверки тесселяции (Z)
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_geometryWirePSO;

    // ── Post-process ──────────────────────────────────────────────────────
    // HDR RT — промежуточный таргет: lighting пишет сюда, post-fx читает
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_hdrRT;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_hdrRtvHeap; // отдельная куча RTV
    // SRV для hdrRT лежит в m_cbvSrvHeap в слоте [4N+4]
    D3D12_CPU_DESCRIPTOR_HANDLE m_hdrRtv{};

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_postFxRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_postFxPSO;
    Microsoft::WRL::ComPtr<ID3DBlob>            m_postVS, m_postPS;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_postFxCB;
    uint8_t* m_mappedPostFxCB = nullptr;

    int   m_postFxMode    = 0;     // 0=off 1=fisheye 2=VHS 3=both
    float m_fishStrength  = 0.5f;
    float m_vhsStrength   = 1.0f;

    struct alignas(16) PostFxConstants
    {
        float gTime;
        int   gMode;
        float gFishStrength;
        float gVhsStrength;
    };
};
