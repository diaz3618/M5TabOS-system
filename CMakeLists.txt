cmake_minimum_required(VERSION 3.16.0)

# A VERSAO DO SISTEMA mora em version.txt (uma linha, SemVer) e so muda pelo
# tools/release.sh - as regras estao em VERSIONING.md.
#
# O numero que vai para o binario (esp_app_get_description()->version, o que
# aparece em Ajustes, no console e no log de boot) diz tambem DE ONDE o build
# saiu:
#   0.8.0              - exatamente o commit da tag v0.8.0: a versao lancada
#   0.8.0+12.a1b2c3d   - 12 commits depois dela, no commit a1b2c3d
# Sem git (copia solta do codigo), fica so o numero do version.txt.
#
# LIDO NA CONFIGURACAO, nao a cada build: um commit novo refaz a configuracao
# (CMAKE_CONFIGURE_DEPENDS no logs/HEAD), mas mudanca ainda nao commitada nao
# aparece no numero - o "+N.hash" e "construido a partir de", nao "identico a".
file(STRINGS "${CMAKE_SOURCE_DIR}/version.txt" M5TABOS_VER LIMIT_COUNT 1)
string(STRIP "${M5TABOS_VER}" M5TABOS_VER)
if(NOT M5TABOS_VER MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "version.txt deve ter uma versao X.Y.Z, tem '${M5TABOS_VER}'")
endif()
set(PROJECT_VER "${M5TABOS_VER}")

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --long --match "v[0-9]*"
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE M5TABOS_DESC OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE M5TABOS_GIT_RES)
  if(M5TABOS_GIT_RES EQUAL 0 AND M5TABOS_DESC MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)-([0-9]+)-g([0-9a-f]+)$")
    set(M5TABOS_TAG "${CMAKE_MATCH_1}")
    set(M5TABOS_N   "${CMAKE_MATCH_2}")
    set(M5TABOS_SHA "${CMAKE_MATCH_3}")

    # A VERSAO SO ANDA PARA FRENTE. version.txt abaixo da ultima tag e um
    # erro de mao (merge, revert, copia velha): pararia o build antes de um
    # binario "0.6" sair depois do 0.7 lancado.
    if(M5TABOS_VER VERSION_LESS M5TABOS_TAG)
      message(FATAL_ERROR "version.txt (${M5TABOS_VER}) esta ABAIXO da ultima versao lancada (v${M5TABOS_TAG}).")
    endif()
    if(NOT M5TABOS_N EQUAL 0)
      set(PROJECT_VER "${M5TABOS_VER}+${M5TABOS_N}.${M5TABOS_SHA}")
    endif()
  endif()

  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --absolute-git-dir
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE M5TABOS_GITDIR OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(EXISTS "${M5TABOS_GITDIR}/logs/HEAD")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${M5TABOS_GITDIR}/logs/HEAD")
  endif()
endif()
message(STATUS "M5tabOS ${PROJECT_VER}")

# REVISAO DO CHIP (ver sdkconfig.defaults.chip_v1 / _v3). O P4 v1.x e o v3.x
# pedem binarios diferentes. Cada revisao compila na SUA pasta de build, com o
# sdkconfig DENTRO dela - um sdkconfig gerado para uma revisao nao pode ser
# reaproveitado pela outra (as chaves de revisao so valem na primeira vez):
#     idf.py build                            -> build/,    P4 v1.x (padrao)
#     idf.py -B build_v3 -DM5_CHIP=v3 build   -> build_v3/, P4 v3.x
set(M5_CHIP "v1" CACHE STRING "Revisao do ESP32-P4: v1 (Tab5 ate aqui) ou v3")
if(NOT M5_CHIP MATCHES "^v[13]$")
  message(FATAL_ERROR "M5_CHIP deve ser v1 ou v3, e '${M5_CHIP}'")
endif()
set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults;${CMAKE_SOURCE_DIR}/sdkconfig.defaults.chip_${M5_CHIP}")
if(NOT M5_CHIP STREQUAL "v1")
  set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()
message(STATUS "M5tabOS para ESP32-P4 ${M5_CHIP}.x")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(M5tabOS)
