#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// GBuffer — три текстуры-рендер-таргета для deferred rendering.
//
// Раскладка слотов:
//   RT0 Albedo   — R8G8B8A8_UNORM  : RGB = диффузный цвет (texture * Kd)
//   RT1 Normal   — R16G16B16A16_FLOAT : XYZ = нормаль в world space (float16!)
//   RT2 Specular — R8G8B8A8_UNORM  : RGB = Ks, A = Ns/255
//
// Каждая текстура рождается в состоянии PIXEL_SHADER_RESOURCE.
// Перед geometry pass надо вызвать TransitionToRenderTarget(),
// после — TransitionToShaderResource(), и только тогда lighting pass
// может читать их как SRV.
// ─────────────────────────────────────────────────────────────────────────────
class GBuffer
{
public:
    enum Slot { Albedo = 0, Normal = 1, Specular = 2, Count = 3 };

    // Форматы по слотам — нужны в нескольких местах снаружи (PSO, root sig)
    static constexpr DXGI_FORMAT kFormats[Count] = {
        DXGI_FORMAT_R8G8B8A8_UNORM,        // Albedo
        DXGI_FORMAT_R16G16B16A16_FLOAT,    // Normal  ← float16 важен для точности
        DXGI_FORMAT_R8G8B8A8_UNORM,        // Specular
    };

    // Создаёт три текстуры и регистрирует дескрипторы.
    //   rtvHeap/rtvOffset  — куда писать RTV (начиная с rtvOffset-го слота кучи RTV)
    //   srvHeap/srvOffset  — куда писать SRV (начиная с srvOffset-го слота CBV/SRV кучи)
    bool Create(ID3D12Device*          device,
                uint32_t               width,
                uint32_t               height,
                ID3D12DescriptorHeap*  rtvHeap,
                uint32_t               rtvOffset,
                uint32_t               rtvDescSize,
                ID3D12DescriptorHeap*  srvHeap,
                uint32_t               srvOffset,
                uint32_t               srvDescSize);

    // Освобождает только GPU-ресурсы текстур (кучи чужие — не трогает)
    void Destroy();

    // Пересоздаёт текстуры с новым размером, обновляет дескрипторы на месте
    void Resize(ID3D12Device*          device,
                uint32_t               width,
                uint32_t               height,
                ID3D12DescriptorHeap*  rtvHeap,
                uint32_t               rtvOffset,
                uint32_t               rtvDescSize,
                ID3D12DescriptorHeap*  srvHeap,
                uint32_t               srvOffset,
                uint32_t               srvDescSize);

    // Барьер PSR → RT (вызывать перед geometry pass)
    void TransitionToRenderTarget(ID3D12GraphicsCommandList* cmd);
    // Барьер RT → PSR (вызывать после geometry pass)
    void TransitionToShaderResource(ID3D12GraphicsCommandList* cmd);

    // Очищает все три RT чёрным (вызывать пока текстуры в RT-состоянии)
    void Clear(ID3D12GraphicsCommandList* cmd);

    // CPU-хендл для OMSetRenderTargets (geometry pass)
    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV(Slot s) const { return m_rtvHandles[s]; }

    // GPU-хендл первого SRV — при непрерывной раскладке в куче
    // lighting pass делает SetDescriptorTable на этот хендл и получает t0/t1/t2
    D3D12_GPU_DESCRIPTOR_HANDLE GetFirstSRV() const { return m_srvGpuHandles[0]; }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_textures[Count];

    D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandles[Count]{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_srvGpuHandles[Count]{};
};
