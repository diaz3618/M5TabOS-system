cmake_minimum_required(VERSION 3.16.0)

# THE SYSTEM VERSION lives in version.txt (one line, SemVer) and only
# changes through tools/release.sh - the rules are in VERSIONING.md.
#
# The number that goes into the binary (esp_app_get_description()->version,
# the one shown in Settings, the console, and the boot log) also says WHERE
# the build came from:
#   0.8.0              - exactly the v0.8.0 tag's commit: the released version
#   0.8.0+12.a1b2c3d   - 12 commits after it, at commit a1b2c3d
# Without git (a loose copy of the code), it's just version.txt's number.
#
# READ AT CONFIGURE TIME, not on every build: a new commit redoes the
# configuration (CMAKE_CONFIGURE_DEPENDS on logs/HEAD), but an uncommitted
# change does not show up in the number - "+N.hash" means "built from",
# not "identical to".
file(STRINGS "${CMAKE_SOURCE_DIR}/version.txt" M5TABOS_VER LIMIT_COUNT 1)
string(STRIP "${M5TABOS_VER}" M5TABOS_VER)
if(NOT M5TABOS_VER MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "version.txt must contain a X.Y.Z version, has '${M5TABOS_VER}'")
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

    # THE VERSION ONLY MOVES FORWARD. version.txt below the last tag is a
    # mistake (a merge, a revert, a stale copy): this stops the build before
    # a "0.6" binary ships after the released 0.7.
    if(M5TABOS_VER VERSION_LESS M5TABOS_TAG)
      message(FATAL_ERROR "version.txt (${M5TABOS_VER}) is BELOW the last released version (v${M5TABOS_TAG}).")
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

# CHIP REVISION (see sdkconfig.defaults.chip_v1 / _v3). P4 v1.x and v3.x
# need different binaries. Each revision builds into ITS OWN build folder,
# with its sdkconfig INSIDE it - an sdkconfig generated for one revision
# cannot be reused by the other (the revision keys only take effect the
# first time):
#     idf.py build                            -> build/,    P4 v1.x (default)
#     idf.py -B build_v3 -DM5_CHIP=v3 build   -> build_v3/, P4 v3.x
set(M5_CHIP "v1" CACHE STRING "ESP32-P4 revision: v1 (Tab5 so far) or v3")
if(NOT M5_CHIP MATCHES "^v[13]$")
  message(FATAL_ERROR "M5_CHIP must be v1 or v3, is '${M5_CHIP}'")
endif()
set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults;${CMAKE_SOURCE_DIR}/sdkconfig.defaults.chip_${M5_CHIP}")
if(NOT M5_CHIP STREQUAL "v1")
  set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()
message(STATUS "M5tabOS for ESP32-P4 ${M5_CHIP}.x")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(M5tabOS)
