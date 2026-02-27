#include <windows.h>
#include "Application.h"
#include "tiny_obj_loader.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    Application app(hInstance, nCmdShow);
    if (!app.Initialize())
        return 0;

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    bool ret = tinyobj::LoadObj
    (
        &attrib, &shapes, &materials,
        &warn, &err,
        "models/sponza.obj",
        "models",
        true
    );

    /*if (!ret)
    {
        std::cerr << ".obj load error: " << err << std::endl;
        return 1;
    }*/



    return app.Run();
}
