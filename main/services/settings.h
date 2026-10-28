// Ajustes do sistema - as PREFERENCIAS DO USUARIO, guardadas na NVS.
//
// A distincao que motivou este modulo: o tab5_profile_t de cada app diz "que
// hardware EU preciso ligado" - Grove, radio, IMU. Isso e requisito do
// aplicativo. Ja o brilho e outra coisa: e preferencia de quem esta segurando
// o aparelho, e nenhum app tem autoridade sobre ela. Estavam misturados no
// mesmo struct, e o app vencia - errado.
//
// Agora o sistema sobrescreve o brilho do perfil na hora de abrir o app. O
// campo continua existindo no tab5_profile_t (que precisa seguir autocontido e
// copiavel para outros projetos), mas quem manda aqui e este modulo.
//
// NVS e nao cartao: ajuste tem de funcionar sem cartao nenhum, e ser lido no
// boot antes de qualquer montagem.

#pragma once

#include <stdint.h>
#include <stddef.h>

// Faixa do brilho, no valor do M5GFX (0-255 vira o duty de um PWM de 9 bits
// a 44,1 kHz no GPIO22). O minimo NAO e zero de proposito: com a tela apagada
// voce nao conseguiria enxergar o controle para desfazer. O olho percebe o
// brilho em escala logaritmica (ver a barra de Ajustes, que segue essa curva),
// entao o piso tem de ser baixo - mas nao tanto: com 2 (0,9% de duty) a tela
// ficava acesa ao arrastar a barra e VOLTAVA APAGADA depois do repouso, porque
// o driver do backlight nao parte do zero com um pulso tao curto (ver
// tab5_power_wake_screen). 6 = 1,8% de duty: ainda escuro, mas sempre visivel.
#define AJUSTES_BRILHO_MIN  6
#define AJUSTES_BRILHO_MAX  255

// Carrega da NVS (e inicializa a NVS, se preciso). Chamar antes das tarefas.
void ajustes_iniciar(void);

uint8_t ajustes_brilho(void);

// Aplica na tela AGORA e grava. Valor fora da faixa e limitado, nao recusado.
void ajustes_definir_brilho(uint8_t brilho);

// Se o usuario quer o WiFi ligado. Quem age sobre isso e o servico de WiFi;
// aqui so mora a vontade.
bool ajustes_wifi(void);
void ajustes_definir_wifi(bool ligado);

// Se o usuario quer o teclado do ExtPort1 ativo. Desligado, o sistema nao
// procura o teclado nem gasta transacao I2C com ele.
bool ajustes_teclado(void);
void ajustes_definir_teclado(bool ligado);

// Manter o 5V do Grove SEMPRE ligado.
//
// Sem isto, um modulo que precisa bootar (o Unit C6L roda o Meshtastic inteiro
// num ESP32-C6) reinicia toda vez que o trilho e cortado, e nunca chega a
// conversar. Com isto ligado ele fica de pe como qualquer periferico.
//
// Custo: o trilho consome mesmo sem ninguem plugado, por isso nasce desligado.
bool ajustes_grove(void);
void ajustes_definir_grove(bool ligado);

// Tempo sem toque ate a tela apagar, em SEGUNDOS.
//
// Guardado em segundos e nao em milissegundos porque cabe num uint8_t da NVS e
// porque e assim que a pessoa pensa no ajuste. O sistema converte.
//
// O minimo de 15 s nao e capricho: abaixo disso a tela apaga no meio de uma
// leitura, e o toque para acordar vira parte do uso normal. O maximo de 60 s
// vem do outro lado - a tela e o maior consumidor do aparelho, e cada segundo
// aceso custa mais que qualquer economia que a gente arrancou do resto.
#define AJUSTES_TELA_MIN_S  15
#define AJUSTES_TELA_MAX_S  60

uint8_t ajustes_tela_s(void);
void    ajustes_definir_tela_s(uint8_t segundos);

// RELOGIO DO PAINEL (DPI/pixel clock), em MHz.
//
// Nasceu: ate aqui so dava pra mudar por 'tela relogio <mhz>'
// no console, e o valor morria no proximo boot (o sistema sempre reaplicava
// 60 fixo). Baixar o relogio corta a leitura continua da PSRAM que o painel
// exige (ver tab5_power.cpp) - a mesma familia de economia do brilho, so que
// mexendo no OUTRO lado da conta (quanto o painel PEDE, nao quanto o sistema
// gasta atendendo). Guardado e nao so em RAM para sobreviver ao reinicio, e
// exposto em Ajustes para quem nao estiver com o cabo serial na mao.
//
// A faixa e a MESMA que tab5_power_dpi_clock() aceita (24..80) - limitada
// aqui tambem, e nao so la, pelo mesmo motivo do tela_s: um valor gravado por
// engano nao pode virar um painel que nunca mais sobe.
// Teto de 80 (era 100). Acima de 80 o
// painel so gasta banda de PSRAM sem ganho visivel, e o passo de 5 de Ajustes
// (24, 29 ... 79) nunca parava em 80: pulava para 84. Com o teto em 80, a
// limitacao faz o resto - 79+5 vira 80, 25-5 vira 24.
#define AJUSTES_RELOGIO_MIN  24
#define AJUSTES_RELOGIO_MAX  80

uint8_t ajustes_relogio_painel(void);

// SO GRAVA - nao aplica no painel. Quem muda o relogio de verdade e
// tab5_power_dpi_clock(); o chamador (console, Ajustes) e quem decide a
// ORDEM entre aplicar e gravar, porque so ele sabe se a troca deu certo.
void    ajustes_definir_relogio_painel(uint8_t mhz);

// VOLUME do alto-falante interno (ES8388). Faixa NATIVA de
// M5.Speaker.setVolume() (0..255) - ao contrario do brilho, 0 e um estado
// legitimo (mudo), entao nao ha piso: a diferenca que justifica o piso do
// brilho (uma tela apagada esconde o proprio controle para desfazer) nao
// existe aqui.
//
// APLICADO NA HORA, mesmo padrao de ajustes_definir_brilho() - mas SEM
// custo de hardware nenhum enquanto ninguem toca som: setVolume() so grava
// um numero no objeto Speaker, o ES8388 continua desligado ate o primeiro
// M5.Speaker.tone()/playWav() de algum app (M5Unified inicializa o codec de
// forma preguicosa - ver o aviso grande em app_config.cpp, junto da secao
// VOLUME). Por isso pode ser aplicado aqui, no boot, sem esperar nenhum app
// abrir.
#define AJUSTES_VOLUME_MIN  0
#define AJUSTES_VOLUME_MAX  255

uint8_t ajustes_volume(void);
void    ajustes_definir_volume(uint8_t volume);

// CASA - posicao de referencia para calcular distancia/direcao ate uma
// localizacao recebida (Mesh, app_mesh.cpp). E do usuario e nao do Mesh
// especificamente, pelo mesmo motivo do brilho: nao e requisito de nenhum
// app, e preferencia de quem segura o aparelho - guardada aqui para poder
// servir qualquer app que precise no futuro, nao so o Mesh.
//
// Devolve false enquanto nao configurada (lat=lon=0 e o valor de fabrica, e
// tambem o unico ponto real que ninguem mora - fica seguro usar como "vazio"
// sem precisar de uma flag a parte na NVS).
bool ajustes_casa(float* lat, float* lon);
void ajustes_definir_casa(float lat, float lon);

// AVISO SONORO DA TRILHA: silenciar so a Trilha, com o Mesh ainda tocando.
// Vive aqui (preferencia do usuario) e nao
// em notification.h (que so guarda o estado em RAM, ver notif_silenciar()) -
// mesma distincao do resto deste arquivo: o QUE soa e decisao de quem
// segura o aparelho, nao do servico. Default TOCAR (true): o beep de
// notificacao já existia e funcionava; isto e um jeito de desligar, nao de
// ligar. Fica dentro do proprio app Trilha por enquanto (nao em Ajustes) -
// se um dia mais fontes pedirem o mesmo controle, cada uma ganha o par
// analogo (ajustes_beep_mesh() etc.), em vez de um mecanismo generico sem
// um segundo caso real para validar o formato (ver ARCHITECTURE.md, 1.1).
bool ajustes_beep_trilha(void);
void ajustes_definir_beep_trilha(bool tocar);

// AVISO SONORO DO MESH - o par analogo previsto no aviso acima, feito no
// mesmo formato. Mesma logica, mesmo
// default (TOCAR).
bool ajustes_beep_mesh(void);
void ajustes_definir_beep_mesh(bool tocar);

// CANAL DO ESP-NOW. 0 = o do roteador; 1..13 = canal fixo (o WiFi
// solta o roteador). Era so RAM: todo reinicio voltava para o roteador e o
// Cardputer no canal 1 sumia ate alguem reabrir o app e escolher de novo.
// Quem aplica e o servico ESP-NOW (tab5_espnow) - aqui so mora a escolha.
uint8_t ajustes_espnow_canal(void);
void    ajustes_definir_espnow_canal(uint8_t canal);

// POR ONDE O TAB5 FALA COM A MALHA MESHTASTIC. Serial = o cabo Grove
// (PROTO/UART); Bluetooth = BLE pelo C6 interno, sem fio; Internet = MQTT
// pelo Wi-Fi, sem no nenhum. O PIN e o do pareamento BLE do Meshtastic
// (padrao 123456 nos aparelhos sem tela).
//
// O GROVE TEM PRIORIDADE: quando a sondagem ACHA um C6L no cabo, a via vira
// Serial sozinha (kernel/core.cpp). Depois disso a escolha manual vale ate o
// proximo achado - e a internet tambem pode ser escolhida assim, alem de
// cobrir a falta do no (ajustes_mesh_internet).
#define AJUSTES_MESH_SERIAL     0
#define AJUSTES_MESH_BLUETOOTH  1
#define AJUSTES_MESH_INTERNET   2
uint8_t  ajustes_mesh_via(void);

// MESH PELA INTERNET QUANDO O NO NAO ESTA CONECTADO: com o Wi-Fi conectado e
// sem o C6L (cabo ou Bluetooth), as mensagens chegam pelo servidor MQTT.
// Ligado de fabrica.
bool     ajustes_mesh_internet(void);
void     ajustes_definir_mesh_internet(bool ligado);
// A REDE PUBLICA PELA INTERNET (mqtt.meshtastic.org, LongFast) em paralelo, com o
// Wi-Fi conectado - nos e conversa separados. Desligada de fabrica: e trafego
// de internet que ninguem pediu. Liga/desliga na pagina de nos do app Mesh.
bool     ajustes_mesh_rede_internet(void);
void     ajustes_definir_mesh_rede_internet(bool ligado);
void     ajustes_definir_mesh_via(uint8_t via);
uint32_t ajustes_mesh_pin(void);
void     ajustes_definir_mesh_pin(uint32_t pin);
// O no escolhido mostra o PIN na tela (um novo a cada pareamento): o PIN salvo
// nao serve, e o sistema pergunta sempre. Aprendido na primeira recusa do PIN
// salvo; volta a false quando outro no e escolhido.
bool     ajustes_mesh_pin_na_tela(void);
void     ajustes_definir_mesh_pin_na_tela(bool sim);
// Endereco BLE do no escolhido ("" = o primeiro Meshtastic que aparecer).
void     ajustes_mesh_ble_endereco(char out[18]);
void     ajustes_definir_mesh_ble_endereco(const char* end);

// GPS DO TAB5 PARA A MALHA: de quantos em quantos MINUTOS o Tab5 passa a
// posicao do GPS (Grove) adiante. 0 = nunca (padrao: posicao e dado pessoal -
// so sai se o usuario pedir). Com o no do outro lado ela vai SO PARA O NO, que
// a difunde no ritmo e com a precisao dele (ver enviar_posicao em
// tab5_mesh.cpp); pela internet, sem no, o Tab5 difunde ele mesmo. Editado no
// app Mesh, Configurar o no > Localizacao.
uint8_t ajustes_mesh_posicao_min(void);
void    ajustes_definir_mesh_posicao_min(uint8_t minutos);

// BLUETOOTH LIGADO. Via Bluetooth no Mesh liga junto (ver o app Ajustes).
bool ajustes_bluetooth(void);
void ajustes_definir_bluetooth(bool ligado);

// Tela de bloqueio: ao acordar do repouso o aparelho mostra hora e data e pede
// um deslize para cima. Desligada de fabrica.
bool ajustes_bloqueio(void);
void ajustes_definir_bloqueio(bool ligado);

// PENDRIVE AO CONECTAR: ligado num computador pelo USB-C, o Tab5 abre o app
// Pendrive (o cartao SD vira um disco no computador). Desligado de fabrica: o
// pendrive tira o console do cabo, e quem grava o firmware pelo USB-C nao
// quer isso acontecendo sozinho. Com um app aberto nao interrompe - so avisa
// (central de avisos). O app Pendrive abre na mao com o ajuste desligado.
bool ajustes_pendrive(void);
void ajustes_definir_pendrive(bool ligado);

// ESCALA DO GIROSCOPIO, por eixo do BMI270 (0 = x, 1 = y, 2 = z). O chip sai
// de fabrica com erro de sensibilidade de alguns por cento, e o rumo do Radar
// herda esse erro a cada volta. Medida no proprio Radar (voltas inteiras contra
// uma referencia); 1,0 = sem correcao. Faixa aceita 0,90..1,10.
float ajustes_giro_escala(int eixo);
void  ajustes_definir_giro_escala(int eixo, float escala);

// ORDEM DOS ICONES NA TELA INICIAL, como os NOMES dos apps separados por
// virgula ("Mesh,Trilha,Radar"). Nome e nao indice: um app novo no firmware
// ou um app removido nao embaralha o resto - quem nao esta na lista entra no
// fim, na ordem de apps.cpp, e nome que nao existe mais e ignorado. Vazio = a
// ordem de apps.cpp. Quem interpreta e o shell (sistema_home_*, app.h).
void ajustes_ordem_apps(char* out, size_t n);
void ajustes_definir_ordem_apps(const char* nomes);   // "" ou nullptr = padrao

// APLICATIVOS ESCONDIDOS DA TELA INICIAL, como os NOMES separados por
// virgula. Nome que nao existe mais e ignorado. Vazio = todos visiveis.
// Quem interpreta e o shell, junto da ordem dos icones.
void ajustes_apps_ocultos(char* out, size_t n);
void ajustes_definir_apps_ocultos(const char* nomes);   // "" ou nullptr = nenhum

// PAPEL DE PAREDE DO INICIO (home): linhas abstratas finas sobre preto, os
// mesmos desenhos do fundo da tela de bloqueio. SORTEIO (o de fabrica) tira
// um desenho a cada boot e o mantem ate o proximo; 1..5 fixa um; NENHUM e o
// preto de antes.
#define AJUSTES_PAREDE_SORTEIO  0
#define AJUSTES_PAREDE_ESTILOS  5   // 1..5 = fios, ondas, teia, raios, circulos
#define AJUSTES_PAREDE_NENHUM   6

uint8_t ajustes_parede(void);
void    ajustes_definir_parede(uint8_t opcao);
