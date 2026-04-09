#define STB_IMAGE_IMPLEMENTATION
#include "D3D12Context.h"
#include "stb_image.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <vector>
#include <wrl.h> 
#include <cstring>
#include <cmath>
#include <algorithm>
#include <limits>
#include <unordered_map>
#include <filesystem>

#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")

using namespace DirectX;

namespace
{
    XMFLOAT4X4 MakeIdentityMatrix()
    {
        XMFLOAT4X4 matrix{};
        XMStoreFloat4x4(&matrix, XMMatrixIdentity());
        return matrix;
    }
}

// инициализация 
bool D3D12Context::Initialize(HWND hwnd, UINT width, UINT height)
{
    static constexpr UINT MaxSrvCount = 128;

    m_width = width;
    m_height = height;

    if (!CreateDevice()) return false;
    if (!CreateCommandObjects()) return false;
    if (!CreateSwapChain(hwnd)) return false;
    if (!CreateRTV()) return false;
    if (!CreateDepthStencil()) return false;
    if (!CreateFence()) return false;
    if (!CompileShaders()) return false;
    if (!CreateRootSignature()) return false;
    if (!CreatePipelineState()) return false;

    if (!CreateSRVHeap(MaxSrvCount)) return false;
    if (!LoadScene(Scene::HighPlane)) return false;
    if (!CreateConstantBuffer()) return false;
    return true;

    // Подготовка путей
    char exeDirA[MAX_PATH];
    GetModuleFileNameA(nullptr, exeDirA, MAX_PATH);
    char* lastSlash = strrchr(exeDirA, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    std::string exeDir = exeDirA;

    std::string modelsDir = exeDir + "models\\";
    std::string objPath = modelsDir + "high plane.obj";
    std::string mtlDir = modelsDir;
    std::string baseColorPath = modelsDir + "textures\\New_Graph_basecolor.jpg";
    std::string displacementPath = modelsDir + "textures\\New_Graph_height.jpg";
    std::string normalMapPath = modelsDir + "textures\\New_Graph_normal.jpg";
    std::string roughnessPath = modelsDir + "textures\\New_Graph_roughness.jpg";


  
    // Reset командного листа
    m_commandAllocators[m_frameIndex]->Reset();
    m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr);


    //загрузка 2 тетктсур в  srv0 и srv1
    if (!CreateSolidColorTexture(0xffffffffu, 0))
        return false;


    // загрузка модели
    bool modelLoaded = LoadModelFromOBJ(objPath.c_str(), mtlDir.c_str());

    if (!modelLoaded)
    {
        if (!CreateGeometry())
            return false;

        m_submeshes.clear();
        Submesh sm{};
        sm.IndexStart = 0;
        sm.IndexCount = m_indexCount;
        sm.MaterialId = -1;
        sm.SrvIndex = 0;
        m_submeshes.push_back(sm);
    }

    m_baseColorSrvIndex = 125;
    if (!CreateTextureFromFile(baseColorPath.c_str(), m_baseColorSrvIndex))
    {
        m_baseColorSrvIndex = 0;
    }

    m_displacementSrvIndex = 127;
    if (!CreateTextureFromFile(displacementPath.c_str(), m_displacementSrvIndex))
    {
        m_displacementSrvIndex = 0;
    }

    m_normalMapSrvIndex = 126;
    if (!CreateTextureFromFile(normalMapPath.c_str(), m_normalMapSrvIndex))
    {
        m_normalMapSrvIndex = 0;
    }

    m_roughnessSrvIndex = 124;
    if (!CreateTextureFromFile(roughnessPath.c_str(), m_roughnessSrvIndex))
    {
        m_roughnessSrvIndex = 0;
    }

    if (m_baseColorSrvIndex != 0)
    {
        for (auto& sm : m_submeshes)
        {
            sm.SrvIndex = m_baseColorSrvIndex;
        }
    }

    const XMFLOAT3 sceneCenter = GetSceneCenter();
    const XMFLOAT3 sceneExtents = GetSceneExtents();
    m_cameraTarget = sceneCenter;
    m_cameraDistance = (std::max)(25.0f, (sceneExtents.x + sceneExtents.y + sceneExtents.z) * 1.35f);
    m_cameraYaw = 0.0f;
    m_cameraPitch = -0.75f;

   // закрытие списка команд
    m_commandList->Close();
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForGPU();

  // константный буфер
    if (!CreateConstantBuffer())
        return false;

    return true;
}

// движение камеры
bool D3D12Context::LoadScene(Scene scene)
{
    namespace fs = std::filesystem;

    WaitForGPU();

    char exeDirA[MAX_PATH];
    GetModuleFileNameA(nullptr, exeDirA, MAX_PATH);
    char* lastSlash = strrchr(exeDirA, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    const fs::path modelsDir = fs::path(exeDirA) / "models";

    const bool isHighPlaneScene = (scene == Scene::HighPlane || scene == Scene::HighPolyDisplacement);
    const bool isCrowdScene = (scene == Scene::ChickenField);
    const bool isDisplacementScene = (scene == Scene::HighPolyDisplacement);
    const bool useDisplacement = isHighPlaneScene;
    const fs::path objPath = modelsDir / (
        isCrowdScene ? "Dracula-bl.obj" :
        (isHighPlaneScene ? "high plane.obj" :
            (scene == Scene::HighPlane ? "high plane.obj" : "sponza.obj")));
    const std::string mtlDir = modelsDir.string();

    m_vertexBuffer.Reset();
    m_indexBuffer.Reset();
    m_textures.clear();
    m_textureUploads.clear();
    m_submeshes.clear();
    m_materialDiffusePaths.clear();
    m_materialToSrv.clear();
    m_sceneObjects.clear();
    m_octreeNodes.clear();
    m_octreeRoot = -1;
    m_indexCount = 0;
    m_use32BitIndices = false;
    m_baseColorSrvIndex = 0;
    m_displacementSrvIndex = 0;
    m_normalMapSrvIndex = 0;
    m_roughnessSrvIndex = 0;
    m_currentScene = scene;

    m_commandAllocators[m_frameIndex]->Reset();
    m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr);

    if (!CreateSolidColorTexture(0xffffffffu, 0))
        return false;
    if (!CreateSolidColorTexture(0xffffffffu, 124))
        return false;
    if (!CreateSolidColorTexture(0xffffffffu, 125))
        return false;
    if (!CreateSolidColorTexture(0xffffffffu, 126))
        return false;
    if (!CreateSolidColorTexture(0xffffffffu, 127))
        return false;

    bool modelLoaded = LoadModelFromOBJ(objPath.string().c_str(), mtlDir.c_str());

    if (!modelLoaded)
    {
        if (!CreateGeometry())
            return false;

        m_submeshes.clear();
        Submesh sm{};
        sm.IndexStart = 0;
        sm.IndexCount = m_indexCount;
        sm.MaterialId = -1;
        sm.SrvIndex = 0;
        m_submeshes.push_back(sm);
    }

    if (isHighPlaneScene)
    {
        const std::string texturesDir = (modelsDir / "textures").string();

        m_baseColorSrvIndex = 125;
        if (!CreateTextureFromFile((texturesDir + "\\New_Graph_basecolor.jpg").c_str(), m_baseColorSrvIndex))
        {
            m_baseColorSrvIndex = 0;
        }

        if (useDisplacement)
        {
            m_displacementSrvIndex = 127;
            if (!CreateTextureFromFile((texturesDir + "\\New_Graph_height.jpg").c_str(), m_displacementSrvIndex))
            {
                m_displacementSrvIndex = 0;
            }
        }

        m_normalMapSrvIndex = 126;
        if (!CreateTextureFromFile((texturesDir + "\\New_Graph_normal.jpg").c_str(), m_normalMapSrvIndex))
        {
            m_normalMapSrvIndex = 0;
        }

        m_roughnessSrvIndex = 124;
        if (!CreateTextureFromFile((texturesDir + "\\New_Graph_roughness.jpg").c_str(), m_roughnessSrvIndex))
        {
            m_roughnessSrvIndex = 0;
        }

        if (m_baseColorSrvIndex != 0)
        {
            for (auto& sm : m_submeshes)
            {
                sm.SrvIndex = m_baseColorSrvIndex;
            }
        }
    }
    else if (isCrowdScene)
    {
        const std::string crowdDiffusePath = (modelsDir / "Dracula_texture.png").string();

        m_baseColorSrvIndex = 125;
        if (CreateTextureFromFile(crowdDiffusePath.c_str(), m_baseColorSrvIndex))
        {
            for (auto& sm : m_submeshes)
            {
                sm.SrvIndex = m_baseColorSrvIndex;
            }
        }
        else
        {
            m_baseColorSrvIndex = 0;
        }
    }
    BuildSceneObjects();
    BuildOctree();

    const XMFLOAT3 sceneCenter = GetSceneCenter();
    const XMFLOAT3 sceneExtents = GetSceneExtents();
    if (m_currentScene == Scene::ChickenField)
    {
        m_cameraTarget = XMFLOAT3(sceneCenter.x, sceneCenter.y + sceneExtents.y * 0.12f, sceneCenter.z);
        m_cameraDistance = (std::max)(120.0f, (std::max)(sceneExtents.x, sceneExtents.z) * 0.46f);
        m_cameraYaw = 0.52f;
        m_cameraPitch = -0.20f;
    }
    else
    {
        m_cameraTarget = sceneCenter;
        m_cameraDistance = (std::max)(25.0f, (sceneExtents.x + sceneExtents.y + sceneExtents.z) * 1.35f);
        m_cameraYaw = 0.0f;
        m_cameraPitch = -0.75f;
    }

    UpdateCameraOrbit(0.0f, 0.0f, 0.0f, false, false, 0.0f, 0.0f);

    m_commandList->Close();
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForGPU();

    return true;
}

void D3D12Context::SetFrustumCullingEnabled(bool enabled)
{
    m_frustumCullingEnabled = enabled;
}

bool D3D12Context::IsFrustumCullingEnabled() const
{
    return m_frustumCullingEnabled;
}

void D3D12Context::SetOctreeEnabled(bool enabled)
{
    m_octreeEnabled = enabled;
}

bool D3D12Context::IsOctreeEnabled() const
{
    return m_octreeEnabled;
}

void D3D12Context::UpdateCameraOrbit(float deltaTime,
    float rotateSpeed, float dollySpeed,
    bool orbitRotate, bool dolly,
    float mouseDeltaX, float mouseDeltaY)
{
    // вращение по орбите
    if (orbitRotate)
    {
        m_cameraYaw += mouseDeltaX * rotateSpeed;
        m_cameraPitch += mouseDeltaY * rotateSpeed;

        const float limit = XM_PIDIV2 - 0.01f;
        if (m_cameraPitch > limit)  m_cameraPitch = limit;
        if (m_cameraPitch < -limit) m_cameraPitch = -limit;
    }

    // приближение/отдаление
    if (dolly)
    {
        m_cameraDistance += mouseDeltaY * dollySpeed;
        if (m_cameraDistance < 2.0f)    m_cameraDistance = 2.0f;
        if (m_cameraDistance > 5000.0f) m_cameraDistance = 5000.0f;
    }

    XMVECTOR target = XMLoadFloat3(&m_cameraTarget);

    XMVECTOR offset = XMVectorSet(0.0f, 0.0f, m_cameraDistance, 0.0f);
    XMMATRIX rot = XMMatrixRotationRollPitchYaw(m_cameraPitch, m_cameraYaw, 0.0f);
    offset = XMVector3TransformCoord(offset, rot);

    XMVECTOR eye = XMVectorAdd(target, offset);
    XMStoreFloat3(&m_cameraPos, eye);
}

// рендер 
void D3D12Context::Render(float r, float g, float b, float a)
{
   
    WaitForFrame(m_frameIndex);
    m_commandAllocators[m_frameIndex]->Reset();
    m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), m_pso.Get());

   
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = m_backBuffers[m_frameIndex].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetCurrentBackBufferRTV();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = GetDepthStencilView();

    float clearColor[4] = { r, g, b, a };
    m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    m_commandList->OMSetRenderTargets(1, &rtv, TRUE, &dsv);

    D3D12_VIEWPORT vp = GetViewport();
    D3D12_RECT sc = GetScissorRect();

    m_commandList->RSSetViewports(1, &vp);
    m_commandList->RSSetScissorRects(1, &sc);
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());


    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, heaps);


    UpdateSceneConstants();
    m_commandList->SetGraphicsRootConstantBufferView(0, GetSceneConstantBufferAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE baseGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE displacementHandle = baseGpu;
    displacementHandle.ptr += SIZE_T(m_displacementSrvIndex) * SIZE_T(m_srvDescriptorSize);
    m_commandList->SetGraphicsRootDescriptorTable(2, displacementHandle);
    D3D12_GPU_DESCRIPTOR_HANDLE normalMapHandle = baseGpu;
    normalMapHandle.ptr += SIZE_T(m_normalMapSrvIndex) * SIZE_T(m_srvDescriptorSize);
    m_commandList->SetGraphicsRootDescriptorTable(3, normalMapHandle);
    D3D12_GPU_DESCRIPTOR_HANDLE roughnessHandle = baseGpu;
    roughnessHandle.ptr += SIZE_T(m_roughnessSrvIndex) * SIZE_T(m_srvDescriptorSize);
    m_commandList->SetGraphicsRootDescriptorTable(4, roughnessHandle);

    // 11) Геометрия
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    m_commandList->IASetVertexBuffers(0, 1, &m_vbView);
    m_commandList->IASetIndexBuffer(&m_ibView);

    if (!m_submeshes.empty())
    {
        for (const auto& sm : m_submeshes)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = baseGpu;
            textureHandle.ptr += SIZE_T(sm.SrvIndex) * SIZE_T(m_srvDescriptorSize);
            m_commandList->SetGraphicsRootDescriptorTable(1, textureHandle);

            m_commandList->DrawIndexedInstanced(
                sm.IndexCount,
                1,
                sm.IndexStart,
                0,
                0);
        }
    }
    else
    {
        m_commandList->SetGraphicsRootDescriptorTable(1, baseGpu);
        m_commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
    }

   
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    m_commandList->ResourceBarrier(1, &barrier);

    // Отправляем команды на GPU и показываем кадр
    m_commandList->Close();
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
    SignalCurrentFrame();
    m_swapChain->Present(1, 0);

    // 15) Стабильно, но медленно
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

// обновление данных для шейдера
void D3D12Context::BuildSceneObjects()
{
    m_sceneObjects.clear();

    const BoundingBox localBounds = m_modelBoundsLocal;
    if (m_currentScene != Scene::ChickenField)
    {
        SceneObject object{};
        object.World = MakeIdentityMatrix();
        object.BoundsLocal = localBounds;
        object.BoundsWorld = localBounds;
        object.DiffuseSrvIndex = UINT_MAX;
        object.Visible = true;
        m_sceneObjects.push_back(object);
        m_sceneBoundsMin = localBounds.Min;
        m_sceneBoundsMax = localBounds.Max;
        return;
    }

    const UINT gridX = 30;
    const UINT gridZ = 30;
    const float crowdScale = 28.0f;
    const float modelWidth = (std::max)((localBounds.Max.x - localBounds.Min.x) * crowdScale, 1.0f);
    const float modelDepth = (std::max)((localBounds.Max.z - localBounds.Min.z) * crowdScale, 1.0f);
    const float modelHeight = (std::max)((localBounds.Max.y - localBounds.Min.y) * crowdScale, 1.0f);
    const float spacingX = (std::max)(modelWidth * 1.28f, 16.0f);
    const float spacingZ = (std::max)(modelDepth * 1.28f, 16.0f);
    const float startX = -0.5f * spacingX * static_cast<float>(gridX - 1);
    const float startZ = -0.5f * spacingZ * static_cast<float>(gridZ - 1);
    const float worldY = -localBounds.Min.y * crowdScale;

    XMFLOAT3 sceneMin(
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)());
    XMFLOAT3 sceneMax(
        -(std::numeric_limits<float>::max)(),
        -(std::numeric_limits<float>::max)(),
        -(std::numeric_limits<float>::max)());

    for (UINT z = 0; z < gridZ; ++z)
    {
        for (UINT x = 0; x < gridX; ++x)
        {
            SceneObject object{};
            const float worldX = startX + static_cast<float>(x) * spacingX;
            const float worldZ = startZ + static_cast<float>(z) * spacingZ;
            XMMATRIX world = XMMatrixScaling(crowdScale, crowdScale, crowdScale) *
                XMMatrixTranslation(worldX, worldY, worldZ);
            XMStoreFloat4x4(&object.World, world);
            object.BoundsLocal = localBounds;
            object.BoundsWorld = TransformBoundingBox(localBounds, object.World);
            object.DiffuseSrvIndex = UINT_MAX;
            object.Visible = true;

            sceneMin.x = (std::min)(sceneMin.x, object.BoundsWorld.Min.x);
            sceneMin.y = (std::min)(sceneMin.y, object.BoundsWorld.Min.y);
            sceneMin.z = (std::min)(sceneMin.z, object.BoundsWorld.Min.z);
            sceneMax.x = (std::max)(sceneMax.x, object.BoundsWorld.Max.x);
            sceneMax.y = (std::max)(sceneMax.y, object.BoundsWorld.Max.y);
            sceneMax.z = (std::max)(sceneMax.z, object.BoundsWorld.Max.z);

            m_sceneObjects.push_back(object);
        }
    }

    sceneMax.y = (std::max)(sceneMax.y, worldY + modelHeight);
    m_sceneBoundsMin = sceneMin;
    m_sceneBoundsMax = sceneMax;
}

void D3D12Context::UpdateObjectVisibility()
{
    if (m_sceneObjects.empty())
    {
        return;
    }

    if (!m_frustumCullingEnabled)
    {
        for (SceneObject& object : m_sceneObjects)
        {
            object.Visible = true;
        }
        return;
    }

    const Frustum frustum = BuildCameraFrustum();

    if (m_octreeEnabled && m_octreeRoot >= 0)
    {
        for (SceneObject& object : m_sceneObjects)
        {
            object.Visible = false;
        }

        std::vector<UINT> visibleIndices;
        visibleIndices.reserve(m_sceneObjects.size());
        QueryOctreeVisible(m_octreeRoot, frustum, visibleIndices);

        for (UINT objectIndex : visibleIndices)
        {
            if (objectIndex < m_sceneObjects.size())
            {
                m_sceneObjects[objectIndex].Visible = true;
            }
        }
        return;
    }

    for (SceneObject& object : m_sceneObjects)
    {
        object.Visible = IntersectsFrustum(object.BoundsWorld, frustum);
    }
}

void D3D12Context::BuildOctree()
{
    m_octreeNodes.clear();
    m_octreeRoot = -1;

    if (m_sceneObjects.empty())
    {
        return;
    }

    std::vector<UINT> objectIndices;
    objectIndices.reserve(m_sceneObjects.size());
    for (UINT i = 0; i < static_cast<UINT>(m_sceneObjects.size()); ++i)
    {
        objectIndices.push_back(i);
    }

    BoundingBox rootBounds{};
    rootBounds.Min = m_sceneBoundsMin;
    rootBounds.Max = m_sceneBoundsMax;
    m_octreeRoot = BuildOctreeNode(rootBounds, objectIndices, 0);
}

int D3D12Context::BuildOctreeNode(const BoundingBox& bounds, const std::vector<UINT>& objectIndices, UINT depth)
{
    const int nodeIndex = static_cast<int>(m_octreeNodes.size());
    m_octreeNodes.emplace_back();
    m_octreeNodes[nodeIndex].Bounds = bounds;
    m_octreeNodes[nodeIndex].ObjectIndices = objectIndices;

    if (depth >= OctreeMaxDepth || objectIndices.size() <= OctreeMaxObjectsPerNode)
    {
        return nodeIndex;
    }

    const XMFLOAT3 center(
        0.5f * (bounds.Min.x + bounds.Max.x),
        0.5f * (bounds.Min.y + bounds.Max.y),
        0.5f * (bounds.Min.z + bounds.Max.z));

    const float extentX = bounds.Max.x - bounds.Min.x;
    const float extentY = bounds.Max.y - bounds.Min.y;
    const float extentZ = bounds.Max.z - bounds.Min.z;
    if (extentX <= 1.0e-3f || extentY <= 1.0e-3f || extentZ <= 1.0e-3f)
    {
        return nodeIndex;
    }

    std::array<BoundingBox, 8> childBounds{};
    for (UINT child = 0; child < 8; ++child)
    {
        BoundingBox childBox{};
        childBox.Min.x = (child & 1u) ? center.x : bounds.Min.x;
        childBox.Max.x = (child & 1u) ? bounds.Max.x : center.x;
        childBox.Min.y = (child & 2u) ? center.y : bounds.Min.y;
        childBox.Max.y = (child & 2u) ? bounds.Max.y : center.y;
        childBox.Min.z = (child & 4u) ? center.z : bounds.Min.z;
        childBox.Max.z = (child & 4u) ? bounds.Max.z : center.z;
        childBounds[child] = childBox;
    }

    std::array<std::vector<UINT>, 8> childObjects;
    std::vector<UINT> remainingObjects;
    remainingObjects.reserve(objectIndices.size());

    for (UINT objectIndex : objectIndices)
    {
        const BoundingBox& objectBounds = m_sceneObjects[objectIndex].BoundsWorld;
        int targetChild = -1;

        for (UINT child = 0; child < 8; ++child)
        {
            if (ContainsBounds(childBounds[child], objectBounds))
            {
                targetChild = static_cast<int>(child);
                break;
            }
        }

        if (targetChild >= 0)
        {
            childObjects[targetChild].push_back(objectIndex);
        }
        else
        {
            remainingObjects.push_back(objectIndex);
        }
    }

    bool createdChild = false;
    m_octreeNodes[nodeIndex].ObjectIndices = std::move(remainingObjects);

    for (UINT child = 0; child < 8; ++child)
    {
        if (childObjects[child].empty())
        {
            continue;
        }

        m_octreeNodes[nodeIndex].Children[child] = BuildOctreeNode(childBounds[child], childObjects[child], depth + 1);
        createdChild = true;
    }

    m_octreeNodes[nodeIndex].IsLeaf = !createdChild;
    return nodeIndex;
}

void D3D12Context::QueryOctreeVisible(int nodeIndex, const Frustum& frustum, std::vector<UINT>& visibleIndices) const
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(m_octreeNodes.size()))
    {
        return;
    }

    const OctreeNode& node = m_octreeNodes[nodeIndex];
    if (!IntersectsFrustum(node.Bounds, frustum))
    {
        return;
    }

    for (UINT objectIndex : node.ObjectIndices)
    {
        if (objectIndex < m_sceneObjects.size() &&
            IntersectsFrustum(m_sceneObjects[objectIndex].BoundsWorld, frustum))
        {
            visibleIndices.push_back(objectIndex);
        }
    }

    if (node.IsLeaf)
    {
        return;
    }

    for (int childIndex : node.Children)
    {
        if (childIndex >= 0)
        {
            QueryOctreeVisible(childIndex, frustum, visibleIndices);
        }
    }
}

Frustum D3D12Context::BuildCameraFrustum() const
{
    const XMVECTOR eye = XMLoadFloat3(&m_cameraPos);
    const XMVECTOR target = XMLoadFloat3(&m_cameraTarget);
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    const XMMATRIX view = XMMatrixLookAtLH(eye, target, up);
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(
        XM_PIDIV4,
        static_cast<float>(m_width) / static_cast<float>(m_height),
        1.0f,
        20000.0f);
    const XMMATRIX viewProj = XMMatrixMultiply(view, proj);

    XMFLOAT4X4 m{};
    XMStoreFloat4x4(&m, viewProj);

    Frustum frustum{};
    frustum.Planes[0] = NormalizePlane(XMFLOAT4(m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41));
    frustum.Planes[1] = NormalizePlane(XMFLOAT4(m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41));
    frustum.Planes[2] = NormalizePlane(XMFLOAT4(m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42));
    frustum.Planes[3] = NormalizePlane(XMFLOAT4(m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42));
    frustum.Planes[4] = NormalizePlane(XMFLOAT4(m._13, m._23, m._33, m._43));
    frustum.Planes[5] = NormalizePlane(XMFLOAT4(m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43));
    return frustum;
}

XMFLOAT4 D3D12Context::NormalizePlane(const XMFLOAT4& plane) const
{
    const float length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
    if (length <= 1.0e-6f)
    {
        return plane;
    }

    const float invLength = 1.0f / length;
    return XMFLOAT4(
        plane.x * invLength,
        plane.y * invLength,
        plane.z * invLength,
        plane.w * invLength);
}

bool D3D12Context::IntersectsFrustum(const BoundingBox& bounds, const Frustum& frustum) const
{
    for (const XMFLOAT4& plane : frustum.Planes)
    {
        XMFLOAT3 positiveVertex{};
        positiveVertex.x = (plane.x >= 0.0f) ? bounds.Max.x : bounds.Min.x;
        positiveVertex.y = (plane.y >= 0.0f) ? bounds.Max.y : bounds.Min.y;
        positiveVertex.z = (plane.z >= 0.0f) ? bounds.Max.z : bounds.Min.z;

        const float distance =
            plane.x * positiveVertex.x +
            plane.y * positiveVertex.y +
            plane.z * positiveVertex.z +
            plane.w;

        if (distance < 0.0f)
        {
            return false;
        }
    }

    return true;
}

bool D3D12Context::ContainsBounds(const BoundingBox& outer, const BoundingBox& inner) const
{
    return
        inner.Min.x >= outer.Min.x && inner.Max.x <= outer.Max.x &&
        inner.Min.y >= outer.Min.y && inner.Max.y <= outer.Max.y &&
        inner.Min.z >= outer.Min.z && inner.Max.z <= outer.Max.z;
}

BoundingBox D3D12Context::TransformBoundingBox(const BoundingBox& localBounds, const XMFLOAT4X4& worldMatrix) const
{
    const XMFLOAT3 corners[8] =
    {
        XMFLOAT3(localBounds.Min.x, localBounds.Min.y, localBounds.Min.z),
        XMFLOAT3(localBounds.Max.x, localBounds.Min.y, localBounds.Min.z),
        XMFLOAT3(localBounds.Min.x, localBounds.Max.y, localBounds.Min.z),
        XMFLOAT3(localBounds.Max.x, localBounds.Max.y, localBounds.Min.z),
        XMFLOAT3(localBounds.Min.x, localBounds.Min.y, localBounds.Max.z),
        XMFLOAT3(localBounds.Max.x, localBounds.Min.y, localBounds.Max.z),
        XMFLOAT3(localBounds.Min.x, localBounds.Max.y, localBounds.Max.z),
        XMFLOAT3(localBounds.Max.x, localBounds.Max.y, localBounds.Max.z)
    };

    BoundingBox transformed{};
    transformed.Min = XMFLOAT3(
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)());
    transformed.Max = XMFLOAT3(
        -(std::numeric_limits<float>::max)(),
        -(std::numeric_limits<float>::max)(),
        -(std::numeric_limits<float>::max)());

    const XMMATRIX world = XMLoadFloat4x4(&worldMatrix);
    for (const XMFLOAT3& corner : corners)
    {
        const XMVECTOR point = XMVector3TransformCoord(XMLoadFloat3(&corner), world);
        XMFLOAT3 transformedPoint{};
        XMStoreFloat3(&transformedPoint, point);

        transformed.Min.x = (std::min)(transformed.Min.x, transformedPoint.x);
        transformed.Min.y = (std::min)(transformed.Min.y, transformedPoint.y);
        transformed.Min.z = (std::min)(transformed.Min.z, transformedPoint.z);
        transformed.Max.x = (std::max)(transformed.Max.x, transformedPoint.x);
        transformed.Max.y = (std::max)(transformed.Max.y, transformedPoint.y);
        transformed.Max.z = (std::max)(transformed.Max.z, transformedPoint.z);
    }

    return transformed;
}

void D3D12Context::UpdateCB(const XMFLOAT4X4& worldMatrix, UINT objectIndex)
{
    if (m_cbMappedData == nullptr)
    {
        return;
    }

    XMVECTOR cameraPos = XMLoadFloat3(&m_cameraPos);
    XMVECTOR cameraTarget = XMLoadFloat3(&m_cameraTarget);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(cameraPos, cameraTarget, up);

    XMMATRIX proj =
        XMMatrixPerspectiveFovLH(
            XM_PIDIV4,
            (float)m_width / (float)m_height,
            1.0f,
            20000.0f);

    const XMMATRIX world = XMLoadFloat4x4(&worldMatrix);
    XMStoreFloat4x4(&m_cbData.World, XMMatrixTranspose(world));
    XMStoreFloat4x4(&m_cbData.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&m_cbData.Proj, XMMatrixTranspose(proj));
    // текстурная анимация
    float uOff = std::fmod(m_time * m_uvScrollSpeed.x, 1.0f);
    float vOff = std::fmod(m_time * m_uvScrollSpeed.y, 1.0f);
    if (uOff < 0.0f) uOff += 1.0f;
    if (vOff < 0.0f) vOff += 1.0f;

    m_cbData.UVTransform = XMFLOAT4(
        m_uvTiling.x, m_uvTiling.y,
        uOff, vOff
    );

    m_cbData.TimeParams = XMFLOAT4(m_time, 1.0f, 0.0f, 0.0f);
    if (m_currentScene == Scene::HighPlane || m_currentScene == Scene::HighPolyDisplacement)
    {
        // More visible displacement with a lower tessellation budget.
        m_cbData.TessellationParams = XMFLOAT4(0.48f, 10.0f, 2.0f, 520.0f);
    }
    else
    {
        m_cbData.TessellationParams = XMFLOAT4(0.0f, 1.0f, 1.0f, 1.0f);
    }

    const UINT safeIndex = (std::min)(objectIndex, MaxSceneObjects - 1);
    std::memcpy(
        m_cbMappedData + size_t(safeIndex) * size_t(m_constantBufferStride),
        &m_cbData,
        sizeof(PerObjectCB));
}

// CPU ждет GPU
void D3D12Context::WaitForGPU()
{
    const UINT64 fenceValue = ++m_fenceValue;
    m_commandQueue->Signal(m_fence.Get(), fenceValue);

    if (m_fence->GetCompletedValue() < fenceValue)
    {
        m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    for (UINT i = 0; i < FrameCount; ++i)
    {
        m_frameFenceValues[i] = fenceValue;
    }
}

void D3D12Context::WaitForFrame(UINT frameIndex)
{
    const UINT64 fenceValue = m_frameFenceValues[frameIndex];
    if (fenceValue == 0)
    {
        return;
    }

    if (m_fence->GetCompletedValue() < fenceValue)
    {
        m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

UINT64 D3D12Context::SignalCurrentFrame()
{
    const UINT64 fenceValue = ++m_fenceValue;
    m_commandQueue->Signal(m_fence.Get(), fenceValue);
    m_frameFenceValues[m_frameIndex] = fenceValue;
    return fenceValue;
}

// загрузка текстуры 
bool D3D12Context::CreateTextureFromFile(const char* filePath, UINT srvIndex)
{
    if (!m_device || !m_commandList || !m_srvHeap)
    {
        OutputDebugStringA("CreateTextureFromFile: device/commandList/srvHeap is null\n");
        return false;
    }
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load(filePath, &w, &h, &comp, 4);
    if (!pixels || w <= 0 || h <= 0)
    {
        OutputDebugStringA(("stbi_load failed: " + std::string(filePath) + "\n").c_str());
        return false;
    }

    std::vector<std::vector<stbi_uc>> mipData;
    std::vector<UINT> mipWidths;
    std::vector<UINT> mipHeights;

    mipData.emplace_back(pixels, pixels + (size_t(w) * size_t(h) * 4));
    mipWidths.push_back(static_cast<UINT>(w));
    mipHeights.push_back(static_cast<UINT>(h));

    while (mipWidths.back() > 1 || mipHeights.back() > 1)
    {
        const UINT srcWidth = mipWidths.back();
        const UINT srcHeight = mipHeights.back();
        const UINT dstWidth = (std::max)(1u, srcWidth / 2u);
        const UINT dstHeight = (std::max)(1u, srcHeight / 2u);

        const std::vector<stbi_uc>& src = mipData.back();
        std::vector<stbi_uc> dst(size_t(dstWidth) * size_t(dstHeight) * 4u, 0);

        for (UINT y = 0; y < dstHeight; ++y)
        {
            for (UINT x = 0; x < dstWidth; ++x)
            {
                for (UINT c = 0; c < 4; ++c)
                {
                    UINT sum = 0;
                    UINT count = 0;

                    for (UINT oy = 0; oy < 2; ++oy)
                    {
                        for (UINT ox = 0; ox < 2; ++ox)
                        {
                            const UINT sx = (std::min)(srcWidth - 1u, x * 2u + ox);
                            const UINT sy = (std::min)(srcHeight - 1u, y * 2u + oy);
                            const size_t srcIndex = (size_t(sy) * size_t(srcWidth) + size_t(sx)) * 4u + c;
                            sum += src[srcIndex];
                            ++count;
                        }
                    }

                    const size_t dstIndex = (size_t(y) * size_t(dstWidth) + size_t(x)) * 4u + c;
                    dst[dstIndex] = static_cast<stbi_uc>(sum / (std::max)(count, 1u));
                }
            }
        }

        mipData.push_back(std::move(dst));
        mipWidths.push_back(dstWidth);
        mipHeights.push_back(dstHeight);
    }

    stbi_image_free(pixels);
    const UINT mipCount = static_cast<UINT>(mipData.size());

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = (UINT64)w;
    texDesc.Height = (UINT)h;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = static_cast<UINT16>(mipCount);
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    Microsoft::WRL::ComPtr<ID3D12Resource> texture; // сюда кладем созданный ресурс текстуры

    // создаем текстуру в видеопамяти
    HRESULT hr = m_device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,  
        nullptr,
        IID_PPV_ARGS(&texture));

    if (FAILED(hr) || !texture)
    {
        OutputDebugStringA(("CreateCommittedResource(DEFAULT) failed: " + std::string(filePath) + "\n").c_str());
        return false;
    }

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipCount);
    std::vector<UINT> numRows(mipCount);
    std::vector<UINT64> rowSizes(mipCount);
    UINT64 totalBytes = 0;

    m_device->GetCopyableFootprints(
        &texDesc,
        0, mipCount, 0,
        footprints.data(),
        numRows.data(),
        rowSizes.data(),
        &totalBytes);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Alignment = 0;
    uploadDesc.Width = totalBytes;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.SampleDesc.Quality = 0;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3D12Resource> upload;

    hr = m_device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&upload));

    if (FAILED(hr) || !upload)
    {
        OutputDebugStringA(("CreateCommittedResource(UPLOAD) failed: " + std::string(filePath) + "\n").c_str());
        return false;
    }

    void* mapped = nullptr;
    hr = upload->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped)
    {
        OutputDebugStringA(("Upload Map failed: " + std::string(filePath) + "\n").c_str());
        return false;
    }

    const UINT bytesPerPixel = 4;
    BYTE* uploadBase = reinterpret_cast<BYTE*>(mapped);

    for (UINT mipIndex = 0; mipIndex < mipCount; ++mipIndex)
    {
        const UINT srcWidth = mipWidths[mipIndex];
        const UINT srcHeight = mipHeights[mipIndex];
        const UINT srcRowBytes = srcWidth * bytesPerPixel;
        BYTE* dstBase = uploadBase + footprints[mipIndex].Offset;
        const std::vector<stbi_uc>& src = mipData[mipIndex];

        for (UINT y = 0; y < srcHeight; ++y)
        {
            BYTE* dstRow = dstBase + y * footprints[mipIndex].Footprint.RowPitch;
            const BYTE* srcRow = src.data() + size_t(y) * size_t(srcRowBytes);
            memcpy(dstRow, srcRow, srcRowBytes);
        }
    }

    upload->Unmap(0, nullptr);

    for (UINT mipIndex = 0; mipIndex < mipCount; ++mipIndex)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = texture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = mipIndex;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[mipIndex];

        m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    m_commandList->ResourceBarrier(1, &barrier);

    
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MostDetailedMip = 0;
    srv.Texture2D.MipLevels = mipCount;
    srv.Texture2D.PlaneSlice = 0;
    srv.Texture2D.ResourceMinLODClamp = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += SIZE_T(srvIndex) * SIZE_T(m_srvDescriptorSize);

    m_device->CreateShaderResourceView(texture.Get(), &srv, cpuHandle);

    
    m_textures.push_back(texture);
    m_textureUploads.push_back(upload);

    OutputDebugStringA(("Texture queued (srv=" + std::to_string(srvIndex) + "): " + std::string(filePath) + "\n").c_str());
    return true;
}

// создание устройства
bool D3D12Context::CreateSolidColorTexture(UINT32 rgba, UINT srvIndex)
{
    if (!m_device || !m_commandList || !m_srvHeap)
    {
        OutputDebugStringA("CreateSolidColorTexture: device/commandList/srvHeap is null\n");
        return false;
    }

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = 1;
    texDesc.Height = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> texture;
    HRESULT hr = m_device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&texture));

    if (FAILED(hr) || !texture)
    {
        OutputDebugStringA("CreateSolidColorTexture: default texture create failed\n");
        return false;
    }

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload;
    hr = m_device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&upload));

    if (FAILED(hr) || !upload)
    {
        OutputDebugStringA("CreateSolidColorTexture: upload texture create failed\n");
        return false;
    }

    void* mapped = nullptr;
    hr = upload->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped)
    {
        OutputDebugStringA("CreateSolidColorTexture: upload map failed\n");
        return false;
    }

    std::memcpy(mapped, &rgba, sizeof(rgba));
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = 1;
    src.PlacedFootprint.Footprint.Height = 1;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += SIZE_T(srvIndex) * SIZE_T(m_srvDescriptorSize);
    m_device->CreateShaderResourceView(texture.Get(), &srv, cpuHandle);

    m_textures.push_back(texture);
    m_textureUploads.push_back(upload);
    return true;
}

bool D3D12Context::CreateDevice()
{
    if (FAILED(D3D12CreateDevice(
        nullptr,
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&m_device))))
    {
        return false;
    }

    return true;
}

// отправка команд
bool D3D12Context::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    if (FAILED(m_device->CreateCommandQueue(&q, IID_PPV_ARGS(&m_commandQueue))))
        return false;

    for (UINT i = 0; i < FrameCount; ++i)
    {
        if (FAILED(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_commandAllocators[i]))))
            return false;
    }

    if (FAILED(m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocators[0].Get(),
        nullptr,
        IID_PPV_ARGS(&m_commandList))))
        return false;

    m_commandList->Close();
    return true;
}

// буфер кадро для показа на экране
bool D3D12Context::CreateSwapChain(HWND hwnd)
{
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.BufferCount = FrameCount;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swap;
    if (FAILED(factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(),
        hwnd,
        &desc,
        nullptr,
        nullptr,
        &swap)))
        return false;

    swap.As(&m_swapChain);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    return true;
}

// чтобы буфер отрисовывался
bool D3D12Context::CreateRTV()
{
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.NumDescriptors = FrameCount;
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

    if (FAILED(m_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_rtvHeap))))
        return false;

    m_rtvDescriptorSize =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < FrameCount; ++i)
    {
        if (FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i]))))
            return false;

        m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, handle);
        handle.ptr += SIZE_T(m_rtvDescriptorSize);
    }
    return true;
}

// буфер глубины
bool D3D12Context::CreateDepthStencil()
{
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.NumDescriptors = 1;
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;

    if (FAILED(m_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_dsvHeap))))
        return false;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = desc.Format;
    clear.DepthStencil.Depth = 1.0f;
    clear.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clear,
        IID_PPV_ARGS(&m_depthBuffer))))
        return false;

    m_device->CreateDepthStencilView(
        m_depthBuffer.Get(),
        nullptr,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    return true;
}

// ожидание GPU
bool D3D12Context::CreateFence()
{
    if (FAILED(m_device->CreateFence(
        0,
        D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&m_fence))))
        return false;

    m_fenceValue = 0;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    return m_fenceEvent != nullptr;
}

// компиляция hlsl
bool D3D12Context::CompileShaders()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);

    wchar_t* lastSlash = wcsrchr(exeDir, L'\\');
    if (!lastSlash)
        return false;
    *(lastSlash + 1) = 0;

    wchar_t shaderPath[MAX_PATH];
    wcscpy_s(shaderPath, exeDir);
    wcscat_s(shaderPath, L"..\\..\\CG_Sem2\\Shaders.hlsl");

    if (GetFileAttributesW(shaderPath) == INVALID_FILE_ATTRIBUTES)
    {
        wcscpy_s(shaderPath, exeDir);
        wcscat_s(shaderPath, L"Shaders.hlsl");
    }

    HRESULT hr = D3DCompileFromFile(
        shaderPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain",
        "vs_5_0",
        flags,
        0,
        &m_vs,
        nullptr);

    if (FAILED(hr))
        return false;

    hr = D3DCompileFromFile(
        shaderPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "HSMain",
        "hs_5_0",
        flags,
        0,
        &m_hs,
        nullptr);

    if (FAILED(hr))
        return false;

    hr = D3DCompileFromFile(
        shaderPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "DSMain",
        "ds_5_0",
        flags,
        0,
        &m_ds,
        nullptr);

    if (FAILED(hr))
        return false;

    hr = D3DCompileFromFile(
        shaderPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain",
        "ps_5_0",
        flags,
        0,
        &m_ps,
        nullptr);

    return SUCCEEDED(hr);
}

// место хранение ссылок на текстуры
bool D3D12Context::CreateSRVHeap(UINT numDescriptors)
{
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = numDescriptors;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr) || m_srvHeap == nullptr)
    {
        OutputDebugStringA("CreateDescriptorHeap for SRV FAILED\n");
        return false;
    }

    m_srvDescriptorSize =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    return true;
}

// массив для текстур 
bool D3D12Context::CreateRootSignature()
{
    const UINT MaxSrvCount = 128;

    // диапазон
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // передача 2 параметров в шейдер
    D3D12_DESCRIPTOR_RANGE displacementRange{};
    displacementRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    displacementRange.NumDescriptors = 1;
    displacementRange.BaseShaderRegister = 1;
    displacementRange.RegisterSpace = 0;
    displacementRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE normalRange{};
    normalRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    normalRange.NumDescriptors = 1;
    normalRange.BaseShaderRegister = 2;
    normalRange.RegisterSpace = 0;
    normalRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE roughnessRange{};
    roughnessRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    roughnessRange.NumDescriptors = 1;
    roughnessRange.BaseShaderRegister = 3;
    roughnessRange.RegisterSpace = 0;
    roughnessRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[5]{};

    // константный буфер
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0; // номер слота
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // таблица для пикселей шейдера
    rootParams[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[1].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_PIXEL;

    rootParams[2].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &displacementRange;
    rootParams[2].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_DOMAIN;

    rootParams[3].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[3].DescriptorTable.pDescriptorRanges = &normalRange;
    rootParams[3].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_PIXEL;

    rootParams[4].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[4].DescriptorTable.pDescriptorRanges = &roughnessRange;
    rootParams[4].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_PIXEL;

    // правила чтения текстуры
    D3D12_STATIC_SAMPLER_DESC staticSampler{};
    staticSampler.Filter = D3D12_FILTER_ANISOTROPIC;
    staticSampler.MaxAnisotropy = 8;
    staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; // сглаживание
    staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;  // повторение текстуры
    staticSampler.Filter = D3D12_FILTER_ANISOTROPIC;
    staticSampler.MaxAnisotropy = 8;
    staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    staticSampler.ShaderRegister = 0;
    staticSampler.ShaderVisibility =
        D3D12_SHADER_VISIBILITY_ALL;


    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 5;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &staticSampler;
    rsDesc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;

    HRESULT hr = D3D12SerializeRootSignature(
        &rsDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &error);

    if (FAILED(hr))
        return false;

    return SUCCEEDED(m_device->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)));
}

// правила отрисовки модели и текстур
bool D3D12Context::CreatePipelineState()
{

    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = { layout, _countof(layout) };
    pso.pRootSignature = m_rootSignature.Get();
    pso.VS = { m_vs->GetBufferPointer(), m_vs->GetBufferSize() };
    pso.HS = { m_hs->GetBufferPointer(), m_hs->GetBufferSize() };
    pso.DS = { m_ds->GetBufferPointer(), m_ds->GetBufferSize() };
    pso.PS = { m_ps->GetBufferPointer(), m_ps->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthClipEnable = TRUE;

    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    for (int i = 0; i < 8; ++i)
    {
        pso.BlendState.RenderTarget[i].BlendEnable = FALSE;
        pso.BlendState.RenderTarget[i].LogicOpEnable = FALSE;
        pso.BlendState.RenderTarget[i].SrcBlend = D3D12_BLEND_ONE;
        pso.BlendState.RenderTarget[i].DestBlend = D3D12_BLEND_ZERO;
        pso.BlendState.RenderTarget[i].BlendOp = D3D12_BLEND_OP_ADD;
        pso.BlendState.RenderTarget[i].SrcBlendAlpha = D3D12_BLEND_ONE;
        pso.BlendState.RenderTarget[i].DestBlendAlpha = D3D12_BLEND_ZERO;
        pso.BlendState.RenderTarget[i].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pso.BlendState.RenderTarget[i].LogicOp = D3D12_LOGIC_OP_NOOP;
        pso.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilEnable = FALSE;

    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pso.SampleDesc.Count = 1;

    return SUCCEEDED(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
}

// геометрия
bool D3D12Context::CreateGeometry()
{
    struct V { XMFLOAT3 p; XMFLOAT3 n; XMFLOAT2 uv; };

    std::vector<V> vertices =
    {
        {{-1,-1, 1},{0,0,1}}, {{-1, 1, 1},{0,0,1}}, {{ 1, 1, 1},{0,0,1}}, {{ 1,-1, 1},{0,0,1}},
        {{ 1,-1,-1},{0,0,-1}}, {{ 1, 1,-1},{0,0,-1}}, {{-1, 1,-1},{0,0,-1}}, {{-1,-1,-1},{0,0,-1}},
        {{-1, 1, 1},{0,1,0}}, {{-1, 1,-1},{0,1,0}}, {{ 1, 1,-1},{0,1,0}}, {{ 1, 1, 1},{0,1,0}},
        {{-1,-1,-1},{0,-1,0}}, {{-1,-1, 1},{0,-1,0}}, {{ 1,-1, 1},{0,-1,0}}, {{ 1,-1,-1},{0,-1,0}},
        {{ 1,-1, 1},{1,0,0}}, {{ 1, 1, 1},{1,0,0}}, {{ 1, 1,-1},{1,0,0}}, {{ 1,-1,-1},{1,0,0}},
        {{-1,-1,-1},{-1,0,0}}, {{-1, 1,-1},{-1,0,0}}, {{-1, 1, 1},{-1,0,0}}, {{-1,-1, 1},{-1,0,0}},
    };

    std::vector<uint16_t> indices =
    {
        0,1,2, 0,2,3,
        4,5,6, 4,6,7,
        8,9,10, 8,10,11,
        12,13,14, 12,14,15,
        16,17,18, 16,18,19,
        20,21,22, 20,22,23
    };

    m_indexCount = (UINT)indices.size();

    UINT vbSize = (UINT)(vertices.size() * sizeof(V));
    UINT ibSize = (UINT)(indices.size() * sizeof(uint16_t));

    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC vbDesc{};
    vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vbDesc.Width = vbSize;
    vbDesc.Height = 1;
    vbDesc.DepthOrArraySize = 1;
    vbDesc.MipLevels = 1;
    vbDesc.SampleDesc.Count = 1;
    vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(m_device->CreateCommittedResource(
        &upload, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_vertexBuffer))))
        return false;

    void* p = nullptr;
    m_vertexBuffer->Map(0, nullptr, &p);
    std::memcpy(p, vertices.data(), vbSize);
    m_vertexBuffer->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC ibDesc = vbDesc;
    ibDesc.Width = ibSize;

    if (FAILED(m_device->CreateCommittedResource(
        &upload, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_indexBuffer))))
        return false;

    m_indexBuffer->Map(0, nullptr, &p);
    std::memcpy(p, indices.data(), ibSize);
    m_indexBuffer->Unmap(0, nullptr);

    m_vbView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vbView.StrideInBytes = sizeof(V);
    m_vbView.SizeInBytes = vbSize;

    m_ibView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_ibView.SizeInBytes = ibSize;
    m_ibView.Format = DXGI_FORMAT_R16_UINT;

    m_modelBoundsLocal.Min = XMFLOAT3(-1.0f, -1.0f, -1.0f);
    m_modelBoundsLocal.Max = XMFLOAT3(1.0f, 1.0f, 1.0f);
    m_sceneBoundsMin = m_modelBoundsLocal.Min;
    m_sceneBoundsMax = m_modelBoundsLocal.Max;

    return true;
}

// загрузка модели и текстур
bool D3D12Context::LoadModelFromOBJ(const char* objPath, const char* mtlBaseDir)
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    bool ret = tinyobj::LoadObj(
        &attrib, &shapes, &materials,
        &warn, &err,
        objPath, mtlBaseDir,
        true);

    if (!warn.empty()) {
        OutputDebugStringA(("TinyObjLoader Warning: " + warn + "\n").c_str());
    }
    if (!err.empty()) {
        OutputDebugStringA(("TinyObjLoader Error: " + err + "\n").c_str());
    }
    if (!ret) {
        OutputDebugStringA(("Failed to load OBJ file: " + std::string(objPath) + "\n").c_str());
        return false;
    }


    m_materialDiffusePaths.clear();
    m_materialDiffusePaths.resize(materials.size());
    for (size_t i = 0; i < materials.size(); ++i)
    {
        // может быть пустая строка
        m_materialDiffusePaths[i] = materials[i].diffuse_texname;
    }

    m_materialToSrv.clear();
    m_materialToSrv.resize(materials.size(), 0);

    UINT nextSrvIndex = 1;
    std::unordered_map<std::string, UINT> texturePathToSrv;

    for (size_t i = 0; i < m_materialDiffusePaths.size(); ++i)
    {
        const std::string& textureName = m_materialDiffusePaths[i];
        if (textureName.empty())
        {
            continue;
        }

        namespace fs = std::filesystem;
        fs::path texturePath(textureName);
        std::error_code ec;

        if (!texturePath.is_absolute())
        {
            texturePath = fs::path(mtlBaseDir) / texturePath;
        }
        texturePath = texturePath.lexically_normal();

        if (!fs::exists(texturePath, ec))
        {
            const fs::path fileName = fs::path(textureName).filename();
            const fs::path modelsDir = fs::path(mtlBaseDir);
            const fs::path textureDirCandidate = modelsDir / "textures" / fileName;
            const fs::path localCandidate = modelsDir / fileName;

            if (fs::exists(textureDirCandidate, ec))
            {
                texturePath = textureDirCandidate;
            }
            else if (fs::exists(localCandidate, ec))
            {
                texturePath = localCandidate;
            }
        }

        const std::string texturePathString = texturePath.string();

        auto existing = texturePathToSrv.find(texturePathString);
        if (existing != texturePathToSrv.end())
        {
            m_materialToSrv[i] = existing->second;
            continue;
        }

        if (nextSrvIndex >= 128)
        {
            OutputDebugStringA("Reached SRV heap limit while loading OBJ textures. Using fallback texture.\n");
            break;
        }

        if (CreateTextureFromFile(texturePathString.c_str(), nextSrvIndex))
        {
            texturePathToSrv.emplace(texturePathString, nextSrvIndex);
            m_materialToSrv[i] = nextSrvIndex;
            ++nextSrvIndex;
        }
        else
        {
            OutputDebugStringA(("Failed to load material texture, using fallback: " + texturePathString + "\n").c_str());
        }
    }

    struct V { XMFLOAT3 p; XMFLOAT3 n; XMFLOAT2 uv; };

    std::vector<V> vertices;

    std::vector<std::vector<uint32_t>> indicesByBucket(materials.size() + 1);


    size_t approxIndexCount = 0;
    for (auto& sh : shapes) approxIndexCount += sh.mesh.indices.size();
    vertices.reserve(approxIndexCount);
    for (auto& b : indicesByBucket) b.reserve(approxIndexCount / (indicesByBucket.size() ? indicesByBucket.size() : 1));

    // раскладываем индексы по материалам
    for (size_t s = 0; s < shapes.size(); s++)
    {
        size_t index_offset = 0;

        const auto& numFaceVerts = shapes[s].mesh.num_face_vertices;
        const auto& shapeIndices = shapes[s].mesh.indices;
        const auto& matIds = shapes[s].mesh.material_ids;

        for (size_t f = 0; f < numFaceVerts.size(); f++)
        {
            int fv = numFaceVerts[f];

            int matId = -1;
            if (f < matIds.size())
                matId = matIds[f];

            size_t bucket = (matId >= 0) ? (size_t)(1 + matId) : 0;

            for (size_t v = 0; v < (size_t)fv; v++)
            {
                tinyobj::index_t idx = shapeIndices[index_offset + v];

                // позиция
                float vx = 0.0f, vy = 0.0f, vz = 0.0f;
                if (idx.vertex_index >= 0 && (size_t)(3 * idx.vertex_index + 2) < attrib.vertices.size())
                {
                    vx = attrib.vertices[3 * idx.vertex_index + 0];
                    vy = attrib.vertices[3 * idx.vertex_index + 1];
                    vz = attrib.vertices[3 * idx.vertex_index + 2];
                }

                // нормаль
                float nx = 0.0f, ny = 0.0f, nz = 0.0f;
                if (idx.normal_index >= 0 && (size_t)(3 * idx.normal_index + 2) < attrib.normals.size())
                {
                    nx = attrib.normals[3 * idx.normal_index + 0];
                    ny = attrib.normals[3 * idx.normal_index + 1];
                    nz = attrib.normals[3 * idx.normal_index + 2];
                }

                // текстура
                float tu = 0.0f, tv = 0.0f;
                if (idx.texcoord_index >= 0 && (size_t)(2 * idx.texcoord_index + 1) < attrib.texcoords.size())
                {
                    tu = attrib.texcoords[2 * idx.texcoord_index + 0];
                    tv = attrib.texcoords[2 * idx.texcoord_index + 1];
                    tv = 1.0f - tv;
                }

                V vertex;
                vertex.p = XMFLOAT3(vx, vy, vz);
                vertex.n = XMFLOAT3(nx, ny, nz);
                vertex.uv = XMFLOAT2(tu, tv);

                vertices.push_back(vertex);

                uint32_t newIndex = (uint32_t)(vertices.size() - 1);
                indicesByBucket[bucket].push_back(newIndex);
            }

            index_offset += (size_t)fv;
        }
    }

    if (vertices.empty()) {
        OutputDebugStringA("Model has no vertices!\n");
        return false;
    }

    float minX = vertices[0].p.x;
    float minY = vertices[0].p.y;
    float minZ = vertices[0].p.z;
    float maxX = vertices[0].p.x;
    float maxY = vertices[0].p.y;
    float maxZ = vertices[0].p.z;

    for (const auto& v : vertices)
    {
        minX = (v.p.x < minX) ? v.p.x : minX;
        minY = (v.p.y < minY) ? v.p.y : minY;
        minZ = (v.p.z < minZ) ? v.p.z : minZ;
        maxX = (v.p.x > maxX) ? v.p.x : maxX;
        maxY = (v.p.y > maxY) ? v.p.y : maxY;
        maxZ = (v.p.z > maxZ) ? v.p.z : maxZ;
    }

    m_modelBoundsLocal.Min = XMFLOAT3(minX, minY, minZ);
    m_modelBoundsLocal.Max = XMFLOAT3(maxX, maxY, maxZ);
    m_sceneBoundsMin = m_modelBoundsLocal.Min;
    m_sceneBoundsMax = m_modelBoundsLocal.Max;

    // сборка индекс буфера
    m_submeshes.clear();

    std::vector<uint32_t> indices;
    indices.reserve(approxIndexCount);

    UINT runningStart = 0;

    for (size_t bucket = 0; bucket < indicesByBucket.size(); ++bucket)
    {
        auto& src = indicesByBucket[bucket];
        if (src.empty()) continue;

        Submesh sm{};
        sm.IndexStart = runningStart;
        sm.IndexCount = (UINT)src.size();
        sm.MaterialId = (bucket == 0) ? -1 : ((int)bucket - 1);
        sm.SrvIndex = 0;
        if (sm.MaterialId >= 0 && (size_t)sm.MaterialId < m_materialToSrv.size())
        {
            sm.SrvIndex = m_materialToSrv[sm.MaterialId];
        }

        indices.insert(indices.end(), src.begin(), src.end());
        runningStart += sm.IndexCount;

        m_submeshes.push_back(sm);
    }

    if (indices.empty()) {
        OutputDebugStringA("Model has no indices!\n");
        return false;
    }

    m_use32BitIndices = (vertices.size() > 65535);
    m_indexCount = (UINT)indices.size();

    UINT vbSize = (UINT)(vertices.size() * sizeof(V));
    UINT ibSize = m_use32BitIndices ?
        (UINT)(indices.size() * sizeof(uint32_t)) :
        (UINT)(indices.size() * sizeof(uint16_t));

    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC vbDesc{};
    vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vbDesc.Alignment = 0;
    vbDesc.Width = vbSize;
    vbDesc.Height = 1;
    vbDesc.DepthOrArraySize = 1;
    vbDesc.MipLevels = 1;
    vbDesc.Format = DXGI_FORMAT_UNKNOWN;
    vbDesc.SampleDesc.Count = 1;
    vbDesc.SampleDesc.Quality = 0;
    vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    vbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(m_device->CreateCommittedResource(
        &upload, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_vertexBuffer))))
    {
        OutputDebugStringA("Create vertex buffer FAILED\n");
        return false;
    }

    void* p = nullptr;
    if (FAILED(m_vertexBuffer->Map(0, nullptr, &p)) || !p) {
        OutputDebugStringA("VB Map FAILED\n");
        return false;
    }
    std::memcpy(p, vertices.data(), vbSize);
    m_vertexBuffer->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC ibDesc = vbDesc;
    ibDesc.Width = ibSize;

    if (FAILED(m_device->CreateCommittedResource(
        &upload, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_indexBuffer))))
    {
        OutputDebugStringA("Create index buffer FAILED\n");
        return false;
    }

    if (FAILED(m_indexBuffer->Map(0, nullptr, &p)) || !p) {
        OutputDebugStringA("IB Map FAILED\n");
        return false;
    }

    if (m_use32BitIndices)
    {
        std::memcpy(p, indices.data(), ibSize);
    }
    else
    {
        uint16_t* indices16 = (uint16_t*)p;
        for (size_t i = 0; i < indices.size(); i++)
            indices16[i] = (uint16_t)indices[i];
    }

    m_indexBuffer->Unmap(0, nullptr);


    m_vbView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vbView.StrideInBytes = sizeof(V);
    m_vbView.SizeInBytes = vbSize;

    m_ibView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_ibView.SizeInBytes = ibSize;
    m_ibView.Format = m_use32BitIndices ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;


    OutputDebugStringA(("Successfully loaded model: " + std::string(objPath) +
        " (Vertices: " + std::to_string(vertices.size()) +
        ", Indices: " + std::to_string(m_indexCount) +
        ", Submeshes: " + std::to_string(m_submeshes.size()) +
        ", Materials: " + std::to_string(materials.size()) + ")\n").c_str());

    for (size_t i = 0; i < m_materialDiffusePaths.size() && i < 10; ++i)
    {
        OutputDebugStringA(("MTL diffuse[" + std::to_string(i) + "]: " + m_materialDiffusePaths[i] + "\n").c_str());
    }

    return true;
}

// константный буфер
bool D3D12Context::CreateConstantBuffer()
{
    m_constantBufferStride = (sizeof(PerObjectCB) + 255) & ~255u;
    const UINT64 cbSize = UINT64(m_constantBufferStride) * UINT64(MaxSceneObjects);

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

    if (FAILED(m_device->CreateCommittedResource(
        &upload, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_constantBuffer))))
        return false;

    XMMATRIX I = XMMatrixIdentity();
    m_cbData.UVTransform = XMFLOAT4(1.0f, 1.0f, 0.0f, 0.0f);
    m_cbData.TimeParams = XMFLOAT4(0.0f, 1.0f, 0.0f, 0.0f);
    m_cbData.TessellationParams = XMFLOAT4(0.0f, 1.0f, 1.0f, 1.0f);
    XMStoreFloat4x4(&m_cbData.World, XMMatrixTranspose(I));

    D3D12_RANGE readRange{ 0,0 };
    if (FAILED(m_constantBuffer->Map(0, &readRange, (void**)&m_cbMappedData)))
        return false;

    std::memset(m_cbMappedData, 0, static_cast<size_t>(cbSize));
    std::memcpy(m_cbMappedData, &m_cbData, sizeof(PerObjectCB));
    return true;
}


// тайлинг и офсет 
void D3D12Context::BeginFrame(ID3D12PipelineState* initialState)
{
    WaitForFrame(m_frameIndex);
    m_commandAllocators[m_frameIndex]->Reset();
    m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), initialState);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_backBuffers[m_frameIndex].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);
}

void D3D12Context::EndFrame()
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_backBuffers[m_frameIndex].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &barrier);

    m_commandList->Close();
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
    SignalCurrentFrame();
    m_swapChain->Present(1, 0);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void D3D12Context::UpdateSceneConstants()
{
    static const XMFLOAT4X4 identity = MakeIdentityMatrix();
    UpdateCB(identity, 0);
}

void D3D12Context::DrawSceneGeometry(
    ID3D12GraphicsCommandList* commandList,
    UINT textureRootParameterIndex,
    UINT displacementRootParameterIndex)
{
    if (commandList == nullptr)
    {
        return;
    }

    UpdateObjectVisibility();

    D3D12_GPU_DESCRIPTOR_HANDLE baseGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    commandList->IASetVertexBuffers(0, 1, &m_vbView);
    commandList->IASetIndexBuffer(&m_ibView);

    if (displacementRootParameterIndex != UINT_MAX)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE displacementHandle = baseGpu;
        displacementHandle.ptr += SIZE_T(m_displacementSrvIndex) * SIZE_T(m_srvDescriptorSize);
        commandList->SetGraphicsRootDescriptorTable(displacementRootParameterIndex, displacementHandle);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE normalMapHandle = baseGpu;
    normalMapHandle.ptr += SIZE_T(m_normalMapSrvIndex) * SIZE_T(m_srvDescriptorSize);
    commandList->SetGraphicsRootDescriptorTable(3, normalMapHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE roughnessHandle = baseGpu;
    roughnessHandle.ptr += SIZE_T(m_roughnessSrvIndex) * SIZE_T(m_srvDescriptorSize);
    commandList->SetGraphicsRootDescriptorTable(4, roughnessHandle);

    const auto drawSubmeshes = [&](UINT textureSrvIndex)
    {
        if (!m_submeshes.empty())
        {
            for (const auto& sm : m_submeshes)
            {
                const UINT resolvedSrvIndex = (textureSrvIndex == UINT_MAX) ? sm.SrvIndex : textureSrvIndex;
                D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = baseGpu;
                textureHandle.ptr += SIZE_T(resolvedSrvIndex) * SIZE_T(m_srvDescriptorSize);
                commandList->SetGraphicsRootDescriptorTable(textureRootParameterIndex, textureHandle);
                commandList->DrawIndexedInstanced(sm.IndexCount, 1, sm.IndexStart, 0, 0);
            }
        }
        else
        {
            D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = baseGpu;
            textureHandle.ptr += SIZE_T((textureSrvIndex == UINT_MAX) ? 0u : textureSrvIndex) * SIZE_T(m_srvDescriptorSize);
            commandList->SetGraphicsRootDescriptorTable(textureRootParameterIndex, textureHandle);
            commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
        }
    };

    if (m_sceneObjects.empty())
    {
        static const XMFLOAT4X4 identity = MakeIdentityMatrix();
        UpdateCB(identity, 0);
        commandList->SetGraphicsRootConstantBufferView(0, GetSceneConstantBufferAddress(0));
        drawSubmeshes(UINT_MAX);
        return;
    }

    for (UINT objectIndex = 0; objectIndex < static_cast<UINT>(m_sceneObjects.size()); ++objectIndex)
    {
        const SceneObject& object = m_sceneObjects[objectIndex];
        if (!object.Visible)
        {
            continue;
        }

        UpdateCB(object.World, objectIndex);
        commandList->SetGraphicsRootConstantBufferView(0, GetSceneConstantBufferAddress(objectIndex));
        drawSubmeshes(object.DiffuseSrvIndex);
    }
}

ID3D12Device* D3D12Context::GetDevice() const
{
    return m_device.Get();
}

ID3D12GraphicsCommandList* D3D12Context::GetCommandList() const
{
    return m_commandList.Get();
}

ID3D12DescriptorHeap* D3D12Context::GetSceneSRVHeap() const
{
    return m_srvHeap.Get();
}

ID3D12RootSignature* D3D12Context::GetSceneRootSignature() const
{
    return m_rootSignature.Get();
}

D3D12_GPU_VIRTUAL_ADDRESS D3D12Context::GetSceneConstantBufferAddress(UINT objectIndex) const
{
    const UINT safeIndex = (std::min)(objectIndex, MaxSceneObjects - 1);
    return m_constantBuffer->GetGPUVirtualAddress() + UINT64(safeIndex) * UINT64(m_constantBufferStride);
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Context::GetCurrentBackBufferRTV() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(m_frameIndex) * SIZE_T(m_rtvDescriptorSize);
    return rtv;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Context::GetDepthStencilView() const
{
    return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
}

D3D12_VIEWPORT D3D12Context::GetViewport() const
{
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(m_width);
    vp.Height = static_cast<float>(m_height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    return vp;
}

D3D12_RECT D3D12Context::GetScissorRect() const
{
    D3D12_RECT sc{};
    sc.left = 0;
    sc.top = 0;
    sc.right = static_cast<LONG>(m_width);
    sc.bottom = static_cast<LONG>(m_height);
    return sc;
}

UINT D3D12Context::GetWidth() const
{
    return m_width;
}

UINT D3D12Context::GetHeight() const
{
    return m_height;
}

DirectX::XMFLOAT3 D3D12Context::GetCameraPosition() const
{
    return m_cameraPos;
}

DirectX::XMFLOAT3 D3D12Context::GetCameraTarget() const
{
    return m_cameraTarget;
}

DirectX::XMFLOAT3 D3D12Context::GetSceneBoundsMin() const
{
    return m_sceneBoundsMin;
}

DirectX::XMFLOAT3 D3D12Context::GetSceneBoundsMax() const
{
    return m_sceneBoundsMax;
}

DirectX::XMFLOAT3 D3D12Context::GetSceneCenter() const
{
    return XMFLOAT3(
        0.5f * (m_sceneBoundsMin.x + m_sceneBoundsMax.x),
        0.5f * (m_sceneBoundsMin.y + m_sceneBoundsMax.y),
        0.5f * (m_sceneBoundsMin.z + m_sceneBoundsMax.z));
}

DirectX::XMFLOAT3 D3D12Context::GetSceneExtents() const
{
    return XMFLOAT3(
        0.5f * (m_sceneBoundsMax.x - m_sceneBoundsMin.x),
        0.5f * (m_sceneBoundsMax.y - m_sceneBoundsMin.y),
        0.5f * (m_sceneBoundsMax.z - m_sceneBoundsMin.z));
}

D3D12Context::Scene D3D12Context::GetCurrentScene() const
{
    return m_currentScene;
}

float D3D12Context::GetTime() const
{
    return m_time;
}

void D3D12Context::SetTime(float t)
{
    m_time = t;
}

void D3D12Context::SetUVTiling(float x, float y)
{
    m_uvTiling = { x, y };
}

void D3D12Context::SetUVScrollSpeed(float uSpeed, float vSpeed)
{
    m_uvScrollSpeed = { uSpeed, vSpeed };
}


// завершение работы рендера
void D3D12Context::Shutdown()
{
    WaitForGPU();

    if (m_cbMappedData)
    {
        m_constantBuffer->Unmap(0, nullptr);
        m_cbMappedData = nullptr;
    }

    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    m_sceneObjects.clear();
    m_octreeNodes.clear();
    m_octreeRoot = -1;
    m_submeshes.clear();
    m_materialDiffusePaths.clear();
    m_materialToSrv.clear();

    m_textures.clear();
    m_textureUploads.clear();

    m_vertexBuffer.Reset();
    m_indexBuffer.Reset();
    m_constantBuffer.Reset();
    m_depthBuffer.Reset();
    for (UINT i = 0; i < FrameCount; ++i)
    {
        m_backBuffers[i].Reset();
        m_commandAllocators[i].Reset();
    }

    m_srvHeap.Reset();
    m_rtvHeap.Reset();
    m_dsvHeap.Reset();
    m_rootSignature.Reset();
    m_pso.Reset();
    m_vs.Reset();
    m_hs.Reset();
    m_ds.Reset();
    m_ps.Reset();
    m_commandList.Reset();
    m_swapChain.Reset();
    m_commandQueue.Reset();
    m_fence.Reset();
    m_device.Reset();
}

// заверщение работы
void D3D12Context::SetRotation(float t)
{
    m_rotationT = t;
}
