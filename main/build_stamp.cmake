# Escreve o carimbo de compilacao (main/CMakeLists.txt roda isto a cada build).
# Precisao de MINUTO, e so reescreve quando o texto muda: dois builds no mesmo
# minuto nao recompilam nada, e o "Compilado em" de Ajustes > Sobre nunca fica
# para tras.
string(TIMESTAMP M5_QUANDO "%d/%m/%Y %H:%M")
set(M5_TXT "// Gerado por main/carimbo.cmake a cada build - nao editar.\nconst char* const m5tabos_compilado = \"${M5_QUANDO}\";\n")
if(EXISTS "${OUT}")
  file(READ "${OUT}" M5_ANTES)
endif()
if(NOT "${M5_ANTES}" STREQUAL "${M5_TXT}")
  file(WRITE "${OUT}" "${M5_TXT}")
endif()
