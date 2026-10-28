// MODO PENDRIVE: o cartao SD of Tab5 aparece no computador como one disco USB.
//
// -------------------------------------------------------------------------
// COMO, NO HARDWARE
// -------------------------------------------------------------------------
// O USB-C of Tab5 vai on PHY full speed 0 of P4, que normalmente e of
// USB-Serial-JTAG (o console, a gravacao). O P4 tem one segundo controlador,
// o OTG full speed (o "USB_WRAP"), e one seletor que troca qual dos dois
// controladores fala com qual PHY (LP_SYS.usb_ctrl.sw_usb_phy_sel). Ligar o
// pendrive = o JTAG released o PHY 0, o OTG pega, e o TinyUSB se anuncia on
// computador como armazenamento em massa. Desligar desfaz, e o console iteration.
// E a mesma receita of M5Launcher (src/massStorage.cpp), que ja roda no Tab5.
//
// DURANTE O PENDRIVE NAO HA CONSOLE nem gravacao pelo cabo: o computador ve
// one disco, nao a porta serial. O log continua sendo gravado no cartao assim
// que ele voltar (tab5_sd_enqueue_append segura o que chegar).
//
// -------------------------------------------------------------------------
// O CARTAO
// -------------------------------------------------------------------------
// Nunca dois donos on mesmo time - ver tab5_sd_emprestar(). A sessao abaixo
// empresta o cartao ANTES of enable o USB e so devolve DEPOIS of desliga-lo.
//
// O computador recebe SETORES, nao arquivos: le e grava direto no cartao,
// sem cache nenhum of lado of Tab5 (cada escrita ja esta no cartao when o
// computador recebe o "ok"). O que pode ficar no middle of caminho e o cache DO
// COMPUTADOR - por isso o certo e ejetar la antes of puxar o cabo. Ajustes >
// USB diz isso e so devolve o cartao sem ejecao se a pessoa insistir.
#pragma once

#include <stdint.h>
#include "sd_protocol_types.h"

// ---------------------------------------------------------------------------
// A SESSAO (em segundo plano - o Tab5 segue usavel durante o pendrive)
// ---------------------------------------------------------------------------
// Ligar: o cartao e emprestado (tab5_sd_emprestar: o pending e gravado, a
// foto e tirada, as portas of gravacao passam a ir para o diario in flash) e
// o USB sobe. Na barra of status stays o aviso "Cartao no computador" enquanto
// durar; touch nele abre Ajustes > USB.
//
// Termina of dois jeitos: o computador EJETA (o cartao iteration sozinho - nada
// mais em cache of lado of la) ou a pessoa manda return em Ajustes > USB (a
// screen confirma antes, se o computador ainda estiver com o disco montado). O
// USB exit first, o cartao iteration, o diario e aplicado e o resultado stays in
// screen e, se houve conflito, in barra of status.
//
// Regras of sistema, nao of one app:
//   - nao comeca com one app que EXIGE o cartao open (app_t.exige_sd), e esse
//     app nao abre enquanto durar (o kernel mostra o reason);
//   - com o disco montado no computador a screen nao entra em sleep
//     (SISTEMA_ACORDADO_PENDRIVE);
//   - durante o pendrive o cabo nao e console nem gravacao, e a CPU stays no
//     clock maximum enquanto o computador estiver falando;
//   - barramento parado (cabo puxado sem ejetar, computador dormindo) por
//     mais of 2 s: a sessao continua, mas a screen pode erase e o clock exit -
//     voltam sozinhos com o computador. Ligado sem computador nenhum, o
//     clock exit depois of 10 s.
typedef enum {
  PENDRIVE_PARADO = 0,
  PENDRIVE_PREPARANDO,   // emprestando o cartao, subindo o USB
  PENDRIVE_ATIVO,        // o cartao esta com o computador
  PENDRIVE_DEVOLVENDO,   // USB saindo, cartao voltando, diario sendo aplicado
} tab5_pendrive_fase_t;

// Por que a ultima tentativa nao ligou (PENDRIVE_OK = ligou ou nunca tentou).
typedef enum {
  PENDRIVE_OK = 0,
  PENDRIVE_RECUSA_APP,         // one app que requires o cartao esta open
  PENDRIVE_RECUSA_EM_USO,      // one servico segurou o cartao alem of prazo
  PENDRIVE_RECUSA_SEM_CARTAO,  // sem cartao, ou ele nao respondeu
  PENDRIVE_RECUSA_SEM_DIARIO,  // falta a particao "reserva" (grave a tabela nova)
  PENDRIVE_RECUSA_USB,         // o USB nao subiu (o cartao ja voltou)
} tab5_pendrive_reason_t;

// Pede para enable. Nao bloqueia (a sessao tem tarefa propria). false = ja
// havia uma sessao, ou recusado in hora (o reason stays no estado).
bool tab5_pendrive_start(void);

// Pede para return o cartao on Tab5. A CONFIRMACAO e of quem chama (a screen):
// se o computador ainda estiver com o disco montado e nao ejetou, perguntar
// antes. Nao bloqueia.
void tab5_pendrive_return(void);

// Onde o pendrive esta esperando now (para dizer onde parou, when para).
typedef enum {
  PENDRIVE_ESPERA_NADA = 0,     // o computador ainda nao fez nada
  PENDRIVE_ESPERA_COMPUTADOR,   // everything respondido: e a vez of computador request
  PENDRIVE_ESPERA_USB,          // uma leitura comecou e os data/status ainda nao sairam
  PENDRIVE_ESPERA_CARTAO,       // dentro of uma leitura/escrita of cartao
} tab5_pendrive_espera_t;

// O USB por dentro, para a screen.
typedef struct {
  bool     enabled;        // o USB esta como pendrive
  bool     montado;       // o computador enumerou o disco
  bool     suspenso;      // o barramento parou: cabo puxado ou computador dormindo
  bool     ejetado;       // o computador ejetou (START STOP UNIT com "eject")
  bool     active;         // houve leitura/escrita nos ultimos 2 s
  uint64_t lidos;         // bytes lidos pelo computador since enable
  uint64_t gravados;      // bytes gravados pelo computador since enable
  uint32_t erros;         // leituras/escritas que o cartao recusou
  uint32_t ms_sem_escrita;// since a ultima escrita (UINT32_MAX = none)
  // DIAGNOSTICO - ate onde o computador chegou, in order em que ele faz:
  uint32_t n_config;      // reconheceu o aparelho (configurou o USB)
  uint32_t n_inquiry;     // perguntou o que ele e
  uint32_t n_capacidade;  // perguntou o tamanho
  uint32_t n_leituras;    // leu (a tabela FAT vem first)
  uint8_t  op_recusado;   // ultimo comando SCSI recusado (0 = nenhum)
  uint32_t leitura_max_ms;// leitura of cartao mais lenta
  uint32_t n_tur;         // perguntou se o disco esta pronto (TEST UNIT READY)
  uint32_t n_sense;       // perguntou qual foi o erro (REQUEST SENSE)
  uint32_t n_respostas;   // commands com a resposta entregue (fora leitura/escrita)
  uint8_t  op_respondido; // o ultimo deles
  uint32_t n_soltou;      // desconfigurou: reset of barramento ou cabo
  uint32_t n_leituras_ok; // commands of leitura com data e status entregues
  uint32_t n_cmd_leitura; // commands of leitura recebidos
  uint32_t ultimo_setor;  // ultimo pedaco request on cartao
  uint32_t parado_ms;     // since o ultimo sinal of vida (computador ou cartao)
  tab5_pendrive_espera_t onde;
} tab5_pendrive_usb_t;
void tab5_pendrive_usb(tab5_pendrive_usb_t* u);

typedef struct {
  tab5_pendrive_fase_t   fase;
  tab5_pendrive_reason_t reason;
  bool                   por_ejecao;   // a ultima sessao terminou pela ejecao no computador
  bool                   sem_ejetar;   // ...ou com o disco montado, sem ejecao: o computador
                                       // vai verificar o cartao in next vez (demora)
  bool                   oferta;       // computador enabled no cabo, esperando a pessoa decidir
  tab5_pendrive_usb_t    usb;
} tab5_pendrive_estado_t;
void tab5_pendrive_estado(tab5_pendrive_estado_t* e);

// Algo para a pessoa ver em Ajustes > USB (sessao em andamento, computador
// oferecido, conflito of ultima iteration) - Ajustes abre direto nessa pagina.
bool tab5_pendrive_pede_atencao(void);
// A pessoa viu o resultado: o aviso of barra exit.
void tab5_pendrive_resultado_visto(void);

// Um computador esta of outro lado of cabo AGORA? Com o pendrive disabled
// quem ve e o USB-Serial-JTAG (os quadros SOF que o computador manda a cada
// 1 ms); carregador of parede nao manda SOF - nao conta.
bool tab5_pendrive_pc_presente(void);

// Boot, depois of sistema: registra o aviso e liga a vigia of cabo (que liga
// o pendrive sozinho when o ajuste pede - settings_flash_drive()).
void tab5_pendrive_servico_start(void);
