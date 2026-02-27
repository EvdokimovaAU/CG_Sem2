#pragma once
#include <windows.h>

#include "Win32Window.h"
#include "GameTimer.h"
#include "D3D12Context.h"
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
    D3D12Context m_dx12;
    InputDevice  m_input;
};
