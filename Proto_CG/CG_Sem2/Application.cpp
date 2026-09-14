#include "Application.h"
#include <windows.h>
#include <algorithm>
#include <cwchar>

Application::Application(HINSTANCE hInstance, int nCmdShow)
    : m_hInstance(hInstance), m_nCmdShow(nCmdShow)
{
}

bool Application::Initialize()
{
    constexpr UINT windowWidth = 800;
    constexpr UINT windowHeight = 600;

    if (!m_window.Create(
        m_hInstance,
        m_nCmdShow,
        windowWidth,
        windowHeight,
        L"DX12WindowClass",
        L"KG_Laba4 - DX12 Final"))
    {
        MessageBoxW(nullptr, L"Window Create FAILED", L"Error", MB_OK);
        return false;
    }

    m_window.SetInputDevice(&m_input);

    if (!m_renderingSystem.Initialize(m_window.GetHWND(), windowWidth, windowHeight))
    {
        MessageBoxW(nullptr, L"DX12 Initialize FAILED (see Output window)", L"Error", MB_OK);
        return false;
    }
    
    m_renderingSystem.SetTechnique(RenderingSystem::Technique::Deferred);
    m_renderingSystem.SetUVTiling(1.0f, 1.0f);
    m_renderingSystem.SetUVScrollSpeed(0.0f, 0.0f);
    m_renderingSystem.SetClearColor(0.48f, 0.52f, 0.80f, 1.0f);

    m_timer.Reset();
    return true;
}

int Application::Run()
{
    MSG msg{};
    while (true)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                return (int)msg.wParam;

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
        {
            PostQuitMessage(0);
            continue;
        }

        const bool scene1Down = m_input.IsKeyDown('1');
        const bool scene2Down = m_input.IsKeyDown('2');
        const bool scene3Down = m_input.IsKeyDown('3');
        const bool scene4Down = m_input.IsKeyDown('4');
        const bool scene5Down = m_input.IsKeyDown('5');
        const bool toggleFrustumDown = m_input.IsKeyDown('Q');
        const bool toggleOctreeDown = m_input.IsKeyDown('E');
        const bool toggleBrdfDown = m_input.IsKeyDown('R');

        if (scene1Down && !m_scene1WasDown)
        {
            m_renderingSystem.LoadScene(RenderingSystem::Scene::HighPlane);
        }

        if (scene2Down && !m_scene2WasDown)
        {
            m_renderingSystem.LoadScene(RenderingSystem::Scene::Sponza);
        }

        if (scene3Down && !m_scene3WasDown)
        {
            m_renderingSystem.LoadScene(RenderingSystem::Scene::ChickenField);
        }

        if (scene4Down && !m_scene4WasDown)
        {
            m_renderingSystem.LoadScene(RenderingSystem::Scene::CerberusPbr);
        }

        if (scene5Down && !m_scene5WasDown)
        {
            m_selectedTerrainTile = 0;
            if (!m_renderingSystem.LoadScene(RenderingSystem::Scene::Terrain))
                MessageBoxW(m_window.GetHWND(), L"Cannot load Terrain. Check models/Heightmap.png next to the executable and the debug output.", L"Terrain", MB_OK | MB_ICONERROR);
        }

        const bool previousTileDown = m_input.IsKeyDown(VK_OEM_4);
        const bool nextTileDown = m_input.IsKeyDown(VK_OEM_6);
        const bool toggleTileDown = m_input.IsKeyDown('T');
        const UINT tileCount = m_renderingSystem.GetTerrainTileCount();
        const bool lodDown = m_input.IsKeyDown('L');
        const bool debugDown = m_input.IsKeyDown('V');
        if (tileCount > 0 && lodDown && !m_toggleTerrainLodWasDown)
            m_renderingSystem.SetTerrainLodEnabled(!m_renderingSystem.IsTerrainLodEnabled());
        if (tileCount > 0 && debugDown && !m_toggleTerrainDebugWasDown)
            m_renderingSystem.SetTerrainLodDebug(!m_renderingSystem.IsTerrainLodDebug());
        m_toggleTerrainLodWasDown = lodDown;
        m_toggleTerrainDebugWasDown = debugDown;
        if (tileCount > 0)
        {
            if (previousTileDown && !m_previousTileWasDown)
                m_selectedTerrainTile = (m_selectedTerrainTile + tileCount - 1) % tileCount;
            if (nextTileDown && !m_nextTileWasDown)
                m_selectedTerrainTile = (m_selectedTerrainTile + 1) % tileCount;
            if (toggleTileDown && !m_toggleTileWasDown)
                m_renderingSystem.SetTerrainTileEnabled(m_selectedTerrainTile,
                    !m_renderingSystem.IsTerrainTileEnabled(m_selectedTerrainTile));
        }
        if (tileCount > 0 || scene1Down || scene2Down || scene3Down || scene4Down || scene5Down ||
            previousTileDown || nextTileDown || toggleTileDown)
        {
            wchar_t title[256];
            if (tileCount > 0)
            {
                const auto counts = m_renderingSystem.GetTerrainLodCounts();
                swprintf_s(title, L"Terrain | LOD 0/1/2/3: %u/%u/%u/%u | L: %ls | V: colors | Tile %u/%u %ls [ ] T",
                    counts[0], counts[1], counts[2], counts[3],
                    m_renderingSystem.IsTerrainLodEnabled() ? L"AUTO" : L"FINE",
                    m_selectedTerrainTile + 1, tileCount,
                    m_renderingSystem.IsTerrainTileEnabled(m_selectedTerrainTile) ? L"ON" : L"OFF");
            }
            else
                swprintf_s(title, L"KG_Laba4 - DX12 Final | Scenes 1-5 (5: Terrain)");
            SetWindowTextW(m_window.GetHWND(), title);
        }
        m_previousTileWasDown = previousTileDown;
        m_nextTileWasDown = nextTileDown;
        m_toggleTileWasDown = toggleTileDown;

        if (toggleFrustumDown && !m_toggleFrustumWasDown)
        {
            const bool enabled = m_renderingSystem.IsFrustumCullingEnabled();
            m_renderingSystem.SetFrustumCullingEnabled(!enabled);
        }

        if (toggleOctreeDown && !m_toggleOctreeWasDown)
        {
            const bool enabled = m_renderingSystem.IsOctreeEnabled();
            m_renderingSystem.SetOctreeEnabled(!enabled);
        }

        if (toggleBrdfDown && !m_toggleBrdfWasDown)
        {
            const bool useGgx = m_renderingSystem.IsUsingGgxDistribution();
            m_renderingSystem.SetUseGgxDistribution(!useGgx);
        }

        m_scene1WasDown = scene1Down;
        m_scene2WasDown = scene2Down;
        m_scene3WasDown = scene3Down;
        m_scene4WasDown = scene4Down;
        m_scene5WasDown = scene5Down;
        m_toggleFrustumWasDown = toggleFrustumDown;
        m_toggleOctreeWasDown = toggleOctreeDown;
        m_toggleBrdfWasDown = toggleBrdfDown;

        m_timer.Tick();
        float deltaTime = m_timer.DeltaTime();

        float forwardInput = 0.0f;
        float strafeInput = 0.0f;
        if (m_input.IsKeyDown('W')) forwardInput += 1.0f;
        if (m_input.IsKeyDown('S')) forwardInput -= 1.0f;
        if (m_input.IsKeyDown('D')) strafeInput += 1.0f;
        if (m_input.IsKeyDown('A')) strafeInput -= 1.0f;

        bool orbitRotate = m_input.IsMouseButtonDown(VK_RBUTTON);
        bool dolly = m_input.IsMouseButtonDown(VK_LBUTTON);

        int mouseDeltaX = 0, mouseDeltaY = 0;
        if (orbitRotate || dolly)
        {
            m_input.GetMouseDelta(mouseDeltaX, mouseDeltaY);
        }
        m_input.ResetMouseDelta();

        // скорость для 
        const float rotateSpeed = 0.0035f;
        const float dollySpeed = 1.5f;
        const float moveSpeed = 260.0f;


        m_renderingSystem.UpdateCameraOrbit(
            deltaTime,
            rotateSpeed,
            dollySpeed,
            orbitRotate,
            dolly,
            (float)mouseDeltaX,
            (float)mouseDeltaY);
        float verticalInput = 0.0f;
        const bool terrain = m_renderingSystem.GetCurrentScene() == RenderingSystem::Scene::Terrain;
        if (terrain && m_input.IsKeyDown(VK_SPACE)) verticalInput += 1.0f;
        if (terrain && m_input.IsKeyDown(VK_CONTROL)) verticalInput -= 1.0f;
        const float boost = terrain && m_input.IsKeyDown(VK_SHIFT) ? 3.0f : 1.0f;
        m_renderingSystem.UpdateCameraMove((std::min)(deltaTime, 0.1f), forwardInput, strafeInput, moveSpeed * boost, verticalInput);

        m_renderingSystem.SetTime(m_timer.TotalTime());
        m_renderingSystem.RenderFrame();
    }
}
