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
{
    LARGE_INTEGER t{}; QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}
static double GetQpf()
{
    LARGE_INTEGER f{}; QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
}

// ─────────────────────────────────────────────────────────────────────────────
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

        m_renderer = new RenderingSystem();          // ← было: new D3D12Context()

        RECT rc{};
        GetClientRect(m_window->GetHwnd(), &rc);
        uint32_t w = (uint32_t)(rc.right  - rc.left);
        uint32_t h = (uint32_t)(rc.bottom - rc.top);

        if (!m_renderer->Initialize(m_window->GetHwnd(), w, h))
            return false;

        return true;
    }
    catch (const std::exception& e) {
        std::string msg = "Initialization failed: ";
        msg += e.what();
        MessageBoxA(nullptr, msg.c_str(), "Error", MB_OK | MB_ICONERROR);
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void App::Render()
{
    if (m_renderer) m_renderer->Draw();              // ← было: m_dx12->Draw()
}

// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
void App::Update(float dt)
{
    using namespace DirectX;

    if (m_input && m_input->IsKeyDown(VK_ESCAPE))
        m_exitRequested = true;

    if (!m_input || !m_renderer) return;

    const bool rmb = m_input->IsKeyDown(VK_RBUTTON);
    if (rmb)
    {
        HWND hwnd = m_window ? m_window->GetHwnd() : nullptr;
        if (!hwnd) return;

        RECT rc{};
        GetClientRect(hwnd, &rc);
        POINT centerClient{ (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
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

        const float limit = DirectX::XM_PIDIV2 - 0.1f;
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

    m_renderer->SetCamera(m_camPos, m_camYaw, m_camPitch); // ← было: m_dx12->SetCamera
}

// ─────────────────────────────────────────────────────────────────────────────
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
        if (m_input) m_input->OnKeyDown(static_cast<uint32_t>(wparam));
        return 0;

    case WM_KEYUP:
        if (m_input) m_input->OnKeyUp(static_cast<uint32_t>(wparam));
        return 0;

    case WM_RBUTTONDOWN:
        if (m_input) m_input->OnKeyDown(VK_RBUTTON);
        m_rmbLook = true;
        m_justEnteredRmbLook = true;
        GetCursorPos(&m_savedCursorPos);
        ShowCursor(FALSE);
        SetCapture(hwnd);
        return 0;

    case WM_RBUTTONUP:
        if (m_input) m_input->OnKeyUp(VK_RBUTTON);
        m_rmbLook = false;
        SetCursorPos(m_savedCursorPos.x, m_savedCursorPos.y);
        ShowCursor(TRUE);
        ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE:
        if (m_input) m_input->OnMouseMove(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;

    case WM_SIZE:
    {
        uint32_t w = (uint32_t)LOWORD(lparam);
        uint32_t h = (uint32_t)HIWORD(lparam);
        if (w == 0 || h == 0) return 0;
        if (m_renderer) m_renderer->OnResize(w, h); // ← было: m_dx12->OnResize
        return 0;
    }

    default:
        return DefWindowProc(hwnd, msg, wparam, lparam);
    }
}
