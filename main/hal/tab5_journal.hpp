// O diario de gravacoes adiadas na flash interna - ver hal/journal_core.hpp
// para o formato e as regras, e hal/tab5_sd.hpp para QUANDO ele e usado (o
// cartao lent ao computador). Quem fala com isto e so o tab5_sd.
//
// Mora na particao "reserva" (partitions.csv): 2 MB depois do app. Fica
// vazio quase sempre - so tem registros enquanto o cartao esta fora, e e
// apagado assim que eles vao para o cartao. O desgaste e desprezivel: alguns
// KB por uso do pendrive, contra ~100 mil apagamentos por setor.
//
// Seguro de qualquer tarefa (uma trava por dentro).
#pragma once

#include "journal_core.hpp"

// Acha a particao e varre o que ficou de antes (um reinicio no storage do
// pendrive). Chamar uma vez no boot, antes de qualquer gravacao.
bool tab5_journal_init(void);

bool tab5_journal_write(const journal_entry_t* reg);

// Ha registros esperando para ir ao cartao.
bool tab5_journal_pending(void);

// Aplica os pending em ordem (ver journal_apply). Devolve quantos.
int  tab5_journal_apply(journal_apply_fn fn, void* ctx);

// Ocupacao, para a tela: bytes usados, size, registros pending.
void tab5_journal_usage(uint32_t* usado, uint32_t* total, uint32_t* pending);
