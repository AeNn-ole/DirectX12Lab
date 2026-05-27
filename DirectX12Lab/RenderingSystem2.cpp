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

static void ThrowIfFailed2(HRESULT hr, const char* what)
{
    if (FAILED(hr)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (hr=0x%08X)", what, (unsigned)hr);
        throw std::runtime_error(buf);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// НОВОЕ: BuildOctree — вызывается один раз после ScatterInstances().
//
// Собирает Octree::Entry (idx + center + radius) для каждого экземпляра
// и передаёт в Octree::Build(). Дерево строится по центрам ограничивающих
// сфер (m_instances[i].Center, .Radius), которые вычислены в ScatterInstances.
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::BuildOctree()
{
    std::vector<Octree::Entry> entries;
    entries.reserve(m_instances.size());
    for (int i = 0; i < (int)m_instances.size(); ++i)
        entries.push_back({ i, m_instances[i].Center, m_instances[i].Radius });
    m_octree.Build(entries);
}

// ─────────────────────────────────────────────────────────────────────────────
// НОВОЕ: CollectVisibleInstances — выбирает какие экземпляры рисовать.
//
// Три режима зависят от флагов:
//
//   1. Culling выкл → все N_inst экземпляров (m_visibleIndices = 0,1,...,N-1)
//
//   2. Frustum culling вкл, Octree выкл →
//      Линейный обход всех экземпляров: IsSphereInFrustum() на каждый.
//      O(N) — для 2500 объектов ~быстро, но с ростом числа масштабируется плохо.
//
//   3. Frustum culling вкл, Octree вкл →
//      Octree::Query() — рекурсивный обход с ранним отсечением целых узлов.
//      O(log N × видимых) — принципиально быстрее для плотных сцен.
//
// После вызова m_visibleIndices содержит индексы экземпляров для этого кадра.
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::CollectVisibleInstances()
{
    m_visibleIndices.clear();

    // Отдаём приоритет octree, если он включён и построен
    if (m_octreeCullingEnabled && m_octree.IsBuilt())
    {
        m_octree.Query(m_frustumPlanes, m_visibleIndices);

        // Точная проверка сферами
        m_visibleIndices.erase(
            std::remove_if(m_visibleIndices.begin(), m_visibleIndices.end(),
                [this](int i) {
                    return !IsSphereInFrustum(m_frustumPlanes,
                        m_instances[i].Center, m_instances[i].Radius);
                }),
            m_visibleIndices.end());
    }
    else if (!m_frustumCullingEnabled)
    {
        // Без фрустум‑куллинга — рисуем всё
        m_visibleIndices.resize(m_instances.size());
        for (int i = 0; i < (int)m_instances.size(); ++i)
            m_visibleIndices[i] = i;
    }
    else
    {
        // Линейный фрустум‑куллинг
        for (int i = 0; i < (int)m_instances.size(); ++i)
        {
            if (IsSphereInFrustum(m_frustumPlanes,
                m_instances[i].Center, m_instances[i].Radius))
                m_visibleIndices.push_back(i);
        }
    }

    m_lastVisibleCount = (int)m_visibleIndices.size();
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::UpdateInstanceCB(int instIdx)
{
    if (!m_mappedInstanceCB) return;
    const auto& inst = m_instances[instIdx];

    PerInstanceCB cb{};
    XMMATRIX world = XMLoadFloat4x4(&inst.World);
    XMMATRIX view  = XMLoadFloat4x4(&m_view);
    XMMATRIX proj  = XMLoadFloat4x4(&m_proj);
    XMStoreFloat4x4(&cb.World,         XMMatrixTranspose(world));
    XMStoreFloat4x4(&cb.WorldViewProj, XMMatrixTranspose(world * view * proj));
    cb.EyePosW  = m_eyePos;
    XMVECTOR L  = XMVector3Normalize(XMLoadFloat3(&m_lightDir));
    XMStoreFloat3(&cb.LightDirW, L);

    std::memcpy(m_mappedInstanceCB + (size_t)instIdx * m_instanceCBByteSize,
                &cb, sizeof(cb));
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::UpdateMaterialCB(int mi)
{
    if (!m_mappedMaterialCB) return;

    static LARGE_INTEGER freq{}, t0{};
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
    }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    m_totalTime = (float)((now.QuadPart - t0.QuadPart) / (double)freq.QuadPart);

    const auto& mat = m_model.materials[mi];
    PerMaterialCB cb{};
    cb.Ambient   = { mat.Ka.x, mat.Ka.y, mat.Ka.z, 1.f };
    cb.Diffuse   = { mat.Kd.x, mat.Kd.y, mat.Kd.z, 1.f };
    cb.Specular  = { mat.Ks.x, mat.Ks.y, mat.Ks.z, 1.f };
    cb.SpecPower = mat.Ns > 0.f ? mat.Ns : 32.f;
    cb.gTime     = m_totalTime;
    cb.UVOffset  = { m_totalTime * 0.02f, 0.f };
    cb.UVTiling  = { 1.f, 1.f };

    std::memcpy(m_mappedMaterialCB + (size_t)mi * m_materialCBByteSize,
                &cb, sizeof(cb));
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::InitLights()
{
    m_numLights = 0;
    auto& L = m_lights;

    L[0].Type = 0; L[0].Direction = { 0.4f,-0.8f,0.2f };
    L[0].Color = { 1.f,0.95f,0.85f,0.7f };

    L[1].Type=1; L[1].Position={-350.f,120.f,0.f}; L[1].Range=300.f; L[1].Color={1.f,0.5f,0.1f,1.5f};
    L[2].Type=1; L[2].Position={-100.f,120.f,0.f}; L[2].Range=300.f; L[2].Color={1.f,0.5f,0.1f,1.5f};
    L[3].Type=1; L[3].Position={ 100.f,120.f,0.f}; L[3].Range=300.f; L[3].Color={0.2f,0.5f,1.f,1.5f};
    L[4].Type=1; L[4].Position={ 350.f,120.f,0.f}; L[4].Range=300.f; L[4].Color={0.2f,0.5f,1.f,1.5f};
    L[5].Type=1; L[5].Position={   0.f,400.f,0.f}; L[5].Range=300.f; L[5].Color={0.8f,0.8f,0.8f,1.f};

    L[6].Type=2; L[6].Position={0.f,450.f,0.f};   L[6].Direction={0.f,-1.f,0.f};
    L[6].Range=500.f; L[6].SpotAngle=cosf(XM_PI/6.f); L[6].Color={0.87f,0.9f,1.f,2.f};

    L[7].Type=2; L[7].Position={500.f,300.f,0.f}; L[7].Direction={-0.6f,-0.8f,0.f};
    L[7].Range=400.f; L[7].SpotAngle=cosf(XM_PI/8.f); L[7].Color={0.9f,0.7f,1.f,1.8f};

    m_numLights = 8;
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::UpdateLightingCB()
{
    if (!m_mappedLightingCB) return;

    XMMATRIX view     = XMLoadFloat4x4(&m_view);
    XMMATRIX proj     = XMLoadFloat4x4(&m_proj);
    XMMATRIX viewProj = view * proj;
    XMVECTOR det;
    XMMATRIX invVP = XMMatrixInverse(&det, viewProj);

    // ── НОВОЕ: обновляем плоскости фрустума ────────────────────────────
    // ViewProj НЕ транспонирован — ExtractFrustumPlanes ожидает CPU-конвенцию
    XMFLOAT4X4 vpf; XMStoreFloat4x4(&vpf, viewProj);
    m_frustumPlanes = ExtractFrustumPlanes(vpf);

    LightingConstants lc{};
    XMStoreFloat4x4(&lc.InvViewProj, XMMatrixTranspose(invVP));
    lc.EyePosW    = m_eyePos;
    lc.ScreenSize = { (float)m_width, (float)m_height };
    lc.NumLights  = m_numLights;
    for (int i = 0; i < m_numLights; ++i) lc.Lights[i] = m_lights[i];

    std::memcpy(m_mappedLightingCB, &lc, sizeof(lc));
}

// ─────────────────────────────────────────────────────────────────────────────
// ИЗМЕНЕНО: GeometryPass — использует m_visibleIndices вместо всех экземпляров.
//
// CollectVisibleInstances() уже заполнила m_visibleIndices нужными индексами.
// Здесь просто итерируемся по ним — GeometryPass не знает о режиме culling,
// он всегда получает готовый список видимых объектов.
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::GeometryPass()
{
    m_gbuffer.TransitionToRenderTarget(m_cmdList.Get());

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {
        m_gbuffer.GetRTV(GBuffer::Albedo),
        m_gbuffer.GetRTV(GBuffer::Normal),
        m_gbuffer.GetRTV(GBuffer::Specular),
    };
    auto dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_cmdList->OMSetRenderTargets(3, rtvs, FALSE, &dsv);
    m_gbuffer.Clear(m_cmdList.Get());
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);

    m_cmdList->SetPipelineState(m_geometryPSO.Get());
    m_cmdList->SetGraphicsRootSignature(m_geometryRootSig.Get());
    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissorRect);
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmdList->IASetVertexBuffers(0, 1, &m_vbv);
    m_cmdList->IASetIndexBuffer(&m_ibv);

    ID3D12DescriptorHeap* heaps[] = { m_cbvSrvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase =
        m_cbvSrvHeap->GetGPUDescriptorHandleForHeapStart();
    const uint32_t N = m_numMaterials;

    // Материальные CB обновляем один раз за кадр
    for (uint32_t mi = 0; mi < N; ++mi)
        UpdateMaterialCB((int)mi);

    // Внешний цикл — только ВИДИМЫЕ экземпляры
    for (int i : m_visibleIndices)
    {
        UpdateInstanceCB(i);

        D3D12_GPU_VIRTUAL_ADDRESS instAddr =
            m_instanceCB->GetGPUVirtualAddress() +
            (D3D12_GPU_VIRTUAL_ADDRESS)i * m_instanceCBByteSize;
        m_cmdList->SetGraphicsRootConstantBufferView(0, instAddr);

        for (const auto& sub : m_model.subMeshes)
        {
            int mi = sub.materialIndex;

            D3D12_GPU_DESCRIPTOR_HANDLE cbvH = gpuBase;
            cbvH.ptr += (SIZE_T)((uint32_t)mi * m_cbvSrvDescriptorSize);
            m_cmdList->SetGraphicsRootDescriptorTable(1, cbvH);

            D3D12_GPU_DESCRIPTOR_HANDLE srvH = gpuBase;
            srvH.ptr += (SIZE_T)((N + (uint32_t)mi) * m_cbvSrvDescriptorSize);
            m_cmdList->SetGraphicsRootDescriptorTable(2, srvH);

            m_cmdList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexStart, 0, 0);
        }
    }

    m_gbuffer.TransitionToShaderResource(m_cmdList.Get());
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::LightingPass()
{
    D3D12_RESOURCE_BARRIER depthToSRV{};
    depthToSRV.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    depthToSRV.Transition = {
        m_depthStencilBuffer.Get(),
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    };
    m_cmdList->ResourceBarrier(1, &depthToSRV);

    auto rtv = CurrentBackBufferRTV();
    m_cmdList->OMSetRenderTargets(1, &rtv, TRUE, nullptr);
    const float clearColor[4] = { 0.f, 0.f, 0.f, 1.f };
    m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

    m_cmdList->SetPipelineState(m_lightingPSO.Get());
    m_cmdList->SetGraphicsRootSignature(m_lightingRootSig.Get());
    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissorRect);
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12DescriptorHeap* heaps[] = { m_cbvSrvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);
    m_cmdList->SetGraphicsRootDescriptorTable(0, m_gbuffer.GetFirstSRV());
    m_cmdList->SetGraphicsRootConstantBufferView(1, m_lightingCB->GetGPUVirtualAddress());
    m_cmdList->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER depthBack = depthToSRV;
    depthBack.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    depthBack.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    m_cmdList->ResourceBarrier(1, &depthBack);
}

// ─────────────────────────────────────────────────────────────────────────────
// ИЗМЕНЕНО: Draw — перед GeometryPass вызывает CollectVisibleInstances().
// UpdateLightingCB теперь также обновляет m_frustumPlanes.
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::Draw()
{
    if (!m_initialized) return;

    // 1. Обновить матрицы + извлечь плоскости фрустума
    UpdateLightingCB();

    // 2. Отобрать видимые экземпляры (режим зависит от флагов)
    CollectVisibleInstances();

    ThrowIfFailed2(m_cmdAlloc->Reset(), "Alloc Draw");
    ThrowIfFailed2(m_cmdList->Reset(m_cmdAlloc.Get(), m_geometryPSO.Get()), "List Draw");

    D3D12_RESOURCE_BARRIER toRT{};
    toRT.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT.Transition = {
        CurrentBackBuffer(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET
    };
    m_cmdList->ResourceBarrier(1, &toRT);

    GeometryPass();
    LightingPass();

    D3D12_RESOURCE_BARRIER toPresent = toRT;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    m_cmdList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed2(m_cmdList->Close(), "CmdList Close");
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
    XMVECTOR fwd = XMVector3Normalize(XMVectorSet(sy*cp, sp, cy*cp, 0));
    XMVECTOR eye = XMVectorSet(eyePos.x, eyePos.y, eyePos.z, 1);
    XMStoreFloat4x4(&m_view, XMMatrixLookToLH(eye, fwd, XMVectorSet(0,1,0,0)));
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

    CreateRtvForBackBuffers();
    CreateDepthStencil();
    m_gbuffer.Create(m_device.Get(), width, height,
        m_rtvHeap.Get(), kSwapChainBufferCount, m_rtvDescriptorSize,
        m_cbvSrvHeap.Get(), 2 * m_numMaterials, m_cbvSrvDescriptorSize);
    RecreateDepthSRV();

    m_viewport    = { 0, 0, (float)width, (float)height, 0, 1 };
    m_scissorRect = { 0, 0, (LONG)width,  (LONG)height };
    float asp = (float)width / height;
    XMStoreFloat4x4(&m_proj,
        XMMatrixPerspectiveFovLH(0.25f*XM_PI, asp, 0.1f, 5000.f));
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
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (size_t)m_currBackBuffer * m_rtvDescriptorSize;
    return h;
}

ID3D12Resource* RenderingSystem::CurrentBackBuffer() const
{
    return m_swapChainBuffers[m_currBackBuffer].Get();
}

// ─────────────────────────────────────────────────────────────────────────────
// LoadAndUploadTexture — без изменений
// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::LoadAndUploadTexture(const wchar_t* path,
    ComPtr<ID3D12Resource>& outTex)
{
    constexpr DXGI_FORMAT kFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t texW = 0, texH = 0;
    std::vector<uint8_t> pixels;

    char pathA[512]{};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, pathA, sizeof(pathA), nullptr, nullptr);

    int w, h, channels;
    uint8_t* data = stbi_load(pathA, &w, &h, &channels, 4);
    if (data) {
        texW = (uint32_t)w; texH = (uint32_t)h;
        pixels.assign(data, data + texW*texH*4);
        stbi_image_free(data);
    } else {
        texW = texH = 64;
        pixels.resize(texW*texH*4);
        for (uint32_t y = 0; y < texH; ++y)
            for (uint32_t x = 0; x < texW; ++x) {
                bool w2 = ((x/8)+(y/8))%2==0;
                uint32_t idx=(y*texW+x)*4;
                pixels[idx+0]=w2?255u:50u; pixels[idx+1]=w2?255u:200u;
                pixels[idx+2]=w2?255u:50u; pixels[idx+3]=255u;
            }
    }

    D3D12_RESOURCE_DESC td{};
    td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width=texW; td.Height=texH;
    td.DepthOrArraySize=1; td.MipLevels=1; td.Format=kFmt;
    td.SampleDesc.Count=1; td.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;

    auto defH = D3D12_HEAP_PROPERTIES{D3D12_HEAP_TYPE_DEFAULT,{},{},1,1};
    ThrowIfFailed2(m_device->CreateCommittedResource(&defH, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outTex)), "Create Tex");

    UINT64 uploadSize=0; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT numRows=0; UINT64 rowSz=0;
    m_device->GetCopyableFootprints(&td,0,1,0,&fp,&numRows,&rowSz,&uploadSize);

    ComPtr<ID3D12Resource> upBuf;
    auto upH=D3D12_HEAP_PROPERTIES{D3D12_HEAP_TYPE_UPLOAD,{},{},1,1};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width=uploadSize;
    bd.Height=1; bd.DepthOrArraySize=1; bd.MipLevels=1;
    bd.SampleDesc.Count=1; bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ThrowIfFailed2(m_device->CreateCommittedResource(&upH, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upBuf)), "Tex Upload");

    {
        uint8_t* p=nullptr; D3D12_RANGE rr{0,0};
        ThrowIfFailed2(upBuf->Map(0,&rr,reinterpret_cast<void**>(&p)),"Map Tex");
        for (uint32_t row=0;row<numRows;++row)
            std::memcpy(p+fp.Offset+(UINT64)row*fp.Footprint.RowPitch,
                        pixels.data()+(size_t)row*texW*4,(size_t)texW*4);
        upBuf->Unmap(0,nullptr);
    }

    ThrowIfFailed2(m_cmdAlloc->Reset(),"Alloc TexUp");
    ThrowIfFailed2(m_cmdList->Reset(m_cmdAlloc.Get(),nullptr),"List TexUp");

    D3D12_TEXTURE_COPY_LOCATION dst{},src{};
    dst.pResource=outTex.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource=upBuf.Get();  src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint=fp;
    m_cmdList->CopyTextureRegion(&dst,0,0,0,&src,nullptr);

    D3D12_RESOURCE_BARRIER bar{};
    bar.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition={outTex.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    m_cmdList->ResourceBarrier(1,&bar);

    ThrowIfFailed2(m_cmdList->Close(),"Close TexUp");
    ID3D12CommandList* ls[]={m_cmdList.Get()};
    m_cmdQueue->ExecuteCommandLists(1,ls);
    FlushCommandQueue();
    return true;
}
