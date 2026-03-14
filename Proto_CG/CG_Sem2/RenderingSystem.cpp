#include "RenderingSystem.h"

#include <array>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
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

void RenderingSystem::Shutdown()
{
    if (m_deferredLightCBMappedData != nullptr && m_deferredLightConstantBuffer != nullptr)
    {
        m_deferredLightConstantBuffer->Unmap(0, nullptr);
        m_deferredLightCBMappedData = nullptr;
    }

    m_deferredLightConstantBuffer.Reset();
    m_deferredGeometryPSO.Reset();
    m_deferredLightingPSO.Reset();
    m_deferredLightingRootSignature.Reset();
    m_deferredGeometryVS.Reset();
    m_deferredGeometryPS.Reset();
    m_deferredLightingVS.Reset();
    m_deferredLightingPS.Reset();
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
    RenderTransparentStage();
    m_context.EndFrame();
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
    m_context.DrawSceneGeometry(commandList, 1);

    m_gbuffer.EndGeometryPass(commandList);
}

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
}

bool RenderingSystem::InitializeDeferredResources()
{
    return CompileDeferredShaders() &&
        CreateDeferredLightingRootSignature() &&
        CreateDeferredGeometryPipeline() &&
        CreateDeferredLightingPipeline() &&
        CreateLightingConstantBuffer();
}

bool RenderingSystem::CompileDeferredShaders()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    wchar_t geometryPath[MAX_PATH];
    wchar_t lightingPath[MAX_PATH];
    if (!ResolveShaderPath(L"DeferredGeometry.hlsl", geometryPath, MAX_PATH) ||
        !ResolveShaderPath(L"DeferredLighting.hlsl", lightingPath, MAX_PATH))
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
    return SUCCEEDED(hr);
}

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
    pso.PS = { m_deferredGeometryPS->GetBufferPointer(), m_deferredGeometryPS->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
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
    const float pointRange = (std::max)(dominantExtent * 0.65f, 600.0f);
    const float spotRange = (std::max)(dominantExtent * 0.90f, 900.0f);

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
