#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Application.h"
#include "WindowManager.h"
#include "InputHandler.h"
#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <string>
#include <DirectXMath.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")

static uint64_t GetQpc()
{ LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
static double GetQpf()
{ LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }

bool App::Initialize(HINSTANCE hInstance, int nCmdShow)
{
    try {
        m_window = new Window();
        m_input  = new Input();
        m_input->Reset();

        if (!m_window->Create(this, hInstance, nCmdShow, 1280, 720, L"DX12 Deferred"))
            return false;

        m_secondsPerTick = 1.0 / GetQpf();
        m_prevTick       = GetQpc();

        m_renderer = new RenderingSystem();

        RECT rc{};
        GetClientRect(m_window->GetHwnd(), &rc);
        uint32_t w = (uint32_t)(rc.right  - rc.left);
        uint32_t h = (uint32_t)(rc.bottom - rc.top);

        if (!m_renderer->Initialize(m_window->GetHwnd(), w, h))
            return false;

        return true;
    }
    catch (const std::exception& e) {
        std::string msg = "Initialization failed: "; msg += e.what();
        MessageBoxA(nullptr, msg.c_str(), "Error", MB_OK | MB_ICONERROR);
        return false;
    }
}

void App::Render() { if (m_renderer) m_renderer->Draw(); }

int App::Run()
{
    MSG msg{};
    while (!m_exitRequested)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { m_exitRequested = true; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        const uint64_t now = GetQpc();
        const double   dt  = (now - m_prevTick) * m_secondsPerTick;
        m_prevTick = now;
        Update(static_cast<float>(dt));
        Render();
    }
    return 0;
}

void App::Update(float dt)
{
    using namespace DirectX;

    if (m_input && m_input->IsKeyDown(VK_ESCAPE)) m_exitRequested = true;
    if (!m_input || !m_renderer) return;

    // ── Читаем текущие состояния хоткеев ─────────────────────────────────
    const bool f1    = m_input->IsKeyDown(VK_F1);
    const bool f2    = m_input->IsKeyDown(VK_F2);
    const bool f3    = m_input->IsKeyDown(VK_F3);
    const bool f5    = m_input->IsKeyDown('Z');
    const bool f6    = m_input->IsKeyDown(VK_F5);
    const bool f7    = m_input->IsKeyDown(VK_F6);
    const bool f4    = m_input->IsKeyDown(VK_F4);
    const bool plus  = m_input->IsKeyDown(VK_OEM_PLUS)  || m_input->IsKeyDown(VK_ADD);
    const bool minus = m_input->IsKeyDown(VK_OEM_MINUS) || m_input->IsKeyDown(VK_SUBTRACT);
    const bool rbrk  = m_input->IsKeyDown(VK_OEM_6);   // ]
    const bool lbrk  = m_input->IsKeyDown(VK_OEM_4);   // [

    // ── F1: frustum culling toggle ────────────────────────────────────────
    if (f1 && !m_prevF1)
    {
        m_renderer->ToggleFrustumCulling();
        if (!m_renderer->IsFrustumCullingOn() && m_renderer->IsOctreeCullingOn())
            m_renderer->ToggleOctreeCulling();
    }

    // ── F2: octree culling toggle ─────────────────────────────────────────
    if (f2 && !m_prevF2)
    {
        if (!m_renderer->IsFrustumCullingOn())
            m_renderer->ToggleFrustumCulling();
        m_renderer->ToggleOctreeCulling();
    }

    // ── F3: tessellation toggle ───────────────────────────────────────────
    if (f3 && !m_prevF3)
        m_renderer->ToggleTessellation();

    // ── F4: normal mapping toggle ─────────────────────────────────────────
    if (f4 && !m_prevF4)
        m_renderer->ToggleNormalMapping();

    // ── Z: wireframe toggle ─────────────────────────────────────────────
    if (f5 && !m_prevF5)
        m_renderer->ToggleWireframe();

    // ── F5: cycle post-effects ───────────────────────────────────────────
    if (f6 && !m_prevF6)
        m_renderer->CyclePostFx();

    // ── F6: debug cascade colours ─────────────────────────────────────────
    if (f7 && !m_prevF7)
        m_renderer->ToggleDebugCascades();

    // ── +/- : tessellation factor (near) ──────────────────────────────────
    if (plus && !m_prevPlus)
    {
        float f = std::min(m_renderer->GetTessFactorNear() + 1.f, 64.f);
        m_renderer->SetTessFactor(f, 1.f);
    }
    if (minus && !m_prevMinus)
    {
        float f = std::max(m_renderer->GetTessFactorNear() - 1.f, 1.f);
        m_renderer->SetTessFactor(f, 1.f);
    }

    // ── [ / ] : displacement scale ────────────────────────────────────────
    if (rbrk && !m_prevRBrk)
        m_renderer->SetDisplacementScale(m_renderer->GetDisplacementScale() + 1.f);
    if (lbrk && !m_prevLBrk)
        m_renderer->SetDisplacementScale(
            std::max(m_renderer->GetDisplacementScale() - 1.f, 0.f));

    // Сохраняем предыдущие состояния
    m_prevF1    = f1;
    m_prevF2    = f2;
    m_prevF3    = f3;
    m_prevF4    = f4;
    m_prevF5    = f5;
    m_prevF6    = f6;
    m_prevF7    = f7;
    m_prevPlus  = plus;
    m_prevMinus = minus;
    m_prevRBrk  = rbrk;
    m_prevLBrk  = lbrk;

    // ── Заголовок окна (~раз в 0.5 сек) ──────────────────────────────────
    m_titleTimer += dt;
    if (m_titleTimer >= 0.5f && m_window)
    {
        m_titleTimer = 0.f;

        const char* cullMode = "No culling";
        if (m_renderer->IsFrustumCullingOn())
            cullMode = m_renderer->IsOctreeCullingOn()
                ? "Frustum+Octree" : "Frustum";

        wchar_t title[512];
        swprintf_s(title,
            L"DX12 Deferred  |  Vis:%d  |  Cull:%hs  |"
            L"  Tess:%hs(%.0f)  |  Disp:%.1f  |  NM:%hs  |  Wire:%hs"
            L"  |  F1=Frustum F2=Oct F3=Tess F4=NM F5=FX F6=CscDbg Z=Wire +/-=Factor []= Disp",
            m_renderer->GetVisibleCount(),
            cullMode,
            m_renderer->IsTessellationOn() ? "ON " : "OFF",
            m_renderer->GetTessFactorNear(),
            m_renderer->GetDisplacementScale(),
            m_renderer->IsNormalMappingOn() ? "ON" : "OFF",
            m_renderer->IsWireframeOn()    ? "ON" : "OFF",
            [&]() -> const char* {
                switch(m_renderer->GetPostFxMode()) {
                    case 1: return "FishEye";
                    case 2: return "VHS";
                    case 3: return "Both";
                    default: return "Off";
                }
            }());

        SetWindowTextW(m_window->GetHwnd(), title);
    }

    // ── Движение камеры ───────────────────────────────────────────────────
    const bool rmb = m_input->IsKeyDown(VK_RBUTTON);
    if (rmb)
    {
        HWND hwnd = m_window ? m_window->GetHwnd() : nullptr;
        if (!hwnd) return;

        RECT rc{};
        GetClientRect(hwnd, &rc);
        POINT centerClient{ (rc.right-rc.left)/2, (rc.bottom-rc.top)/2 };
        POINT centerScreen = centerClient;
        ClientToScreen(hwnd, &centerScreen);

        if (m_justEnteredRmbLook)
        {
            SetCursorPos(centerScreen.x, centerScreen.y);
            m_justEnteredRmbLook = false;
            return;
        }

        POINT curScreen{};
        GetCursorPos(&curScreen);
        const int   dx = curScreen.x - centerScreen.x;
        const int   dy = curScreen.y - centerScreen.y;
        const float mouseSens = 0.005f;
        m_camYaw   += dx * mouseSens;
        m_camPitch -= dy * mouseSens;
        const float limit = XM_PIDIV2 - 0.1f;
        m_camPitch = std::clamp(m_camPitch, -limit, limit);
        SetCursorPos(centerScreen.x, centerScreen.y);
    }

    float speed = 200.0f;
    if (m_input->IsKeyDown(VK_SHIFT)) speed = 600.0f;

    XMVECTOR fwd   = XMVector3Normalize(XMVectorSet(sinf(m_camYaw), 0.f, cosf(m_camYaw), 0.f));
    XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0,1,0,0), fwd));
    XMVECTOR up    = XMVectorSet(0,1,0,0);
    XMVECTOR move  = XMVectorZero();

    if (m_input->IsKeyDown('W')) move = XMVectorAdd(move, fwd);
    if (m_input->IsKeyDown('S')) move = XMVectorSubtract(move, fwd);
    if (m_input->IsKeyDown('D')) move = XMVectorAdd(move, right);
    if (m_input->IsKeyDown('A')) move = XMVectorSubtract(move, right);
    if (m_input->IsKeyDown('E')) move = XMVectorAdd(move, up);
    if (m_input->IsKeyDown('Q')) move = XMVectorSubtract(move, up);

    if (!XMVector3Equal(move, XMVectorZero()))
    {
        move = XMVector3Normalize(move);
        XMVECTOR pos = XMVectorSet(m_camPos.x, m_camPos.y, m_camPos.z, 1.f);
        pos = XMVectorAdd(pos, XMVectorScale(move, speed * dt));
        XMStoreFloat3(&m_camPos, pos);
    }

    m_renderer->SetCamera(m_camPos, m_camYaw, m_camPitch);
}

LRESULT App::HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_CLOSE:
    case WM_DESTROY:
        m_exitRequested = true;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (m_input) m_input->OnKeyDown((uint32_t)wparam);
        return 0;
    case WM_KEYUP:
        if (m_input) m_input->OnKeyUp((uint32_t)wparam);
        return 0;
    case WM_RBUTTONDOWN:
        if (m_input) m_input->OnKeyDown(VK_RBUTTON);
        m_rmbLook = true;
        m_justEnteredRmbLook = true;
        GetCursorPos(&m_savedCursorPos);
        ShowCursor(FALSE); SetCapture(hwnd);
        return 0;
    case WM_RBUTTONUP:
        if (m_input) m_input->OnKeyUp(VK_RBUTTON);
        m_rmbLook = false;
        SetCursorPos(m_savedCursorPos.x, m_savedCursorPos.y);
        ShowCursor(TRUE); ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        if (m_input) m_input->OnMouseMove(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
    case WM_SIZE:
    {
        uint32_t w = (uint32_t)LOWORD(lparam);
        uint32_t h = (uint32_t)HIWORD(lparam);
        if (w == 0 || h == 0) return 0;
        if (m_renderer) m_renderer->OnResize(w, h);
        return 0;
    }
    default:
        return DefWindowProc(hwnd, msg, wparam, lparam);
    }
}
