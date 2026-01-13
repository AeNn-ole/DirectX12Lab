#include "DirectContext.h"
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <array>
#include <cmath>
#include <DirectXMath.h>
using namespace DirectX;

using Microsoft::WRL::ComPtr;

static void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (hr=0x%08X)", what, (unsigned)hr);

        // Вывод в отладочную консоль
        OutputDebugStringA("DirectX Error: ");
        OutputDebugStringA(what);
        OutputDebugStringA("\n");

        throw std::runtime_error(buf);
    }
}

bool D3D12Context::Initialize(HWND hwnd, uint32_t width, uint32_t height)
{
    m_initialized = false;

    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
    }
#endif

    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory1");

    CreateDevice();
    CreateCommandObjects();

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");
    m_fenceValue = 0;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
        throw std::runtime_error("CreateEvent for fence failed");

    CreateSwapChain();

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    CreateDescriptorHeaps();
    CreateRtvForBackBuffers();
    CreateDepthStencil();

    m_viewport = { 0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f };
    m_scissorRect = { 0, 0, (LONG)m_width, (LONG)m_height };

    XMStoreFloat4x4(&m_world, DirectX::XMMatrixIdentity());

    XMVECTOR eye = XMVectorSet(m_eyePos.x, m_eyePos.y, m_eyePos.z, 1.0f);
    XMVECTOR target = XMVectorZero();
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMStoreFloat4x4(&m_view, XMMatrixLookAtLH(eye, target, up));

    float aspect = (m_height > 0) ? ((float)m_width / (float)m_height) : 1.0f;
    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(0.25f * XM_PI, aspect, 1.0f, 1000.0f));

    BuildShaders();
    BuildGeometry();
    BuildConstantBuffer();
    BuildRootSignature();
    BuildPSO();

    m_initialized = true;
    return true;
}

void D3D12Context::Shutdown()
{
    if (m_cmdQueue) FlushCommandQueue();

    if (m_objectCB && m_mappedObjectCB)
    {
        m_objectCB->Unmap(0, nullptr);
        m_mappedObjectCB = nullptr;
    }

    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

bool D3D12Context::CreateDevice()
{
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
    if (FAILED(hr))
    {
        ComPtr<IDXGIAdapter> warp;
        ThrowIfFailed(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "EnumWarpAdapter");
        ThrowIfFailed(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)), "D3D12CreateDevice (WARP)");
    }
    return true;
}

bool D3D12Context::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(m_device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&m_cmdQueue)), "CreateCommandQueue");

    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAlloc)), "CreateCommandAllocator");
    ThrowIfFailed(m_device->CreateCommandList(
        0,D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAlloc.Get(),
        nullptr, IID_PPV_ARGS(&m_cmdList)), "CreateCommandList");

    ThrowIfFailed(m_cmdList->Close(), "CommandList Close (initial)");
    return true;
}

bool D3D12Context::CreateSwapChain()
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width = m_width;
    sd.BufferDesc.Height = m_height;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kSwapChainBufferCount;
    sd.OutputWindow = m_hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = 0;

    m_swapChain.Reset();

    ThrowIfFailed(m_factory->CreateSwapChain(m_cmdQueue.Get(), &sd,
        m_swapChain.GetAddressOf()), "CreateSwapChain");

    m_currBackBuffer = 0;
    return true;
}

bool D3D12Context::CreateDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = kSwapChainBufferCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)), "Create RTV Heap");

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.NumDescriptors = 1;
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_dsvHeap)), "Create DSV Heap");

    return true;
}

bool D3D12Context::CreateRtvForBackBuffers()
{
    auto rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t i = 0; i < kSwapChainBufferCount; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_swapChainBuffers[i])), "SwapChain GetBuffer");
        m_device->CreateRenderTargetView(m_swapChainBuffers[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }
    return true;
}

bool D3D12Context::CreateDepthStencil()
{
    m_depthStencilBuffer.Reset();

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE optClear{};
    optClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    optClear.DepthStencil.Depth = 1.0f;
    optClear.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        &optClear,
        IID_PPV_ARGS(&m_depthStencilBuffer)
    ), "CreateCommittedResource (DepthStencil)");

    ThrowIfFailed(m_cmdAlloc->Reset(), "CmdAlloc Reset (DepthStencil)");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "CmdList Reset (DepthStencil)");

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_depthStencilBuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_cmdList->Close(), "CmdList Close (DepthStencil)");
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
    FlushCommandQueue();

    m_device->CreateDepthStencilView(
        m_depthStencilBuffer.Get(), nullptr,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart()
    );

    return true;
}

void D3D12Context::OnResize(uint32_t width, uint32_t height)
{
    if (!m_initialized) return;
    if (width == 0 || height == 0) return;

    m_width = width;
    m_height = height;

    FlushCommandQueue();

    for (auto& b : m_swapChainBuffers) b.Reset();
    m_depthStencilBuffer.Reset();

    ThrowIfFailed(m_swapChain->ResizeBuffers(
        kSwapChainBufferCount, m_width, m_height,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0), "SwapChain ResizeBuffers");

    m_currBackBuffer = 0;

    CreateRtvForBackBuffers();
    CreateDepthStencil();

    m_viewport = { 0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f };
    m_scissorRect = { 0, 0, (LONG)m_width, (LONG)m_height };

    float aspect = (m_height > 0) ? ((float)m_width / (float)m_height) : 1.0f;
    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(0.25f * XM_PI, aspect, 1.0f, 1000.0f));
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Context::CurrentBackBufferRTV() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (size_t)m_currBackBuffer * m_rtvDescriptorSize;
    return h;
}

ID3D12Resource* D3D12Context::CurrentBackBuffer() const
{
    return m_swapChainBuffers[m_currBackBuffer].Get();
}

void D3D12Context::Draw()
{
    if (!m_initialized) return;

    UpdateConstantBuffer();

    ThrowIfFailed(m_cmdAlloc->Reset(), "CmdAlloc Reset");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), m_pso.Get()), "CmdList Reset");

    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissorRect);

    m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());

    D3D12_RESOURCE_BARRIER toRT{};
    toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT.Transition.pResource = CurrentBackBuffer();
    toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &toRT);

    auto rtv = CurrentBackBufferRTV();
    auto dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_cmdList->OMSetRenderTargets(1, &rtv, TRUE, &dsv);

    const float clearColor[4] = { 0.10f, 0.10f, 0.35f, 1.0f };
    m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // кубек
    ID3D12DescriptorHeap* heaps[] = { m_cbvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);
    m_cmdList->SetGraphicsRootDescriptorTable(0, m_cbvHeap->GetGPUDescriptorHandleForHeapStart());

    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmdList->IASetVertexBuffers(0, 1, &m_vbv);
    m_cmdList->IASetIndexBuffer(&m_ibv);
    m_cmdList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);

    D3D12_RESOURCE_BARRIER toPresent = toRT;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_cmdList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(m_cmdList->Close(), "CmdList Close");

    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);

    ThrowIfFailed(m_swapChain->Present(1, 0), "SwapChain Present");
    m_currBackBuffer = (m_currBackBuffer + 1) % kSwapChainBufferCount;

    FlushCommandQueue();
}

void D3D12Context::SetCamera(const DirectX::XMFLOAT3& eyePos, float yaw, float pitch)
{
    using namespace DirectX;

    m_eyePos = eyePos;

    const float cy = cosf(yaw);
    const float sy = sinf(yaw);
    const float cp = cosf(pitch);
    const float sp = sinf(pitch);

    XMVECTOR forward = XMVector3Normalize(XMVectorSet(sy*cp, sp, cy*cp, 0.0f));
    XMVECTOR eye = XMVectorSet(eyePos.x, eyePos.y, eyePos.z, 1.0f);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMStoreFloat4x4(&m_view, XMMatrixLookToLH(eye, forward, up));
}

void D3D12Context::FlushCommandQueue()
{
    const uint64_t fenceToWaitFor = ++m_fenceValue;
    ThrowIfFailed(m_cmdQueue->Signal(m_fence.Get(), fenceToWaitFor), "Fence Signal");

    if (m_fence->GetCompletedValue() < fenceToWaitFor)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fenceToWaitFor, m_fenceEvent), "Fence SetEventOnCompletion");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

static uint32_t AlignConstantBufferByteSize(uint32_t byteSize)
{
    return (byteSize + 255u) & ~255u;
}

static D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

static D3D12_RESOURCE_DESC BufferDesc(UINT64 byteSize)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = byteSize;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

bool D3D12Context::BuildShaders()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errors;

    HRESULT hr = D3DCompileFromFile(
        L"Shaders.hlsl",
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain",
        "vs_5_0",
        flags,
        0,
        &m_vsBytecode,
        &errors
    );
    if (FAILED(hr))
    {
        if (errors) throw std::runtime_error((const char*)errors->GetBufferPointer());
        ThrowIfFailed(hr, "D3DCompileFromFile VS failed");
    }

    errors.Reset();
    hr = D3DCompileFromFile(
        L"Shaders.hlsl",
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain",
        "ps_5_0",
        flags,
        0,
        &m_psBytecode,
        &errors
    );
    if (FAILED(hr))
    {
        if (errors) throw std::runtime_error((const char*)errors->GetBufferPointer());
        ThrowIfFailed(hr, "D3DCompileFromFile PS failed");
    }

    m_inputLayout[0] = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    m_inputLayout[1] = { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
                         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };

    return true;
}

bool D3D12Context::BuildGeometry()
{
    const float s = 1.0f;

    const std::array<Vertex, 24> vertices =
    {
        Vertex{{+s,+s,-s},{+1,0,0}}, Vertex{{+s,+s,+s},{+1,0,0}}, Vertex{{+s,-s,+s},{+1,0,0}}, Vertex{{+s,-s,-s},{+1,0,0}},
        Vertex{{-s,+s,+s},{-1,0,0}}, Vertex{{-s,+s,-s},{-1,0,0}}, Vertex{{-s,-s,-s},{-1,0,0}}, Vertex{{-s,-s,+s},{-1,0,0}},
        Vertex{{-s,+s,-s},{0,+1,0}}, Vertex{{+s,+s,-s},{0,+1,0}}, Vertex{{+s,+s,+s},{0,+1,0}}, Vertex{{-s,+s,+s},{0,+1,0}},
        Vertex{{-s,-s,+s},{0,-1,0}}, Vertex{{+s,-s,+s},{0,-1,0}}, Vertex{{+s,-s,-s},{0,-1,0}}, Vertex{{-s,-s,-s},{0,-1,0}},
        Vertex{{+s,+s,+s},{0,0,+1}}, Vertex{{-s,+s,+s},{0,0,+1}}, Vertex{{-s,-s,+s},{0,0,+1}}, Vertex{{+s,-s,+s},{0,0,+1}},
        Vertex{{-s,+s,-s},{0,0,-1}}, Vertex{{+s,+s,-s},{0,0,-1}}, Vertex{{+s,-s,-s},{0,0,-1}}, Vertex{{-s,-s,-s},{0,0,-1}},
    };

    const std::array<uint16_t, 36> indices =
    {
        0,1,2, 0,2,3,
        4,5,6, 4,6,7,
        8,9,10, 8,10,11,
        12,13,14, 12,14,15,
        16,17,18, 16,18,19,
        20,21,22, 20,22,23
    };

    m_indexCount = (uint32_t)indices.size();

    const UINT64 vbByteSize = (UINT64)vertices.size()*sizeof(Vertex);
    const UINT64 ibByteSize = (UINT64)indices.size()*sizeof(uint16_t);

    D3D12_HEAP_PROPERTIES defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES uploadHeap  = HeapProps(D3D12_HEAP_TYPE_UPLOAD);

    D3D12_RESOURCE_DESC vbDesc = BufferDesc(vbByteSize);
    D3D12_RESOURCE_DESC ibDesc = BufferDesc(ibByteSize);

    ThrowIfFailed(m_device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &vbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_vertexBufferGPU)
    ), "Create vertex buffer (default)");

    ThrowIfFailed(m_device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &ibDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_indexBufferGPU)
    ), "Create index buffer (default)");

    ComPtr<ID3D12Resource> vertexUpload;
    ComPtr<ID3D12Resource> indexUpload;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&vertexUpload)
    ), "Create vertex buffer (upload)");

    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&indexUpload)
    ), "Create index buffer (upload)");

    {
        void* mapped = nullptr;
        D3D12_RANGE readRange{ 0,0 };
        ThrowIfFailed(vertexUpload->Map(0, &readRange, &mapped), "Map vertex upload");
        std::memcpy(mapped, vertices.data(), (size_t)vbByteSize);
        vertexUpload->Unmap(0, nullptr);
    }
    {
        void* mapped = nullptr;
        D3D12_RANGE readRange{ 0,0 };
        ThrowIfFailed(indexUpload->Map(0, &readRange, &mapped), "Map index upload");
        std::memcpy(mapped, indices.data(), (size_t)ibByteSize);
        indexUpload->Unmap(0, nullptr);
    }

    ThrowIfFailed(m_cmdAlloc->Reset(), "CmdAlloc Reset (Geometry)");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "CmdList Reset (Geometry)");

    m_cmdList->CopyBufferRegion(m_vertexBufferGPU.Get(), 0, vertexUpload.Get(), 0, vbByteSize);
    m_cmdList->CopyBufferRegion(m_indexBufferGPU.Get(), 0, indexUpload.Get(), 0, ibByteSize);

    D3D12_RESOURCE_BARRIER barriers[2]{};

    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = m_vertexBufferGPU.Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = m_indexBufferGPU.Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    m_cmdList->ResourceBarrier(2, barriers);

    ThrowIfFailed(m_cmdList->Close(), "CmdList Close (Geometry upload)");
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
    FlushCommandQueue();

    m_vbv.BufferLocation = m_vertexBufferGPU->GetGPUVirtualAddress();
    m_vbv.StrideInBytes = sizeof(Vertex);
    m_vbv.SizeInBytes = (UINT)vbByteSize;

    m_ibv.BufferLocation = m_indexBufferGPU->GetGPUVirtualAddress();
    m_ibv.SizeInBytes = (UINT)ibByteSize;
    m_ibv.Format = DXGI_FORMAT_R16_UINT;

    return true;
}

bool D3D12Context::BuildConstantBuffer()
{
    m_objectCBByteSize = AlignConstantBufferByteSize(sizeof(ObjectConstants));

    D3D12_HEAP_PROPERTIES uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC cbDesc = BufferDesc((UINT64)m_objectCBByteSize);

    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE,
        &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_objectCB)), "Create Object Constant Buffer");

    D3D12_RANGE readRange{ 0,0 };
    ThrowIfFailed(m_objectCB->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedObjectCB)),
                  "Map Object Constant Buffer");

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 1;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_cbvHeap)), "Create CBV heap");

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
    cbvDesc.BufferLocation = m_objectCB->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = m_objectCBByteSize;
    m_device->CreateConstantBufferView(&cbvDesc, m_cbvHeap->GetCPUDescriptorHandleForHeapStart());

    UpdateConstantBuffer();
    return true;
}

bool D3D12Context::BuildRootSignature()
{
    D3D12_DESCRIPTOR_RANGE cbvRange{};
    cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    cbvRange.NumDescriptors = 1;
    cbvRange.BaseShaderRegister = 0;
    cbvRange.RegisterSpace = 0;
    cbvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &cbvRange;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &param;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(hr))
    {
        if (errors) throw std::runtime_error((const char*)errors->GetBufferPointer());
        ThrowIfFailed(hr, "D3D12SerializeRootSignature failed");
    }

    ThrowIfFailed(m_device->CreateRootSignature(0,
        serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)), "CreateRootSignature");

    return true;
}

bool D3D12Context::BuildPSO()
{
    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_NONE;
    rast.FrontCounterClockwise = TRUE;
    rast.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rast.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rast.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rast.DepthClipEnable = TRUE;

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ds.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { m_vsBytecode->GetBufferPointer(), m_vsBytecode->GetBufferSize() };
    psoDesc.PS = { m_psBytecode->GetBufferPointer(), m_psBytecode->GetBufferSize() };
    psoDesc.BlendState = blend;
    psoDesc.RasterizerState = rast;
    psoDesc.DepthStencilState = ds;
    psoDesc.SampleMask = UINT_MAX;

    psoDesc.InputLayout = { m_inputLayout, 2 };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)), "CreateGraphicsPipelineState");
    return true;
}

void D3D12Context::UpdateConstantBuffer()
{
    if (!m_mappedObjectCB) return;

    ObjectConstants cb{};

    XMMATRIX world = XMLoadFloat4x4(&m_world);
    XMMATRIX view = XMLoadFloat4x4(&m_view);
    XMMATRIX proj = XMLoadFloat4x4(&m_proj);
    XMMATRIX wvp = world*view*proj;

    XMStoreFloat4x4(&cb.World, XMMatrixTranspose(world));
    XMStoreFloat4x4(&cb.WorldViewProj, XMMatrixTranspose(wvp));

    cb.EyePosW = m_eyePos;
    cb.LightDirW = m_lightDir;
    DirectX::XMVECTOR L = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&m_lightDir));
    DirectX::XMStoreFloat3(&cb.LightDirW, L);

    cb.Ambient = DirectX::XMFLOAT4(0.08f, 0.08f, 0.08f, 1.0f);
    cb.Diffuse = DirectX::XMFLOAT4(0.90f, 0.90f, 0.90f, 1.0f);
    cb.Specular = DirectX::XMFLOAT4(0.90f, 0.90f, 0.90f, 1.0f);
    cb.SpecPower = 64.0f;

    std::memcpy(m_mappedObjectCB, &cb, sizeof(cb));
}