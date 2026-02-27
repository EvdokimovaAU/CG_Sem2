#include "InputDevice.h"

void InputDevice::OnKeyDown(WPARAM key)
{
    m_keys[key] = true;
}

void InputDevice::OnKeyUp(WPARAM key)
{
    m_keys[key] = false;
}

bool InputDevice::IsKeyDown(WPARAM key) const
{
    auto it = m_keys.find(key);
    return it != m_keys.end() && it->second;
}

void InputDevice::OnMouseMove(int x, int y)
{
    if (m_firstMouse)
    {
        m_lastMouseX = x;
        m_lastMouseY = y;
        m_firstMouse = false;

        // На первом move после ResetMouseAnchor не даём дельту
        m_mouseDeltaX = 0;
        m_mouseDeltaY = 0;
        return;
    }

    m_mouseDeltaX = x - m_lastMouseX;
    m_mouseDeltaY = y - m_lastMouseY;

    m_lastMouseX = x;
    m_lastMouseY = y;
}

void InputDevice::OnMouseButtonDown(int button)
{
    m_mouseButtons[button] = true;
}

void InputDevice::OnMouseButtonUp(int button)
{
    m_mouseButtons[button] = false;
}

bool InputDevice::IsMouseButtonDown(int button) const
{
    auto it = m_mouseButtons.find(button);
    return it != m_mouseButtons.end() && it->second;
}

void InputDevice::GetMouseDelta(int& dx, int& dy) const
{
    dx = m_mouseDeltaX;
    dy = m_mouseDeltaY;
}

void InputDevice::ResetMouseDelta()
{
    m_mouseDeltaX = 0;
    m_mouseDeltaY = 0;
}

void InputDevice::ResetMouseAnchor()
{
    m_firstMouse = true;
    m_mouseDeltaX = 0;
    m_mouseDeltaY = 0;
}