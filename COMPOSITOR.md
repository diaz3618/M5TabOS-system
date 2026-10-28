# M5tabOS — a nova base de tela (compositor estilo LVGL)

Decidido. Não vamos adotar a LVGL: vamos construir a **nossa**
base, com os mesmos princípios que fazem a LVGL ser suave, dentro do nosso
contexto (M5GFX como rasterizador, FreeRTOS, perfis de energia, Tab5).

A regra que orienta tudo daqui em diante:

> **Problema de tela é problema de SISTEMA.** Se um app precisa de um
> `fillRect` extra, de um respiro, de um caso especial para não piscar ou não
> rasgar, a base está errada, não o app. Nenhum puxadinho por app.

---

## 1. Por que a base atual não tem conserto por remendo

Hoje o M5GFX escreve **direto na página que o painel está varrendo**. Tudo o
que existe em volta disso são defesas contra essa decisão:

| remendo de hoje | contra o quê |
|---|---|
| pintar por diferença, largura fixa, `pintura_*` | apagar-e-redesenhar aparece como flash |
| `s_sujo_layout` × `s_sujo_dados`, `sistema_area_foi_limpa()` | o app não sabe quando a tela foi limpa |
| salvar/restaurar `getTextStyle()` (regra nº 2) | estado global de desenho vazando entre funções |
| `tab5_tela_limpar` em faixas, `respirar`, `tab5_tela_gastou` | rajada de escrita na PSRAM → underrun → frame azul |
| `tab5_tela_rolar` + `rolagem_item_preservado` | reaproveitar pixel para rolar mais barato |
| `ROLAR_RESPIRO_US`, "escrever mais rápido que o feixe" | rasgo na imagem durante a rolagem |
| `REPINTURA_MIN_MS = 80` | excesso de repinturas por toque |
| double buffer só para repintura completa | flash preto ao trocar de app |

Cada linha dessa tabela foi um bug real, corrigido do jeito certo **para a
base que tínhamos**. O problema é a base. A primeira tentativa (double buffer
só na janela das mensagens) mostrou o limite: o flip é de página inteira, e
sem saber **tudo** o que mudou no frame (barra de escrita, ícone do
LongFast), a outra página fica velha nesses pontos e pisca.

---

## 2. As seis regras (o que a LVGL faz, e o que vamos fazer)

1. **Nunca escrever na página que o painel está varrendo.** Todo desenho vai
   para a página de trás. A da frente só é lida pelo DMA do DSI.
2. **Uma lista GLOBAL de áreas sujas por frame.** Qualquer mudança (um texto,
   a bateria, a rolagem) *invalida* um retângulo. Depois do flip, copiamos
   para a nova página de trás **só esses retângulos** — é isso que faltou na
   primeira tentativa, e é o que torna o double buffer barata: o custo é proporcional ao que
   mudou, não à tela.
3. **Flip no limite do frame.** O driver do IDF já faz isso (o
   `cur_fb_index` só é lido quando o DMA reinicia, `esp_lcd_panel_dpi.c:75`).
   Esperamos `on_frame_buf_complete` antes de tocar na página que saiu.
4. **Um frame por ciclo.** Toque, dado novo e temporizador só *invalidam*. Um
   loop de frame junta tudo e desenha uma vez. Nada é desenhado "na hora" do
   evento. Sem nada sujo, não há frame — custo zero parado.
5. **Rolar é redesenhar recortado, não deslocar pixel.** A lista muda o
   deslocamento, invalida a própria área, e o frame seguinte a redesenha na
   página de trás. Sem fragmento possível, porque nada de antes é reaproveitado.
6. **Fonte no tamanho exato, com suavização.** Nada de escala fracionária de
   fonte bitmap (o `TAM_MSG 3.8`). Fontes geradas no tamanho de uso.

---

## 3. O que o hardware já oferece (verificado no IDF 5.5.5 instalado)

- **Dois framebuffers** já alocados (`num_fbs = 2` no Panel_DSI do M5GFX),
  720×1280 RGB565 = 1,84 MB cada, na PSRAM.
- **Flip por hardware**: `esp_lcd_panel_draw_bitmap()` com ponteiro dentro de
  um framebuffer só troca o índice (`esp_lcd_panel_dpi.c:506-524`).
- **Aviso de frame completo**: `esp_lcd_dpi_panel_register_event_callbacks()`
  com `on_frame_buf_complete` (e `on_vsync`).
- **PPA** (`esp_driver_ppa`): `ppa_do_fill`, `ppa_do_blend`,
  `ppa_do_scale_rotate_mirror` (com escala 1 = cópia de retângulo com passo).
  Copia e preenche por DMA2D, sem a CPU.
- **Prioridade no barramento AXI** (`hal/axi_icm_ll.h`): QoS por mestre,
  incluindo o DW_GDMA, que é o DMA que alimenta o DSI. O próprio driver aponta
  isso como o remédio do underrun (`esp_lcd_panel_dpi.c:113`). É a **causa
  raiz** do frame azul: hoje a CPU e o painel disputam a PSRAM em pé de
  igualdade; com QoS, a leitura do painel ganha.

---

## 4. A arquitetura

```
  eventos (toque, dados, timers)
        │  só invalidam
        ▼
 ┌──────────────────────┐
 │  ÁRVORE DE OBJETOS   │  telas, rótulos, botões, listas, imagens, canvas
 │  (kernel/ui_*)       │  mudar propriedade = invalidar a própria área
 └─────────┬────────────┘
           │ áreas sujas (lista global, fundidas)
           ▼
 ┌──────────────────────┐
 │  COMPOSITOR          │  1 quadro por ciclo, na tarefa de interface
 │  (kernel/quadro)     │  para cada área suja → fatias na SRAM interna
 └─────────┬────────────┘
           │ fatia pronta (SRAM) → cópia por DMA2D/PPA
           ▼
 ┌──────────────────────┐
 │  HAL DE QUADRO       │  página de trás ← fatias
 │  (hal/tab5_frame)   │  flip no limite do quadro
 └─────────┬────────────┘  espera on_frame_buf_complete
           │               sincroniza as áreas sujas na página que saiu
           ▼
        painel DSI (lê a página da frente, com prioridade no AXI)
```

### 4.1 Renderizar na SRAM, não na PSRAM

Esse é o ganho de banda que acaba com a luta contra o frame azul. Hoje um
balão de mensagem escreve na PSRAM três vezes: o fundo, o balão por cima e o
texto por cima do balão. Com fatias:

- o compositor aluga uma **fatia** na SRAM interna (ex.: 720 × 32 linhas ×
  2 B = 46 KB; duas fatias para alternar);
- desenha nela tudo o que cai naquela faixa da área suja, na ordem das
  camadas (sobreposição acontece na SRAM, que é rápida e não disputa com o
  painel);
- manda a fatia pronta para a página de trás **uma vez**, por DMA2D, enquanto
  a CPU já desenha a próxima fatia na outra.

A PSRAM passa a ver cada pixel final **uma vez**, em rajadas de DMA
comportadas, em vez de N escritas da CPU. Somando a QoS do AXI, as faixas, os
respiros e o `tab5_tela_gastou` deixam de ter motivo para existir.

O rasterizador continua sendo o M5GFX: cada fatia é um `LGFX_Sprite` com
`setBuffer()` apontando para a SRAM (verificado: `LGFX_Sprite.hpp:155`). O
contexto de desenho carrega o deslocamento da fatia, e os objetos desenham
em coordenadas próprias sem saber disso.

### 4.2 A árvore de objetos (modo retido)

Hoje o app é "imediato": o sistema chama `desenhar()` e o app pinta tudo, com
as regras de desenho na cabeça. Na nova base o app **declara** a tela uma vez
(`ao_abrir`) e depois só **muda propriedades**:

```c
ui_obj_t* tela  = ui_tela_do_app();
ui_obj_t* bat   = ui_rotulo(tela, 16, 16, "Bateria: --");
ui_obj_t* lista = ui_lista(tela, ...);         // rolagem, inércia, recorte: do sistema

// quando o dado muda (na tarefa de interface):
ui_rotulo_texto(bat, "Bateria: 87%");            // invalida só o retângulo do rótulo
```

Cada objeto sabe a própria área, então invalidação é automática. Estilo
(cor, fonte, raio, borda) é do objeto, não estado global: a regra nº 2
desaparece. Largura fixa e pintar por diferença desaparecem: o compositor
redesenha o retângulo inteiro na SRAM, com fundo, sem piscar.

Objetos previstos: `tela`, `painel` (retângulo com raio e borda), `rotulo`,
`botao`, `imagem` (PNG/emoji), `lista` (rolável e **virtualizada**: só existem
os itens visíveis — essencial para o histórico do Mesh), `canvas` (desenho
livre para Azimutal, Câmera e Cobra).

### 4.3 Entrada

O toque vai para o sistema, que acha o objeto sob o dedo e entrega eventos
(pressionou, soltou, clicou, arrastou, toque longo). A lista trata o arrasto
sozinha (limiar, inércia ao soltar, freio nas pontas) e engole o clique de
quem estava rolando. Some o `ao_tocar`/`ao_tocar_nivel`/`rolagem_foi_arraste`
em cada app.

### 4.4 Energia

Sem área suja, não há frame: a tarefa de interface dorme na queue de eventos
como hoje. O loop de frame só roda a 60 Hz enquanto há animação ou dedo na
tela; um relógio que muda por minuto custa um retângulo por minuto. Os perfis
de energia por app não mudam.

### 4.5 Migração sem big-bang

Um app ainda não portado roda dentro de um objeto `canvas` com um `M5Canvas`
próprio na PSRAM: o `desenhar()` antigo pinta nele como sempre, e o
compositor copia só a parte suja. Isso deixa o sistema inteiro funcionando
durante a troca, e é o caminho do Cobra (fronteira com a aba do jogo: o
contrato do app muda do lado de cá, o jogo porta quando a aba dele quiser).

---

## 5. Etapas

Cada etapa compila, grava e é testada no aparelho antes da próxima.

| # | etapa | entrega | como sabemos que funcionou |
|---|---|---|---|
| 0 | **medir** | contador de underrun, tempo de frame, SRAM livre | números de base para comparar |
| 1 | **HAL de frame** | `hal/tab5_frame`: QoS AXI, flip no limite, `on_frame_buf_complete`, sincronizar áreas, cópia por PPA | tela de teste no console: retângulo animado + rolagem sintética, zero underrun, zero rasgo |
| 2 | **compositor** | lista de áreas sujas, fusão, fatias na SRAM, loop de frame | a mesma tela de teste passando pelo compositor |
| 3 | **objetos + entrada** | árvore, estilos, `rotulo/botao/painel/imagem/lista/canvas`, toque, lista com inércia; fontes suaves no tamanho exato | home e barra de status reescritas na base nova |
| 4 | **Mesh** | mensagens como `lista` virtualizada de balões | a rolagem que hoje deforma, lisa, sem nenhum código de tela no app |
| 5 | **demais apps** | um por vez; os não portados rodam no `canvas` de ponte | cada app testado sozinho |
| 6 | **apagar o legado** | `paint.*`, faixas/respiros do `tab5_screen`, `rolagem_deslocar`, `REPINTURA_MIN_MS`, regras de desenho nº 1–4 | o código de tela dos apps vira só declaração |

---

## 6. Perguntas em aberto (a medir, não a chutar)

- **SRAM interna livre** com WiFi/esp_hosted ligados: decide o tamanho da
  fatia (etapa 0).
- **Nível de QoS** do DW_GDMA que zera o underrun sem estrangular a CPU
  (etapa 1).
- **PPA para a cópia fatia → página**, ou `draw_bitmap` com `use_dma2d`: o
  segundo é mais simples, o primeiro permite preencher e misturar por
  hardware. Decidir medindo (etapa 1).
- **Fontes**: VLW suave (o M5GFX já carrega) gerada nos tamanhos de uso por
  script, como o `generate_emoji.py` já faz com os emoji (etapa 3).

---

## 7. Diário

**Etapas 0 e 1 no código, aguardando teste no aparelho.**
- `hal/tab5_frame.{hpp,cpp}`: QoS do DW-GDMA no AXI (liga/desliga), aviso de
  fim de frame (`on_frame_buf_complete`), cópia de retângulo entre páginas
  pelo PPA, contador de underrun (vigia no canal 2 da saída da ROM, que o
  boot deixa livre — conta sem substituir o console).
- `hal/tab5_screen`: `tab5_tela_igualar_paginas()`, `tab5_tela_apresentar()`
  (sincroniza cache só das áreas → pede troca → espera o fim do frame →
  copia as áreas para a página que saiu), `tab5_tela_escrever_na_frente()`.
- `app_quadro` (descartável): a mesma animação no LEGADO e no NOVO, com QoS
  ON/OFF, medindo q/s, tempo de desenho, tempo de troca, pior frame e
  underrun.

**Resultado da etapa 1 no aparelho.**
- Troca sincronizada **funciona**: "sem troca: 0" em todas as rodadas do NOVO.
- Desenhar a janela inteira direto na PSRAM custa **~34 ms** (legado e novo
  iguais): teto de ~29 q/s. É o argumento da etapa 2.
- NOVO em 16–17 q/s porque esperava a troca logo após o pedido e copiava a
  janela inteira que o frame seguinte redesenharia. Corrigido na etapa 2
  (espera e sincronia adiadas para o início do frame seguinte).
- **QoS máximo no DW-GDMA PIORA**: com o painel a 79 MHz a tela piscou azul
  sem parar (observado em uso). Fica DESLIGADO; não entra na base até ser
  entendido e medido. Os 35 Hz de uma rodada foram o clock do painel baixado
  pelos Ajustes, não defeito.
- Pendente: confirmar se o vigia de underrun conta de verdade (autoteste no
  boot) — ele marcou 0 durante o que pode ter sido a rodada azul.

**Etapa 2 no código (`kernel/compositor.{hpp,cpp}`).** Lista de
áreas sujas com fusão (regra da LVGL), duas fatias na SRAM interna, montagem
com o M5GFX em coordenadas de tela (sprite com ponteiro recuado), cópia
fatia → página pelo PPA assíncrono, sincronia adiada que pula o que vai ser
redesenhado, troca pedida sem esperar. Teste: COMP ROLAR / COMP QUADRADO.

**Correções depois das piscadas azuis.**
- O "QoS" do app de teste DUPLICAVA o `hal/tab5_bus` (já ligado no
  boot: arbitragem 8 + ARQOS 8). "QoS OFF" do teste = estado misto; "QoS ON"
  = ARQOS 15, que piscou azul sem parar. Duplicata removida; o botão do teste
  agora liga/desliga o módulo original de verdade.
- Linha de cache L2: 64B → 128B (ver memória de causas do azul). Principal
  suspeita da falta de banda.
- Relógio do painel limitado a 24..80 MHz (Ajustes e console limitam em vez de
  recusar); o passo de 5 agora alcança 80.

**Etapas 1–2 validadas no aparelho; etapa 3a (a ponte) no código.**
Resultado do teste: COMP ROLAR "bem suave, nada de rasgo nem fragmento";
COMP QUADRADO a 63,5 q/s (teto do painel) com 2,4 ms; nenhum azul a 80 MHz.

Etapa 3a — **o sistema inteiro passa pelo compositor**:
- `kernel/virtual_screen`: canvas 720×1280 na PSRAM que o painel nunca lê, com
  um **painel rastreador** (subclasse do `Panel_Sprite`) que anota cada
  retângulo tocado por qualquer primitiva do M5GFX, já convertido para
  coordenadas físicas (a mesma conta de rotação do `Panel_Sprite`, então o
  Mesh em paisagem funciona).
- Kernel (home, barras, apps) desenha na tela virtual; no fim de cada volta
  do loop, `comp_quadro_imagem()` leva só as áreas anotadas para a página de
  trás pelo PPA e pede a troca. Ao acordar: tela inteira + espera a troca
  antes de acender a luz.
- `tab5_tela_limpar/rolar` escrevem no alvo (tela virtual) e **sem respiro**.
- Painel recriado (repouso, relógio) → o compositor percebe pela geração do
  `tab5_screen` e invalida a tela inteira.
- Azimutal: `pushSprite(px,py)` escrevia no PAI do canvas (M5.Display) —
  passava por fora; agora `pushSprite(&d, ...)`, sem fatiamento.
- App Frame: só ROLAR / QUADRADO / QoS.

Legado que ficou sem função mas ainda existe (sai na etapa 6): double buffer
antiga (`para_tras/mostrar`), `REPINTURA_MIN_MS`, `rolagem_deslocar`,
`pintura_*`, marcas de "já limpo".

**Etapa 3a validada; limpeza do
legado; primeiro componente padrão.**
- `hal/tab5_screen` 811 → 275 linhas: saíram double buffer antiga, faixas,
  respiros, `gastou` (vira no-op só por causa do Cobra), sincronização de
  cache em faixas. `REPINTURA_MIN_MS` e `rolagem_em_arraste` saíram do kernel;
  o "assentamento" de 70 ms ao acordar saiu do `tab5_power`; `comp_encerrar`
  saiu do compositor.
- **`kernel/list`**: lista rolável padrão (altura por item em cache, faixa
  exposta, barra, compensação de item novo em lista ancorada no fim).
- **Mesh/mensagens** migrado para a lista, com **cache de balões**
  (`construir_layout`): cada mensagem é medida uma vez por carga do histórico,
  não a cada passo de rolagem (antes: quebra de linha, emoji, 2×gmtime e o
  mutex do serviço, para TODAS as mensagens, a cada frame). O arraste não
  força mais a janela inteira.
- Pendentes no Mesh: Conversas e Nós ainda na rolagem antiga (migrar para a
  lista); mensagem nova ainda dispara repintura completa do app inteiro.

**Correções + auditoria.**
- Mesh: hora do envio era gravada como 0 (virava 31/12/1969 no fuso -3);
  agora UTC do sistema quando válida (`MESH_EPOCH_VALIDO`), e mensagem sem
  data não abre divisor. Botão AVISO e ENV com `TAB5_BOTAO_TEXTO_GRANDE`.
- Teclado virtual: fonte 24pt negrito nativa (era 12pt × 1,5 = serrilhada);
  o vão entre teclas vai para a tecla mais próxima; a tecla é confirmada na
  SOLTURA do dedo (antes só pelo clique do M5Unified, que descarta toque com
  arrasto ou longo), com o clique como rede para toque rápido demais.
- Auditoria estrutural (ver resposta da sessão): UI acorda a 40–50 Hz mesmo
  parada (M5.update → I2C do toque); o Mesh lê o histórico do SD NA TAREFA DE
  INTERFACE a cada mensagem nova; I2C interno só pode ser usado pela UI (sem
  mutex no M5Unified) e isso força pontes; apps sondam por `periodo_ms` em vez
  de receber evento.

**As quatro correções estruturais + revisão do protobuf.**
1. **Avisos** (`sistema_avisar`, `AVISO_*`, `app_t.avisos/ao_aviso/periodo_agora`):
   serviço diz o que mudou, app redesenha na hora. Mesh: sem período nas
   mensagens (só Nós/Malha, que mostram idade, têm 1 s).
2. **Worker de sistema** (`kernel/work`): trabalho lento fora da
   interface; resultado entregue por `AVISO_TRABALHO`; fechar app espera o
   trabalho em andamento e descarta a queue. Mesh (histórico e cartão, com dois
   buffers) e Arquivos (contagem de linhas e página) migrados.
3. **Interface dorme parada**: dedo ou trabalho pendente → 20 ms com a
   interrupção do toque desligada; parada → interrupção ligada e sono até o
   próximo prazo real. `sistema_pedir_redesenho` de outra tarefa acorda a
   interface.
4. **I2C interno numa trava só**: o M5GFX já trava a porta por transação; o
   SCCB da câmera (driver do IDF) não. `-Wl,--wrap=sccb_new_i2c_io` entrega ao
   esp_video um SCCB feito de transações do M5GFX (`hal/tab5_sccb.cpp`). Ponte
   da câmera removida. Isso também cobre o controle automático de exposição
   (IPA/AGC), que escrevia no sensor durante a captura sem trava.
- Teclado: debounce na soltura (2 amostras sem dedo, 80 ms entre letras).
- **Protobuf conferido contra `meshtastic/src/mesh/generated/.../mesh.pb.h`**:
  `MeshPacket.via_mqtt` é o campo 14 e `hop_start` o 15 (o código lia 13 e 14);
  `QueueStatus.mesh_packet_id` é varint (era lido como fixed32). O resto do
  decodificador e todo o codificador (ToRadio, heartbeat, want_config) batem.

**Tarefas: medição virou serviço.** `services/tab5_monitor`
mede sempre (1 Hz), guarda picos/mínimos com hora, histórico de 5 min e
eventos de gatilhos; grava eventos e um resumo por minuto no cartão. O app é
só a janela e, com o compositor, simplesmente redesenha tudo a cada segundo.

**App Energia revisado.** Painel lê a medida do kernel (sem 2ª
leitura I2C), fontes nativas 18pt/12pt, "HOJE" (arquivo do dia, pelo
worker, eixo 0–24h, buraco onde desligado) no lugar do histórico sem
prazo, corrente com sinal + linha do zero, potência em W, consumo do dia
integrado (mAh gastos/carregados, Wh). Redesenho por região. Registro:
`Sistema/energia/energia-AAAA-MM-DD.csv` (ENE3, hora real), lote de 5; `tarefas.csv`
saiu (o monitor cobre). Monitor ganhou gatilho de UNDERRUN com o app aberto.

**Revisão de retalhos e o padrão de lista.**
- `kernel/list` virou a ÚNICA forma de lista rolável: ganhou o toque
  (`lista_arrastar`, `lista_item_tocado` com `LISTA_ARRASTE`), a atualização
  de um item só (`lista_item_mudou`), respiro no topo e `lista_esconder`.
  Migradas: Mesh (mensagens, conversas, nós), Arquivos, Tarefas, Azimutal
  (dias) e Trilha (sessões — as páginas ANTERIOR/PRÓXIMA saíram). Nenhum app
  chama `rolagem_*` direto; `kernel/scroll` é o motor da lista e do kernel.
- Limites que escondiam os arquivos NOVOS (readdir devolve na ordem de
  criação): Arquivos 24→512 (ordenado, pastas primeiro, arquivos do mais novo),
  Azimutal 24→400 dias, Trilha 60→1000 sessões — tudo na PSRAM.
- Compositor: saiu o caminho de fatias na SRAM (só o app de teste usava e
  segurava 90 KB da SRAM interna desde o boot) e o "igualar páginas" a cada
  painel novo (1,8 MB a mais de cópia na PSRAM no boot e no acordar).
  `tab5_quadro_copiar` é a única cópia (bloqueante).
- `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` + `EXT_RAM_BSS_ATTR` nos
  estáticos grandes (nós do Mesh, dreno do log, eventos do monitor...):
  ~45 KB a menos na SRAM interna.
- Removidos: app Quadro (banco de prova, cumpriu a etapa 6),
  `tab5_power_tela` (apagava o painel por fora do repouso),
  `notif_esta_silenciada`, `pintura_mudou_f`, `rolagem_item_preservado`.
- Piscada azul: cor do underrun da ponte = MAGENTA (diagnóstico), relógio do
  frame e erros do DSI host viram eventos do monitor e aparecem no `tela`.

**Auditoria de custo; passos 0 e 1.**
O pixel ainda passa 5 vezes pela PSRAM (CPU na tela virtual, PPA para a
página de trás, sincronia no quadro seguinte); a rolagem ainda desloca pela
CPU (`copyRect` na tela virtual). Plano: 0 medir → 1 ritmo + fusão →
2 modo direto (desenhar na página de trás, sem tela virtual) com rolagem
pelo PPA da frente para trás → 3 Radar sem sprites, Tarefas por diferença →
4 taxa de atualização adaptativa (experimento medido).
- **Medida** (`comp_estat`): quadros, pixels de quadro e de sincronia, tempo
  de PPA, de espera da troca, de desenho e da volta inteira (histograma por
  faixa: <4, <8, <12, <16, <33, >=33 ms), rolagem pela CPU. Console:
  `tela medir` abre a janela, `tela` mostra. Monitor: `quadros-AAAA-MM-DD.csv`
  por minuto (só minuto com quadro).
- **Ritmo:** no modo rápido a volta que apresentou espera a troca e recomeça
  na hora, em vez de dormir 20 ms depois do trabalho (~30 q/s com trepidação
  contra o painel de ~63 Hz).
- **Fusão:** 32 áreas; lista cheia funde só o par de menor desperdício (antes
  virava um retângulo envolvente, às vezes a tela inteira).
- **Primeira medida (rolagem, painel 47,4 Hz):** volta com quadro média
  120 ms, 157 de 159 acima de 33 ms. A cópia pelo PPA era 88 ms por quadro
  (~32 MB/s em uso): o gargalo, não o atalho.
- **`tela banco`** (720x1024, painel varrendo): PPA 92 MB/s, DMA2D puro
  (`esp_async_fbcpy`) 288 MB/s, GDMA AXI 206 MB/s, CPU 118 MB/s. A cópia
  entre páginas passou para o DMA2D (PPA só de reserva). Cache do destino:
  devolver+invalidar, porque "só invalidar" exige bordas de 128 B.
- **Segunda medida (cópia por DMA2D):** cópia 88 → 11,4 ms por quadro, volta
  120 → 40,8 ms. Desenho (26,9 ms, rolagem da CPU inclusa) virou o maior custo.

**Passo 2: modo direto (o "direct mode" da LVGL com duas páginas).**
- A tela virtual saiu (−1,84 MB de PSRAM). O rastreador escreve na página de
  trás; a primeira escrita da volta ABRE o quadro (`comp_abrir`: espera a
  troca anterior e sincroniza frente → trás), `comp_apresentar` devolve o
  cache das linhas desenhadas e pede a troca. Troca de página só pelo
  ponteiro do sprite (`setBuffer` zeraria a janela no meio da escrita).
- Rolagem: `copyRect` vira cópia por DMA2D da FRENTE (deslocada) para trás,
  quando a origem não foi desenhada neste quadro; antes de abrir, a janela é
  prometida (`comp_cobrir`) e a sincronia não a copia à toa. Senão, CPU.
- Painel novo (boot, relógio, acordar) = páginas sem nada nosso:
  `comp_painel_novo()` faz o core redesenhar tudo. Desenho sem painel cai
  numa lixeira alocada só se acontecer (com log).
- Câmera: superfície de DMA própria, alocada no primeiro pedido e devolvida
  ao fechar o app; levada à página por DMA2D no `desenhar`.
- Troca do relógio do painel pelo console agora roda na tarefa de interface
  (`sistema_trocar_relogio_painel`): destruir as páginas de outra tarefa no
  meio de uma passada de desenho seria escrever em memória liberada.
- Histograma da volta em PERÍODOS do painel medido (1, 2, 3, 4+), não em ms
  fixos. CSV novo: `quadros2-AAAA-MM-DD.csv`.
- **Terceira medida (modo direto):** volta 40,8 → 23,0 ms, 75% dos quadros
  numa varredura só; desenho 26,9 → 14,0; rolagem 6,9 → 2,2 ms (DMA). Mas a
  sincronia virou 2/3 da cópia (336 MB contra 154 da rolagem): os apps
  desenham o cabeçalho ANTES da lista, o quadro abria e sincronizava a
  janela inteira, e só depois a rolagem a sobrescrevia.
- **Sincronia preguiçosa por área:** ao abrir, as áreas do quadro anterior
  ficam pendentes; cada uma vem da frente só quando algo escreve ou LÊ nela
  (`comp_preparar`, chamado pelo rastreador, inclusive no `readRect`), e o
  resto ao apresentar. Cópia por DMA que cobre uma pendente inteira (rolagem,
  superfície) a descarta sem copiar.
- Desenho medido separado: passada completa × parcial.
- **Quarta medida (muitas aberturas de app):** passada completa 123,5 ms,
  parcial 14,0 ms - o custo agora é abrir app. Sincronia ainda alta: a fusão
  junta o cabeçalho e a janela da lista num retângulo pendente só, e tocar o
  cabeçalho trazia a janela inteira.
- **Pendentes recortadas:** só a interseção com o que vai ser tocado vem da
  frente; o resto continua pendente em até 4 pedaços. `fillRect` é escrita
  opaca: descarta a pendente que cobre em vez de copiá-la (a limpeza da área
  útil numa abertura de app deixa de copiar 1,6 MB para pintar por cima).
- Log `desenho completo de <app>: N ms` acima de 40 ms, para achar quem pesa.
- **Passada completa por app** (tabela no `tela`): "limpar" 20-25 ms em todo
  app (CPU escrevendo 1,6 MB de preto); "conteúdo" segue o volume de texto
  (Camera 6 ms, Arquivos 40, home 57, Mesh 55, Ajustes 77, Tarefas 116).
- **Preenchimento por hardware:** `fillRect` de 16 K px ou mais vai para o
  motor FILL do PPA (só escrita; o driver devolve+invalida o cache das linhas
  antes). Autoconferência na primeira cor com bytes diferentes; se não bater,
  desliga e volta à CPU. `tela banco` compara PPA-fill × CPU-fill.
- **Medida com o PPA-fill:** "limpar" 20-25 → ~8 ms em todos os apps (banco
  feito em repouso, a 40 MHz: só a proporção vale - PPA-fill 3,6x a CPU).
  Arquivos com pior caso de 2,3 s: a foto (1,4 MB) era lida do cartão linha a
  linha DENTRO do desenhar(), e relida a cada redesenho completo. Agora o
  worker lê uma vez para um buffer na PSRAM (já na ordem da tela) e o
  desenhar() só pinta; o buffer é solto ao voltar para a lista ou fechar.
- **Medida com a tela acesa:** PPA-fill 4,2 ms contra CPU-fill 14,1 ms
  (720x1024). Arquivos: pior 2.271 → 92 ms. Passada completa média 61 ms;
  o que sobra é texto (Ajustes 61, ESP-NOW 55, home 54 de "conteúdo").
- **Por que texto custa:** o `VLWfont::drawChar` do M5GFX faz um
  `writeFillRect` 1x1 por pixel de borda suavizada, mais um por linha de
  fundo - ~200 chamadas por letra de 30 px, cada uma passando pelo rastreador.
- **Cache de glifos** (`tab5_glyphs`, `FonteRapida` no lugar do `VLWfont`):
  a letra é desenhada uma vez pelo próprio M5GFX num rascunho RGB565 e
  guardada por (fonte, letra, cor, fundo); depois, um `pushImage` por letra.
  Só texto com fundo, tamanho 1, destino de 16 bits e letra que não invade a
  anterior - o resto segue o caminho original. 512 KB de PSRAM, recomeça do
  zero se encher. Estatística no `tela`.
- **Época das páginas** (`g_tab5_tela_epoca`): o caminho de cada escrita
  confere "a página ainda existe?" com uma comparação, em vez de consultar o
  painel a cada pixel.
- **Medida com arena de 1 MB:** cache de glifos 79% (97% sem contar espaços,
  que já são baratos); recusas: espaço 3202, invade 435, outro 75; não
  recomeçou. Passada completa 40 ms, parcial 4,5, volta 17,9.
- **Tarefas por campo (passo C):** a janela apagava a área inteira e
  redesenhava tudo a cada amostra (1 Hz). Agora cada linha de texto é um
  campo que guarda o que está na tela e só repinta quando muda; a tabela,
  por linha (assinatura do texto + `lista_item_mudou`); o gráfico, o miolo,
  uma vez por amostra; títulos, moldura e botão só na passada completa.
