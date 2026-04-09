#pragma once

#include <windows.h>

#include <DirectXMath.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl.h>

#include "D3D12Context.h"
#include "GBuffer.h"

class RenderingSystem
{
public:
    enum class Technique
    {
        Forward,
        Deferred
    };

    using Scene = D3D12Context::Scene;

    bool Initialize(HWND hwnd, UINT width, UINT height);
    bool LoadScene(Scene scene);
    void Shutdown();

    void SetTechnique(Technique technique);
    Technique GetTechnique() const;
    Scene GetCurrentScene() const;
    void SetFrustumCullingEnabled(bool enabled);
    bool IsFrustumCullingEnabled() const;
    void SetOctreeEnabled(bool enabled);
    bool IsOctreeEnabled() const;

    void SetClearColor(float r, float g, float b, float a);
    void SetTime(float timeSeconds);
    void SetUVTiling(float x, float y);
    void SetUVScrollSpeed(float uSpeed, float vSpeed);

    void UpdateCameraOrbit(
        float deltaTime,
        float rotateSpeed,
        float dollySpeed,
        bool orbitRotate,
        bool dolly,
        float mouseDeltaX,
        float mouseDeltaY);

    void RenderFrame();

private:
    void RenderForwardFrame();
    void RenderDeferredFrame();
    void RenderOpaqueStage();
    void RenderLightingStage();
    void RenderGBufferDebugOverlay();
    void RenderTransparentStage();
    bool InitializeDeferredResources();
    bool CompileDeferredShaders();
    bool CreateDeferredLightingRootSignature();
    bool CreateDeferredGeometryPipeline();
    bool CreateDeferredLightingPipeline();
    bool CreateDebugOverlayRootSignature();
    bool CreateDebugOverlayPipeline();
    bool CreateWaterRootSignature();
    bool CreateWaterPipeline();
    bool CreateLightingConstantBuffer();
    bool CreateWaterConstantBuffer();
    void UpdateLightingConstants();
    void UpdateWaterConstants();

private:
    static constexpr UINT MaxPointLights = 6;
    static constexpr UINT MaxSpotLights = 4;

    struct DeferredLightCB
    {
        DirectX::XMFLOAT4 LightDirection;
        DirectX::XMFLOAT4 LightColor;
        DirectX::XMFLOAT4 AmbientColor;
        DirectX::XMFLOAT4 LightCounts;
        DirectX::XMFLOAT4 PointLightPositionRange[MaxPointLights];
        DirectX::XMFLOAT4 PointLightColorIntensity[MaxPointLights];
        DirectX::XMFLOAT4 SpotLightPositionRange[MaxSpotLights];
        DirectX::XMFLOAT4 SpotLightDirectionCosine[MaxSpotLights];
        DirectX::XMFLOAT4 SpotLightColorIntensity[MaxSpotLights];
        DirectX::XMFLOAT4 ScreenSize;
        DirectX::XMFLOAT4X4 InvView;
        DirectX::XMFLOAT4X4 InvProj;
    };

    struct WaterCB
    {
        DirectX::XMFLOAT4X4 View;
        DirectX::XMFLOAT4X4 Proj;
        DirectX::XMFLOAT4 CameraPos;
        DirectX::XMFLOAT4 WaterOrigin;
        DirectX::XMFLOAT4 WaterSize;
        DirectX::XMFLOAT4 WaterColor;
        DirectX::XMFLOAT4 WaveA;
        DirectX::XMFLOAT4 WaveB;
    };

private:
    D3D12Context m_context;
    GBuffer m_gbuffer;
    Technique m_technique = Technique::Forward;
    float m_clearColor[4] = { 0.48f, 0.52f, 0.80f, 1.0f };
    UINT m_width = 0;
    UINT m_height = 0;

    Microsoft::WRL::ComPtr<ID3DBlob> m_deferredGeometryVS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_deferredGeometryHS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_deferredGeometryDS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_deferredGeometryPS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_deferredLightingVS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_deferredLightingPS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_debugOverlayVS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_debugOverlayPS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_waterVS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_waterHS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_waterDS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_waterPS;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_deferredLightingRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_debugOverlayRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_waterRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_deferredGeometryPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_deferredLightingPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_debugOverlayPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_waterPSO;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_deferredLightConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_waterConstantBuffer;
    UINT8* m_deferredLightCBMappedData = nullptr;
    UINT8* m_waterCBMappedData = nullptr;
};
