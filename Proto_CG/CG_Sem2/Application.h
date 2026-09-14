#pragma once
#include <windows.h>

#include "Win32Window.h"
#include "GameTimer.h"
#include "RenderingSystem.h"
#include "InputDevice.h"

class Application
{
public:
    Application(HINSTANCE hInstance, int nCmdShow);

    bool Initialize();
    int  Run();

private:
    HINSTANCE   m_hInstance{}; 
    int         m_nCmdShow{};

    Win32Window  m_window;
    GameTimer    m_timer;
    RenderingSystem m_renderingSystem;
    InputDevice  m_input;
    bool m_scene1WasDown = false;
    bool m_scene2WasDown = false;
    bool m_scene3WasDown = false;
    bool m_scene4WasDown = false;
    bool m_scene5WasDown = false;
    UINT m_selectedTerrainTile = 0;
    bool m_previousTileWasDown = false;
    bool m_nextTileWasDown = false;
    bool m_toggleTileWasDown = false;
    bool m_toggleTerrainLodWasDown = false;
    bool m_toggleTerrainDebugWasDown = false;
    bool m_toggleFrustumWasDown = false;
    bool m_toggleOctreeWasDown = false;
    bool m_toggleBrdfWasDown = false;
};
