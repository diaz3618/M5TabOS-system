# M5tabOS

Sistema operacional de aplicativos para o **M5Stack Tab5** (ESP32-P4 + ESP32-C6),
escrito em C++ sobre **ESP-IDF 5.5** e **FreeRTOS**, com interface própria em
M5GFX.

> *An app-based operating system for the M5Stack Tab5, built on ESP-IDF 5.5 and
> FreeRTOS. Documentation and code comments are in Portuguese.*

Versão atual: **0.7.0** (ver [CHANGELOG.md](CHANGELOG.md)).

## O que tem

**Sistema**
- Tela inicial com ícones, barra de status (Wi-Fi, Bluetooth, ESP-NOW,
  bateria, avisos) e barra de navegação com a hora.
- Compositor de tela com double buffer, sem tearing e sem piscada.
- Componentes padrão: lista rolável, componente de conversa (balões, teclado
  virtual, rolagem), botões e fontes suaves (Roboto em alturas nativas, com
  acentos).
- Toque com reconhecimento próprio, vigia de toque e diagnóstico pelo console.
- Energia: repouso da tela, perfis por app (cada app declara o hardware que
  usa), medição de bateria pelo INA226 embutido.
- Cartão SD: um único gravador do sistema, em lote, para todos os apps; logs
  diários no cartão.
- Multitarefa: apps com parte de fundo (continuam depois de fechar a janela),
  worker do sistema para leituras lentas, e registro de dono de cada
  tarefa.
- Console serial com diagnóstico (`help`).

**Rádio**
- **Wi-Fi** pelo ESP32-C6 (esp_hosted, SDIO).
- **Bluetooth LE**: a stack NimBLE roda no P4 e o controlador é o do C6.
- **ESP-NOW**: roda no C6, com firmware próprio (pasta `c6/`).
- **Meshtastic**: conversa com um nó (por exemplo o Unit C6L) **pelo cabo
  Grove ou por Bluetooth**; pode enviar a posição do GPS para a malha.

**Aplicativos do sistema**

| App | Para quê |
|---|---|
| Mesh | Mensagens Meshtastic: conversas, nós, estado da malha |
| ESP-NOW | Conversa direta entre aparelhos ESP32 |
| Câmera | Visualização ao vivo e foto em BMP no cartão |
| Arquivos | Navega pelo cartão: texto, CSV e imagens |
| Energia | Bateria, consumo e gráficos do dia |
| Tarefas | Tarefas do FreeRTOS, CPU e memória, agrupadas por dono |
| IMU | Acelerômetro e giroscópio |
| Ajustes | Brilho, volume, Wi-Fi, Bluetooth, ESP-NOW, Meshtastic, tela e mais |
| Exemplo | Um cartão para cada API do sistema: o molde para escrever um app |

Qualquer pasta `main/app_<nome>/` vira um app na tela inicial, sem registro em
lugar nenhum: o seu app mora ao lado destes.

## Hardware

- **M5Stack Tab5**, qualquer uma das três telas que ele já teve
  (ILI9881C + GT911, ST7123, ST7121). O M5GFX detecta e o sistema se ajusta;
  as telas ST71xx ainda não foram testadas em aparelho (veja *Limitações*).
- **Revisão do ESP32-P4**: v1.x (os Tab5 vendidos até aqui) e v3.x pedem
  binários diferentes. O `./flash.sh` lê a revisão do aparelho e compila o
  certo; à mão é `idf.py build` (v1.x) ou `idf.py -B build_v3 -DM5_CHIP=v3 build`
  (v3.x). As chaves de cada um estão em `sdkconfig.defaults.chip_v1` e `_v3`.
- Opcional:
  - cartão microSD em FAT32;
  - teclado do ExtPort1;
  - GPS ou nó Meshtastic no Grove.

## Compilar e gravar

1. Instale o **ESP-IDF 5.5** ([guia oficial](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/get-started/)).
2. Prepare o terminal e compile:

   ```bash
   source environment.sh
   idf.py build
   ```

   O `environment.sh` carrega `$IDF_PATH/export.sh` (padrão `~/esp/esp-idf`).
   Para um ambiente fora do padrão, crie `environment.local.sh`; ele não vai para
   o repositório.

3. Grave com o Tab5 ligado pelo USB-C:

   ```bash
   ./flash.sh
   ```

   O script acha a porta sozinho (`/dev/cu.usbmodem*` no macOS,
   `/dev/ttyACM*` no Linux) e abre o monitor serial. Para escolher outra
   porta: `PORTA=/dev/xxx ./flash.sh`.

### Firmware do ESP32-C6 (opcional)

**O sistema funciona com o rádio de fábrica, sem atualizar nada.** Wi-Fi,
internet, Meshtastic pelo cabo Grove ou pela internet e todos os apps rodam
direto. A atualização do C6 só acrescenta três coisas:

| Recurso | Rádio de fábrica | Rádio atualizado |
|---|---|---|
| Wi-Fi e internet | sim | sim |
| Meshtastic pelo Grove ou pela internet | sim | sim |
| ESP-NOW | não | sim |
| Bluetooth (e Meshtastic por Bluetooth) | não | sim |
| Farejo e CSI do rádio | não | sim |

Sem a atualização, as telas dessas funções dizem "rádio C6 de fábrica:
atualização opcional", e o resto segue normal.

Para quem quiser os três recursos: o firmware fica na pasta `c6/` (mesma
versão do esp_hosted do sistema, 3.0.9) e precisa de um gravador serial,
porque o C6 não tem USB próprio.

O C6 não tem USB próprio. É gravado pelo conector de programação da placa,
com um gravador serial (por exemplo, o M5Stack ESP32 Downloader):

1. **Tab5 desligado**; o gravador alimenta o C6.
2. Gravador ligado ao computador por **USB-A**. Com cabo USB-C para USB-C, o
   gravador fica sem energia.
3. Antes de tudo, faça um **backup do firmware de fábrica**:

   ```bash
   esptool.py -p /dev/xxx read_flash 0 0x400000 backup_c6.bin
   ```

4. Grave:

   ```bash
   c6/flash.sh
   ```

   O script acha a porta do gravador sozinho (`/dev/cu.usbserial-*` no macOS,
   `/dev/ttyUSB*` no Linux). Para escolher outra: `PORTA=/dev/xxx c6/flash.sh`.

5. Ligue o Tab5. No log de boot deve aparecer
   `host=3.0.9 coprocessor=3.0.9 (match)`.

Para voltar ao original, grave o backup com
`esptool.py write_flash 0 backup_c6.bin`, ou use a ferramenta oficial da
M5Stack.

O que roda no C6 (farejo, CSI, FTM, 802.15.4), quem decide a configuração do
Wi-Fi dele e o roteiro de verificação depois de gravar estão em
[RADIO_C6.md](RADIO_C6.md).

## Primeiro uso

- **Wi-Fi:** em Ajustes > Wi-Fi, toque PROCURAR REDES e escolha a rede. A
  senha é pedida na tela e guardada **cifrada** no cartão
  (`apps/Sistema/wifi.txt`), com uma chave derivada do chip de cada aparelho.
  Cabem várias redes: ao ligar, o sistema conecta na conhecida de sinal mais
  forte. Também dá para escrever o arquivo à mão (pares de linhas: rede e
  senha, `-` para rede aberta; a senha é cifrada no boot seguinte) ou usar o
  console: `wifi <rede> <senha>`.
- **Ajustes:** ligue o Wi-Fi, o Bluetooth e o Grove 5V, e escolha como falar
  com o nó Meshtastic.
- **Meshtastic pelo Bluetooth:** em Ajustes > Meshtastic, escolha BLUETOOTH.
  - Na primeira conexão o nó pede o **PIN**. O padrão testado é 123456, o dos
    aparelhos sem tela.
  - Se o nó recusar, o teclado abre sozinho para digitar o PIN que ele mostra.

## Escrevendo apps

A referência das APIs (tela, toque, tarefas, cartão, notificações, energia,
sensores, Wi-Fi, ESP-NOW, Meshtastic, câmera, console) está em
[API.md](API.md); o guia de como pensar um app, com os erros que já
cometemos, em [WRITING_APPS.md](WRITING_APPS.md). Para criar um app:

1. copie `main/app_example/` para `main/app_<nome>/`;
2. troque `APP_EXAMPLE` por `APP_<NOME>` (o nome segue a pasta);
3. declare o perfil de energia dele e apague os cartões que não usar.

Um app também pode responder a comandos do console serial
(`app <app> <comando>`), declarando `.comandos` no `app_t`.

A arquitetura do kernel está em [ARCHITECTURE.md](ARCHITECTURE.md), e a base de
tela em [COMPOSITOR.md](COMPOSITOR.md).

## Estrutura

```
main/kernel/      núcleo: laço de interface, compositor, componentes (lista, conversa, teclado)
main/hal/         hardware: tela, toque, energia, cartão, Wi-Fi, Bluetooth, GPS, câmera, IMU
main/services/    serviços: ajustes, notificações, log, Meshtastic, ESP-NOW, registro
main/app_*/       aplicativos
c6/               firmware do coprocessador ESP32-C6 (ESP-NOW + esp_hosted)
shared/    contrato de mensagens entre o P4 e o C6
tools/      geradores (fontes, emoji, tabela de hardware) e utilitários
```

## Limitações conhecidas

- As telas ST7123 e ST7121 e o ESP32-P4 v3.x compilam e seguem o caminho
  conservador (repouso só apaga a luz, relógio do painel de fábrica), mas
  ainda não foram testados num aparelho. Relatos são bem-vindos.
- A interface e a documentação estão em português, e o fuso horário é fixo
  (`TAB5_FUSO_S`, UTC−3) em `services/tab5_time.hpp`.
- A curva de bateria (`hal/tab5_power.hpp`) foi calibrada em uma unidade
  (1306,8 mAh medidos). Outras baterias podem pedir recalibração.
- O Bluetooth é só BLE: o C6 não faz Bluetooth clássico.
- Wi-Fi e ESP-NOW dividem um rádio de um canal só. Com o roteador conectado, o
  ESP-NOW fica no canal dele.

## Licença

[MIT](LICENSE). Os componentes de terceiros têm licença própria: ver
[THIRD_PARTY.md](THIRD_PARTY.md).
