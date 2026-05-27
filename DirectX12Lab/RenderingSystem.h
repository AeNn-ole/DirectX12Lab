#pragma once
// NOMINMAX до любого windows.h — иначе макросы min/max сломают std::
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

    void ToggleFrustumCulling()  { m_frustumCullingEnabled = !m_frustumCullingEnabled; }
    void ToggleOctreeCulling()   { m_octreeCullingEnabled  = !m_octreeCullingEnabled;  }
    bool IsFrustumCullingOn()    const { return m_frustumCullingEnabled; }
    bool IsOctreeCullingOn()     const { return m_octreeCullingEnabled;  }
    int  GetVisibleCount()       const { return m_lastVisibleCount; }

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

private:
    // ── Константы — объявляем ПЕРВЫМИ чтобы массивы ниже их видели ───────
    static constexpr uint32_t kSwapChainBufferCount = 2;
    static constexpr int      kMaxInstances         = 2500;
    static constexpr int      kMaxLights            = 16;

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
        DirectX::XMFLOAT3 EyePosW;   float _pad0 = 0.f;
        DirectX::XMFLOAT3 LightDirW; float _pad1 = 0.f;
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
        Light               Lights[kMaxLights];
    };

    struct GpuMaterial { Microsoft::WRL::ComPtr<ID3D12Resource> texture; };

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

    Microsoft::WRL::ComPtr<ID3DBlob> m_gbVS, m_gbPS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_lightVS, m_lightPS;

    D3D12_INPUT_ELEMENT_DESC m_inputLayout[3]{};

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

    ObjModel                 m_model;
    std::vector<GpuMaterial> m_gpuMaterials;
    uint32_t                 m_numMaterials = 0;

    std::vector<InstanceData> m_instances;
    DirectX::XMFLOAT3         m_modelCenter{ 0.f, 0.f, 0.f };
    float                     m_modelRadius = 1.f;

    bool             m_frustumCullingEnabled = false;
    bool             m_octreeCullingEnabled  = false;
    FrustumPlanes    m_frustumPlanes{};
    Octree           m_octree;
    std::vector<int> m_visibleIndices;
    int              m_lastVisibleCount = 0;

    DirectX::XMFLOAT4X4 m_view{};
    DirectX::XMFLOAT4X4 m_proj{};
    DirectX::XMFLOAT3   m_eyePos{ 0.5f, 4.f, -5.f };
    DirectX::XMFLOAT3   m_lightDir{ 0.8f, -0.5f, 0.4f };

    float m_totalTime = 0.f;
    Light m_lights[kMaxLights]{};
    int   m_numLights = 0;
};
