// Servico Meshtastic - o enlace com o M5 Unit C6L pelo Grove.
//
// SERVICO, nao aplicativo. A tarefa fica de pe enquanto o Grove estiver
// entregue ao Meshtastic, independente de qual app esta na tela. E o que
// permite notificar mensagem nova sem o app aberto - se o enlace morresse junto
// com a janela, so haveria mensagem enquanto voce estivesse olhando.
//
// Mesmo desenho de tab5_wifi e tab5_teclado: aqui mora o estado, o app le.
//
// -------------------------------------------------------------------------
// O PROTOCOLO, EM DUAS LINHAS
// -------------------------------------------------------------------------
// O Meshtastic em modo PROTO manda protobuf enquadrado:
//
//   94 C3 <tam_hi> <tam_lo> <protobuf de FromRadio>
//
// Verificado na bancada: 20 frames seguidos decodificados, e o
// numero do no lido aqui bateu com o que a CLI leu pelo USB (4044735316).
//
// NAO usamos os .proto oficiais do Meshtastic. Sao dezenas de arquivos e um
// gerador para ler meia duzia de campos. Protobuf e simples o bastante para
// andar a mao: cada campo e <tag varint><valor>, e a tag traz numero e tipo.
// Mesmo caminho do NMEA e do teclado, que deram certo.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   // size_t - usado em tab5_mesh_nome_exibicao()

// NOS AO VIVO. Eram 32 - o mesmo teto apertado de um no pequeno. Com o MQTT
// ligado no C6L (downlink), cada no da internet que fala vira entrada aqui,
// e 32 vagas giravam em segundos: um no de RADIO quieto (apresentacao de 3 em
// 3 h, telemetria de 30 em 30 min) era despejado antes de aparecer na tela,
// mesmo com os pacotes dele passando pelo C6L. 256 na PSRAM sao ~30 KB.
#define MESH_MAX_NOS     256
#define MESH_MAX_MSGS      8
#define MESH_NOME_LONGO   40
#define MESH_NOME_CURTO    6

// Buffer de TEXTO RECEBIDO: 238 cabe qualquer payload valido com folga e o
// terminador. O teto do protocolo e menor - ver MESH_PAYLOAD_MAX.
#define MESH_TEXTO       238

// TETO DE ENVIO: Data.payload tem max_size 233 no mesh.pb.h do firmware
// (meshtastic_Constants_DATA_PAYLOAD_LEN). Um ToRadio com payload maior falha
// na decodificacao nanopb DENTRO do no e e descartado sem resposta nenhuma -
// nem queueStatus, nem Routing: a mensagem so vira "sem resposta" 60 s depois.
// Por isso o corte acontece ANTES de sair, na fronteira de um caractere UTF-8,
// e a barra de escrita ja nao aceita mais que isto (conversa_t.limite).
//
// E O TETO DO PROTOBUF NAO E O DO AR. O firmware ainda acrescenta, no Data
// codificado, o 'bitfield' (2 bytes) e o cabecalho de 16; o pacote inteiro
// nao passa de 255 (MAX_LORA_PAYLOAD_LEN). Texto de canal cabe ate ~232; a
// mensagem DIRETA cifrada com chave publica (PKI) leva mais 12 e cabe ate
// ~220. Acima disso o no responde TOO_LARGE (Router.cpp, perhapsEncode). Os
// clientes oficiais param em 200 - e o mesmo teto aqui, com folga para os
// campos que versoes novas acrescentarem.
#define MESH_PAYLOAD_MAX 200

// Hora valida: tab5_hora_valida() / tab5_hora_epoch() (services/tab5_hora.hpp).

// AS DUAS REDES. NO e a de sempre (o no, o modo internet, o proxy); INTERNET e
// o LongFast pelo servidor publico (services/tab5_mesh_mqtt.hpp), ligado em
// ajustes_mesh_rede_internet(). Os nos ficam marcados com a rede onde foram
// ouvidos (NO vence: no ouvido aqui e pela internet). A conversa da INTERNET e
// um CANAL com o indice MESH_CANAL_INTERNET - fora da faixa dos 8 canais do no, entao nunca
// colide com eles, e a chave (canal, idx) continua valendo em todo lugar.
#define MESH_REDE_NO   0
#define MESH_REDE_INTERNET  1
#define MESH_CANAL_INTERNET 0x80
#define MESH_MAX_NOS_INTERNET 96   // teto da rede publica na tabela de 256: nao expulsa os daqui
static inline bool mesh_canal_internet(uint8_t idx) { return (idx & MESH_CANAL_INTERNET) != 0; }

// Sentinela de mesh_msg_t::saltos - "nao da para saber", nunca "zero saltos".
#define MESH_SALTOS_DESCONHECIDO 255

typedef struct {
  uint32_t num;                        // identificador do no na malha
  char     longo[MESH_NOME_LONGO];     // "Meshtastic c354"
  char     curto[MESH_NOME_CURTO];     // "capc"
  float    snr;                        // qualidade do ultimo pacote dele
  uint32_t visto_em;                   // last_heard, hora UTC do radio

  // Uptime NOSSO do ultimo pacote que ouvimos dele; 0 = nao ouvimos nenhum
  // desde que ligamos. Existe porque o last_heard acima e a hora do RADIO, e
  // nos nem sempre sabemos que horas sao - ver tab5_mesh_epoch().
  uint32_t visto_local_ms;

  // Ha quantos segundos o no foi ouvido pela ultima vez; -1 = nao da para
  // saber. NAO e guardado na tabela: e calculado no tab5_mesh_no(), no momento
  // da copia, justamente para nao envelhecer entre a leitura e o desenho.
  int32_t  idade_s;

  // IDENTIDADE DE HARDWARE, que e o que nao muda.
  //
  // 'longo' e 'curto' sao ETIQUETAS: quem renomeia o no no celular troca as
  // duas, e elas nao servem para dizer com que aparelho voce esta falando. O
  // que serve e isto:
  //
  //   id      - "!a1b2c3d4", o identificador que o Meshtastic usa em todo lugar
  //             (log, mapa, aplicativo). E derivado do MAC e nao muda.
  //   mac     - os 6 bytes, quando o no os manda. Campo marcado como obsoleto
  //             no protobuf, entao firmware novo costuma omitir - por isso
  //             'tem_mac' existe, para nao desenhar 00:00:00:00:00:00 como se
  //             fosse um endereco de verdade.
  //   hw      - HardwareModel: qual placa e (Heltec, T-Beam, Unit C6L...).
  char     id[16];                     // "!a1b2c3d4"; vazio = nao veio
  uint8_t  mac[6];
  bool     tem_mac;
  uint8_t  hw;                         // HardwareModel; 0 = desconhecido

  uint8_t  bateria;                    // 0-100; 101 = na tomada
  float    tensao;
  uint8_t  saltos;                     // hops_away: 0 = vizinho direto

  // O no chegou pela INTERNET (MQTT), nao pelo radio. Muda o sentido de tudo
  // que se ve dele: o SNR nao e da nossa antena, os saltos nao sao de RF, e
  // nao da para alcanca-lo se a internet cair.
  bool     via_mqtt;
  int32_t  lat_i, lon_i;               // graus x 1e7 (0 = sem posicao)
  bool     tem_pos;
  bool     usado;
  // MESH_REDE_NO/INTERNET. No vao que o alinhamento ja deixava antes de 'fixado':
  // o tamanho da struct nao muda, e o nodedb.bin antigo continua valendo
  // (com zero ali, que e NO).
  uint8_t  rede;

  // LUGAR FIXO NO TOPO DA TABELA: a ordem em que o no foi ouvido pelo ar (com
  // SNR) pela primeira vez; 0 = ainda nao. Dado uma vez, nao muda - o no nao
  // desce quando outro fala depois. Ver ordem_nos() no .cpp.
  uint32_t fixado;

  // Estrela - derivado na copia (tab5_mesh_no), a lista mora no servico.
  bool     favorito;
} mesh_no_t;

// ---------------------------------------------------------------------------
// CONFIRMACAO DE ENTREGA
// ---------------------------------------------------------------------------
// O Meshtastic confirma de verdade. Ao enviar com want_ack, o radio devolve um
// pacote na porta ROUTING (5) trazendo request_id = o id do NOSSO pacote e um
// Routing.error_reason: zero e ACK, qualquer outro valor e a razao da falha.
//
// UMA DISTINCAO QUE IMPORTA:
//   - Mensagem DIRETA: o ACK vem do DESTINATARIO. "Chegou nele."
//   - Mensagem de CANAL (difusao): o ACK e IMPLICITO - o radio ouviu o proprio
//     pacote sendo repetido por um vizinho. Quer dizer "entrou na malha", nao
//     "alguem leu". Nao ha como saber quem recebeu uma difusao, e nenhum
//     cliente Meshtastic sabe.
typedef enum {
  MESH_ENVIO_NA = 0,     // recebida, ou nossa mas de antes do ultimo reinicio
  MESH_ENVIANDO,         // esperando o radio confirmar
  MESH_ENTREGUE,         // ACK
  MESH_FALHOU,           // NAK com motivo, ou o radio recusou enfileirar
  MESH_SEM_RESPOSTA,     // esgotou o prazo sem o radio dizer nada
  // MENSAGEM DIRETA OUVIDA PELA MALHA, SEM CONFIRMACAO DO DESTINO. O no gera
  // um ACK IMPLICITO (Routing vindo do proprio no, erro 0) quando ouve um
  // vizinho repetir o pacote. Na difusao isso e o melhor que existe e vale
  // ENTREGUE; na direta e so meio caminho - ENTREGUE fica reservado ao ACK
  // vindo do PROPRIO destinatario. Numero novo no FIM do enum: o envios.txt
  // guarda o valor, e os antigos continuam significando o mesmo.
  MESH_OUVIDO,
} mesh_envio_t;

// Codigo nosso, fora da faixa do Routing.Error do Meshtastic: o radio recusou
// pôr o pacote na queue (QueueStatus com res != 0).
#define MESH_ERRO_FILA 200

// Nome do motivo, para a tela. Devolve "?" para codigo que nao conhecemos - a
// lista do Meshtastic cresce a cada versao e inventar nome seria pior.
const char* mesh_erro_nome(uint8_t erro);

typedef struct {
  uint32_t de;                         // no de origem
  uint32_t para;                       // 0xFFFFFFFF = canal; nosso num = direta
  uint8_t  canal;                      // indice do canal (0 = o primario)
  bool     direta;                     // conveniencia: para == nosso numero
  char     texto[MESH_TEXTO];
  uint32_t quando;                     // rx_time do radio
  float    snr;
  int32_t  rssi;

  // O PACOTE CHEGOU PELA INTERNET (ponte MQTT de algum no), NAO PELO RADIO.
  // Campo 13 do MeshPacketpara distinguir "ouvi isso
  // no ar" de "isso me chegou pela nuvem". Quando true, snr/rssi ainda vem
  // preenchido pelo radio (e o do ULTIMO salto local que entregou a ponte),
  // mas nao descreve o link fim-a-fim da mensagem - so o pedaco por RF que
  // trouxe ela ate quem tinha a ponte.
  bool     via_mqtt;

  // QUANTOS SALTOS DE RADIO esta mensagem especifica levou ate chegar aqui
  // (so faz sentido para LoRa; via MQTT nao ha saltos). Calculado de
  // hop_start - hop_limit (campos 14 e 9 do MeshPacket, os DOIS lidos do
  // MESMO pacote): hop_start e o teto que o remetente armou, hop_limit e o
  // que sobrou quando o pacote chegou aqui - a diferenca e quantos
  // repetidores ele passou.
  //
  // 255 = NAO DA PARA SABER (firmware antigo nao manda hop_start, ou a
  // mensagem e via_mqtt e a pergunta nem faz sentido do lado da internet) -
  // mesmo espirito do sentinela de SNR (-128): nao inventar um numero que
  // nao existe.
  uint8_t  saltos;

  // Id do pacote na malha. E o que liga a mensagem gravada no cartao ao ACK
  // que chega depois. 0 quando nao e nossa.
  uint32_t id;

  // Campos DERIVADOS, preenchidos na copia a partir do id - nao ficam no
  // cartao. O estado vive so em RAM: gravar "enviando" num arquivo e acordar
  // com ele no dia seguinte seria mentira.
  uint8_t  envio;                      // mesh_envio_t
  uint8_t  erro;                       // motivo, quando envio == MESH_FALHOU
} mesh_msg_t;

// Cria a tarefa do servico. NAO liga o trilho do Grove - quem faz isso e o
// perfil de energia, via ajuste do usuario (ver sistema_impor_preferencias).
void tab5_mesh_iniciar(void);

// Sobe o servico SO pela internet (MQTT), sem o no - ver
// services/tab5_mesh_mqtt.hpp. Quem chama e o kernel, quando o Wi-Fi esta
// conectado e o no nao esta (cabo vazio, Bluetooth desligado). Achar o no
// depois (tab5_mesh_iniciar) derruba este modo e sobe o do radio.
void tab5_mesh_iniciar_internet(void);

// Para o servico, fecha a UART e devolve o Grove. false = nao conseguiu (a
// tarefa nao confirmou a saida), e nesse caso NADA foi desmontado.
bool tab5_mesh_parar(void);

// Para o servico SO se ele estiver no Grove (via serial). Quem mexe no cabo
// (sondagem, Grove esvaziou) chama esta: via Bluetooth o no nao esta no cabo e
// a conversa segue.
bool tab5_mesh_soltar_grove(void);

// Por onde o servico fala com o no (AJUSTES_MESH_SERIAL/BLUETOOTH) e como vai
// o enlace, em texto para a tela ("conectado a ...", "procurando ...").
uint8_t     tab5_mesh_via(void);
const char* tab5_mesh_enlace_descricao(void);

// Estado do modo internet (MQTT pelo Wi-Fi) em texto para a tela: desligado,
// em espera pelo Grove, esperando o Wi-Fi, ou o servidor e a raiz em uso.
const char* tab5_mesh_internet_estado(void);

// Estado do proxy MQTT (a internet do Tab5 emprestada ao no), curto, para a
// linha da pagina "No": sem Wi-Fi, conectando, ou conectado com os contadores
// (publicadas pelo no / entregues a ele).
const char* tab5_mesh_proxy_estado(void);

// true quando ha tarefa e UART de pe. false = so leitura: da para consultar o
// que esta no cartao, nao da para enviar.
bool tab5_mesh_servico_ativo(void);

// Le a PASTA do cartao e cria uma conversa para cada arquivo de historico.
// Permite consultar o arquivo sem o radio plugado. Devolve quantas viu.
int  tab5_mesh_carregar_do_cartao(void);

// O enlace esta vivo? (recebemos frame valido recentemente)
bool tab5_mesh_no_ar(void);

// Ha quantos ms o radio nao manda frame; UINT32_MAX se nunca mandou. Para quem
// precisa escolher o proprio prazo em vez de herdar o do no_ar() - ver a vigia
// do Grove em kernel/core.cpp.
uint32_t tab5_mesh_silencio_ms(void);

// Nosso proprio numero de no; 0 enquanto o my_info nao chegou.
uint32_t tab5_mesh_meu_numero(void);

// Firmware do C6L, string vazia se ainda nao veio.
const char* tab5_mesh_firmware(void);

// AVISO SONORO, par analogo de trilha_definir_beep() (app_trilha/trilha_fundo)
// (ver o aviso la). Liga/desliga e GRAVA a preferencia
// (ajustes_definir_beep_mesh()) e, se a fonte "Meshtastic" ja existir em
// notificacao.h, atualiza a bandeira dela tambem - so ela, a Trilha e
// qualquer outra fonte continuam tocando normalmente.
void tab5_mesh_definir_beep(bool tocar);
bool tab5_mesh_beep_ligado(void);

// Hora UTC estimada, em segundos de epoch; 0 enquanto nao houver referencia.
//
// O Tab5 nao tem relogio com hora certa, mas o radio tem: ele carimba
// last_heard em cada NodeInfo e rx_time em cada pacote. Guardamos o MAIOR
// carimbo que ja vimos junto com o nosso uptime naquele instante, e dai em
// diante a hora anda com o nosso relogio. Basta para responder "ha quanto
// tempo", que e o unico uso.
uint32_t tab5_mesh_epoch(void);

// Hora do relogio do no lida em trafego AO VIVO (ver o .cpp); 0 sem nenhuma
// na ultima hora. E a que tab5_hora usa para acertar o relogio - a de cima,
// ancorada no last_heard, pode estar horas atrasada.
uint32_t tab5_mesh_hora_viva(void);

// Nome PRONTO PARA MOSTRAR de um no, na mesma regra em todo lugar (coluna de
// conversas, rotulo do remetente no canal):
//   1. o nome CURTO, se for legivel;
//   2. senao o nome LONGO legivel;
//   3. so entao o id "!a1b2c3d4".
// "Legivel" = sobra texto depois de tirar o que as fontes do sistema nao
// desenham (emoji e simbolos de 3-4 bytes em UTF-8; acentos ficam) e o "?"
// que o Meshtastic poe quando ninguem escolheu o curto. O id e o ultimo
// recurso: ele identifica, mas ninguem reconhece um amigo por oito digitos
// em hexa. Vale com o no fora do ar, pelo cache do cartao (nos.txt).
void tab5_mesh_nome_exibicao(uint32_t num, char* out, size_t out_tam);

// SO o cache persistente (nos.txt) - sem o fallback de hex que
// tab5_mesh_nome_exibicao() tem. Devolve false (out intocado) quando nao ha
// nome conhecido daquele numero.
//
// Para colunas ESTREITAS onde repetir o ID em hexa seria redundante (ver
// montar_linha() em app_mesh.cpp: a tabela de nos ja tem uma coluna ID
// propria que mostra o hex quando falta nome - mostra-lo de novo na coluna
// de nome curto nao acrescenta informacao nenhuma, so ocupa espaco que um
// nome de verdade usaria melhor).
bool tab5_mesh_nome_cache(uint32_t num, char* out, size_t out_tam);

// MESMA IDEIA de tab5_mesh_nome_cache(), so que para o nome LONGO. Antes
// so o curto entrava no cache (ver o aviso acima) porque
// so tab5_mesh_nome_exibicao() (que nunca usa o longo) o consultava - mas a
// tabela de nos (montar_linha() em app_mesh.cpp) sempre mostrou o longo, e
// SO da entrada AO VIVO (a NodeInfo desta sessao). NodeInfo e rara (ver o
// aviso em ler_node_info()); a maioria dos nos e aprendida por telemetria/
// posicao/texto, sem nome nenhum ainda - por isso "sempre sem nome". Agora
// o longo tambem persiste. Devolve false (out intocado) quando nao ha.
bool tab5_mesh_nome_longo_cache(uint32_t num, char* out, size_t out_tam);

// Ultimo enlace CONHECIDO do no (via_mqtt/hops_away), vindo da NodeInfo dele -
// nao de um pacote especifico. Serve de FALLBACK para uma mensagem cujo
// proprio pacote nao trouxe hop_start (firmware do remetente as vezes nao
// manda) - "de quem" fala uma aproximacao razoavel de "como", quando o "como"
// exato nao veio nesta mensagem.: "a pagina dos nos
// sempre tem, na conversa nao aparece - da pra usar a mesma informacao?".
// Devolve false se o no nao estiver na tabela ao vivo agora (nunca se
// apresentou, ou foi despejado por inatividade - ver achar_no).
bool tab5_mesh_no_enlace(uint32_t num, bool* via_mqtt, uint8_t* saltos);

// ---- nos ------------------------------------------------------------------
// O no foi ouvido PELO AR (radio): tem SNR medido e nao chegou pela ponte
// MQTT (la o SNR e o do gateway, nao o da nossa antena). A tabela os fixa no
// topo, logo abaixo do nosso.
bool tab5_mesh_no_pelo_ar(const mesh_no_t* no);

// FAVORITOS (estrela). Os mesmos do no Meshtastic: marcar aqui manda o
// AdminMessage set/remove_favorite_node ao radio (ou deixa pendente ate o
// proximo dump, sem ele), e o is_favorite do dump atualiza a lista daqui. Na
// tabela ficam no topo, logo abaixo do nosso, na ordem em que foram marcados,
// e nunca sao despejados.
bool tab5_mesh_favorito(uint32_t num);
void tab5_mesh_definir_favorito(uint32_t num, bool favorito);

int  tab5_mesh_num_nos(void);
// Copia o no de indice i, na ordem da tabela (o nosso, os ouvidos pelo ar,
// o resto; em cada faixa o ouvido mais recentemente primeiro). Devolve false
// se i for invalido. Copia em vez de
// devolver ponteiro de proposito: a tarefa do servico reescreve a tabela a
// qualquer momento, e um ponteiro entregue a tela envelheceria no meio do
// desenho.
bool tab5_mesh_no(int i, mesh_no_t* saida);

// A MESMA TABELA, SO DE UMA REDE (MESH_REDE_NO/INTERNET), na mesma ordem. E o que
// a pagina de nos mostra; as duas de cima continuam sendo "todos".
int  tab5_mesh_num_nos_rede(uint8_t rede);
bool tab5_mesh_no_rede(uint8_t rede, int i, mesh_no_t* saida);

// A rede publica pela internet em texto para a tela: desligada, sem Wi-Fi, conectando, ou
// conectada com quantos pacotes ja chegaram.
const char* tab5_mesh_rede_internet_estado(void);

// Quantas vezes a tabela de nos (MESH_MAX_NOS - ver achar_no) esteve
// cheia e precisou despejar o mais antigo para abrir espaco a um no novo,
// desde o boot. Nao e erro nem log automatico (poluiria o log) - so um numero para consultar sob demanda, no
// comando "mesh" do console, quando quiser saber se a malha tem mais de 32
// nos de verdade.
uint32_t tab5_mesh_despejos(void);

// ---- mensagens ------------------------------------------------------------
int  tab5_mesh_num_msgs(void);
bool tab5_mesh_msg(int i, mesh_msg_t* saida);   // 0 = mais recente

// NAO USE tab5_mesh_num_msgs() PARA DETECTAR "chegou mensagem nova" - ele
// devolve o tamanho do anel de 8 posicoes (MESH_MAX_MSGS) e PARA DE MUDAR
// assim que 8 mensagens (de qualquer conversa) ja passaram por ele, ficando
// parado para sempre dali em diante (o app Mesh comparava este numero e
// mensagem nova so aparecia ao trocar de tela). tab5_mesh_total_msgs() e o contador que NUNCA satura -
// use este para "mudou desde a ultima vez que olhei".
// Muda tambem quando um carimbo do historico e corrigido (salto do relogio):
// para quem rele, "o historico mudou" e o que importa.
uint32_t tab5_mesh_total_msgs(void);

// Mensagens que chegaram e ainda nao foram vistas: a SOMA das conversas
// (mesh_conversa_t.nao_lidas). Abrir uma conversa a marca como lida
// (tab5_mesh_marcar_conversa_lida); abrir o app nao zera as outras.
int  tab5_mesh_nao_lidas(void);
// Nao lidas apenas das mensagens DIRETAS. Merecem destaque proprio: uma
// mensagem endereçada a voce nao e a mesma coisa que trafego do canal.
int  tab5_mesh_nao_lidas_diretas(void);

// ---------------------------------------------------------------------------
// CONVERSAS
// ---------------------------------------------------------------------------
// Uma conversa e um CANAL (LongFast e os outros) ou um NO (mensagem direta).
// Elas aparecem sozinhas conforme a mensagem chega - nao ha lista fixa.
//
// O historico mora no cartao, um arquivo por conversa, em
// /sdcard/apps/Meshtastic/. Motivo: a RAM guarda so as ultimas para a tela
// desenhar rapido, mas conversa que some no reinicio nao serve para nada.
#define MESH_MAX_CONVERSAS 12
#define MESH_MAX_CANAIS     8   // canais de um no Meshtastic (0 = o primario)
#define MESH_NOME_CONVERSA 24

typedef struct {
  bool     canal;                       // true = canal; false = mensagem direta
  uint8_t  idx;                         // indice do canal, se canal
  uint32_t no;                          // numero do no, se direta
  char     nome[MESH_NOME_CONVERSA];    // "LongFast", "capc"...
  // SEGUNDA LINHA, derivada na copia (nao guardada): o nome longo do no na
  // conversa direta, "canal N" no canal. E o que deixa a coluna dizer QUEM e
  // sem cair no id em hexa.
  char     sub[MESH_NOME_LONGO];
  int      nao_lidas;
  uint32_t ultima_em;
  bool     usada;
} mesh_conversa_t;

// Abre (ou cria) a conversa direta com um no e devolve o indice VISIVEL dela,
// ou -1 se o numero nao servir. E o que permite comecar a falar com alguem a
// partir da lista de nos, sem esperar que a pessoa fale primeiro.
int  tab5_mesh_conversa_com(uint32_t no);

// Mesmo papel de tab5_mesh_conversa_com(), para CANAL - criado
// junto da opcao de apagar canal, para "canais disponiveis" (mais abaixo)
// poder reabrir um que foi escondido.
int  tab5_mesh_conversa_do_canal(uint8_t idx);

// Apaga a conversa de indice VISIVEL - canal OU direta (ate
// entao recusava canal: "eles voltam sozinhos, fingir que apagou seria
// mentira" - deixou de ser verdade quando canal passou a entrar na mesma
// flag de apagados que ja existia para no). Nao mexe no historico do cartao
// nos dois casos - so o registro em memoria some da lista.
bool tab5_mesh_esquecer_conversa(int i);

// OS CANAIS DO NO - todos os ATIVOS na ultima configuracao conhecida (do
// radio ao vivo, ou a guardada na NVS quando ele nao esta). Responde "em que
// canal da para falar" independente de a conversa estar na coluna: um canal
// apagado da coluna continua aqui, e tocar nele o traz de volta. Sem
// configuracao conhecida nenhuma, o primario (LongFast) aparece assim mesmo.
// 'visivel' = ja ha conversa dele na coluna.
int  tab5_mesh_num_canais(void);
bool tab5_mesh_canal(int i, uint8_t* idx, char* nome, size_t tam, bool* visivel);

// A ORDEM DA COLUNA: canais primeiro (pelo indice), depois as diretas, da
// conversa mais recente para a mais antiga. Indice VISIVEL e posicao nesta
// ordem - e ela muda (conversa nova, apagada, direta que recebeu mensagem).
// Quem guarda um indice (a conversa aberta no app) compara esta versao e,
// quando ela muda, reencontra a sua pela CHAVE com tab5_mesh_conversa_indice.
// Sem isso, uma conversa nova numa vaga anterior deslocava os indices e o
// app passava a mostrar e escrever na conversa VIZINHA.
uint32_t tab5_mesh_conversas_versao(void);
int      tab5_mesh_conversa_indice(bool canal, uint8_t idx, uint32_t no);

int  tab5_mesh_num_conversas(void);
bool tab5_mesh_conversa(int i, mesh_conversa_t* saida);
void tab5_mesh_marcar_conversa_lida(int i);

// Le do cartao as ultimas mensagens de uma conversa, da mais antiga para a mais
// nova (que e a ordem em que a tela desenha). Devolve quantas leu.
int  tab5_mesh_historico(int conversa, mesh_msg_t* saida, int max);
// O mesmo, pela CHAVE (canal + indice, ou numero do no) - o arquivo e nomeado
// por ela. E o que o worker do app usa: entre o pedido e a leitura a ordem da
// coluna pode mudar, a chave nao.
int  tab5_mesh_historico_de(bool canal, uint8_t idx, uint32_t no, mesh_msg_t* saida, int max);

// Envia texto. para = 0xFFFFFFFF difunde no canal 'canal'; qualquer outro valor
// e mensagem direta para aquele no. Devolve false se o enlace estiver mudo.
bool tab5_mesh_enviar(uint32_t para, uint8_t canal, const char* texto);

// Estado de entrega de um pacote NOSSO, pelo id. Devolve MESH_ENVIO_NA quando
// o id nao esta mais na tabela - so guardamos os ultimos envios, e nada
// sobrevive ao reinicio. Nao saber e um estado legitimo: melhor nao desenhar
// marca nenhuma do que desenhar uma errada.
uint8_t tab5_mesh_estado_envio(uint32_t id, uint8_t* erro);

// Muda a cada vez que algum estado de envio muda. A tela compara com o valor
// que ela viu por ultimo e so repinta quando ha novidade - sem isso o ACK
// chegaria e o balao ficaria com a marca velha ate o proximo toque.
uint32_t tab5_mesh_envios_versao(void);

// ---- saude da malha (LocalStats / telemetria do proprio no) ----------------
typedef struct {
  float    uso_canal;      // channel_utilization, %
  float    airtime_tx;     // air_util_tx, %
  uint32_t uptime_s;
  int32_t  piso_ruido;     // noise_floor, dBm
  uint16_t nos_online;
  uint16_t nos_total;
  // Contadores de pacote do LocalStats (so o nosso no os conta; 'tem_local'
  // diz se vieram).
  uint32_t pkt_tx, pkt_rx, pkt_ruins, pkt_repetidos;
  // DE ONDE VEIO. Sem LocalStats (modo internet, ou o no ainda nao mandou) os
  // numeros acima sao zero - e zero e um valor plausivel, entao a tela precisa
  // saber que e "nao veio" para escrever "-" em vez de mentir "0 %".
  bool     tem_local;      // LocalStats do nosso no chegou
  bool     tem_metricas;   // DeviceMetrics do nosso no chegou (uptime, uso)
} mesh_saude_t;

void tab5_mesh_saude(mesh_saude_t* saida);

// Nome legivel do HardwareModel (campo 'hw' do no). Tabela COMPLETA, gerada
// do enum oficial do Meshtastic por gerar_hw.py (ver tab5_mesh_hw_dados.cpp).
// Devolve nullptr para o que a lista oficial ainda nao tem (ela cresce a cada
// versao do firmware) - quem chama decide como mostrar o numero cru; inventar
// um nome aqui seria pior que admitir a ignorancia.
const char* mesh_hw_nome(uint8_t hw);

// A tabela em si, 256 posicoes indexadas pelo proprio codigo - a maioria
// nullptr. Exposta para quem quiser ler direto (mesh_hw_nome ja cobre o caso
// comum); gerada, nao editar a mao.
extern const char* const TAB5_MESH_HW_NOMES[256];

// Diagnostico: quantos frames, pacotes e variantes de telemetria passaram.
// Serve para distinguir "nao chega" de "chega e o filtro descarta".
void tab5_mesh_contadores(uint32_t* quadros, uint32_t* pacotes, uint32_t* telemetria,
                          uint32_t* localstats, uint32_t* texto, uint32_t* nodeinfo,
                          int* ultima_variante);

// Quantas vezes um Routing chegou DEPOIS do pacote ja estar confirmado
// (segunda resposta, ignorada para o ESTADO mas nao para este numero). Um
// termometro de qualidade de rota: sobe quando "alguem ouviu" vem facil mas a
// confirmacao forte do radio nao vem - a malha funciona, so que no limite.
uint32_t tab5_mesh_routing_extra(void);
