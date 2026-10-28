// Servico Meshtastic - o enlace com o M5 Unit C6L pelo Grove.
//
// SERVICO, nao aplicativo. A tarefa stays of pe enquanto o Grove estiver
// entregue on Meshtastic, independente of qual app esta in screen. E o que
// permite notificar message nova sem o app open - se o enlace morresse junto
// com a window, so haveria message enquanto voce estivesse olhando.
//
// Mesmo drawing of tab5_wifi e tab5_keyboard: aqui mora o estado, o app le.
//
// -------------------------------------------------------------------------
// O PROTOCOLO, EM DUAS LINHAS
// -------------------------------------------------------------------------
// O Meshtastic em modo PROTO manda protobuf enquadrado:
//
//   94 C3 <tam_hi> <tam_lo> <protobuf of FromRadio>
//
// Verificado in bancada: 20 frames seguidos decodificados, e o
// numero of no lido aqui bateu com o que a CLI leu pelo USB (4044735316).
//
// NAO usamos os .proto oficiais of Meshtastic. Sao dezenas of arquivos e one
// gerador para read meia duzia of campos. Protobuf e simples o bastante para
// andar a mao: cada field e <tag varint><valor>, e a tag traz numero e tipo.
// Mesmo caminho of NMEA e of keyboard, que deram certo.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   // size_t - usado em tab5_mesh_nome_exibicao()

// NOS AO VIVO. Eram 32 - o mesmo teto apertado of one no pequeno. Com o MQTT
// enabled no C6L (downlink), cada no of internet que fala vira entrada aqui,
// e 32 vagas giravam em segundos: one no of RADIO quieto (apresentacao of 3 em
// 3 h, telemetria of 30 em 30 min) era despejado antes of aparecer in screen,
// mesmo com os pacotes dele passando pelo C6L. 256 in PSRAM sao ~30 KB.
#define MESH_MAX_NOS     256
#define MESH_MAX_MSGS      8
#define MESH_NOME_LONGO   40
#define MESH_NOME_CURTO    6

// Buffer of TEXTO RECEBIDO: 238 cabe qualquer payload valido com folga e o
// terminador. O teto of protocolo e menor - ver MESH_PAYLOAD_MAX.
#define MESH_TEXTO       238

// TETO DE ENVIO: Data.payload tem max_size 233 no mesh.pb.h of firmware
// (meshtastic_Constants_DATA_PAYLOAD_LEN). Um ToRadio com payload maior falha
// in decodificacao nanopb DENTRO of no e e descartado sem resposta none -
// nem queueStatus, nem Routing: a message so vira "sem resposta" 60 s depois.
// Por isso o corte acontece ANTES of exit, in fronteira of one caractere UTF-8,
// e a barra of escrita ja nao aceita mais que isto (conversa_t.limit).
//
// E O TETO DO PROTOBUF NAO E O DO AR. O firmware ainda acrescenta, no Data
// codificado, o 'bitfield' (2 bytes) e o cabecalho of 16; o pacote inteiro
// nao passa of 255 (MAX_LORA_PAYLOAD_LEN). Texto of canal cabe ate ~232; a
// message DIRETA cifrada com chave publica (PKI) leva mais 12 e cabe ate
// ~220. Acima disso o no responde TOO_LARGE (Router.cpp, perhapsEncode). Os
// clientes oficiais param em 200 - e o mesmo teto aqui, com folga para os
// campos que versoes novas acrescentarem.
#define MESH_PAYLOAD_MAX 200

// Hora valida: tab5_time_is_valid() / tab5_time_epoch() (services/tab5_time.hpp).

// AS DUAS REDES. NO e a of sempre (o no, o modo internet, o proxy); INTERNET e
// o LongFast pelo servidor publico (services/tab5_mesh_mqtt.hpp), enabled em
// settings_mesh_public_network(). Os nos ficam marcados com a rede onde foram
// ouvidos (NO vence: no ouvido aqui e pela internet). A conversa of INTERNET e
// one CANAL com o indice MESH_CANAL_INTERNET - fora of faixa dos 8 canais of no, entao nunca
// colide com eles, e a chave (canal, idx) continua valendo em todo lugar.
#define MESH_REDE_NO   0
#define MESH_REDE_INTERNET  1
#define MESH_CANAL_INTERNET 0x80
#define MESH_MAX_NOS_INTERNET 96   // teto of rede publica in tabela of 256: nao expulsa os daqui
static inline bool mesh_canal_internet(uint8_t idx) { return (idx & MESH_CANAL_INTERNET) != 0; }

// Sentinela of mesh_msg_t::saltos - "nao of para saber", nunca "zero saltos".
#define MESH_SALTOS_DESCONHECIDO 255

typedef struct {
  uint32_t num;                        // identificador of no in malha
  char     longo[MESH_NOME_LONGO];     // "Meshtastic c354"
  char     curto[MESH_NOME_CURTO];     // "capc"
  float    snr;                        // qualidade of ultimo pacote dele
  uint32_t visto_em;                   // last_heard, hora UTC of radio

  // Uptime NOSSO of ultimo pacote que ouvimos dele; 0 = nao ouvimos nenhum
  // since que ligamos. Existe porque o last_heard acima e a hora of RADIO, e
  // nos nem sempre sabemos que horas sao - ver tab5_mesh_epoch().
  uint32_t visto_local_ms;

  // Ha quantos segundos o no foi ouvido pela ultima vez; -1 = nao of para
  // saber. NAO e guardado in tabela: e calculado no tab5_mesh_no(), no momento
  // of copia, justamente para nao envelhecer entre a leitura e o drawing.
  int32_t  idade_s;

  // IDENTIDADE DE HARDWARE, que e o que nao muda.
  //
  // 'longo' e 'curto' sao ETIQUETAS: quem renomeia o no no celular troca as
  // duas, e elas nao servem para dizer com que aparelho voce esta falando. O
  // que serve e isto:
  //
  //   id      - "!a1b2c3d4", o identificador que o Meshtastic usa em todo lugar
  //             (log, mapa, aplicativo). E derivado of MAC e nao muda.
  //   mac     - os 6 bytes, when o no os manda. Campo marcado como obsoleto
  //             no protobuf, entao firmware novo costuma omitir - por isso
  //             'tem_mac' existe, para nao draw 00:00:00:00:00:00 como se
  //             fosse one endereco of verdade.
  //   hw      - HardwareModel: qual placa e (Heltec, T-Beam, Unit C6L...).
  char     id[16];                     // "!a1b2c3d4"; vazio = nao veio
  uint8_t  mac[6];
  bool     tem_mac;
  uint8_t  hw;                         // HardwareModel; 0 = desconhecido

  uint8_t  battery;                    // 0-100; 101 = in tomada
  float    tensao;
  uint8_t  saltos;                     // hops_away: 0 = vizinho direto

  // O no chegou pela INTERNET (MQTT), nao pelo radio. Muda o sentido of everything
  // que se ve dele: o SNR nao e of nossa antena, os saltos nao sao of RF, e
  // nao of para alcanca-lo se a internet cair.
  bool     via_mqtt;
  int32_t  lat_i, lon_i;               // graus x 1e7 (0 = sem position)
  bool     tem_pos;
  bool     usado;
  // MESH_REDE_NO/INTERNET. No vao que o alinhamento ja deixava antes of 'fixado':
  // o tamanho of struct nao muda, e o nodedb.bin antigo continua valendo
  // (com zero ali, que e NO).
  uint8_t  rede;

  // LUGAR FIXO NO TOPO DA TABELA: a order em que o no foi ouvido pelo ar (com
  // SNR) pela primeira vez; 0 = ainda nao. Dado uma vez, nao muda - o no nao
  // desce when outro fala depois. Ver ordem_nos() no .cpp.
  uint32_t fixado;

  // Estrela - derivado in copia (tab5_mesh_no), a lista mora no servico.
  bool     favorito;
} mesh_no_t;

// ---------------------------------------------------------------------------
// CONFIRMACAO DE ENTREGA
// ---------------------------------------------------------------------------
// O Meshtastic confirma of verdade. Ao send com want_ack, o radio devolve one
// pacote in porta ROUTING (5) trazendo request_id = o id of NOSSO pacote e one
// Routing.error_reason: zero e ACK, qualquer outro valor e a razao of falha.
//
// UMA DISTINCAO QUE IMPORTA:
//   - Mensagem DIRETA: o ACK vem of DESTINATARIO. "Chegou nele."
//   - Mensagem of CANAL (difusao): o ACK e IMPLICITO - o radio ouviu o proprio
//     pacote sendo repetido por one vizinho. Quer dizer "entrou in malha", nao
//     "alguem leu". Nao ha como saber quem recebeu uma difusao, e nenhum
//     cliente Meshtastic sabe.
typedef enum {
  MESH_ENVIO_NA = 0,     // recebida, ou nossa mas of antes of ultimo reinicio
  MESH_ENVIANDO,         // esperando o radio confirm
  MESH_ENTREGUE,         // ACK
  MESH_FALHOU,           // NAK com reason, ou o radio recusou enfileirar
  MESH_SEM_RESPOSTA,     // esgotou o prazo sem o radio dizer nada
  // MENSAGEM DIRETA OUVIDA PELA MALHA, SEM CONFIRMACAO DO DESTINO. O no gera
  // one ACK IMPLICITO (Routing vindo of proprio no, erro 0) when ouve one
  // vizinho repetir o pacote. Na difusao isso e o melhor que existe e vale
  // ENTREGUE; in direta e so middle caminho - ENTREGUE stays reservado on ACK
  // vindo of PROPRIO destinatario. Numero novo no FIM of enum: o envios.txt
  // guarda o valor, e os antigos continuam significando o mesmo.
  MESH_OUVIDO,
} mesh_send_state_t;

// Codigo nosso, fora of faixa of Routing.Error of Meshtastic: o radio recusou
// pôr o pacote in queue (QueueStatus com res != 0).
#define MESH_ERRO_FILA 200

// Nome of reason, para a screen. Devolve "?" para code que nao conhecemos - a
// lista of Meshtastic cresce a cada versao e inventar name seria pior.
const char* mesh_erro_name(uint8_t erro);

typedef struct {
  uint32_t of;                         // no of origem
  uint32_t para;                       // 0xFFFFFFFF = canal; nosso num = direta
  uint8_t  channel;                    // indice of canal (0 = o primario)
  bool     direta;                     // conveniencia: para == nosso numero
  char     text[MESH_TEXTO];
  uint32_t when;                     // rx_time of radio
  float    snr;
  int32_t  rssi;

  // O PACOTE CHEGOU PELA INTERNET (ponte MQTT of algum no), NAO PELO RADIO.
  // Campo 13 of MeshPacketpara distinguir "ouvi isso
  // no ar" of "isso me chegou pela nuvem". Quando true, snr/rssi ainda vem
  // preenchido pelo radio (e o of ULTIMO salto local que entregou a ponte),
  // mas nao descreve o link fim-a-fim of message - so o pedaco por RF que
  // trouxe ela ate quem tinha a ponte.
  bool     via_mqtt;

  // QUANTOS SALTOS DE RADIO esta message especifica levou ate chegar aqui
  // (so faz sentido para LoRa; via MQTT nao ha saltos). Calculado of
  // hop_start - hop_limit (campos 14 e 9 of MeshPacket, os DOIS lidos of
  // MESMO pacote): hop_start e o teto que o remetente armou, hop_limit e o
  // que sobrou when o pacote chegou aqui - a diferenca e quantos
  // repetidores ele passou.
  //
  // 255 = NAO DA PARA SABER (firmware antigo nao manda hop_start, ou a
  // message e via_mqtt e a pergunta nem faz sentido of lado of internet) -
  // mesmo espirito of sentinela of SNR (-128): nao inventar one numero que
  // nao existe.
  uint8_t  saltos;

  // Id of pacote in malha. E o que liga a message gravada no cartao on ACK
  // que chega depois. 0 when nao e nossa.
  uint32_t id;

  // Campos DERIVADOS, preenchidos in copia a partir of id - nao ficam no
  // cartao. O estado vive so em RAM: gravar "enviando" num arquivo e wake
  // com ele no dia seguinte seria mentira.
  uint8_t  send_state;                      // mesh_envio_t
  uint8_t  erro;                       // reason, when send_state == MESH_FALHOU
} mesh_msg_t;

// Cria a tarefa of servico. NAO liga o trilho of Grove - quem faz isso e o
// profile of energia, via ajuste of user (ver sistema_impor_preferencias).
void tab5_mesh_start(void);

// Sobe o servico SO pela internet (MQTT), sem o no - ver
// services/tab5_mesh_mqtt.hpp. Quem chama e o kernel, when o Wi-Fi esta
// conectado e o no nao esta (cabo vazio, Bluetooth disabled). Achar o no
// depois (tab5_mesh_iniciar) derruba este modo e sobe o of radio.
void tab5_mesh_start_internet(void);

// Para o servico, fecha a UART e devolve o Grove. false = nao conseguiu (a
// tarefa nao confirmou a saida), e nesse caso NADA foi desmontado.
bool tab5_mesh_stop(void);

// Para o servico SO se ele estiver no Grove (via serial). Quem mexe no cabo
// (sondagem, Grove esvaziou) chama esta: via Bluetooth o no nao esta no cabo e
// a conversa segue.
bool tab5_mesh_release_grove(void);

// Por onde o servico fala com o no (SETTINGS_MESH_TRANSPORT_SERIAL/BLUETOOTH) e como vai
// o enlace, em text para a screen ("conectado a ...", "procurando ...").
uint8_t     tab5_mesh_via(void);
const char* tab5_mesh_enlace_descricao(void);

// Estado of modo internet (MQTT pelo Wi-Fi) em text para a screen: disabled,
// em espera pelo Grove, esperando o Wi-Fi, ou o servidor e a raiz em uso.
const char* tab5_mesh_internet_estado(void);

// Estado of proxy MQTT (a internet of Tab5 emprestada on no), curto, para a
// line of pagina "No": sem Wi-Fi, conectando, ou conectado com os contadores
// (publicadas pelo no / entregues a ele).
const char* tab5_mesh_proxy_estado(void);

// true when ha tarefa e UART of pe. false = so leitura: of para consultar o
// que esta no cartao, nao of para send.
bool tab5_mesh_servico_active(void);

// Le a PASTA of cartao e cria uma conversa para cada arquivo of historico.
// Permite consultar o arquivo sem o radio plugado. Devolve quantas viu.
int  tab5_mesh_carregar_of_cartao(void);

// O enlace esta vivo? (recebemos frame valido recentemente)
bool tab5_mesh_no_ar(void);

// Ha quantos ms o radio nao manda frame; UINT32_MAX se nunca mandou. Para quem
// precisa escolher o proprio prazo em vez of herdar o of no_ar() - ver a vigia
// of Grove em kernel/core.cpp.
uint32_t tab5_mesh_silencio_ms(void);

// Nosso proprio numero of no; 0 enquanto o my_info nao chegou.
uint32_t tab5_mesh_meu_numero(void);

// Firmware of C6L, string vazia se ainda nao veio.
const char* tab5_mesh_firmware(void);

// AVISO SONORO, par analogo of trilha_definir_beep() (app_trilha/trilha_fundo)
// (ver o aviso la). Liga/desliga e GRAVA a preferencia
// (settings_set_mesh_beep()) e, se a fonte "Meshtastic" ja existir em
// notification.h, atualiza a bandeira dela tambem - so ela, a Trilha e
// qualquer outra fonte continuam tocando normalmente.
void tab5_mesh_set_beep(bool touch);
bool tab5_mesh_beep_enabled(void);

// Hora UTC estimada, em segundos of epoch; 0 enquanto nao houver referencia.
//
// O Tab5 nao tem clock com hora certa, mas o radio tem: ele carimba
// last_heard em cada NodeInfo e rx_time em cada pacote. Guardamos o MAIOR
// carimbo que ja vimos junto com o nosso uptime naquele instante, e dai em
// diante a hora anda com o nosso clock. Basta para responder "ha quanto
// time", que e o unico uso.
uint32_t tab5_mesh_epoch(void);

// Hora of clock of no lida em trafego AO VIVO (ver o .cpp); 0 sem none
// in ultima hora. E a que tab5_time usa para acertar o clock - a of top,
// anchored no last_heard, pode estar horas atrasada.
uint32_t tab5_mesh_hora_alive(void);

// Nome PRONTO PARA MOSTRAR of one no, in mesma regra em todo lugar (coluna of
// conversas, rotulo of remetente no canal):
//   1. o name CURTO, se for legivel;
//   2. senao o name LONGO legivel;
//   3. so entao o id "!a1b2c3d4".
// "Legivel" = sobra text depois of tirar o que as fontes of sistema nao
// desenham (emoji e simbolos of 3-4 bytes em UTF-8; acentos ficam) e o "?"
// que o Meshtastic poe when ninguem escolheu o curto. O id e o ultimo
// recurso: ele identifica, mas ninguem reconhece one amigo por oito digitos
// em hexa. Vale com o no fora of ar, pelo cache of cartao (nos.txt).
void tab5_mesh_name_exibicao(uint32_t num, char* out, size_t out_size);

// SO o cache persistente (nos.txt) - sem o fallback of hex que
// tab5_mesh_nome_exibicao() tem. Devolve false (out intocado) when nao ha
// name conhecido daquele numero.
//
// Para colunas ESTREITAS onde repetir o ID em hexa seria redundante (ver
// montar_linha() em app_mesh.cpp: a tabela of nos ja tem uma coluna ID
// propria que mostra o hex when falta name - mostra-lo of novo in coluna
// of name curto nao acrescenta informacao none, so ocupa espaco que one
// name of verdade usaria melhor).
bool tab5_mesh_name_cache(uint32_t num, char* out, size_t out_size);

// MESMA IDEIA of tab5_mesh_nome_cache(), so que para o name LONGO. Antes
// so o curto entrava no cache (ver o aviso acima) porque
// so tab5_mesh_nome_exibicao() (que nunca usa o longo) o consultava - mas a
// tabela of nos (montar_linha() em app_mesh.cpp) sempre mostrou o longo, e
// SO of entrada AO VIVO (a NodeInfo desta sessao). NodeInfo e rara (ver o
// aviso em ler_node_info()); a maioria dos nos e aprendida por telemetria/
// position/text, sem name nenhum ainda - por isso "sempre sem name". Agora
// o longo tambem persiste. Devolve false (out intocado) when nao ha.
bool tab5_mesh_name_longo_cache(uint32_t num, char* out, size_t out_size);

// Ultimo enlace CONHECIDO of no (via_mqtt/hops_away), vindo of NodeInfo dele -
// nao of one pacote especifico. Serve of FALLBACK para uma message cujo
// proprio pacote nao trouxe hop_start (firmware of remetente as vezes nao
// manda) - "of quem" fala uma aproximacao razoavel of "como", when o "como"
// exato nao veio nesta message.: "a pagina dos nos
// sempre tem, in conversa nao aparece - of pra usar a mesma informacao?".
// Devolve false se o no nao estiver in tabela on vivo now (nunca se
// apresentou, ou foi despejado por inatividade - ver achar_no).
bool tab5_mesh_no_enlace(uint32_t num, bool* via_mqtt, uint8_t* saltos);

// ---- nos ------------------------------------------------------------------
// O no foi ouvido PELO AR (radio): tem SNR measured e nao chegou pela ponte
// MQTT (la o SNR e o of gateway, nao o of nossa antena). A tabela os fixa no
// topo, logo abaixo of nosso.
bool tab5_mesh_no_pelo_ar(const mesh_no_t* no);

// FAVORITOS (estrela). Os mesmos of no Meshtastic: marcar aqui manda o
// AdminMessage set/remove_favorite_node on radio (ou deixa pending ate o
// next dump, sem ele), e o is_favorite of dump atualiza a lista daqui. Na
// tabela ficam no topo, logo abaixo of nosso, in order em que foram marcados,
// e nunca sao despejados.
bool tab5_mesh_favorito(uint32_t num);
void tab5_mesh_set_favorito(uint32_t num, bool favorito);

int  tab5_mesh_num_nos(void);
// Copia o no of indice i, in order of tabela (o nosso, os ouvidos pelo ar,
// o resto; em cada faixa o ouvido mais recentemente first). Devolve false
// se i for invalido. Copia em vez of
// return ponteiro of proposito: a tarefa of servico reescreve a tabela a
// qualquer momento, e one ponteiro entregue a screen envelheceria no middle of
// drawing.
bool tab5_mesh_no(int i, mesh_no_t* saida);

// A MESMA TABELA, SO DE UMA REDE (MESH_REDE_NO/INTERNET), in mesma order. E o que
// a pagina of nos mostra; as duas of top continuam sendo "todos".
int  tab5_mesh_num_nos_rede(uint8_t rede);
bool tab5_mesh_no_rede(uint8_t rede, int i, mesh_no_t* saida);

// A rede publica pela internet em text para a screen: desligada, sem Wi-Fi, conectando, ou
// conectada com quantos pacotes ja chegaram.
const char* tab5_mesh_rede_internet_estado(void);

// Quantas vezes a tabela of nos (MESH_MAX_NOS - ver achar_no) esteve
// cheia e precisou despejar o mais antigo para open espaco a one no novo,
// since o boot. Nao e erro nem log automatico (poluiria o log) - so one numero para consultar sob demanda, no
// comando "mesh" of console, when quiser saber se a malha tem mais of 32
// nos of verdade.
uint32_t tab5_mesh_despejos(void);

// ---- mensagens ------------------------------------------------------------
int  tab5_mesh_num_msgs(void);
bool tab5_mesh_msg(int i, mesh_msg_t* saida);   // 0 = mais recente

// NAO USE tab5_mesh_num_msgs() PARA DETECTAR "chegou message nova" - ele
// devolve o tamanho of anel of 8 posicoes (MESH_MAX_MSGS) e PARA DE MUDAR
// assim que 8 mensagens (of qualquer conversa) ja passaram por ele, ficando
// parado para sempre dali em diante (o app Mesh comparava este numero e
// message nova so aparecia on switch of screen). tab5_mesh_total_msgs() e o contador que NUNCA satura -
// use este para "changed since a ultima vez que olhei".
// Muda tambem when one carimbo of historico e corrigido (salto of clock):
// para quem rele, "o historico changed" e o que importa.
uint32_t tab5_mesh_total_msgs(void);

// Mensagens que chegaram e ainda nao foram vistas: a SOMA das conversas
// (mesh_conversa_t.nao_lidas). Abrir uma conversa a marca como lida
// (tab5_mesh_marcar_conversa_lida); open o app nao zera as outras.
int  tab5_mesh_nao_lidas(void);
// Nao lidas apenas das mensagens DIRETAS. Merecem destaque proprio: uma
// message endereçada a voce nao e a mesma coisa que trafego of canal.
int  tab5_mesh_nao_lidas_diretas(void);

// ---------------------------------------------------------------------------
// CONVERSAS
// ---------------------------------------------------------------------------
// Uma conversa e one CANAL (LongFast e os outros) ou one NO (message direta).
// Elas aparecem sozinhas conforme a message chega - nao ha lista fixa.
//
// O historico mora no cartao, one arquivo por conversa, em
// /sdcard/apps/Meshtastic/. Motivo: a RAM guarda so as ultimas para a screen
// draw rapido, mas conversa que some no reinicio nao serve para nada.
#define MESH_MAX_CONVERSAS 12
#define MESH_MAX_CANAIS     8   // canais of one no Meshtastic (0 = o primario)
#define MESH_NOME_CONVERSA 24

typedef struct {
  bool     channel;                     // true = canal; false = message direta
  uint8_t  idx;                         // indice of canal, se canal
  uint32_t no;                          // numero of no, se direta
  char     name[MESH_NOME_CONVERSA];    // "LongFast", "capc"...
  // SEGUNDA LINHA, derivada in copia (nao guardada): o name longo of no in
  // conversa direta, "canal N" no canal. E o que deixa a coluna dizer QUEM e
  // sem cair no id em hexa.
  char     sub[MESH_NOME_LONGO];
  int      nao_lidas;
  uint32_t ultima_em;
  bool     usada;
} mesh_conversa_t;

// Abre (ou cria) a conversa direta com one no e devolve o indice VISIVEL dela,
// ou -1 se o numero nao servir. E o que permite comecar a falar com alguem a
// partir of lista of nos, sem wait que a pessoa fale first.
int  tab5_mesh_conversa_com(uint32_t no);

// Mesmo papel of tab5_mesh_conversa_com(), para CANAL - criado
// junto of opcao of erase canal, para "canais disponiveis" (mais abaixo)
// poder reabrir one que foi escondido.
int  tab5_mesh_conversa_of_canal(uint8_t idx);

// Apaga a conversa of indice VISIVEL - canal OU direta (ate
// entao recusava canal: "eles voltam sozinhos, fingir que apagou seria
// mentira" - deixou of ser verdade when canal passou a entrar in mesma
// flag of apagados que ja existia para no). Nao mexe no historico of cartao
// nos dois casos - so o registro em memoria some of lista.
bool tab5_mesh_esquecer_conversa(int i);

// OS CANAIS DO NO - todos os ATIVOS in ultima configuracao conhecida (of
// radio on vivo, ou a guardada in NVS when ele nao esta). Responde "em que
// canal of para falar" independente of a conversa estar in coluna: one canal
// apagado of coluna continua aqui, e touch nele o traz of iteration. Sem
// configuracao conhecida none, o primario (LongFast) aparece assim mesmo.
// 'visivel' = ja ha conversa dele in coluna.
int  tab5_mesh_num_canais(void);
bool tab5_mesh_canal(int i, uint8_t* idx, char* name, size_t size, bool* visivel);

// A ORDEM DA COLUNA: canais first (pelo indice), depois as diretas, of
// conversa mais recente para a mais antiga. Indice VISIVEL e position nesta
// order - e ela muda (conversa nova, apagada, direta que recebeu message).
// Quem guarda one indice (a conversa open no app) compara esta versao e,
// when ela muda, reencontra a sua pela CHAVE com tab5_mesh_conversa_indice.
// Sem isso, uma conversa nova numa vaga anterior deslocava os indices e o
// app passava a mostrar e write in conversa VIZINHA.
uint32_t tab5_mesh_conversas_versao(void);
int      tab5_mesh_conversa_indice(bool canal, uint8_t idx, uint32_t no);

int  tab5_mesh_num_conversas(void);
bool tab5_mesh_conversa(int i, mesh_conversa_t* saida);
void tab5_mesh_marcar_conversa_lida(int i);

// Le of cartao as ultimas mensagens of uma conversa, of mais antiga para a mais
// nova (que e a order em que a screen desenha). Devolve quantas leu.
int  tab5_mesh_historico(int conversa, mesh_msg_t* saida, int max);
// O mesmo, pela CHAVE (canal + indice, ou numero of no) - o arquivo e nomeado
// por ela. E o que o worker of app usa: entre o request e a leitura a order of
// coluna pode mudar, a chave nao.
int  tab5_mesh_historico_of(bool canal, uint8_t idx, uint32_t no, mesh_msg_t* saida, int max);

// Envia text. para = 0xFFFFFFFF difunde no canal 'canal'; qualquer outro valor
// e message direta para aquele no. Devolve false se o enlace estiver mudo.
bool tab5_mesh_send(uint32_t para, uint8_t canal, const char* text);

// Estado of entrega of one pacote NOSSO, pelo id. Devolve MESH_ENVIO_NA when
// o id nao esta mais in tabela - so guardamos os ultimos envios, e nada
// sobrevive on reinicio. Nao saber e one estado legitimo: melhor nao draw
// marca none of que draw uma errada.
uint8_t tab5_mesh_estado_send_state(uint32_t id, uint8_t* erro);

// Muda a cada vez que algum estado of send_state muda. A screen compara com o valor
// que ela viu por ultimo e so repinta when ha novidade - sem isso o ACK
// chegaria e o bubble ficaria com a marca velha ate o next touch.
uint32_t tab5_mesh_envios_versao(void);

// ---- saude of malha (LocalStats / telemetria of proprio no) ----------------
typedef struct {
  float    uso_canal;      // channel_utilization, %
  float    airtime_tx;     // air_util_tx, %
  uint32_t uptime_s;
  int32_t  piso_ruido;     // noise_floor, dBm
  uint16_t nos_online;
  uint16_t nos_total;
  // Contadores of pacote of LocalStats (so o nosso no os conta; 'tem_local'
  // diz se vieram).
  uint32_t pkt_tx, pkt_rx, pkt_ruins, pkt_repetidos;
  // DE ONDE VEIO. Sem LocalStats (modo internet, ou o no ainda nao mandou) os
  // numeros acima sao zero - e zero e one valor plausivel, entao a screen precisa
  // saber que e "nao veio" para write "-" em vez of mentir "0 %".
  bool     tem_local;      // LocalStats of nosso no chegou
  bool     tem_metricas;   // DeviceMetrics of nosso no chegou (uptime, uso)
} mesh_saude_t;

void tab5_mesh_saude(mesh_saude_t* saida);

// Nome legivel of HardwareModel (field 'hw' of no). Tabela COMPLETA, gerada
// of enum oficial of Meshtastic por generate_hw.py (ver tab5_mesh_hw_dados.cpp).
// Devolve nullptr para o que a lista oficial ainda nao tem (ela cresce a cada
// versao of firmware) - quem chama decide como mostrar o numero cru; inventar
// one name aqui seria pior que admitir a ignorancia.
const char* mesh_hw_name(uint8_t hw);

// A tabela em si, 256 posicoes indexadas pelo proprio code - a maioria
// nullptr. Exposta para quem quiser read direto (mesh_hw_nome ja cobre o caso
// comum); gerada, nao edit a mao.
extern const char* const TAB5_MESH_HW_NOMES[256];

// Diagnostico: quantos frames, pacotes e variantes of telemetria passaram.
// Serve para distinguir "nao chega" of "chega e o filtro descarta".
void tab5_mesh_contadores(uint32_t* quadros, uint32_t* pacotes, uint32_t* telemetria,
                          uint32_t* localstats, uint32_t* text, uint32_t* nodeinfo,
                          int* ultima_variante);

// Quantas vezes one Routing chegou DEPOIS of pacote ja estar confirmado
// (segunda resposta, ignorada para o ESTADO mas nao para este numero). Um
// termometro of qualidade of rota: sobe when "alguem ouviu" vem facil mas a
// confirmacao forte of radio nao vem - a malha funciona, so que no limit.
uint32_t tab5_mesh_routing_extra(void);
