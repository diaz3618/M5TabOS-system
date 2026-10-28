# Firmware do COPROCESSADOR (ESP32-C6) do Tab5.
#
# Projeto SEPARADO do M5tabOS de proposito: alvo diferente (esp32c6, nao
# esp32p4), toolchain diferente, binario diferente. O `idf.py build` da raiz
# nao toca aqui, e este nao toca la.
#
#   cd c6 && source ../environment.sh && idf.py set-target esp32c6 && idf.py build
cmake_minimum_required(VERSION 3.16)
# Versao do APLICATIVO (o esp_hosted tem a dele, 3.0.7). Sobe junto com
# C6_PROTO_VERSAO de shared/tab5_c6.h.
set(PROJECT_VER "m5tabos-c6-4")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_c6)
