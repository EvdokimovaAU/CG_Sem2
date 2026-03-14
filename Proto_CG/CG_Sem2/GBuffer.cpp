#include "GBuffer.h"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr std::array<DXGI_FORMAT, GBuffer::TargetCount> kFormats =
    {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R32_FLOAT
    };

    constexpr std::array<float, 4> kClearAlbedo = { 0.0f, 0.0f, 0.0f, 1.0f };
    constexpr std::array<float, 4> kClearWorld = { 0.0f, 0.0f, 0.0f, 0.0f };
    constexpr std::array<float, 4> kClearNormal = { 0.5f, 0.5f, 1.0f, 0.0f };
    constexpr std::array<float, 4> kClearDepth = { 1.0f, 0.0f, 0.0f, 0.0f };

    constexpr std::array<std::array<float, 4>, GBuffer::TargetCount> kClearColors =
    {
        kClearAlbedo,
        kClearWorld,
        kClearNormal,
        kClearDepth
    };
}

bool GBuffer::Initialize(ID3D12Device* device, UINT width, UINT height)
{
    if (device == nullptr || width == 0 || height == 0)
    {
        return false;
    }

    m_device = device;
    m_width = width;
    m_height = height;
    m_currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    if (!CreateDescriptorHeaps())
    {
        return false;
    }

    return CreateRenderTargets();
}

void GBuffer::Shutdown()
{
    for (auto& target : m_targets)
    {
        target.Reset();
    }

    m_srvHeap.Reset();
    m_rtvHeap.Reset();
    m_device.Reset();
    m_width = 0;
    m_height = 0;
    m_rtvDescriptorSize = 0;
    m_srvDescriptorSize = 0;
}

void GBuffer::BeginGeometryPass(
    ID3D12GraphicsCommandList* commandList,
    D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView)
{
    if (commandList == nullptr)
    {
        return;
    }

    if (m_currentState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        TransitionAll(
            commandList,
            m_currentState,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, TargetCount> rtvs{};
    for (UINT i = 0; i < TargetCount; ++i)
    {
        rtvs[i] = GetRTV(static_cast<Slot>(i));
        commandList->ClearRenderTargetView(
            rtvs[i],
            kClearColors[i].data(),
            0,
            nullptr);
    }

    commandList->OMSetRenderTargets(
        TargetCount,
        rtvs.data(),
        FALSE,
        &depthStencilView);
}

void GBuffer::EndGeometryPass(ID3D12GraphicsCommandList* commandList)
{
    if (commandList == nullptr)
    {
        return;
    }

    if (m_currentState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        TransitionAll(
            commandList,
            m_currentState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
}

ID3D12DescriptorHeap* GBuffer::GetRTVHeap() const
{
    return m_rtvHeap.Get();
}

ID3D12DescriptorHeap* GBuffer::GetSRVHeap() const
{
    return m_srvHeap.Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetRTV(Slot slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T(static_cast<UINT>(slot)) * SIZE_T(m_rtvDescriptorSize);
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetSRV(Slot slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T(static_cast<UINT>(slot)) * SIZE_T(m_srvDescriptorSize);
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE GBuffer::GetSRVGPU(Slot slot) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T(static_cast<UINT>(slot)) * SIZE_T(m_srvDescriptorSize);
    return handle;
}

DXGI_FORMAT GBuffer::GetFormat(Slot slot) const
{
    return kFormats[static_cast<UINT>(slot)];
}

UINT GBuffer::GetWidth() const
{
    return m_width;
}

UINT GBuffer::GetHeight() const
{
    return m_height;
}

bool GBuffer::CreateDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = TargetCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

    if (FAILED(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap))))
    {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.NumDescriptors = TargetCount;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(m_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap))))
    {
        return false;
    }

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool GBuffer::CreateRenderTargets()
{
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (UINT i = 0; i < TargetCount; ++i)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = m_width;
        desc.Height = m_height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kFormats[i];
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = kFormats[i];
        for (UINT c = 0; c < 4; ++c)
        {
            clearValue.Color[c] = kClearColors[i][c];
        }

        if (FAILED(m_device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            &clearValue,
            IID_PPV_ARGS(&m_targets[i]))))
        {
            return false;
        }

        m_device->CreateRenderTargetView(m_targets[i].Get(), nullptr, GetRTV(static_cast<Slot>(i)));

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = kFormats[i];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_targets[i].Get(), &srvDesc, GetSRV(static_cast<Slot>(i)));
    }

    return true;
}

void GBuffer::TransitionAll(
    ID3D12GraphicsCommandList* commandList,
    D3D12_RESOURCE_STATES beforeState,
    D3D12_RESOURCE_STATES afterState)
{
    std::array<D3D12_RESOURCE_BARRIER, TargetCount> barriers{};

    for (UINT i = 0; i < TargetCount; ++i)
    {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = m_targets[i].Get();
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[i].Transition.StateBefore = beforeState;
        barriers[i].Transition.StateAfter = afterState;
    }

    commandList->ResourceBarrier(TargetCount, barriers.data());
}
