#pragma once
#include <windows.h>
#include <unordered_map>

class InputDevice {
public:
    void OnKeyDown(WPARAM key);
    void OnKeyUp(WPARAM key);

    void OnMouseMove(int x, int y);
    void OnMouseButtonDown(int button);
    void OnMouseButtonUp(int button);

    bool IsKeyDown(WPARAM key) const;
    bool IsMouseButtonDown(int button) const;

    void GetMouseDelta(int& dx, int& dy) const;
    void ResetMouseDelta();
    void ResetMouseAnchor();

private:
    std::unordered_map<WPARAM, bool> m_keys;
    std::unordered_map<int, bool> m_mouseButtons;

    int m_lastMouseX = 0;
    int m_lastMouseY = 0;
    int m_mouseDeltaX = 0;
    int m_mouseDeltaY = 0;
    bool m_firstMouse = true;
};