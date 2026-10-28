# M5tabOS — kernel e shell

Documento de arquitetura. Descreve o que **existe hoje**, o que **falta para
chamar de kernel**, e um plano em etapas. Cada etapa é independente: dá para
parar em qualquer uma e o sistema continua de pé.

Escrito depois de medir. Onde há número, ele veio do código ou do `sdkconfig`,
com o arquivo e a linha. Onde não medi, está dito que não medi.

---

## 1. Onde estamos

O sistema já tem um kernel — ele só não está reunido num lugar.

**Já é kernel, e está bem:**

| peça | arquivo | o que faz |
|---|---|---|
| ciclo de vida dos apps | `app_task.cpp` (84 l.) | cria e destrói a tarefa de cada app |
| energia | `tab5_power.cpp` (1248 l.) | perfis, INA226, repouso, teto de clock |
| notificações | `notification.cpp` | serviço do sistema para apps e serviços: registro idempotente por nome, estado por fonte, 4 níveis, destaque único na barra - ver WRITING_APPS.md §8.2 |
| preferências | `settings.cpp` (183 l.) | persistência em NVS (brilho, volume, wifi, grove, tela, relógio, casa) |
| partes de fundo | `kernel/background.cpp` | trabalho de app que sobrevive à janela: tarefa, pastilha, perfil somado, parada limpa (1º cliente: Trilha, `app_trilha/trilha_background.cpp`) |
| log persistente | `tab5_log.cpp` (183 l.) | hook do vprintf, buffer, descarga diária no cartão |
| orçamento de pintura | `paint.cpp` | limita repintura por frame |
| trava preguiçosa | `tab5_mutex.hpp` | mutex criado sob demanda - usado por 4 módulos que antes reescreviam a mesma lógica |
| drivers | `tab5_screen`, `tab5_sd`, `tab5_grove`, `tab5_keyboard`, `tab5_camera` | um dono por periférico |

**ETAPA 1 FEITA.** `sistema.cpp` chegou a 2457 linhas fazendo
oito coisas diferentes (agenda o desenho, roteia o toque, desenha a home,
desenha a barra de status, gerencia o ciclo de vida dos apps, aplica
política de energia, corrige a calibração do toque, e — desde mais cedo no
mesmo dia — faz o marshaling de chamadas HAL entre tarefas para a câmera) e
foi partido em dois, **sem mudar uma linha de lógica** (a única exceção,
documentada em `kernel/internal.hpp`, é uma chamada trocada por sua
equivalente pública):

| arquivo | linhas | responsabilidades | itens acima |
|---|---|---|---|
| `kernel/core.cpp` | 1571 | o motor - agenda de desenho, ciclo de vida dos apps, política de energia, ponte da câmera | 1, 5, 6, 8 |
| `kernel/shell.cpp` | 960 | a aparência - home, barra de status, roteamento de toque, calibração | 2, 3, 4, 7 |
| `kernel/internal.hpp` | 78 | a ponte privada entre os dois - nunca incluída por um app | — |

`sistema.cpp` foi renomeado para `sistema.cpp.old` até o comportamento ser
confirmado em hardware - confirmado em várias sessões de teste, o arquivo foi
removido de vez. As ~24 referências a "sistema.cpp" que
sobravam em comentários de outros arquivos (apontando para código que agora
mora em `core.cpp`/`shell.cpp`/`internal.hpp`) foram corrigidas no mesmo dia -
as poucas que restam citando "sistema.cpp" são história de verdade (descrevem
o arquivo como ele era ANTES da Etapa 1), não ponteiros esquecidos.

O item 8 (a ponte da câmera) ficou dentro de `core.cpp` por ser ciclo de
vida/HAL, não desenho. A pergunta que tinha ficado em aberto - "e se um som
precisar tocar de uma tarefa de fundo, o padrão de ponte se repete?" - já
tem resposta: **não.** O som apareceu como terceiro caso
(investigando por que a trava de PM `i2s_driver` nunca soltava, ver 2.2.1) e
pediu uma forma completamente diferente da ponte - ver 1.1, abaixo. Continuam
existindo só as duas cópias específicas da câmera; a lição não foi "faltou
generalizar a ponte", foi "nem todo recurso shared quer ser
shared da mesma forma".

### 1.1 Recursos que dormem por conta própria — três formas, não uma

Catalogadas ao decidir o que fazer com o alto-falante. Não é
uma abstração nova para construir - é o nome do que **já existe**, espalhado
pelo código, e a pergunta certa antes de escrever qualquer serviço novo de
segundo plano: qual das três (ou nenhuma) esse recurso precisa?

| padrão | quem usa hoje | formato do uso |
|---|---|---|
| **timeout de ociosidade** | alto-falante (`kernel/core.cpp`, `s_speaker_ocioso_desde`) | rajadas curtas, ninguém guarda o recurso aberto - liga sozinho no primeiro uso, desliga sozinho depois de alguns segundos quieto |
| **contagem de empréstimo** | cartão SD (`tab5_sd_montar()`/`tab5_sd_desmontar()`, referência contada) | sessão explícita, vários pedem ao mesmo tempo, só solta quando o último devolve |
| **dono único** | Grove (`tab5_grove_dono()`), ponte da câmera | só um consumidor por vez faz sentido, a posse passa de um para outro explicitamente |

Existe uma quarta resposta, tão válida quanto as três: **não fazer nada.** O
IMU (`tab5_imu_medir()`, 1×/s, acordado ou em repouso - `kernel/core.cpp`) não
segue nenhum dos três de propósito: uma transação I²C por segundo é barata o
bastante para não valer o rastreio de "quem está usando agora". Generalizar
cedo demais teria forçado essa mesma complexidade num lugar que não precisa
dela.

Quando a Etapa 2 trouxer serviços de verdade em segundo plano, a pergunta
"qual desses quatro" entra no design de cada um - em vez de inventar um
mecanismo genérico sem um quarto caso real para validar o formato.

---

## 2. O que falta para ser kernel

### 2.1 Não existe queue de eventos — e é isso que impede a autonomia

Hoje **tudo é sondado**. Os serviços levantam bandeiras (`s_sujo_dados`,
`s_msgs_sujo`, contadores) e a interface lê essas bandeiras de tempo em tempo:

```
PERIODO_UI    = 20 ms   sistema.cpp:48    em uso
PERIODO_OCIOSO = 50 ms  sistema.cpp:58    3 s sem toque
PERIODO_ZZZ   = 80 ms   sistema.cpp:75    tela apagada
```

Repare na última linha. **Com a tela apagada, a interface ainda acorda 12,5
vezes por segundo** para perguntar se aconteceu alguma coisa. Na esmagadora
maioria das vezes, não aconteceu.

Uma tarefa que acorda 12 vezes por segundo impede o processador de dormir de
verdade. Não importa o quão curto seja o trabalho: o custo é o *acordar*.

O formato certo é o inverso: a interface **bloqueia numa queue** e alguém a
acorda quando há motivo.

> Verifiquei uma coisa antes de escrever isso, porque mudaria o diagnóstico: o
> toque tem pino de interrupção, **GPIO 23**, e o M5GFX já o usa para não fazer
> leitura I²C quando não há dedo (`Touch_GT911.cpp:174`). Ou seja, a sondagem
> ociosa hoje é mais barata do que parecia. O desperdício não está no I²C
> — está no acordar. Mas significa também que o pino já está configurado e a
> etapa 2 abaixo tem onde se apoiar.

### 2.2 A máquina de economia existe e **está desligada**

Esta é a descoberta que mais importa:

```
sdkconfig:2951        # CONFIG_FREERTOS_USE_TICKLESS_IDLE is not set
sdkconfig.defaults:88 # CONFIG_FREERTOS_USE_TICKLESS_IDLE=y     ← comentado
```

Sem *tickless idle*, o tique do FreeRTOS a 1000 Hz (`CONFIG_FREERTOS_HZ=1000`)
acorda os dois cores **mil vezes por segundo, para sempre**. E como
consequência:

- `esp_pm_configure()` recebe `light_sleep_enable` (`tab5_power.cpp:84`), mas
  ele **não tem efeito nenhum** — não há janela ociosa longa o bastante.
- Nenhum perfil chega a pedir: `light_sleep_ocioso` é `false` no padrão
  (`tab5_power.hpp:109`) e **ninguém atribui outro valor**.

Traduzindo: o suporte a light sleep no `tab5_power` é **código morto hoje**.
Escrevemos a peça e nunca ligamos a chave.

### 2.2.1 As travas de PM — e por que a notícia é melhor do que parece

Fui verificar quem, além do tique, impede o processador de dormir. No IDF a
economia toda passa por **travas de PM**: um driver que precisa de clock estável
cria uma trava na inicialização e a segura. Enquanto houver uma trava viva, o
sistema não desce para o modo que ela proíbe — sem aviso nenhum.

O painel MIPI-DSI segura **duas**, e as duas são adquiridas na criação e
liberadas só no destrutor (`esp_lcd_panel_dpi.c` 291→405,
`esp_lcd_mipi_dsi_bus.c` 72→151):

```
dsi_dpi   CPU_FREQ_MAX     prende a CPU em 360 MHz
dsi_phy   NO_LIGHT_SLEEP   proíbe light sleep
```

Isto seria fatal — com o painel vivo, nem tickless idle nem `esp_pm` mudariam
coisa alguma. **Mas já está resolvido**: `DESMONTAR_DSI_NO_REPOUSO = true`
(`tab5_power.cpp`) destrói o painel ao entrar em repouso, e com ele as duas
travas caem. Isso foi medido com `esp_pm_dump_locks()`, depois de
duas tentativas mais baratas terem sido testadas e reprovadas (`set_pattern` e
baixar o clock com o fluxo ligado — os dois estão documentados no fonte).

Ou seja: **o obstáculo mais difícil da etapa 3 já foi derrubado meses atrás.**
A etapa 3 é mais promissora do que a seção acima faz parecer.

**O suspeito que sobra** é o UART. O driver cria `ESP_PM_APB_FREQ_MAX` enquanto
estiver instalado (`uart.c:1922`), e APB_FREQ_MAX **também** impede o light
sleep, porque dormir pararia o APB. O Grove do Meshtastic mantém um UART
instalado o tempo todo.

Aí está a tensão real da etapa 3, e ela não é técnica, é de projeto: **não dá
para dormir e escutar o rádio ao mesmo tempo.** Ou o Meshtastic perde mensagens
enquanto o tablet dorme, ou o tablet não dorme. Existe meio-termo no hardware
(acordar pelo bit de início do UART), mas é uma decisão de projeto em aberto.

**Atualização — uma trava nova, achada e corrigida; outra,
explicada.**

Rodar `energia travas 40` com a tela genuinamente apagada (confirmado pelo
log: `repouso apos 30 s`, `DSI desmontado`, clock `360 -> 40 MHz`) revelou uma
trava que a lista acima não previa: `i2s_driver` (`APB_FREQ_MAX`), presa
desde o primeiro `tone()`/`playWav()` de qualquer sessão, **para sempre** -
tocando som ou não. Causa: no ESP32-P4 (codec ES8388 externo por I2S "de
verdade", sem o DAC interno da família ESP32 clássica), a biblioteca
M5Unified só chama `i2s_stop()` no ramo ocioso da `spk_task` dentro de um
`#if M5UNIFIED_I2S_ADC_DAC` que nunca é compilado para este chip
(`managed_components/m5stack__m5unified/src/utility/Speaker_Class.cpp:615`).
Não vale remendar um componente vendorizado - perde-se na próxima
atualização. A correção ficou do nosso lado, pela API pública:
`M5.Speaker.end()` chamado por um watchdog de ociosidade em `kernel/core.cpp`
(primeiro uso real do padrão "timeout de ociosidade" da seção 1.1). Confirmado
com `energia travas 40` de novo: a trava não aparece mais na lista depois do
prazo de ociosidade passar.

A trava `rtos0` (`CPU_FREQ_MAX`, core 0) segue presa mesmo depois desse
conserto - e essa é esperada, não é bug. É a trava interna do próprio
`esp_pm` (`s_rtos_lock_handle`, `pm_impl.c`) que só solta quando a tarefa
IDLE daquele core roda, e é repega no instante de qualquer interrupção -
ou seja, é o termômetro de quão realmente ocioso o core 0 está. Com `log`
consumindo 10,3% de CPU ali (medido com `tarefas`) mais o `esp_timer` do
`energy_log.cpp` acordando a cada ~15s, o core 0 nunca fica parado por
trechos longos o bastante. É a mesma sondagem descrita em 2.1, vista agora
pelo lado da trava em vez do período de espera - confirma, não substitui, o
motivo de a Etapa 2 vir antes da Etapa 3.

### 2.3 Prioridades e cores foram decididos um de cada vez

```
ui         prio 4   pilha 8192    núcleo 1     sistema.cpp:2455   fixa (única pinada)
gps        prio 3   pilha 4096    qualquer     tab5_gps.cpp:265
mesh       prio 3   pilha 4096    qualquer     tab5_mesh.cpp:2645
wifi       prio 3   pilha 4096    qualquer     tab5_wifi.cpp:550
cam        prio 3   pilha 8192    qualquer     tab5_camera.cpp:406   só enquanto o app Câmera está aberto
trilha     prio 2   pilha 4096    qualquer     tab5_trilha.cpp:197   serviço, sobrevive ao app fechar
NMEAParse  prio 2   pilha 4096    qualquer     app_azimutal.cpp:367  só com o Azimutal aberto
console    prio 1   pilha 12288   qualquer     console.cpp:2196
registro   prio 1   pilha 4096    qualquer     energy_log.cpp:271
log        prio 1   pilha 6144    qualquer     tab5_log.cpp:179
tarefas    prio 1   pilha 4096    qualquer     app_tasks.cpp:240   só com o app Tarefas aberto
sd_monta/
desmonta   prio 1   pilha 4096    qualquer     sistema.cpp:1361/1427/1505   curta, uma vez por abertura de app
grove_sonda prio 1  pilha 4096    qualquer     sistema.cpp:1672      curta, uma sondagem
```

Atualizada - a primeira versão da lista tinha 5 linhas; hoje tem 12. Todo
serviço novo (trilha, log, câmera) entrou nas prioridades já estabelecidas
(3 para rádio/sensor em tempo real, 2 para trabalho de fundo sem pressa, 1
para plumbing do kernel) sem precisar inventar uma faixa nova - o esquema de
original aguentou o crescimento.

Por prioridade está coerente: a interface (4) preempta os serviços (3), então
nenhum serviço interrompe um frame no meio. Isso está certo.

O que **não** foi decidido, foi herdado: `tskNO_AFFINITY` em todos os serviços.
Eles flutuam entre os cores. Fixá-los no core 0 deixaria o core 1 só para
a interface — o que ajuda quando quisermos desligar um core inteiro no
repouso.

**Não vou fazer isso agora.** O ganho é plausível e não foi medido, e otimizar
sem medir é exatamente o erro que já nos custou tempo aqui.

---

## 3. O instrumento de medida já existe

Antes de mexer, vale medir — e dá para medir hoje:

```
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64=y
```

O comando `tarefas` (e o app Tarefas) já mostram **CPU% real por tarefa**, por
delta do `ulRunTimeCounter` — não é estimativa.

São **duas** medidas, e elas respondem perguntas diferentes.

**1. Quanto há a ganhar** — deixe a tela apagar e rode `tarefas`. Olhe a linha
do **IDLE**: essa porcentagem é o teto do que o light sleep pode economizar. Se
estiver em 97%, há muito a ganhar. Se estiver em 60%, alguém está trabalhando no
escuro e o conserto é outro — achar quem.

**2. Se o caminho está livre** — `energia travas 30` (comando novo). Ele espera
30 s para a tela apagar e só então despeja a tabela de travas de PM com
`esp_pm_dump_locks()`. Não toque na tela durante a espera.

O que ler no despejo:

- **vazio, ou só travas soltas** → caminho livre, a etapa 3 vale a tentativa;
- **`dsi_dpi` ou `dsi_phy` presos** → o desmonte da tela não aconteceu, e é
  isso que tem que ser investigado antes de qualquer outra coisa;
- **`uart_driver` preso** → é o Grove do Meshtastic, e aí a decisão de projeto
  da seção 2.2.1 vem antes do código.

A segunda medida é a que importa mais. A primeira diz quanto se ganharia; a
segunda diz se dá para ganhar.

---

## 4. O plano, em etapas

### Etapa 1 — fronteira sem mudar comportamento ✅ FEITA

Partir `sistema.cpp` em dois, **sem alterar uma linha de lógica**:

```
kernel/core.cpp    agenda de desenho, ciclo de vida dos apps, política de energia
kernel/shell.cpp   home, barra de status, navegação, roteamento de toque, calibração
```

(Nomes em inglês, os termos padrão de SO: `core.cpp` é o miolo do kernel, e
"shell" é a camada
visual em volta dos apps - ver o aviso grande em `kernel/internal.hpp` para
a distinção com o shell de linha de comando do `console.cpp`.)

Ganho: zero em desempenho, muito em conseguir enxergar - confirmado, o
binário final ficou a ~96 bytes de diferença do anterior (só a indireção
extra de algumas chamadas cruzadas). Um arquivo de 2457 linhas com oito
responsabilidades e estáticas compartilhadas é onde os bugs moram. A costura
entre os dois é `kernel/internal.hpp`: poucas variáveis `extern` e algumas
funções que cruzam de um lado para o outro, nunca exposto a um app.

### Etapa 2 — a queue de eventos (é aqui que está o ganho)

**PRIMEIRA FATIA FEITA.** Uma queue única, criada em
`sistema_iniciar()` e consumida no fim de `vTaskInterface()`:

```c
typedef enum {
  EV_TOQUE,     // da ISR do GPIO 23 - so "acorde e confira M5.Touch de novo"
  EV_SERVICO,   // sistema_acordar_interface() - quem publicou algo novo
} evento_t;
```

O loop da interface deixou de ser `vTaskDelay(periodo)` e passou a ser
`xQueueReceive(fila, &ev, periodo)` - o conteúdo/motivo do retorno é
ignorado de propósito nesta fatia: as próprias linhas seguintes do loop já
conferem `M5.Touch` e todas as bandeiras sozinhas, exatamente como sempre
fizeram. **Nenhuma lógica de decisão existente mudou** - só a forma de
esperar.

O que muda na prática, medido e não só esperado:

- **Em uso e ocioso com a tela acesa:** comportamento IDÊNTICO a antes -
  mesmo `PERIODO_UI`/`PERIODO_OCIOSO`, só trocando `vTaskDelay` por
  `xQueueReceive` com o mesmo valor. A ISR do toque fica **desligada** nesses
  dois estados (ver risco 2 abaixo) - zero mudança observável.
- **Tela apagada (repouso):** o teto deixou de ser `PERIODO_ZZZ=80ms` fixo e
  virou "quanto falta para o próximo prazo real que o loop já respeitava" -
  o menor entre a amostragem de sensores (1000ms), a barra/beep (`BARRA_MS`)
  e a vigia do Grove (`VIGIA_MS`, só quando ela de fato avança). Na prática
  o loop passa a acordar sozinho por volta de **1x/s em vez de 12,5x/s** -
  uma ISR de verdade no pino de toque (GPIO 23, que não tinha nenhuma até
  hoje - só um nível lido de forma síncrona, ver `Touch_GT911.cpp:174`)
  acorda a queue na hora certa se um dedo tocar antes disso.
- **Um serviço publicando enquanto o aparelho dorme:** `notification.cpp`
  virou o primeiro produtor ativo (`sistema_acordar_interface()`, chamado
  de dentro de `notif_publicar()` só quando algo muda de verdade - a mesma
  guarda que já existia para `notif_versao()`). Mesh e Trilha ganham isto de
  graça, sem saber que a queue existe: uma mensagem chegando com a tela
  apagada não espera mais o próximo prazo de ~1s para o beep tocar.

**Ficou de fora desta fatia, de propósito:**

- **Mesh/GPS/Trilha não tocam na queue diretamente.** Não precisam - os três
  já avisam pela via de `notif_publicar()`, e ganham o benefício acima sem
  mudar uma linha própria. Convertê-los diretamente multiplicaria arquivos
  tocados sem ganho adicional.
- **`PERIODO_OCIOSO` (acordado, sem toque há 3s) continua fixo em 25ms**,
  não virou prazo-adaptativo. Daria para fazer com a mesma conta de prazos
  já escrita, mas exigiria também ligar/desligar a interrupção de toque
  exatamente nos limites de um arraste em andamento - o GT911 pulsa o pino
  a cada 5-20ms enquanto há dedo na tela, e a ISR ligada nesse estado faria
  a queue nunca esvaziar, quebrando o próprio limite de velocidade que
  `PERIODO_UI`/`PERIODO_OCIOSO` existem para impor. O ganho ali (40Hz→~16Hz)
  é pequeno perto do repouso (12,5Hz→~1Hz) e não valeu a complexidade extra
  agora.

**Dois riscos achados na revisão do desenho, antes de escrever a primeira
linha** (um agente de plano validou o desenho e achou os dois - nenhum é
hipotético):

1. *Loop girando sem parar.* O prazo da vigia do Grove só avança quando
   `ajustes_grove() && !s_grove_resondar`. Sem essa guarda no cálculo do
   mínimo, Grove desligado (configuração normal) ou uma re-sondagem em
   andamento deixariam esse prazo preso em zero para sempre - a queue nunca
   bloquearia, e o core giraria a ~100% no exato estado que deveria
   dormir. Corrigido: o prazo do Grove só entra na conta quando as duas
   condições valem; caso contrário é excluído do `min()`, não tratado como
   "já venceu".
2. *A ISR não pode alimentar a mesma queue em todo estado.* Ver o item
   "PERIODO_OCIOSO" acima - a interrupção nasce desligada e só é ligada
   (`gpio_intr_enable`) no instante em que `tab5_power_repouso()` tem
   sucesso, desligada (`gpio_intr_disable`) no instante em que
   `tab5_power_retomar()` é chamado.

**Falta para chamar de "Etapa 2" completa** (a próxima fatia, não decidida
ainda): converter Mesh/GPS/Trilha em produtores diretos se algum dia
precisarem acordar a interface por um motivo que não passe por
`notif_publicar()`, e decidir se `PERIODO_OCIOSO` vale a complexidade do
enable/disable fino durante um arraste.

### Etapa 3 — tickless idle e light sleep

Ligar `CONFIG_FREERTOS_USE_TICKLESS_IDLE` e fazer o perfil de repouso pedir
`light_sleep_ocioso = true`.

**Só faz sentido depois da etapa 2.** Com a primeira fatia feita,
o repouso já acorda por volta de 1x/s em vez de 12,5x/s - a janela para
tentar `CONFIG_FREERTOS_USE_TICKLESS_IDLE` existe agora de um jeito que não
existia antes. Ainda vale medir de novo (`tarefas`/`energia travas`) antes de
ligar a chave: o ganho da queue de eventos é teórico até ser confirmado em
hardware, e os obstáculos abaixo (UART do Grove, SDIO do esp_hosted) não
mudaram nada com esta fatia.

**Os obstáculos honestos**, em ordem de quanto preocupam:

1. **O UART do Grove** (seção 2.2.1). É decisão de projeto antes de ser código:
   dormir custa mensagens do Meshtastic. Rode `energia travas 30` para ver se
   ele aparece mesmo na lista.
2. **O SDIO do esp_hosted.** Link ativo com light sleep é combinação delicada.
   Vale dizer que o próprio esp_hosted traz um exemplo pronto para isto
   (`examples/power_save/host+cp/…`), o que sugere que é caminho previsto e não
   terreno virgem. Mas não dá para testar com o C6 mudo.
3. **O painel DSI.** Já resolvido — ver 2.2.1. Fica aqui só para registrar que
   foi verificado, não esquecido.

---

## 5. Resumo em três linhas

1. O kernel já existe espalhado e está razoável; o que não existe é **queue de
   eventos**, e é ela que trava a autonomia.
2. `sistema.cpp` misturava core e shell em 2457 linhas — a Etapa 1 já separou
   (ver `kernel/core.cpp`/`kernel/shell.cpp`). Não acelerou nada,
   mas agora é seguro mexer na costura para a Etapa 2.
3. A economia de energia está **escrita e desligada** (`sdkconfig:2951`) — mas
   o obstáculo mais difícil (as travas do painel DSI) já foi derrubado em
   . Antes de ligar: `tarefas` para o teto do ganho, e
   `energia travas 30` para ver se o caminho está livre.

**Ordem recomendada:** medir → ~~etapa 2~~ → ~~etapa 1~~ (feita) → etapa 3.
Etapa 1 acabou saindo antes da 2 (por conveniência, não por prioridade de
ganho) - não muda a lógica original: o ganho de verdade ainda está na
Etapa 2, e ela segue sendo o próximo passo de código depois de medir.

Etapa 2 antes da 1 porque o ganho está nela, e a 1 é higiene. Se um dia
tivermos que parar no meio, é melhor parar com o sistema econômico e o código
bagunçado do que com o código arrumado e o sistema comendo bateria.
