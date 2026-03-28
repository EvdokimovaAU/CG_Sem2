#include <windows.h>
#include "Application.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    Application app(hInstance, nCmdShow);
    if (!app.Initialize())
        return 0;

    return app.Run();
}
