#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <array>
#include <string>
#include <vector>

#include "tiny_obj_loader.h"
#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct PerObjectCB
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT4 UVTransform;
    XMFLOAT4 TimeParams;
    XMFLOAT4 TessellationParams;
};

struct Submesh
{
    UINT IndexStart = 0;
    UINT IndexCount = 0;
    int MaterialId = -1;
    UINT SrvIndex = 0;
};

struct BoundingBox
{
    XMFLOAT3 Min = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 Max = { 0.0f, 0.0f, 0.0f };
};

struct MeshData
{
    ComPtr<ID3D12Resource> VertexBuffer;
    ComPtr<ID3D12Resource> IndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW VbView{};
    D3D12_INDEX_BUFFER_VIEW IbView{};
    UINT IndexCount = 0;
    bool Use32BitIndices = false;
    std::vector<Submesh> Submeshes;
    BoundingBox BoundsLocal{};
    UINT DiffuseSrvIndex = UINT_MAX;
    UINT NormalSrvIndex = UINT_MAX;
};

struct SceneObject
{
    XMFLOAT4X4 World{};
    BoundingBox BoundsLocal{};
    BoundingBox BoundsWorld{};
    UINT DiffuseSrvIndex = UINT_MAX;
    bool Visible = true;
};

struct Frustum
{
    std::array<XMFLOAT4, 6> Planes{};
};

struct OctreeNode
{
    BoundingBox Bounds{};
    std::vector<UINT> ObjectIndices;
    std::array<int, 8> Children = { -1, -1, -1, -1, -1, -1, -1, -1 };
    bool IsLeaf = true;
};

class D3D12Context
{
public:
    enum class Scene
    {
        HighPlane,
        Sponza,
        HighPolyDisplacement,
        ChickenField
    };

    void SetTime(float t);
    void SetUVTiling(float x, float y);
    void SetUVScrollSpeed(float uSpeed, float vSpeed);

    bool Initialize(HWND hwnd, UINT width, UINT height);
    bool LoadScene(Scene scene);
    void Shutdown();
    void SetFrustumCullingEnabled(bool enabled);
    bool IsFrustumCullingEnabled() const;
    void SetOctreeEnabled(bool enabled);
    bool IsOctreeEnabled() const;

    void Render(float r, float g, float b, float a);
    void SetRotation(float t);
    void BeginFrame(ID3D12PipelineState* initialState = nullptr);
    void EndFrame();
    void UpdateSceneConstants();
    void DrawSceneGeometry(
        ID3D12GraphicsCommandList* commandList,
        UINT textureRootParameterIndex,
        UINT displacementRootParameterIndex = UINT_MAX);

    void UpdateCameraOrbit(
        float deltaTime,
        float rotateSpeed,
        float dollySpeed,
        bool orbitRotate,
        bool dolly,
        float mouseDeltaX,
        float mouseDeltaY);
    void UpdateCameraMove(float deltaTime, float forwardInput, float strafeInput, float moveSpeed);

    ID3D12Device* GetDevice() const;
    ID3D12GraphicsCommandList* GetCommandList() const;
    ID3D12DescriptorHeap* GetSceneSRVHeap() const;
    ID3D12RootSignature* GetSceneRootSignature() const;
    D3D12_GPU_VIRTUAL_ADDRESS GetSceneConstantBufferAddress(UINT objectIndex = 0) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentBackBufferRTV() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetDepthStencilView() const;
    D3D12_VIEWPORT GetViewport() const;
    D3D12_RECT GetScissorRect() const;
    UINT GetWidth() const;
    UINT GetHeight() const;
    DirectX::XMFLOAT3 GetCameraPosition() const;
    DirectX::XMFLOAT3 GetCameraTarget() const;
    DirectX::XMFLOAT3 GetSceneBoundsMin() const;
    DirectX::XMFLOAT3 GetSceneBoundsMax() const;
    DirectX::XMFLOAT3 GetSceneCenter() const;
    DirectX::XMFLOAT3 GetSceneExtents() const;
    Scene GetCurrentScene() const;
    float GetTime() const;

private:
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
    bool CreateSolidColorTexture(UINT32 rgba, UINT srvIndex);
    bool CreateSRVHeap(UINT numDescriptors);

    void UpdateCB(const DirectX::XMFLOAT4X4& worldMatrix, UINT objectIndex, UINT lodIndex = 0);
    void BuildSceneObjects();
    void UpdateObjectVisibility();
    void BuildOctree();
    int BuildOctreeNode(const BoundingBox& bounds, const std::vector<UINT>& objectIndices, UINT depth);
    void QueryOctreeVisible(int nodeIndex, const Frustum& frustum, std::vector<UINT>& visibleIndices) const;
    Frustum BuildCameraFrustum() const;
    DirectX::XMFLOAT4 NormalizePlane(const DirectX::XMFLOAT4& plane) const;
    bool IntersectsFrustum(const BoundingBox& bounds, const Frustum& frustum) const;
    bool ContainsBounds(const BoundingBox& outer, const BoundingBox& inner) const;
    BoundingBox TransformBoundingBox(const BoundingBox& localBounds, const DirectX::XMFLOAT4X4& worldMatrix) const;
    MeshData CaptureCurrentMesh() const;
    void ApplyMesh(const MeshData& mesh);
    UINT SelectCrowdLodIndex(const SceneObject& object) const;
    void WaitForFrame(UINT frameIndex);
    UINT64 SignalCurrentFrame();
    void WaitForGPU();

private:
    UINT m_width = 0;
    UINT m_height = 0;

    std::vector<ComPtr<ID3D12Resource>> m_textures;
    std::vector<ComPtr<ID3D12Resource>> m_textureUploads;

    ComPtr<ID3D12Device> m_device;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    static const UINT FrameCount = 2;
    ComPtr<ID3D12CommandAllocator> m_commandAllocators[FrameCount];
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    UINT m_frameIndex = 0;
    UINT64 m_frameFenceValues[FrameCount] = {};

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
    ComPtr<ID3DBlob> m_hs;
    ComPtr<ID3DBlob> m_ds;
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
    UINT m_constantBufferStride = 0;
    static constexpr UINT MaxSceneObjects = 2048;

    float m_time = 0.0f;
    float m_rotationT = 0.0f;

    XMFLOAT2 m_uvTiling = { 1.0f, 1.0f };
    XMFLOAT2 m_uvScrollSpeed = { 0.0f, 0.0f };

    DirectX::XMFLOAT3 m_cameraTarget = { 0.0f, 220.0f, 0.0f };
    float m_cameraDistance = 1150.0f;
    float m_cameraYaw = 0.0f;
    float m_cameraPitch = -0.22f;
    XMFLOAT3 m_cameraPos = { 0.0f, 420.0f, -1150.0f };

    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_srvDescriptorSize = 0;

    std::vector<Submesh> m_submeshes;
    std::vector<MeshData> m_lodMeshes;
    std::vector<SceneObject> m_sceneObjects;
    std::vector<std::string> m_materialDiffusePaths;
    std::vector<UINT> m_materialToSrv;

    BoundingBox m_modelBoundsLocal{};
    DirectX::XMFLOAT3 m_sceneBoundsMin = { -1.0f, -1.0f, -1.0f };
    DirectX::XMFLOAT3 m_sceneBoundsMax = { 1.0f, 1.0f, 1.0f };
    UINT m_displacementSrvIndex = 0;
    UINT m_normalMapSrvIndex = 0;
    UINT m_baseColorSrvIndex = 0;
    UINT m_roughnessSrvIndex = 0;
    Scene m_currentScene = Scene::HighPlane;
    bool m_frustumCullingEnabled = true;
    bool m_octreeEnabled = true;
    std::vector<OctreeNode> m_octreeNodes;
    int m_octreeRoot = -1;
    static constexpr UINT OctreeMaxDepth = 6;
    static constexpr UINT OctreeMaxObjectsPerNode = 12;
};
