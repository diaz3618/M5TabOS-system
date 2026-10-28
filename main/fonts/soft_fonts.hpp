// As tabelas geradas por generate_fonts.py (soft_fonts.cpp). Quem usa e
// tab5_ui.cpp - o resto do sistema so ve tab5_texto()/tab5_fonte_corpo().
#pragma once
#include <stdint.h>

typedef struct {
  int            altura;   // altura da linha em px (o d.fontHeight() dela)
  const uint8_t* dados;    // o arquivo VLW inteiro, na flash
  uint32_t       tamanho;
} fonte_suave_t;

extern const fonte_suave_t FONTES_CORPO[];
extern const int           FONTES_CORPO_N;
extern const fonte_suave_t FONTES_NEGRITO[];
extern const int           FONTES_NEGRITO_N;
