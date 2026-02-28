#include "Application.h"
#include <windows.h>

Application::Application(HINSTANCE hInstance, int nCmdShow)
    : m_hInstance(hInstance), m_nCmdShow(nCmdShow)
{
}

bool Application::Initialize()
{
    if (!m_window.Create(
        m_hInstance,
        m_nCmdShow,
        800,
        600,
        L"DX12WindowClass",
        L"KG_Laba4 - DX12 Final"))
    {
        MessageBoxW(nullptr, L"Window Create FAILED", L"Error", MB_OK);
        return false;
    }

    m_window.SetInputDevice(&m_input);

    if (!m_dx12.Initialize(m_window.GetHWND(), 1200, 1000))
    {
        MessageBoxW(nullptr, L"DX12 Initialize FAILED (see Output window)", L"Error", MB_OK);
        return false;
    }
    
    
    m_dx12.SetUVTiling(4.0f, 4.0f);          
    m_dx12.SetUVScrollSpeed(0.15f, 0.0f);

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

        m_timer.Tick();
        float deltaTime = m_timer.DeltaTime();

        bool orbitRotate = m_input.IsMouseButtonDown(VK_RBUTTON);
        bool dolly = m_input.IsMouseButtonDown(VK_LBUTTON);

        int mouseDeltaX = 0, mouseDeltaY = 0;
        if (orbitRotate || dolly)
        {
            m_input.GetMouseDelta(mouseDeltaX, mouseDeltaY);
        }
        m_input.ResetMouseDelta();

        // скорость для 
        const float rotateSpeed = 0.005f; 
        const float dollySpeed = 0.2f; 


        m_dx12.UpdateCameraOrbit(
            deltaTime,
            rotateSpeed,
            dollySpeed,
            orbitRotate,
            dolly,
            (float)mouseDeltaX,
            (float)mouseDeltaY);

        m_dx12.SetTime(m_timer.TotalTime());

        m_dx12.Render(0.48f, 0.52f, 0.80f, 1.0f);
    }
}