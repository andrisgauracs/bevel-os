// App registry.
#include <string.h>
#include "apps.h"
#include "os.h"

const app_def_t os_apps[] = {
    {"Files",     "&Files",              &icon16_drive,    &icon32_drive,    app_files,    APP_ON_DESKTOP},
    {"Editor",    "&Text Editor",        &icon16_text,     &icon32_text,     app_editor,   APP_ON_DESKTOP},
    {"Browser",   "&Web Browser",        &icon16_app,      &icon32_app,      app_browser,  APP_ON_DESKTOP},
    {"Camera",    "&Camera",             &icon16_camera,   &icon32_camera,   app_camera,   APP_ON_DESKTOP},
    {"Player",    "Sound &Player",       &icon16_sound,    &icon32_sound,    app_player,   APP_ON_DESKTOP},
    {"Network",   "&Network",            &icon16_computer, &icon32_computer, app_network,  APP_ON_DESKTOP},
    {"Terminal",  "T&erminal",           &icon16_terminal, &icon32_terminal, app_terminal, APP_ON_DESKTOP},
    {"Settings",  "&Settings",           &icon16_settings, &icon32_settings, app_settings, APP_ON_DESKTOP},
    {"System",    "System &Information", &icon16_computer, &icon32_computer, app_system,   APP_ON_DESKTOP},
    {"Tasks",     "T&asks",              &icon16_tasks,    &icon32_tasks,    app_tasks,    0},
    {"Help",      "&Help",               &icon16_help,     &icon32_help,     app_help,     APP_ON_DESKTOP},
    {"Benchmark", "&Benchmark",          &icon16_tasks,    &icon32_tasks,    app_bench,    APP_IN_PROGRAMS | APP_ON_DESKTOP},
    {"Gallery",   "&Widget Gallery",     &icon16_app,      &icon32_app,      app_gallery,  APP_IN_PROGRAMS},
    {"Viewer",    "&Picture Viewer",     &icon16_image,    &icon32_image,    app_viewer,   APP_HIDDEN},
};
const int os_app_count = sizeof(os_apps) / sizeof(os_apps[0]);

void app_launch(int idx, const char *arg)
{
    if (idx >= 0 && idx < os_app_count) os_apps[idx].launch(arg);
}

void app_launch_name(const char *name, const char *arg)
{
    for (int i = 0; i < os_app_count; i++) {
        if (!strcmp(os_apps[i].name, name)) { os_apps[i].launch(arg); return; }
    }
}
