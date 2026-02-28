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

#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")

using namespace DirectX;

// инициализация !!!
bool D3D12Context::Initialize(HWND hwnd, UINT width, UINT height)
{
    // размер окна
    m_width = width;
    m_height = height;

    // создание устройства 
    if (!CreateDevice()) {
        MessageBoxW(nullptr, L"CreateDevice FAILED", L"DX12", MB_OK);
        return false;
    }

    // список команд
    if (!CreateCommandObjects()) {
        MessageBoxW(nullptr, L"CreateCommandObjects FAILED", L"DX12", MB_OK);
        return false;
    }

    // буферы кадров
    if (!CreateSwapChain(hwnd)) {
        MessageBoxW(nullptr, L"CreateSwapChain FAILED", L"DX12", MB_OK);
        return false;
    }

    // для рисования буферов
    if (!CreateRTV()) {
        MessageBoxW(nullptr, L"CreateRTV FAILED", L"DX12", MB_OK);
        return false;
    }

    // буфер глубины 
    if (!CreateDepthStencil()) {
        MessageBoxW(nullptr, L"CreateDepthStencil FAILED", L"DX12", MB_OK);
        return false;
    }

    if (!CreateFence()) {
        MessageBoxW(nullptr, L"CreateFence FAILED", L"DX12", MB_OK);
        return false;
    }


    // шейдеры
    if (!CompileShaders()) {
        MessageBoxW(nullptr, L"CompileShaders FAILED", L"DX12", MB_OK);
        return false;
    }

    if (!CreateRootSignature()) {
        MessageBoxW(nullptr, L"CreateRootSignature FAILED", L"DX12", MB_OK);
        return false;
    }

    if (!CreatePipelineState()) {
        MessageBoxW(nullptr, L"CreatePipelineState FAILED", L"DX12", MB_OK);
        return false;
    }

    // текстуры
    const UINT MaxSrvCount = 8;
    if (!CreateSRVHeap(MaxSrvCount)) {
        MessageBoxW(nullptr, L"CreateSRVHeap FAILED", L"DX12", MB_OK);
        return false;
    }

    // определение папки .exe
    char exeDirA[MAX_PATH];
    GetModuleFileNameA(nullptr, exeDirA, MAX_PATH);
    char* lastSlash = strrchr(exeDirA, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    std::string exeDir = exeDirA;

    // модель и текстура
    std::string modelsDir = exeDir + "models\\";
    std::string objPath = modelsDir + "Y02KIUZDQIQUI871CWZ8ZL02C.obj";
    std::string mtlDir = modelsDir;
    std::string defaultTex = modelsDir + "texture.jpg";

    OutputDebugStringA(("OBJ PATH: " + objPath + "\n").c_str());
    OutputDebugStringA(("MTL DIR : " + mtlDir + "\n").c_str());
    OutputDebugStringA(("DEF TEX : " + defaultTex + "\n").c_str());

    // загрузка модели
    bool modelLoaded = LoadModelFromOBJ(objPath.c_str(), mtlDir.c_str());

    if (!modelLoaded)
    {
        OutputDebugStringA("OBJ load failed. Using fallback cube.\n");

        if (!CreateGeometry()) {
            MessageBoxW(nullptr, L"CreateGeometry FAILED", L"DX12", MB_OK);
            return false;
        }

        m_submeshes.clear();
        Submesh sm{};
        sm.IndexStart = 0;
        sm.IndexCount = m_indexCount;
        sm.MaterialId = -1;
        sm.SrvIndex = 0;
        m_submeshes.push_back(sm);

        // куб без материалов
        m_materialDiffusePaths.clear();
        m_materialToSrv.clear();
    }
    else
    {
        // модель без текстуры
        if (m_submeshes.empty())
        {
            Submesh sm{};
            sm.IndexStart = 0;
            sm.IndexCount = m_indexCount;
            sm.MaterialId = -1;
            sm.SrvIndex = 0;
            m_submeshes.push_back(sm);
        }
    }


    if (FAILED(m_commandAllocator->Reset())) {
        MessageBoxW(nullptr, L"CommandAllocator Reset FAILED", L"DX12", MB_OK);
        return false;
    }
    if (FAILED(m_commandList->Reset(m_commandAllocator.Get(), nullptr))) {
        MessageBoxW(nullptr, L"CommandList Reset FAILED", L"DX12", MB_OK);
        return false;
    }

    UINT nextSrv = 0; // счетчик текстур

    // загрузка текстуры
    if (!CreateTextureFromFile(defaultTex.c_str(), nextSrv)) {
        MessageBoxW(nullptr, L"Default texture load FAILED", L"DX12", MB_OK);
        return false;
    }
    nextSrv++;

    // заполнение массива текстур
    if (modelLoaded && !m_materialDiffusePaths.empty())
    {
        m_materialToSrv.assign(m_materialDiffusePaths.size(), 0);

        for (size_t i = 0; i < m_materialDiffusePaths.size(); ++i)
        {
            const std::string& rel = m_materialDiffusePaths[i];
            if (rel.empty())
            {
                m_materialToSrv[i] = 0;
                continue;
            }

            if (nextSrv >= MaxSrvCount)
            {
                // места в heap больше нет — fallback на default
                m_materialToSrv[i] = 0;
                continue;
            }

            // ВАЖНО: tinyobj может давать "textures/xxx.jpg"
            // fullPath станет models\textures\xxx.jpg
            std::string fullPath = modelsDir + rel;

            OutputDebugStringA(("TRY LOAD: " + fullPath + "\n").c_str());

            if (CreateTextureFromFile(fullPath.c_str(), nextSrv))
            {
                m_materialToSrv[i] = nextSrv;
                nextSrv++;
            }
            else
            {
                m_materialToSrv[i] = 0; // default
            }
        }

        // назначаем srv сабмешам
        for (auto& sm : m_submeshes)
        {
            if (sm.MaterialId >= 0 && sm.MaterialId < (int)m_materialToSrv.size())
                sm.SrvIndex = m_materialToSrv[(size_t)sm.MaterialId];
            else
                sm.SrvIndex = 0;
        }
    }
    else
    {
        for (auto& sm : m_submeshes)
            sm.SrvIndex = 0;
    }

    if (FAILED(m_commandList->Close())) {
        MessageBoxW(nullptr, L"CommandList Close FAILED", L"DX12", MB_OK);
        return false;
    }

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForGPU();

    if (!CreateConstantBuffer()) {
        MessageBoxW(nullptr, L"CreateConstantBuffer FAILED", L"DX12", MB_OK);
        return false;
    }

    return true;
}

// движение камеры
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

// рендер !!!
void D3D12Context::Render(float r, float g, float b, float a)
{
    //static int frame = 0;
    //if ((frame++ % 60) == 0) printf("Render frame: %d\n", frame);

    // очитстка листа команд
    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), m_pso.Get());

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = m_backBuffers[m_frameIndex].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(m_frameIndex) * SIZE_T(m_rtvDescriptorSize);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    // очитска экрана
    float clearColor[4] = { r, g, b, a };
    m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    // куда рисовать
    m_commandList->OMSetRenderTargets(1, &rtv, TRUE, &dsv);

    // карта до размеров экрана
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = 0.f;
    vp.TopLeftY = 0.f;
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MinDepth = 0.f;
    vp.MaxDepth = 1.f;

    // не рисовать за пределами прямоугольника
    D3D12_RECT sc{};
    sc.left = 0;
    sc.top = 0;
    sc.right = (LONG)m_width;
    sc.bottom = (LONG)m_height;

    m_commandList->RSSetViewports(1, &vp);
    m_commandList->RSSetScissorRects(1, &sc);
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    // подключение таблицы текстур
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, heaps);

    // обновление и запись в 0 слот в буффере
    UpdateCB();
    m_commandList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());

    // геометрия 
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); // треугольники
    m_commandList->IASetVertexBuffers(0, 1, &m_vbView); // вершины
    m_commandList->IASetIndexBuffer(&m_ibView); // индексы

    // указатель на первый дескриптор
    D3D12_GPU_DESCRIPTOR_HANDLE baseGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();

    // рисование по сабмешам
    if (!m_submeshes.empty())
    {
        for (const auto& sm : m_submeshes)
        {
            // root param 1 = SRV table. Мы сдвигаем базу на нужный srvIndex,
            // и тогда в шейдере t0 будет указывать на "текущую" текстуру.
            D3D12_GPU_DESCRIPTOR_HANDLE texHandle = baseGpu;
            texHandle.ptr += SIZE_T(sm.SrvIndex) * SIZE_T(m_srvDescriptorSize);

            m_commandList->SetGraphicsRootDescriptorTable(1, texHandle);

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
        // одна текстура
        m_commandList->SetGraphicsRootDescriptorTable(1, baseGpu);
        m_commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
    }

    // переход из буфера в режим показа
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    m_commandList->ResourceBarrier(1, &barrier);

    // закрытие списка команд и переход к видеокарте
    m_commandList->Close();
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // показ кадра на экране
    m_swapChain->Present(1, 0);
    printf("Presented\n");

    WaitForGPU();
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

// обновление данных для шейдера
void D3D12Context::UpdateCB()
{
    XMMATRIX world = XMMatrixIdentity();

    XMVECTOR cameraPos = XMLoadFloat3(&m_cameraPos);
    XMVECTOR cameraTarget = XMLoadFloat3(&m_cameraTarget);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(cameraPos, cameraTarget, up);

    XMMATRIX proj =
        XMMatrixPerspectiveFovLH(
            XM_PIDIV4,
            (float)m_width / (float)m_height,
            0.1f,
            1000.0f);

    XMStoreFloat4x4(&m_cbData.World, XMMatrixTranspose(world));
    XMStoreFloat4x4(&m_cbData.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&m_cbData.Proj, XMMatrixTranspose(proj));

    float uOff = std::fmod(m_time * m_uvScrollSpeed.x, 1.0f);
    float vOff = std::fmod(m_time * m_uvScrollSpeed.y, 1.0f);
    if (uOff < 0.0f) uOff += 1.0f;
    if (vOff < 0.0f) vOff += 1.0f;

    m_cbData.UVTransform = XMFLOAT4(
        m_uvTiling.x, m_uvTiling.y,
        uOff, vOff
    );

    memcpy(m_cbMappedData, &m_cbData, sizeof(PerObjectCB));
}

// CPU ждет GPU
void D3D12Context::WaitForGPU()
{
    m_fenceValue++;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);

    if (m_fence->GetCompletedValue() < m_fenceValue)
    {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

// загрузка текстуры !!!
bool D3D12Context::CreateTextureFromFile(const char* filePath, UINT srvIndex)
{
    if (!m_device || !m_commandList || !m_srvHeap)
    {
        OutputDebugStringA("CreateTextureFromFile: device/commandList/srvHeap is null\n");
        return false;
    }

    // 0) Проверка что srvIndex влезает в heap (хотя бы грубо)
    // (в D3D12 нельзя прочитать NumDescriptors у heap, так что просто sanity-check)
    // Если хочешь — храни m_srvHeapCapacity в CreateSRVHeap.
    // Тут оставим без жёсткой проверки.

    // 1) Load image via stb_image (force RGBA8)
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load(filePath, &w, &h, &comp, 4);
    if (!pixels || w <= 0 || h <= 0)
    {
        OutputDebugStringA(("stbi_load failed: " + std::string(filePath) + "\n").c_str());
        return false;
    }

    // 2) Create DEFAULT heap texture (GPU resource)
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = (UINT64)w;
    texDesc.Height = (UINT)h;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    Microsoft::WRL::ComPtr<ID3D12Resource> texture;

    HRESULT hr = m_device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,  // важно: сначала COPY_DEST
        nullptr,
        IID_PPV_ARGS(&texture));

    if (FAILED(hr) || !texture)
    {
        stbi_image_free(pixels);
        OutputDebugStringA(("CreateCommittedResource(DEFAULT) failed: " + std::string(filePath) + "\n").c_str());
        return false;
    }

    // 3) Create UPLOAD buffer for texture data
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;

    m_device->GetCopyableFootprints(
        &texDesc,
        0, 1, 0,
        &footprint,
        &numRows,
        &rowSizeInBytes,
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
        stbi_image_free(pixels);
        OutputDebugStringA(("CreateCommittedResource(UPLOAD) failed: " + std::string(filePath) + "\n").c_str());
        return false;
    }

    // 4) Copy pixels -> upload (учитывая RowPitch)
    void* mapped = nullptr;
    hr = upload->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped)
    {
        stbi_image_free(pixels);
        OutputDebugStringA(("Upload Map failed: " + std::string(filePath) + "\n").c_str());
        return false;
    }

    const UINT bytesPerPixel = 4;
    const UINT srcRowBytes = (UINT)w * bytesPerPixel;

    BYTE* dstBase = reinterpret_cast<BYTE*>(mapped) + footprint.Offset;

    for (UINT y = 0; y < (UINT)h; ++y)
    {
        BYTE* dstRow = dstBase + y * footprint.Footprint.RowPitch;
        const BYTE* srcRow = reinterpret_cast<const BYTE*>(pixels) + y * srcRowBytes;
        memcpy(dstRow, srcRow, srcRowBytes);
    }

    upload->Unmap(0, nullptr);
    stbi_image_free(pixels);

    // 5) Record COPY + BARRIER into CURRENT m_commandList
    // ВАЖНО: тут НЕТ Reset/Execute/Wait — этим управляет Initialize()

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    m_commandList->ResourceBarrier(1, &barrier);

    // 6) Create SRV descriptor at [srvIndex]
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MostDetailedMip = 0;
    srv.Texture2D.MipLevels = 1;
    srv.Texture2D.PlaneSlice = 0;
    srv.Texture2D.ResourceMinLODClamp = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += SIZE_T(srvIndex) * SIZE_T(m_srvDescriptorSize);

    m_device->CreateShaderResourceView(texture.Get(), &srv, cpuHandle);

    // 7) Keep resources alive
    m_textures.push_back(texture);
    m_textureUploads.push_back(upload);

    OutputDebugStringA(("Texture queued (srv=" + std::to_string(srvIndex) + "): " + std::string(filePath) + "\n").c_str());
    return true;
}

// создание устройства
bool D3D12Context::CreateDevice()
{
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
        }
    }
#endif

return SUCCEEDED(D3D12CreateDevice(
    nullptr,
    D3D_FEATURE_LEVEL_11_0,
    IID_PPV_ARGS(&m_device)));
}

// отправка команд
bool D3D12Context::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    if (FAILED(m_device->CreateCommandQueue(&q, IID_PPV_ARGS(&m_commandQueue))))
        return false;

    if (FAILED(m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_commandAllocator))))
        return false;

    if (FAILED(m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(),
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

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (!lastSlash)
        return false;
    *(lastSlash + 1) = 0;

    wcscat_s(exePath, L"Shaders.hlsl");

    HRESULT hr = D3DCompileFromFile(
        exePath,
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
        exePath,
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

// массив для текстур !!!
bool D3D12Context::CreateRootSignature()
{
    const UINT MaxSrvCount = 8;

    // диапазон
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = MaxSrvCount;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // передача 2 параметров в шейдер
    D3D12_ROOT_PARAMETER rootParams[2]{};

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

    // правила чтения текстуры
    D3D12_STATIC_SAMPLER_DESC staticSampler{};
    staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; // сглаживание
    staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;  // повторение текстуры
    staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    staticSampler.ShaderRegister = 0;
    staticSampler.ShaderVisibility =
        D3D12_SHADER_VISIBILITY_PIXEL;


    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
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
    pso.PS = { m_ps->GetBufferPointer(), m_ps->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
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
    UINT cbSize = (sizeof(PerObjectCB) + 255) & ~255u;

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
    XMStoreFloat4x4(&m_cbData.World, XMMatrixTranspose(I));

    D3D12_RANGE readRange{ 0,0 };
    if (FAILED(m_constantBuffer->Map(0, &readRange, (void**)&m_cbMappedData)))
        return false;

    std::memcpy(m_cbMappedData, &m_cbData, sizeof(PerObjectCB));
    return true;
}


// тайлинг и офсет !!!
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
}

// заверщение работы
void D3D12Context::SetRotation(float t)
{
    m_rotationT = t;
}