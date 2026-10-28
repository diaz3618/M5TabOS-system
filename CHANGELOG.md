# Mudanças

O formato segue [Keep a Changelog](https://keepachangelog.com/pt-BR/1.1.0/),
e as versões seguem [SemVer](https://semver.org/lang/pt-BR/). A versão do
firmware mora em `version.txt`. As regras de qual número subir, e como
lançar uma versão (`tools/release.sh`), estão em `VERSIONING.md`.

## [Não lançado]

### Adicionado
- Versionamento: `tools/release.sh patch|minor|major` lança uma versão
  (version.txt, seção no CHANGELOG, commit e tag), com as regras em
  `VERSIONING.md`. O número no binário diz de onde o build saiu
  (`0.7.0+66.16a4d6d` = 66 commits depois da 0.7.0), e o build para se o
  `version.txt` ficar abaixo da última versão lançada.
- Mesh > Nó > Localização: compartilhar a localização liga e desliga (é a
  precisão da posição no canal principal do nó: não compartilhar, exata ou
  aproximada de ~90 m a ~6 km), enviar também ao se mover (smart broadcast),
  GPS do próprio nó (ligado/desligado; "a placa não tem GPS" quando não tem),
  e o GPS do Tab5 para o nó, junto da posição fixa e do intervalo de envio.
- Modo pendrive: o cartão SD aparece no computador como um disco USB pelo
  USB-C, em segundo plano — o Tab5 segue usável. Liga e devolve em
  Ajustes > USB; enquanto durar, a barra mostra "Cartão no computador".
  Ejetar no computador devolve o cartão sozinho; devolver no Tab5 sem o
  computador ter ejetado pede confirmação. Durante o pendrive o cabo não é
  console nem gravação, a CPU fica no clock máximo e, com o disco montado, a
  tela não apaga.
- Diário de gravações na flash interna (partição `reserva`, 2 MB): com o
  cartão no computador, o que o sistema grava (log, conversas, energia,
  nós do Mesh, redes Wi-Fi) vai para o diário e chega ao cartão na volta, na
  ordem em que aconteceu — inclusive depois de um reinício no meio. Se o
  computador mudou um arquivo que o Tab5 também reescreveu, o do computador
  fica e o do Tab5 vai para `<nome>.tab5`. A barra avisa quando o diário
  passa de 90%; a página USB mostra o que se perdeu. **Exige regravar com
  `./flash.sh`** (tabela de partições nova; os ajustes ficam).
- Ajustes > USB: ligar o pendrive sozinho ao conectar num computador
  (desligado de fábrica; com um app aberto ou a tela bloqueada, só avisa).
- `tab5_sd_acrescentar()` e `tab5_sd_substituir()` — as portas de gravação
  do sistema —, `tab5_sd_emprestar()`/`tab5_sd_devolver()`
  (`hal/tab5_sd.hpp`), `app_t.exige_sd`, `sistema_manter_acordado()`,
  `sistema_bloqueado()` e `sistema_app_exige_sd()` (`kernel/app.h`).
- Central de avisos: tocar a barra de status abre uma lista com todos os
  apps que estão avisando (nome, resumo, contagem, cor da gravidade); tocar
  uma linha abre o app. Fecha tocando fora, na barra, com Esc ou deslizando
  para cima. A barra ganha um ponto colorido por app além do destaque, e a
  tela de bloqueio lista até 4 apps em vez de um só.
- `notif_listar()` e `notif_associar_app()` em `services/notification.h`.
- Mesh: rede americana em paralelo. Um segundo cliente MQTT
  (`mqtt.meshtastic.org`, raiz `msh/US`, LongFast de fábrica) roda junto do
  enlace de sempre — no modo internet, com o nó no cabo/Bluetooth e com o
  proxy. As mensagens vão para a conversa "LongFast EUA", embaixo de um traço
  "EUA" na coluna de conversas; os nós ficam marcados com a rede e a página
  de nós ganha os botões BRASIL / EUA, com o liga/desliga da rede americana
  (desligada de fábrica). Os EUA têm teto de 96 nós na tabela, não apitam
  nem entram na barra de status, e nó americano não tem CONVERSAR (só canal,
  sem PKI).
- Conversas: a hora de cada mensagem embaixo do balão, do lado de quem falou
  (Mesh e ESP-NOW).

### Mudado
- A hora subiu para o canto esquerdo da barra de cima (menos na tela de
  bloqueio, que já mostra a hora grande); a temperatura do P4 desceu para a
  faixa de baixo, com a média desde o boot. O termômetro enche e muda de cor
  (azul, verde, amarelo, laranja, vermelho) conforme a temperatura.
- O botão da faixa de baixo troca a palavra "INÍCIO" por um círculo, o
  símbolo de tela inicial.
- Mesh > Nó em grupos: a página abre numa lista curta (Nome e papel,
  Localização, Rádio LoRa, Conexões e internet, Avisos e telemetria,
  Manutenção), cada grupo com um resumo; tocar abre as opções dele juntas e
  VOLTAR sobe. O pendente vale para a página toda — mudar coisas em grupos
  diferentes ainda grava numa transação só (o nó reinicia uma vez).
- O GPS do Tab5 com o nó do outro lado vai **só para o nó**, como faz o app
  do celular: o nó a difunde no ritmo e com a precisão dele. Antes ia em
  difusão de 3 saltos e o nó repetia a mesma posição no ritmo dele — duas
  vezes o tempo de ar. Com posição fixa ou o GPS do nó ligado, o Tab5 não
  manda nada (o nó ignoraria ou atropelaria). Pela internet, sem nó, o Tab5
  respeita a precisão do canal que o nó deixou (0 = não manda).
- "Posição do GPS para a malha" saiu de Ajustes > Mesh: agora fica só em
  Mesh > Nó > Localização (dois lugares para a mesma coisa).
- Pôr ou tirar a posição fixa pede ao nó só a configuração de volta
  (want_config 69420), não a NodeDB inteira; firmware antigo manda tudo,
  como antes.
- Desligar o MQTT do nó leva o proxy (internet do Tab5) junto — o espelho
  da regra que já existia ao ligar o proxy.
- Arquivos e Câmera não abrem com o cartão no computador: o sistema mostra o
  motivo no lugar do app e abre sozinho quando o cartão volta
  (`app_t.exige_sd`), e o pendrive não começa com um deles aberto.
- Toda gravação de dados passa pelas portas do sistema (`hal/tab5_sd.hpp`);
  `tab5_sd_abrir()` fica para ler e para arquivo novo grande. Os serviços que
  reescrevem um arquivo inteiro (redes Wi-Fi, redes do nó, conversas
  apagadas, NodeDB) só o fazem depois de tê-lo lido — sem cartão, a mudança
  vale até reiniciar em vez de apagar o que estava guardado.
- Trocar um arquivo inteiro sobrevive a queda de energia no meio: quem for
  ler completa a troca interrompida (o conteúdo novo estava no `.tmp`).
- `sistema_manter_acordado()` acende a tela se o pedido chegar com ela
  apagada (o computador montando o disco com o Tab5 em repouso).
- A navegação travada (`sistema_travar_navegacao`) passa a valer também para
  a barra de status (não abre a central de avisos) e para pedidos de fora de
  trocar de app (`sistema_simular_abrir`, console).
- Emoji guardados com paleta (1 byte por pixel + as cores de cada um) em vez
  de RGB565 cru: de ~1,49MB para ~0,91MB de flash, com a mesma imagem pixel a
  pixel. O fundo transparente passa a ser o índice 0 da paleta, não mais a
  cor magenta.
- Emoji em texto menor que 24px encolhe para a altura da linha, em vez de
  invadir as linhas vizinhas.
- Teclado virtual no padrão do celular. Shift vale para UMA letra e solta
  sozinho; dois toques seguidos travam (caps lock), e a seta mostra o estado
  (contorno / cheia / cheia com barra). Na conversa a maiúscula arma sozinha
  no começo do texto e depois de ". ", "! " e "? " (senha, PIN e nome não).
  Shift e apagar foram para as pontas da fileira do "zxcv", como no Android,
  e no lugar do apagar entrou a tecla "?123": duas páginas de símbolos com
  parênteses, chaves, colchetes, `< > \ | ~ ^` e os acentos do português
  (á à â ã é ê í ó ô õ ú ç, maiúsculos com o shift). Espaço ou acento numa
  página de símbolos volta às letras. Vírgula e ponto não viram mais "?" e
  "!" com o shift, e a fileira dos números fica sempre números.
- Teclado virtual: um `tecvirt_editar()` só para os campos simples (senha do
  Wi-Fi, nome e Wi-Fi do nó) no lugar de três cópias do "põe no fim / tira o
  último"; o apagar deles tira o caractere UTF-8 inteiro, não um byte.
- Um codec Protobuf só (`services/mesh_pb.hpp`) no lugar de três cópias
  (`tab5_mesh`, `tab5_mesh_admin`, `tab5_mesh_mqtt`). A escrita confere o
  limite do buffer: o que não cabe é recusado, em vez de montado com
  ponteiro solto e bytes mágicos. Texto, admin e posição saem pelo mesmo
  envelope; o teste no Mac (`testes/rodar_mesh_pb.sh`, com ASan/UBSan)
  confere que os bytes são idênticos aos de antes.
- Parada de tarefa de serviço num mecanismo só (`kernel/service_stop.hpp`): GPS,
  Mesh e o enlace Bluetooth do Mesh esperam a saída num semáforo em vez de
  perguntar a cada 10 ms. No GPS as bandeiras nem eram `volatile`.
- Ouvinte de evento (`hal/tab5_listener.hpp`) para a varredura BLE, o farejo
  e o CSI: remover o ouvinte espera a chamada em voo. O Radar deixa de
  dormir 40 ms "para dar tempo" ao fechar.
- Troca de relógio do painel pedida pelo console: espera num semáforo em vez
  de perguntar a cada 10 ms.
- "Relógio acertado?" num lugar só (`tab5_hora_epoch()`), no lugar de doze
  cópias do mesmo número com quatro nomes.
- Ponte MQTT: os quadros para o serviço são montados direto no quadro
  estático da fila (a tarefa do cliente dos EUA tem 3,5 KB de pilha); o
  ToRadio é lido por uma função só nas duas saídas (Brasil e EUA).
- RAM interna: as pilhas das 6 tarefas do esp_hosted (`sdio_*`, `eh_rpc_*`)
  foram para a PSRAM (`CONFIG_ESP_HOSTED_DFLT_TASK_FROM_SPIRAM`). Eram 24 KB
  presos para ~8 KB de uso medido. Com WiFi + MQTT no ar, a RAM livre foi de
  202 para 227 KB, o mínimo de 157 para 182 KB e o maior bloco de 77 para
  102 KB. Com a tela acesa, a foto do monitor e os underruns não mudaram
  (~2,3 ms no pior caso, zero piscadas). Em repouso, durante varredura BLE,
  o pior caso subiu de ~25 para ~50 ms, mas o painel está desligado.
- `mem extrato` ganhou a coluna "mínimo" (o pico temporário dentro de cada
  etapa, que o delta esconde) e `tarefas` mostra a duração da foto do
  monitor (última e pior).
- Mesh: a cópia da NodeDB do Tab5 (31 KB de PSRAM) só existe enquanto grava
  ou lê o `nodedb.bin`; os nomes dos nós dos EUA não vão para o `nos.txt`; o
  cliente MQTT dos EUA usa buffers de 768/512 bytes e pilha de 3,5 KB (RAM
  interna).
- Mesh: emprestar a internet do Tab5 ao nó (MQTT Client Proxy do
  Meshtastic), na página Nó do app Mesh, linha "internet do Tab5 (proxy)",
  junto do "MQTT do nó" e com as mesmas regras da página: o toque deixa
  pendente, GRAVAR NO NÓ grava (ligar o proxy leva o MQTT junto, na mesma
  transação) e a linha mostra o que o nó confirmou, com o estado da ponte ao
  lado. O nó publica e assina pelo enlace com o Tab5 (Bluetooth ou cabo) e o
  Tab5 leva ao servidor pelo Wi-Fi: o nó vira gateway completo sem ligar o
  Wi-Fi dele, e o Grove fica livre para o GPS. O Tab5 só carrega bytes - quem
  cifra é o nó, então a mensagem direta com PKI também passa - e usa o
  servidor configurado no nó.
- Radar enxerga WiFi: o farejo do C6 entra como segunda fonte de alvos ao lado
  do BLE (roteadores, celulares procurando rede, aparelhos conectados), no
  mesmo ajuste de cosseno, na mesma busca quente/frio e com filtro próprio
  (TODOS / SÓ TAGS / SÓ WiFi). O farejo fica no canal em que o rádio já está.
- Radar mostra movimento pelo CSI: o serviço `tab5_csi` mede quanto o canal
  entre o roteador e o Tab5 varia (parado / leve / forte), aprende o "parado"
  de cada lugar e suspende a medida enquanto o próprio Tab5 se mexe.
- Serviço `tab5_csi`, dono único da mensagem de CSI (o peer_data guarda um
  ouvinte por mensagem): pede ao C6 só o CSI do roteador, pinga o roteador a
  20 Hz como fonte e custa zero parado. O `radio csi` do console passa por ele
  e mostra a medida de movimento.
- Firmware do C6 (contrato v3): a gravação passa a levar de uma vez tudo que
  depende do rádio, cada recurso ligado sob pedido do P4 e dormente até lá —
  ESP-NOW (texto, dois dialetos) e canal, farejador promiscuo em lotes, tubo
  cru de ESP-NOW, INFO, mais CSI, FTM e 802.15.4. Como regravar o C6 é físico
  (Downloader), as próximas evoluções acontecem do lado do P4 sem reabrir o
  aparelho. Cada capacidade anuncia um bit em `caps` (só acende se o módulo foi
  compilado), e o P4 esconde o que a gravação não trouxe.
- Serviço `tab5_sniffer` e comando `farejo` no console: o C6 ouve em modo
  promiscuo, resume por MAC e manda em lotes; o P4 guarda a tabela só enquanto
  ligado (na PSRAM, numa vaga do pool) e o C6 desliga sozinho se o P4 parar de
  renovar. `farejo info` mostra as capacidades de rádio do C6 gravado.
- Console serial reorganizado: os comandos moram numa tabela única, e o
  `help` é gerado dela, em sete grupos (Sistema, Tela, Cartão, Rede,
  Meshtastic, Bancada, Testes), com todas as formas de cada comando e
  exemplos. `help <comando>` explica para que serve; `help tudo` mostra tudo. Comando errado sugere os
  parecidos.
- Console: `estado` (antigo `servicos`, agora com versão, tempo ligado e RAM);
  `wifi` sozinho mostra o estado, mais `wifi redes`, `wifi gravar`,
  `wifi conectar` e `wifi esquecer`; `log` aceita nomes de nível
  (`log eh_sdio debug`) e mostra o nível atual; `ler <arq> [linhas]`;
  caminhos do cartão relativos a `/sdcard`; `mesh nos` e `mesh enviar`;
  `grove cru <baud>`; `tela prioridade alta|padrao`; `mem tudo`.
  Os nomes antigos (`servicos`, `redes`, `nos`, `enviar`, `grove <baud>`,
  `tela on|off`) continuam aceitos.
- Mesh: escolha entre três vias em Ajustes > Meshtastic (Cabo Grove,
  Bluetooth, Internet). O Grove tem prioridade: quando a sondagem acha o C6L no
  cabo, o Mesh passa para ele sozinho, de qualquer via; depois disso a escolha
  manual vale até o próximo achado.
- Mesh, página Nós: todos os canais ativos do nó no topo, como botões (o que
  não está na coluna aparece com "+" e volta ao ser tocado); tocar num nó abre
  o cartão dele - id, MAC (quando o nó envia), modelo, SNR, saltos, bateria e
  tensão, visto, posição com distância e rumo - com CONVERSAR e VOLTAR.
- Mesh: página "Nó" (engrenagem no trilho) para configurar o nó Meshtastic
  pelo Tab5, sem PC nem celular: nomes, papel, saltos, potência, intervalos de
  posição/apresentação/telemetria, MQTT e Bluetooth do nó. As mudanças ficam
  pendentes (amarelo) e "GRAVAR NO NÓ" manda tudo numa transação
  (`begin/commit_edit_settings`) - o nó grava e reinicia uma vez, e a tela
  mostra o que ele confirmou no dump seguinte. Ações imediatas: posição fixa
  pelo GPS do Tab5, acertar a hora do nó, reiniciar (com confirmação). Mostra
  também firmware, placa, região/modem, nós guardados (rádio · Tab5),
  reinícios e o último aviso do firmware (`ClientNotification`). O som das
  notificações saiu da página Malha para esta.
- Mesh: `services/tab5_mesh_admin` - set_config/set_module_config reenviam a
  seção crua do dump com os campos novos no fim (o protobuf fica com a última
  ocorrência), sem modelar o protobuf inteiro. Seções com senha ou chave
  (network, power, security) e o módulo serial nunca são reenviados.
- Mesh: NodeDB do Tab5 no cartão (`nodedb.bin`, PSRAM, 256 nós), carregada ao
  subir o serviço e gravada a cada 10 min e ao parar: o Tab5 lembra mais nós
  do que o teto do rádio (100 no C6L até a 2.7).
- `tools/test_mesh_admin.py`: decodifica com os protobufs oficiais os
  bytes que o M5tabOS monta (admin, texto, canais).
- Mesh: favoritos (estrela). No cartão do nó, FAVORITAR marca; a tabela mostra
  a estrela e põe os favoritos logo abaixo do nosso nó, na ordem em que foram
  marcados, sempre presentes e nunca despejados. São os favoritos do próprio
  Meshtastic: a marca vai ao nó (AdminMessage `set_favorite_node` /
  `remove_favorite_node`, admin local) e aparece no cliente web; o
  `is_favorite` do dump atualiza a lista daqui. Sem o rádio (modo internet) a
  mudança fica pendente e vai no próximo dump. Lista guardada na NVS.
- Mesh: marca cinza de "ouvida pela malha" na mensagem direta - o vizinho
  repetiu, o destinatário ainda não confirmou. O visto verde fica para o ACK do
  próprio destinatário.
- Home em páginas: com mais apps do que cabem na tela (12 hoje), arrastar para o
  lado troca de página e bolinhas embaixo mostram a atual. `TAB5_HOME_LINHAS_MAX`
  (kernel/internal.hpp) limita as linhas por página.
- Tela de bloqueio (Ajustes > Tela, desligada de fábrica): ao acordar mostra
  hora, data e o aviso em destaque; deslizar para cima desbloqueia e volta ao
  que estava aberto. Enquanto bloqueado, nada chega ao app por baixo.
- Mesh pela internet (MQTT) quando o nó não está conectado: com Wi-Fi e sem o
  C6L, as mensagens de canal chegam e saem pelo servidor MQTT. Sem servidor
  aprendido do nó, usa o do Mesh Brasil (`platform.meshbrasil.com`, raiz
  `meshdev`, canal LongFast com a chave padrão); a região do rádio não entra
  no tópico. Assina também as sub-raízes (`meshdev/<país>/<região>/...`) e os relatórios
  de mapa, que viram nome e posição de nós. Canais, chaves e servidor são
  aprendidos do nó quando ele está conectado.
- Ajustes > Meshtastic: botão LIGAR/DESLIGAR da internet e linha de estado
  (servidor e raiz em uso). O nó no Grove tem prioridade: com ele, a internet
  nunca roda, mesmo com o Wi-Fi ligado.
- `tools/teste_mqtt_vivo.py`: escuta o servidor real e decifra com o
  mesmo algoritmo, para conferir tópico e criptografia fora do aparelho.
- Ajustes > Wi-Fi: lista das redes em volta, senha digitada na tela, várias
  redes guardadas (cifradas) e conexão automática na conhecida mais forte.
- Ajustes > Bluetooth: tocar num nó Meshtastic conecta o Mesh a ele, e a
  página mostra o estado da conexão; nós reconhecidos também pelo nome.
- Hora pela internet (NTP) quando o Wi-Fi está conectado. Ordem de confiança:
  internet, GPS (só com fix), Mesh (só tráfego ao vivo); uma fonte pior não
  desfaz o acerto de uma melhor.
- Ajustes > Bluetooth reconhece rastreadores (rede Find My/AirTag, Tile,
  SmartTag, Google) e beacons (iBeacon, Eddystone), e mostra a distância
  estimada pelo sinal. Ajustes > Wi-Fi mostra a distância estimada de cada rede.
- Log: a hora acertada aparece no horário local, com o fuso, e diz em palavras
  quanto o relógio estava errado ("o relógio estava 1 s atrasado"). Antes saía
  em UTC ("18:19:01Z") com "relogio mudou +1s".
- Log: mensagens de boot e de ajustes reescritas para serem lidas sem o fonte -
  ligado/desligado no lugar de "=0/=1", o perfil de energia lista só o que
  está ligado, endereços de memória e nomes internos (peer_data, AXI, fb0,
  app_main) trocados pelo que importa para quem lê.
- Teclado virtual é um componente do sistema (`tecvirt_t`): conversa, senha do
  Wi-Fi e PIN usam o mesmo, com retorno da tecla e vários dedos. Desceu até a
  barra de baixo (sem a faixa morta; a navegação travada entrega o toque na
  fileira de comando), a letra sai uma amostra antes, e no Ajustes cada letra
  repinta só o campo em vez da página e do teclado inteiros.
- Toque por nível recebe a mesma correção vertical do clique (10 px).
- RAM interna: ~80 KB de variáveis estáticas dos apps e serviços (monitor,
  Trilha, Radar, MQTT, Mesh, ESP-NOW, tabelas de fonte) e o `.bss` do lwIP
  saem da SRAM para a PSRAM via `main/linker.lf`. O buffer do log (12 KB), o
  vetor de linhas da segunda página da tela e o retrato de tarefas do monitor
  também.
- Pilhas redimensionadas pelo pico medido em `pilhas-*.csv`: console 12→8 KB,
  WiFi e ESP-NOW 8→6 KB, MQTT 6→4 KB. A do `sys_evt` sobe de 2,3 para 3 KB
  (chegava a 600 bytes livres).
- mbedTLS aloca na PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`): uma sessão TLS
  não tira mais ~40 KB da interna de uma vez.
- Console: `mem` mostra o extrato da RAM interna (quanto cada etapa do boot e
  cada rádio consumiu) e o mapa de fragmentação por região do heap.
- Ajustes > Meshtastic: a assinatura de repintura vira um hash da página
  inteira; os estados longos (internet, proxy) no fim passavam do limite de
  comparação e a tela não acompanhava a mudança.
- Energia reformulado no molde das telas de bateria dos celulares. No topo, a
  porcentagem grande com o estado (na bateria / carregando / na tomada) e a
  estimativa pela média dos últimos 30 min ("dura ~6 h 30 min neste ritmo",
  "cheia em ~1 h 20 min"). Abaixo, um índice de seis cartões (bateria,
  corrente, potência, tensão, temperatura e gasto das 24 h); tocar num cartão
  escolhe o gráfico. Saem o título, a linha do registro (só aparece se houver
  falha de gravação), os MHz da CPU e a legenda de sinal.
- Energia, gráfico de nível: eixo com números redondos em cada linha da grade
  (antes só o mínimo e o máximo medidos, no canto), área preenchida, horas de
  carga tingidas de verde atrás da curva e um vértice por coluna de pixel (a
  média suaviza o ruído da corrente). Na corrente o preenchimento vai até o
  zero: verde acima, laranja abaixo. Tocar põe um cursor com a hora e o valor.
  O período 24 H é uma janela móvel (da hora cheia de 23 h atrás até agora,
  lendo os arquivos de ontem e de hoje) no lugar do HOJE de 0 a 24 h, que
  ficava vazio logo depois da meia-noite.
- Energia, gráfico novo de gasto por hora: barras de mAh nas últimas 24 h,
  separando tela acesa de repouso, com uma faixa verde nas horas em que houve
  carga e o mesmo eixo de tempo do gráfico de nível. Tocar numa barra mostra o
  detalhe da hora; sem seleção, os totais das 24 h.

### Corrigido
- Cartão: "caronas esgotadas" no boot. Cinco serviços gravam pela tarefa do
  cartão e só cabiam quatro; o último (as redes Wi-Fi do nó do Mesh, na
  ordem de hoje) nunca gravava.
- Mesh: mensagem direta para um nó desligado, com o C6L na internet, ganhava
  o check cinza ("outro nó ouviu") e nunca mostrava o X. Era a confirmação de
  que o pacote chegou ao servidor MQTT, tomada por confirmação de rádio.
- Wi-Fi não voltava sozinho ao chegar em casa: depois de desistir longe do
  roteador, a queda gerada pela própria parada do rádio trocava o estado
  FALHOU por DESLIGADO, e a retomada automática (que só olhava FALHOU) nunca
  mais procurava — só a procura de Ajustes > Wi-Fi reconectava. Agora a
  queda não apaga o motivo, a retomada age sempre que o Wi-Fi está ligado em
  Ajustes e sem rede, e acender a tela procura na hora. Um "conectando" sem
  resposta do C6 por 60 s também passa a desistir (e a retomada assume), e o
  `varrer` do console devolve a conexão no fim.
- CSI e Farejo: ligar logo depois de desligar podia criar duas tarefas
  (CSI) ou deixar o farejo desligado e sem tabela depois de "ligar" ter
  respondido sim. Os dois passam a usar a parada do kernel (`service_stop.hpp`).
- Mesh: canais e NodeDB não são mais gravados no laço do rádio (um cartão
  lento fazia perder quadros do nó); vão de carona na tarefa do cartão.
- Partes de fundo (Trilha, Azimutal): parar espera a saída num semáforo, sem
  o laço de 10 ms na interface; a máquina de estados própria deu lugar à
  mesma parada dos serviços.
- Energia e desempenho: a interface acorda por notificação de tarefa (não
  mais por fila); o teclado só é lido pelo I2C quando o pino de interrupção
  avisa (depois de provar que avisa); o GPS dorme na fila de eventos da UART;
  o ESP-NOW dorme 30 s sem enlace com o C6; Ajustes redesenha no ritmo de
  cada página; o papel de parede da home fica pronto enquanto se está nela.
- Memória: o Bluetooth não tem mais tarefa permanente (−4 KB de RAM
  interna); ligar/desligar usa uma vaga do pool.
- `tab5_dentro()` e `tab5_agora_ms()` no lugar das cópias nos apps.
- `sdkconfig.defaults`: `CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES=2`
  (num sdkconfig antigo o código funciona igual; para ganhar a separação,
  `idf.py menuconfig` ou apagar o `sdkconfig`).
- Mesh: parar o serviço apagava o mutex principal enquanto a barra da home
  e o app Mesh podiam estar segurando ou esperando por ele (toda troca de
  via e toda re-sondagem do Grove). Agora o mutex nasce uma vez e fica.
- Wi-Fi: a reconexão depois de uma queda dormia 1 s DENTRO da tarefa de
  eventos do sistema, congelando todos os eventos (IP, esp_hosted). A
  espera foi para a tarefa do Wi-Fi.
- Wi-Fi: ESP-NOW, CSI e Farejo alteravam a mesma máscara de "receptor
  ligado" sem operação atômica e podiam apagar o pedido um do outro.
- Texto cortado para caber agora corta por caractere (UTF-8) em todo o
  sistema: uma função só (`tab5_cortar_ao_caber`) no lugar de uma dúzia de
  cópias — três cortavam por byte e desenhavam lixo em palavras com acento
  (barra de status, faixa da home, nomes no Arquivos).
- Mutex criado sob demanda (`tab5_mutex_travar`) não tem mais corrida na
  primeira chamada; GPS, IMU, MQTT e Azimutal passam a usar ele.
- Pedidos do console para abrir/fechar app esperam o app terminar de abrir
  (antes chamavam `ao_fechar` de um app que nunca abriu).
- Energia: com a tela apagada, o IMU não é mais lido por I2C a cada segundo,
  o monitor amostra a cada 10 s e não espera quadro de painel desmontado, a
  tabela de travas de PM sai só no primeiro repouso, e a potência é
  calculada (V×I) em vez de lida do INA226.
- Ajustes: arrastar brilho/volume grava na flash só o valor final (1 s depois),
  e não a cada quadro do arraste.
- Memória: a "lixeira" de desenho (1,8 MB de PSRAM) é solta quando o painel
  volta; histórico do monitor e buffers de ordem da home saíram da RAM
  interna.
- Pendrive, cabo puxado sem ejetar: a sessão segue (o computador pode ter
  algo do cartão em cache), mas com o barramento parado por mais de 2 s a
  tela volta a apagar e a CPU sai do clock máximo — antes ficavam assim para
  sempre, na bateria. Ligado sem computador no cabo, o clock máximo sai
  depois de 10 s; volta quando o computador configurar o disco.
- Pendrive, ao devolver: se a tarefa do USB não parasse em 2 s, o cartão
  voltava ao Tab5 com ela ainda podendo estar no meio de uma leitura ou
  escrita (memória liberada em uso). Agora a volta espera ela parar.
- Pendrive: "devolver" pedido enquanto ele ainda preparava era descartado
  em silêncio; agora fica guardado e a sessão devolve assim que o USB sobe.
- A foto do cartão no empréstimo (48 KB de PSRAM) é solta quando o cartão
  volta, ou quando o empréstimo desiste — ficava presa até o reinício.
- Diário: aplicar um registro não põe mais 512 B de nomes na pilha de quem
  lê o cartão (os nomes vão no mesmo bloco da PSRAM que o registro).
- Vigia do cabo: com "ligar sozinho" desligado (o de fábrica) olha a cada
  1 s em vez de 250 ms.
- Wi-Fi volta sozinho para uma rede conhecida. Depois de 5 tentativas sem
  sucesso (fora de alcance, roteador reiniciando) o rádio desistia e nunca
  mais tentava, e a procura pela tela via a rede conhecida e desligava o
  rádio de novo. Agora, com o Wi-Fi ligado em Ajustes, ele procura de novo
  sozinho (30 s, 1, 2, 4 e depois a cada 5 min) e conecta na conhecida mais
  forte que aparecer; a procura pela tela também conecta.
- Mesh: se a tarefa do serviço não pudesse ser criada, a UART e o Grove
  ficavam com o Mesh até reiniciar. Agora são devolvidos.
- Ponte MQTT: os clientes do Brasil e dos EUA decifravam e montavam pacotes
  nos MESMOS buffers estáticos, cada um na sua tarefa, ao mesmo tempo — um
  podia sobrescrever o pacote do outro no meio. Cada cliente tem o seu par.
- Ponte MQTT: um quadro que não coubesse no buffer podia sair cortado em
  alguns caminhos (o tamanho era calculado mesmo com a escrita falhando).
- Aviso sumindo da barra sem abrir o app: com a tela bloqueada (ou acordando
  do repouso direto no bloqueio) o kernel entregava os avisos ao app que
  estava por trás, e o ESP-NOW marcava as mensagens como lidas sem ninguém
  ver. App encoberto não recebe mais aviso, toque nem tecla.
- Mesh: parar e subir o serviço (troca de via, Wi-Fi caindo no modo
  internet, Bluetooth caindo, C6L achado no cabo) zerava as não lidas e
  apagava o aviso. As não lidas de cada conversa ficam guardadas até serem
  lidas, e registrar a fonte de novo não zera mais o estado dela.
- Mensagens vindas da ponte do Telegram (e de qualquer celular) apareciam em
  quadrados: todo caractere fora da fonte que não é emoji vira o retângulo
  da M5GFX. O texto agora passa por `tab5_unicode` antes de desenhar — letra
  "estilizada" (𝗻𝗲𝗴𝗿𝗶𝘁𝗼, 𝓈𝒸𝓇𝒾𝓅𝓉) vira letra, bandeira vira "BR", tom de
  pele e caracteres invisíveis somem, acento combinado se junta, cirílico e
  grego são transliterados, e o que sobra vira "?". Testado no Mac:
  `testes/rodar_unicode.sh`.
- Mensagem com quebra de linha era cortada no histórico do cartão (uma
  mensagem por linha): a quebra vira espaço no recebimento.
- Mesh: enviar duas mensagens seguidas congelava a tela até 2,1 s (o limite
  de texto do nó era esperado na tarefa de interface). O texto vai para uma
  fila e a tarefa do serviço respeita o limite; a recusa que já dá para saber
  (Bluetooth fora, internet com direta, EUA desconectado) continua na hora.
- Mesh pelo cabo: a leitura da serial esperava 200 ms por quadro mesmo com
  ele já chegado (pedia 256 bytes). Acorda no primeiro byte e lê o resto.
- Mesh pelo Bluetooth: a leitura parava a cada 64 mensagens e só voltava no
  próximo aviso do nó ou em 5 s — o dump de uma malha grande andava aos
  soluços. Continua na volta seguinte; e o serviço esvazia o que chegou em vez
  de um quadro por volta.
- Mesh pela internet: fila esvaziada de 16 em 16 (era 1 por volta), descida
  do proxy de 8 em 8 (era 4), cópias do mesmo pacote de vários gateways
  descartadas antes de decifrar, e o aviso de fila cheia diz qual fila, uma
  vez a cada 10 s.
- Mesh: a tabela de nós era reordenada a cada linha desenhada quando chegava
  tráfego; agora no máximo a cada 250 ms, e o aviso de "nós mudaram" para a
  tela a no máximo 4/s (1/s vindo dos EUA).
- Mesh: anel de mensagens já vistas de 64 para 256 (a cópia atrasada pela
  ponte não entra de novo com o tráfego dos EUA passando junto).
- Mesh, página Nó: mudar o MQTT ou o proxy do nó pelo Bluetooth não gravava
  nem reiniciava o nó. O firmware desliga o Bluetooth assim que recebe a
  seção MQTT, mesmo no meio da transação, e o commit nunca chegava - a
  transação ficava aberta (o firmware não a expira) e o nó sem Bluetooth até
  ser reiniciado na mão. Pelo Bluetooth a seção MQTT agora vai sozinha, fora
  da transação: o nó valida, grava e reinicia por conta própria. Com outras
  mudanças pendentes junto, a página pede para gravá-las antes. Pelo cabo
  nada muda. A página também mostra o motivo quando uma gravação é recusada.
- Mesh e ESP-NOW abriam deitados com o aparelho em pé. Na abertura, uma
  leitura do IMU zerada ou velha (o cache é de 1 s; no modo de economia do
  BMI270 o primeiro dado demora) caía na zona morta, e o padrão era deitado.
  Agora a abertura lê o IMU até ter uma gravidade plausível, decide pelo eixo
  que domina (sem a histerese, que só faz sentido com o app já aberto) e, sem
  decisão clara, abre na posição em que a tela já está. Com o app aberto, o
  limite para girar desceu de 30° para ~24° de inclinação: segurando o
  aparelho recostado, um app aberto deitado não se corrigia.
- CSI recusado pelo C6 com `ESP_FAIL`: o esp_hosted inicia o Wi-Fi do C6 com a
  config que o P4 manda, e o P4 mandava `csi_enable = 0`; o driver confere
  esse campo antes de tudo ("CSI not enabled in menuconfig!"). O P4 agora
  inicia o Wi-Fi com o CSI habilitado — não exige regravar o C6. O C6 liga o
  CSI por um caminho só e passa a aceitar também HE SU (802.11ax). Detalhes e
  prova em [RADIO_C6.md](RADIO_C6.md).
- 802.15.4 deixava o Wi-Fi associado mas sem passar dados, por duas causas.
  Ligado: faltava a coexistência Wi-Fi/802.15.4 (`esp_coex_wifi_i154_enable`),
  como fazem os exemplos oficiais de Thread e Zigbee no C6. Ao desligar: o
  `esp_ieee802154_disable` corta o clock do MAC no meio de uma recepção; o C6
  passa a desligar por `esp_ieee802154_sleep` (o caminho do OpenThread). O
  `radio 154` pinga o roteador antes, durante e depois.
- CSI vinha raro e cortado: o beacon de 2,4 GHz (11b) não gera CSI, então o
  `radio csi` pinga o roteador a 10 Hz como fonte, como o esp-csi; o record
  deixou de ser cortado em 256 B (teto de 1024) e passou a trazer o formato
  do quadro e a flag de primeira palavra inválida do hardware do C6.
- FTM: o "status 255" era o C6 recusando iniciar a sessão. O motivo agora vem
  no relatório: `ESP_ERR_NOT_SUPPORTED`, porque o ESP-IDF desliga o FTM
  initiator nos ESP32-C6 v0.0/v0.1 (errata do silício). O C6 só anuncia a
  capacidade FTM quando a revisão permite, e o `farejo info` mostra a revisão.
- CSI: o limite de taxa anda por cadência, e não descarta mais quadros que
  chegam com atraso variável.
- C6: a saída para o P4 virou um ring buffer de tamanho variável (records de
  7 B a ~1 KB sem cópia na pilha da tarefa de Wi-Fi), e o INFO mostra as
  mensagens descartadas e o estado do 802.15.4.
- Console: `espnow enviar` mandava só a primeira palavra da mensagem.
- Console: listagens longas (`mesh` com centenas de nós vindos pelo MQTT,
  `ble`, `lista`, `cartao`) prendiam o núcleo 0 por mais de 5 s e disparavam
  o watchdog da IDLE0. Agora cedem a CPU a cada 8 itens.
- Mesh, páginas "Nó" e Nós: rastros no topo e na base da lista ao rolar.
  Linha que ainda não tinha aparecido tinha assinatura vazia e "mudava" em
  toda passada; como a marca de redesenho é um intervalo, cada amostra do
  arraste redesenhava todas as linhas visíveis por cima do que a rolagem
  tinha acabado de deslocar. Agora só linha já desenhada é comparada, e cada
  linha da página "Nó" pinta o próprio fundo.
- Console `tela rolar cpu|dma`: compara a rolagem por DMA (da página da
  frente) com a cópia pela CPU, sem regravar.
- Mesh: dois textos com menos de 2 s de intervalo - o firmware descarta o
  segundo (rate limit da porta TEXT no PhoneAPI; na 2.7 sem erro, a mensagem
  virava "sem resposta"). O envio agora espera o que falta.
- Mesh: cabeçalho e corpo do quadro serial iam em duas escritas, e a tarefa
  do Mesh (batimento) e a interface (texto, favorito, admin) escrevem na UART;
  um quadro podia cair no meio do outro e os dois se perdiam. Agora um quadro
  por vez (trava de transmissão).
- Mesh: a configuração do canal 0 (o primário, LongFast) era ignorada e canal
  desabilitado era lido como ativo. `index` e `role` do Channel são proto3 e
  somem do fio quando valem zero; o leitor partia de -1 e PRIMARY. Um LongFast
  apagado não tinha como ser reaberto e canal desligado continuava na coluna.
- Mesh: canais desabilitados no nó saem da coluna (o histórico fica no cartão)
  e a configuração dos canais fica guardada na NVS, valendo também sem o nó.
- Mesh: apresentação (NODEINFO_APP) e posição (POSITION_APP) de nós ao vivo
  passaram a ser lidas. Antes nome, modelo, MAC e posição só vinham no dump de
  configuração: nó que aparecia depois ficava pelo id em hexa.
- Mesh: o nome mostrado segue curto → longo → id; o id só aparece quando não
  há nome legível. A coluna de conversas mostra o nome longo numa 2ª linha.
- Mesh: a coluna de conversas tem ordem explícita (canais, depois diretas da
  mais recente) e o app segue a conversa aberta pela chave. Uma conversa nova
  numa vaga anterior deslocava os índices e a tela mostrava e enviava para a
  conversa vizinha.
- Mesh: texto longo não saía. Acima de 233 bytes o nó descartava sem
  resposta, e entre ~220 e 233 recusava com TOO_LARGE (o pacote inteiro não
  passa de 255 bytes: cabeçalho, bitfield e, na direta, +12 da cifra PKI). O
  teto agora é 200 bytes, o mesmo dos clientes oficiais; a barra de escrita
  para nele e o envio corta na fronteira de caractere.
- Mesh: mensagem que só aparecia na tela quando chegava a seguinte. O gravador
  do cartão troca de buffer antes de escrever, e quem pedia para ler nesse
  intervalo via "nada pendente" e lia sem a linha; `tab5_sd_gravar_pendentes`
  agora espera a gravação em andamento. E o contador de mensagens só sobe
  depois de a linha estar no gravador.
- Mesh: mensagem própria reentregue durante o dump virava "sem resposta" e
  sobrescrevia no cartão o desfecho verdadeiro.
- Mesh: o anel de ids vistos não é mais zerado ao trocar de via - a mesma
  mensagem pelo rádio e pela ponte MQTT entrava duas vezes.
- Mesh, Malha: uso do canal, airtime e uptime mostram "-" sem telemetria do
  nó (era "0"), nós online contados localmente sem LocalStats, enlace com a
  via, e o nosso nó pelo nome.
- Mesh: tabela de nós com o nosso no topo (em branco), depois os ouvidos pelo
  ar com SNR (em amarelo) FIXOS na ordem em que apareceram - não descem quando
  outro nó fala -, depois o resto (MQTT em azul, sem SNR em cinza) do ouvido
  mais recentemente ao mais antigo. A tabela cheia despeja primeiro os não
  fixos. Linhas de 44 px com um fio de pouco contraste entre elas.
- Mesh: nó de rádio que não aparecia na tabela com o MQTT ligado no C6L. A
  tabela tinha 32 vagas e o downlink do MQTT as girava em segundos; o despejo
  olhava só o avistamento local, e os nós do dump (avistamento 0) giravam
  numa vaga só. Agora: 256 vagas na PSRAM, cache de nomes de 1024, despejo
  pela idade completa e primeiro os da internet, e o lugar fixo (ouvido pelo
  ar) dado na chegada do pacote - protege do despejo mesmo com a tela fechada.
- Mesh: botões de canal na mesma fonte da tabela, com o número do canal
  ("ch0 LongFast") e o rótulo "canais".
- Mesh pela internet: mensagem direta saía cifrada com a chave do canal e
  aparecia para todos no LongFast; agora é recusada (falta a chave pública).
- Mesh pela internet: `via_mqtt` e `hop_start` eram gravados nos campos 13 e 14
  do pacote em vez de 14 e 15.
- Teclado virtual: cada dedo tem a sua tecla e a letra sai na soltura
  daquele dedo - digitando rápido, a letra anterior se perdia quando o dedo
  seguinte descia antes de o anterior subir. Sem o intervalo mínimo de 80 ms
  entre letras (comia a segunda letra de "ss").
- Barra de escrita: mostra as duas últimas linhas do rascunho (da terceira em
  diante o texto não aparecia), com cursor no fim; a quebra de linha usa a
  largura real (o limite de 64 bytes cortava a linha no meio e o fim sumia).
- Brilho mínimo mais baixo (0,9% do PWM, era 4%) e barra de brilho na escala
  do olho: a metade de baixo da barra agora tem degraus finos. Novo comando de
  console `brilho <1-255> [hz]` para testar o backlight sem gravar.
- Mensagem enviada ficava no dia errado: o relógio era acertado pelo
  last_heard dos nós (hora do passado). Agora só tráfego ao vivo acerta o
  relógio, e mensagens carimbadas antes de um acerto grande são corrigidas.
- nos.txt crescia com os mesmos nós a cada boot em que o cartão não montava a
  tempo; agora só grava depois de ler, e é compactado quando tem sobra.
- Câmera com "zoom": o modo 1280x720 do sensor é uma janela no centro (80% x
  60% do campo). O sensor passou para 1600x1200; a foto sai com o quadro
  inteiro (1200x1600) e o preview não estica mais a imagem.
- O Bluetooth ligava em todo boot com o Mesh configurado para ele, mesmo
  desligado em Ajustes. Agora só o botão do Bluetooth liga o rádio.
- IMU que falhava no boot ficava fora até reiniciar; o sistema tenta de novo.
- Azimutal: durante o início a frio do GPS o mapa ficava vazio por minutos
  (satélites ouvidos ainda sem elevação/azimute eram descartados). Agora mostra
  quantos estão sendo ouvidos e uma barra de sinal por satélite.
- Azimutal não liga mais o 5V do Grove por conta própria (só energizava um
  módulo que o sistema não detectava): quem liga é Ajustes > Grove 5V, e o app
  avisa quando está desligado.
- Tocar num aparelho Bluetooth não dava retorno nenhum.
- Emojis não apareciam no texto desenhado pelo compositor (o gancho de emoji
  só existia no display físico) e tinham as cores trocadas (ordem de bytes do
  RGB565). Agora saem em tamanho inteiro, sem ampliação fracionária.
- Teclado virtual: fonte suave ajustada à largura de cada tecla (o ENVIAR não
  cabia com a tela em pé); setas no lugar de "^" e "<-".
- PIN do Meshtastic: o número mostrado pelo nó não fica mais salvo como PIN
  fixo; um PIN salvo recusado volta ao padrão e o sistema passa a perguntar.

## [0.7.0] - 2026-09-25

Primeira versão pública.

### Sistema
- Núcleo com laço de interface, compositor de página dupla, perfis de energia
  por app, repouso da tela e partes de fundo.
- Componentes: lista rolável, conversa (balões, teclado virtual), botões,
  fontes suaves Roboto com acentos.
- Gravador do cartão único para todos os apps, log diário e trabalhador para
  leituras lentas fora do toque.
- Ajustes organizado em páginas.

### Rádio
- Wi-Fi e Bluetooth LE pelo ESP32-C6 (esp_hosted; NimBLE no P4).
- ESP-NOW com firmware próprio no C6; o canal escolhido fica salvo.
- Meshtastic pelo cabo Grove ou por Bluetooth, com envio opcional da posição
  do GPS.

### Segurança
- Senha do Wi-Fi cifrada no cartão com uma chave derivada de cada aparelho.

### Aplicativos
- Mesh, ESP-NOW, Trilha, Azimutal, Câmera, Arquivos, Energia, Tarefas, IMU,
  Ajustes, CreazySnake e Exemplo.
