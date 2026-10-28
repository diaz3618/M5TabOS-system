# Firmware for the Tab5's COPROCESSOR (ESP32-C6).
#
# A SEPARATE project from M5tabOS on purpose: different target (esp32c6, not
# esp32p4), different toolchain, different binary. The root `idf.py build`
# does not touch this, and this does not touch that.
#
#   cd c6 && source ../environment.sh && idf.py set-target esp32c6 && idf.py build
cmake_minimum_required(VERSION 3.16)
# APPLICATION version (esp_hosted has its own, 3.0.7). Goes up together with
# C6_PROTO_VERSAO in shared/tab5_c6.h.
set(PROJECT_VER "m5tabos-c6-4")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_c6)
