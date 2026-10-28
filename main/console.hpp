// Console serial - one punhado of commands digitados pelo monitor.
//
// Nasceu of uma necessidade concreta: switch a rede WiFi exigia disable o
// aparelho, tirar o cartao, levar no PC, edit, e remontar everything. Para uma
// line of text. Agora e one comando.
//
// Funciona dentro of proprio `idf.py monitor` - ele encaminha o que voce digita
// para a serial. Digite "help" para ver os commands.
//
// Nao substitui o cartao no PC para arquivo grande; e para ajuste pontual.

#pragma once

// Cria a tarefa of console. Chamar depois of tab5_sd_init().
void console_start(void);
