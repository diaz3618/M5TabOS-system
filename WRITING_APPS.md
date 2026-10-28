# Escrevendo aplicativos para o M5tabOS

Este guia é a soma dos erros que a gente já cometeu. Quase toda regra aqui
existe porque alguma coisa quebrou de um jeito que levou horas para entender —
e o comentário diz qual, para você não precisar repetir.

> **A referência das APIs** (cada função, o que faz e de qual tarefa pode ser
> chamada) está no [API.md](API.md). O app [Exemplo](main/app_example/app_example.cpp)
> usa cada uma delas num cartão: o jeito mais rápido de começar é copiar a
> pasta dele.

---

## 1. O mínimo para existir

Um aplicativo é **uma pasta dentro de `main/`, com o prefixo `app_`** e um ou
mais `.cpp` (o prefixo existe, para distinguir apps de
`main/kernel/`, `main/hal/` e `main/services/` só de olhar o nome da pasta -
ver a árvore completa no fim desta seção). O `CMakeLists.txt` faz glob
recursivo: criar a pasta basta, não há nada a registrar no build.

```
main/
  app_meuapp/
    app_meuapp.cpp
```

No fim do arquivo, um descritor:

```c
extern const app_t APP_MEUAPP = {
  .nome   = "MeuApp",
  .cor    = TFT_ORANGE,
  .perfil = { },              // o hardware que ELE precisa ligado
  .usa_sd = false,
  .orientacao = ORI_EM_PE,    // ou ORI_DEITADO, ou ORI_SEGUE_IMU (seção 9)
  .piso_repouso_mhz = 0,
  .ao_abrir  = ao_abrir,      // pode ser nullptr
  .ao_fechar = ao_fechar,     // pode ser nullptr
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,      // pode ser nullptr
  .periodo_ms = 0,
};
```

> **`extern` é obrigatório.** Em C++, um `const` no escopo de arquivo tem
> ligação **interna**. Sem o `extern`, o `apps.cpp` não enxerga o símbolo e o
> erro só aparece no link, como `undefined reference` — longe da causa.

Não há lista para editar: o `main/CMakeLists.txt` gera a tabela a partir das
pastas `app_*`, e o nome do símbolo segue o da pasta (`app_meuapp` →
`APP_MEUAPP`). Para tirar um app, apague a pasta.

**A árvore completa do sistema, para situar onde cada coisa mora:**

```
main/
  kernel/     core.cpp, shell.cpp, apps.cpp (gerada), app_task.*, paint.*, app.h
              (o contrato entre apps e sistema; quem liga/desliga serviço)
  hal/        tab5_screen, tab5_power, tab5_grove, tab5_keyboard, tab5_sd,
              tab5_sensors, tab5_bus, tab5_wifi, tab5_gps, nmea
              (hardware puro - nenhum vive só porque um app está aberto)
  services/   tab5_mesh, notification, settings, energy_log
              (estado que precisa existir com a tela fechada - ver seção 8)
  app_*/      cada aplicativo, na própria pasta com o prefixo app_
```

Regra de dependência, de baixo para cima: HAL não depende de serviço nem de
app; serviço não depende de app; kernel não depende de app (só o contrário -
app inclui `app.h`, nunca o inverso). Um app pode usar kernel, HAL e serviço
livremente, mas nunca o header de outro app.

---

## 2. As quatro regras de desenho

Esta seção é a mais importante do documento. **A maior parte dos bugs visuais
do projeto veio de violar uma destas quatro.**

### 2.1. Nunca apague para redesenhar

O M5GFX escreve direto no framebuffer que o painel está varrendo. Um
`fillRect` de limpeza seguido de um `drawString` **aparece como piscada**.

Escreva **por cima**, com cor de fundo e **largura fixa**:

```c
// ERRADO - pisca
d.fillRect(x, y, 200, 30, TFT_BLACK);
d.drawString(texto, x, y);

// CERTO - sobrescreve
snprintf(buf, sizeof(buf), "%-20.20s", texto);   // largura fixa!
d.setTextColor(TFT_WHITE, TFT_BLACK);            // cor de fundo!
d.drawString(buf, x, y);
```

Se o conteúdo **encolheu** (uma lista com menos itens), limpe **só a faixa que
sobrou**, nunca a área toda.

### 2.2. Estado de texto é GLOBAL, não do seu app

`setTextDatum`, `setTextSize` e `setTextColor` valem para o display inteiro.
**Entre dois desenhos do seu app, a barra de status desenha** e deixa o datum
dela (`middle_right`, da porcentagem de bateria).

Portanto: **defina datum e tamanho antes de todo `drawString`**, não só na
primeira passada.

```c
// ERRADO - o texto sai ancorado onde a barra de status deixou
if (primeira_vez) { d.setTextDatum(top_left); }
d.drawString(buf, x, y);

// CERTO
d.setTextSize(2);
d.setTextDatum(top_left);
d.drawString(buf, x, y);
```

Se você criar uma função de botão ou caixa que muda o estilo, **salve e
restaure** — por valor, porque `getTextStyle()` devolve referência para o membro
interno:

```c
const lgfx::TextStyle estilo = d.getTextStyle();   // cópia, não referência
...desenha...
d.setTextStyle(estilo);
```

### 2.3. Pinte por diferença, com o `paint.hpp`

```c
#include "paint.hpp"

static pintura_t s_p;
static int  s_ult_valor;
static char s_ult_nome[24];

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  pintura_iniciar(&s_p);                    // decide se a passada é completa

  if (pintura_completa(&s_p)) {
    ...títulos, molduras, tudo que nunca muda...
  }

  if (pintura_mudou_i(&s_p, &s_ult_valor, valor)) {
    ...desenha só o número...
  }
}
```

Três coisas para não errar:

**O `pintura_iniciar()` consulta sozinho se o sistema limpou a área.** Sem isso
(ou com um `s_tudo` feito à mão que esqueça essa consulta), **a tela fica em
branco ao voltar do repouso** — o app acha que nada mudou, mas tudo foi apagado.

**Não use `||` entre comparadores.** O `||` curto-circuita e os seguintes nem
são chamados, deixando a memória deles desatualizada — dá o clássico "só
atualiza uma vez a cada duas":

```c
// ERRADO
if (pintura_mudou_i(&s_p, &a, x) || pintura_mudou_i(&s_p, &b, y)) { ... }

// CERTO
const bool m1 = pintura_mudou_i(&s_p, &a, x);
const bool m2 = pintura_mudou_i(&s_p, &b, y);
if (m1 || m2) { ... }
```

**Os comparadores são para decidir DESENHO, não E/S.** Eles devolvem `true` em
toda passada completa — por construção. Se você usar um para decidir "reler o
cartão", e a leitura pedir repintura, entra em **loop infinito**. Para decisão
de E/S, compare na mão.

### 2.4. Área grande se limpa em FAIXAS

Quando você **precisa** limpar de verdade — trocou de página, o layout mudou —
não use `fillRect` na área inteira. Use:

```c
#include "tab5_screen.hpp"

tab5_tela_limpar(a.x, a.y, a.largura, a.altura, TFT_BLACK);
```

O motivo não é estético, é **banda de memória**. O painel MIPI-DSI puxa
~110 MB/s contínuos da PSRAM só para manter a imagem viva. Um `fillRect` de
tela cheia escreve ~1,8 MB na mesma memória, o mais rápido que a CPU consegue.
Quando as duas rajadas se somam, o FIFO do painel seca — e o frame sai
**azul**. O driver do IDF descreve o sintoma na própria fonte
(`esp_lcd_panel_dpi.c:113`):

```c
// when an underrun happens, the LCD display may already becomes blue
ESP_DRAM_LOGE(TAG, "can't fetch data from external memory fast enough,
                    underrun happens");
```

O `tab5_tela_limpar()` preenche em faixas e respira entre elas. Custa poucos
milissegundos numa operação que só acontece ao abrir, fechar ou acordar.

> Esse azul apareceu **ao sair de um app para a tela inicial**, e não ao abrir.
> A home é quase toda preta, e área preta grande é o preenchimento mais rápido
> e mais contíguo que existe. Abrir um app escreve o mesmo tanto, mas espalhado
> em dezenas de operações pequenas — e a rajada nunca se forma.

**Para retângulos pequenos — botão, célula, barra — o `fillRect` normal
continua sendo o certo.** A faixa única sairia igual e ainda pagaria o loop.

**Inclusive o "apaga o que sobrou".** O padrão de limpar só a faixa que a lista
deixou de ocupar parece pequeno, mas quando a lista encolhe muito ele vira meia
tela. O azul voltou por isso: o sistema limpava em faixas e os
apps ainda tinham nove `fillRect` grandes espalhados, cada um capaz de virar a
rajada sozinho. Se o retângulo **pode** ficar grande, use `tab5_tela_limpar()`.

**Se você vir azul, procure `lcd.dsi.dpi` no monitor serial.** O driver loga o
underrun em nível ERROR. Sem essa linha, o azul é outra coisa.

---

## 3. Toque

A assinatura é `void ao_tocar(int x, int y)`, com coordenadas **relativas à
área do app**. A área em si vem de `sistema_area()` — **nunca reconstrua com
constantes**:

```c
// ERRADO - as alturas da barra e da navegação são privadas do sistema
const area_t a = { 0, 0, M5.Display.width(), M5.Display.height() - 64 - 96 };

// CERTO
const area_t a = sistema_area();
```

**Não faça E/S bloqueante no toque.** Ler cartão, falar com hardware ou
qualquer coisa demorada trava a tarefa de interface **antes de qualquer
desenho**. Marque a intenção e faça o trabalho no `desenhar()`:

```c
static bool s_precisa_ler = false;

static void ao_tocar(int x, int y) {
  if (...) { s_pagina++; s_precisa_ler = true; sistema_pedir_redesenho_completo(); }
}

static void desenhar(...) {
  if (s_precisa_ler) { ler_do_cartao(); s_precisa_ler = false; }
  ...
}
```

### Pedindo redesenho

| chamada | quando |
|---|---|
| `sistema_pedir_redesenho()` | mudou um **valor** |
| `sistema_pedir_redesenho_completo()` | mudou a **tela** (página, seleção, layout) |

A versão completa limpa a área e passa pelo double buffer, então **não pisca**.
A incremental escreve direto na página visível — correto para poucos pixels.

---

## 4. Perfil de energia

O `perfil` diz **o que o seu app precisa ligado**, nunca o que os outros podem
desligar. O sistema **soma** o que ele mesmo garante:

```c
.perfil = { .grove_5v = true },   // este app usa o Grove
```

| campo | quem manda |
|---|---|
| `brilho` | **o sistema** (preferência do usuário). Não defina — é ignorado. |
| `radio` | **o sistema**, se o serviço de WiFi precisar |
| `grove_5v` | você **soma**; o ajuste do usuário também pode ligar |
| `imu`, `microfone`, `alto_falante`, `rtc` | fase 1: união de todos os apps no boot |

> A regra existe porque a violamos: todo app declarava `radio = false` (o
> padrão), então **abrir qualquer app cortava a energia do rádio** e derrubava
> o WiFi no meio de uma conexão.

O `piso_repouso_mhz` é a frequência mínima que o app precisa com a tela
apagada. `0` significa "pode descer ao mínimo".

### Se o seu app muda uma preferência do sistema

Um app de ajustes (como o app Ajustes) muda algo que atravessa todos os apps. Para
isso valer **na hora**, não mexa no hardware:

```c
// ERRADO - passa por cima da regra do "app só soma"
tab5_power_grove(false);

// CERTO - o sistema remonta o perfil de quem está na tela e aplica
ajustes_definir_grove(false);
sistema_reaplicar_perfil();
```

> Isto nasceu de um bug: desligar o 5V do Grove em Ajustes não apagava nada — o
> trilho só caía ao voltar para a home. O app ligava na mão com
> `tab5_power_grove(true)` e o sentido contrário não tinha par, então "OFF" na
> tela e 5 V no fio conviviam.
>
> A correção não foi chamar `tab5_power_grove(false)` também. Foi **parar de
> mexer no hardware a partir do app**: quem sabe montar o estado final é o
> sistema, porque só ele conhece a regra de que o app soma e nunca subtrai.

---

## 5. Tarefas

Se o app precisa de trabalho de fundo, **use o `tarefa_app`**, nunca
`xTaskCreate` direto:

```c
static tarefa_app_t s_t;

static void meu_laco(tarefa_app_t* t) {
  while (tarefa_app_ativa(t)) {
    ...trabalho...                      // com esperas LIMITADAS
  }
  // aqui devolva o que voce pegou: fclose, xSemaphoreGive, etc.
}

static bool ao_abrir(void) {
  return tarefa_app_criar(&s_t, meu_laco, "meuapp", 4096, 3);
}

static void ao_fechar(void) {
  tarefa_app_encerrar(&s_t, 200);   // pede, espera ate 200ms, so entao segue
}
```

`tarefa_app_encerrar()` sinaliza, aborta o bloqueio pendente
(`xTaskAbortDelay`) e **espera a tarefa confirmar** antes de você destruir queue
ou driver - so depois dela voltar e que e seguro liberar o que a tarefa usava.
Matar tarefa com `vTaskDelete` enquanto ela segura um semaphore ou está dentro
de um `uart_read_bytes` deixa recurso pendurado para sempre.

### Mais de uma tarefa no mesmo app

Sim, e não é exótico — é **um `tarefa_app_t` por tarefa**, cada uma com seu
próprio corpo. Por exemplo, um leitor de GPS por UART:

```c
static tarefa_app_t s_t_leitura;   // le a UART, so isso
static tarefa_app_t s_t_parser;    // interpreta o que a leitura publicou

static bool ao_abrir(void) {
  if (!tarefa_app_criar(&s_t_leitura, laco_leitura, "GPSRead",   4096, 3)) { return false; }
  if (!tarefa_app_criar(&s_t_parser,  laco_parser,  "NMEAParse", 4096, 2)) { return false; }
  return true;
}

static void ao_fechar(void) {
  tarefa_app_encerrar(&s_t_parser,  200);   // a ORDEM importa: quem consome
  tarefa_app_encerrar(&s_t_leitura, 200);   // primeiro (senao o produtor
}                                           // escreve numa fila cujo leitor já foi embora)
```

Por que duas em vez de uma: **cada tarefa deveria fazer UMA coisa**, no seu
próprio ritmo. Ler UART é bloqueante e não pode esperar; interpretar o que
chegou pode ser mais lento sem atrasar a leitura seguinte — as duas com
prioridades diferentes (`3` e `2` acima) deixam isso explícito. Elas se
comunicam por **queue** (`xQueueCreate`), nunca por variável global sem trava:
é a mesma regra de "quem desenha não toca hardware" aplicada a tarefas que
falam entre si.

O orçamento de stack **soma**: duas tarefas de 4096 bytes são 8KB, além da
UI (8192) e de qualquer serviço do sistema já rodando. Não é motivo para não
criar a segunda tarefa — é motivo para não criar uma terceira "só porque
pode": se duas queues resolvem, duas tarefas bastam.

---

## 6. Cartão SD

O cartão pode estar **no computador** (modo pendrive, Ajustes > USB). Por isso
gravar e ler têm regras do sistema — siga-as e o seu app não precisa saber se
o cartão está ou não no Tab5.

**Gravar dados: só pelas três portas** (`hal/tab5_sd.hpp`). Elas montam o
cartão sozinhas e, com ele no computador, guardam no diário da flash interna,
que chega ao cartão na volta, na ordem certa:

```c
// acrescentar uma linha, sem esperar (vai em lote; ótimo para log e histórico)
tab5_sd_anexar("MeuApp", "dados.csv", linha, "data,valor\n");   // cabeçalho se o arquivo estiver vazio

// acrescentar um bloco agora (para quem já junta em lote)
tab5_sd_acrescentar("MeuApp", "dados.bin", bloco, n);

// trocar o arquivo inteiro (nunca deixa meio arquivo)
tab5_sd_substituir("MeuApp", "config.txt", texto, n);
```

`acrescentar` e `substituir` bloqueiam: chame do worker (`sistema_trabalho`),
nunca da tarefa de interface.

**Só reescreva o que leu.** Se o seu app guarda uma lista no cartão e a
reescreve inteira com `tab5_sd_substituir`, ele precisa ter lido o arquivo
antes. Sem cartão na leitura, não reescreva: a lista vazia da RAM apagaria o
que estava guardado. (É o que as redes Wi-Fi, a NodeDB do Mesh e as conversas
apagadas fazem.)

**Ler:** `tab5_sd_abrir(app, arquivo, "r")` — ele grava antes o que estava
pendente, então você sempre lê a última versão. Com o cartão no computador ele
devolve `NULL`, como se o cartão não estivesse lá; trate como "sem dados". Ele
distingue "arquivo ainda não existe" (normal) de falha real.

**Declare o que o app precisa:**

- `.usa_sd = true` — o sistema monta o cartão **antes** do `ao_abrir()` e
  desmonta **depois** do `ao_fechar()`. Abre mesmo sem cartão.
- `.exige_sd = true` — sem o cartão o app não tem função (navegar nos
  arquivos, gravar foto). Com o cartão no computador, **o sistema não abre o
  app**: mostra o motivo no lugar dele e abre sozinho quando o cartão volta. E
  o pendrive não começa com ele aberto. O app não precisa tratar "o cartão
  sumiu no meio".

Arquivo novo **grande** (foto, firmware recebido) é o único caso de gravar com
`tab5_sd_abrir(..., "wb")` direto: ele exige o cartão de verdade, então quem o
produz declara `.exige_sd = true`.

Se precisar montar por conta própria (para ler numa tarefa, por exemplo), a
montagem é contada por referência e **toda montagem precisa da sua
desmontagem, em todos os caminhos de saída** — inclusive nos de erro.

```c
if (!tab5_sd_montar()) { return; }      // false também com o cartão no computador
FILE* f = tab5_sd_abrir("MeuApp", "dados.csv", "r");
if (!f) { tab5_sd_desmontar(); return; }    // <- não esqueça aqui
...
fclose(f);
tab5_sd_desmontar();
```

---

## 7. Recursos exclusivos

Alguns recursos cabem **um de cada vez** e têm árbitro no sistema. Sempre peça,
e **respeite o não**:

```c
if (!tab5_grove_tomar(GROVE_DONO_GPS)) {
  // Outro dono. Abra assim mesmo se fizer sentido, e EXPLIQUE na tela.
}
...
tab5_grove_soltar(GROVE_DONO_GPS);   // no ao_fechar()
```

> Recusar abrir sem explicar é a pior saída: o usuário toca o ícone e o
> aparelho parece não fazer nada.

---

## 8. Aplicativo ou serviço?

Esta é a decisão de projeto mais importante.

| | aplicativo | serviço |
|---|---|---|
| vive enquanto | está na tela | o sistema está ligado |
| exemplos | Arquivos, Ajustes, Câmera | WiFi, Meshtastic, GPS, registro, teclado |
| use quando | o trabalho **é** a tela | o estado precisa existir com a tela fechada |

Se o recurso precisa **notificar** algo com o app fechado, ele é serviço — e o
app vira só a **janela** dele. Foi assim que o Meshtastic avisa de mensagem nova
sem estar aberto - e foi exatamente essa a razão de o GPS deixar de ser lido
direto por um app: o app Mesh precisou perguntar "onde
estou" para calcular distância até uma localização recebida, e um app não
enxerga o estado privado de outro. Promovido a serviço, qualquer app pode
perguntar - e o app que mostrava o GPS virou só a janela dele, exatamente como
o Mesh já era para o Meshtastic.

Serviço mora em `main/services/` (ou em `main/hal/` quando o que ele expõe é
essencialmente uma leitura de hardware, como o GPS - a linha entre os dois é
"tem regra de negócio" vs "só entrega o dado cru"), tem tarefa própria criada
pelo KERNEL (`main/kernel/`) - nunca pelo app - e expõe leitura por cópia,
nunca ponteiro para estrutura que a tarefa reescreve. Quem liga/desliga por
causa de troca de módulo no Grove é sempre o kernel (`kernel/core.cpp`), do mesmo
jeito para Mesh e para GPS - um app nunca chama `_iniciar()`/`_parar()` de
serviço nenhum diretamente.

**Peças prontas para serviço — não reescreva:**

| precisa de | use | em vez de |
|---|---|---|
| parar a própria tarefa com segurança | `kernel/service_stop.hpp` (`parada_preparar` / `parada_pedida` / `parada_confirmar` / `parada_pedir`) | duas bandeiras + `for (...) vTaskDelay(10)` perguntando se saiu |
| entregar evento a UM ouvinte | `hal/tab5_listener.hpp` — remover o ouvinte só volta depois da chamada em voo | ponteiro `volatile` e um `vTaskDelay` "para dar tempo" antes de destruir a fila |
| ler/escrever protobuf do Meshtastic | `services/mesh_pb.hpp` — leitor que nunca soma no ponteiro, escritor que nunca passa do fim | `*p++ = 0x22` e buffer "com folga" |
| "o relógio já foi acertado?" | `tab5_hora_epoch()` / `tab5_hora_valida()` (`services/tab5_time.hpp`) | `time(nullptr) > 1600000000` |

A regra por trás das quatro: **espere por um evento, não por um prazo.**
Quem precisa saber que algo terminou recebe um sinal (semáforo, contagem);
`vTaskDelay` serve para ritmo de hardware documentado (o datasheet pede X
µs) e para ceder CPU — nunca para "deve ter dado tempo".

### 8.1. App com PARTE DE FUNDO: `kernel/background.h`

Entre "app" e "serviço" existe um terceiro caso: o app cujo trabalho continua
quando a janela fecha - gravar a trilha, enviar a posição pela internet, um
cálculo longo. **Não** vire serviço feito à mão por isso: descreva a parte de
fundo e deixe o kernel rodar.

```c
static void meu_laco(fundo_t* f) {
    while (fundo_esperar(f, 5000)) {          // dorme; false = pediram para parar
        ...trabalho com esperas LIMITADAS...
        fundo_estado(f, n, "42 envios");       // o texto da pastilha cinza
    }
}

const fundo_def_t MEU_FUNDO = {
    .nome   = "GPS online",                    // tarefa, notificação e listas
    .corpo  = meu_laco,
    .pilha  = 4096,
    .perfil = { .radio = true },               // SOMA no perfil efetivo enquanto roda
    .ao_iniciar = preparar,                    // opcional, na interface
    .ao_parar   = liberar,                     // opcional, depois que o laço saiu
};

// na janela:
sistema_fundo_iniciar(&MEU_FUNDO);
sistema_fundo_parar(&MEU_FUNDO);               // espera o laço sair (até 3 s)
bool rodando = sistema_fundo_ativo(&MEU_FUNDO);
```

O que o sistema garante sem você escrever nada: tarefa própria em prioridade 1
(nunca trava o toque), a **pastilha cinza** com o nome enquanto roda (nada roda
escondido), o **perfil de energia somado** ao da tela, parada que acorda o loop
na hora, e limpeza quando o loop termina sozinho. No console: `fundo` lista,
`fundo parar <nome>` para.

As regras do loop:

- **Nunca espere para sempre.** A espera de verdade é `fundo_esperar()`; o
  resto com prazo, senão o pedido de parada não chega.
- **Loop de cálculo chama `fundo_ceder(f)` a cada volta** - sem isso o kernel
  fica tomado, o IDLE não roda, o watchdog dispara e o aparelho não dorme.
- **O loop não desenha.** Guarda o estado (protegido) e a janela lê por cópia.
- **A memória do fundo é do fundo** - estática, ou alocada no `ao_iniciar()` e
  liberada no `ao_parar()`. Nunca um buffer da janela: ela fecha quando quiser.

O exemplo vivo é o cartão FUNDO do app Exemplo: um cronômetro (`laco_fundo`,
`EXEMPLO_FUNDO`) que continua contando, com a pastilha na barra, depois que a
janela fecha.

### 8.2. Avisando o usuário: `services/notification.h`

A notificação é **serviço do sistema**: qualquer serviço ou app publica, e a
barra de status mostra sem saber quem é quem. Ninguém mexe na barra para
avisar nada.

```c
#include "notification.h"

static int s_notif = -1;

// Registrar: pode chamar a cada subida/abertura - e idempotente pelo NOME
// (mesmo nome devolve a mesma vaga, COM o estado que ela tinha). O nome tem
// que ser literal/estatico.
s_notif = notif_registrar("Meu servico");
notif_silenciar(s_notif, !minha_preferencia_de_som);   // opcional, por fonte
notif_associar_app(s_notif, "MeuApp");   // opcional: o app que o toque abre

// Publicar o ESTADO ATUAL (nao um evento). De qualquer tarefa.
notif_publicar(s_notif, 3, NOTIF_INFO, "3 novas");
notif_limpar(s_notif);                                 // nada a dizer
```

As regras, e por quê:

- **Estado, não evento.** Publique "tenho 3", nunca "chegou mais 1". Republicar
  o mesmo estado não custa nada (a barra só repinta quando algo muda).
- **A quantidade é derivada da sua fonte da verdade**, nunca mantida por
  aritmética em paralelo. O Mesh mantinha um total à parte das conversas, e os
  dois se descolaram: mensagem nova sumia da barra. Recalcule a
  partir do que você guarda, na hora de publicar.
- **Escolha o nível pelo que ele significa:**

  | nível | quando | na barra |
  |---|---|---|
  | `NOTIF_ESTADO` | atividade em curso ("gravando trilha") | cinza; só aparece se nada mais tiver o que dizer |
  | `NOTIF_INFO` | novidade que não exige você (tráfego de canal) | azul |
  | `NOTIF_ATENCAO` | endereçada a você (mensagem direta) | laranja |
  | `NOTIF_URGENTE` | exige ação (bateria acabando, cartão cheio) | vermelho |

- **A barra mostra UMA fonte em destaque** (`notif_destaque`): a de maior
  nível e, no empate, a que mudou por último, com o **seu resumo** - então
  escreva um resumo que se explique sozinho ("1 em LongFast", não "1"). As
  outras ativas viram um **ponto na cor do nível** ao lado (o que não couber
  vira "+N").
- **Central de avisos:** tocar a barra de status abre a lista de TODAS as
  fontes ativas (`notif_listar`), uma linha por app com nome, resumo e
  contagem. Tocar a linha abre o app associado - por padrão o app com o
  **mesmo nome** da fonte; se o nome for outro ("Meshtastic" → app "Mesh"),
  chame `notif_associar_app()`. A tela de bloqueio mostra a mesma lista (até
  4 linhas).
- **Não apague o aviso por reinício.** Registrar de novo não zera mais a
  fonte, e quem para e sobe o serviço sozinho (troca de via, rede caindo)
  não deve chamar `notif_limpar()` se ainda há o que ler - o aviso sumia sem
  o usuário ter aberto nada. Apague quando a coisa foi **vista**.
- **"Visto" é só com o app visível.** Com a tela bloqueada ou a central
  aberta o app fica *encoberto* (`sistema_app_encoberto()`): não recebe
  `ao_aviso`, toque nem tecla - os avisos esperam até ele voltar à frente.
  Então marcar como lido dentro de `ao_aviso`/`desenhar` é seguro.
- **O beep** toca quando a quantidade de uma fonte não silenciada **sobe**.
  Contador de progresso (pontos, bytes) em nível acima de `ESTADO` faria bipar
  a cada passo - use `ESTADO`, ou silencie a fonte.
- **App x serviço:** um app só roda aberto, então só avisa enquanto está na
  tela. Aviso que precisa existir com o app fechado é trabalho de serviço (ver
  acima). Ao fechar, um app que publicou deve `notif_limpar()`.

---

## 9. Orientação: em pé, deitado ou seguindo o aparelho

`.orientacao` escolhe entre os dois estados de tela que existem no sistema:

| valor | tela | quando |
|---|---|---|
| `ORI_EM_PE` (padrão) | 720×1280, rotação 0 | a maioria dos apps, como a home |
| `ORI_DEITADO` | 1280×720, rotação 3 | paisagem fixa, ExtPort1 (teclado) embaixo |
| `ORI_SEGUE_IMU` | um dos dois, conforme o aparelho | apps de mensagem (Mesh, ESP-NOW) |

As rotações 1 e 2 nunca são usadas: a tela não vira "para o outro lado".
O sistema gira ao abrir e volta em pé ao fechar. O `sistema_area()` já devolve
as dimensões novas.

**`ORI_SEGUE_IMU`** (`kernel/orientation`): com o app aberto, o sistema lê o
IMU a 5 Hz e gira quando a gravidade fica claramente num dos eixos da tela por
~0,6 s. Aparelho na mesa ou na diagonal não muda nada (zona morta), e depois de
girar há 1,5 s de carência. Com o teclado físico encaixado, fica deitado. O
giro acontece entre dois quadros, nunca com dedo na tela, e vem seguido de
passada completa. O app precisa:

- declarar `.perfil = { .imu = true }` (o IMU só existe se algum app pedir);
- desenhar **sempre** a partir da área que chega — a largura muda com o app
  aberto. Medida guardada entre quadros tem que ser refeita (a `kernel/list`
  e a `kernel/conversation` já fazem isso sozinhas);
- se tiver layouts diferentes em pé e deitado, trocar no `ao_girar` (opcional):
  ele é chamado depois de a rotação valer e antes da passada completa.

Desenho e toque precisam da **mesma** função de geometria (`area` → posição),
para concordarem nas duas orientações — o Mesh usa `det_btn_l(a)`,
`cfg_btn_l(a)` e a grade `COLUNA`/`COLUNA_PE` da tabela de nós.

Se o seu app põe controle grande perto da borda inferior, trave a navegação
enquanto ele estiver aberto — o botão INÍCIO fica logo abaixo. Travada, a faixa
de navegação passa a ser do app: o toque nela chega na última linha da área.

```c
sistema_travar_navegacao(true);    // ao abrir
sistema_travar_navegacao(false);   // ao fechar
```

O teclado virtual já faz isso sozinho (`tecvirt_abrir`/`tecvirt_fechar`). O
sistema destrava ao fechar qualquer app, como rede de segurança.

Travada, a navegação também não deixa sair por outro caminho: tocar a barra
de status não abre a central de avisos (uma linha dela abriria outro app), e
`sistema_simular_abrir` de outro app é recusado.

Para trabalho que a pessoa acompanha sem tocar na tela (uma cópia longa), o
app pode manter a tela acesa — o repouso por inatividade não acontece:

```c
sistema_manter_acordado(SISTEMA_ACORDADO_APP, true);    // enquanto precisar
sistema_manter_acordado(SISTEMA_ACORDADO_APP, false);   // o sistema também solta ao fechar o app
```

### Teclado virtual

Para pedir texto (senha, PIN, mensagem), use o teclado do sistema — o app não
desenha tecla nem trata dedo:

```c
static tecvirt_t s_kb;
tecvirt_abrir(&s_kb);                        // ao começar a editar
tecvirt_posicionar(&s_kb, a);                // no desenhar: a área (tela) em cuja base ele abre
tecvirt_pintar(&s_kb, d, completo);          // TODA passada; sem mudança não desenha nada
n = tecvirt_nivel(&s_kb, acoes, 8);          // no ao_tocar_nivel: teclas confirmadas
ac = tecvirt_clique(&s_kb, xs, ys);          // no ao_tocar, se tecvirt_contem(&s_kb, xs, ys)
tecvirt_fechar(&s_kb);                       // ao terminar e no ao_fechar
```

As ações são o caractere (>0, o **codepoint** já com o shift — a segunda
página de símbolos tem os acentos do português), `TECVIRT_APAGAR`,
`TECVIRT_ENVIAR` e `TECVIRT_FECHAR`. Shift e troca de página são do teclado e
nunca chegam ao app. Campo sem cursor (senha, nome) não precisa tratar
caractere nem UTF-8 à mão:

```c
if (ac == TECVIRT_ENVIAR) { ... }
else if (ac == TECVIRT_FECHAR) { ... }
else { tecvirt_editar(buf, sizeof(buf), ac, so_ascii); }   // entra no fim / apaga o último
```

Texto corrido (mensagem) liga a maiúscula automática com
`tecvirt_frase(&s_kb, true)` e conta o texto e o cursor ao teclado sempre que
um dos dois muda: `tecvirt_contexto(&s_kb, texto, cursor)`. A conversa do
sistema já faz isso. Repinte só o campo quando o texto mudar — nunca a página
inteira por letra. Exemplo: senha do Wi-Fi em `app_config/app_config.cpp`.

A altura do teclado **depende da orientação** (em pé as teclas são mais altas,
porque lá falta largura e sobra altura). Para reservar espaço acima dele,
pergunte `tecvirt_altura(a)` com a mesma área do `tecvirt_posicionar` — nunca
guarde um número.

---

## 10. Rolagem por toque

Se o seu conteúdo não cabe na área, **não encolha a fonte**. Declare a altura
total e o sistema cuida do resto:

```c
extern const app_t APP_X = {
  ...
  .altura_conteudo = 1240,   // maior que a área: rola
};
```

`0` (o padrão) significa "não rola", e o app enxerga a área visível como
sempre.

Com rolagem ligada, o sistema:

- arrasta o dedo e rola, com limite nas duas pontas;
- **recorta** o desenho na área — sem isso você pintaria por cima da barra de
  status sem perceber;
- **soma a rolagem nas coordenadas do `ao_tocar`**, num lugar só;
- **engole o clique** quando o dedo andou mais de 12 px, para o fim de uma
  rolagem não apertar o que está embaixo;
- desenha a barrinha indicadora.

O ponto importante é o terceiro: **você compara o toque contra as mesmas
constantes que usou para desenhar.** O app inteiro vive num sistema de
coordenadas que começa em 0 e vai até `altura_conteudo`, e nunca soma
deslocamento nenhum. A alternativa — cada app somar um offset em todo `y` — é a
receita para o app que esquece de somar em *um* lugar e fica com um botão que
responde errado depois de rolar.

> O app Ajustes nasceu sem grade: os elementos eram posicionados no olho, e o valor
> do tempo de tela (x 264 a 528) passava por cima dos dois botões dele (400 a
> 490 e 500 a 590). A correção não foi mexer nesses números — foi dar altura
> conhecida a cada seção e posicionar tudo por deslocamento relativo. Aí não
> coube mais na tela, e a rolagem deixou de ser um problema.

---

## 11. Checklist antes de abrir PR

- [ ] pasta `main/app_x/` com `extern const app_t APP_X` (o nome segue a pasta)
- [ ] Nenhum `fillRect` de limpeza em caminho periódico
- [ ] Área grande limpa com `tab5_tela_limpar()`, não `fillRect` — inclusive as que só *podem* ficar grandes
- [ ] `setTextDatum`/`setTextSize` antes de todo `drawString`
- [ ] Usa `paint.hpp`, sem `||` entre comparadores
- [ ] `ao_tocar` usa `sistema_area()`, sem constantes copiadas
- [ ] Nenhuma E/S bloqueante no `ao_tocar`
- [ ] Tarefas criadas com `tarefa_app_criar` e paradas no `ao_fechar`
- [ ] Toda `tab5_sd_montar()` tem desmontagem em todos os caminhos
- [ ] `.perfil` não define `brilho`
- [ ] Preferência do sistema aplicada por `sistema_reaplicar_perfil()`, sem mexer no hardware
- [ ] Recurso exclusivo pedido com `tomar()` e devolvido com `soltar()`
- [ ] Botão/célula usa `tab5_ui.hpp`, não uma cópia local
- [ ] Testado: abrir, fechar, entrar em repouso e **acordar**

O último item pega mais bug que todos os outros juntos.

## Componentes prontos: não reimplemente botão

`tab5_ui.hpp` existe porque **quatro apps diferentes** escreveram o mesmo
botão, byte a byte igual - comentário incluso - antes de alguém notar
. O comentário que cada cópia carregava contava a mesma história:
sem salvar e devolver o `TextStyle`, o `middle_center` de um botão **vaza**
para o próximo `drawString` de quem desenhar depois, empurrando texto para
fora da tela. É a mesma pegadinha que já tinha mordido `tab5_power_mostrar` -
e mordeu de novo em cada cópia, porque cada uma redescobriu o bug sozinha em
vez de herdar o conserto.

```c
#include "tab5_ui.hpp"

tab5_botao(d, x, y, largura, altura, "OK", TFT_SKYBLUE);
tab5_botao(d, x, y, largura, altura, "OK", TFT_SKYBLUE, TFT_WHITE, 3);  // cor do texto e tamanho, se quiser diferente do padrão
```

Para um painel de "rótulo pequeno em cima, valor grande embaixo" - o mesmo
desenho da tela inicial (`GROVE`/`CARTÃO`) - use
`tab5_celula()` em vez de desenhar os três `drawString` na mão:

```c
tab5_celula(d, centro_x, y0, "FIX", "GPS", TFT_WHITE, "HDOP 0.9 - 8 sats");
```

Se você notar outro pedaço de desenho se repetindo em dois apps, é sinal de
que ele também deveria morar aqui - não na terceira cópia.

## Limpeza de tela: peça sempre, sem condição

Chame `tab5_tela_limpar()` sempre que quiser uma área limpa. **Não** consulte
`sistema_area_foi_limpa()` para decidir se vale a pena — essa decisão não é sua.

O sistema limpa a área útil inteira antes de te chamar numa repintura completa,
e avisa a camada de tela do que fez. Se o seu pedido já está satisfeito, ele
vira operação vazia lá embaixo. Você escreve a intenção; o kernel decide o
trabalho.

Isso importa porque limpar duas vezes a mesma superfície é ~3,2 MB escritos na
PSRAM em rajada, enquanto o painel puxa 119 MB/s dela para se manter vivo. O
FIFO do painel seca e o frame sai **azul**. Era um bug que aparecia num app
qualquer, meses depois, longe da causa — e que três dos oito apps evitavam por
lembrarem da regra, o que é a definição de regra no lugar errado.

`tela` no console mostra quantas limpezas foram dispensadas.

Nunca use `d.fillRect()` para áreas grandes: ele escreve tudo de uma vez, sem
pausa. `tab5_tela_limpar()` escreve em faixas com respiro entre elas.

**O número, para não ficar no "grande":** a faixa é de 96 KB, ou seja ~49.000
pixels. Um `fillRect` acima disso vira rajada; abaixo, cabe numa faixa e o
`tab5_tela_limpar` não muda nada. Na prática: largura da tela × mais de 40
linhas já passa do limite em retrato, e × mais de 38 em paisagem.

Na dúvida, use `tab5_tela_limpar()`. Ele nunca é pior que o `fillRect`.

## Loop que desenha muita coisa

Não precisa de nada especial. Desde o compositor ([COMPOSITOR.md](COMPOSITOR.md))
o app desenha na página de trás e o sistema leva à tela por DMA, entre duas
varreduras do painel: a rajada de escrita não disputa mais com a leitura do
painel. O antigo `tab5_tela_gastou()` ficou como função vazia, só para não
quebrar quem ainda chama.

---

## Animação: `periodo_ms` e `sistema_quadro_dt()`

O `periodo_ms` é o ritmo **pedido**. O sistema o arredonda para um número
inteiro de varreduras do painel (o mais perto do pedido, nunca menos de uma)
e começa o quadro logo depois de uma varredura — assim ele vai ao ar sempre
na mesma fase, sem o tranco de "um quadro a cada tantos fica um período a
mais na tela" (`kernel/frame_pacing.hpp`). Não há nada a fazer para ganhar isso.

Para **mover** coisas, não meça o tempo você mesmo dentro do `desenhar()`:
peça `sistema_quadro_dt()`. Ele dá o tempo entre as varreduras em que este
quadro e o anterior **aparecem**, não entre os instantes em que foram
desenhados — o que aparece andou exatamente o que ficou na tela. Limite do
seu jeito (depois de uma pausa ele é grande):

```c
float dt = sistema_quadro_dt();
if (dt > 0.1f) { dt = 0.1f; }
bola.x += bola.vx * dt;
```

E desenhe por caixas pequenas à vontade: muitos objetos pequenos por quadro
são problema do compositor, não seu — ele junta o que se sobrepõe e aguenta
dezenas de áreas sem copiar sobra.

---

## A piscada azul: o que é do sistema e o que é seu

Vale saber a causa, porque ela explica todas as regras acima.

O painel é MIPI-DSI em modo DPI. Ele **não recebe** a imagem: um DMA lê o
framebuffer da PSRAM linha a linha, para sempre, no ritmo do relógio de pixel.
Se esse DMA atrasar, o FIFO do painel seca e o frame sai **azul** — o próprio
driver do IDF diz isso em `esp_lcd_panel_dpi.c:113`.

Duas coisas atrasam o DMA, e **as duas são do sistema, não suas**:

| Causa | Onde foi resolvida |
|---|---|
| A CPU disputa a PSRAM com o painel | Prioridade no AXI-ICM (`tab5_bus`) e, na tela ILI9881C, relógio do painel a 60 MHz em vez de 80 (`tab5_power_relogio_dpi`; nas telas ST71xx fica o de fábrica) |
| A interrupção que religa o DMA é desligada durante escrita na flash | `CONFIG_LCD_DSI_ISR_CACHE_SAFE=y` |

O que sobra do seu lado é só **não fazer rajada grande**, e para isso as regras
já estão nas seções 2.1 e 2.4:

- limpe com `tab5_tela_limpar()` — **sempre, sem condição**; quem decide se há
  trabalho a fazer é o kernel.

### Peça a repintura CERTA

Este é o erro mais caro e o mais fácil de cometer:

```c
sistema_pedir_redesenho();            // mudou um VALOR    -> barato
sistema_pedir_redesenho_completo();   // mudou o LAYOUT    -> ~4 MB de PSRAM
```

A repintura completa apaga a área útil (1,6 MB), redesenha tudo, sincroniza o
framebuffer inteiro (1,84 MB) e troca de página. **Pedir isso para trocar uma
letra dentro de um campo de texto é o caminho mais curto para a piscada azul** —
e era exatamente o que o app Mesh fazia a cada tecla digitada.

Regra prática: se o que mudou continua **no mesmo lugar e do mesmo tamanho**,
é `sistema_pedir_redesenho()`. Só peça a completa quando algo mudou de posição,
apareceu ou sumiu.

### Teclado físico

O teclado do ExtPort1 chega pelo `ao_teclar` do `app_t`, já traduzido pelo
driver (`hal/tab5_keyboard`, modo HID com a tabela oficial das 70 teclas). O
texto vem em UTF-8; as teclas especiais vêm no `codigo`:

| código | tecla |
|---|---|
| `TAB5_TECLA_ENTER`, `_ESC`, `_TAB` | Enter, Esc, Tab |
| `TAB5_TECLA_APAGAR` / `_DEL` | Backspace (antes do cursor) / Del (depois) |
| `TAB5_TECLA_ESQUERDA`, `_DIREITA`, `_CIMA`, `_BAIXO` | setas |

Código desconhecido: ignore. Com Ctrl/Alt (`mod`) é atalho, não texto.
Acentos vêm por tecla morta, como no layout US internacional: `'`+vogal = á,
`'`+c = ç, `~`+a = ã, `^`+e = ê, `` ` ``+a = à; `'`+espaço = `'`. Campo que só
aceita ASCII (senha, nome do nó) passa o texto por `tab5_teclado_ascii()`, que
desfaz a composição (á → `'a`).

```c
static void ao_teclar(const char* texto, uint8_t codigo, uint8_t mod)
{
  if (codigo == TAB5_TECLA_ENTER) { enviar(); return; }
  if (codigo == TAB5_TECLA_APAGAR) { apagar_ultimo(); return; }
  acrescentar(texto);            // vazio quando foi tecla especial
}
```

`tab5_teclado_presente()` diz se há teclado encaixado. Se o seu app tem teclado
virtual, use isso para não roubar meia tela — **mas então implemente
`ao_teclar`**, senão não sobra nenhuma forma de escrever.

---

## Listas roláveis: use `kernel/list` (padrão, toque)

Não escreva rolagem à mão, nem a conta de "qual item o dedo tocou". Toda lista
do sistema (Mesh: mensagens, conversas e nós; Arquivos; Tarefas) usa a mesma
peça. O padrão é o da LVGL: **o item é medido uma vez, quando muda, e a lista
guarda o resultado.**

```c
static lista_t s_l;
static int  altura_de(int i, void* ctx);                  // lê do SEU cache
static void desenhar_item(lgfx::LovyanGFX& d, int i, int x, int y, int largura, void* ctx);

// ao abrir
lista_definir(&s_l, ROLAGEM_INICIO /* ou ROLAGEM_FIM */, n, altura_de, desenhar_item, nullptr);
s_l.topo = 12;                          // opcional: respiro antes do 1o item
// itens mudaram (entrou, saiu, re-medido)
lista_itens_mudaram(&s_l, n);
// UM item mudou de valor, mesma altura (idade, contador)
lista_item_mudou(&s_l, i);
// ao_tocar_nivel
if (lista_arrastar(&s_l, x, y, pressionado)) { sistema_pedir_redesenho(); }
// ao_tocar
int dy;
const int i = lista_item_tocado(&s_l, x, y, &dy);   // -1 = nada; LISTA_ARRASTE = foi rolagem
// desenhar
lista_desenhar(&s_l, d, janela, cor_de_fundo, pintura_completa(&s_p));
// a tela da lista saiu (outra página do app, arquivo aberto por cima)
lista_esconder(&s_l);
```

Regras:

- **A lista é dona da janela.** Ela limpa, recorta, rola, desenha a barra
  (8 px à direita — não desenhe item ali) e só repinta a faixa exposta ou os
  itens marcados. Não chame `tab5_tela_limpar` na janela dela.
- **O item desenha só dentro da própria altura.** Enfeite que sobe acima do
  item (um contador no canto) precisa de respiro dentro do item; a lista só
  repinta o item que cruza a faixa, e o que sai dele fica sem dono.
- **O toque vale só onde a lista desenhou por último.** Ela converte sozinha
  as coordenadas da área, e a janela já exclui o que outra coisa cobre
  (teclado, barra de escrita). Numa troca de página, `lista_esconder` nas
  listas que saíram.
- **Moldura e cabeçalho ficam fora da janela.** Desenhe-os na passada
  completa; a lista nunca os alcança.
- **Nunca meça texto dentro de `desenhar_item`**: meça ao montar o seu cache
  (ver `construir_layout` no app Mesh).

`kernel/scroll` é o motor por baixo da lista e da rolagem da área inteira
(`altura_conteudo`). App não usa `rolagem_*` direto.

## App de mensagens: use `kernel/conversation`

Baloes, barra de escrita, teclado virtual e fisico, rolagem e divisor de dia
sao um componente do sistema - nao copie o app Mesh. O app responde tres
perguntas (quantas mensagens, como e a mensagem i, envie este texto) e repassa
desenho e toque:

```c
static conversa_t s_c;  static pintura_t s_p;
ao_abrir:        conversa_abrir(&s_c, &s_p, contar, ler, enviar, nullptr);
ao_fechar:       conversa_fechar(&s_c);
ao_aviso:        conversa_mudou(&s_c);
desenhar:        pintura_iniciar(&s_p);
                 conversa_desenhar(&s_c, d, retangulo, pintura_completa(&s_p), pode_escrever, motivo);
ao_tocar / ao_tocar_nivel / ao_teclar: conversa_tocar / _tocar_nivel / _teclar
```

O retangulo vai ate a base da area do app (o teclado abre por cima da parte de
baixo). Exemplo completo e curto: `app_espnow/app_espnow.cpp`. O teclado
virtual mora em `kernel/virtual_keyboard` (ver a seção 9) e a quebra de linhas e
`conversa_quebrar_linhas` - o Mesh ja usa as duas.
