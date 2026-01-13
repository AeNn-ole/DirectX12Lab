#include "InputHandler.h"
#include <cstring>

void Input::Reset() {
    std::memset(m_keys, 0, sizeof(m_keys));
    m_mouseX = 0;
    m_mouseY = 0;
}

void Input::OnKeyDown(uint32_t vk) { if (vk < 256) m_keys[vk] = true; }

void Input::OnKeyUp(uint32_t vk) { if (vk < 256) m_keys[vk] = false; }

void Input::OnMouseMove(int x, int y) {
    m_mouseX = x;
    m_mouseY = y;
}

bool Input::IsKeyDown(uint32_t vk) const { return (vk < 256) ? m_keys[vk] : false; }
