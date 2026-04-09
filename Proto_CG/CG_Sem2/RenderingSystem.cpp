#include "RenderingSystem.h"

#include <array>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    struct DebugOverlayConstants
    {
        XMFLOAT4 OverlayRect;
        XMFLOAT4 SceneCenter;
        XMFLOAT4 SceneExtents;
        UINT DebugMode = 0;
        UINT Padding[3] = {};
    };

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
    return m_context.LoadScene(scene);
}
// корректное завершение рендера и очистка памяти
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

    m_deferredLightConstantBuffer.Reset();
    m_waterConstantBuffer.Reset();
    m_deferredGeometryPSO.Reset();
    m_deferredLightingPSO.Reset();
    m_debugOverlayPSO.Reset();
    m_waterPSO.Reset();
    m_debugOverlayRootSignature.Reset();
    m_deferredLightingRootSignature.Reset();
    m_waterRootSignature.Reset();
    m_deferredGeometryVS.Reset();
    m_deferredGeometryHS.Reset();
    m_deferredGeometryDS.Reset();
    m_deferredGeometryPS.Reset();
    m_deferredLightingVS.Reset();
    m_deferredLightingPS.Reset();
    m_debugOverlayVS.Reset();
    m_debugOverlayPS.Reset();
    m_waterVS.Reset();
    m_waterHS.Reset();
    m_waterDS.Reset();
    m_waterPS.Reset();
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
    if (!m_deferredGeometryPSO || !m_deferredLightingPSO)
    {
        RenderForwardFrame();
        return;
    }

    m_context.BeginFrame();
    RenderOpaqueStage();
    RenderLightingStage();
    RenderGBufferDebugOverlay();
    RenderTransparentStage();
    m_context.EndFrame();
}
// свойства поверхности для GBuffer
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
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = m_context.GetCurrentBackBufferRTV();

    UpdateLightingConstants();

    commandList->RSSetViewports(1, &vp);
    commandList->RSSetScissorRects(1, &sc);
    commandList->OMSetRenderTargets(1, &backBufferRtv, TRUE, nullptr);
    commandList->ClearRenderTargetView(backBufferRtv, m_clearColor, 0, nullptr);

    commandList->SetPipelineState(m_deferredLightingPSO.Get());
    commandList->SetGraphicsRootSignature(m_deferredLightingRootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = { m_gbuffer.GetSRVHeap() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootDescriptorTable(0, m_gbuffer.GetSRVGPU(GBuffer::Slot::AlbedoSpec));
    commandList->SetGraphicsRootConstantBufferView(1, m_deferredLightConstantBuffer->GetGPUVirtualAddress());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
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

// инициализация Deferred Resources
bool RenderingSystem::InitializeDeferredResources()
{
    return CompileDeferredShaders() &&
        CreateDeferredLightingRootSignature() &&
        CreateDeferredGeometryPipeline() &&
        CreateDeferredLightingPipeline() &&
        CreateDebugOverlayRootSignature() &&
        CreateDebugOverlayPipeline() &&
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
    wchar_t debugPath[MAX_PATH];
    wchar_t waterPath[MAX_PATH];
    if (!ResolveShaderPath(L"DeferredGeometry.hlsl", geometryPath, MAX_PATH) ||
        !ResolveShaderPath(L"DeferredLighting.hlsl", lightingPath, MAX_PATH) ||
        !ResolveShaderPath(L"GBufferDebug.hlsl", debugPath, MAX_PATH) ||
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

// что может читать шейдер
bool RenderingSystem::CreateDeferredLightingRootSignature()
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

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 2;
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
        IID_PPV_ARGS(&m_deferredLightingRootSignature)));
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

//сцена рисуется не в один цветовой буфер, а сразу в несколько текстур GBuffer
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
        commandList->DrawInstanced(3, 1, 0, 0);
    }
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
        const float pointRange = (std::max)(dominantExtent * 0.65f, 600.0f);
        const float spotRange = (std::max)(dominantExtent * 0.90f, 900.0f);

        // Imported Sponza lighting setup: directional + point + spot.
        cb.LightDirection = XMFLOAT4(-0.4f, -1.0f, -0.2f, 0.0f);
        cb.LightColor = XMFLOAT4(0.92f, 0.91f, 0.89f, 0.22f);
        cb.AmbientColor = XMFLOAT4(0.010f, 0.010f, 0.012f, 1.0f);
        cb.LightCounts = XMFLOAT4(1.0f, 1.0f, 0.0f, 0.0f);

        cb.PointLightPositionRange[0] = XMFLOAT4(
            sceneCenter.x - sceneExtents.x * 0.35f,
            sceneCenter.y + sceneExtents.y * 0.25f,
            sceneCenter.z - sceneExtents.z * 0.10f,
            pointRange);
        cb.PointLightColorIntensity[0] = XMFLOAT4(0.12f, 0.48f, 1.00f, 7.50f);

        cb.SpotLightPositionRange[0] = XMFLOAT4(
            sceneCenter.x + sceneExtents.x * 0.28f,
            sceneCenter.y + sceneExtents.y * 0.75f,
            sceneCenter.z - sceneExtents.z * 0.25f,
            spotRange);
        cb.SpotLightDirectionCosine[0] = XMFLOAT4(-0.22f, -0.95f, 0.18f, 0.82f);
        cb.SpotLightColorIntensity[0] = XMFLOAT4(1.00f, 0.38f, 0.10f, 8.00f);
    }
    else
    {
        const float pointRange = (std::max)(dominantExtent * 0.95f, 900.0f);

        cb.LightDirection = XMFLOAT4(0.88f, -1.0f, -0.34f, 0.0f);
        cb.LightColor = XMFLOAT4(1.00f, 0.95f, 0.88f, 3.40f);
        cb.AmbientColor = XMFLOAT4(0.09f, 0.10f, 0.12f, 1.0f);
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

    XMMATRIX invView = XMMatrixInverse(nullptr, view);
    XMMATRIX invProj = XMMatrixInverse(nullptr, proj);
    XMStoreFloat4x4(&cb.InvView, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&cb.InvProj, XMMatrixTranspose(invProj));

    std::memcpy(m_deferredLightCBMappedData, &cb, sizeof(cb));
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

