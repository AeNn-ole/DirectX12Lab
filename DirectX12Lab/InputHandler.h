#pragma once
#include <cstdint>

class Input
{
public:
    void Reset();

    void OnKeyDown(uint32_t vk);
    void OnKeyUp(uint32_t vk);
    void OnMouseMove(int x, int y);
    bool IsKeyDown(uint32_t vk) const;

    int MouseX() const { return m_mouseX; }
    int MouseY() const { return m_mouseY; }

private:
    bool m_keys[256]{};
    int m_mouseX = 0;
    int m_mouseY = 0;
};
