// ═══════════════════════════════════════════════════════════════════════════
// RenderingSystem.cpp — только инфраструктура D3D12 и геометрия.
//
// Разбивка по файлам:
//   RenderingSystem.cpp  — Initialize, Shutdown, Create*, BuildGeometry, Scatter
//   RenderingSystem2.cpp — Update*, GeometryPass, LightingPass, BillboardPass, Draw
//   RenderingSystem3.cpp — BuildShaders, BuildCBs, BuildDescriptors, BuildRS, BuildPSOs
// ═══════════════════════════════════════════════════════════════════════════
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "RenderingSystem.h"
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <algorithm>
#include <DirectXMath.h>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

static void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (hr=0x%08X)", what, (unsigned)hr);
        throw std::runtime_error(buf);
    }
}

static D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type; p.CreationNodeMask = 1; p.VisibleNodeMask = 1;
    return p;
}

static D3D12_RESOURCE_DESC BufDesc(UINT64 bytes)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::Initialize(HWND hwnd, uint32_t width, uint32_t height)
{
    m_initialized = false;
    m_hwnd = hwnd; m_width = width; m_height = height;

#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
            dbg->EnableDebugLayer();
    }
#endif

    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory1");

    CreateDevice();
    CreateCommandObjects();

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&m_fence)), "CreateFence");
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) throw std::runtime_error("CreateEvent failed");

    CreateSwapChain();

    m_rtvDescriptorSize    = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_dsvDescriptorSize    = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_cbvSrvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CreateDescriptorHeaps();
    CreateRtvForBackBuffers();
    CreateDepthStencil();

    m_viewport    = { 0, 0, (float)width, (float)height, 0, 1 };
    m_scissorRect = { 0, 0, (LONG)width,  (LONG)height };

    XMVECTOR eye = XMVectorSet(m_eyePos.x, m_eyePos.y, m_eyePos.z, 1);
    XMStoreFloat4x4(&m_view, XMMatrixLookAtLH(eye, XMVectorZero(), XMVectorSet(0,1,0,0)));
    float asp = height > 0 ? (float)width / height : 1.f;
    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(0.25f * XM_PI, asp, 0.1f, 5000.f));

    // Загрузка модели
    if (!LoadObj(L"model.obj", m_model))
        throw std::runtime_error("Не удалось открыть model.obj");

    if (m_model.materials.empty())
        m_model.materials.push_back({ "__default__" });
    m_numMaterials = (uint32_t)m_model.materials.size();

    ComputeModelBounds();   // → RenderingSystem3.cpp
    ScatterInstances();
    BuildOctree();          // → RenderingSystem2.cpp

    BuildShaders();         // → RenderingSystem3.cpp
    BuildGeometry();
    BuildConstantBuffers(); // → RenderingSystem3.cpp
    BuildDescriptorViews(); // → RenderingSystem3.cpp
    BuildRootSignatures();  // → RenderingSystem3.cpp
    BuildPSOs();            // → RenderingSystem3.cpp
    InitLights();           // → RenderingSystem2.cpp

    m_initialized = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::Shutdown()
{
    if (m_cmdQueue) {
        const uint64_t v = ++m_fenceValue;
        m_cmdQueue->Signal(m_fence.Get(), v);
        if (m_fence->GetCompletedValue() < v) {
            m_fence->SetEventOnCompletion(v, m_fenceEvent);
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }
    auto unmap = [](ComPtr<ID3D12Resource>& r, uint8_t*& p) {
        if (r && p) { r->Unmap(0, nullptr); p = nullptr; }
    };
    unmap(m_instanceCB,        m_mappedInstanceCB);
    unmap(m_materialCB,        m_mappedMaterialCB);
    unmap(m_lightingCB,        m_mappedLightingCB);
    unmap(m_billboardFrameCB,  m_mappedBillboardFrameCB);
    unmap(m_billboardCentersBuf, m_mappedBillboardCenters);
    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::ScatterInstances()
{
    m_instances.clear();
    m_instances.reserve(kMaxInstances);

    const int   side    = (int)std::sqrtf((float)kMaxInstances);
    const float spacing = m_modelRadius * 2.f * 1.5f;
    const float offset  = -spacing * (side - 1) * 0.5f;

    for (int z = 0; z < side; ++z)
    for (int x = 0; x < side; ++x)
    {
        InstanceData d{};
        float wx = offset + x * spacing;
        float wz = offset + z * spacing;
        XMStoreFloat4x4(&d.World, XMMatrixTranslation(wx, 0.f, wz));
        d.Center = { wx + m_modelCenter.x, m_modelCenter.y, wz + m_modelCenter.z };
        d.Radius = m_modelRadius;
        m_instances.push_back(d);
    }

    // Порог переключения 3D → billboard:
    // объекты дальше 4 шагов сетки от камеры рисуются как спрайты.
    // При side=50 и spacing~3*radius это примерно первые ~2-3 "ряда" вокруг камеры
    // остаются 3D, остальные ~2400 — билборды.
    m_billboardDistance = spacing * 4.f;
}

// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::CreateDevice()
{
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
    if (FAILED(hr)) {
        ComPtr<IDXGIAdapter> warp;
        ThrowIfFailed(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "EnumWarpAdapter");
        ThrowIfFailed(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&m_device)), "D3D12CreateDevice WARP");
    }
    return true;
}

bool RenderingSystem::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_cmdQueue)), "CreateCmdQueue");
    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_cmdAlloc)), "CreateCmdAlloc");
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&m_cmdList)), "CreateCmdList");
    ThrowIfFailed(m_cmdList->Close(), "CmdList Close init");
    return true;
}

bool RenderingSystem::CreateSwapChain()
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc   = { m_width, m_height, {60,1}, DXGI_FORMAT_R8G8B8A8_UNORM };
    sd.SampleDesc   = { 1, 0 };
    sd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount  = kSwapChainBufferCount;
    sd.OutputWindow = m_hwnd;
    sd.Windowed     = TRUE;
    sd.SwapEffect   = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    m_swapChain.Reset();
    ThrowIfFailed(m_factory->CreateSwapChain(m_cmdQueue.Get(), &sd,
        m_swapChain.GetAddressOf()), "CreateSwapChain");
    m_currBackBuffer = 0;
    return true;
}

bool RenderingSystem::CreateDescriptorHeaps()
{
    {
        D3D12_DESCRIPTOR_HEAP_DESC rd{};
        rd.NumDescriptors = kSwapChainBufferCount + GBuffer::Count;
        rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&m_rtvHeap)), "RTV Heap");
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC dd{};
        dd.NumDescriptors = 1;
        dd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&m_dsvHeap)), "DSV Heap");
    }
    return true;
}

bool RenderingSystem::CreateRtvForBackBuffers()
{
    auto h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kSwapChainBufferCount; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_swapChainBuffers[i])), "GetBuffer");
        m_device->CreateRenderTargetView(m_swapChainBuffers[i].Get(), nullptr, h);
        h.ptr += m_rtvDescriptorSize;
    }
    return true;
}

bool RenderingSystem::CreateDepthStencil()
{
    m_depthStencilBuffer.Reset();
    D3D12_RESOURCE_DESC td{};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = m_width; td.Height = m_height;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format           = DXGI_FORMAT_R24G8_TYPELESS;
    td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    cv.DepthStencil.Depth = 1.f;

    auto hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COMMON, &cv, IID_PPV_ARGS(&m_depthStencilBuffer)), "Create DS");

    ThrowIfFailed(m_cmdAlloc->Reset(), "Alloc DS");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "List DS");
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { m_depthStencilBuffer.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                     D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE };
    m_cmdList->ResourceBarrier(1, &b);
    ThrowIfFailed(m_cmdList->Close(), "CmdList Close DS");
    ID3D12CommandList* ls[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, ls);

    const uint64_t v = ++m_fenceValue;
    m_cmdQueue->Signal(m_fence.Get(), v);
    if (m_fence->GetCompletedValue() < v) {
        m_fence->SetEventOnCompletion(v, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvd{};
    dsvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(m_depthStencilBuffer.Get(), &dsvd,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

void RenderingSystem::RecreateDepthSRV()
{
    const uint32_t N = m_numMaterials;
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)((2 * N + 3) * m_cbvSrvDescriptorSize);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format                  = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels     = 1;
    m_device->CreateShaderResourceView(m_depthStencilBuffer.Get(), &srvd, h);
}

// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::BuildGeometry()
{
    const UINT64 vbBytes = (UINT64)m_model.vertices.size() * sizeof(ObjVertex);
    const UINT64 ibBytes = (UINT64)m_model.indices.size()  * sizeof(uint32_t);

    auto defHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto upHeap  = HeapProps(D3D12_HEAP_TYPE_UPLOAD);

    auto vbDesc = BufDesc(vbBytes);
    auto ibDesc = BufDesc(ibBytes);

    ThrowIfFailed(m_device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE,
        &vbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_vertexBufferGPU)), "VB GPU");
    ThrowIfFailed(m_device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE,
        &ibDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_indexBufferGPU)), "IB GPU");

    ComPtr<ID3D12Resource> vbUp, ibUp;
    ThrowIfFailed(m_device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE,
        &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&vbUp)), "VB Upload");
    ThrowIfFailed(m_device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE,
        &ibDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&ibUp)), "IB Upload");

    auto upload = [](ID3D12Resource* r, const void* data, size_t sz) {
        void* p; D3D12_RANGE rr{0,0};
        r->Map(0,&rr,&p); std::memcpy(p,data,sz); r->Unmap(0,nullptr);
    };
    upload(vbUp.Get(), m_model.vertices.data(), (size_t)vbBytes);
    upload(ibUp.Get(),  m_model.indices.data(),  (size_t)ibBytes);

    ThrowIfFailed(m_cmdAlloc->Reset(), "Alloc Geom");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "List Geom");

    m_cmdList->CopyBufferRegion(m_vertexBufferGPU.Get(), 0, vbUp.Get(), 0, vbBytes);
    m_cmdList->CopyBufferRegion(m_indexBufferGPU.Get(),  0, ibUp.Get(), 0, ibBytes);

    D3D12_RESOURCE_BARRIER bars[2]{};
    bars[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[0].Transition = { m_vertexBufferGPU.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER };
    bars[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[1].Transition = { m_indexBufferGPU.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER };
    m_cmdList->ResourceBarrier(2, bars);

    ThrowIfFailed(m_cmdList->Close(), "CmdList Close Geom");
    ID3D12CommandList* ls[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, ls);

    const uint64_t v = ++m_fenceValue;
    m_cmdQueue->Signal(m_fence.Get(), v);
    if (m_fence->GetCompletedValue() < v) {
        m_fence->SetEventOnCompletion(v, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    m_vbv = { m_vertexBufferGPU->GetGPUVirtualAddress(), (UINT)vbBytes, sizeof(ObjVertex) };
    m_ibv = { m_indexBufferGPU->GetGPUVirtualAddress(),  (UINT)ibBytes, DXGI_FORMAT_R32_UINT };
    return true;
}
