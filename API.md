# API do M5tabOS

A referência do que o sistema oferece a um aplicativo: a função, o que ela faz
e **de qual tarefa ela pode ser chamada**. O app [`Exemplo`](main/app_example/app_example.cpp)
usa cada uma destas áreas num cartão próprio. Abra-o no aparelho e leia o
código lado a lado.

Para o *como pensar* (as regras de desenho, o porquê de cada uma, os erros que
já cometemos) o guia é o [WRITING_APPS.md](WRITING_APPS.md). Este arquivo
é o *o quê*.

---

## Por que passar pela API

Nada impede um app de falar direto com o hardware: `M5.Imu`, `Wire`, `fopen`,
`xTaskCreate`, `M5.Display.fillRect` em tela cheia. Funciona na bancada e
quebra no uso, longe da causa. Cada API abaixo existe porque o caminho direto
já quebrou assim:

| caminho direto | o que acontece | a API |
|---|---|---|
| ler sensor pelo I2C interno de outra tarefa | o `I2C_Class` do M5Unified não tem trava: duas tarefas intercalam e leem o **registrador errado, sem erro** | `tab5_imu_ultima`, `tab5_power_ultima` (cópia, sem I2C) |
| desenhar de uma tarefa do app | o LovyanGFX não é seguro para concorrência: ponteiro corrompido, *exception* | mude o dado, chame `sistema_pedir_redesenho()` |
| `fopen(..., "a")` para gravar | com o cartão emprestado ao computador (modo pendrive), corrompe o FAT ou perde o dado | `tab5_sd_anexar` / `_acrescentar` / `_substituir` |
| `xTaskCreate` + `vTaskDelete` | a tarefa morta segurando um semáforo deixa o recurso preso para sempre | `tarefa_app_criar` / `tarefa_app_encerrar` |
| `M5.Speaker.tone()` | o codec é desligado ocioso; o primeiro bip sai **mudo** | `sistema_beep` |
| `fillRect` grande | rajada na PSRAM, o painel fica sem dado e a tela **pisca azul** | `tab5_tela_limpar` |
| ligar hardware no `ao_abrir` com `tab5_power_*` | desliga de baixo de outro app ou fica ligado depois de fechar | `app_t.perfil` (o sistema soma e desfaz) |

**A regra das três tarefas**, que decide quase todas as colunas "de onde" abaixo:

- **interface**: a única que desenha e a única que fala no I2C interno. É nela
  que rodam `ao_abrir`, `ao_fechar`, `desenhar`, `ao_tocar*`, `ao_teclar`,
  `ao_aviso`. Nada nela pode bloquear (cartão, rede, espera longa).
- **tarefas do app** (`tarefa_app`), **worker** (`sistema_trabalho`), **fundo**
  (`background.h`) e **console**: fazem o trabalho lento, mudam dado (atômico, fila
  ou com trava) e pedem redesenho.
- **serviços do sistema** (Mesh, ESP-NOW, WiFi, GPS...): têm tarefa própria e
  entregam por cópia (`tab5_*_msg`, `tab5_*_posicao`) e por aviso (`AVISO_*`).

Na coluna **de onde**: **UI** = só da tarefa de interface; **qualquer** =
qualquer tarefa; **fora da UI** = qualquer uma, menos a interface (bloqueia).

---

## 1. O app: `kernel/app.h`

Um app é uma pasta `main/app_<nome>/` que define `extern const app_t APP_<NOME>`.
O build gera a lista sozinho (`main/CMakeLists.txt`). Não há tabela para editar.

| campo | para quê |
|---|---|
| `nome`, `cor`, `icone` | identidade na tela inicial (`icone` opcional: desenha o seu) |
| `perfil` | o hardware que **este** app precisa ligado (`imu`, `alto_falante`, `microfone`, `camera`, `grove_5v`, `usb_a_5v`). O resto fica desligado com ele na tela |
| `usa_sd` / `exige_sd` | o sistema monta o cartão antes do `ao_abrir` / não abre o app sem o cartão |
| `orientacao` | `ORI_EM_PE`, `ORI_DEITADO`, `ORI_SEGUE_IMU` |
| `piso_repouso_mhz` | clock mínimo com a tela apagada (0 = o mínimo) |
| `ao_abrir` / `ao_fechar` | criar / destruir **tudo**. App fechado custa zero |
| `ao_repouso` / `ao_retomar` | soltar e religar hardware que não convive com o painel desmontado (DMA da câmera) |
| `desenhar(tela, area)` | a única função que desenha |
| `ao_tocar(x, y)` | clique (na soltura), coordenadas relativas à área |
| `ao_tocar_nivel(x, y, pressionado)` | a cada ciclo, com o dedo na tela (jogo, arraste, lista) |
| `ao_teclar(texto, codigo, mod)` | teclado físico já decodificado (UTF-8) |
| `periodo_ms` / `periodo_agora` | redesenho periódico (prefira 0 e avisos) |
| `avisos` / `ao_aviso` | máscara de `AVISO_*`: o serviço avisa, o app redesenha na hora |
| `altura_conteudo` | maior que a área: o sistema rola a área inteira |
| `ao_girar` | a tela girou (`ORI_SEGUE_IMU`) |
| `comandos` | comandos de console do app (seção 12) |

---

## 2. Redesenho e geometria: `kernel/app.h`

| função | de onde | o que faz |
|---|---|---|
| `sistema_pedir_redesenho()` | qualquer | um **valor** mudou: redesenha no próximo ciclo |
| `sistema_pedir_redesenho_completo()` | UI | a **tela** mudou (página, layout): limpa e repinta sem piscar |
| `sistema_area_foi_limpa()` | UI, no `desenhar` | `true` = a área chegou limpa (abertura, volta do repouso): repinte tudo |
| `sistema_area()` | UI | a área do app; **nunca** recalcule com constantes de barra |
| `sistema_quadro_dt()` | UI, no `desenhar` | segundos que este quadro representa (animação por tempo) |
| `sistema_avisar(AVISO_*)` | qualquer | publica um aviso (serviço → apps que o declararam) |
| `sistema_acordar_interface()` | qualquer | tira a interface da espera do repouso agora |
| `sistema_zerar_rolagem()` / `sistema_travar_rolagem(b)` | UI | rolagem do sistema (`altura_conteudo`) |
| `sistema_superficie(r, &s)` / `_mudou(r)` | UI | buffer RGB565 para quem escreve imagem por DMA (câmera) |
| `sistema_app_atual()`, `sistema_bloqueado()` | qualquer | quem está na tela; tela de bloqueio na frente |

Avisos existentes: `AVISO_MESH_MSG`, `AVISO_MESH_ENVIO`, `AVISO_MESH_NOS`,
`AVISO_GPS`, `AVISO_TRILHA`, `AVISO_TRABALHO`, `AVISO_ESPNOW`.

> **Exemplo:** todo cartão chama `sistema_pedir_redesenho()` depois de mudar
> dado; `.avisos` declara `AVISO_TRABALHO | AVISO_ESPNOW | ...`.

---

## 3. Desenho

### `tab5_ui.hpp`: componentes prontos

| função | o que faz |
|---|---|
| `tab5_fonte_corpo(d)`, `tab5_fonte_corpo_negrito(d)`, `tab5_titulo(d, px)` | fontes suaves (Roboto com acento) |
| `tab5_texto(d, n)` | tamanho N (altura N×8 px) na altura nativa mais próxima |
| `tab5_botao(d, x, y, l, a, txt, cor)` | o botão do sistema; salva e devolve o estilo de texto |
| `tab5_celula(d, cx, y, rotulo, valor, cor, sub)` | rótulo pequeno + valor grande |
| `tab5_dentro(x, y, rx, ry, rl, ra)` | teste de toque num retângulo |
| `tab5_cortar_ao_caber` / `_com_reticencias` | corta texto UTF-8 até caber em N px |
| `tab5_linha_grossa(d, ...)` | linha suave que respeita o recorte (use no lugar de `drawWideLine`) |
| `tab5_sprite_novo` / `tab5_sprite_soltar` | sprite sempre na PSRAM (ou `nullptr`) |
| `SIS_FUNDO`, `SIS_LINHA`, `SIS_BORDA` | a paleta do sistema (`app.h`) |

### Pintura por diferença: `kernel/paint.hpp`

`pintura_iniciar(&p)` no começo do `desenhar`; `pintura_completa(&p)` para o
que nunca muda; `pintura_mudou_i/_s(&p, &memoria, agora)` para cada valor. Não
encadeie comparadores com `||` (veja o guia, 2.3).

### Limpar: `hal/tab5_screen.hpp`

`tab5_tela_limpar(x, y, l, a, cor)` (UI): limpa em faixas, sem piscar azul. Use
para qualquer área que **pode** passar de ~40 linhas da tela.

### Listas: `kernel/list.hpp`

A lista rolável do sistema: mede cada item uma vez, rola deslocando pixels e
repinta só o item que mudou (`lista_item_mudou`). O Exemplo inteiro é uma lista.

```c
lista_definir(&l, ROLAGEM_INICIO, n, altura_de, desenhar_item, ctx);   // ao_abrir
lista_desenhar(&l, d, area, TFT_BLACK, sistema_area_foi_limpa());       // desenhar
if (lista_arrastar(&l, x, y, pressionado)) sistema_pedir_redesenho();   // ao_tocar_nivel
int i = lista_item_tocado(&l, x, y, &dy);   // ao_tocar; <0 = nada ou fim de arraste
```

### Conversa e teclado virtual: `kernel/conversationtion.hpp`, `kernel/virtual_keyboard.hpp`

`conversa_t` é a tela de mensagens pronta (balões, histórico, barra de escrita,
teclado); o app só diz como contar, ler e enviar. `tecvirt_*` é o teclado na tela
para quem precisa só dele. Os apps Mesh e ESP-NOW usam os dois.

---

## 4. Toque e teclado: `kernel/app.h`, `hal/tab5_keyboard.hpp`

| função | de onde | o que faz |
|---|---|---|
| `sistema_toques(out, max)` | UI, no `ao_tocar_nivel` | todos os dedos da amostra (multitoque) |
| `sistema_consumir_toque()` | UI | o toque já foi tratado por nível: a soltura não vira clique |
| `sistema_travar_navegacao(b)` | UI | a faixa de navegação vira do app (teclado virtual) |
| `tab5_teclado_presente()` | qualquer | teclado físico encaixado |
| `tab5_teclado_ascii(txt, out, n)` | qualquer | texto sem acento, para protocolos ASCII |

As teclas chegam por `ao_teclar`. Códigos especiais: `TAB5_TECLA_ENTER`,
`_APAGAR`, `_DEL`, `_ESC`, `_TAB`, `_ESQUERDA`, `_DIREITA`, `_CIMA`, `_BAIXO`;
modificadores `TAB5_TECLA_CTRL/SHIFT/ALT`.

---

## 5. Trabalho fora da interface

| API | quando | de onde |
|---|---|---|
| `tarefa_app_criar(&t, corpo, nome, pilha, prio)` / `tarefa_app_encerrar(&t, ms)` (`kernel/app_task.h`) | laço contínuo enquanto o app está aberto | UI (`ao_abrir`/`ao_fechar`) |
| `sistema_trabalho(fn, arg)` (`kernel/work.hpp`) | uma operação lenta (ler arquivo, montar lista); fim chega por `AVISO_TRABALHO` | qualquer |
| `sistema_trabalho_cancelado()` | dentro do trabalho: o app fechou, pare | worker |
| `sistema_trabalho_solto(fn, arg)` | segundos de trabalho que não podem segurar o fechamento (gravar foto); `arg` é dele | qualquer |
| `sistema_fundo_iniciar/parar/ativo(&def)` (`kernel/background.h`) | trabalho que **continua com o app fechado** (gravar trilha, cronômetro) | UI |
| `fundo_esperar(f, ms)`, `fundo_ceder(f)`, `fundo_estado(f, n, txt)` | dentro do laço de fundo | fundo |

Regras do laço (as três): nunca espere com `portMAX_DELAY` (o pedido de parada
precisa chegar); não desenhe; memória sua, nunca da janela.

> **Exemplo:** cartões TAREFA (tarefa + fila de 1), TRABALHO (conta arquivos e
> recebe `AVISO_TRABALHO`) e FUNDO (cronômetro que aparece na barra com o app fechado).

---

## 6. Cartão SD: `hal/tab5_sd.hpp`

Pasta do app: `/sdcard/apps/<Nome>/`, criada sozinha.

| função | de onde | o que faz |
|---|---|---|
| `tab5_sd_anexar(app, arq, linha, cabecalho)` | qualquer | acrescenta uma linha **sem esperar** (gravação em lote) |
| `tab5_sd_acrescentar(app, arq, dados, n)` | fora da UI | acrescenta um bloco agora |
| `tab5_sd_substituir(app, arq, dados, n, flags)` | fora da UI | troca o arquivo inteiro (nunca deixa meio arquivo); `TAB5_SD_SIGILOSO` zera o antigo |
| `tab5_sd_abrir(app, arq, "r")` | fora da UI | **ler** (e arquivo novo grande, com `exige_sd`) |
| `tab5_sd_pasta(app, buf, n)` | fora da UI | caminho da pasta |
| `tab5_sd_montado()`, `tab5_sd_espaco(&t, &l)` | qualquer | estado |

As três portas de gravação guardam no diário da flash quando o cartão está no
computador e entregam na volta, na ordem. **Só reescreva o que leu.**

> **Exemplo:** cartão CARTÃO (`tab5_sd_anexar` com cabeçalho de CSV).

---

## 7. Notificações: `services/notification.h`

Estado, não fila: cada fonte tem **um** estado atual, e publicar de novo substitui.

```c
int id = notif_registrar("MeuApp");                 // idempotente pelo nome
notif_associar_app(id, "MeuApp");                   // tocar no aviso abre o app
notif_publicar(id, 3, NOTIF_ATENCAO, "3 alertas");  // qualquer tarefa
notif_limpar(id);                                   // = publicar 0
```

Níveis: `NOTIF_ESTADO` (atividade), `NOTIF_INFO`, `NOTIF_ATENCAO` (endereçado a
você), `NOTIF_URGENTE`. A barra pinta pela pior fonte ativa.

---

## 8. Energia, som e tela

| função | de onde | o que faz |
|---|---|---|
| `app_t.perfil` | (declaração) | o que o app precisa ligado; o sistema soma com a preferência do usuário |
| `sistema_reaplicar_perfil()` | UI | uma preferência mudou: vale na hora |
| `sistema_manter_acordado(SISTEMA_ACORDADO_APP, b)` | qualquer | a tela não apaga (o sistema solta ao fechar o app) |
| `tab5_power_ultima()` | qualquer | bateria: volts, mA, mW, % (cópia, sem I2C) |
| `sistema_beep(hz, ms)` | UI | bip (precisa de `perfil.alto_falante`) |
| `tab5_painel()`, `tab5_painel_nome()` | qualquer | qual das três telas do Tab5 (ILI9881C, ST7123, ST7121) |
| `tab5_power_cpu_mhz()` | qualquer | clock agora (360 MHz no P4 v1.x, 400 no v3.x) |

> **Exemplo:** cartões ENERGIA, SOM e ACORDADO.

---

## 9. Sensores e hora

| função | de onde | o que faz |
|---|---|---|
| `tab5_imu_ultima(&d)` (`hal/tab5_imu.hpp`) | qualquer | aceleração (g) e giro (°/s); precisa de `perfil.imu` |
| `tab5_gps_no_ar()`, `tab5_gps_posicao()`, `tab5_gps_tempo()` (`hal/tab5_gps.hpp`) | qualquer | GPS no Grove; lat/lon zerados sem fix |
| `tab5_gps_assinar(fila)` | qualquer | recebe cada sentença nova numa fila sua |
| `tab5_sensores_temp(&c)` (`hal/tab5_sensors.hpp`) | qualquer | temperatura interna |
| `tab5_hora_epoch()` (`services/tab5_time.hpp`) | qualquer | agora em UTC, ou 0 se o relógio não foi acertado |
| `tab5_hora_local(epoch, &tm)` | qualquer | para **mostrar** (fuso aplicado); guarde sempre UTC |
| `tab5_hora_fonte()` | qualquer | de onde veio o acerto (GPS, rede, malha, RTC) |
| `tab5_agora_ms()` | qualquer | relógio monótono para medir intervalo (compare por diferença) |

> **Exemplo:** cartões HORA, IMU e GPS.

---

## 10. Rede

### WiFi: `hal/tab5_wifi.hpp`

| função | de onde | o que faz |
|---|---|---|
| `tab5_wifi_estado()`, `tab5_wifi_descricao()` | qualquer | estado e texto pronto para a tela |
| `tab5_wifi_ssid()`, `tab5_wifi_ip()`, `tab5_wifi_rssi()` | qualquer | a conexão atual |
| `tab5_wifi_ligar()` / `_desligar()` | UI | normalmente é o usuário em Ajustes, não o app |
| `tab5_wifi_procurar()`, `tab5_wifi_vista(i, &v)` | qualquer | varrer redes em volta |
| `tab5_wifi_gravar_credenciais(ssid, senha)` | fora da UI | guarda rede (senha cifrada) |

Com o WiFi de pé, o resto é o ESP-IDF normal (`esp_http_client`, sockets, MQTT).

### ESP-NOW: `services/tab5_espnow.hpp`

Precisa do firmware nosso no C6 (`c6/`, ver [RADIO_C6.md](RADIO_C6.md)).

| função | de onde | o que faz |
|---|---|---|
| `tab5_espnow_pronto()`, `tab5_espnow_canal()` | qualquer | no ar, e em que canal |
| `tab5_espnow_enviar(texto)` | qualquer | difusão; não bloqueia |
| `tab5_espnow_n()`, `tab5_espnow_msg(i, &m)` | qualquer | histórico em RAM (cópia) |
| `tab5_espnow_janela(b)` | UI | a janela abriu/fechou: receptor ligado com o app na tela |

Aviso: `AVISO_ESPNOW`.

### Mesh (Meshtastic): `services/tab5_mesh.hpp`

Ver [MESHTASTIC.md](MESHTASTIC.md). Serviço do sistema: o rádio, os nós e o
histórico vivem nele, e o app é só janela.

| função | de onde | o que faz |
|---|---|---|
| `tab5_mesh_servico_ativo()`, `tab5_mesh_no_ar()` | qualquer | estado |
| `tab5_mesh_num_nos()`, `tab5_mesh_no(i, &no)` | qualquer | nós conhecidos (cópia) |
| `tab5_mesh_num_msgs()`, `tab5_mesh_msg(i, &m)` | qualquer | mensagens (0 = mais recente) |
| `tab5_mesh_nao_lidas()` | qualquer | não lidas |
| `tab5_mesh_enviar(para, canal, texto)` | qualquer | `para = 0xFFFFFFFF` difunde no canal; outro = direta |
| `tab5_mesh_estado_envio(id, &erro)` | qualquer | ACK, falha |

Avisos: `AVISO_MESH_MSG`, `AVISO_MESH_ENVIO`, `AVISO_MESH_NOS`.

### Bluetooth: `hal/tab5_ble.hpp`

`tab5_ble_pronto()`, `tab5_ble_varrer(ms)`, `tab5_ble_visto(i, &v)`. Quem liga e
desliga é o usuário (`sistema_bluetooth`).

> **Exemplo:** cartões WIFI, ESP-NOW (com botão de difundir) e MESH (só leitura:
> um app de exemplo não deve falar num canal público).

---

## 11. Câmera: `kernel/app.h`, `hal/tab5_camera.hpp`

| função | de onde | o que faz |
|---|---|---|
| `sistema_camera_iniciar(l, a)` / `sistema_camera_parar()` | qualquer | liga/desliga (contagem de referência: vários usuários convivem) |
| `tab5_camera_definir_destino(&d)` | UI | onde cada quadro é escrito (use `sistema_superficie`) |
| `tab5_camera_pedir_foto(buf)`, `tab5_camera_foto_pronta(&ok)` | qualquer | foto em resolução cheia |

A câmera e o painel dividem o mesmo DMA: solte a captura em `ao_repouso` e religue
em `ao_retomar`. O app Câmera é a referência completa.

---

## 12. Console do app: `app_t.comandos`

O console serial (`idf.py monitor`, digite `help`) é do sistema, mas um app pode
oferecer comandos próprios. Eles aparecem num grupo **APPS** do `help` e são
chamados sempre com o nome do app na frente, para nunca colidir:

```
app exemplo contar 5
```

```c
static void cmd_contar(int argc, char** argv) { ... }   // argv[0] = "contar"

static const app_comando_t COMANDOS[] = {
  { "contar", "[n]\tsoma n no contador", cmd_contar, false },
  { "zerar",  "\tzera (so com o app aberto)", cmd_zerar, true },
  { nullptr },
};
// no app_t:  .comandos = COMANDOS,
```

O comando roda na **tarefa do console**: valem as regras de qualquer tarefa que
não é a interface (não desenhe, mude dado atômico ou com trava, peça redesenho).
`so_aberto = true` faz o sistema recusar o comando com o app fechado, e é o
certo para todo comando que toca estado criado no `ao_abrir`.

---

## 13. Hardware: o que muda entre aparelhos

O mesmo binário roda nas **três telas** do Tab5 (o M5GFX detecta). O sistema
ajusta sozinho o que difere: vigia do toque, repouso e relógio do painel (veja
`tab5_painel()` em `hal/tab5_power.hpp`). Um app que desenha pelo `M5.Display`
e lê o toque pelo `ao_tocar` não precisa saber qual é.

O **chip** é outra história: o ESP32-P4 v1.x e o v3.x pedem binários
diferentes (`sdkconfig.defaults.chip_v1` / `_v3`). O `./flash.sh` lê a
revisão do aparelho e escolhe. Para compilar à mão:

```bash
idf.py -B build_v3 -DM5_CHIP=v3 build
```

Código de app não deve depender da revisão. Se precisar mesmo, use
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3` (v1.x) do `sdkconfig.h`.
