// Console serial - um punhado de comandos digitados pelo monitor.
//
// Nasceu de uma necessidade concreta: trocar a rede WiFi exigia desligar o
// aparelho, tirar o cartao, levar no PC, editar, e remontar tudo. Para uma
// linha de texto. Agora e um comando.
//
// Funciona dentro do proprio `idf.py monitor` - ele encaminha o que voce digita
// para a serial. Digite "help" para ver os comandos.
//
// Nao substitui o cartao no PC para arquivo grande; e para ajuste pontual.

#pragma once

// Cria a tarefa do console. Chamar depois de tab5_sd_init().
void console_iniciar(void);
