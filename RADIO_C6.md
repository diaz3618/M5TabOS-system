# O rádio do Tab5: o ESP32-C6

O ESP32-P4 do Tab5 não tem rádio. Wi-Fi, Bluetooth LE e o rádio 802.15.4 moram
no **ESP32-C6**, ligado ao P4 por SDIO e falando com ele pelo **esp_hosted**
(3.0.9 nos dois lados). Este documento descreve como o M5tabOS usa o C6, quem
manda em quê, e as armadilhas que custaram testes no aparelho — com a prova de
cada uma nas fontes do ESP-IDF e do esp_hosted, ou no binário do driver.

Versões conferidas: ESP-IDF 5.5.5, esp_hosted 3.0.9, firmware do C6
`m5tabos-c6-3` (contrato `C6_PROTO_VERSAO 3`).

## Quem roda o quê

| Recurso | Onde roda | Como o P4 chega lá |
|---|---|---|
| Wi-Fi STA (associar, IP, sockets) | driver no C6 | RPC do esp_hosted (`esp_wifi_*` do lado do P4 viram chamadas remotas) |
| Bluetooth LE | controlador no C6, stack NimBLE no P4 | HCI por VHCI |
| ESP-NOW, farejo (promíscuo), CSI, FTM, 802.15.4 | firmware próprio do C6 (`c6/`) | canal `peer_data` do esp_hosted, contrato em `shared/tab5_c6.h` |

O esp_hosted não leva ESP-NOW, modo promíscuo nem CSI pelo RPC: os IDs existem
no `rpc_v2.proto`, mas não há implementação dos dois lados. Por isso esses
recursos rodam **no C6**, e o P4 só liga, desliga e recebe os resultados.

## Regra de ouro: a config de init do Wi-Fi do C6 vem do P4

Quem chama `esp_wifi_init()` no C6 não é o nosso `app_main`: é o handler de RPC
do esp_hosted (`req_wifi_init`, em
`coprocessor/features/eh_cp_feat_rpc_ext_v2/src/eh_cp_feat_rpc_ext_v2_handler_req_wifi.c`),
com a `wifi_init_config_t` que **o P4 mandou**. A função
`get_merged_init_config` declara a política no próprio comentário — *"always
use value from host"* — para quase todos os campos:

`static_rx_buf_num`, `dynamic_rx_buf_num`, `tx_buf_type`, `static_tx_buf_num`,
`dynamic_tx_buf_num`, `rx_mgmt_buf_*`, **`csi_enable`**, `ampdu_*`, `amsdu_tx`,
`nvs_enable`, `rx_ba_win`, `beacon_max_len`, `mgmt_sbuf_num`,
`sta_disconnected_pm`, `espnow_max_encrypt_num`...

Consequência: **o `sdkconfig` do C6 não decide esses valores.**
`CONFIG_ESP_WIFI_CSI_ENABLED=y` no C6 só muda o `WIFI_INIT_CONFIG_DEFAULT` de
lá, que o handler sobrescreve com o que chega do P4. Os valores que valem são
os `CONFIG_WIFI_RMT_*` do `sdkconfig` do P4, ou o que o código do P4 puser na
struct antes do `esp_wifi_init` (`main/hal/tab5_wifi.cpp`).

A exceção é `feature_caps` (FTM, WPA3, GCMP...): o C6 só aceita a do P4 quando
ela é igual à dele ou quando há cache de TX (só com PSRAM, que o C6 não tem);
senão usa **a própria**. Conferido no binário gravado (`req_wifi_init`): o
valor do C6 é 301 (0x12D = WPA3-SAE, FTM initiator e responder, GMAC, BSS max
idle) e é ele que segue para o `esp_wifi_init`. O suporte a FTM do driver
mora nesse campo (`ftm_is_initiator_supported` lê o bit 2 de `feature_caps`),
então é ele que decide se o C6 aceita iniciar uma sessão FTM — ver
[FTM](#ftm-medida-de-distância).

Quando o P4 reinicia, ele reseta o C6, então uma mudança nessa config vale no
boot seguinte. Se o C6 já estiver com o Wi-Fi iniciado e a config chegar
diferente, o esp_hosted para, desinicia e reinicia o Wi-Fi sozinho
(`__wrap_esp_wifi_init`).

## Armadilha 1: CSI recusado com `ESP_FAIL`

**Sintoma.** `radio csi` no console: o C6 recusa ligar o CSI com `ESP_FAIL` e
o callback nunca é chamado. A configuração era igual à do exemplo oficial
(esp-csi) para o C6; trocar a ordem das chamadas ou ligar o modo promíscuo
antes não mudou nada.

**Causa.** O P4 iniciava o Wi-Fi com `csi_enable = 0`
(`CONFIG_WIFI_RMT_CSI_ENABLED` desligado), e o C6 obedecia.

**Prova, no binário do driver** (`components/esp_wifi/lib/esp32c6/libnet80211.a`,
desmontado com `riscv32-esp-elf-objdump`):

1. `wifi_menuconfig_init` (em `ieee80211_ioctl.o`) copia campos da
   `wifi_init_config_t` para a global `g_wifi_menuconfig`. O campo no offset
   80 da struct de entrada vai para o offset 24 da global. Offset 80 é o
   `csi_enable`: `osi_funcs` (4) + `wpa_crypto_funcs` (44) põem
   `static_rx_buf_num` em 48, e mais oito `int` levam a 80.
2. `esp_wifi_set_csi` e `esp_wifi_set_csi_config` (em `ieee80211_api.o`)
   conferem, nesta ordem: Wi-Fi iniciado (`ESP_ERR_WIFI_NOT_INIT`), Wi-Fi
   ligado (`ESP_ERR_WIFI_NOT_STARTED`), e **`g_wifi_menuconfig + 24`**. Se for
   zero, chamam `wifi_log` com a string *"CSI not enabled in menuconfig!"* e
   devolvem **-1 (`ESP_FAIL`)**.
3. Depois dessa checagem não há outro caminho de erro: `wifi_set_csi` →
   `ic_set_csi` → `hal_mac_set_csi`, e `wifi_csi_set_config` →
   `ic_csi_set_config` só gravam registradores. Nem ordem de chamada nem modo
   promíscuo influenciam.

**Correção.** No P4, `cfg.csi_enable = 1` antes do `esp_wifi_init`
(`main/hal/tab5_wifi.cpp`). Não exige regravar o C6. Custo: o C6 reserva cerca
de 1 KB por buffer RX estático para o CSI (com 10 buffers, ~10 KB da RAM
dele), mesmo com o CSI parado. Ligar só sob pedido obrigaria a reiniciar o
Wi-Fi a cada vez.

No C6, o CSI passou a ter um caminho só (o do exemplo da Espressif):
`esp_wifi_set_csi_config` → `esp_wifi_set_csi_rx_cb` → `esp_wifi_set_csi(true)`,
partindo da config que o driver já tem. Tipos de quadro ligados: L-LTF
(legado, beacons), HT20, HT40 e HE SU (802.11ax, o que um roteador Wi-Fi 6
manda ao C6), com `acquire_csi_he_stbc = 2`. No C6, `CONFIG_SOC_WIFI_MAC_VERSION_NUM`
é 2, então a struct é a variante sem `acquire_csi_force_lltf`/`vht`
(`esp_wifi_he_types.h`).

O `farejo info` mostra o diagnóstico: erro, etapa em que parou (1 config,
2 callback, 3 ligar) e se está ligado. `ESP_FAIL` ali significa sempre
"Wi-Fi iniciado com `csi_enable` 0".

### De onde vem o CSI (e por que vinham só 6 em 10 s)

Com o CSI ligado, o primeiro teste deu **6 callbacks em 10 s**, e não perto de
100. O driver só chamou o callback 6 vezes. Com o ping como fonte, foram 110
callbacks em 10 s, todos HT.

O CSI é medido nos campos de treino **OFDM** do preâmbulo (L-LTF, HT-LTF,
HE-LTF). Em 2,4 GHz, o beacon do roteador costuma sair a 1 Mbps (11b, DSSS),
que não tem nenhum deles e por isso não gera CSI. Sem modo promíscuo, o C6 só
recebe os quadros endereçados a ele, e com o Wi-Fi parado eles quase não
existem: os 6 records foram as mensagens MQTT que chegaram durante o teste.

Para ter um fluxo constante, alguém precisa **provocar tráfego OFDM para o
C6**. O exemplo oficial (esp-csi, `csi_recv_router`) pinga o roteador sem
parar, e o `radio csi` faz o mesmo: ping ao gateway a 10 Hz durante a janela.
Cada resposta é um quadro HT/HE do roteador para o C6. Para CSI de **outros**
aparelhos, o modo promíscuo (farejo) precisa estar ligado.

### O que vem em cada record

`c6_csi_cab_t` (16 B) + os bytes do CSI. O byte `flags` traz o que muda a
leitura dos bytes:

- bits 4..7: o formato do quadro (`cur_bb_format` do driver: 0 11b, 1 11g,
  2 HT, 4 HE SU...), porque cada formato traz um conjunto diferente de
  subportadoras;
- `C6_CSI_F_HT40`: há canal secundário;
- `C6_CSI_F_W0_RUIM`: os 4 primeiros bytes são inválidos. É o
  `first_word_invalid` do driver, um limite do hardware do C6; quem ignora
  essa flag lê duas subportadoras de lixo;
- `C6_CSI_F_CORTADO`: o driver entregou mais que `C6_CSI_MAX`.

`C6_CSI_MAX` é 1024 B, porque `rx_channel_estimate_len` tem 10 bits (até
1023). O primeiro firmware cortava em 256 B, e todo record do teste veio com
exatamente 256: estava perdendo dado.

Limites práticos:
- Em modem sleep o C6 desliga o receptor entre beacons; o `radio csi` mantém o
  Wi-Fi acordado durante o teste (`tab5_wifi_manter_acordado`).
- `min_intervalo_ms` limita a taxa no C6 para não inundar o SDIO. O limite anda
  por **cadência** (passos fixos, aceita o primeiro quadro de cada passo), e
  não por "intervalo desde o último": com a fonte no mesmo ritmo do limite, o
  atraso variável do ar descartava quase metade (64 records de 110). A fonte do
  `radio csi` roda ao dobro do limite (20 Hz) pelo mesmo motivo.

## Armadilha 2: o 802.15.4 derrubava o Wi-Fi

**Sintoma.** `ping` ao roteador dava 4/4 antes de `radio 154` e 0/4 depois. O
C6 continuava associado, mas os dados não passavam, e o rádio 802.15.4 não
recebeu nenhum quadro.

Eram **duas** causas, achadas uma de cada vez: a primeira correção (a
coexistência) fez o Wi-Fi passar dados **com** o 802.15.4 ligado (ping 4/4 no
meio do teste), mas ele ainda morria **depois** de desligar. Daí o teste do
`radio 154` medir antes, durante e depois — são falhas em momentos diferentes.

### Causa 1 (ligado): a coexistência

Wi-Fi, BLE e 802.15.4 dividem **um** caminho de RF no C6, e quem reparte o
tempo entre eles é o árbitro de coexistência. Ele só leva o 802.15.4 em conta
depois de `esp_coex_wifi_i154_enable()`, e o firmware não chamava essa função.

**Prova, nas fontes do IDF:**
- `components/esp_coex/src/coexist.c`: `esp_coex_wifi_i154_enable()` faz
  `coex_enable()` + `esp_coex_ieee802154_status_enable()`.
- O driver 802.15.4 (`components/ieee802154/`) **nunca** chama
  `esp_coex_ieee802154_status_enable`: ele só ajusta as prioridades (PTI) por
  evento. Quem liga o aviso de status é a aplicação.
- Os dois exemplos oficiais de "Wi-Fi + 802.15.4 num C6 só" chamam a função na
  aplicação: `examples/openthread/ot_br/main/esp_ot_br.c` e
  `examples/zigbee/esp_zigbee_gateway/main/esp_zigbee_gateway.c`.

**Correção:** `esp_coex_wifi_i154_enable()` antes do primeiro
`esp_ieee802154_enable()`, e `esp_coex_ieee802154_status_enable()` a cada vez
que volta a ouvir.

Com a coexistência ligada, o `esp_ieee802154_receive()` põe o rádio na cena
`RX`, que pede o rádio com a prioridade `txrx` da `s_coex_config`
(`driver/esp_ieee802154_util.c`; `IDLE` é só para o MAC parado). O Wi-Fi passa
na frente quando tem tráfego, e o 802.15.4 ouve nas sobras. No teste, a
latência do ping ao roteador subiu de 5 ms para 48 ms com o 802.15.4 ouvindo,
mas nenhum pacote se perdeu.

### Causa 2 (ao desligar): `disable` no meio de uma recepção

```c
esp_err_t esp_ieee802154_disable(void)   // components/ieee802154/esp_ieee802154.c
{
    esp_btbb_disable();
    ieee802154_rf_disable();   // esp_phy_disable(PHY_MODEM_IEEE802154)
    ieee802154_disable();      // modem_clock_module_disable + estado DISABLE
    return ieee802154_mac_deinit();
}
```

`ieee802154_disable()` corta o clock do MAC e troca o estado **sem
`stop_current_operation()`**. Com o receptor sempre ligado (`rx_when_idle`), o
MAC está em `RX` e com o pedido de rádio feito ao árbitro, e é congelado
assim. Os clocks shareds não são o problema: `modem_clock` conta
referências, e o Wi-Fi e o BLE seguram as deles.

O driver tem o caminho limpo: `esp_ieee802154_sleep()` faz
`stop_current_operation()` (manda `CMD_STOP` ao MAC), entra em `SLEEP` e solta
o RF por `esp_phy_disable`. É o que o OpenThread usa o tempo todo com o Wi-Fi
ligado (`esp_openthread_radio.c`); o `disable` dele só roda ao desmontar a
pilha inteira.

**Correção:** desligar = `esp_ieee802154_set_rx_when_idle(false)` (senão o
driver volta a ouvir sozinho) + `esp_ieee802154_sleep()` +
`esp_coex_ieee802154_status_disable()`. **`esp_ieee802154_disable()` nunca é
chamado.** O driver fica habilitado e dormindo; parado, o MAC custa só o
clock dele, e voltar a ouvir é um `esp_ieee802154_receive()`. O `farejo info`
mostra o estado: nunca ligado, ouvindo ou dormindo.

Ligar e desligar acontecem em tarefas diferentes (o RPC do host liga, o vigia
de 1 s desliga o que o host largou), e uma trava (`s_radio_mtx`) impede as
duas de mexerem no driver ao mesmo tempo.

**O que esperar** — tabela de coexistência do IDF para o ESP32-C6
([api-guides/coexist](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c6/api-guides/coexist.html)):

| 802.15.4 | Wi-Fi STA varrendo | Wi-Fi STA conectando | Wi-Fi STA conectado |
|---|---|---|---|
| Scan / Router (receptor sempre ligado) | C1 (instável) | C1 (instável) | **Y (estável)** |
| End Device | Y | Y | Y |

O modo do M5tabOS (só ouvir, receptor sempre ligado) equivale a "Router": com o
Wi-Fi conectado é suportado e estável, mas o 802.15.4 perde quadros quando o
Wi-Fi está ocupado. A própria Espressif recomenda dois chips com antenas
separadas para gateway Thread/Zigbee de verdade; no Tab5, o 802.15.4 serve para
ouvir e medir, não para ser nó de rede.

O 802.15.4 fica **só ouvindo**: o ACK automático é desligado
(`ieee802154_pib_set_auto_ack_tx(false)`, API privada do driver), senão em modo
promíscuo o C6 confirmaria quadros alheios e enganaria a rede vizinha.

**Teste.** `radio 154 [canal] [s]` pinga o roteador antes, no meio da janela
(com o 802.15.4 ligado) e depois. `DURANTE` em 4/4 confirma a coexistência, e
`DEPOIS` em 4/4 confirma o desligamento. Zero quadros sem Zigbee/Thread por
perto é normal. Medido com as duas correções: 4/4 nos três, latência de 12 ms
antes, 19 ms durante e 7 ms depois.

## FTM: medida de distância — limite do chip

**Sintoma.** `radio ftm` devolvia `status 255`. Esse valor não vem do driver
(os status dele vão de 0 a 6): é o código do firmware para "o
`esp_wifi_ftm_initiate_session` recusou". Com o motivo exposto, o erro é
`ESP_ERR_NOT_SUPPORTED`.

**Causa — errata do silício, não configuração.** No binário,
`esp_wifi_ftm_initiate_session` só devolve `ESP_ERR_NOT_SUPPORTED` em dois
casos: Wi-Fi em modo AP ("FTM initiator needs Station mode!") ou
`ftm_is_initiator_supported()` falso ("FTM Initiator mode not supported!"),
que lê o bit 2 do `feature_caps`. O C6 inicia o Wi-Fi com esse bit ligado (301,
acima), mas o `esp_wifi_init` do IDF o apaga antes de chegar ao driver:

```c
/* components/esp_wifi/src/wifi_init.c */
#ifdef CONFIG_ESP_WIFI_FTM_ENABLE
    esp_chip_info_t info = {0};
    esp_chip_info(&info);
    if (info.model == CHIP_ESP32C6 && info.revision <= 1) {
        ((wifi_init_config_t *)config)->feature_caps &= ~(CONFIG_FEATURE_FTM_INITIATOR_BIT);
    }
#endif
```

O **ESP32-C6 das revisões v0.0 e v0.1 não faz FTM initiator**; a Espressif
desliga no próprio IDF. O C6 deste Tab5 é de uma dessas revisões. Não há
contorno honesto: religar o bit à força daria medidas de tempo de voo erradas.
O lado responder (o C6 servir de referência para outro aparelho medir) não é
afetado.

**No firmware:** o C6 só anuncia `C6_CAP_FTM` se a revisão permitir
(`caps_reais()`), o INFO traz a revisão do chip (`chip_rev`), e o `radio ftm`
explica o motivo quando o driver recusa. A recusa síncrona tem outras saídas
possíveis no binário — `ESP_ERR_INVALID_ARG` (`frm_count` fora de
0/8/16/24/32/64, `burst_period` 1 ou acima de 255, canal fora do domínio
regulatório), `ESP_FAIL` (interface STA não criada), `ESP_ERR_NO_MEM` — e o
nome do erro sempre aparece. Um roteador que não fala FTM **não** causa
recusa síncrona: a sessão começa e termina com status 1 no
`WIFI_EVENT_FTM_REPORT`.

## Quem usa o rádio no P4

| Serviço | Mensagem | Quem usa |
|---|---|---|
| `tab5_sniffer` | `C6_MSG_FAREJO` (lotes) | Radar (alvos WiFi), console `farejo` |
| `tab5_csi` | `C6_MSG_CSI` | Radar (movimento), console `radio csi` |
| console | `C6_MSG_FTM`, `C6_MSG_154` | testes `radio ftm` / `radio 154` |

**Um ouvinte por mensagem.** O dispatcher do peer_data no host
(`eh_host_feat_peer_data_dispatch.c`) guarda uma entrada por `msg_id`, e
`eh_host_peer_data_register` com um `msg_id` já registrado **substitui** o
ouvinte. Por isso cada mensagem tem um dono só (um serviço), e quem mais
quiser os dados pede ao serviço (`tab5_farejo_ouvinte`, `tab5_csi_ouvinte`).

**Movimento pelo CSI** (`main/services/tab5_csi.cpp`). Usa as 52 subportadoras
válidas do L-LTF (presente em 11g e HT). Para cada quadro: amplitude por
subportadora, normalizada pela média do quadro (o AGC do C6 muda o ganho de
quadro para quadro); desvio médio contra a média móvel do próprio canal
(~1 s). O "parado" de cada lugar é aprendido nos primeiros ~3 s e segue
devagar; o nível sai da razão entre o desvio atual e o parado (2× leve,
4× forte, com histerese). O Tab5 se mexendo muda o CSI tanto quanto uma
pessoa: quem usa avisa (`tab5_csi_aparelho_mexendo`), e a referência
recomeça quando ele para.

**Alvos WiFi no Radar.** Cada registro do lote do farejo (100 ms) entra na
fila do Radar com o horário do meio do lote e o RSSI médio daquele MAC, e casa
com o rumo do giroscópio como um pacote BLE. O farejo não salta de canal:
associado, trocar de canal derruba o WiFi, e no modo só rádio o canal é o do
ESP-NOW.

## Contrato P4 ↔ C6

Mora em `shared/tab5_c6.h`, incluído pelos dois projetos.

- **Teto de 8 handlers** do `peer_data` no C6
  (`EH_CP_FEAT_PEER_DATA_MAX_HANDLERS`), e a tabela está cheia (msg_id 1..8).
  Um nono registro falharia calado. Comando novo do P4 para o C6 entra como
  chave de `C6_MSG_HOST_AJUSTE` ou sub-comando de `C6_MSG_HOST_RADIO`, nunca
  como msg_id novo. O sentido C6 → P4 não tem esse teto.
- **Capacidades.** O PRONTO e o INFO levam `caps`; cada bit (`C6_CAP_CSI`,
  `C6_CAP_FTM`, `C6_CAP_154`...) só acende se o módulo foi compilado. O P4
  esconde o que a gravação não trouxe.
- **Tudo dorme até o P4 pedir**, e CSI, farejo e 802.15.4 desligam sozinhos se
  o P4 parar de renovar (`C6_RADIO_VALIDADE_MS`, `C6_FAREJO_VALIDADE_MS`). FTM
  é disparo único.
- **Callbacks do rádio não falam com o host.** O `eh_cp_feat_peer_data_send`
  pode bloquear no RPC; chamado de dentro de um callback do Wi-Fi, trava a
  stack inteira. Os callbacks só escrevem num **ring buffer** de 12 KB
  (`s_saida`), e a tarefa `c6saida` envia. Ring e não fila de itens fixos: as
  mensagens vão de 7 B a ~1 KB, e com fila cada item custaria o tamanho máximo
  e seria montado na pilha da tarefa de Wi-Fi. No ring o produtor reserva o
  tamanho exato (`xRingbufferSendAcquire`) e escreve direto nele. O callback
  do 802.15.4 roda na ISR do rádio e usa `xRingbufferSendFromISR`. Ring cheio
  descarta, e o `farejo info` mostra quantas mensagens se perderam.

## Gravar o C6

O C6 não tem USB próprio: grava-se pelo conector de programação da placa, com
o Tab5 **desligado** (o gravador alimenta o C6). O passo a passo, o backup e a
volta ao firmware de fábrica estão no [README](README.md#firmware-do-esp32-c6-esp-now-e-bluetooth).

```bash
c6/flash.sh   # acha a porta sozinho; PORTA=/dev/xxx c6/flash.sh escolhe outra
```

O firmware tem duas redes de segurança: rollback do bootloader e uma carência
de 120 s no próprio app — se o P4 não confirmar o enlace nesse prazo, o C6
aponta o boot de volta para a imagem anterior.

**Mudar só o lado do P4 não exige reabrir o aparelho.** A correção do CSI é
desse tipo. Só mudanças em `c6/` precisam do gravador.

### Ao mexer no `sdkconfig.defaults` do C6

O `sdkconfig` existente tem precedência sobre o `sdkconfig.defaults`: mudar
uma chave no defaults não muda um `sdkconfig` já gerado. Apague o
`c6/sdkconfig` antes de compilar e confira com `grep` se a chave apareceu.
Chave desconhecida no defaults é descartada sem aviso.

## Roteiro de verificação depois de gravar

1. `farejo info` — `contrato v3`, capacidades `CSI s | FTM s | 802.15.4 s`.
2. `ping` — roteador e internet respondendo.
3. `radio csi 10` — perto de 100 records (1 a cada 100 ms), contagem por
   formato (HT/HE com roteador Wi-Fi 4/6), nenhum cortado; no `farejo info`,
   CSI `OK` e 0 mensagens descartadas. Com um roteador 802.11n, cada record HT20
   tem 256 B (L-LTF 128 + HT-LTF 128).
4. `radio ftm` — sessão feita (status 0 a 6, vindo do driver), ou o nome do
   erro que impediu o início. Num C6 v0.0/v0.1, `ESP_ERR_NOT_SUPPORTED` é o
   esperado (ver [FTM](#ftm-medida-de-distância--limite-do-chip)).
5. `radio 154 15 20` — `WiFi ANTES`, `DURANTE` e `DEPOIS` em 4/4; o
   `farejo info` depois diz 802.15.4 "dormindo".
6. `ping` de novo — o Wi-Fi segue normal depois de tudo.
7. `farejo on` / `farejo` / `farejo off` — MACs aparecendo em lotes.
