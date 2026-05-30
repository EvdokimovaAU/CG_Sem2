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
    void UpdateCameraMove(float deltaTime, float forwardInput, float strafeInput, float moveSpeed);

    void RenderFrame();

private:
    struct DeferredLightCB;

    void RenderForwardFrame();
    void RenderDeferredFrame();
    void RenderShadowStage();
    void RenderOpaqueStage();
    void RenderLightingStage();
    void RenderPostProcessStage();
    void RenderGBufferDebugOverlay();
    void UpdateParticleSimulation();
    void RenderParticleStage();
    void RenderTransparentStage();
    bool InitializeDeferredResources();
    bool CompileDeferredShaders();
    bool CreateShadowResources();
    bool CreateShadowRootSignature();
    bool CreateShadowPipeline();
    bool CreateHdrResources();
    bool CreateLightingSrvHeap();
    bool CreateShadowMaskTexture();
    bool CreateDeferredLightingRootSignature();
    bool CreateDeferredGeometryPipeline();
    bool CreateDeferredLightingPipeline();
    bool CreatePostProcessRootSignature();
    bool CreatePostProcessPipeline();
    bool CreateDebugOverlayRootSignature();
    bool CreateDebugOverlayPipeline();
    bool CreateParticleRootSignature();
    bool CreateParticleSimulationPipeline();
    bool CreateParticleRenderPipeline();
    bool CreateParticleResources();
    bool CreateParticleTexture();
    bool CreateParticleConstantBuffers();
    bool CreateFireRootSignature();
    bool CreateFireSimulationPipeline();
    bool CreateFireRenderPipeline();
    bool CreateFireResources();
    bool CreateFireConstantBuffers();
    bool CreateWaterRootSignature();
    bool CreateWaterPipeline();
    bool CreateLightingConstantBuffer();
    bool CreateWaterConstantBuffer();
    void InitializeParticleData();
    void ResetParticleCounters(ID3D12GraphicsCommandList* commandList, bool resetSourceCounter);
    void UpdateParticleSimulationConstants(float deltaTime);
    void UpdateParticleRenderConstants();
    void UpdateFireSimulation();
    void RenderFireStage();
    void InitializeFireData();
    void ResetFireCounters(ID3D12GraphicsCommandList* commandList, bool resetSourceCounter);
    void UpdateFireSimulationConstants(float deltaTime);
    void UpdateFireRenderConstants();
    void UpdateLightingConstants();
    void UpdateShadowMatrices(DeferredLightCB& cb) const;
    void UpdateWaterConstants();

private:
    static constexpr UINT MaxPointLights = 6;
    static constexpr UINT MaxSpotLights = 4;
    static constexpr UINT ShadowCascadeCount = 4; // каскады
    static constexpr UINT ShadowMapResolution = 2048;
    static constexpr UINT MaxDustParticles = 512;
    static constexpr UINT SphereDustParticles = 192;
    static constexpr UINT MaxFireParticles = 256;

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
        DirectX::XMFLOAT4 CascadeSplits; // границы каскадов по глубине камеры
        DirectX::XMFLOAT4 ShadowParams;
        DirectX::XMFLOAT4X4 View;
        DirectX::XMFLOAT4X4 LightViewProj[ShadowCascadeCount]; // сами матрицы каскадов
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

    struct ParticleSimCB
    {
        DirectX::XMFLOAT4 DeltaTimeTime;
        DirectX::XMFLOAT4 BoundsMin;
        DirectX::XMFLOAT4 BoundsMax;
        DirectX::XMFLOAT4 NoiseParams;
        DirectX::XMFLOAT4 EmitterPosition;
        DirectX::XMFLOAT4 SphereData;
    };

    struct ParticleRenderCB
    {
        DirectX::XMFLOAT4X4 ViewProj;
        DirectX::XMFLOAT4 CameraRight;
        DirectX::XMFLOAT4 CameraUp;
        DirectX::XMFLOAT4 DustColor;
        DirectX::XMFLOAT4 EffectParams;
    };

    struct DustParticleCPU
    {
        DirectX::XMFLOAT3 Position;
        float Size = 1.0f;
        DirectX::XMFLOAT3 Velocity;
        float Seed = 0.0f;
        float Age = 0.0f;
        float Lifetime = 1.0f;
        float Kind = 0.0f;
    };

    struct FireParticleCPU
    {
        DirectX::XMFLOAT3 Position;
        float Size = 1.0f;
        DirectX::XMFLOAT3 Velocity;
        float Seed = 0.0f;
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
    Microsoft::WRL::ComPtr<ID3DBlob> m_postProcessVS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_postProcessPS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_shadowVS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_shadowHS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_shadowDS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_debugOverlayVS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_debugOverlayPS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_particleVS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_particleGS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_particlePS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_particleCS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_fireVS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_fireGS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_firePS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_fireCS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_waterVS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_waterHS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_waterDS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_waterPS;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_shadowRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_deferredLightingRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_postProcessRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_debugOverlayRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_particleGraphicsRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_particleComputeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_fireGraphicsRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_fireComputeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_waterRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_shadowPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_deferredGeometryPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_deferredLightingPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_postProcessPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_debugOverlayPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_particleGraphicsPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_particleComputePSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_fireGraphicsPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_fireComputePSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_waterPSO;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowMap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_hdrColorBuffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_shadowDsvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_hdrRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_lightingSrvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_postProcessSrvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowMaskTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowMaskTextureUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_deferredLightConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_particleSimulationConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_particleRenderConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_fireSimulationConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_fireRenderConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_waterConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_particleBuffers[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_particleCounterBuffers[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_particleUploadBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_particleCounterUploadBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_particleTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_particleTextureUpload;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_particleHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_fireBuffers[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_fireCounterBuffers[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_fireUploadBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_fireCounterUploadBuffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_fireHeap;
    UINT8* m_deferredLightCBMappedData = nullptr;
    UINT8* m_particleSimulationCBMappedData = nullptr;
    UINT8* m_particleRenderCBMappedData = nullptr;
    UINT8* m_fireSimulationCBMappedData = nullptr;
    UINT8* m_fireRenderCBMappedData = nullptr;
    UINT8* m_waterCBMappedData = nullptr;
    UINT m_particleDescriptorSize = 0;
    UINT m_fireDescriptorSize = 0;
    UINT m_shadowDsvDescriptorSize = 0;
    UINT m_hdrRtvDescriptorSize = 0;
    UINT m_lightingSrvDescriptorSize = 0;
    D3D12_RESOURCE_STATES m_shadowMapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES m_hdrColorBufferState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bool m_shadowMaskUploaded = false;
    UINT m_particleSourceIndex = 0;
    UINT m_fireSourceIndex = 0;
    bool m_particleDataInitialized = false;
    bool m_particleTextureUploaded = false;
    bool m_fireDataInitialized = false;
    float m_previousParticleTime = 0.0f;
    float m_previousFireTime = 0.0f;
};
