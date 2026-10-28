// Servico de CAMERA - sensor SC2356 (SC202CS) do Tab5, MIPI-CSI, 1600x1200.
//
// PESQUISADO, nao inventado: nao existia NENHUM driver de
// camera neste projeto ate aqui - so o CAM_RST (tab5_power_camera). Os fatos
// abaixo vieram do BSP OFICIAL da Espressif para esta placa exata
// (github.com/espressif/esp-bsp, bsp/m5stack_tab5), nao de suposicao:
//
//   - Sensor: SC2356 / SC202CS, MIPI-CSI. Kconfig: CONFIG_CAMERA_SC202CS.
//   - Pino de reset: NENHUM GPIO direto - e o MESMO CAM_RST que
//     tab5_power_camera() ja controla (expansor 0, bit 6). o
//     sistema de apps ligava isso sozinho via profile.camera=true; desde que
//     a camera virou service (ver o aviso grande logo abaixo), who liga e
//     DESLIGA o pino e o proprio tab5_camera_start()/stop().
//   - SCCB (o I2C do sensor, para configura-lo): G32/G31 - EXATAMENTE os
//     mesmos pins que M5Unified ja usa para o barramento INTERNO
//     (M5.In_I2C: touch, IMU, PMIC, expansores - ver M5Unified.cpp,
//     board_M5Tab5). Ou seja, a camera e so mais um address no MESMO
//     barramento que o resto do sistema ja possui e protege com a regra "so a
//     tarefa de interface fala I2C".
//
// COMO ISTO NAO COLIDE COM O QUE JA EXISTE: em vez de abrir um segundo
// barramento nos mesmos pins (proibido - o ESP-IDF nao deixa dois donos na
// mesma GPIO), tab5_camera_start() pega o handle que o M5Unified ja
// instalou com i2c_master_get_bus_handle() - uma funcao do proprio IDF feita
// exatamente para "quero o barramento que outro componente ja abriu", sem
// inicializar nada de novo. Confirmado: M5GFX usa i2c_new_master_bus() (o
// driver NOVO) por baixo do M5.In_I2C - por isso o handle e do type certo.
//
// O RESTO (V4L2: open/ioctl/mmap em /dev/videoN) e o mesmo padrao que o
// exemplo oficial da Espressif usa para ESTA placa
// (esp-bsp/examples/display_camera_video) - so trocamos LVGL por M5GFX e
// callback assincrono por tarefa_app_t (o molde que este projeto ja usa para
// todo service de fundo, ver kernel/app_task.h).
#pragma once

#include <stddef.h>

#include <stdbool.h>
#include <stdint.h>

// -------------------------------------------------------------------------
// VIROU SERVICO, NAO MAIS "RECURSO DO APP CAMERA"
// ("quais sensors internos podem virar service?" -> a camera era a unica
// peca de hardware caro que ainda so existia enquanto UM app especifico
// estava na tela).
// -------------------------------------------------------------------------
// CONTAGEM DE REFERENCIA, mesmo molde de tab5_sd.hpp: tab5_camera_start()
// pode ser chamada por MAIS DE UM consumidor ao mesmo time (o app Camera E,
// por exemplo, o comando de console 'foto', ou um futuro segundo app) - a
// primeira chamada liga o sensor de verdade, as seguintes so contam mais um
// interessado, e o sensor so desliga quando o ULTIMO chamar
// tab5_camera_stop(). Abrir o app Camera enquanto o comando 'foto' ainda
// segura a camera (ou vice-versa) e seguro: nenhum dos dois derruba o outro.
//
// UMA UNICA RESOLUCAO POR VEZ, DE PROPOSITO - so existe UM par de buffers de
// output (ver TAB5_CAM_NBUF em tab5_camera.cpp). Enquanto ja houver alguem
// usando a camera, um segundo tab5_camera_start() com width/height
// DIFERENTES e recusado (devolve false, log explica) em vez de reconfigurar
// o sensor por baixo de who ja esta lendo. Hoje so existe UM consumidor por
// resolucao (o app Camera usa a area da tela; 'foto' usa um size fixo
// proprio) - o dia em que isso incomodar, a solucao e um segundo par de
// buffers, nao esta funcao.
//
// A CAMERA SE ALIMENTA SOZINHA AGORA - antes, CAM_RST (o pino de reset do
// sensor) so era on porque o app Camera declarava profile.camera=true, e
// o KERNEL aplicava isso antes de ao_abrir() rodar (ver tab5_power.hpp). Um
// service de verdade nao pode depender de UM app especifico estar aberto
// para ter energia - por isso tab5_camera_start() agora chama
// tab5_power_camera(true) ELA MESMA, no mesmo espirito que tab5_grove.cpp ja
// liga o trilho de 5V sozinho para GPS/Mesh (ver o aviso la). O app Camera
// NAO declara mais profile.camera - ver o aviso em app_camera.cpp.
//
// O UNICO PONTO DELICADO: core.cpp ainda reaplica o profile de energia
// (imu/mic/camera/etc) toda vez que um app abre ou fecha, como uniao do que
// TODOS os apps abertos hoje declaram - e SEM SABER que este service existe,
// aquela reaplicacao desligaria CAM_RST no storage de uma sessao pedida por
// fora de um app (o comando 'foto', por exemplo) so porque o app QUE ABRIU
// NAQUELE INSTANTE nao pede camera. tab5_camera_active() (abaixo) existe
// exatamente para tapar esse buraco: sistema_impor_preferencias() faz OR com
// ela, do mesmo jeito que ja faz com ajustes_grove() para o Grove.
//
// SO DA TAREFA DE INTERFACE, DIRETO - achado ao vivo com o
// comando 'foto': estas duas funcoes tocam SCCB (o I2C interno, o MESMO
// barramento de touch/IMU/energia/RTC), e a regra deste projeto e "so a
// tarefa de interface fala I2C" (sem trava nenhuma no driver). app_camera.cpp
// pode chamar direto porque ao_abrir()/ao_fechar() JA rodam nessa tarefa.
// QUALQUER outro chamador (um comando de console, uma futura tarefa de
// fundo) DEVE passar por sistema_camera_iniciar()/sistema_camera_parar()
// (kernel/app.h) em vez de chamar estas aqui - a ponte manda o pedido para a
// tarefa certa. Chamar estas direto de outra tarefa e o que produziu "Set
// format regs fail" na primeira tentativa do 'foto'.

// Liga o sensor (ou so soma mais um interessado, se ja estiver on - ver
// o aviso grande acima sobre contagem de referencia): barramento I2C
// shared, esp_video_init, abre o dispositivo, aloca os buffers de
// captura, registra o PPA (escala + rotaciona 270 graus - o sensor esta
// mounted de lado na placa) e sobe a tarefa que produz frames.
// 'width'/'height' e o size do buffer de SAIDA (RGB565, ja ready para
// pushImage) - a foto cheia do sensor (1600x1200) e reduzida para isto a
// cada frame.
//
// CHAMAR SO DA TAREFA DE INTERFACE - ver o aviso grande acima. Nao depende
// mais de profile.camera, ela mesma liga CAM_RST. Devolve false se qualquer
// step falhar (log explica qual, inclusive um pedido de resolucao
// diferente da que ja esta em uso) - nesse caso nao ha nada para turn_off.
bool tab5_camera_start(int width, int height);

// Descarta UM interesse (ver a contagem de referencia acima). So quando o
// ULTIMO cai e que a tarefa desce de verdade (tarefa_app_encerrar, com
// timeout - ver o aviso la sobre tarefa abandonada de proposito em vez de
// morta a força), o stream para, os buffers somem, o cliente do PPA sai e
// CAM_RST volta a ficar em reset. CHAMAR SO DA TAREFA DE INTERFACE (ver o
// aviso grande acima) - de onde tab5_camera_start() foi chamada
// (ao_fechar()). Seguro chamar mesmo se tab5_camera_start() nunca rodou,
// failed, ou ja foi parada por outro caminho.
void tab5_camera_stop(void);

// Ha algum interessado vivo agora (contagem de referencia > 0)? Existe para
// sistema_impor_preferencias() (kernel/core.cpp) nao cortar CAM_RST por
// baixo de um consumidor que nao e o app em foco - ver o aviso grande no
// topo deste file. Apps normais nao precisam chamar isto.
bool tab5_camera_active(void);

// ONDE O PREVIEW E ESCRITO. A tarefa da camera escreve cada
// frame, ja girado e escalado para width x height (os de
// tab5_camera_start), DIRETO num retangulo de uma imagem RGB565 - na pratica
// a tela virtual do sistema (sistema_superficie, kernel/app.h). Sem copia
// intermediaria. 'buf' precisa de alinhamento de cache (a tela virtual tem).
// 'avisar' (opcional) e chamado da tarefa da camera a cada frame escrito -
// sistema_acordar_interface, para a interface levar o frame na hora.
typedef struct {
  void*  buf;          // nulo = nenhum target (so fotos)
  size_t bytes;        // size total de buf
  int    pic_l, pic_a; // dimensoes da imagem inteira
  int    x, y;         // canto do retangulo dentro dela
  void (*avisar)(void);
} tab5_cam_target_t;

void tab5_camera_set_target(const tab5_cam_target_t* d);   // nullptr = nenhum

// true UMA vez por frame escrito no target desde a latest chamada.
bool tab5_camera_frame_written(void);

// FOTO EM RESOLUCAO CHEIA: 1200x1600, o quadro INTEIRO do sensor girado,
// sem escala - independente do size do preview.
//
// O SENSOR RODA NO MODO 1600x1200 (sdkconfig.defaults). O mode 1280x720 do
// driver nao reduz a imagem: ele liga uma JANELA no storage do sensor (colunas
// 160-1447, linhas 240-967), 80% da width e 60% da height - a foto saia
// como se tivesse zoom.
//
// A CAMERA SO CAPTURA; QUEM GRAVA E QUEM PEDIU. A version anterior gravava o
// .bmp na propria tarefa da camera, linha a linha: o preview parava e o app
// ficava travado (INICIO bloqueado) por mais de 10 s. Agora o pedido e
// atendido no proximo frame com UMA operacao de PPA (poucos ms) para o
// buffer de who pediu; write e com o worker do sistema (app) ou com a
// tarefa do console - ver services/tab5_bmp.
#define TAB5_CAM_PHOTO_W 1200
#define TAB5_CAM_PHOTO_H 1600

// Buffer certo para a foto (PSRAM, alinhado a linha de cache, o size que o
// PPA exige). Liberar com heap_caps_free.
uint16_t* tab5_camera_alloc_photo(void);
size_t    tab5_camera_photo_bytes(void);

// Pede a proxima foto para 'target' (de tab5_camera_alloc_photo). false = a
// camera nao esta ligada ou ja ha um pedido pending. 'target' e escrito pela
// tarefa da camera ate tab5_camera_photo_ready() return_card true - nao libere
// antes disso (tab5_camera_stop() tambem espera a tarefa terminar).
bool tab5_camera_request_photo(uint16_t* target);

// true UMA vez quando a foto pedida chegou ao buffer (ok = PPA deu certo).
bool tab5_camera_photo_ready(bool* ok);

// REGISTRADOR DO SENSOR, para diagnostico (comando 'camera' do console).
// value >= 0 escreve antes de read; -1 so le. false = camera desligada ou o
// SCCB failed. O que for escrito vale ate a camera turn_off: ao turn_on de novo
// o driver reaplica a tabela de inicializacao.
bool tab5_camera_reg(uint16_t reg, int value, uint8_t* value_read);
