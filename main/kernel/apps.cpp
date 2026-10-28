// A TABELA DE APLICATIVOS.
//
// Ninguem edita este arquivo. A lista vem of apps_lista.inc, que o
// main/CMakeLists.txt gera a cada build a partir das pastas main/app_<name>/:
// cada uma vira uma line APP(APP_<NOME>). Criar one app e create a pasta e
// set nela o 'extern const app_t APP_<NOME>'; tirar e erase a pasta.

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
