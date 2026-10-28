// Servico of CAMERA - sensor SC2356 (SC202CS) of Tab5, MIPI-CSI, 1600x1200.
//
// PESQUISADO, nao inventado: nao existia NENHUM driver of
// camera neste projeto ate aqui - so o CAM_RST (tab5_power_camera). Os fatos
// abaixo vieram of BSP OFICIAL of Espressif para esta placa exata
// (github.com/espressif/esp-bsp, bsp/m5stack_tab5), nao of suposicao:
//
//   - Sensor: SC2356 / SC202CS, MIPI-CSI. Kconfig: CONFIG_CAMERA_SC202CS.
//   - Pino of reset: NENHUM GPIO direto - e o MESMO CAM_RST que
//     tab5_power_camera() ja controla (expansor 0, bit 6). o
//     sistema of apps ligava isso sozinho via profile.camera=true; since que
//     a camera virou service (ver o aviso grande logo abaixo), who liga e
//     DESLIGA o pino e o proprio tab5_camera_start()/stop().
//   - SCCB (o I2C of sensor, para configura-lo): G32/G31 - EXATAMENTE os
//     mesmos pins que M5Unified ja usa para o barramento INTERNO
//     (M5.In_I2C: touch, IMU, PMIC, expansores - ver M5Unified.cpp,
//     board_M5Tab5). Ou seja, a camera e so mais one address no MESMO
//     barramento que o resto of sistema ja possui e protege com a regra "so a
//     tarefa of interface fala I2C".
//
// COMO ISTO NAO COLIDE COM O QUE JA EXISTE: em vez of open one segundo
// barramento nos mesmos pins (proibido - o ESP-IDF nao deixa dois donos in
// mesma GPIO), tab5_camera_start() pega o handle que o M5Unified ja
// instalou com i2c_master_get_bus_handle() - uma funcao of proprio IDF feita
// exatamente para "quero o barramento que outro componente ja abriu", sem
// inicializar nada of novo. Confirmado: M5GFX usa i2c_new_master_bus() (o
// driver NOVO) por bottom of M5.In_I2C - por isso o handle e of type certo.
//
// O RESTO (V4L2: open/ioctl/mmap em /dev/videoN) e o mesmo default que o
// exemplo oficial of Espressif usa para ESTA placa
// (esp-bsp/examples/display_camera_video) - so trocamos LVGL por M5GFX e
// callback assincrono por tarefa_app_t (o molde que este projeto ja usa para
// todo service of fundo, ver kernel/app_task.h).
#pragma once

#include <stddef.h>

#include <stdbool.h>
#include <stdint.h>

// -------------------------------------------------------------------------
// VIROU SERVICO, NAO MAIS "RECURSO DO APP CAMERA"
// ("quais sensors internos podem virar service?" -> a camera era a unica
// peca of hardware caro que ainda so existia enquanto UM app especifico
// estava in screen).
// -------------------------------------------------------------------------
// CONTAGEM DE REFERENCIA, mesmo molde of tab5_sd.hpp: tab5_camera_start()
// pode ser chamada por MAIS DE UM consumidor on mesmo time (o app Camera E,
// por exemplo, o comando of console 'foto', ou one futuro segundo app) - a
// primeira chamada liga o sensor of verdade, as seguintes so contam mais one
// interessado, e o sensor so desliga when o ULTIMO chamar
// tab5_camera_stop(). Abrir o app Camera enquanto o comando 'foto' ainda
// segura a camera (ou vice-versa) e seguro: nenhum dos dois derruba o outro.
//
// UMA UNICA RESOLUCAO POR VEZ, DE PROPOSITO - so existe UM par of buffers of
// output (ver TAB5_CAM_NBUF em tab5_camera.cpp). Enquanto ja houver alguem
// usando a camera, one segundo tab5_camera_start() com width/height
// DIFERENTES e recusado (devolve false, log explica) em vez of reconfigurar
// o sensor por bottom of who ja esta lendo. Hoje so existe UM consumidor por
// resolucao (o app Camera usa a area of screen; 'foto' usa one size fixo
// proprio) - o dia em que isso incomodar, a solucao e one segundo par of
// buffers, nao esta funcao.
//
// A CAMERA SE ALIMENTA SOZINHA AGORA - antes, CAM_RST (o pino of reset of
// sensor) so era on porque o app Camera declarava profile.camera=true, e
// o KERNEL aplicava isso antes of ao_abrir() rodar (ver tab5_power.hpp). Um
// service of verdade nao pode depender of UM app especifico estar open
// para ter energia - por isso tab5_camera_start() now chama
// tab5_power_camera(true) ELA MESMA, no mesmo espirito que tab5_grove.cpp ja
// liga o trilho of 5V sozinho para GPS/Mesh (ver o aviso la). O app Camera
// NAO declara mais profile.camera - ver o aviso em app_camera.cpp.
//
// O UNICO PONTO DELICADO: core.cpp ainda reaplica o profile of energia
// (imu/mic/camera/etc) toda vez que one app abre ou fecha, como uniao of que
// TODOS os apps abertos hoje declaram - e SEM SABER que este service existe,
// aquela reaplicacao desligaria CAM_RST no storage of uma sessao requested por
// fora of one app (o comando 'foto', por exemplo) so porque o app QUE ABRIU
// NAQUELE INSTANTE nao pede camera. tab5_camera_active() (abaixo) existe
// exatamente para tapar esse buraco: sistema_impor_preferencias() faz OR com
// ela, of mesmo jeito que ja faz com settings_grove() para o Grove.
//
// SO DA TAREFA DE INTERFACE, DIRETO - achado on vivo com o
// comando 'foto': estas duas funcoes tocam SCCB (o I2C interno, o MESMO
// barramento of touch/IMU/energia/RTC), e a regra deste projeto e "so a
// tarefa of interface fala I2C" (sem trava none no driver). app_camera.cpp
// pode chamar direto porque ao_abrir()/ao_fechar() JA rodam nessa tarefa.
// QUALQUER outro chamador (one comando of console, uma futura tarefa of
// fundo) DEVE passar por sistema_camera_iniciar()/sistema_camera_parar()
// (kernel/app.h) em vez of chamar estas aqui - a ponte manda o request para a
// tarefa certa. Chamar estas direto of outra tarefa e o que produziu "Set
// format regs fail" in primeira tentativa of 'foto'.

// Liga o sensor (ou so soma mais one interessado, se ja estiver on - ver
// o aviso grande acima sobre contagem of referencia): barramento I2C
// shared, esp_video_init, abre o dispositivo, aloca os buffers of
// captura, registra o PPA (escala + rotaciona 270 graus - o sensor esta
// mounted of lado in placa) e sobe a tarefa que produz frames.
// 'width'/'height' e o size of buffer of SAIDA (RGB565, ja ready para
// pushImage) - a foto cheia of sensor (1600x1200) e reduzida para isto a
// cada frame.
//
// CHAMAR SO DA TAREFA DE INTERFACE - ver o aviso grande acima. Nao depende
// mais of profile.camera, ela mesma liga CAM_RST. Devolve false se qualquer
// step falhar (log explica qual, inclusive one request of resolucao
// diferente of que ja esta em uso) - nesse caso nao ha nada para turn_off.
bool tab5_camera_start(int width, int height);

// Descarta UM interesse (ver a contagem of referencia acima). So when o
// ULTIMO cai e que a tarefa desce of verdade (tarefa_app_encerrar, com
// timeout - ver o aviso la sobre tarefa abandoned of proposito em vez of
// morta a força), o stream para, os buffers somem, o cliente of PPA exit e
// CAM_RST iteration a ficar em reset. CHAMAR SO DA TAREFA DE INTERFACE (ver o
// aviso grande acima) - of onde tab5_camera_start() foi chamada
// (ao_fechar()). Seguro chamar mesmo se tab5_camera_start() nunca rodou,
// failed, ou ja foi parada por outro caminho.
void tab5_camera_stop(void);

// Ha algum interessado vivo now (contagem of referencia > 0)? Existe para
// sistema_impor_preferencias() (kernel/core.cpp) nao cortar CAM_RST por
// bottom of one consumidor que nao e o app em foco - ver o aviso grande no
// topo deste file. Apps normais nao precisam chamar isto.
bool tab5_camera_active(void);

// ONDE O PREVIEW E ESCRITO. A tarefa of camera escreve cada
// frame, ja girado e escalado para width x height (os of
// tab5_camera_start), DIRETO num rectangle of uma imagem RGB565 - in pratica
// a screen virtual of sistema (sistema_superficie, kernel/app.h). Sem copia
// intermediaria. 'buf' precisa of alinhamento of cache (a screen virtual tem).
// 'notify' (opcional) e chamado of tarefa of camera a cada frame escrito -
// sistema_acordar_interface, para a interface levar o frame in hora.
typedef struct {
  void*  buf;          // nulo = nenhum target (so fotos)
  size_t bytes;        // size total of buf
  int    pic_l, pic_a; // dimensoes of imagem inteira
  int    x, y;         // canto of rectangle dentro dela
  void (*notify)(void);
} tab5_cam_target_t;

void tab5_camera_set_target(const tab5_cam_target_t* d);   // nullptr = nenhum

// true UMA vez por frame escrito no target since a latest chamada.
bool tab5_camera_frame_written(void);

// FOTO EM RESOLUCAO CHEIA: 1200x1600, o frame INTEIRO of sensor girado,
// sem escala - independente of size of preview.
//
// O SENSOR RODA NO MODO 1600x1200 (sdkconfig.defaults). O mode 1280x720 of
// driver nao reduz a imagem: ele liga uma JANELA no storage of sensor (colunas
// 160-1447, lines 240-967), 80% of width e 60% of height - a foto saia
// como se tivesse zoom.
//
// A CAMERA SO CAPTURA; QUEM GRAVA E QUEM PEDIU. A version anterior gravava o
// .bmp in propria tarefa of camera, line a line: o preview parava e o app
// ficava travado (INICIO locked) por mais of 10 s. Agora o request e
// atendido no next frame com UMA operacao of PPA (poucos ms) para o
// buffer of who pediu; write e com o worker of sistema (app) ou com a
// tarefa of console - ver services/tab5_bmp.
#define TAB5_CAM_PHOTO_W 1200
#define TAB5_CAM_PHOTO_H 1600

// Buffer certo para a foto (PSRAM, alinhado a line of cache, o size que o
// PPA requires). Liberar com heap_caps_free.
uint16_t* tab5_camera_alloc_photo(void);
size_t    tab5_camera_photo_bytes(void);

// Pede a next foto para 'target' (of tab5_camera_alloc_photo). false = a
// camera nao esta enabled ou ja ha one request pending. 'target' e escrito pela
// tarefa of camera ate tab5_camera_photo_ready() return_card true - nao libere
// antes disso (tab5_camera_stop() tambem espera a tarefa terminar).
bool tab5_camera_request_photo(uint16_t* target);

// true UMA vez when a foto requested chegou on buffer (ok = PPA deu certo).
bool tab5_camera_photo_ready(bool* ok);

// REGISTRADOR DO SENSOR, para diagnostics (comando 'camera' of console).
// value >= 0 escreve antes of read; -1 so le. false = camera desligada ou o
// SCCB failed. O que for escrito vale ate a camera turn_off: on turn_on of novo
// o driver reaplica a tabela of inicializacao.
bool tab5_camera_reg(uint16_t reg, int value, uint8_t* value_read);
