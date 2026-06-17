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
void RenderingSystem::BuildOctree()
{
    std::vector<Octree::Entry> entries;
    entries.reserve(m_instances.size());
    for (int i = 0; i < (int)m_instances.size(); ++i)
        entries.push_back({ i, m_instances[i].Center, m_instances[i].Radius });
    m_octree.Build(entries);
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::CollectVisibleInstances()
{
    m_visibleIndices.clear();

    if (m_octreeCullingEnabled && m_octree.IsBuilt())
    {
        m_octree.Query(m_frustumPlanes, m_visibleIndices);
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
        m_visibleIndices.resize(m_instances.size());
        for (int i = 0; i < (int)m_instances.size(); ++i)
            m_visibleIndices[i] = i;
    }
    else
    {
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
    cb.UVOffset  = { 0.f, 0.f };  // убрали анимацию UV — она мешает displacement
    cb.UVTiling  = { 1.f, 1.f };

    // Параметры тесселяции из CPU-состояния
    cb.TessFactorNear    = m_tessellationEnabled ? m_tessFactorNear : 1.f;
    cb.TessFactorFar     = m_tessellationEnabled ? m_tessFactorFar  : 1.f;
    cb.TessDistNear      = m_tessDistNear;
    cb.TessDistFar       = m_tessDistFar;
    cb.DisplacementScale = m_tessellationEnabled ? m_displacementScale : 0.f;
    cb.EnableNormalMap   = m_normalMappingEnabled ? 1 : 0;

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

    XMFLOAT4X4 vpf; XMStoreFloat4x4(&vpf, viewProj);
    m_frustumPlanes = ExtractFrustumPlanes(vpf);

    LightingConstants lc{};
    XMStoreFloat4x4(&lc.InvViewProj, XMMatrixTranspose(invVP));
    lc.EyePosW    = m_eyePos;
    lc.ScreenSize = { (float)m_width, (float)m_height };
    lc.NumLights  = m_numLights;

    // Forward вектор камеры = третья строка view матрицы (row 2)
    // View матрица хранится transposed в m_view после XMStoreFloat4x4
    // Row 2 view = (m[0][2], m[1][2], m[2][2]) в column-major
    {
        XMMATRIX v = XMLoadFloat4x4(&m_view);
        XMVECTOR fwd = XMVector3Normalize(XMVectorSet(
            XMVectorGetZ(v.r[0]),
            XMVectorGetZ(v.r[1]),
            XMVectorGetZ(v.r[2]), 0.f));
        XMStoreFloat3(&lc.CameraForward, fwd);
    }

    for (int i = 0; i < m_numLights; ++i) lc.Lights[i] = m_lights[i];

    // CSM
    for (int i = 0; i < kCsmCascades; ++i)
        lc.LightViewProj[i] = m_cascades[i].LightViewProj;
    lc.CascadeFarPlanes = {
        m_cascades[0].FarPlane, m_cascades[1].FarPlane,
        m_cascades[2].FarPlane, m_cascades[3].FarPlane
    };
    lc.NumCascades    = kCsmCascades;
    lc.DebugCascades  = m_debugCascades ? 1 : 0;
    lc.ShadowMapSize  = (float)kShadowMapSize;
    lc.ShadowBias     = 0.005f;

    std::memcpy(m_mappedLightingCB, &lc, sizeof(lc));
}

// ─────────────────────────────────────────────────────────────────────────────
// GeometryPass
//
// Ключевые изменения по сравнению с предыдущей версией:
//   1. Топология: D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
//      (каждые 3 индекса = один патч для HS)
//   2. Привязываем normal map (param[3]) и displacement map (param[4])
//      для каждого subMesh
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::GeometryPass()
{
    // ── Wireframe mode: рисуем прямо в back buffer, минуя G-Buffer ───────
    if (m_wireframe)
    {
        auto rtv = CurrentBackBufferRTV();
        auto dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        m_cmdList->OMSetRenderTargets(1, &rtv, TRUE, &dsv);
        const float black[4] = {0.05f, 0.05f, 0.05f, 1.f};
        m_cmdList->ClearRenderTargetView(rtv, black, 0, nullptr);
        m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);
        m_cmdList->SetPipelineState(m_geometryWirePSO.Get());
    }
    else
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
    }
    m_cmdList->SetGraphicsRootSignature(m_geometryRootSig.Get());
    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissorRect);

    // Тесселяция: 3-control-point patches (треугольники)
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

    m_cmdList->IASetVertexBuffers(0, 1, &m_vbv);
    m_cmdList->IASetIndexBuffer(&m_ibv);

    ID3D12DescriptorHeap* heaps[] = { m_cbvSrvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase =
        m_cbvSrvHeap->GetGPUDescriptorHandleForHeapStart();
    const uint32_t N = m_numMaterials;

    // Обновляем материальные CB один раз за кадр
    for (uint32_t mi = 0; mi < N; ++mi)
        UpdateMaterialCB((int)mi);

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

            // param[1]: PerMaterialCB [0..N-1]
            D3D12_GPU_DESCRIPTOR_HANDLE cbvH = gpuBase;
            cbvH.ptr += (SIZE_T)((uint32_t)mi * m_cbvSrvDescriptorSize);
            m_cmdList->SetGraphicsRootDescriptorTable(1, cbvH);

            // param[2]: albedo [N..2N-1]
            D3D12_GPU_DESCRIPTOR_HANDLE albedoH = gpuBase;
            albedoH.ptr += (SIZE_T)((N + (uint32_t)mi) * m_cbvSrvDescriptorSize);
            m_cmdList->SetGraphicsRootDescriptorTable(2, albedoH);

            // param[3]: normal map [2N..3N-1]
            D3D12_GPU_DESCRIPTOR_HANDLE normalH = gpuBase;
            normalH.ptr += (SIZE_T)((2 * N + (uint32_t)mi) * m_cbvSrvDescriptorSize);
            m_cmdList->SetGraphicsRootDescriptorTable(3, normalH);

            // param[4]: displacement [3N..4N-1]
            D3D12_GPU_DESCRIPTOR_HANDLE dispH = gpuBase;
            dispH.ptr += (SIZE_T)((3 * N + (uint32_t)mi) * m_cbvSrvDescriptorSize);
            m_cmdList->SetGraphicsRootDescriptorTable(4, dispH);

            m_cmdList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexStart, 0, 0);
        }
    }

    if (!m_wireframe)
        m_gbuffer.TransitionToShaderResource(m_cmdList.Get());
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::LightingPass()
{
    // Depth → SRV
    D3D12_RESOURCE_BARRIER depthToSRV{};
    depthToSRV.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    depthToSRV.Transition = {
        m_depthStencilBuffer.Get(),
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    };
    // HDR RT: PSR → RT
    D3D12_RESOURCE_BARRIER hdrToRT{};
    hdrToRT.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    hdrToRT.Transition = {
        m_hdrRT.Get(),
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET
    };
    D3D12_RESOURCE_BARRIER bars2[2] = { depthToSRV, hdrToRT };
    m_cmdList->ResourceBarrier(2, bars2);

    // Рисуем в HDR RT (не в back buffer)
    m_cmdList->OMSetRenderTargets(1, &m_hdrRtv, TRUE, nullptr);
    const float clearColor[4] = { 0.f, 0.f, 0.f, 1.f };
    m_cmdList->ClearRenderTargetView(m_hdrRtv, clearColor, 0, nullptr);

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

    // Depth → DSV, HDR RT → PSR (для чтения в PostFxPass)
    D3D12_RESOURCE_BARRIER depthBack{};
    depthBack.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    depthBack.Transition = {
        m_depthStencilBuffer.Get(),
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE
    };
    D3D12_RESOURCE_BARRIER hdrBack{};
    hdrBack.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    hdrBack.Transition = {
        m_hdrRT.Get(),
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    };
    D3D12_RESOURCE_BARRIER barsEnd[2] = { depthBack, hdrBack };
    m_cmdList->ResourceBarrier(2, barsEnd);
}


// ─────────────────────────────────────────────────────────────────────────────
// PostFxPass — читает HDR RT, пишет в back buffer с пост-эффектами
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::PostFxPass()
{
    // Обновляем CB
    static LARGE_INTEGER freq2{}, t02{};
    if (!freq2.QuadPart) {
        QueryPerformanceFrequency(&freq2);
        QueryPerformanceCounter(&t02);
    }
    LARGE_INTEGER now2; QueryPerformanceCounter(&now2);
    float t = (float)((now2.QuadPart - t02.QuadPart) / (double)freq2.QuadPart);

    PostFxConstants pfc{};
    pfc.gTime        = t;
    pfc.gMode        = m_postFxMode;
    pfc.gFishStrength = m_fishStrength;
    pfc.gVhsStrength  = m_vhsStrength;
    std::memcpy(m_mappedPostFxCB, &pfc, sizeof(pfc));

    // Back buffer RT
    auto rtv = CurrentBackBufferRTV();
    m_cmdList->OMSetRenderTargets(1, &rtv, TRUE, nullptr);
    const float black[4] = {0.f,0.f,0.f,1.f};
    m_cmdList->ClearRenderTargetView(rtv, black, 0, nullptr);

    m_cmdList->SetPipelineState(m_postFxPSO.Get());
    m_cmdList->SetGraphicsRootSignature(m_postFxRootSig.Get());
    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissorRect);
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12DescriptorHeap* heaps[] = { m_cbvSrvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);

    // HDR RT SRV в слоте [4N+5]
    D3D12_GPU_DESCRIPTOR_HANDLE srvH =
        m_cbvSrvHeap->GetGPUDescriptorHandleForHeapStart();
    srvH.ptr += (SIZE_T)((4 * m_numMaterials + 5) * m_cbvSrvDescriptorSize);
    m_cmdList->SetGraphicsRootDescriptorTable(0, srvH);
    m_cmdList->SetGraphicsRootConstantBufferView(1, m_postFxCB->GetGPUVirtualAddress());

    m_cmdList->DrawInstanced(3, 1, 0, 0);
}


// ─────────────────────────────────────────────────────────────────────────────
// UpdateCsmCascades
//
// Нелинейное (Practical Split Scheme) распределение каскадов:
//   split_i = lerp( log_split, uniform_split, lambda )
// Для каждого каскада строим tight ortho матрицу вокруг суб-фрустума,
// ориентированную вдоль направления света.
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::UpdateCsmCascades()
{
    // Параметры фрустума камеры (берём из m_proj)
    const float nearZ  = 0.1f;
    const float farZ   = 5000.f;
    const float lambda = 0.75f;  // смешение log и uniform

    // Вычисляем дальние плоскости каскадов (Practical Split Scheme)
    float splits[kCsmCascades + 1];
    splits[0] = nearZ;
    for (int i = 1; i <= kCsmCascades; ++i)
    {
        float iF  = (float)i / kCsmCascades;
        float log  = nearZ * powf(farZ / nearZ, iF);
        float uni  = nearZ + (farZ - nearZ) * iF;
        splits[i]  = lambda * log + (1.f - lambda) * uni;
    }

    // Направление света (нормализованное)
    XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&m_lightDir));

    XMMATRIX view = XMLoadFloat4x4(&m_view);
    XMMATRIX proj = XMLoadFloat4x4(&m_proj);
    XMMATRIX invVP; XMVECTOR det;

    for (int c = 0; c < kCsmCascades; ++c)
    {
        float zNear = splits[c];
        float zFar  = splits[c + 1];

        // 8 вершин суб-фрустума в NDC (D3D: z in [0,1])
        // строим суб-проекцию только для [zNear, zFar]
        XMMATRIX subProj = XMMatrixPerspectiveFovLH(
            0.25f * XM_PI,
            m_width > 0 ? (float)m_width / m_height : 1.f,
            zNear, zFar);
        XMMATRIX subVP    = view * subProj;
        invVP = XMMatrixInverse(&det, subVP);

        // Вершины единичного куба NDC
        static const XMFLOAT3 ndcCorners[8] = {
            {-1,-1, 0}, { 1,-1, 0}, {-1, 1, 0}, { 1, 1, 0},
            {-1,-1, 1}, { 1,-1, 1}, {-1, 1, 1}, { 1, 1, 1},
        };

        // Трансформируем в world space
        XMVECTOR worldCorners[8];
        XMVECTOR center = XMVectorZero();
        for (int k = 0; k < 8; ++k)
        {
            XMVECTOR ndc = XMVectorSet(ndcCorners[k].x, ndcCorners[k].y,
                                       ndcCorners[k].z, 1.f);
            XMVECTOR wc  = XMVector4Transform(ndc, invVP);
            wc = XMVectorDivide(wc, XMVectorSplatW(wc));
            worldCorners[k] = wc;
            center = XMVectorAdd(center, wc);
        }
        center = XMVectorScale(center, 1.f / 8.f);

        // Light view matrix.
        // Важно: используем фиксированное начало координат (0,0,0) как target,
        // а не центр фрустума — иначе при повороте камеры матрица меняется
        // и тени двигаются. AABB всё равно захватит нужную область через snap.
        XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
        if (fabsf(XMVectorGetY(lightDir)) > 0.99f)
            worldUp = XMVectorSet(0, 0, 1, 0);
        // eye = центр фрустума минус lightDir (смотрим вдоль света)
        // Используем centre только для позиционирования — направление фиксировано
        XMVECTOR eye = XMVectorSubtract(center, lightDir);
        XMMATRIX lightView = XMMatrixLookAtLH(eye, center, worldUp);

        // Проецируем все 8 вершин в light view space → AABB
        float minX =  FLT_MAX, maxX = -FLT_MAX;
        float minY =  FLT_MAX, maxY = -FLT_MAX;
        float minZ =  FLT_MAX, maxZ = -FLT_MAX;
        for (int k = 0; k < 8; ++k)
        {
            XMVECTOR lv = XMVector4Transform(worldCorners[k], lightView);
            float x = XMVectorGetX(lv), y = XMVectorGetY(lv), z = XMVectorGetZ(lv);
            minX = (std::min)(minX, x); maxX = (std::max)(maxX, x);
            minY = (std::min)(minY, y); maxY = (std::max)(maxY, y);
            minZ = (std::min)(minZ, z); maxZ = (std::max)(maxZ, z);
        }

        // Расширяем Z назад чтобы захватить объекты за фрустумом (отбрасывают тени)
        float zMult = 3.f;
        if (minZ < 0) minZ *= zMult; else minZ /= zMult;
        if (maxZ < 0) maxZ /= zMult; else maxZ *= zMult;

        // ── Snap to texel grid (стабилизация теней) ──────────────────────
        // Без этого при повороте камеры AABB плавно смещается в light space
        // и тени "плывут". Решение: округляем min/max до границ текселей.
        //
        // Размер одного тексела в world units (в light space):
        float worldUnitsPerTexelX = (maxX - minX) / (float)kShadowMapSize;
        float worldUnitsPerTexelY = (maxY - minY) / (float)kShadowMapSize;

        // Округляем границы AABB вниз до ближайшего тексела
        minX = floorf(minX / worldUnitsPerTexelX) * worldUnitsPerTexelX;
        maxX = floorf(maxX / worldUnitsPerTexelX) * worldUnitsPerTexelX;
        minY = floorf(minY / worldUnitsPerTexelY) * worldUnitsPerTexelY;
        maxY = floorf(maxY / worldUnitsPerTexelY) * worldUnitsPerTexelY;

        // Ortho projection для стабилизированного AABB
        XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(
            minX, maxX, minY, maxY, minZ, maxZ);

        XMMATRIX lvp = lightView * lightProj;
        XMStoreFloat4x4(&m_cascades[c].LightViewProj, XMMatrixTranspose(lvp));
        m_cascades[c].FarPlane = zFar;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ShadowPass — рендерим сцену kCsmCascades раз в shadow map array
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::ShadowPass()
{
    // Shadow map: PSR → DEPTH_WRITE
    D3D12_RESOURCE_BARRIER toDepth{};
    toDepth.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toDepth.Transition = {
        m_shadowMap.Get(),
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE
    };
    m_cmdList->ResourceBarrier(1, &toDepth);

    m_cmdList->SetPipelineState(m_shadowPSO.Get());
    m_cmdList->SetGraphicsRootSignature(m_shadowRootSig.Get());
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmdList->IASetVertexBuffers(0, 1, &m_vbv);
    m_cmdList->IASetIndexBuffer(&m_ibv);

    D3D12_VIEWPORT shadowVP = { 0, 0,
        (float)kShadowMapSize, (float)kShadowMapSize, 0, 1 };
    D3D12_RECT shadowScissor = { 0, 0,
        (LONG)kShadowMapSize, (LONG)kShadowMapSize };

    uint32_t dsvSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvBase =
        m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();

    // два float4x4 = 128 байт, выравниваем до 256
    const uint32_t cbStride = 256u;

    for (int c = 0; c < kCsmCascades; ++c)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvBase;
        dsv.ptr += (SIZE_T)(c * dsvSize);

        m_cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);
        m_cmdList->RSSetViewports(1, &shadowVP);
        m_cmdList->RSSetScissorRects(1, &shadowScissor);

        for (int i : m_visibleIndices)
        {
            // Пишем CB: LightViewProj каскада + World инстанса
            struct { DirectX::XMFLOAT4X4 LightViewProj; DirectX::XMFLOAT4X4 World; } cbd{};
            cbd.LightViewProj = m_cascades[c].LightViewProj;
            cbd.World         = m_instances[i].World;
            uint32_t slot = (uint32_t)(c * kMaxInstances + i);
            std::memcpy(m_mappedShadowCB + (size_t)slot * cbStride,
                        &cbd, sizeof(cbd));

            D3D12_GPU_VIRTUAL_ADDRESS addr =
                m_shadowCB->GetGPUVirtualAddress() +
                (D3D12_GPU_VIRTUAL_ADDRESS)slot * cbStride;
            m_cmdList->SetGraphicsRootConstantBufferView(0, addr);

            for (const auto& sub : m_model.subMeshes)
                m_cmdList->DrawIndexedInstanced(
                    sub.indexCount, 1, sub.indexStart, 0, 0);
        }
    }

    // Shadow map: DEPTH_WRITE → PSR
    D3D12_RESOURCE_BARRIER toPSR = toDepth;
    toPSR.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    toPSR.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_cmdList->ResourceBarrier(1, &toPSR);
}


// ─────────────────────────────────────────────────────────────────────────────
// UpdateWaterCB — обновляет константы воды (время, матрицы, параметры волн)
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::UpdateWaterCB()
{
    if (!m_mappedWaterCB) return;

    static LARGE_INTEGER freqW{}, t0W{};
    if (!freqW.QuadPart) {
        QueryPerformanceFrequency(&freqW);
        QueryPerformanceCounter(&t0W);
    }
    LARGE_INTEGER nowW; QueryPerformanceCounter(&nowW);
    float t = (float)((nowW.QuadPart - t0W.QuadPart) / (double)freqW.QuadPart);

    WaterCB cb{};

    // Плоскость воды — Identity World, но сдвинута по Y на m_waterY
    XMMATRIX world = XMMatrixTranslation(0.f, m_waterY, 0.f);
    XMMATRIX view  = XMLoadFloat4x4(&m_view);
    XMMATRIX proj  = XMLoadFloat4x4(&m_proj);
    XMStoreFloat4x4(&cb.World,         XMMatrixTranspose(world));
    XMStoreFloat4x4(&cb.WorldViewProj, XMMatrixTranspose(world * view * proj));

    cb.EyePosW = m_eyePos;
    cb.Time    = t * m_waterSpeedMul;
    XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&m_lightDir));
    XMStoreFloat3(&cb.LightDirW, L);

    // Амплитуды волн масштабируются множителем (хоткей [/])
    cb.WaveDir0Amp = { 1.0f, 0.3f, 1.2f  * m_waterAmpMul, 0.04f };
    cb.WaveDir1Amp = { 0.4f, 1.0f, 0.7f  * m_waterAmpMul, 0.08f };
    cb.WaveDir2Amp = {-0.7f, 0.6f, 0.4f  * m_waterAmpMul, 0.15f };
    cb.WaveDir3Amp = { 0.8f,-0.5f, 0.25f * m_waterAmpMul, 0.30f };
    cb.WaveSpeeds  = { 1.0f, 1.4f, 2.0f, 2.6f };

    cb.TessFactorNear = 12.f;
    cb.TessFactorFar  = 1.f;
    cb.TessDistNear   = 15.f;
    cb.TessDistFar    = 250.f;

    cb.ShallowColor = { 0.25f, 0.55f, 0.55f, 1.f };
    cb.DeepColor    = { 0.02f, 0.10f, 0.18f, 1.f };

    std::memcpy(m_mappedWaterCB, &cb, sizeof(cb));
}

// ─────────────────────────────────────────────────────────────────────────────
// WaterPass — forward-рендер воды поверх HDR RT (после deferred lighting).
// Использует depth buffer основной сцены для корректного теста глубины
// (вода скрывается за объектами, но сама не пишет в depth — DepthWriteMask=ZERO).
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::WaterPass()
{
    if (!m_waterEnabled) return;

    UpdateWaterCB();

    // Рисуем в HDR RT (после deferred lighting) либо прямо в back buffer
    // (wireframe mode — GeometryPass там рисует прямо в back buffer)
    auto dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    if (m_wireframe)
    {
        auto rtv = CurrentBackBufferRTV();
        m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    }
    else
    {
        m_cmdList->OMSetRenderTargets(1, &m_hdrRtv, FALSE, &dsv);
    }

    m_cmdList->SetPipelineState(m_wireframe ? m_waterWirePSO.Get() : m_waterPSO.Get());
    m_cmdList->SetGraphicsRootSignature(m_waterRootSig.Get());
    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissorRect);
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    m_cmdList->IASetVertexBuffers(0, 1, &m_waterVbv);
    m_cmdList->IASetIndexBuffer(&m_waterIbv);

    m_cmdList->SetGraphicsRootConstantBufferView(0, m_waterCB->GetGPUVirtualAddress());
    m_cmdList->DrawIndexedInstanced(m_waterIndexCount, 1, 0, 0, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::Draw()
{
    if (!m_initialized) return;

    UpdateLightingCB();
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

    UpdateCsmCascades();
    ShadowPass();
    GeometryPass();
    if (!m_wireframe) {
        LightingPass();
        WaterPass();
        PostFxPass();
    } else {
        WaterPass(); // показываем воду тоже в wireframe для проверки тесселяции
    }

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
        m_cbvSrvHeap.Get(), 4 * m_numMaterials, m_cbvSrvDescriptorSize);
    RecreateDepthSRV();
    CreateShadowMap();
    CreateHdrRT();

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
// LoadAndUploadTexture
// Если файл не найден — генерируем заглушку:
//   - для normal map: сплошной {128, 128, 255} (нормаль (0,0,1) в tangent space)
//   - для displacement: сплошной серый {128} (нулевое смещение)
//   - для albedo: шахматная текстура
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
        // Определяем тип заглушки по имени файла
        std::string p(pathA);
        texW = texH = 4;
        pixels.resize(texW * texH * 4);

        bool isNormal = (p.find("normal") != std::string::npos ||
                         p.find("bump")   != std::string::npos ||
                         p.find("_n.")    != std::string::npos);
        bool isDisp   = (p.find("disp")   != std::string::npos ||
                         p.find("height") != std::string::npos ||
                         p.find("_h.")    != std::string::npos ||
                         p.find("_default") != std::string::npos);

        for (uint32_t pi = 0; pi < texW * texH; ++pi) {
            if (isNormal) {
                // (0, 0, 1) в tangent space → (128, 128, 255)
                pixels[pi*4+0] = 128; pixels[pi*4+1] = 128;
                pixels[pi*4+2] = 255; pixels[pi*4+3] = 255;
            } else if (isDisp) {
                // Нулевой дисплейсмент — средний серый
                pixels[pi*4+0] = pixels[pi*4+1] = pixels[pi*4+2] = 0;
                pixels[pi*4+3] = 255;
            } else {
                // Шахматная текстура для albedo
                uint32_t y2 = pi / texW, x2 = pi % texW;
                bool wh = ((x2/2)+(y2/2))%2==0;
                pixels[pi*4+0] = wh?255u:50u; pixels[pi*4+1] = wh?255u:200u;
                pixels[pi*4+2] = wh?255u:50u; pixels[pi*4+3] = 255u;
            }
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
