#include "RenderingSystem.h"

#include <array>
#include <fstream>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>

#include "stb_image.h"

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    constexpr UINT kParticleSrvBuffer0 = 0;
    constexpr UINT kParticleSrvBuffer1 = 1;
    constexpr UINT kParticleUavBuffer0 = 2;
    constexpr UINT kParticleUavBuffer1 = 3;
    constexpr UINT kParticleTextureSrv = 4;
    constexpr UINT kParticleThreadGroupSize = 64;
    constexpr UINT kFireSrvBuffer0 = 0;
    constexpr UINT kFireSrvBuffer1 = 1;
    constexpr UINT kFireUavBuffer0 = 2;
    constexpr UINT kFireUavBuffer1 = 3;
    constexpr UINT kFireThreadGroupSize = 64;
    constexpr UINT kLightingShadowMapSrv = GBuffer::TargetCount;
    constexpr UINT kLightingShadowMaskSrv = GBuffer::TargetCount + 1;
    constexpr UINT kLightingIrradianceSrv = GBuffer::TargetCount + 2;
    constexpr UINT kLightingBrdfSrv = GBuffer::TargetCount + 3;
    constexpr UINT kLightingPrefilterSrv = GBuffer::TargetCount + 4;
    constexpr UINT kLightingSrvCount = GBuffer::TargetCount + 5;

    constexpr UINT DDS_MAGIC = 0x20534444u;
    constexpr UINT DDS_FOURCC = 0x00000004u;
    constexpr UINT DDSCAPS2_CUBEMAP = 0x00000200u;
    constexpr UINT DDSCAPS2_CUBEMAP_ALLFACES = 0x0000FC00u;
    constexpr UINT DDS_RESOURCE_MISC_TEXTURECUBE = 0x4u;

    struct DDS_PIXELFORMAT
    {
        UINT size;
        UINT flags;
        UINT fourCC;
        UINT RGBBitCount;
        UINT RBitMask;
        UINT GBitMask;
        UINT BBitMask;
        UINT ABitMask;
    };

    struct DDS_HEADER
    {
        UINT size;
        UINT flags;
        UINT height;
        UINT width;
        UINT pitchOrLinearSize;
        UINT depth;
        UINT mipMapCount;
        UINT reserved1[11];
        DDS_PIXELFORMAT ddspf;
        UINT caps;
        UINT caps2;
        UINT caps3;
        UINT caps4;
        UINT reserved2;
    };

    struct DDS_HEADER_DXT10
    {
        DXGI_FORMAT dxgiFormat;
        UINT resourceDimension;
        UINT miscFlag;
        UINT arraySize;
        UINT miscFlags2;
    };

    UINT BitsPerPixel(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 4;
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 8;
        default:
            return 0;
        }
    }

    void GetSurfaceInfo(size_t width, size_t height, DXGI_FORMAT fmt, size_t& outNumBytes, size_t& outRowBytes, size_t& outNumRows)
    {
        const UINT bpe = BitsPerPixel(fmt);
        if (bpe == 0)
        {
            outNumBytes = 0;
            outRowBytes = 0;
            outNumRows = 0;
            return;
        }

        const size_t numBlocksWide = (std::max<size_t>)(1u, (width + 3u) / 4u);
        const size_t numBlocksHigh = (std::max<size_t>)(1u, (height + 3u) / 4u);
        outRowBytes = numBlocksWide * bpe;
        outNumRows = numBlocksHigh;
        outNumBytes = outRowBytes * numBlocksHigh;
    }

    struct DebugOverlayConstants
    {
        XMFLOAT4 OverlayRect;
        XMFLOAT4 SceneCenter;
        XMFLOAT4 SceneExtents;
        UINT DebugMode = 0;
        UINT Padding[3] = {};
    };

    float Hash01(UINT value)
    {
        value ^= 2747636419u;
        value *= 2654435769u;
        value ^= value >> 16;
        value *= 2654435769u;
        value ^= value >> 16;
        value *= 2654435769u;
        return static_cast<float>(value & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
    }

    bool ResolveShaderPath(const wchar_t* fileName, wchar_t* outPath, size_t outPathCount)
    {
        wchar_t exeDir[MAX_PATH];
        GetModuleFileNameW(nullptr, exeDir, MAX_PATH);

        wchar_t* lastSlash = wcsrchr(exeDir, L'\\');
        if (lastSlash == nullptr)
        {
            return false;
        }
        *(lastSlash + 1) = 0;

        wcscpy_s(outPath, outPathCount, exeDir);
        wcscat_s(outPath, outPathCount, L"..\\..\\CG_Sem2\\");
        wcscat_s(outPath, outPathCount, fileName);

        if (GetFileAttributesW(outPath) == INVALID_FILE_ATTRIBUTES)
        {
            wcscpy_s(outPath, outPathCount, exeDir);
            wcscat_s(outPath, outPathCount, fileName);
        }

        return GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES;
    }

    bool ResolveAssetPath(const wchar_t* relativePath, wchar_t* outPath, size_t outPathCount)
    {
        wchar_t exeDir[MAX_PATH];
        GetModuleFileNameW(nullptr, exeDir, MAX_PATH);

        wchar_t* lastSlash = wcsrchr(exeDir, L'\\');
        if (lastSlash == nullptr)
        {
            return false;
        }
        *(lastSlash + 1) = 0;

        wcscpy_s(outPath, outPathCount, exeDir);
        wcscat_s(outPath, outPathCount, relativePath);
        return GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES;
    }

    XMFLOAT4X4 StoreMatrix(const XMMATRIX& matrix)
    {
        XMFLOAT4X4 result{};
        XMStoreFloat4x4(&result, matrix);
        return result;
    }
}

bool RenderingSystem::Initialize(HWND hwnd, UINT width, UINT height)
{
    m_width = width;
    m_height = height;

    if (!m_context.Initialize(hwnd, width, height))
    {
        return false;
    }

    if (!m_gbuffer.Initialize(m_context.GetDevice(), width, height))
    {
        return false;
    }

    return InitializeDeferredResources();
}

bool RenderingSystem::LoadScene(Scene scene)
{
    m_particleDataInitialized = false;
    m_particleSourceIndex = 0;
    m_previousParticleTime = 0.0f;
    m_fireDataInitialized = false;
    m_fireSourceIndex = 0;
    m_previousFireTime = 0.0f;
    return m_context.LoadScene(scene);
}

void RenderingSystem::Shutdown()
{
    if (m_deferredLightCBMappedData != nullptr && m_deferredLightConstantBuffer != nullptr)
    {
        m_deferredLightConstantBuffer->Unmap(0, nullptr);
        m_deferredLightCBMappedData = nullptr;
    }
    if (m_waterCBMappedData != nullptr && m_waterConstantBuffer != nullptr)
    {
        m_waterConstantBuffer->Unmap(0, nullptr);
        m_waterCBMappedData = nullptr;
    }
    if (m_particleSimulationCBMappedData != nullptr && m_particleSimulationConstantBuffer != nullptr)
    {
        m_particleSimulationConstantBuffer->Unmap(0, nullptr);
        m_particleSimulationCBMappedData = nullptr;
    }
    if (m_particleRenderCBMappedData != nullptr && m_particleRenderConstantBuffer != nullptr)
    {
        m_particleRenderConstantBuffer->Unmap(0, nullptr);
        m_particleRenderCBMappedData = nullptr;
    }
    if (m_fireSimulationCBMappedData != nullptr && m_fireSimulationConstantBuffer != nullptr)
    {
        m_fireSimulationConstantBuffer->Unmap(0, nullptr);
        m_fireSimulationCBMappedData = nullptr;
    }
    if (m_fireRenderCBMappedData != nullptr && m_fireRenderConstantBuffer != nullptr)
    {
        m_fireRenderConstantBuffer->Unmap(0, nullptr);
        m_fireRenderCBMappedData = nullptr;
    }

    m_deferredLightConstantBuffer.Reset();
    m_particleSimulationConstantBuffer.Reset();
    m_particleRenderConstantBuffer.Reset();
    m_fireSimulationConstantBuffer.Reset();
    m_fireRenderConstantBuffer.Reset();
    m_waterConstantBuffer.Reset();
    m_shadowMap.Reset();
    m_hdrColorBuffer.Reset();
    m_irradianceMap.Reset();
    m_irradianceMapUpload.Reset();
    m_brdfIntegrationMap.Reset();
    m_brdfIntegrationMapUpload.Reset();
    m_prefilteredEnvMap.Reset();
    m_prefilteredEnvMapUpload.Reset();
    m_shadowMaskTexture.Reset();
    m_shadowMaskTextureUpload.Reset();
    m_deferredGeometryPSO.Reset();
    m_shadowPSO.Reset();
    m_deferredLightingPSO.Reset();
    m_postProcessPSO.Reset();
    m_debugOverlayPSO.Reset();
    m_particleGraphicsPSO.Reset();
    m_particleComputePSO.Reset();
    m_fireGraphicsPSO.Reset();
    m_fireComputePSO.Reset();
    m_waterPSO.Reset();
    m_shadowRootSignature.Reset();
    m_postProcessRootSignature.Reset();
    m_debugOverlayRootSignature.Reset();
    m_deferredLightingRootSignature.Reset();
    m_particleGraphicsRootSignature.Reset();
    m_particleComputeRootSignature.Reset();
    m_fireGraphicsRootSignature.Reset();
    m_fireComputeRootSignature.Reset();
    m_waterRootSignature.Reset();
    m_deferredGeometryVS.Reset();
    m_deferredGeometryHS.Reset();
    m_deferredGeometryDS.Reset();
    m_deferredGeometryPS.Reset();
    m_deferredLightingVS.Reset();
    m_deferredLightingPS.Reset();
    m_postProcessVS.Reset();
    m_postProcessPS.Reset();
    m_shadowVS.Reset();
    m_shadowHS.Reset();
    m_shadowDS.Reset();
    m_debugOverlayVS.Reset();
    m_debugOverlayPS.Reset();
    m_particleVS.Reset();
    m_particleGS.Reset();
    m_particlePS.Reset();
    m_particleCS.Reset();
    m_fireVS.Reset();
    m_fireGS.Reset();
    m_firePS.Reset();
    m_fireCS.Reset();
    m_waterVS.Reset();
    m_waterHS.Reset();
    m_waterDS.Reset();
    m_waterPS.Reset();
    m_particleBuffers[0].Reset();
    m_particleBuffers[1].Reset();
    m_particleCounterBuffers[0].Reset();
    m_particleCounterBuffers[1].Reset();
    m_particleUploadBuffer.Reset();
    m_particleCounterUploadBuffer.Reset();
    m_particleTexture.Reset();
    m_particleTextureUpload.Reset();
    m_particleHeap.Reset();
    m_shadowDsvHeap.Reset();
    m_hdrRtvHeap.Reset();
    m_lightingSrvHeap.Reset();
    m_postProcessSrvHeap.Reset();
    m_fireBuffers[0].Reset();
    m_fireBuffers[1].Reset();
    m_fireCounterBuffers[0].Reset();
    m_fireCounterBuffers[1].Reset();
    m_fireUploadBuffer.Reset();
    m_fireCounterUploadBuffer.Reset();
    m_fireHeap.Reset();
    m_gbuffer.Shutdown();
    m_context.Shutdown();
}

void RenderingSystem::SetTechnique(Technique technique)
{
    m_technique = technique;
}

RenderingSystem::Technique RenderingSystem::GetTechnique() const
{
    return m_technique;
}

RenderingSystem::Scene RenderingSystem::GetCurrentScene() const
{
    return m_context.GetCurrentScene();
}

void RenderingSystem::SetFrustumCullingEnabled(bool enabled)
{
    m_context.SetFrustumCullingEnabled(enabled);
}

bool RenderingSystem::IsFrustumCullingEnabled() const
{
    return m_context.IsFrustumCullingEnabled();
}

void RenderingSystem::SetOctreeEnabled(bool enabled)
{
    m_context.SetOctreeEnabled(enabled);
}

bool RenderingSystem::IsOctreeEnabled() const
{
    return m_context.IsOctreeEnabled();
}

void RenderingSystem::SetClearColor(float r, float g, float b, float a)
{
    m_clearColor[0] = r;
    m_clearColor[1] = g;
    m_clearColor[2] = b;
    m_clearColor[3] = a;
}

void RenderingSystem::SetTime(float timeSeconds)
{
    m_context.SetTime(timeSeconds);
}

void RenderingSystem::SetUVTiling(float x, float y)
{
    m_context.SetUVTiling(x, y);
}

void RenderingSystem::SetUVScrollSpeed(float uSpeed, float vSpeed)
{
    m_context.SetUVScrollSpeed(uSpeed, vSpeed);
}

void RenderingSystem::UpdateCameraOrbit(
    float deltaTime,
    float rotateSpeed,
    float dollySpeed,
    bool orbitRotate,
    bool dolly,
    float mouseDeltaX,
    float mouseDeltaY)
{
    m_context.UpdateCameraOrbit(
        deltaTime,
        rotateSpeed,
        dollySpeed,
        orbitRotate,
        dolly,
        mouseDeltaX,
        mouseDeltaY);
}

void RenderingSystem::UpdateCameraMove(float deltaTime, float forwardInput, float strafeInput, float moveSpeed)
{
    m_context.UpdateCameraMove(deltaTime, forwardInput, strafeInput, moveSpeed);
}

// выбирает способ отрисовки
void RenderingSystem::RenderFrame()
{
    switch (m_technique)
    {
    case Technique::Deferred:
        RenderDeferredFrame();
        break;
    case Technique::Forward:
    default:
        RenderForwardFrame();
        break;
    }
}

void RenderingSystem::RenderForwardFrame()
{
    m_context.Render(
        m_clearColor[0],
        m_clearColor[1],
        m_clearColor[2],
        m_clearColor[3]);
}

void RenderingSystem::RenderDeferredFrame()
{
    if (!m_deferredGeometryPSO || !m_deferredLightingPSO || !m_postProcessPSO)
    {
        RenderForwardFrame();
        return;
    }

    m_context.BeginFrame();
    UpdateParticleSimulation();
    UpdateFireSimulation();
    RenderShadowStage();
    RenderOpaqueStage();
    RenderLightingStage();
    RenderPostProcessStage();
    RenderFireStage();
    RenderParticleStage();
    RenderGBufferDebugOverlay();
    RenderTransparentStage();
    m_context.EndFrame();
}

// рендеринг с точки зрения света
void RenderingSystem::RenderShadowStage()
{
    if (!m_shadowPSO || !m_shadowRootSignature || !m_shadowMap || !m_shadowDsvHeap)
    {
        return;
    }

    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    UpdateLightingConstants();
    const DeferredLightCB shadowCb = *reinterpret_cast<const DeferredLightCB*>(m_deferredLightCBMappedData);
    static const XMFLOAT4X4 identity = StoreMatrix(XMMatrixIdentity());

    if (m_shadowMapState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_shadowMap.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = m_shadowMapState;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        commandList->ResourceBarrier(1, &barrier);
        m_shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    commandList->SetPipelineState(m_shadowPSO.Get());
    commandList->SetGraphicsRootSignature(m_shadowRootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = { m_context.GetSceneSRVHeap() };
    commandList->SetDescriptorHeaps(1, heaps);

    D3D12_VIEWPORT shadowViewport{};
    shadowViewport.Width = static_cast<float>(ShadowMapResolution);
    shadowViewport.Height = static_cast<float>(ShadowMapResolution);
    shadowViewport.MaxDepth = 1.0f;
    D3D12_RECT shadowScissor{ 0, 0, static_cast<LONG>(ShadowMapResolution), static_cast<LONG>(ShadowMapResolution) };
    commandList->RSSetViewports(1, &shadowViewport);
    commandList->RSSetScissorRects(1, &shadowScissor);

    for (UINT cascadeIndex = 0; cascadeIndex < ShadowCascadeCount; ++cascadeIndex)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsv.ptr += SIZE_T(cascadeIndex) * SIZE_T(m_shadowDsvDescriptorSize);
        commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        const XMMATRIX lightViewProj = XMMatrixTranspose(XMLoadFloat4x4(&shadowCb.LightViewProj[cascadeIndex]));
        const XMFLOAT4X4 lightViewProjFloat = StoreMatrix(lightViewProj);
        m_context.DrawSceneShadowGeometry(
            commandList,
            1,
            identity,
            lightViewProjFloat);
    }

    if (m_shadowMapState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_shadowMap.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = m_shadowMapState;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        commandList->ResourceBarrier(1, &barrier);
        m_shadowMapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
}

void RenderingSystem::RenderOpaqueStage()
{
    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    D3D12_VIEWPORT vp = m_context.GetViewport();
    D3D12_RECT sc = m_context.GetScissorRect();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_context.GetDepthStencilView();

    commandList->RSSetViewports(1, &vp);
    commandList->RSSetScissorRects(1, &sc);
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    m_gbuffer.BeginGeometryPass(commandList, dsv);

    commandList->SetPipelineState(m_deferredGeometryPSO.Get());
    commandList->SetGraphicsRootSignature(m_context.GetSceneRootSignature());

    ID3D12DescriptorHeap* heaps[] = { m_context.GetSceneSRVHeap() };
    commandList->SetDescriptorHeaps(1, heaps);

    m_context.UpdateSceneConstants();
    commandList->SetGraphicsRootConstantBufferView(0, m_context.GetSceneConstantBufferAddress());
    m_context.DrawSceneGeometry(commandList, 1, 2);

    m_gbuffer.EndGeometryPass(commandList);
}


// рачсет освещения
void RenderingSystem::RenderLightingStage()
{
    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    D3D12_VIEWPORT vp = m_context.GetViewport();
    D3D12_RECT sc = m_context.GetScissorRect();
    D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv = m_hdrRtvHeap->GetCPUDescriptorHandleForHeapStart();

    UpdateLightingConstants();

    if (!m_shadowMaskUploaded && m_shadowMaskTexture != nullptr && m_shadowMaskTextureUpload != nullptr)
    {
        const D3D12_RESOURCE_DESC textureDesc = m_shadowMaskTexture->GetDesc();
        const UINT rowPitch = (static_cast<UINT>(textureDesc.Width) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
            ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = m_shadowMaskTexture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = m_shadowMaskTextureUpload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = static_cast<UINT>(textureDesc.Width);
        src.PlacedFootprint.Footprint.Height = textureDesc.Height;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = rowPitch;

        commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER textureBarrier{};
        textureBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        textureBarrier.Transition.pResource = m_shadowMaskTexture.Get();
        textureBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        textureBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        textureBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        commandList->ResourceBarrier(1, &textureBarrier);

        m_shadowMaskUploaded = true;
    }

    if (m_hdrColorBufferState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_hdrColorBuffer.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = m_hdrColorBufferState;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList->ResourceBarrier(1, &barrier);
        m_hdrColorBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    commandList->RSSetViewports(1, &vp);
    commandList->RSSetScissorRects(1, &sc);
    const float hdrClear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    commandList->OMSetRenderTargets(1, &hdrRtv, TRUE, nullptr);
    commandList->ClearRenderTargetView(hdrRtv, hdrClear, 0, nullptr);

    commandList->SetPipelineState(m_deferredLightingPSO.Get());
    commandList->SetGraphicsRootSignature(m_deferredLightingRootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = { m_lightingSrvHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootDescriptorTable(0, m_lightingSrvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetGraphicsRootConstantBufferView(1, m_deferredLightConstantBuffer->GetGPUVirtualAddress());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(6, 1, 0, 0);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_hdrColorBuffer.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = m_hdrColorBufferState;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &barrier);
    m_hdrColorBufferState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

void RenderingSystem::RenderPostProcessStage()
{
    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    D3D12_VIEWPORT vp = m_context.GetViewport();
    D3D12_RECT sc = m_context.GetScissorRect();
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = m_context.GetCurrentBackBufferRTV();

    commandList->RSSetViewports(1, &vp);
    commandList->RSSetScissorRects(1, &sc);
    commandList->OMSetRenderTargets(1, &backBufferRtv, TRUE, nullptr);
    commandList->ClearRenderTargetView(backBufferRtv, m_clearColor, 0, nullptr);

    commandList->SetPipelineState(m_postProcessPSO.Get());
    commandList->SetGraphicsRootSignature(m_postProcessRootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = { m_postProcessSrvHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootDescriptorTable(0, m_postProcessSrvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(6, 1, 0, 0);
}

void RenderingSystem::RenderTransparentStage()
{
    if (!m_waterPSO || !m_waterRootSignature)
    {
        return;
    }

    if (m_context.GetCurrentScene() != Scene::Sponza)
    {
        return;
    }

    UpdateWaterConstants();

    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    D3D12_VIEWPORT vp = m_context.GetViewport();
    D3D12_RECT sc = m_context.GetScissorRect();
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = m_context.GetCurrentBackBufferRTV();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_context.GetDepthStencilView();

    commandList->RSSetViewports(1, &vp);
    commandList->RSSetScissorRects(1, &sc);
    commandList->OMSetRenderTargets(1, &backBufferRtv, TRUE, &dsv);
    commandList->SetPipelineState(m_waterPSO.Get());
    commandList->SetGraphicsRootSignature(m_waterRootSignature.Get());
    commandList->SetGraphicsRootConstantBufferView(0, m_waterConstantBuffer->GetGPUVirtualAddress());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    commandList->DrawInstanced(6, 1, 0, 0);
}

bool RenderingSystem::InitializeDeferredResources()
{
    return CompileDeferredShaders() &&
        CreateShadowResources() &&
        CreateShadowMaskTexture() &&
        CreateShadowRootSignature() &&
        CreateShadowPipeline() &&
        CreateHdrResources() &&
        CreateIrradianceMapResource() &&
        CreateBrdfIntegrationMapResource() &&
        CreatePrefilteredEnvMapResource() &&
        CreateLightingSrvHeap() &&
        CreateDeferredLightingRootSignature() &&
        CreateDeferredGeometryPipeline() &&
        CreateDeferredLightingPipeline() &&
        CreatePostProcessRootSignature() &&
        CreatePostProcessPipeline() &&
        CreateDebugOverlayRootSignature() &&
        CreateDebugOverlayPipeline() &&
        CreateParticleRootSignature() &&
        CreateParticleSimulationPipeline() &&
        CreateParticleRenderPipeline() &&
        CreateParticleResources() &&
        CreateParticleTexture() &&
        CreateParticleConstantBuffers() &&
        CreateFireRootSignature() &&
        CreateFireSimulationPipeline() &&
        CreateFireRenderPipeline() &&
        CreateFireResources() &&
        CreateFireConstantBuffers() &&
        CreateWaterRootSignature() &&
        CreateWaterPipeline() &&
        CreateLightingConstantBuffer() &&
        CreateWaterConstantBuffer();
}

bool RenderingSystem::CompileDeferredShaders()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    wchar_t geometryPath[MAX_PATH];
    wchar_t lightingPath[MAX_PATH];
    wchar_t postProcessPath[MAX_PATH];
    wchar_t shadowPath[MAX_PATH];
    wchar_t debugPath[MAX_PATH];
    wchar_t particlePath[MAX_PATH];
    wchar_t firePath[MAX_PATH];
    wchar_t waterPath[MAX_PATH];
    if (!ResolveShaderPath(L"DeferredGeometry.hlsl", geometryPath, MAX_PATH) ||
        !ResolveShaderPath(L"DeferredLighting.hlsl", lightingPath, MAX_PATH) ||
        !ResolveShaderPath(L"PostProcessComposite.hlsl", postProcessPath, MAX_PATH) ||
        !ResolveShaderPath(L"ShadowMap.hlsl", shadowPath, MAX_PATH) ||
        !ResolveShaderPath(L"GBufferDebug.hlsl", debugPath, MAX_PATH) ||
        !ResolveShaderPath(L"ParticleDust.hlsl", particlePath, MAX_PATH) ||
        !ResolveShaderPath(L"FireParticles.hlsl", firePath, MAX_PATH) ||
        !ResolveShaderPath(L"Water.hlsl", waterPath, MAX_PATH))
    {
        return false;
    }

    HRESULT hr = D3DCompileFromFile(
        geometryPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain",
        "vs_5_0",
        flags,
        0,
        &m_deferredGeometryVS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        geometryPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "HSMain",
        "hs_5_0",
        flags,
        0,
        &m_deferredGeometryHS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        geometryPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "DSMain",
        "ds_5_0",
        flags,
        0,
        &m_deferredGeometryDS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        geometryPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain",
        "ps_5_0",
        flags,
        0,
        &m_deferredGeometryPS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        lightingPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain",
        "vs_5_0",
        flags,
        0,
        &m_deferredLightingVS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        lightingPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain",
        "ps_5_0",
        flags,
        0,
        &m_deferredLightingPS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        postProcessPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain",
        "vs_5_0",
        flags,
        0,
        &m_postProcessVS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        postProcessPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain",
        "ps_5_0",
        flags,
        0,
        &m_postProcessPS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        shadowPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain",
        "vs_5_0",
        flags,
        0,
        &m_shadowVS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        shadowPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "HSMain",
        "hs_5_0",
        flags,
        0,
        &m_shadowHS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        shadowPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "DSMain",
        "ds_5_0",
        flags,
        0,
        &m_shadowDS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        debugPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain",
        "vs_5_0",
        flags,
        0,
        &m_debugOverlayVS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        debugPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain",
        "ps_5_0",
        flags,
        0,
        &m_debugOverlayPS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        particlePath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain",
        "vs_5_0",
        flags,
        0,
        &m_particleVS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        particlePath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "GSMain",
        "gs_5_0",
        flags,
        0,
        &m_particleGS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        particlePath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain",
        "ps_5_0",
        flags,
        0,
        &m_particlePS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        particlePath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "CSMain",
        "cs_5_0",
        flags,
        0,
        &m_particleCS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        firePath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain",
        "vs_5_0",
        flags,
        0,
        &m_fireVS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        firePath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "GSMain",
        "gs_5_0",
        flags,
        0,
        &m_fireGS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        firePath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain",
        "ps_5_0",
        flags,
        0,
        &m_firePS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        firePath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "CSMain",
        "cs_5_0",
        flags,
        0,
        &m_fireCS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        waterPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain",
        "vs_5_0",
        flags,
        0,
        &m_waterVS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        waterPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "HSMain",
        "hs_5_0",
        flags,
        0,
        &m_waterHS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        waterPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "DSMain",
        "ds_5_0",
        flags,
        0,
        &m_waterDS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        waterPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain",
        "ps_5_0",
        flags,
        0,
        &m_waterPS,
        nullptr);
    return SUCCEEDED(hr);
}

// карта теней
bool RenderingSystem::CreateShadowResources()
{
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = ShadowMapResolution;
    desc.Height = ShadowMapResolution;
    desc.DepthOrArraySize = ShadowCascadeCount;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    if (FAILED(m_context.GetDevice()->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearValue,
        IID_PPV_ARGS(&m_shadowMap))))
    {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.NumDescriptors = ShadowCascadeCount;
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(m_context.GetDevice()->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_shadowDsvHeap))))
    {
        return false;
    }

    m_shadowDsvDescriptorSize = m_context.GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    for (UINT cascadeIndex = 0; cascadeIndex < ShadowCascadeCount; ++cascadeIndex)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc{};
        viewDesc.Format = DXGI_FORMAT_D32_FLOAT;
        viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        viewDesc.Texture2DArray.FirstArraySlice = cascadeIndex;
        viewDesc.Texture2DArray.ArraySize = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(cascadeIndex) * SIZE_T(m_shadowDsvDescriptorSize);
        m_context.GetDevice()->CreateDepthStencilView(m_shadowMap.Get(), &viewDesc, handle);
    }

    return true;
}

bool RenderingSystem::CreateLightingSrvHeap()
{
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.NumDescriptors = kLightingSrvCount;
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_context.GetDevice()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_lightingSrvHeap))))
    {
        return false;
    }

    m_lightingSrvDescriptorSize = m_context.GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (UINT slotIndex = 0; slotIndex < GBuffer::TargetCount; ++slotIndex)
    {
        const GBuffer::Slot slot = static_cast<GBuffer::Slot>(slotIndex);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = m_gbuffer.GetFormat(slot);
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_lightingSrvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(slotIndex) * SIZE_T(m_lightingSrvDescriptorSize);
        m_context.GetDevice()->CreateShaderResourceView(m_gbuffer.GetResource(slot), &srvDesc, handle);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc{};
    shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    shadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    shadowSrvDesc.Texture2DArray.MipLevels = 1;
    shadowSrvDesc.Texture2DArray.ArraySize = ShadowCascadeCount;

    D3D12_CPU_DESCRIPTOR_HANDLE shadowHandle = m_lightingSrvHeap->GetCPUDescriptorHandleForHeapStart();
    shadowHandle.ptr += SIZE_T(kLightingShadowMapSrv) * SIZE_T(m_lightingSrvDescriptorSize);
    m_context.GetDevice()->CreateShaderResourceView(m_shadowMap.Get(), &shadowSrvDesc, shadowHandle);

    if (m_shadowMaskTexture != nullptr)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC maskSrvDesc{};
        maskSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        maskSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        maskSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        maskSrvDesc.Texture2D.MipLevels = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE maskHandle = m_lightingSrvHeap->GetCPUDescriptorHandleForHeapStart();
        maskHandle.ptr += SIZE_T(kLightingShadowMaskSrv) * SIZE_T(m_lightingSrvDescriptorSize);
        m_context.GetDevice()->CreateShaderResourceView(m_shadowMaskTexture.Get(), &maskSrvDesc, maskHandle);
    }

    if (m_irradianceMap != nullptr)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC irradianceSrvDesc{};
        irradianceSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        irradianceSrvDesc.Format = m_irradianceMap->GetDesc().Format;
        irradianceSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        irradianceSrvDesc.TextureCube.MostDetailedMip = 0;
        irradianceSrvDesc.TextureCube.MipLevels = m_irradianceMap->GetDesc().MipLevels;

        D3D12_CPU_DESCRIPTOR_HANDLE irradianceHandle = m_lightingSrvHeap->GetCPUDescriptorHandleForHeapStart();
        irradianceHandle.ptr += SIZE_T(kLightingIrradianceSrv) * SIZE_T(m_lightingSrvDescriptorSize);
        m_context.GetDevice()->CreateShaderResourceView(m_irradianceMap.Get(), &irradianceSrvDesc, irradianceHandle);
    }

    if (m_brdfIntegrationMap != nullptr)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC brdfSrvDesc{};
        brdfSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        brdfSrvDesc.Format = m_brdfIntegrationMap->GetDesc().Format;
        brdfSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        brdfSrvDesc.Texture2D.MostDetailedMip = 0;
        brdfSrvDesc.Texture2D.MipLevels = m_brdfIntegrationMap->GetDesc().MipLevels;

        D3D12_CPU_DESCRIPTOR_HANDLE brdfHandle = m_lightingSrvHeap->GetCPUDescriptorHandleForHeapStart();
        brdfHandle.ptr += SIZE_T(kLightingBrdfSrv) * SIZE_T(m_lightingSrvDescriptorSize);
        m_context.GetDevice()->CreateShaderResourceView(m_brdfIntegrationMap.Get(), &brdfSrvDesc, brdfHandle);
    }

    if (m_prefilteredEnvMap != nullptr)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC prefilterSrvDesc{};
        prefilterSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        prefilterSrvDesc.Format = m_prefilteredEnvMap->GetDesc().Format;
        prefilterSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        prefilterSrvDesc.TextureCube.MostDetailedMip = 0;
        prefilterSrvDesc.TextureCube.MipLevels = m_prefilteredEnvMap->GetDesc().MipLevels;

        D3D12_CPU_DESCRIPTOR_HANDLE prefilterHandle = m_lightingSrvHeap->GetCPUDescriptorHandleForHeapStart();
        prefilterHandle.ptr += SIZE_T(kLightingPrefilterSrv) * SIZE_T(m_lightingSrvDescriptorSize);
        m_context.GetDevice()->CreateShaderResourceView(m_prefilteredEnvMap.Get(), &prefilterSrvDesc, prefilterHandle);
    }

    return true;
}

// HDR
bool RenderingSystem::CreateHdrResources()
{
    ID3D12Device* device = m_context.GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = 1;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_hdrRtvHeap))))
    {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.NumDescriptors = 1;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_postProcessSrvHeap))))
    {
        return false;
    }

    m_hdrRtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = desc.Format;

    if (FAILED(device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clearValue,
        IID_PPV_ARGS(&m_hdrColorBuffer))))
    {
        return false;
    }

    device->CreateRenderTargetView(
        m_hdrColorBuffer.Get(),
        nullptr,
        m_hdrRtvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC hdrSrvDesc{};
    hdrSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    hdrSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    hdrSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    hdrSrvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(
        m_hdrColorBuffer.Get(),
        &hdrSrvDesc,
        m_postProcessSrvHeap->GetCPUDescriptorHandleForHeapStart());

    m_hdrColorBufferState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return true;
}

bool RenderingSystem::CreateIrradianceMapResource()
{
    ID3D12Device* device = m_context.GetDevice();
    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    if (device == nullptr || commandList == nullptr)
    {
        return false;
    }

    wchar_t texturePathW[MAX_PATH];
    if (!ResolveAssetPath(L"Stuff\\IrradianceMap_BC6U.dds", texturePathW, MAX_PATH))
    {
        return false;
    }

    std::ifstream file(texturePathW, std::ios::binary | std::ios::ate);
    if (!file)
    {
        return false;
    }

    const std::streamsize fileSize = file.tellg();
    if (fileSize <= 0)
    {
        return false;
    }

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> ddsData(static_cast<size_t>(fileSize));
    if (!file.read(reinterpret_cast<char*>(ddsData.data()), fileSize))
    {
        return false;
    }

    if (ddsData.size() < sizeof(UINT) + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10))
    {
        return false;
    }

    const UINT magic = *reinterpret_cast<const UINT*>(ddsData.data());
    if (magic != DDS_MAGIC)
    {
        return false;
    }

    const DDS_HEADER* header = reinterpret_cast<const DDS_HEADER*>(ddsData.data() + sizeof(UINT));
    if (header->size != sizeof(DDS_HEADER) || header->ddspf.size != sizeof(DDS_PIXELFORMAT))
    {
        return false;
    }

    if ((header->ddspf.flags & DDS_FOURCC) == 0 || header->ddspf.fourCC != '01XD')
    {
        return false;
    }

    const DDS_HEADER_DXT10* dxt10Header =
        reinterpret_cast<const DDS_HEADER_DXT10*>(ddsData.data() + sizeof(UINT) + sizeof(DDS_HEADER));

    if (dxt10Header->resourceDimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        (dxt10Header->miscFlag & DDS_RESOURCE_MISC_TEXTURECUBE) == 0 ||
        (header->caps2 & DDSCAPS2_CUBEMAP) == 0 ||
        (header->caps2 & DDSCAPS2_CUBEMAP_ALLFACES) != DDSCAPS2_CUBEMAP_ALLFACES)
    {
        return false;
    }

    UINT arraySize = dxt10Header->arraySize;
    if (arraySize == 0)
    {
        return false;
    }

    if (arraySize == 6)
    {
        arraySize = 1;
    }

    const UINT mipLevels = header->mipMapCount > 0 ? header->mipMapCount : 1u;
    const UINT faceCount = arraySize * 6u;
    const DXGI_FORMAT format = dxt10Header->dxgiFormat;
    if (BitsPerPixel(format) == 0)
    {
        return false;
    }

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = header->width;
    textureDesc.Height = header->height;
    textureDesc.DepthOrArraySize = static_cast<UINT16>(faceCount);
    textureDesc.MipLevels = static_cast<UINT16>(mipLevels);
    textureDesc.Format = format;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_irradianceMap))))
    {
        return false;
    }

    const UINT subresourceCount = faceCount * mipLevels;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresourceCount);
    std::vector<UINT> numRows(subresourceCount);
    std::vector<UINT64> rowSizes(subresourceCount);
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(
        &textureDesc,
        0,
        subresourceCount,
        0,
        footprints.data(),
        numRows.data(),
        rowSizes.data(),
        &uploadSize);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_irradianceMapUpload))))
    {
        return false;
    }

    void* mappedData = nullptr;
    if (FAILED(m_irradianceMapUpload->Map(0, nullptr, &mappedData)) || mappedData == nullptr)
    {
        return false;
    }

    size_t dataOffset = sizeof(UINT) + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10);
    for (UINT face = 0; face < faceCount; ++face)
    {
        size_t width = header->width;
        size_t height = header->height;

        for (UINT mip = 0; mip < mipLevels; ++mip)
        {
            const UINT subresourceIndex = face * mipLevels + mip;
            size_t srcNumBytes = 0;
            size_t srcRowBytes = 0;
            size_t srcNumRows = 0;
            GetSurfaceInfo(width, height, format, srcNumBytes, srcRowBytes, srcNumRows);
            if (srcNumBytes == 0 || dataOffset + srcNumBytes > ddsData.size())
            {
                m_irradianceMapUpload->Unmap(0, nullptr);
                return false;
            }

            auto* dstSlice = static_cast<uint8_t*>(mappedData) + footprints[subresourceIndex].Offset;
            const uint8_t* srcSlice = ddsData.data() + dataOffset;

            for (size_t row = 0; row < srcNumRows; ++row)
            {
                memcpy(
                    dstSlice + row * footprints[subresourceIndex].Footprint.RowPitch,
                    srcSlice + row * srcRowBytes,
                    srcRowBytes);
            }

            dataOffset += srcNumBytes;
            width = (std::max<size_t>)(1u, width >> 1u);
            height = (std::max<size_t>)(1u, height >> 1u);
        }
    }

    m_irradianceMapUpload->Unmap(0, nullptr);

    for (UINT subresourceIndex = 0; subresourceIndex < subresourceCount; ++subresourceIndex)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = m_irradianceMap.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = subresourceIndex;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = m_irradianceMapUpload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[subresourceIndex];

        commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_irradianceMap.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &barrier);

    return true;
}

bool RenderingSystem::CreateBrdfIntegrationMapResource()
{
    ID3D12Device* device = m_context.GetDevice();
    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    if (device == nullptr || commandList == nullptr)
    {
        return false;
    }

    wchar_t texturePathW[MAX_PATH];
    if (!ResolveAssetPath(L"Stuff\\IntegrationMap.dds", texturePathW, MAX_PATH))
    {
        return false;
    }

    std::ifstream file(texturePathW, std::ios::binary | std::ios::ate);
    if (!file)
    {
        return false;
    }

    const std::streamsize fileSize = file.tellg();
    if (fileSize <= 0)
    {
        return false;
    }

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> ddsData(static_cast<size_t>(fileSize));
    if (!file.read(reinterpret_cast<char*>(ddsData.data()), fileSize))
    {
        return false;
    }

    if (ddsData.size() < sizeof(UINT) + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10))
    {
        return false;
    }

    const UINT magic = *reinterpret_cast<const UINT*>(ddsData.data());
    if (magic != DDS_MAGIC)
    {
        return false;
    }

    const DDS_HEADER* header = reinterpret_cast<const DDS_HEADER*>(ddsData.data() + sizeof(UINT));
    if (header->size != sizeof(DDS_HEADER) || header->ddspf.size != sizeof(DDS_PIXELFORMAT))
    {
        return false;
    }

    if ((header->ddspf.flags & DDS_FOURCC) == 0 || header->ddspf.fourCC != '01XD')
    {
        return false;
    }

    const DDS_HEADER_DXT10* dxt10Header =
        reinterpret_cast<const DDS_HEADER_DXT10*>(ddsData.data() + sizeof(UINT) + sizeof(DDS_HEADER));

    if (dxt10Header->resourceDimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        (dxt10Header->miscFlag & DDS_RESOURCE_MISC_TEXTURECUBE) != 0 ||
        dxt10Header->arraySize != 1)
    {
        return false;
    }

    const UINT mipLevels = header->mipMapCount > 0 ? header->mipMapCount : 1u;
    const DXGI_FORMAT format = dxt10Header->dxgiFormat;
    const UINT bytesPerPixel = 8u; // IntegrationMap.dds is R32G32_FLOAT.
    if (format != DXGI_FORMAT_R32G32_FLOAT)
    {
        return false;
    }

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = header->width;
    textureDesc.Height = header->height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = static_cast<UINT16>(mipLevels);
    textureDesc.Format = format;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_brdfIntegrationMap))))
    {
        return false;
    }

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipLevels);
    std::vector<UINT> numRows(mipLevels);
    std::vector<UINT64> rowSizes(mipLevels);
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(
        &textureDesc,
        0,
        mipLevels,
        0,
        footprints.data(),
        numRows.data(),
        rowSizes.data(),
        &uploadSize);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_brdfIntegrationMapUpload))))
    {
        return false;
    }

    void* mappedData = nullptr;
    if (FAILED(m_brdfIntegrationMapUpload->Map(0, nullptr, &mappedData)) || mappedData == nullptr)
    {
        return false;
    }

    size_t dataOffset = sizeof(UINT) + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10);
    size_t width = header->width;
    size_t height = header->height;
    for (UINT mip = 0; mip < mipLevels; ++mip)
    {
        const size_t srcRowBytes = width * bytesPerPixel;
        const size_t srcNumRows = height;
        const size_t srcNumBytes = srcRowBytes * srcNumRows;
        if (dataOffset + srcNumBytes > ddsData.size())
        {
            m_brdfIntegrationMapUpload->Unmap(0, nullptr);
            return false;
        }

        auto* dstSlice = static_cast<uint8_t*>(mappedData) + footprints[mip].Offset;
        const uint8_t* srcSlice = ddsData.data() + dataOffset;

        for (size_t row = 0; row < srcNumRows; ++row)
        {
            memcpy(
                dstSlice + row * footprints[mip].Footprint.RowPitch,
                srcSlice + row * srcRowBytes,
                srcRowBytes);
        }

        dataOffset += srcNumBytes;
        width = (std::max<size_t>)(1u, width >> 1u);
        height = (std::max<size_t>)(1u, height >> 1u);
    }

    m_brdfIntegrationMapUpload->Unmap(0, nullptr);

    for (UINT mip = 0; mip < mipLevels; ++mip)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = m_brdfIntegrationMap.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = mip;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = m_brdfIntegrationMapUpload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[mip];

        commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_brdfIntegrationMap.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &barrier);

    return true;
}

bool RenderingSystem::CreatePrefilteredEnvMapResource()
{
    ID3D12Device* device = m_context.GetDevice();
    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    if (device == nullptr || commandList == nullptr)
    {
        return false;
    }

    wchar_t texturePathW[MAX_PATH];
    if (!ResolveAssetPath(L"Stuff\\PreFilteredEnvMap_BC6U.dds", texturePathW, MAX_PATH))
    {
        return false;
    }

    std::ifstream file(texturePathW, std::ios::binary | std::ios::ate);
    if (!file)
    {
        return false;
    }

    const std::streamsize fileSize = file.tellg();
    if (fileSize <= 0)
    {
        return false;
    }

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> ddsData(static_cast<size_t>(fileSize));
    if (!file.read(reinterpret_cast<char*>(ddsData.data()), fileSize))
    {
        return false;
    }

    if (ddsData.size() < sizeof(UINT) + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10))
    {
        return false;
    }

    const UINT magic = *reinterpret_cast<const UINT*>(ddsData.data());
    if (magic != DDS_MAGIC)
    {
        return false;
    }

    const DDS_HEADER* header = reinterpret_cast<const DDS_HEADER*>(ddsData.data() + sizeof(UINT));
    if (header->size != sizeof(DDS_HEADER) || header->ddspf.size != sizeof(DDS_PIXELFORMAT))
    {
        return false;
    }

    if ((header->ddspf.flags & DDS_FOURCC) == 0 || header->ddspf.fourCC != '01XD')
    {
        return false;
    }

    const DDS_HEADER_DXT10* dxt10Header =
        reinterpret_cast<const DDS_HEADER_DXT10*>(ddsData.data() + sizeof(UINT) + sizeof(DDS_HEADER));

    if (dxt10Header->resourceDimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        (dxt10Header->miscFlag & DDS_RESOURCE_MISC_TEXTURECUBE) == 0 ||
        (header->caps2 & DDSCAPS2_CUBEMAP) == 0 ||
        (header->caps2 & DDSCAPS2_CUBEMAP_ALLFACES) != DDSCAPS2_CUBEMAP_ALLFACES)
    {
        return false;
    }

    UINT arraySize = dxt10Header->arraySize;
    if (arraySize == 0)
    {
        return false;
    }

    if (arraySize == 6)
    {
        arraySize = 1;
    }

    const UINT mipLevels = header->mipMapCount > 0 ? header->mipMapCount : 1u;
    const UINT faceCount = arraySize * 6u;
    const DXGI_FORMAT format = dxt10Header->dxgiFormat;
    if (BitsPerPixel(format) == 0)
    {
        return false;
    }

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = header->width;
    textureDesc.Height = header->height;
    textureDesc.DepthOrArraySize = static_cast<UINT16>(faceCount);
    textureDesc.MipLevels = static_cast<UINT16>(mipLevels);
    textureDesc.Format = format;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_prefilteredEnvMap))))
    {
        return false;
    }

    const UINT subresourceCount = faceCount * mipLevels;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresourceCount);
    std::vector<UINT> numRows(subresourceCount);
    std::vector<UINT64> rowSizes(subresourceCount);
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(
        &textureDesc,
        0,
        subresourceCount,
        0,
        footprints.data(),
        numRows.data(),
        rowSizes.data(),
        &uploadSize);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_prefilteredEnvMapUpload))))
    {
        return false;
    }

    void* mappedData = nullptr;
    if (FAILED(m_prefilteredEnvMapUpload->Map(0, nullptr, &mappedData)) || mappedData == nullptr)
    {
        return false;
    }

    size_t dataOffset = sizeof(UINT) + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10);
    for (UINT face = 0; face < faceCount; ++face)
    {
        size_t width = header->width;
        size_t height = header->height;

        for (UINT mip = 0; mip < mipLevels; ++mip)
        {
            const UINT subresourceIndex = face * mipLevels + mip;
            size_t srcNumBytes = 0;
            size_t srcRowBytes = 0;
            size_t srcNumRows = 0;
            GetSurfaceInfo(width, height, format, srcNumBytes, srcRowBytes, srcNumRows);
            if (srcNumBytes == 0 || dataOffset + srcNumBytes > ddsData.size())
            {
                m_prefilteredEnvMapUpload->Unmap(0, nullptr);
                return false;
            }

            auto* dstSlice = static_cast<uint8_t*>(mappedData) + footprints[subresourceIndex].Offset;
            const uint8_t* srcSlice = ddsData.data() + dataOffset;

            for (size_t row = 0; row < srcNumRows; ++row)
            {
                memcpy(
                    dstSlice + row * footprints[subresourceIndex].Footprint.RowPitch,
                    srcSlice + row * srcRowBytes,
                    srcRowBytes);
            }

            dataOffset += srcNumBytes;
            width = (std::max<size_t>)(1u, width >> 1u);
            height = (std::max<size_t>)(1u, height >> 1u);
        }
    }

    m_prefilteredEnvMapUpload->Unmap(0, nullptr);

    for (UINT subresourceIndex = 0; subresourceIndex < subresourceCount; ++subresourceIndex)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = m_prefilteredEnvMap.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = subresourceIndex;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = m_prefilteredEnvMapUpload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[subresourceIndex];

        commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_prefilteredEnvMap.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &barrier);

    return true;
}

bool RenderingSystem::CreateShadowMaskTexture()
{
    ID3D12Device* device = m_context.GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    wchar_t texturePathW[MAX_PATH];
    if (!ResolveAssetPath(L"models\\textures\\design.png", texturePathW, MAX_PATH))
    {
        return false;
    }

    char texturePathA[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, texturePathW, -1, texturePathA, MAX_PATH, nullptr, nullptr);

    int width = 0;
    int height = 0;
    int comp = 0;
    stbi_uc* pixels = stbi_load(texturePathA, &width, &height, &comp, 4);
    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        return false;
    }

    const UINT srcRowPitch = static_cast<UINT>(width) * 4u;
    const UINT alignedRowPitch =
        (srcRowPitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    const UINT64 uploadSize = UINT64(alignedRowPitch) * UINT64(height);

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = static_cast<UINT>(width);
    textureDesc.Height = static_cast<UINT>(height);
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    if (FAILED(device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_shadowMaskTexture))))
    {
        stbi_image_free(pixels);
        return false;
    }

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_shadowMaskTextureUpload))))
    {
        stbi_image_free(pixels);
        return false;
    }

    void* mappedData = nullptr;
    if (FAILED(m_shadowMaskTextureUpload->Map(0, nullptr, &mappedData)) || mappedData == nullptr)
    {
        stbi_image_free(pixels);
        return false;
    }

    auto* dstBytes = static_cast<stbi_uc*>(mappedData);
    for (int y = 0; y < height; ++y)
    {
        memcpy(dstBytes + SIZE_T(y) * SIZE_T(alignedRowPitch), pixels + SIZE_T(y) * SIZE_T(srcRowPitch), srcRowPitch);
    }
    m_shadowMaskTextureUpload->Unmap(0, nullptr);
    stbi_image_free(pixels);

    m_shadowMaskUploaded = false;
    return true;
}

bool RenderingSystem::CreateShadowRootSignature()
{
    D3D12_DESCRIPTOR_RANGE displacementRange{};
    displacementRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    displacementRange.NumDescriptors = 1;
    displacementRange.BaseShaderRegister = 0;
    displacementRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &displacementRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_DOMAIN;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_DOMAIN;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(rootParams);
    desc.pParameters = rootParams;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &error);
    if (FAILED(hr))
    {
        return false;
    }

    return SUCCEEDED(m_context.GetDevice()->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_shadowRootSignature)));
}

bool RenderingSystem::CreateShadowPipeline()
{
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = { layout, _countof(layout) };
    pso.pRootSignature = m_shadowRootSignature.Get();
    pso.VS = { m_shadowVS->GetBufferPointer(), m_shadowVS->GetBufferSize() };
    pso.HS = { m_shadowHS->GetBufferPointer(), m_shadowHS->GetBufferSize() };
    pso.DS = { m_shadowDS->GetBufferPointer(), m_shadowDS->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.DepthBias = 240;
    pso.RasterizerState.SlopeScaledDepthBias = 1.0f;
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.NumRenderTargets = 0;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;

    return SUCCEEDED(m_context.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_shadowPSO)));
}

// что может читать шейдер
bool RenderingSystem::CreateDeferredLightingRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = kLightingSrvCount;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[0].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC shadowSampler{};
    shadowSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    shadowSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    shadowSampler.MaxLOD = D3D12_FLOAT32_MAX;
    shadowSampler.ShaderRegister = 0;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC maskSampler{};
    maskSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    maskSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    maskSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    maskSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    maskSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    maskSampler.MaxLOD = D3D12_FLOAT32_MAX;
    maskSampler.ShaderRegister = 1;
    maskSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 2;
    desc.pParameters = rootParams;
    D3D12_STATIC_SAMPLER_DESC samplers[] = { shadowSampler, maskSampler };
    desc.NumStaticSamplers = _countof(samplers);
    desc.pStaticSamplers = samplers;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &error);
    if (FAILED(hr))
    {
        return false;
    }

    return SUCCEEDED(m_context.GetDevice()->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_deferredLightingRootSignature)));
}

bool RenderingSystem::CreatePostProcessRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParam.DescriptorTable.NumDescriptorRanges = 1;
    rootParam.DescriptorTable.pDescriptorRanges = &srvRange;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1;
    desc.pParameters = &rootParam;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &error);
    if (FAILED(hr))
    {
        return false;
    }

    return SUCCEEDED(m_context.GetDevice()->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_postProcessRootSignature)));
}

bool RenderingSystem::CreateDebugOverlayRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = GBuffer::TargetCount;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[0].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[1].Constants.ShaderRegister = 0;
    rootParams[1].Constants.Num32BitValues = sizeof(DebugOverlayConstants) / sizeof(UINT);
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(rootParams);
    desc.pParameters = rootParams;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &error);
    if (FAILED(hr))
    {
        return false;
    }

    return SUCCEEDED(m_context.GetDevice()->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_debugOverlayRootSignature)));
}

bool RenderingSystem::CreateParticleRootSignature()
{
    D3D12_DESCRIPTOR_RANGE particleBufferRange{};
    particleBufferRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    particleBufferRange.NumDescriptors = 1;
    particleBufferRange.BaseShaderRegister = 0;
    particleBufferRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE particleTextureRange{};
    particleTextureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    particleTextureRange.NumDescriptors = 1;
    particleTextureRange.BaseShaderRegister = 1;
    particleTextureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER graphicsRootParams[3]{};
    graphicsRootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    graphicsRootParams[0].Descriptor.ShaderRegister = 1;
    graphicsRootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    graphicsRootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsRootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    graphicsRootParams[1].DescriptorTable.pDescriptorRanges = &particleBufferRange;
    graphicsRootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    graphicsRootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsRootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    graphicsRootParams[2].DescriptorTable.pDescriptorRanges = &particleTextureRange;
    graphicsRootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC graphicsDesc{};
    graphicsDesc.NumParameters = _countof(graphicsRootParams);
    graphicsDesc.pParameters = graphicsRootParams;
    graphicsDesc.NumStaticSamplers = 1;
    graphicsDesc.pStaticSamplers = &sampler;
    graphicsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &graphicsDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &error);
    if (FAILED(hr))
    {
        return false;
    }

    if (FAILED(m_context.GetDevice()->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_particleGraphicsRootSignature))))
    {
        return false;
    }

    D3D12_DESCRIPTOR_RANGE consumeRange{};
    consumeRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    consumeRange.NumDescriptors = 1;
    consumeRange.BaseShaderRegister = 0;
    consumeRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE appendRange{};
    appendRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    appendRange.NumDescriptors = 1;
    appendRange.BaseShaderRegister = 1;
    appendRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER computeRootParams[3]{};
    computeRootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    computeRootParams[0].Descriptor.ShaderRegister = 0;
    computeRootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeRootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeRootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    computeRootParams[1].DescriptorTable.pDescriptorRanges = &consumeRange;
    computeRootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeRootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeRootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    computeRootParams[2].DescriptorTable.pDescriptorRanges = &appendRange;
    computeRootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC computeDesc{};
    computeDesc.NumParameters = _countof(computeRootParams);
    computeDesc.pParameters = computeRootParams;

    serialized.Reset();
    error.Reset();
    hr = D3D12SerializeRootSignature(
        &computeDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &error);
    if (FAILED(hr))
    {
        return false;
    }

    return SUCCEEDED(m_context.GetDevice()->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_particleComputeRootSignature)));
}

bool RenderingSystem::CreateFireRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER graphicsRootParams[2]{};
    graphicsRootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    graphicsRootParams[0].Descriptor.ShaderRegister = 1;
    graphicsRootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    graphicsRootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsRootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    graphicsRootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    graphicsRootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC graphicsDesc{};
    graphicsDesc.NumParameters = _countof(graphicsRootParams);
    graphicsDesc.pParameters = graphicsRootParams;
    graphicsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &graphicsDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &error);
    if (FAILED(hr))
    {
        return false;
    }

    if (FAILED(m_context.GetDevice()->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_fireGraphicsRootSignature))))
    {
        return false;
    }

    D3D12_DESCRIPTOR_RANGE consumeRange{};
    consumeRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    consumeRange.NumDescriptors = 1;
    consumeRange.BaseShaderRegister = 0;
    consumeRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE appendRange{};
    appendRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    appendRange.NumDescriptors = 1;
    appendRange.BaseShaderRegister = 1;
    appendRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER computeRootParams[3]{};
    computeRootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    computeRootParams[0].Descriptor.ShaderRegister = 0;
    computeRootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeRootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeRootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    computeRootParams[1].DescriptorTable.pDescriptorRanges = &consumeRange;
    computeRootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeRootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeRootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    computeRootParams[2].DescriptorTable.pDescriptorRanges = &appendRange;
    computeRootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC computeDesc{};
    computeDesc.NumParameters = _countof(computeRootParams);
    computeDesc.pParameters = computeRootParams;

    serialized.Reset();
    error.Reset();
    hr = D3D12SerializeRootSignature(
        &computeDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &error);
    if (FAILED(hr))
    {
        return false;
    }

    return SUCCEEDED(m_context.GetDevice()->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_fireComputeRootSignature)));
}

bool RenderingSystem::CreateWaterRootSignature()
{
    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParam.Descriptor.ShaderRegister = 0;
    rootParam.Descriptor.RegisterSpace = 0;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1;
    desc.pParameters = &rootParam;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &error);
    if (FAILED(hr))
    {
        return false;
    }

    return SUCCEEDED(m_context.GetDevice()->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_waterRootSignature)));
}


bool RenderingSystem::CreateDeferredGeometryPipeline()
{
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = { layout, _countof(layout) };
    pso.pRootSignature = m_context.GetSceneRootSignature();
    pso.VS = { m_deferredGeometryVS->GetBufferPointer(), m_deferredGeometryVS->GetBufferSize() };
    pso.HS = { m_deferredGeometryHS->GetBufferPointer(), m_deferredGeometryHS->GetBufferSize() };
    pso.DS = { m_deferredGeometryDS->GetBufferPointer(), m_deferredGeometryDS->GetBufferSize() };
    pso.PS = { m_deferredGeometryPS->GetBufferPointer(), m_deferredGeometryPS->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = TRUE;

    for (UINT i = 0; i < 8; ++i)
    {
        pso.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilEnable = FALSE;

    pso.NumRenderTargets = GBuffer::TargetCount;
    pso.RTVFormats[0] = m_gbuffer.GetFormat(GBuffer::Slot::AlbedoSpec);
    pso.RTVFormats[1] = m_gbuffer.GetFormat(GBuffer::Slot::WorldPosition);
    pso.RTVFormats[2] = m_gbuffer.GetFormat(GBuffer::Slot::Normal);
    pso.RTVFormats[3] = m_gbuffer.GetFormat(GBuffer::Slot::Depth);
    pso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pso.SampleDesc.Count = 1;

    return SUCCEEDED(m_context.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_deferredGeometryPSO)));
}

// считывание освещения из буфера
bool RenderingSystem::CreateDeferredLightingPipeline()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_deferredLightingRootSignature.Get();
    pso.VS = { m_deferredLightingVS->GetBufferPointer(), m_deferredLightingVS->GetBufferSize() };
    pso.PS = { m_deferredLightingPS->GetBufferPointer(), m_deferredLightingPS->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;

    return SUCCEEDED(m_context.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_deferredLightingPSO)));
}

bool RenderingSystem::CreatePostProcessPipeline()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_postProcessRootSignature.Get();
    pso.VS = { m_postProcessVS->GetBufferPointer(), m_postProcessVS->GetBufferSize() };
    pso.PS = { m_postProcessPS->GetBufferPointer(), m_postProcessPS->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.SampleDesc.Count = 1;

    return SUCCEEDED(m_context.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_postProcessPSO)));
}

bool RenderingSystem::CreateDebugOverlayPipeline()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_debugOverlayRootSignature.Get();
    pso.VS = { m_debugOverlayVS->GetBufferPointer(), m_debugOverlayVS->GetBufferSize() };
    pso.PS = { m_debugOverlayPS->GetBufferPointer(), m_debugOverlayPS->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;

    return SUCCEEDED(m_context.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_debugOverlayPSO)));
}

bool RenderingSystem::CreateParticleSimulationPipeline()
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_particleComputeRootSignature.Get();
    desc.CS = { m_particleCS->GetBufferPointer(), m_particleCS->GetBufferSize() };

    return SUCCEEDED(m_context.GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_particleComputePSO)));
}

bool RenderingSystem::CreateParticleRenderPipeline()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_particleGraphicsRootSignature.Get();
    pso.VS = { m_particleVS->GetBufferPointer(), m_particleVS->GetBufferSize() };
    pso.GS = { m_particleGS->GetBufferPointer(), m_particleGS->GetBufferSize() };
    pso.PS = { m_particlePS->GetBufferPointer(), m_particlePS->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pso.SampleDesc.Count = 1;

    return SUCCEEDED(m_context.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_particleGraphicsPSO)));
}

bool RenderingSystem::CreateParticleResources()
{
    ID3D12Device* device = m_context.GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 5;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_particleHeap))))
    {
        return false;
    }
    m_particleDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    const UINT particleBufferSize = sizeof(DustParticleCPU) * MaxDustParticles;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC particleDesc{};
    particleDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    particleDesc.Width = particleBufferSize;
    particleDesc.Height = 1;
    particleDesc.DepthOrArraySize = 1;
    particleDesc.MipLevels = 1;
    particleDesc.SampleDesc.Count = 1;
    particleDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    particleDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (UINT i = 0; i < 2; ++i)
    {
        if (FAILED(device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &particleDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_particleBuffers[i]))))
        {
            return false;
        }
    }

    D3D12_RESOURCE_DESC counterDesc = particleDesc;
    counterDesc.Width = sizeof(UINT);

    for (UINT i = 0; i < 2; ++i)
    {
        if (FAILED(device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &counterDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_particleCounterBuffers[i]))))
        {
            return false;
        }
    }

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = particleBufferSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_particleUploadBuffer))))
    {
        return false;
    }

    uploadDesc.Width = sizeof(UINT) * 2;
    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_particleCounterUploadBuffer))))
    {
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_particleHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.NumElements = MaxDustParticles;
    srvDesc.Buffer.StructureByteStride = sizeof(DustParticleCPU);

    device->CreateShaderResourceView(m_particleBuffers[0].Get(), &srvDesc, cpuHandle);
    cpuHandle.ptr += SIZE_T(m_particleDescriptorSize);
    device->CreateShaderResourceView(m_particleBuffers[1].Get(), &srvDesc, cpuHandle);
    cpuHandle.ptr += SIZE_T(m_particleDescriptorSize);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = MaxDustParticles;
    uavDesc.Buffer.StructureByteStride = sizeof(DustParticleCPU);
    uavDesc.Buffer.CounterOffsetInBytes = 0;

    device->CreateUnorderedAccessView(
        m_particleBuffers[0].Get(),
        m_particleCounterBuffers[0].Get(),
        &uavDesc,
        cpuHandle);
    cpuHandle.ptr += SIZE_T(m_particleDescriptorSize);
    device->CreateUnorderedAccessView(
        m_particleBuffers[1].Get(),
        m_particleCounterBuffers[1].Get(),
        &uavDesc,
        cpuHandle);

    return true;
}

bool RenderingSystem::CreateParticleTexture()
{
    ID3D12Device* device = m_context.GetDevice();
    if (device == nullptr || m_particleHeap == nullptr)
    {
        return false;
    }

    wchar_t texturePathW[MAX_PATH];
    if (!ResolveAssetPath(L"models\\textures\\simply.png", texturePathW, MAX_PATH))
    {
        return false;
    }

    char texturePathA[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, texturePathW, -1, texturePathA, MAX_PATH, nullptr, nullptr);

    int width = 0;
    int height = 0;
    int comp = 0;
    stbi_uc* pixels = stbi_load(texturePathA, &width, &height, &comp, 4);
    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        return false;
    }

    const UINT srcRowPitch = static_cast<UINT>(width) * 4u;
    const UINT alignedRowPitch =
        (srcRowPitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    const UINT64 uploadSize = UINT64(alignedRowPitch) * UINT64(height);

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = static_cast<UINT>(width);
    textureDesc.Height = static_cast<UINT>(height);
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    if (FAILED(device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_particleTexture))))
    {
        stbi_image_free(pixels);
        return false;
    }

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_particleTextureUpload))))
    {
        stbi_image_free(pixels);
        return false;
    }

    void* mappedData = nullptr;
    if (FAILED(m_particleTextureUpload->Map(0, nullptr, &mappedData)) || mappedData == nullptr)
    {
        stbi_image_free(pixels);
        return false;
    }
    for (int y = 0; y < height; ++y)
    {
        std::memcpy(
            static_cast<unsigned char*>(mappedData) + static_cast<size_t>(alignedRowPitch) * static_cast<size_t>(y),
            pixels + static_cast<size_t>(srcRowPitch) * static_cast<size_t>(y),
            srcRowPitch);
    }
    m_particleTextureUpload->Unmap(0, nullptr);
    stbi_image_free(pixels);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_particleHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += SIZE_T(kParticleTextureSrv) * SIZE_T(m_particleDescriptorSize);
    device->CreateShaderResourceView(m_particleTexture.Get(), &srvDesc, cpuHandle);

    m_particleTextureUploaded = false;
    return true;
}

bool RenderingSystem::CreateParticleConstantBuffers()
{
    ID3D12Device* device = m_context.GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    const UINT simCbSize = (sizeof(ParticleSimCB) + 255) & ~255u;
    const UINT renderCbSize = (sizeof(ParticleRenderCB) + 255) & ~255u;

    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = simCbSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &upload,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_particleSimulationConstantBuffer))))
    {
        return false;
    }

    desc.Width = renderCbSize;
    if (FAILED(device->CreateCommittedResource(
        &upload,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_particleRenderConstantBuffer))))
    {
        return false;
    }

    D3D12_RANGE readRange{ 0, 0 };
    if (FAILED(m_particleSimulationConstantBuffer->Map(
        0,
        &readRange,
        reinterpret_cast<void**>(&m_particleSimulationCBMappedData))))
    {
        return false;
    }

    if (FAILED(m_particleRenderConstantBuffer->Map(
        0,
        &readRange,
        reinterpret_cast<void**>(&m_particleRenderCBMappedData))))
    {
        return false;
    }

    return true;
}

bool RenderingSystem::CreateFireSimulationPipeline()
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_fireComputeRootSignature.Get();
    desc.CS = { m_fireCS->GetBufferPointer(), m_fireCS->GetBufferSize() };

    return SUCCEEDED(m_context.GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_fireComputePSO)));
}

bool RenderingSystem::CreateFireRenderPipeline()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_fireGraphicsRootSignature.Get();
    pso.VS = { m_fireVS->GetBufferPointer(), m_fireVS->GetBufferSize() };
    pso.GS = { m_fireGS->GetBufferPointer(), m_fireGS->GetBufferSize() };
    pso.PS = { m_firePS->GetBufferPointer(), m_firePS->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pso.SampleDesc.Count = 1;

    return SUCCEEDED(m_context.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_fireGraphicsPSO)));
}

bool RenderingSystem::CreateFireResources()
{
    ID3D12Device* device = m_context.GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 4;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_fireHeap))))
    {
        return false;
    }
    m_fireDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    const UINT fireBufferSize = sizeof(FireParticleCPU) * MaxFireParticles;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC particleDesc{};
    particleDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    particleDesc.Width = fireBufferSize;
    particleDesc.Height = 1;
    particleDesc.DepthOrArraySize = 1;
    particleDesc.MipLevels = 1;
    particleDesc.SampleDesc.Count = 1;
    particleDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    particleDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (UINT i = 0; i < 2; ++i)
    {
        if (FAILED(device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &particleDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_fireBuffers[i]))))
        {
            return false;
        }
    }

    D3D12_RESOURCE_DESC counterDesc = particleDesc;
    counterDesc.Width = sizeof(UINT);

    for (UINT i = 0; i < 2; ++i)
    {
        if (FAILED(device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &counterDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_fireCounterBuffers[i]))))
        {
            return false;
        }
    }

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = fireBufferSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_fireUploadBuffer))))
    {
        return false;
    }

    uploadDesc.Width = sizeof(UINT) * 2;
    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_fireCounterUploadBuffer))))
    {
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_fireHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.NumElements = MaxFireParticles;
    srvDesc.Buffer.StructureByteStride = sizeof(FireParticleCPU);

    device->CreateShaderResourceView(m_fireBuffers[0].Get(), &srvDesc, cpuHandle);
    cpuHandle.ptr += SIZE_T(m_fireDescriptorSize);
    device->CreateShaderResourceView(m_fireBuffers[1].Get(), &srvDesc, cpuHandle);
    cpuHandle.ptr += SIZE_T(m_fireDescriptorSize);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = MaxFireParticles;
    uavDesc.Buffer.StructureByteStride = sizeof(FireParticleCPU);

    device->CreateUnorderedAccessView(m_fireBuffers[0].Get(), m_fireCounterBuffers[0].Get(), &uavDesc, cpuHandle);
    cpuHandle.ptr += SIZE_T(m_fireDescriptorSize);
    device->CreateUnorderedAccessView(m_fireBuffers[1].Get(), m_fireCounterBuffers[1].Get(), &uavDesc, cpuHandle);

    return true;
}

bool RenderingSystem::CreateFireConstantBuffers()
{
    ID3D12Device* device = m_context.GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    const UINT simCbSize = (sizeof(ParticleSimCB) + 255) & ~255u;
    const UINT renderCbSize = (sizeof(ParticleRenderCB) + 255) & ~255u;

    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = simCbSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &upload,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_fireSimulationConstantBuffer))))
    {
        return false;
    }

    desc.Width = renderCbSize;
    if (FAILED(device->CreateCommittedResource(
        &upload,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_fireRenderConstantBuffer))))
    {
        return false;
    }

    D3D12_RANGE readRange{ 0, 0 };
    if (FAILED(m_fireSimulationConstantBuffer->Map(
        0,
        &readRange,
        reinterpret_cast<void**>(&m_fireSimulationCBMappedData))))
    {
        return false;
    }

    if (FAILED(m_fireRenderConstantBuffer->Map(
        0,
        &readRange,
        reinterpret_cast<void**>(&m_fireRenderCBMappedData))))
    {
        return false;
    }

    return true;
}

bool RenderingSystem::CreateWaterPipeline()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_waterRootSignature.Get();
    pso.VS = { m_waterVS->GetBufferPointer(), m_waterVS->GetBufferSize() };
    pso.HS = { m_waterHS->GetBufferPointer(), m_waterHS->GetBufferSize() };
    pso.DS = { m_waterDS->GetBufferPointer(), m_waterDS->GetBufferSize() };
    pso.PS = { m_waterPS->GetBufferPointer(), m_waterPS->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pso.SampleDesc.Count = 1;

    return SUCCEEDED(m_context.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_waterPSO)));
}

void RenderingSystem::RenderGBufferDebugOverlay()
{
    if (!m_debugOverlayPSO || !m_debugOverlayRootSignature)
    {
        return;
    }

    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = m_context.GetCurrentBackBufferRTV();

    ID3D12DescriptorHeap* heaps[] = { m_gbuffer.GetSRVHeap() };
    commandList->OMSetRenderTargets(1, &backBufferRtv, TRUE, nullptr);
    commandList->SetPipelineState(m_debugOverlayPSO.Get());
    commandList->SetGraphicsRootSignature(m_debugOverlayRootSignature.Get());
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootDescriptorTable(0, m_gbuffer.GetSRVGPU(GBuffer::Slot::AlbedoSpec));
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const float width = static_cast<float>(m_width);
    const float height = static_cast<float>(m_height);
    const float padding = (std::max)(12.0f, width * 0.0125f);
    const float tileWidth = (std::max)(width * 0.22f, 180.0f);
    const float tileHeight = tileWidth * 0.58f;

    const XMFLOAT3 sceneCenter = m_context.GetSceneCenter();
    const XMFLOAT3 sceneExtents = m_context.GetSceneExtents();

    const std::array<D3D12_VIEWPORT, GBuffer::TargetCount> viewports =
    {
        D3D12_VIEWPORT{ padding, padding, tileWidth, tileHeight, 0.0f, 1.0f },
        D3D12_VIEWPORT{ width - padding - tileWidth, padding, tileWidth, tileHeight, 0.0f, 1.0f },
        D3D12_VIEWPORT{ padding, height - padding - tileHeight, tileWidth, tileHeight, 0.0f, 1.0f },
        D3D12_VIEWPORT{ width - padding - tileWidth, height - padding - tileHeight, tileWidth, tileHeight, 0.0f, 1.0f }
    };

    for (UINT i = 0; i < GBuffer::TargetCount; ++i)
    {
        const D3D12_VIEWPORT& viewport = viewports[i];
        D3D12_RECT scissor{};
        scissor.left = static_cast<LONG>(viewport.TopLeftX);
        scissor.top = static_cast<LONG>(viewport.TopLeftY);
        scissor.right = static_cast<LONG>(viewport.TopLeftX + viewport.Width);
        scissor.bottom = static_cast<LONG>(viewport.TopLeftY + viewport.Height);

        DebugOverlayConstants constants{};
        constants.OverlayRect = XMFLOAT4(
            viewport.TopLeftX,
            viewport.TopLeftY,
            viewport.Width,
            viewport.Height);
        constants.SceneCenter = XMFLOAT4(sceneCenter.x, sceneCenter.y, sceneCenter.z, 1.0f);
        constants.SceneExtents = XMFLOAT4(sceneExtents.x, sceneExtents.y, sceneExtents.z, 1.0f);
        constants.DebugMode = i;

        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        commandList->SetGraphicsRoot32BitConstants(
            1,
            sizeof(DebugOverlayConstants) / sizeof(UINT),
            &constants,
            0);
        commandList->DrawInstanced(6, 1, 0, 0);
    }
}

void RenderingSystem::InitializeParticleData()
{
    if (m_particleUploadBuffer == nullptr || m_particleCounterUploadBuffer == nullptr)
    {
        return;
    }

    std::vector<DustParticleCPU> particles(MaxDustParticles);
    const XMFLOAT3 sceneCenter = m_context.GetSceneCenter();
    const XMFLOAT3 sceneExtents = m_context.GetSceneExtents();

    const float minY = sceneCenter.y - sceneExtents.y * 0.18f;
    const float sphereRadius = 18.0f;
    const XMFLOAT3 sphereCenter(
        sceneCenter.x,
        minY + sphereRadius + 22.0f,
        sceneCenter.z);
    const XMFLOAT3 emitterPosition(
        sphereCenter.x,
        sphereCenter.y,
        sphereCenter.z);

    for (UINT i = 0; i < MaxDustParticles; ++i)
    {
        const float seed0 = Hash01(i * 17u + 11u);
        const float seed1 = Hash01(i * 29u + 7u);
        const float seed2 = Hash01(i * 43u + 19u);
        const float seed3 = Hash01(i * 71u + 3u);
        const float seed4 = Hash01(i * 89u + 23u);
        const float seed5 = Hash01(i * 113u + 47u);

        DustParticleCPU particle{};
        particle.Seed = 1.0f + static_cast<float>(i) * 0.6180339f + seed2 * 11.0f;

        if (i < SphereDustParticles)
        {
            const float z = seed0 * 2.0f - 1.0f;
            const float angle = seed1 * XM_2PI;
            const float radial = sqrtf((std::max)(0.0f, 1.0f - z * z));
            const XMFLOAT3 dir(
                cosf(angle) * radial,
                z,
                sinf(angle) * radial);

            particle.Position = XMFLOAT3(
                sphereCenter.x + dir.x * sphereRadius,
                sphereCenter.y + dir.y * sphereRadius,
                sphereCenter.z + dir.z * sphereRadius);
            particle.Size = 1.6f + seed3 * 1.2f;
            particle.Velocity = XMFLOAT3(0.0f, 0.0f, 0.0f);
            particle.Age = 0.0f;
            particle.Lifetime = 999999.0f;
            particle.Kind = 1.0f;
        }
        else
        {
            const float zDynamic = seed0 * 2.0f - 1.0f;
            const float angleDynamic = seed1 * XM_2PI;
            const float radialDynamic = sqrtf((std::max)(0.0f, 1.0f - zDynamic * zDynamic));
            const XMFLOAT3 dirDynamic(
                cosf(angleDynamic) * radialDynamic,
                zDynamic,
                sinf(angleDynamic) * radialDynamic);

            particle.Position = XMFLOAT3(
                sphereCenter.x + dirDynamic.x * (sphereRadius * (0.8f + seed2 * 0.2f)),
                sphereCenter.y + dirDynamic.y * (sphereRadius * (0.8f + seed2 * 0.2f)),
                sphereCenter.z + dirDynamic.z * (sphereRadius * (0.8f + seed2 * 0.2f)));
            particle.Size = 1.8f + seed3 * 2.0f;
            particle.Velocity = XMFLOAT3(
                dirDynamic.x * (18.0f + seed4 * 12.0f),
                dirDynamic.y * (18.0f + seed5 * 12.0f),
                dirDynamic.z * (18.0f + seed0 * 12.0f));
            particle.Age = seed1 * (2.4f + seed4 * 1.7f);
            particle.Lifetime = 2.4f + seed4 * 1.7f;
            particle.Kind = 0.0f;
        }
        particles[i] = particle;
    }

    void* particleData = nullptr;
    if (SUCCEEDED(m_particleUploadBuffer->Map(0, nullptr, &particleData)) && particleData != nullptr)
    {
        std::memcpy(particleData, particles.data(), sizeof(DustParticleCPU) * MaxDustParticles);
        m_particleUploadBuffer->Unmap(0, nullptr);
    }

    UINT* counterData = nullptr;
    if (SUCCEEDED(m_particleCounterUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&counterData))) &&
        counterData != nullptr)
    {
        counterData[0] = MaxDustParticles;
        counterData[1] = 0;
        m_particleCounterUploadBuffer->Unmap(0, nullptr);
    }
}

void RenderingSystem::ResetParticleCounters(ID3D12GraphicsCommandList* commandList, bool resetSourceCounter)
{
    if (commandList == nullptr)
    {
        return;
    }

    const UINT destinationIndex = 1u - m_particleSourceIndex;

    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT barrierCount = 0;

    if (resetSourceCounter)
    {
        barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[barrierCount].Transition.pResource = m_particleCounterBuffers[m_particleSourceIndex].Get();
        barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        ++barrierCount;
    }

    barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[barrierCount].Transition.pResource = m_particleCounterBuffers[destinationIndex].Get();
    barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    ++barrierCount;

    commandList->ResourceBarrier(barrierCount, barriers);

    if (resetSourceCounter)
    {
        commandList->CopyBufferRegion(
            m_particleCounterBuffers[m_particleSourceIndex].Get(),
            0,
            m_particleCounterUploadBuffer.Get(),
            0,
            sizeof(UINT));
    }

    commandList->CopyBufferRegion(
        m_particleCounterBuffers[destinationIndex].Get(),
        0,
        m_particleCounterUploadBuffer.Get(),
        sizeof(UINT),
        sizeof(UINT));

    D3D12_RESOURCE_BARRIER toUav[2]{};
    UINT toUavCount = 0;

    if (resetSourceCounter)
    {
        toUav[toUavCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toUav[toUavCount].Transition.pResource = m_particleCounterBuffers[m_particleSourceIndex].Get();
        toUav[toUavCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toUav[toUavCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toUav[toUavCount].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ++toUavCount;
    }

    toUav[toUavCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toUav[toUavCount].Transition.pResource = m_particleCounterBuffers[destinationIndex].Get();
    toUav[toUavCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toUav[toUavCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toUav[toUavCount].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ++toUavCount;

    commandList->ResourceBarrier(toUavCount, toUav);
}

void RenderingSystem::UpdateParticleSimulationConstants(float deltaTime)
{
    if (m_particleSimulationCBMappedData == nullptr)
    {
        return;
    }

    const XMFLOAT3 sceneCenter = m_context.GetSceneCenter();
    const XMFLOAT3 sceneExtents = m_context.GetSceneExtents();
    const float halfWidth = (std::max)(sceneExtents.x * 0.42f, 180.0f);
    const float halfDepth = (std::max)(sceneExtents.z * 0.30f, 140.0f);
    const float minY = sceneCenter.y - sceneExtents.y * 0.18f;
    const float maxY = sceneCenter.y + sceneExtents.y * 0.28f;
    const float sphereRadius = 18.0f;
    const XMFLOAT3 sphereCenter(
        sceneCenter.x,
        minY + sphereRadius + 22.0f,
        sceneCenter.z);
    const XMFLOAT3 emitterPosition(
        sphereCenter.x,
        sphereCenter.y,
        sphereCenter.z);

    ParticleSimCB cb{};
    cb.DeltaTimeTime = XMFLOAT4(deltaTime, m_context.GetTime(), static_cast<float>(MaxDustParticles), 0.0f);
    cb.BoundsMin = XMFLOAT4(
        sceneCenter.x - halfWidth,
        minY,
        sceneCenter.z - halfDepth,
        0.0f);
    cb.BoundsMax = XMFLOAT4(
        sceneCenter.x + halfWidth,
        maxY,
        sceneCenter.z + halfDepth,
        0.0f);
    cb.NoiseParams = XMFLOAT4(2.6f, -9.0f, 0.08f, 0.82f);
    cb.EmitterPosition = XMFLOAT4(emitterPosition.x, emitterPosition.y, emitterPosition.z, sphereRadius);
    cb.SphereData = XMFLOAT4(sphereCenter.x, sphereCenter.y, sphereCenter.z, sphereRadius);

    std::memcpy(m_particleSimulationCBMappedData, &cb, sizeof(cb));
}

void RenderingSystem::UpdateParticleRenderConstants()
{
    if (m_particleRenderCBMappedData == nullptr)
    {
        return;
    }

    const XMFLOAT3 cameraPosValue = m_context.GetCameraPosition();
    const XMFLOAT3 cameraTargetValue = m_context.GetCameraTarget();
    const XMFLOAT3 sceneCenter = m_context.GetSceneCenter();
    const XMFLOAT3 sceneExtents = m_context.GetSceneExtents();
    const float sphereRadius = 18.0f;
    const float minY = sceneCenter.y - sceneExtents.y * 0.18f;
    const XMFLOAT3 sphereCenter(
        sceneCenter.x,
        minY + sphereRadius + 22.0f,
        sceneCenter.z);

    XMVECTOR cameraPos = XMLoadFloat3(&cameraPosValue);
    XMVECTOR cameraTarget = XMLoadFloat3(&cameraTargetValue);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(cameraPos, cameraTarget, up);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(
        XM_PIDIV4,
        static_cast<float>(m_width) / static_cast<float>(m_height),
        1.0f,
        20000.0f);
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);

    XMFLOAT3 forwardValue = XMFLOAT3(
        cameraTargetValue.x - cameraPosValue.x,
        cameraTargetValue.y - cameraPosValue.y,
        cameraTargetValue.z - cameraPosValue.z);
    XMVECTOR forward = XMVector3Normalize(XMLoadFloat3(&forwardValue));
    XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));
    XMVECTOR billboardUp = XMVector3Normalize(XMVector3Cross(forward, right));

    XMFLOAT3 rightValue{};
    XMFLOAT3 upValue{};
    XMStoreFloat3(&rightValue, right);
    XMStoreFloat3(&upValue, billboardUp);

    ParticleRenderCB cb{};
    XMStoreFloat4x4(&cb.ViewProj, XMMatrixTranspose(viewProj));
    cb.CameraRight = XMFLOAT4(rightValue.x, rightValue.y, rightValue.z, 0.0f);
    cb.CameraUp = XMFLOAT4(upValue.x, upValue.y, upValue.z, 0.0f);
    cb.DustColor = XMFLOAT4(0.34f, 0.78f, 1.0f, 1.0f);
    cb.EffectParams = XMFLOAT4(sphereCenter.x, sphereCenter.y, sphereCenter.z, sphereRadius);

    std::memcpy(m_particleRenderCBMappedData, &cb, sizeof(cb));
}

void RenderingSystem::UpdateParticleSimulation()
{
    if (m_context.GetCurrentScene() != Scene::Sponza ||
        !m_particleComputePSO ||
        !m_particleComputeRootSignature ||
        !m_particleHeap)
    {
        return;
    }

    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    if (commandList == nullptr)
    {
        return;
    }

    if (!m_particleDataInitialized)
    {
        InitializeParticleData();

        commandList->CopyResource(m_particleBuffers[m_particleSourceIndex].Get(), m_particleUploadBuffer.Get());
        commandList->CopyResource(m_particleBuffers[1u - m_particleSourceIndex].Get(), m_particleUploadBuffer.Get());

        D3D12_RESOURCE_BARRIER initBarriers[4]{};
        for (UINT i = 0; i < 2; ++i)
        {
            initBarriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            initBarriers[i].Transition.pResource = m_particleBuffers[i].Get();
            initBarriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            initBarriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            initBarriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

            initBarriers[2 + i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            initBarriers[2 + i].Transition.pResource = m_particleCounterBuffers[i].Get();
            initBarriers[2 + i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            initBarriers[2 + i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            initBarriers[2 + i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        commandList->ResourceBarrier(_countof(initBarriers), initBarriers);
        ResetParticleCounters(commandList, true);
        m_particleDataInitialized = true;
        m_previousParticleTime = m_context.GetTime();
    }

    if (!m_particleTextureUploaded && m_particleTexture != nullptr && m_particleTextureUpload != nullptr)
    {
        const D3D12_RESOURCE_DESC textureDesc = m_particleTexture->GetDesc();
        const UINT rowPitch = (static_cast<UINT>(textureDesc.Width) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
            ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = m_particleTexture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = m_particleTextureUpload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = static_cast<UINT>(textureDesc.Width);
        src.PlacedFootprint.Footprint.Height = textureDesc.Height;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = rowPitch;

        commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER textureBarrier{};
        textureBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        textureBarrier.Transition.pResource = m_particleTexture.Get();
        textureBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        textureBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        textureBarrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        commandList->ResourceBarrier(1, &textureBarrier);

        m_particleTextureUploaded = true;
    }

    const float currentTime = m_context.GetTime();
    float deltaTime = currentTime - m_previousParticleTime;
    if (deltaTime < 0.0f)
    {
        deltaTime = 0.0f;
    }
    if (deltaTime > 0.033f)
    {
        deltaTime = 0.033f;
    }
    m_previousParticleTime = currentTime;

    UpdateParticleSimulationConstants(deltaTime);
    ResetParticleCounters(commandList, false);

    ID3D12DescriptorHeap* heaps[] = { m_particleHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetPipelineState(m_particleComputePSO.Get());
    commandList->SetComputeRootSignature(m_particleComputeRootSignature.Get());
    commandList->SetComputeRootConstantBufferView(0, m_particleSimulationConstantBuffer->GetGPUVirtualAddress());

    const UINT destinationIndex = 1u - m_particleSourceIndex;
    D3D12_GPU_DESCRIPTOR_HANDLE consumeHandle = m_particleHeap->GetGPUDescriptorHandleForHeapStart();
    consumeHandle.ptr += SIZE_T((m_particleSourceIndex == 0) ? kParticleUavBuffer0 : kParticleUavBuffer1) *
        SIZE_T(m_particleDescriptorSize);
    commandList->SetComputeRootDescriptorTable(1, consumeHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE appendHandle = m_particleHeap->GetGPUDescriptorHandleForHeapStart();
    appendHandle.ptr += SIZE_T((destinationIndex == 0) ? kParticleUavBuffer0 : kParticleUavBuffer1) *
        SIZE_T(m_particleDescriptorSize);
    commandList->SetComputeRootDescriptorTable(2, appendHandle);

    const UINT groupCount = (MaxDustParticles + kParticleThreadGroupSize - 1) / kParticleThreadGroupSize;
    commandList->Dispatch(groupCount, 1, 1);

    D3D12_RESOURCE_BARRIER uavBarrier[4]{};
    for (UINT i = 0; i < 2; ++i)
    {
        uavBarrier[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier[i].UAV.pResource = m_particleBuffers[i].Get();
        uavBarrier[2 + i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier[2 + i].UAV.pResource = m_particleCounterBuffers[i].Get();
    }
    commandList->ResourceBarrier(_countof(uavBarrier), uavBarrier);

    D3D12_RESOURCE_BARRIER toSrv{};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = m_particleBuffers[destinationIndex].Get();
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &toSrv);

    m_particleSourceIndex = destinationIndex;
}

void RenderingSystem::RenderParticleStage()
{
    if (m_context.GetCurrentScene() != Scene::Sponza ||
        !m_particleGraphicsPSO ||
        !m_particleGraphicsRootSignature ||
        !m_particleHeap ||
        !m_particleBuffers[m_particleSourceIndex])
    {
        return;
    }

    UpdateParticleRenderConstants();

    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    D3D12_VIEWPORT vp = m_context.GetViewport();
    D3D12_RECT sc = m_context.GetScissorRect();
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = m_context.GetCurrentBackBufferRTV();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_context.GetDepthStencilView();

    commandList->RSSetViewports(1, &vp);
    commandList->RSSetScissorRects(1, &sc);
    commandList->OMSetRenderTargets(1, &backBufferRtv, TRUE, &dsv);
    commandList->SetPipelineState(m_particleGraphicsPSO.Get());
    commandList->SetGraphicsRootSignature(m_particleGraphicsRootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = { m_particleHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootConstantBufferView(0, m_particleRenderConstantBuffer->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = m_particleHeap->GetGPUDescriptorHandleForHeapStart();
    srvHandle.ptr += SIZE_T(m_particleSourceIndex == 0 ? kParticleSrvBuffer0 : kParticleSrvBuffer1) *
        SIZE_T(m_particleDescriptorSize);
    commandList->SetGraphicsRootDescriptorTable(1, srvHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = m_particleHeap->GetGPUDescriptorHandleForHeapStart();
    textureHandle.ptr += SIZE_T(kParticleTextureSrv) * SIZE_T(m_particleDescriptorSize);
    commandList->SetGraphicsRootDescriptorTable(2, textureHandle);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    commandList->DrawInstanced(MaxDustParticles, 1, 0, 0);

    D3D12_RESOURCE_BARRIER backToUav{};
    backToUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    backToUav.Transition.pResource = m_particleBuffers[m_particleSourceIndex].Get();
    backToUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    backToUav.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    backToUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    commandList->ResourceBarrier(1, &backToUav);
}

void RenderingSystem::InitializeFireData()
{
    if (m_fireUploadBuffer == nullptr || m_fireCounterUploadBuffer == nullptr)
    {
        return;
    }

    std::vector<FireParticleCPU> particles(MaxFireParticles);
    const XMFLOAT3 sceneCenter = m_context.GetSceneCenter();
    const XMFLOAT3 sceneExtents = m_context.GetSceneExtents();
    const XMFLOAT3 fireCenter(
        sceneCenter.x - sceneExtents.x * 0.08f,
        sceneCenter.y - sceneExtents.y * 0.72f,
        sceneCenter.z + sceneExtents.z * 0.04f);

    for (UINT i = 0; i < MaxFireParticles; ++i)
    {
        const float seed0 = Hash01(i * 13u + 5u);
        const float seed1 = Hash01(i * 37u + 17u);
        const float seed2 = Hash01(i * 61u + 9u);
        const float seed3 = Hash01(i * 97u + 29u);

        FireParticleCPU particle{};
        particle.Position = XMFLOAT3(
            fireCenter.x + (seed0 * 2.0f - 1.0f) * 18.0f,
            fireCenter.y + seed1 * 24.0f,
            fireCenter.z + (seed2 * 2.0f - 1.0f) * 18.0f);
        particle.Size = 5.4f + seed3 * 6.8f;
        particle.Velocity = XMFLOAT3(
            (seed0 * 2.0f - 1.0f) * 4.8f,
            24.0f + seed1 * 22.0f,
            (seed2 * 2.0f - 1.0f) * 4.8f);
        particle.Seed = seed3;
        particles[i] = particle;
    }

    void* fireData = nullptr;
    if (SUCCEEDED(m_fireUploadBuffer->Map(0, nullptr, &fireData)) && fireData != nullptr)
    {
        std::memcpy(fireData, particles.data(), sizeof(FireParticleCPU) * MaxFireParticles);
        m_fireUploadBuffer->Unmap(0, nullptr);
    }

    UINT* counterData = nullptr;
    if (SUCCEEDED(m_fireCounterUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&counterData))) &&
        counterData != nullptr)
    {
        counterData[0] = MaxFireParticles;
        counterData[1] = 0;
        m_fireCounterUploadBuffer->Unmap(0, nullptr);
    }
}

void RenderingSystem::ResetFireCounters(ID3D12GraphicsCommandList* commandList, bool resetSourceCounter)
{
    if (commandList == nullptr)
    {
        return;
    }

    const UINT destinationIndex = 1u - m_fireSourceIndex;
    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT barrierCount = 0;

    if (resetSourceCounter)
    {
        barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[barrierCount].Transition.pResource = m_fireCounterBuffers[m_fireSourceIndex].Get();
        barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        ++barrierCount;
    }

    barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[barrierCount].Transition.pResource = m_fireCounterBuffers[destinationIndex].Get();
    barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    ++barrierCount;
    commandList->ResourceBarrier(barrierCount, barriers);

    if (resetSourceCounter)
    {
        commandList->CopyBufferRegion(
            m_fireCounterBuffers[m_fireSourceIndex].Get(),
            0,
            m_fireCounterUploadBuffer.Get(),
            0,
            sizeof(UINT));
    }

    commandList->CopyBufferRegion(
        m_fireCounterBuffers[destinationIndex].Get(),
        0,
        m_fireCounterUploadBuffer.Get(),
        sizeof(UINT),
        sizeof(UINT));

    D3D12_RESOURCE_BARRIER toUav[2]{};
    UINT toUavCount = 0;
    if (resetSourceCounter)
    {
        toUav[toUavCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toUav[toUavCount].Transition.pResource = m_fireCounterBuffers[m_fireSourceIndex].Get();
        toUav[toUavCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toUav[toUavCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toUav[toUavCount].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ++toUavCount;
    }

    toUav[toUavCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toUav[toUavCount].Transition.pResource = m_fireCounterBuffers[destinationIndex].Get();
    toUav[toUavCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toUav[toUavCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toUav[toUavCount].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ++toUavCount;
    commandList->ResourceBarrier(toUavCount, toUav);
}

void RenderingSystem::UpdateFireSimulationConstants(float deltaTime)
{
    if (m_fireSimulationCBMappedData == nullptr)
    {
        return;
    }

    const XMFLOAT3 sceneCenter = m_context.GetSceneCenter();
    const XMFLOAT3 sceneExtents = m_context.GetSceneExtents();
    const XMFLOAT3 fireCenter(
        sceneCenter.x - sceneExtents.x * 0.08f,
        sceneCenter.y - sceneExtents.y * 0.72f,
        sceneCenter.z + sceneExtents.z * 0.04f);

    ParticleSimCB cb{};
    cb.DeltaTimeTime = XMFLOAT4(deltaTime, m_context.GetTime(), static_cast<float>(MaxFireParticles), 0.0f);
    cb.BoundsMin = XMFLOAT4(fireCenter.x - 22.0f, fireCenter.y, fireCenter.z - 22.0f, 0.0f);
    cb.BoundsMax = XMFLOAT4(fireCenter.x + 22.0f, fireCenter.y + 118.0f, fireCenter.z + 22.0f, 0.0f);
    cb.NoiseParams = XMFLOAT4(fireCenter.x, fireCenter.y, fireCenter.z, 1.0f);
    std::memcpy(m_fireSimulationCBMappedData, &cb, sizeof(cb));
}

void RenderingSystem::UpdateFireRenderConstants()
{
    if (m_fireRenderCBMappedData == nullptr)
    {
        return;
    }

    const XMFLOAT3 cameraPosValue = m_context.GetCameraPosition();
    const XMFLOAT3 cameraTargetValue = m_context.GetCameraTarget();
    const XMFLOAT3 sceneCenter = m_context.GetSceneCenter();
    const XMFLOAT3 sceneExtents = m_context.GetSceneExtents();
    const XMFLOAT3 fireCenter(
        sceneCenter.x - sceneExtents.x * 0.08f,
        sceneCenter.y - sceneExtents.y * 0.72f,
        sceneCenter.z + sceneExtents.z * 0.04f);

    XMVECTOR cameraPos = XMLoadFloat3(&cameraPosValue);
    XMVECTOR cameraTarget = XMLoadFloat3(&cameraTargetValue);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(cameraPos, cameraTarget, up);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(
        XM_PIDIV4,
        static_cast<float>(m_width) / static_cast<float>(m_height),
        1.0f,
        20000.0f);
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);

    XMFLOAT3 forwardValue = XMFLOAT3(
        cameraTargetValue.x - cameraPosValue.x,
        cameraTargetValue.y - cameraPosValue.y,
        cameraTargetValue.z - cameraPosValue.z);
    XMVECTOR forward = XMVector3Normalize(XMLoadFloat3(&forwardValue));
    XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));
    XMVECTOR billboardUp = XMVector3Normalize(XMVector3Cross(forward, right));

    XMFLOAT3 rightValue{};
    XMFLOAT3 upValue{};
    XMStoreFloat3(&rightValue, right);
    XMStoreFloat3(&upValue, billboardUp);

    ParticleRenderCB cb{};
    XMStoreFloat4x4(&cb.ViewProj, XMMatrixTranspose(viewProj));
    cb.CameraRight = XMFLOAT4(rightValue.x, rightValue.y, rightValue.z, 0.0f);
    cb.CameraUp = XMFLOAT4(upValue.x, upValue.y, upValue.z, 0.0f);
    cb.DustColor = XMFLOAT4(1.0f, 0.55f, 0.08f, 1.0f);
    cb.EffectParams = XMFLOAT4(
        fireCenter.y,
        fireCenter.y + 118.0f,
        0.0f,
        0.0f);
    std::memcpy(m_fireRenderCBMappedData, &cb, sizeof(cb));
}

void RenderingSystem::UpdateFireSimulation()
{
    if (m_context.GetCurrentScene() != Scene::Sponza ||
        !m_fireComputePSO ||
        !m_fireComputeRootSignature ||
        !m_fireHeap)
    {
        return;
    }

    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    if (commandList == nullptr)
    {
        return;
    }

    if (!m_fireDataInitialized)
    {
        InitializeFireData();
        commandList->CopyResource(m_fireBuffers[m_fireSourceIndex].Get(), m_fireUploadBuffer.Get());
        commandList->CopyResource(m_fireBuffers[1u - m_fireSourceIndex].Get(), m_fireUploadBuffer.Get());

        D3D12_RESOURCE_BARRIER initBarriers[4]{};
        for (UINT i = 0; i < 2; ++i)
        {
            initBarriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            initBarriers[i].Transition.pResource = m_fireBuffers[i].Get();
            initBarriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            initBarriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            initBarriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

            initBarriers[2 + i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            initBarriers[2 + i].Transition.pResource = m_fireCounterBuffers[i].Get();
            initBarriers[2 + i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            initBarriers[2 + i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            initBarriers[2 + i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        commandList->ResourceBarrier(_countof(initBarriers), initBarriers);
        ResetFireCounters(commandList, true);
        m_fireDataInitialized = true;
        m_previousFireTime = m_context.GetTime();
    }

    float deltaTime = m_context.GetTime() - m_previousFireTime;
    if (deltaTime < 0.0f)
    {
        deltaTime = 0.0f;
    }
    if (deltaTime > 0.033f)
    {
        deltaTime = 0.033f;
    }
    m_previousFireTime = m_context.GetTime();

    UpdateFireSimulationConstants(deltaTime);
    ResetFireCounters(commandList, false);

    ID3D12DescriptorHeap* heaps[] = { m_fireHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetPipelineState(m_fireComputePSO.Get());
    commandList->SetComputeRootSignature(m_fireComputeRootSignature.Get());
    commandList->SetComputeRootConstantBufferView(0, m_fireSimulationConstantBuffer->GetGPUVirtualAddress());

    const UINT destinationIndex = 1u - m_fireSourceIndex;
    D3D12_GPU_DESCRIPTOR_HANDLE consumeHandle = m_fireHeap->GetGPUDescriptorHandleForHeapStart();
    consumeHandle.ptr += SIZE_T((m_fireSourceIndex == 0) ? kFireUavBuffer0 : kFireUavBuffer1) * SIZE_T(m_fireDescriptorSize);
    commandList->SetComputeRootDescriptorTable(1, consumeHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE appendHandle = m_fireHeap->GetGPUDescriptorHandleForHeapStart();
    appendHandle.ptr += SIZE_T((destinationIndex == 0) ? kFireUavBuffer0 : kFireUavBuffer1) * SIZE_T(m_fireDescriptorSize);
    commandList->SetComputeRootDescriptorTable(2, appendHandle);

    const UINT groupCount = (MaxFireParticles + kFireThreadGroupSize - 1) / kFireThreadGroupSize;
    commandList->Dispatch(groupCount, 1, 1);

    D3D12_RESOURCE_BARRIER uavBarrier[4]{};
    for (UINT i = 0; i < 2; ++i)
    {
        uavBarrier[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier[i].UAV.pResource = m_fireBuffers[i].Get();
        uavBarrier[2 + i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier[2 + i].UAV.pResource = m_fireCounterBuffers[i].Get();
    }
    commandList->ResourceBarrier(_countof(uavBarrier), uavBarrier);

    D3D12_RESOURCE_BARRIER toSrv{};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = m_fireBuffers[destinationIndex].Get();
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &toSrv);

    m_fireSourceIndex = destinationIndex;
}

void RenderingSystem::RenderFireStage()
{
    if (m_context.GetCurrentScene() != Scene::Sponza ||
        !m_fireGraphicsPSO ||
        !m_fireGraphicsRootSignature ||
        !m_fireHeap ||
        !m_fireBuffers[m_fireSourceIndex])
    {
        return;
    }

    UpdateFireRenderConstants();

    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    D3D12_VIEWPORT vp = m_context.GetViewport();
    D3D12_RECT sc = m_context.GetScissorRect();
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = m_context.GetCurrentBackBufferRTV();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_context.GetDepthStencilView();

    commandList->RSSetViewports(1, &vp);
    commandList->RSSetScissorRects(1, &sc);
    commandList->OMSetRenderTargets(1, &backBufferRtv, TRUE, &dsv);
    commandList->SetPipelineState(m_fireGraphicsPSO.Get());
    commandList->SetGraphicsRootSignature(m_fireGraphicsRootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = { m_fireHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootConstantBufferView(0, m_fireRenderConstantBuffer->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = m_fireHeap->GetGPUDescriptorHandleForHeapStart();
    srvHandle.ptr += SIZE_T(m_fireSourceIndex == 0 ? kFireSrvBuffer0 : kFireSrvBuffer1) * SIZE_T(m_fireDescriptorSize);
    commandList->SetGraphicsRootDescriptorTable(1, srvHandle);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    commandList->DrawInstanced(MaxFireParticles, 1, 0, 0);

    D3D12_RESOURCE_BARRIER backToUav{};
    backToUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    backToUav.Transition.pResource = m_fireBuffers[m_fireSourceIndex].Get();
    backToUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    backToUav.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    backToUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    commandList->ResourceBarrier(1, &backToUav);
}
// передача источников света в буфер света
bool RenderingSystem::CreateLightingConstantBuffer()
{
    const UINT cbSize = (sizeof(DeferredLightCB) + 255) & ~255u;

    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = cbSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(m_context.GetDevice()->CreateCommittedResource(
        &upload,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_deferredLightConstantBuffer))))
    {
        return false;
    }

    D3D12_RANGE readRange{ 0, 0 };
    if (FAILED(m_deferredLightConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_deferredLightCBMappedData))))
    {
        return false;
    }

    UpdateLightingConstants();

    return true;
}

bool RenderingSystem::CreateWaterConstantBuffer()
{
    const UINT cbSize = (sizeof(WaterCB) + 255) & ~255u;

    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = cbSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(m_context.GetDevice()->CreateCommittedResource(
        &upload,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_waterConstantBuffer))))
    {
        return false;
    }

    D3D12_RANGE readRange{ 0, 0 };
    if (FAILED(m_waterConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_waterCBMappedData))))
    {
        return false;
    }

    UpdateWaterConstants();
    return true;
}


void RenderingSystem::UpdateLightingConstants()
{
    if (m_deferredLightCBMappedData == nullptr)
    {
        return;
    }

    DeferredLightCB cb{};
    const XMFLOAT3 sceneCenter = m_context.GetSceneCenter();
    const XMFLOAT3 sceneExtents = m_context.GetSceneExtents();
    const float dominantExtent = (std::max)(sceneExtents.x, (std::max)(sceneExtents.y, sceneExtents.z));
    if (m_context.GetCurrentScene() == Scene::Sponza)
    {
        const float pointRange = (std::max)(dominantExtent * 0.52f, 420.0f);
        const float spotRange = (std::max)(dominantExtent * 0.62f, 520.0f);

        // Imported Sponza lighting setup: directional + point + spot.
        cb.LightDirection = XMFLOAT4(-0.4f, -1.0f, -0.2f, 0.0f);
        cb.LightColor = XMFLOAT4(1.00f, 0.98f, 0.95f, 0.55f);
        cb.AmbientColor = XMFLOAT4(0.030f, 0.032f, 0.040f, 1.0f);
        cb.LightCounts = XMFLOAT4(1.0f, 1.0f, 0.0f, 0.0f);

        cb.PointLightPositionRange[0] = XMFLOAT4(
            sceneCenter.x - sceneExtents.x * 0.18f,
            sceneCenter.y + sceneExtents.y * 0.20f,
            sceneCenter.z - sceneExtents.z * 0.04f,
            pointRange);
        cb.PointLightColorIntensity[0] = XMFLOAT4(0.18f, 0.56f, 1.00f, 28.0f);

        cb.SpotLightPositionRange[0] = XMFLOAT4(
            sceneCenter.x + sceneExtents.x * 0.14f,
            sceneCenter.y + sceneExtents.y * 0.58f,
            sceneCenter.z - sceneExtents.z * 0.08f,
            spotRange);
        cb.SpotLightDirectionCosine[0] = XMFLOAT4(-0.08f, -0.99f, 0.10f, 0.76f);
        cb.SpotLightColorIntensity[0] = XMFLOAT4(1.00f, 0.42f, 0.12f, 22.0f);
    }
    else if (m_context.GetCurrentScene() == Scene::ChickenField)
    {
        const float pointRange = (std::max)(dominantExtent * 0.60f, 420.0f);

        cb.LightDirection = XMFLOAT4(0.62f, -1.0f, -0.26f, 0.0f);
        cb.LightColor = XMFLOAT4(0.58f, 0.52f, 0.46f, 1.10f);
        cb.AmbientColor = XMFLOAT4(0.020f, 0.018f, 0.017f, 1.0f);
        cb.LightCounts = XMFLOAT4(1.0f, 0.0f, 0.0f, 0.0f);

        cb.PointLightPositionRange[0] = XMFLOAT4(
            sceneCenter.x + sceneExtents.x * 0.85f,
            sceneCenter.y + sceneExtents.y * 0.80f,
            sceneCenter.z - sceneExtents.z * 0.75f,
            pointRange);
        cb.PointLightColorIntensity[0] = XMFLOAT4(0.72f, 0.58f, 0.46f, 0.85f);
    }
    else
    {
        const float pointRange = (std::max)(dominantExtent * 0.95f, 900.0f);

        cb.LightDirection = XMFLOAT4(0.88f, -1.0f, -0.34f, 0.0f);
        cb.LightColor = XMFLOAT4(1.00f, 0.95f, 0.88f, 3.40f);
        cb.AmbientColor = XMFLOAT4(0.09f, 0.10f, 0.12f, 0.52f);
        cb.LightCounts = XMFLOAT4(1.0f, 0.0f, 0.0f, 0.0f);

        cb.PointLightPositionRange[0] = XMFLOAT4(
            sceneCenter.x + sceneExtents.x * 1.80f,
            sceneCenter.y + sceneExtents.y * 1.25f,
            sceneCenter.z - sceneExtents.z * 1.35f,
            pointRange);
        cb.PointLightColorIntensity[0] = XMFLOAT4(1.00f, 0.94f, 0.86f, 2.10f);
    }

    cb.ScreenSize = XMFLOAT4(
        static_cast<float>(m_width),
        static_cast<float>(m_height),
        1.0f / static_cast<float>(m_width),
        1.0f / static_cast<float>(m_height));

    const XMFLOAT3 cameraPosValue = m_context.GetCameraPosition();
    const XMFLOAT3 cameraTargetValue = m_context.GetCameraTarget();

    XMVECTOR cameraPos = XMLoadFloat3(&cameraPosValue);
    XMVECTOR cameraTarget = XMLoadFloat3(&cameraTargetValue);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(cameraPos, cameraTarget, up);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(
        XM_PIDIV4,
        static_cast<float>(m_width) / static_cast<float>(m_height),
        1.0f,
        20000.0f);

    XMStoreFloat4x4(&cb.View, XMMatrixTranspose(view));
    UpdateShadowMatrices(cb);

    XMMATRIX invView = XMMatrixInverse(nullptr, view);
    XMMATRIX invProj = XMMatrixInverse(nullptr, proj);
    XMStoreFloat4x4(&cb.InvView, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&cb.InvProj, XMMatrixTranspose(invProj));
    const UINT prefilteredMipCount = (m_prefilteredEnvMap != nullptr) ? m_prefilteredEnvMap->GetDesc().MipLevels : 1u;
    cb.IblParams = XMFLOAT4(static_cast<float>((std::max)(int(prefilteredMipCount) - 1, 0)), 0.0f, 0.0f, 0.0f);

    std::memcpy(m_deferredLightCBMappedData, &cb, sizeof(cb));
}
//  положение и размер каждого каскада
void RenderingSystem::UpdateShadowMatrices(DeferredLightCB& cb) const
{
    const bool useStaticShadowCascades = true;
    const XMFLOAT3 sceneCenterValue = m_context.GetSceneCenter();
    const XMFLOAT3 sceneMinValue = m_context.GetSceneBoundsMin();
    const XMFLOAT3 sceneMaxValue = m_context.GetSceneBoundsMax();
    const XMFLOAT3 sceneExtents = m_context.GetSceneExtents();
    const float dominantExtent = (std::max)(sceneExtents.x, (std::max)(sceneExtents.y, sceneExtents.z));
    const float nearClip = 1.0f;
    const float farClip = (std::min)(20000.0f, (std::max)(dominantExtent * 2.5f, 1400.0f));
    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    const float fovY = XM_PIDIV4;
    const float lambda = 0.85f;
    const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMFLOAT3 cameraPosValue = m_context.GetCameraPosition();
    const XMFLOAT3 cameraTargetValue = m_context.GetCameraTarget();
    const XMVECTOR cameraPos = XMLoadFloat3(&cameraPosValue);
    const XMVECTOR cameraTarget = XMLoadFloat3(&cameraTargetValue);
    const XMMATRIX cameraView = XMMatrixLookAtLH(cameraPos, cameraTarget, worldUp);
    const XMMATRIX invCameraView = XMMatrixInverse(nullptr, cameraView);

    const XMVECTOR surfaceToLight = XMVector3Normalize(XMVectorSet(
        -cb.LightDirection.x,
        -cb.LightDirection.y,
        -cb.LightDirection.z,
        0.0f));

    XMVECTOR lightUp = worldUp;
    if (std::abs(XMVectorGetX(XMVector3Dot(lightUp, surfaceToLight))) > 0.98f)
    {
        lightUp = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    }

    std::array<XMVECTOR, 8> sceneCorners =
    {
        XMVectorSet(sceneMinValue.x, sceneMinValue.y, sceneMinValue.z, 1.0f),
        XMVectorSet(sceneMinValue.x, sceneMinValue.y, sceneMaxValue.z, 1.0f),
        XMVectorSet(sceneMinValue.x, sceneMaxValue.y, sceneMinValue.z, 1.0f),
        XMVectorSet(sceneMinValue.x, sceneMaxValue.y, sceneMaxValue.z, 1.0f),
        XMVectorSet(sceneMaxValue.x, sceneMinValue.y, sceneMinValue.z, 1.0f),
        XMVectorSet(sceneMaxValue.x, sceneMinValue.y, sceneMaxValue.z, 1.0f),
        XMVectorSet(sceneMaxValue.x, sceneMaxValue.y, sceneMinValue.z, 1.0f),
        XMVectorSet(sceneMaxValue.x, sceneMaxValue.y, sceneMaxValue.z, 1.0f)
    };

    std::array<float, ShadowCascadeCount> cascadeSplits{};
    float cascadeNear = nearClip;
    const float tanHalfFovY = std::tan(fovY * 0.5f);
    const float tanHalfFovX = tanHalfFovY * aspect;

    const XMVECTOR sceneCenter = XMLoadFloat3(&sceneCenterValue);
    const float staticLightDistance = dominantExtent * 3.2f + 420.0f;
    const XMVECTOR staticLightPosition = sceneCenter + surfaceToLight * staticLightDistance;
    const XMMATRIX staticLightView = XMMatrixLookAtLH(staticLightPosition, sceneCenter, lightUp);

    XMFLOAT3 minSceneStaticLS(
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)());
    XMFLOAT3 maxSceneStaticLS(
        -(std::numeric_limits<float>::max)(),
        -(std::numeric_limits<float>::max)(),
        -(std::numeric_limits<float>::max)());
    for (const XMVECTOR& sceneCorner : sceneCorners)
    {
        XMFLOAT3 lightSpaceSceneCorner{};
        XMStoreFloat3(&lightSpaceSceneCorner, XMVector3TransformCoord(sceneCorner, staticLightView));
        minSceneStaticLS.x = (std::min)(minSceneStaticLS.x, lightSpaceSceneCorner.x);
        minSceneStaticLS.y = (std::min)(minSceneStaticLS.y, lightSpaceSceneCorner.y);
        minSceneStaticLS.z = (std::min)(minSceneStaticLS.z, lightSpaceSceneCorner.z);
        maxSceneStaticLS.x = (std::max)(maxSceneStaticLS.x, lightSpaceSceneCorner.x);
        maxSceneStaticLS.y = (std::max)(maxSceneStaticLS.y, lightSpaceSceneCorner.y);
        maxSceneStaticLS.z = (std::max)(maxSceneStaticLS.z, lightSpaceSceneCorner.z);
    }

    for (UINT cascadeIndex = 0; cascadeIndex < ShadowCascadeCount; ++cascadeIndex)
    {
        const float p = static_cast<float>(cascadeIndex + 1) / static_cast<float>(ShadowCascadeCount); // доля глубины
        const float logSplit = nearClip * std::pow(farClip / nearClip, p);
        const float uniformSplit = nearClip + (farClip - nearClip) * p;
        const float cascadeFar = lambda * logSplit + (1.0f - lambda) * uniformSplit; // итоговая граница каскада
        cascadeSplits[cascadeIndex] = cascadeFar;

        if (useStaticShadowCascades)
        {
            const float centerX = 0.5f * (minSceneStaticLS.x + maxSceneStaticLS.x);
            const float centerY = 0.5f * (minSceneStaticLS.y + maxSceneStaticLS.y);
            const float halfWidth = 0.5f * (maxSceneStaticLS.x - minSceneStaticLS.x) + dominantExtent * 0.12f + 56.0f;
            const float halfHeight = 0.5f * (maxSceneStaticLS.y - minSceneStaticLS.y) + dominantExtent * 0.12f + 56.0f;
            const float minZ = minSceneStaticLS.z - dominantExtent * 0.60f - 260.0f;
            const float maxZ = maxSceneStaticLS.z + dominantExtent * 0.60f + 260.0f;

            const float extentX = halfWidth * 2.0f;
            const float extentY = halfHeight * 2.0f;
            const float texelStepX = extentX / static_cast<float>(ShadowMapResolution);
            const float texelStepY = extentY / static_cast<float>(ShadowMapResolution);
            const float snappedCenterX = std::floor(centerX / texelStepX) * texelStepX;
            const float snappedCenterY = std::floor(centerY / texelStepY) * texelStepY;

            const XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(
                snappedCenterX - halfWidth,
                snappedCenterX + halfWidth,
                snappedCenterY - halfHeight,
                snappedCenterY + halfHeight,
                minZ,
                maxZ);

            XMStoreFloat4x4(&cb.LightViewProj[cascadeIndex], XMMatrixTranspose(staticLightView * lightProj));
            cascadeNear = cascadeFar;
            continue;
        }

        const float cascadeNearBounds = (std::max)(nearClip, cascadeNear * 0.82f);
        const float cascadeFarBounds = (std::min)(farClip, cascadeFar * 1.06f);
        const float nearX = cascadeNearBounds * tanHalfFovX;
        const float nearY = cascadeNearBounds * tanHalfFovY;
        const float farX = cascadeFarBounds * tanHalfFovX;
        const float farY = cascadeFarBounds * tanHalfFovY;

        std::array<XMVECTOR, 8> frustumCornersVS =
        {
            XMVectorSet(-nearX,  nearY, cascadeNearBounds, 1.0f),
            XMVectorSet( nearX,  nearY, cascadeNearBounds, 1.0f),
            XMVectorSet( nearX, -nearY, cascadeNearBounds, 1.0f),
            XMVectorSet(-nearX, -nearY, cascadeNearBounds, 1.0f),
            XMVectorSet(-farX,   farY,  cascadeFarBounds,  1.0f),
            XMVectorSet( farX,   farY,  cascadeFarBounds,  1.0f),
            XMVectorSet( farX,  -farY,  cascadeFarBounds,  1.0f),
            XMVectorSet(-farX,  -farY,  cascadeFarBounds,  1.0f)
        };

        XMVECTOR cascadeCenter = XMVectorZero();
        std::array<XMVECTOR, 8> frustumCornersWS{};
        for (UINT cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
        {
            frustumCornersWS[cornerIndex] = XMVector3TransformCoord(frustumCornersVS[cornerIndex], invCameraView);
            cascadeCenter += frustumCornersWS[cornerIndex];
        }
        cascadeCenter /= 8.0f;

        const float lightDistance = dominantExtent * 2.8f + cascadeFar * 0.65f + 250.0f;
        const XMVECTOR lightPosition = cascadeCenter + surfaceToLight * lightDistance;
        const XMMATRIX lightView = XMMatrixLookAtLH(lightPosition, cascadeCenter, lightUp);

        XMFLOAT3 minSceneLS(
            (std::numeric_limits<float>::max)(),
            (std::numeric_limits<float>::max)(),
            (std::numeric_limits<float>::max)());
        XMFLOAT3 maxSceneLS(
            -(std::numeric_limits<float>::max)(),
            -(std::numeric_limits<float>::max)(),
            -(std::numeric_limits<float>::max)());
        for (const XMVECTOR& sceneCorner : sceneCorners)
        {
            XMFLOAT3 lightSpaceSceneCorner{};
            XMStoreFloat3(&lightSpaceSceneCorner, XMVector3TransformCoord(sceneCorner, lightView));
            minSceneLS.x = (std::min)(minSceneLS.x, lightSpaceSceneCorner.x);
            minSceneLS.y = (std::min)(minSceneLS.y, lightSpaceSceneCorner.y);
            minSceneLS.z = (std::min)(minSceneLS.z, lightSpaceSceneCorner.z);
            maxSceneLS.x = (std::max)(maxSceneLS.x, lightSpaceSceneCorner.x);
            maxSceneLS.y = (std::max)(maxSceneLS.y, lightSpaceSceneCorner.y);
            maxSceneLS.z = (std::max)(maxSceneLS.z, lightSpaceSceneCorner.z);
        }

        XMFLOAT3 minCascadeLS(
            (std::numeric_limits<float>::max)(),
            (std::numeric_limits<float>::max)(),
            (std::numeric_limits<float>::max)());
        XMFLOAT3 maxCascadeLS(
            -(std::numeric_limits<float>::max)(),
            -(std::numeric_limits<float>::max)(),
            -(std::numeric_limits<float>::max)());

        for (const XMVECTOR& corner : frustumCornersWS)
        {
            XMFLOAT3 lightSpaceCorner{};
            XMStoreFloat3(&lightSpaceCorner, XMVector3TransformCoord(corner, lightView));
            minCascadeLS.x = (std::min)(minCascadeLS.x, lightSpaceCorner.x);
            minCascadeLS.y = (std::min)(minCascadeLS.y, lightSpaceCorner.y);
            minCascadeLS.z = (std::min)(minCascadeLS.z, lightSpaceCorner.z);
            maxCascadeLS.x = (std::max)(maxCascadeLS.x, lightSpaceCorner.x);
            maxCascadeLS.y = (std::max)(maxCascadeLS.y, lightSpaceCorner.y);
            maxCascadeLS.z = (std::max)(maxCascadeLS.z, lightSpaceCorner.z);
        }

        const float paddingXY = 24.0f + dominantExtent * 0.018f + (1.0f - p) * 26.0f;
        const float minX = minCascadeLS.x - paddingXY;
        const float maxX = maxCascadeLS.x + paddingXY;
        const float minY = minCascadeLS.y - paddingXY;
        const float maxY = maxCascadeLS.y + paddingXY;
        const float minZ = minSceneLS.z - dominantExtent * 0.40f - 220.0f;
        const float maxZ = maxSceneLS.z + dominantExtent * 0.40f + 220.0f;

        const float extentX = maxX - minX;
        const float extentY = maxY - minY;
        const float texelStepX = extentX / static_cast<float>(ShadowMapResolution);
        const float texelStepY = extentY / static_cast<float>(ShadowMapResolution);
        const float centerX = std::floor((0.5f * (minX + maxX)) / texelStepX) * texelStepX;
        const float centerY = std::floor((0.5f * (minY + maxY)) / texelStepY) * texelStepY;
        const float halfWidth = 0.5f * extentX;
        const float halfHeight = 0.5f * extentY;

        // ортографическая проекция света
        const XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(
            centerX - halfWidth,
            centerX + halfWidth,
            centerY - halfHeight,
            centerY + halfHeight,
            minZ,
            maxZ);
        // итоговая матрица
        XMStoreFloat4x4(&cb.LightViewProj[cascadeIndex], XMMatrixTranspose(lightView * lightProj));
        cascadeNear = cascadeFar;
    }

    cb.CascadeSplits = XMFLOAT4(
        cascadeSplits[0],
        cascadeSplits[1],
        cascadeSplits[2],
        cascadeSplits[3]);
    cb.ShadowParams = XMFLOAT4(
        useStaticShadowCascades ? 1.0f : 0.0f,
        1.0f / static_cast<float>(ShadowMapResolution),
        useStaticShadowCascades ? 0.00028f : 0.00055f,
        useStaticShadowCascades ? 0.0f : 0.0035f);
}


void RenderingSystem::UpdateWaterConstants()
{
    if (m_waterCBMappedData == nullptr)
    {
        return;
    }

    WaterCB cb{};

    const XMFLOAT3 cameraPosValue = m_context.GetCameraPosition();
    const XMFLOAT3 cameraTargetValue = m_context.GetCameraTarget();
    const XMFLOAT3 sceneCenter = m_context.GetSceneCenter();
    const XMFLOAT3 sceneExtents = m_context.GetSceneExtents();
    const XMFLOAT3 sceneMin = m_context.GetSceneBoundsMin();
    const float time = m_context.GetTime();

    XMVECTOR cameraPos = XMLoadFloat3(&cameraPosValue);
    XMVECTOR cameraTarget = XMLoadFloat3(&cameraTargetValue);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(cameraPos, cameraTarget, up);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(
        XM_PIDIV4,
        static_cast<float>(m_width) / static_cast<float>(m_height),
        1.0f,
        20000.0f);

    XMStoreFloat4x4(&cb.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&cb.Proj, XMMatrixTranspose(proj));
    cb.CameraPos = XMFLOAT4(cameraPosValue.x, cameraPosValue.y, cameraPosValue.z, 1.0f);

    const float waterY = sceneMin.y + sceneExtents.y * 1.72f;
    cb.WaterOrigin = XMFLOAT4(
        sceneCenter.x,
        waterY,
        sceneCenter.z,
        time);
    cb.WaterSize = XMFLOAT4(
        (std::max)(sceneExtents.x * 0.62f, 420.0f),
        (std::max)(sceneExtents.z * 0.34f, 220.0f),
        0.0f,
        0.0f);
    cb.WaterColor = XMFLOAT4(0.14f, 0.58f, 0.76f, 0.64f);
    cb.WaveA = XMFLOAT4(0.036f, 4.2f, 1.35f, 2.10f);
    cb.WaveB = XMFLOAT4(0.052f, 2.7f, -1.70f, 0.031f);

    std::memcpy(m_waterCBMappedData, &cb, sizeof(cb));
}

