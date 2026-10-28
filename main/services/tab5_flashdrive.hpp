// MODO PENDRIVE: o cartao SD do Tab5 aparece no computador como um disco USB.
//
// -------------------------------------------------------------------------
// COMO, NO HARDWARE
// -------------------------------------------------------------------------
// O USB-C do Tab5 vai ao PHY full speed 0 do P4, que normalmente e do
// USB-Serial-JTAG (o console, a gravacao). O P4 tem um segundo controlador,
// o OTG full speed (o "USB_WRAP"), e um seletor que troca qual dos dois
// controladores fala com qual PHY (LP_SYS.usb_ctrl.sw_usb_phy_sel). Ligar o
// pendrive = o JTAG solta o PHY 0, o OTG pega, e o TinyUSB se anuncia ao
// computador como armazenamento em massa. Desligar desfaz, e o console volta.
// E a mesma receita do M5Launcher (src/massStorage.cpp), que ja roda no Tab5.
//
// DURANTE O PENDRIVE NAO HA CONSOLE nem gravacao pelo cabo: o computador ve
// um disco, nao a porta serial. O log continua sendo gravado no cartao assim
// que ele voltar (tab5_sd_anexar segura o que chegar).
//
// -------------------------------------------------------------------------
// O CARTAO
// -------------------------------------------------------------------------
// Nunca dois donos ao mesmo tempo - ver tab5_sd_emprestar(). A sessao abaixo
// empresta o cartao ANTES de ligar o USB e so devolve DEPOIS de desliga-lo.
//
// O computador recebe SETORES, nao arquivos: le e grava direto no cartao,
// sem cache nenhum do lado do Tab5 (cada escrita ja esta no cartao quando o
// computador recebe o "ok"). O que pode ficar no meio do caminho e o cache DO
// COMPUTADOR - por isso o certo e ejetar la antes de puxar o cabo. Ajustes >
// USB diz isso e so devolve o cartao sem ejecao se a pessoa insistir.
#pragma once

#include <stdint.h>
#include "sd_protocol_types.h"

// ---------------------------------------------------------------------------
// A SESSAO (em segundo plano - o Tab5 segue usavel durante o pendrive)
// ---------------------------------------------------------------------------
// Ligar: o cartao e emprestado (tab5_sd_emprestar: o pendente e gravado, a
// foto e tirada, as portas de gravacao passam a ir para o diario na flash) e
// o USB sobe. Na barra de status fica o aviso "Cartao no computador" enquanto
// durar; tocar nele abre Ajustes > USB.
//
// Termina de dois jeitos: o computador EJETA (o cartao volta sozinho - nada
// mais em cache do lado de la) ou a pessoa manda devolver em Ajustes > USB (a
// tela confirma antes, se o computador ainda estiver com o disco montado). O
// USB sai primeiro, o cartao volta, o diario e aplicado e o resultado fica na
// tela e, se houve conflito, na barra de status.
//
// Regras do sistema, nao de um app:
//   - nao comeca com um app que EXIGE o cartao aberto (app_t.exige_sd), e esse
//     app nao abre enquanto durar (o kernel mostra o motivo);
//   - com o disco montado no computador a tela nao entra em repouso
//     (SISTEMA_ACORDADO_PENDRIVE);
//   - durante o pendrive o cabo nao e console nem gravacao, e a CPU fica no
//     clock maximo enquanto o computador estiver falando;
//   - barramento parado (cabo puxado sem ejetar, computador dormindo) por
//     mais de 2 s: a sessao continua, mas a tela pode apagar e o clock sai -
//     voltam sozinhos com o computador. Ligado sem computador nenhum, o
//     clock sai depois de 10 s.
typedef enum {
  PENDRIVE_PARADO = 0,
  PENDRIVE_PREPARANDO,   // emprestando o cartao, subindo o USB
  PENDRIVE_ATIVO,        // o cartao esta com o computador
  PENDRIVE_DEVOLVENDO,   // USB saindo, cartao voltando, diario sendo aplicado
} tab5_pendrive_fase_t;

// Por que a ultima tentativa nao ligou (PENDRIVE_OK = ligou ou nunca tentou).
typedef enum {
  PENDRIVE_OK = 0,
  PENDRIVE_RECUSA_APP,         // um app que exige o cartao esta aberto
  PENDRIVE_RECUSA_EM_USO,      // um servico segurou o cartao alem do prazo
  PENDRIVE_RECUSA_SEM_CARTAO,  // sem cartao, ou ele nao respondeu
  PENDRIVE_RECUSA_SEM_DIARIO,  // falta a particao "reserva" (grave a tabela nova)
  PENDRIVE_RECUSA_USB,         // o USB nao subiu (o cartao ja voltou)
} tab5_pendrive_motivo_t;

// Pede para ligar. Nao bloqueia (a sessao tem tarefa propria). false = ja
// havia uma sessao, ou recusado na hora (o motivo fica no estado).
bool tab5_pendrive_iniciar(void);

// Pede para devolver o cartao ao Tab5. A CONFIRMACAO e de quem chama (a tela):
// se o computador ainda estiver com o disco montado e nao ejetou, perguntar
// antes. Nao bloqueia.
void tab5_pendrive_devolver(void);

// Onde o pendrive esta esperando agora (para dizer onde parou, quando para).
typedef enum {
  PENDRIVE_ESPERA_NADA = 0,     // o computador ainda nao fez nada
  PENDRIVE_ESPERA_COMPUTADOR,   // tudo respondido: e a vez do computador pedir
  PENDRIVE_ESPERA_USB,          // uma leitura comecou e os dados/status ainda nao sairam
  PENDRIVE_ESPERA_CARTAO,       // dentro de uma leitura/escrita do cartao
} tab5_pendrive_espera_t;

// O USB por dentro, para a tela.
typedef struct {
  bool     ligado;        // o USB esta como pendrive
  bool     montado;       // o computador enumerou o disco
  bool     suspenso;      // o barramento parou: cabo puxado ou computador dormindo
  bool     ejetado;       // o computador ejetou (START STOP UNIT com "eject")
  bool     ativo;         // houve leitura/escrita nos ultimos 2 s
  uint64_t lidos;         // bytes lidos pelo computador desde ligar
  uint64_t gravados;      // bytes gravados pelo computador desde ligar
  uint32_t erros;         // leituras/escritas que o cartao recusou
  uint32_t ms_sem_escrita;// desde a ultima escrita (UINT32_MAX = nenhuma)
  // DIAGNOSTICO - ate onde o computador chegou, na ordem em que ele faz:
  uint32_t n_config;      // reconheceu o aparelho (configurou o USB)
  uint32_t n_inquiry;     // perguntou o que ele e
  uint32_t n_capacidade;  // perguntou o tamanho
  uint32_t n_leituras;    // leu (a tabela FAT vem primeiro)
  uint8_t  op_recusado;   // ultimo comando SCSI recusado (0 = nenhum)
  uint32_t leitura_max_ms;// leitura do cartao mais lenta
  uint32_t n_tur;         // perguntou se o disco esta pronto (TEST UNIT READY)
  uint32_t n_sense;       // perguntou qual foi o erro (REQUEST SENSE)
  uint32_t n_respostas;   // comandos com a resposta entregue (fora leitura/escrita)
  uint8_t  op_respondido; // o ultimo deles
  uint32_t n_soltou;      // desconfigurou: reset do barramento ou cabo
  uint32_t n_leituras_ok; // comandos de leitura com dados e status entregues
  uint32_t n_cmd_leitura; // comandos de leitura recebidos
  uint32_t ultimo_setor;  // ultimo pedaco pedido ao cartao
  uint32_t parado_ms;     // desde o ultimo sinal de vida (computador ou cartao)
  tab5_pendrive_espera_t onde;
} tab5_pendrive_usb_t;
void tab5_pendrive_usb(tab5_pendrive_usb_t* u);

typedef struct {
  tab5_pendrive_fase_t   fase;
  tab5_pendrive_motivo_t motivo;
  bool                   por_ejecao;   // a ultima sessao terminou pela ejecao no computador
  bool                   sem_ejetar;   // ...ou com o disco montado, sem ejecao: o computador
                                       // vai verificar o cartao na proxima vez (demora)
  bool                   oferta;       // computador ligado no cabo, esperando a pessoa decidir
  tab5_pendrive_usb_t    usb;
} tab5_pendrive_estado_t;
void tab5_pendrive_estado(tab5_pendrive_estado_t* e);

// Algo para a pessoa ver em Ajustes > USB (sessao em andamento, computador
// oferecido, conflito da ultima volta) - Ajustes abre direto nessa pagina.
bool tab5_pendrive_pede_atencao(void);
// A pessoa viu o resultado: o aviso da barra sai.
void tab5_pendrive_resultado_visto(void);

// Um computador esta do outro lado do cabo AGORA? Com o pendrive desligado
// quem ve e o USB-Serial-JTAG (os quadros SOF que o computador manda a cada
// 1 ms); carregador de parede nao manda SOF - nao conta.
bool tab5_pendrive_pc_presente(void);

// Boot, depois do sistema: registra o aviso e liga a vigia do cabo (que liga
// o pendrive sozinho quando o ajuste pede - ajustes_pendrive()).
void tab5_pendrive_servico_iniciar(void);
