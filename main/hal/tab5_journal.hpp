// O diario of gravacoes adiadas in flash interna - ver hal/journal_core.hpp
// para o formato e as regras, e hal/tab5_sd.hpp para QUANDO ele e usado (o
// cartao lent on computador). Quem fala com isto e so o tab5_sd.
//
// Mora in particao "reserva" (partitions.csv): 2 MB depois of app. Fica
// vazio quase sempre - so tem registros enquanto o cartao esta fora, e e
// apagado assim que eles vao para o cartao. O desgaste e desprezivel: alguns
// KB por uso of pendrive, contra ~100 mil apagamentos por setor.
//
// Seguro of qualquer tarefa (uma trava por dentro).
#pragma once

#include "journal_core.hpp"

// Acha a particao e varre o que became of antes (one reinicio no storage of
// pendrive). Chamar uma vez no boot, antes of qualquer gravacao.
bool tab5_journal_init(void);

bool tab5_journal_write(const journal_entry_t* reg);

// Ha registros esperando para ir on cartao.
bool tab5_journal_pending(void);

// Aplica os pending em order (ver journal_apply). Devolve quantos.
int  tab5_journal_apply(journal_apply_fn fn, void* ctx);

// Ocupacao, para a screen: bytes usados, size, registros pending.
void tab5_journal_usage(uint32_t* usado, uint32_t* total, uint32_t* pending);
