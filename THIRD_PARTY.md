# Componentes de terceiros

O código do M5tabOS está sob a licença MIT (ver [LICENSE](LICENSE)). Este
repositório também inclui ou usa as obras abaixo, cada uma com a sua licença.

## Incluídos no repositório

| Obra | Onde | Licença |
|---|---|---|
| **Roboto** (Google) | `tools/Roboto-*.ttf`; glifos gerados em `main/fonts/soft_fonts.cpp` | Apache License 2.0 |
| **Twemoji** (Twitter/X e colaboradores) | imagens convertidas em `main/tab5_emoji_dados.cpp` | CC-BY 4.0 |
| **Tabela de modelos de hardware do Meshtastic** | `main/services/tab5_mesh_hw_dados.cpp`, gerada de `meshtastic/protobufs` | ver nota abaixo |

**Twemoji:** Copyright 2020 Twitter, Inc and other contributors. Gráficos sob
CC-BY 4.0: https://creativecommons.org/licenses/by/4.0/ — fonte:
https://github.com/twitter/twemoji

**Roboto:** Copyright 2011 Google Inc. Licenciada sob a Apache License 2.0:
https://www.apache.org/licenses/LICENSE-2.0

**Meshtastic:** a tabela contém só os nomes e números do enum `HardwareModel`,
usados para interoperar com o protocolo. O arquivo de origem
(`meshtastic/protobufs`, GPL-3.0) não está incluído; a tabela é gerada por
`tools/generate_hw.py`.

## Baixados na compilação (gerenciador de componentes do ESP-IDF)

| Componente | Licença |
|---|---|
| ESP-IDF, esp_hosted, esp_wifi_remote, NimBLE (no ESP-IDF) | Apache License 2.0 |
| esp_video, esp_cam_sensor, esp_ipa, esp_h264, usb_host_uvc | Apache License 2.0 |
| M5GFX / LovyanGFX | MIT (LovyanGFX: FreeBSD) |
| M5Unified | MIT |
| TinyUSB (espressif/tinyusb, modo pendrive) | MIT |

A lista exata, com versões, está em `dependencies.lock`.

## Não incluído

O firmware de fábrica do ESP32-C6 (da M5Stack) não faz parte deste
repositório e não deve ser redistribuído. O README mostra como fazer um
backup dele antes de gravar o firmware da pasta `c6/`.
