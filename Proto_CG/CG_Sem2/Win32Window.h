#pragma once
#include <windows.h>

class InputDevice;

class Win32Window
{
public:
    bool Create(HINSTANCE hInstance, int nCmdShow,
        int width, int height,
        const wchar_t* className,
        const wchar_t* title);

    HWND GetHWND() const { return m_hwnd; }

    void SetInputDevice(InputDevice* inputDevice) { m_inputDevice = inputDevice; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT CALLBACK        WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HWND m_hwnd = nullptr;
    InputDevice* m_inputDevice = nullptr;
};