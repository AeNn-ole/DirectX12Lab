#include "GBuffer.h"
#include <stdexcept>
#include <cstdio>

static void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (hr=0x%08X)", what, (unsigned)hr);
        throw std::runtime_error(buf);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
bool GBuffer::Create(ID3D12Device*          device,
                     uint32_t               width,
                     uint32_t               height,
                     ID3D12DescriptorHeap*  rtvHeap,
                     uint32_t               rtvOffset,
                     uint32_t               rtvDescSize,
                     ID3D12DescriptorHeap*  srvHeap,
                     uint32_t               srvOffset,
                     uint32_t               srvDescSize)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;

    for (int i = 0; i < Count; ++i)
    {
        // ── Создаём текстуру ──────────────────────────────────────────────
        D3D12_RESOURCE_DESC td{};
        td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width            = width;
        td.Height           = height;
        td.DepthOrArraySize = 1;
        td.MipLevels        = 1;
        td.Format           = kFormats[i];
        td.SampleDesc.Count = 1;
        td.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        // Optimized clear value — D3D12 требует при ALLOW_RENDER_TARGET
        D3D12_CLEAR_VALUE cv{};
        cv.Format   = kFormats[i];
        cv.Color[0] = cv.Color[1] = cv.Color[2] = cv.Color[3] = 0.0f;

        // Создаём сразу в PIXEL_SHADER_RESOURCE — TransitionToRenderTarget
        // переключит его перед первым geometry pass
        ThrowIfFailed(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &cv, IID_PPV_ARGS(&m_textures[i])),
            "GBuffer::Create — CreateCommittedResource");

        // ── RTV ───────────────────────────────────────────────────────────
        D3D12_CPU_DESCRIPTOR_HANDLE rtvH =
            rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvH.ptr += (SIZE_T)((rtvOffset + i) * rtvDescSize);
        m_rtvHandles[i] = rtvH;
        device->CreateRenderTargetView(m_textures[i].Get(), nullptr, rtvH);

        // ── SRV ───────────────────────────────────────────────────────────
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpuH =
            srvHeap->GetCPUDescriptorHandleForHeapStart();
        srvCpuH.ptr += (SIZE_T)((srvOffset + i) * srvDescSize);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format                  = kFormats[i];
        srvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Texture2D.MipLevels     = 1;
        device->CreateShaderResourceView(m_textures[i].Get(), &srvd, srvCpuH);

        // Кешируем GPU-хендл для SetDescriptorTable в lighting pass
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpuH =
            srvHeap->GetGPUDescriptorHandleForHeapStart();
        srvGpuH.ptr += (SIZE_T)((srvOffset + i) * srvDescSize);
        m_srvGpuHandles[i] = srvGpuH;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
void GBuffer::Destroy()
{
    for (auto& t : m_textures) t.Reset();
}

// ─────────────────────────────────────────────────────────────────────────────
void GBuffer::Resize(ID3D12Device*          device,
                     uint32_t               width,
                     uint32_t               height,
                     ID3D12DescriptorHeap*  rtvHeap,
                     uint32_t               rtvOffset,
                     uint32_t               rtvDescSize,
                     ID3D12DescriptorHeap*  srvHeap,
                     uint32_t               srvOffset,
                     uint32_t               srvDescSize)
{
    Destroy();
    Create(device, width, height,
           rtvHeap, rtvOffset, rtvDescSize,
           srvHeap, srvOffset, srvDescSize);
}

// ─────────────────────────────────────────────────────────────────────────────
void GBuffer::TransitionToRenderTarget(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER bars[Count]{};
    for (int i = 0; i < Count; ++i)
    {
        bars[i].Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[i].Transition = {
            m_textures[i].Get(),
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET
        };
    }
    // Все три барьера за один вызов — эффективнее
    cmd->ResourceBarrier(Count, bars);
}

// ─────────────────────────────────────────────────────────────────────────────
void GBuffer::TransitionToShaderResource(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER bars[Count]{};
    for (int i = 0; i < Count; ++i)
    {
        bars[i].Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[i].Transition = {
            m_textures[i].Get(),
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        };
    }
    cmd->ResourceBarrier(Count, bars);
}

// ─────────────────────────────────────────────────────────────────────────────
void GBuffer::Clear(ID3D12GraphicsCommandList* cmd)
{
    const float black[4] = { 0.f, 0.f, 0.f, 0.f };
    for (int i = 0; i < Count; ++i)
        cmd->ClearRenderTargetView(m_rtvHandles[i], black, 0, nullptr);
}
