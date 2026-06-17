#ifndef NOMINMAX
#define NOMINMAX
#endif
#pragma once
#include <windows.h>
#include <cstdint>
#include "RenderingSystem.h"

class Window;
class Input;

class App
{
public:
    bool Initialize(HINSTANCE hInstance, int nCmdShow);
    int Run();
    LRESULT HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

private:
    void Update(float dt);
    void Render();

private:
    Window*          m_window   = nullptr;
    Input*           m_input    = nullptr;
    RenderingSystem* m_renderer = nullptr;

    bool m_exitRequested = false;

    uint64_t m_prevTick       = 0;
    double   m_secondsPerTick = 0.;

    float m_camYaw   = 1.f;
    float m_camPitch = 0.f;
    DirectX::XMFLOAT3 m_camPos{ -5.f, 1.f, -5.f };

    bool  m_rmbLook            = false;
    POINT m_savedCursorPos     { 0, 0 };
    bool  m_justEnteredRmbLook = false;

    // Edge-trigger флаги для всех хоткеев
    bool  m_prevF1 = false;   // frustum culling
    bool  m_prevF2 = false;   // octree culling
    bool  m_prevF3 = false;   // tessellation toggle
    bool  m_prevF4 = false;   // normal map toggle
    bool  m_prevF5 = false;   // wireframe toggle
    bool  m_prevF6 = false;   // post-fx cycle
    bool  m_prevF7 = false;   // debug cascades
    bool  m_prevWaterToggle = false; // X
    bool  m_prevAmpUp       = false; // .
    bool  m_prevAmpDown     = false; // ,
    bool  m_prevTileUp      = false; // M
    bool  m_prevTileDown    = false; // N (для отладки)

    // Клавиши изменения параметров тесселяции
    bool  m_prevPlus  = false; // увеличить tess factor
    bool  m_prevMinus = false; // уменьшить tess factor
    bool  m_prevRBrk  = false; // ] увеличить displacement scale
    bool  m_prevLBrk  = false; // [ уменьшить displacement scale

    bool  m_wireframe = false;

    float m_titleTimer = 0.f;
};
