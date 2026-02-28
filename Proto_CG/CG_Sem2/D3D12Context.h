#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <string>
#include <vector>             
#include "tiny_obj_loader.h"
#include <d3dcompiler.h>
using Microsoft::WRL::ComPtr;
using namespace DirectX;
using Microsoft::WRL::ComPtr;

// параметры модели, камеры и текстуры
struct PerObjectCB
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT4 UVTransform; // для тайлинга и офсета 
    XMFLOAT4 TimeParams;
};

// кусок модели 1 матреиалом
struct Submesh
{
    UINT IndexStart = 0;
    UINT IndexCount = 0;
    int  MaterialId = -1;
    UINT SrvIndex = 0;
};

class D3D12Context
{
public:
    void SetTime(float t);
    void SetUVTiling(float x, float y);
    void SetUVScrollSpeed(float uSpeed, float vSpeed);

    bool Initialize(HWND hwnd, UINT width, UINT height);
    void Shutdown();

    void Render(float r, float g, float b, float a);
    void SetRotation(float t);

    void UpdateCameraOrbit(float deltaTime,
        float rotateSpeed, float dollySpeed,
        bool orbitRotate, bool dolly,
        float mouseDeltaX, float mouseDeltaY);

private:

    float   m_time = 0.0f; // текущее время сцены

    XMFLOAT2 m_uvTiling = { 1.0f, 1.0f }; // тайлинг
    XMFLOAT2 m_uvScrollSpeed = { 0.0f, 0.0f }; // скорость движения текстуры

    bool CreateDevice();
    bool CreateCommandObjects();
    bool CreateSwapChain(HWND hwnd);
    bool CreateRTV();
    bool CreateDepthStencil();
    bool CreateFence();

    bool CreateRootSignature();
    bool CreatePipelineState();
    bool CreateGeometry();
    bool LoadModelFromOBJ(const char* objPath, const char* mtlBaseDir);
    bool CreateConstantBuffer();
    bool CompileShaders();
    bool CreateTextureFromFile(const char* filePath, UINT srvIndex);
    bool CreateSRVHeap(UINT numDescriptors);

    void UpdateCB();
    void WaitForGPU();

private:
    UINT m_width = 0;
    UINT m_height = 0;

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_textures;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_textureUploads;

    ComPtr<ID3D12Device> m_device;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;

    static const UINT FrameCount = 2;
    UINT m_frameIndex = 0;

    ComPtr<ID3D12Resource> m_backBuffers[FrameCount];
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    UINT m_rtvDescriptorSize = 0;

    ComPtr<ID3D12Resource> m_depthBuffer;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;

    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue = 0;
    HANDLE m_fenceEvent = nullptr;

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pso;

    ComPtr<ID3DBlob> m_vs;
    ComPtr<ID3DBlob> m_ps;

    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vbView{};
    D3D12_INDEX_BUFFER_VIEW m_ibView{};
    UINT m_indexCount = 0;
    bool m_use32BitIndices = false;

    ComPtr<ID3D12Resource> m_constantBuffer;
    PerObjectCB m_cbData{};
    UINT8* m_cbMappedData = nullptr;

    float m_rotationT = 0.f;

    // обзор каемры
    DirectX::XMFLOAT3 m_cameraTarget = { 0.0f, 0.2f, 0.0f };
    float m_cameraDistance = 15.0f;   // растояние
    float m_cameraYaw = 0.0f;
    float m_cameraPitch = -0.5f;  // высота

    // расположение камеры
    XMFLOAT3 m_cameraPos = { 0.0f, 5.0f, -20.0f };

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_srvDescriptorSize = 0;

    // диапазоны индексов по материалам
    std::vector<Submesh> m_submeshes;

    // текстуры материалов из MTL 
    std::vector<std::string> m_materialDiffusePaths;

    // материал 
    std::vector<UINT> m_materialToSrv;
};