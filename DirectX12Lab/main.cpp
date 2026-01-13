#include "Application.h"
#include <windows.h>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    App app;

    if (!app.Initialize(hInstance, nCmdShow))
        return 0;

    return app.Run();
}