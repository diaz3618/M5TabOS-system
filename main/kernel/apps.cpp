// THE APPLICATION TABLE.
//
// Nobody edits this file. The list comes from apps_lista.inc, which
// main/CMakeLists.txt generates each build from the main/app_<name>/ folders:
// each becomes a line APP(APP_<NAME>). Create one app and create the folder and
// set 'extern const app_t APP_<NAME>' in it; remove and erase the folder.

#include "app.h"

#define APP(simbolo) extern const app_t simbolo;
#include "apps_lista.inc"
#undef APP

const app_t* const SISTEMA_APPS[] = {
#define APP(simbolo) &simbolo,
#include "apps_lista.inc"
#undef APP
};

const int SISTEMA_NUM_APPS = sizeof(SISTEMA_APPS) / sizeof(SISTEMA_APPS[0]);
