#pragma once
#include <windows.h>

class App;

class Window
{
public:
    bool Create(App* app, HINSTANCE hInstance, int nCmdShow, int width, int height, const wchar_t* title);
    HWND GetHwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

private:
    HWND m_hwnd = nullptr;
};
