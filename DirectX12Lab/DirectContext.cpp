#include "DirectContext.h"
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <wincodec.h>       // WIC: загрузка PNG/JPG/BMP
#include <DirectXMath.h>
using namespace DirectX;
using Microsoft::WRL::ComPtr;

#pragma comment(lib, "windowscodecs.lib")

// ─────────────────────────────────────────────────────────────────────────────
static void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (hr=0x%08X)", what, (unsigned)hr);
        OutputDebugStringA("DirectX Error: ");
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
        throw std::runtime_error(buf);
    }
}

static uint32_t AlignCB(uint32_t size) { return (size + 255u) & ~255u; }

static D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;  p.CreationNodeMask = 1;  p.VisibleNodeMask = 1;
    p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    return p;
}

static D3D12_RESOURCE_DESC BufDesc(UINT64 bytes)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;  d.Height = 1;  d.DepthOrArraySize = 1;
    d.MipLevels = 1;  d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

// ─────────────────────────────────────────────────────────────────────────────
bool D3D12Context::Initialize(HWND hwnd, uint32_t width, uint32_t height)
{
    m_initialized = false;
    m_hwnd = hwnd;  m_width = width;  m_height = height;

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

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) throw std::runtime_error("CreateEvent failed");

    CreateSwapChain();

    m_rtvDescriptorSize    = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_dsvDescriptorSize    = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_cbvSrvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CreateDescriptorHeaps();
    CreateRtvForBackBuffers();
    CreateDepthStencil();

    m_viewport    = { 0, 0, (float)width,  (float)height, 0, 1 };
    m_scissorRect = { 0, 0, (LONG)width,   (LONG)height };

    XMStoreFloat4x4(&m_world, XMMatrixIdentity());
    XMVECTOR eye = XMVectorSet(m_eyePos.x, m_eyePos.y, m_eyePos.z, 1);
    XMStoreFloat4x4(&m_view, XMMatrixLookAtLH(eye, XMVectorZero(), XMVectorSet(0,1,0,0)));
    float asp = height > 0 ? (float)width / height : 1.f;
    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(0.25f * XM_PI, asp, 0.1f, 1000.f));

    // ── Загрузка модели ───────────────────────────────────────────────────
    // Кладём model.obj рядом с .exe.  Если не найден — исключение с подсказкой.
    if (!LoadObj(L"model.obj", m_model))
        throw std::runtime_error(
            "Не удалось открыть model.obj.\n"
            "Положите OBJ-файл рядом с .exe и переименуйте его в model.obj");

    if (m_model.materials.empty())
        m_model.materials.push_back({ "__default__" });

    m_numMaterials = (uint32_t)m_model.materials.size();

    BuildShaders();
    BuildGeometry();
    BuildConstantBuffer();
    BuildDescriptorViews();   // загружает текстуры и создаёт CBV+SRV
    BuildRootSignature();
    BuildPSO();

    m_initialized = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
void D3D12Context::Shutdown()
{
    if (m_cmdQueue) FlushCommandQueue();
    if (m_objectCB && m_mappedObjectCB) { m_objectCB->Unmap(0, nullptr); m_mappedObjectCB = nullptr; }
    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
}

// ─────────────────────────────────────────────────────────────────────────────
bool D3D12Context::CreateDevice()
{
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
    if (FAILED(hr))
    {
        ComPtr<IDXGIAdapter> warp;
        ThrowIfFailed(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "EnumWarpAdapter");
        ThrowIfFailed(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)), "D3D12CreateDevice WARP");
    }
    return true;
}

bool D3D12Context::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_cmdQueue)), "CreateCommandQueue");
    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAlloc)), "CreateCommandAllocator");
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&m_cmdList)), "CreateCommandList");
    ThrowIfFailed(m_cmdList->Close(), "CmdList Close (init)");
    return true;
}

bool D3D12Context::CreateSwapChain()
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc = { m_width, m_height, {60,1}, DXGI_FORMAT_R8G8B8A8_UNORM };
    sd.SampleDesc = { 1, 0 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kSwapChainBufferCount;
    sd.OutputWindow = m_hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    m_swapChain.Reset();
    ThrowIfFailed(m_factory->CreateSwapChain(m_cmdQueue.Get(), &sd, m_swapChain.GetAddressOf()), "CreateSwapChain");
    m_currBackBuffer = 0;
    return true;
}

bool D3D12Context::CreateDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rd{}; rd.NumDescriptors = kSwapChainBufferCount; rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&m_rtvHeap)), "RTV Heap");

    D3D12_DESCRIPTOR_HEAP_DESC dd{}; dd.NumDescriptors = 1; dd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&m_dsvHeap)), "DSV Heap");
    return true;
}

bool D3D12Context::CreateRtvForBackBuffers()
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

bool D3D12Context::CreateDepthStencil()
{
    m_depthStencilBuffer.Reset();
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = m_width; td.Height = m_height; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv{}; cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; cv.DepthStencil.Depth = 1.f;
    auto hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COMMON, &cv, IID_PPV_ARGS(&m_depthStencilBuffer)), "Create DS");

    ThrowIfFailed(m_cmdAlloc->Reset(), "Alloc Reset DS");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "List Reset DS");

    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { m_depthStencilBuffer.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                     D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE };
    m_cmdList->ResourceBarrier(1, &b);
    ThrowIfFailed(m_cmdList->Close(), "CmdList Close DS");
    ID3D12CommandList* ls[] = { m_cmdList.Get() }; m_cmdQueue->ExecuteCommandLists(1, ls);
    FlushCommandQueue();

    m_device->CreateDepthStencilView(m_depthStencilBuffer.Get(), nullptr,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool D3D12Context::BuildShaders()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompileFromFile(L"Shaders.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain", "vs_5_0", flags, 0, &m_vsBytecode, &err);
    if (FAILED(hr)) { if (err) throw std::runtime_error((char*)err->GetBufferPointer()); ThrowIfFailed(hr, "VS compile"); }
    err.Reset();
    hr = D3DCompileFromFile(L"Shaders.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain", "ps_5_0", flags, 0, &m_psBytecode, &err);
    if (FAILED(hr)) { if (err) throw std::runtime_error((char*)err->GetBufferPointer()); ThrowIfFailed(hr, "PS compile"); }

    m_inputLayout[0] = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    m_inputLayout[1] = { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    m_inputLayout[2] = { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool D3D12Context::BuildGeometry()
{
    const UINT64 vbBytes = (UINT64)m_model.vertices.size() * sizeof(ObjVertex);
    const UINT64 ibBytes = (UINT64)m_model.indices.size()  * sizeof(uint32_t);

    auto defHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto upHeap  = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto vbDesc  = BufDesc(vbBytes), ibDesc = BufDesc(ibBytes);

    ThrowIfFailed(m_device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_vertexBufferGPU)), "VB GPU");
    ThrowIfFailed(m_device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_indexBufferGPU)), "IB GPU");

    ComPtr<ID3D12Resource> vbUp, ibUp;
    ThrowIfFailed(m_device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vbUp)), "VB Upload");
    ThrowIfFailed(m_device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ibUp)), "IB Upload");

    auto upload = [](ID3D12Resource* r, const void* data, size_t sz)
    {
        void* p; D3D12_RANGE rr{0,0};
        r->Map(0, &rr, &p); std::memcpy(p, data, sz); r->Unmap(0, nullptr);
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
    ID3D12CommandList* ls[] = { m_cmdList.Get() }; m_cmdQueue->ExecuteCommandLists(1, ls);
    FlushCommandQueue();

    m_vbv = { m_vertexBufferGPU->GetGPUVirtualAddress(), (UINT)vbBytes, sizeof(ObjVertex) };
    m_ibv = { m_indexBufferGPU->GetGPUVirtualAddress(),  (UINT)ibBytes, DXGI_FORMAT_R32_UINT };  // 32-bit!
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool D3D12Context::BuildConstantBuffer()
{
    // Один выровненный слот на каждый материал
    m_objectCBByteSize = AlignCB(sizeof(ObjectConstants));
    const UINT64 totalBytes = (UINT64)m_numMaterials * m_objectCBByteSize;

    auto up = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto bd = BufDesc(totalBytes);
    ThrowIfFailed(m_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_objectCB)), "Create CB");

    D3D12_RANGE rr{0,0};
    ThrowIfFailed(m_objectCB->Map(0, &rr, reinterpret_cast<void**>(&m_mappedObjectCB)), "Map CB");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// LoadAndUploadTexture: WIC → GPU ресурс.
// Если path пустой или файл не найден — генерирует шахматный паттерн 64×64.
// ─────────────────────────────────────────────────────────────────────────────
bool D3D12Context::LoadAndUploadTexture(const wchar_t* path,
                                        ComPtr<ID3D12Resource>& outTex)
{
    constexpr DXGI_FORMAT kFmt = DXGI_FORMAT_R8G8B8A8_UNORM;

    uint32_t texW = 0, texH = 0;
    std::vector<uint8_t> pixels;
    bool loaded = false;

    // ── WIC ───────────────────────────────────────────────────────────────
    if (path && path[0] != L'\0')
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* wic = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        {
            IWICBitmapDecoder* dec = nullptr;
            if (SUCCEEDED(wic->CreateDecoderFromFilename(path, nullptr,
                GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)))
            {
                IWICBitmapFrameDecode* frame = nullptr;
                if (SUCCEEDED(dec->GetFrame(0, &frame)))
                {
                    UINT w = 0, h = 0; frame->GetSize(&w, &h);
                    IWICFormatConverter* conv = nullptr;
                    if (SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
                        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
                    {
                        texW = w; texH = h;
                        const UINT stride = texW * 4, bufSz = stride * texH;
                        pixels.resize(bufSz);
                        if (SUCCEEDED(conv->CopyPixels(nullptr, stride, bufSz, pixels.data())))
                            loaded = true;
                    }
                    if (conv) conv->Release();
                    frame->Release();
                }
                dec->Release();
            }
            wic->Release();
        }
    }

    // ── Шахматный fallback ────────────────────────────────────────────────
    if (!loaded)
    {
        OutputDebugStringA("Texture not found, using checkerboard fallback\n");
        texW = texH = 64;
        pixels.resize(texW * texH * 4);
        for (uint32_t y = 0; y < texH; ++y)
            for (uint32_t x = 0; x < texW; ++x)
            {
                bool w = ((x / 8) + (y / 8)) % 2 == 0;
                uint32_t i = (y * texW + x) * 4;
                pixels[i+0] = w ? 255u :  50u;
                pixels[i+1] = w ? 255u : 200u;
                pixels[i+2] = w ? 255u :  50u;
                pixels[i+3] = 255u;
            }
    }

    // ── Создаём текстурный ресурс ─────────────────────────────────────────
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = texW; td.Height = texH; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = kFmt; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    auto defH = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(&defH, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outTex)), "Create Texture");

    // ── Upload buffer ─────────────────────────────────────────────────────
    UINT64 uploadSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT numRows = 0; UINT64 rowSz = 0;
    m_device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &numRows, &rowSz, &uploadSize);

    ComPtr<ID3D12Resource> upBuf;
    auto upH = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto bd  = BufDesc(uploadSize);
    ThrowIfFailed(m_device->CreateCommittedResource(&upH, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upBuf)), "Texture Upload Buf");

    {
        uint8_t* p = nullptr; D3D12_RANGE rr{0,0};
        ThrowIfFailed(upBuf->Map(0, &rr, reinterpret_cast<void**>(&p)), "Map Tex Upload");
        for (uint32_t row = 0; row < numRows; ++row)
            std::memcpy(p + fp.Offset + (UINT64)row * fp.Footprint.RowPitch,
                        pixels.data() + (size_t)row * texW * 4, (size_t)texW * 4);
        upBuf->Unmap(0, nullptr);
    }

    // ── Команды загрузки ──────────────────────────────────────────────────
    ThrowIfFailed(m_cmdAlloc->Reset(), "Alloc TexUp");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "List TexUp");

    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource        = outTex.Get();
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    src.pResource        = upBuf.Get();
    src.Type             = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint  = fp;
    m_cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER bar{}; bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition = { outTex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
    m_cmdList->ResourceBarrier(1, &bar);

    ThrowIfFailed(m_cmdList->Close(), "Close TexUp");
    ID3D12CommandList* ls[] = { m_cmdList.Get() }; m_cmdQueue->ExecuteCommandLists(1, ls);
    FlushCommandQueue();
    // upBuf освобождается при выходе из функции — GPU уже закончил

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildDescriptorViews: создаёт кучу (2N слотов), заполняет CBV и SRV.
//   Слоты [0..N-1]   → CBV (по одному на каждый материал)
//   Слоты [N..2N-1]  → SRV текстуры материала (порядок тот же)
// ─────────────────────────────────────────────────────────────────────────────
bool D3D12Context::BuildDescriptorViews()
{
    const uint32_t N = m_numMaterials;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 2 * N;
    hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_cbvSrvHeap)), "CBV/SRV Heap");

    D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart();

    // ── CBV: слоты [0..N-1] ───────────────────────────────────────────────
    for (uint32_t i = 0; i < N; ++i)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvd{};
        cbvd.BufferLocation = m_objectCB->GetGPUVirtualAddress() + (UINT64)i * m_objectCBByteSize;
        cbvd.SizeInBytes    = m_objectCBByteSize;

        D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
        h.ptr += (SIZE_T)(i * m_cbvSrvDescriptorSize);
        m_device->CreateConstantBufferView(&cbvd, h);
    }

    // ── SRV: слоты [N..2N-1], загружаем текстуры ─────────────────────────
    m_gpuMaterials.resize(N);
    for (uint32_t i = 0; i < N; ++i)
    {
        // Приоритет: map_Kd из MTL → texture.png рядом с .exe → шахматный fallback
        const wchar_t* texPath = m_model.materials[i].map_Kd.empty()
            ? L"texture.png"
            : m_model.materials[i].map_Kd.c_str();

        LoadAndUploadTexture(texPath, m_gpuMaterials[i].texture);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format                    = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Texture2D.MipLevels       = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
        h.ptr += (SIZE_T)((N + i) * m_cbvSrvDescriptorSize);
        m_device->CreateShaderResourceView(m_gpuMaterials[i].texture.Get(), &srvd, h);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool D3D12Context::BuildRootSignature()
{
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    // param[0] → 1 CBV (b0)
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    // param[1] → 1 SRV (t0)
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable                     = { 1, &ranges[0] };
    params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable                     = { 1, &ranges[1] };
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Статический сэмплер (s0): линейная фильтрация + WRAP
    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.MaxAnisotropy    = 1;
    samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    samp.MaxLOD           = D3D12_FLOAT32_MAX;
    samp.ShaderRegister   = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters     = 2;
    rsd.pParameters       = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers   = &samp;
    rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> ser, err;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &err);
    if (FAILED(hr)) { if (err) throw std::runtime_error((char*)err->GetBufferPointer()); ThrowIfFailed(hr, "SerializeRS"); }
    ThrowIfFailed(m_device->CreateRootSignature(0, ser->GetBufferPointer(), ser->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)), "CreateRS");
    return true;
}

bool D3D12Context::BuildPSO()
{
    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode = D3D12_FILL_MODE_SOLID; rast.CullMode = D3D12_CULL_MODE_BACK;
    rast.FrontCounterClockwise = FALSE;    // OBJ — правосторонняя система
    rast.DepthClipEnable = TRUE;
    rast.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rast.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rast.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS; ds.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature    = m_rootSignature.Get();
    pd.VS                = { m_vsBytecode->GetBufferPointer(), m_vsBytecode->GetBufferSize() };
    pd.PS                = { m_psBytecode->GetBufferPointer(), m_psBytecode->GetBufferSize() };
    pd.BlendState        = blend;
    pd.RasterizerState   = rast;
    pd.DepthStencilState = ds;
    pd.SampleMask        = UINT_MAX;
    pd.InputLayout       = { m_inputLayout, 3 };
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets  = 1;
    pd.RTVFormats[0]     = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat         = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.SampleDesc.Count  = 1;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pso)), "CreatePSO");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
void D3D12Context::UpdateConstantBufferForMaterial(int mi)
{
    if (!m_mappedObjectCB) return;

    // Время (для UV-анимации)
    static LARGE_INTEGER freq{}, t0{};
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0); }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    m_totalTime = (float)((now.QuadPart - t0.QuadPart) / (double)freq.QuadPart);

    const auto& mat = m_model.materials[mi];

    ObjectConstants cb{};
    XMMATRIX world = XMLoadFloat4x4(&m_world);
    XMMATRIX view  = XMLoadFloat4x4(&m_view);
    XMMATRIX proj  = XMLoadFloat4x4(&m_proj);
    XMStoreFloat4x4(&cb.World,         XMMatrixTranspose(world));
    XMStoreFloat4x4(&cb.WorldViewProj, XMMatrixTranspose(world * view * proj));

    cb.EyePosW  = m_eyePos;
    XMVECTOR L  = XMVector3Normalize(XMLoadFloat3(&m_lightDir));
    XMStoreFloat3(&cb.LightDirW, L);

    // Материальные свойства из MTL
    cb.Ambient  = { mat.Ka.x, mat.Ka.y, mat.Ka.z, 1.f };
    cb.Diffuse  = { mat.Kd.x, mat.Kd.y, mat.Kd.z, 1.f };
    cb.Specular = { mat.Ks.x, mat.Ks.y, mat.Ks.z, 1.f };
    cb.SpecPower = mat.Ns > 0.f ? mat.Ns : 32.f;

    // UV: тайлинг 1×1, медленный скролл по U (демо анимации)
    cb.UVOffset = { m_totalTime * 0.02f, 0.f };
    cb.UVTiling = { 1.f, 1.f };

    // Пишем в слот mi константного буфера
    std::memcpy(m_mappedObjectCB + (size_t)mi * m_objectCBByteSize, &cb, sizeof(cb));
}

// ─────────────────────────────────────────────────────────────────────────────
void D3D12Context::Draw()
{
    if (!m_initialized) return;

    // Обновляем CB для всех материалов (CPU-сторона, до ExecuteCommandLists)
    for (int i = 0; i < (int)m_numMaterials; ++i)
        UpdateConstantBufferForMaterial(i);

    ThrowIfFailed(m_cmdAlloc->Reset(), "Alloc Draw");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), m_pso.Get()), "List Draw");

    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissorRect);
    m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());

    D3D12_RESOURCE_BARRIER toRT{};
    toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT.Transition = { CurrentBackBuffer(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
    m_cmdList->ResourceBarrier(1, &toRT);

    auto rtv = CurrentBackBufferRTV();
    auto dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_cmdList->OMSetRenderTargets(1, &rtv, TRUE, &dsv);

    const float clearColor[4] = { 0.10f, 0.10f, 0.35f, 1.f };
    m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);

    // Привязываем кучу один раз
    ID3D12DescriptorHeap* heaps[] = { m_cbvSrvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);

    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmdList->IASetVertexBuffers(0, 1, &m_vbv);
    m_cmdList->IASetIndexBuffer(&m_ibv);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = m_cbvSrvHeap->GetGPUDescriptorHandleForHeapStart();
    const uint32_t N = m_numMaterials;

    // Один draw-call на каждый сабмеш (группа треугольников с одним материалом)
    for (const auto& sub : m_model.subMeshes)
    {
        int mi = sub.materialIndex;

        // CBV: слот mi  (первые N слотов кучи)
        D3D12_GPU_DESCRIPTOR_HANDLE cbvH = gpuBase;
        cbvH.ptr += (SIZE_T)((uint32_t)mi * m_cbvSrvDescriptorSize);
        m_cmdList->SetGraphicsRootDescriptorTable(0, cbvH);

        // SRV: слот N+mi  (вторые N слотов кучи)
        D3D12_GPU_DESCRIPTOR_HANDLE srvH = gpuBase;
        srvH.ptr += (SIZE_T)((N + (uint32_t)mi) * m_cbvSrvDescriptorSize);
        m_cmdList->SetGraphicsRootDescriptorTable(1, srvH);

        m_cmdList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexStart, 0, 0);
    }

    D3D12_RESOURCE_BARRIER toPresent = toRT;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    m_cmdList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(m_cmdList->Close(), "CmdList Close Draw");
    ID3D12CommandList* ls[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, ls);

    ThrowIfFailed(m_swapChain->Present(1, 0), "Present");
    m_currBackBuffer = (m_currBackBuffer + 1) % kSwapChainBufferCount;
    FlushCommandQueue();
}

// ─────────────────────────────────────────────────────────────────────────────
void D3D12Context::SetCamera(const DirectX::XMFLOAT3& eyePos, float yaw, float pitch)
{
    m_eyePos = eyePos;
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    XMVECTOR fwd = XMVector3Normalize(XMVectorSet(sy*cp, sp, cy*cp, 0));
    XMVECTOR eye = XMVectorSet(eyePos.x, eyePos.y, eyePos.z, 1);
    XMStoreFloat4x4(&m_view, XMMatrixLookToLH(eye, fwd, XMVectorSet(0,1,0,0)));
}

void D3D12Context::FlushCommandQueue()
{
    const uint64_t v = ++m_fenceValue;
    ThrowIfFailed(m_cmdQueue->Signal(m_fence.Get(), v), "Signal");
    if (m_fence->GetCompletedValue() < v)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(v, m_fenceEvent), "SetEvent");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void D3D12Context::OnResize(uint32_t width, uint32_t height)
{
    if (!m_initialized || !width || !height) return;
    m_width = width; m_height = height;
    FlushCommandQueue();
    for (auto& b : m_swapChainBuffers) b.Reset();
    m_depthStencilBuffer.Reset();
    ThrowIfFailed(m_swapChain->ResizeBuffers(kSwapChainBufferCount, width, height,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0), "ResizeBuffers");
    m_currBackBuffer = 0;
    CreateRtvForBackBuffers();
    CreateDepthStencil();
    m_viewport    = { 0, 0, (float)width, (float)height, 0, 1 };
    m_scissorRect = { 0, 0, (LONG)width, (LONG)height };
    float asp = (float)width / height;
    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(0.25f*XM_PI, asp, 0.1f, 1000.f));
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
