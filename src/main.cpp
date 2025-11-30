#include "ui/ImguiApp.h"

int main(int, char**)
{
    ImguiApp app;
    if (!app.init())
        return 1;

    app.run();
    // 析构中会自动 shutdown()

    return 0;
}
