#include "Win32Window.h"
#include "InputDevice.h"
#include <windowsx.h>

bool Win32Window::Create
(HINSTANCE hInstance, int nCmdShow,
    int width, int height,
    const wchar_t* className,
    const wchar_t* title)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.style = CS_HREDRAW | CS_VREDRAW;

    RegisterClassExW(&wc);

    RECT rc{ 0,0,width,height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(
        0,
        className,
        title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left,
        rc.bottom - rc.top,
        nullptr, nullptr,
        hInstance,
        this);

    if (!m_hwnd)
        return false;

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);
    return true;
}

LRESULT CALLBACK Win32Window::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Win32Window* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<Win32Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    else
    {
        self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self)
        return self->WndProc(hwnd, msg, wParam, lParam);

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK Win32Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if (m_inputDevice)
            m_inputDevice->OnKeyDown(wParam);
        return 0;

    case WM_KEYUP:
        if (m_inputDevice)
            m_inputDevice->OnKeyUp(wParam);
        return 0;

    case WM_MOUSEMOVE:
        if (m_inputDevice)
        {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            m_inputDevice->OnMouseMove(x, y);
        }
        return 0;

    case WM_RBUTTONDOWN:
        if (m_inputDevice)
        {
            m_inputDevice->ResetMouseAnchor();
            m_inputDevice->OnMouseButtonDown(VK_RBUTTON);
        }
        SetCapture(hwnd);
        return 0;

    case WM_RBUTTONUP:
        if (m_inputDevice)
            m_inputDevice->OnMouseButtonUp(VK_RBUTTON);
        ReleaseCapture();
        return 0;

    case WM_LBUTTONDOWN:
        if (m_inputDevice)
        {
            m_inputDevice->ResetMouseAnchor();
            m_inputDevice->OnMouseButtonDown(VK_LBUTTON);
        }
        SetCapture(hwnd);
        return 0;

    case WM_LBUTTONUP:
        if (m_inputDevice)
            m_inputDevice->OnMouseButtonUp(VK_LBUTTON);
        ReleaseCapture();
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}