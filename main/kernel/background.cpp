#include "background.h"
#include "app.h"
#include "tab5_owners.hpp"
#include "task_pool.h"
#include "service_stop.hpp"      // pedir/confirmar a saida - o mesmo de GPS, Mesh e Mesh BLE

#include <string.h>
#include <stdio.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "fundo";

#define FUNDO_PRIORIDADE  1      // abaixo da interface (4), radios (3) e worker (2)
#define FUNDO_PARAR_MS    3000
#define FUNDO_CEDER_US    20000  // fundo_ceder() so cede de fato a cada 20 ms

// A PARADA E A DO KERNEL (kernel/service_stop.hpp), e nao uma maquina de estados
// propria. Antes havia uma (LIVRE, RODANDO, SAIU, ABANDONADA, LIXO) e quem
// parava ESPERAVA PERGUNTANDO o estado a cada 10 ms, ate 3 s, na tarefa da
// interface - o mesmo padrao que o service_stop.hpp aposentou nos servicos. Agora:
//   - a tarefa DA o semaforo 'saiu' como ultima coisa (parada_confirmar) e
//     NAO TOCA MAIS NA VAGA depois disso;
//   - quem para ESPERA NELE: acorda no instante da saida;
//   - passou do prazo, a vaga fica 'abandonada' com o pedido de pe, e
//     fundo_manter() limpa quando o semaforo finalmente for dado;
//   - o loop que termina sozinho e visto do mesmo jeito (semaforo dado sem
//     ninguem ter pedido).
// O semaforo 'saiu' e o 'acordar' nascem uma vez por vaga e nunca sao
// apagados: o trampolim pode dar o 'saiu' depois de a interface ter desistido.
struct fundo_s {
  const fundo_def_t*  def;
  volatile bool       em_uso;         // do iniciar ate a limpeza (finalizar)
  volatile bool       parar_pedido;   // alguem de fora da interface pediu (console)
  bool                parando;        // a interface esta dentro de sistema_fundo_parar()
  bool                abandonada;     // nao saiu no prazo: limpa quando sair
  parada_t            parada;         // pedido de parada e confirmacao de saida
  SemaphoreHandle_t   acordar;        // tira o loop de fundo_esperar(); criado 1x, nunca apagado
  int                 notif;
  int64_t             desde_us;
  int64_t             cedeu_us;
  char                resumo[NOTIF_RESUMO];
};

static fundo_s          s_f[FUNDO_MAX];
static portMUX_TYPE     s_mux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<bool> s_perfil_sujo{false};

static void trampolim(void* arg)
{
  fundo_s* f = (fundo_s*)arg;
  f->def->corpo(f);
  // A ULTIMA vez que esta tarefa toca a vaga: daqui em diante a interface
  // pode limpa-la (o semaforo nao e apagado, so a vaga e zerada).
  parada_confirmar(&f->parada);
  sistema_acordar_interface();   // fundo_manter() limpa quem saiu sozinho
  // Retorna: a vaga do pool devolve o dono e volta a dormir. Nunca vTaskDelete.
}

// So na interface (a unica que muda 'def').
static fundo_s* achar(const fundo_def_t* d)
{
  for (int i = 0; i < FUNDO_MAX; ++i) {
    if (s_f[i].em_uso && s_f[i].def == d) { return &s_f[i]; }
  }
  return nullptr;
}

// Volta a vaga a livre preservando os dois semaforos, e esvazia o 'acordar':
// um aviso que sobrou da parada nao pode fazer o proximo loop pular a
// primeira espera. (O 'saiu' e esvaziado pelo parada_preparar do proximo.)
static void zerar_vaga(fundo_s* f)
{
  SemaphoreHandle_t acordar = f->acordar;
  SemaphoreHandle_t saiu    = f->parada.saiu;
  taskENTER_CRITICAL(&s_mux);
  memset(f, 0, sizeof(*f));   // em_uso = false
  f->acordar = acordar;
  f->parada.saiu = saiu;
  taskEXIT_CRITICAL(&s_mux);
  if (acordar) { xSemaphoreTake(acordar, 0); }
}

// A limpeza comum a todo fim: parada pedida, loop que terminou sozinho, ou
// abandonada que por fim saiu. Sempre na interface.
static void finalizar(fundo_s* f)
{
  if (f->def->ao_parar) { f->def->ao_parar(); }
  notif_limpar(f->notif);
  ESP_LOGI(TAG, "'%s' parou (rodou %lld s)", f->def->nome,
           (esp_timer_get_time() - f->desde_us) / 1000000);
  zerar_vaga(f);
  s_perfil_sujo = true;
}

// A tarefa da vaga ja saiu (deu o semaforo)? Nao consome o semaforo.
static bool saiu(const fundo_s* f) { return !parada_rodando(&f->parada); }

bool sistema_fundo_iniciar(const fundo_def_t* d)
{
  if (!d || !d->corpo || !d->nome) { return false; }
  if (achar(d)) { return false; }   // ja rodando (ou ainda saindo)

  fundo_s* f = nullptr;
  for (int i = 0; i < FUNDO_MAX && !f; ++i) {
    if (!s_f[i].em_uso) { f = &s_f[i]; }
  }
  if (!f) {
    ESP_LOGW(TAG, "sem vaga para '%s' (limite %d)", d->nome, FUNDO_MAX);
    return false;
  }

  if (d->ao_iniciar && !d->ao_iniciar()) {
    ESP_LOGW(TAG, "'%s' recusou iniciar", d->nome);
    return false;
  }

  if (!f->acordar) { f->acordar = xSemaphoreCreateBinary(); }   // uma vez por vaga, para sempre
  zerar_vaga(f);
  f->def      = d;
  f->desde_us = esp_timer_get_time();
  f->cedeu_us = f->desde_us;
  snprintf(f->resumo, sizeof(f->resumo), "rodando");

  // VISIVEL DESDE O PRIMEIRO INSTANTE - a pastilha cinza e a promessa de
  // "nada roda escondido". O registro e idempotente pelo nome.
  f->notif = notif_registrar(d->nome);
  notif_silenciar(f->notif, d->com_som ? !d->com_som() : false);
  notif_publicar(f->notif, 1, NOTIF_ESTADO, f->resumo);

  const bool pronta = f->acordar && parada_preparar(&f->parada);
  taskENTER_CRITICAL(&s_mux);
  f->em_uso = true;   // por ultimo e sob a trava: quem lista ve a vaga ja completa
  taskEXIT_CRITICAL(&s_mux);
  if (!pronta
   || !pool_criar(nullptr, trampolim, f, d->nome, d->pilha ? d->pilha : 4096,
                  FUNDO_PRIORIDADE, DONO_FUNDO, d->nome)) {   // dono: ver hal/tab5_owners.hpp
    ESP_LOGE(TAG, "sem memoria para a tarefa de '%s'", d->nome);
    parada_desistir(&f->parada);
    finalizar(f);
    return false;
  }
  s_perfil_sujo = true;
  sistema_acordar_interface();
  ESP_LOGI(TAG, "'%s' rodando em fundo", d->nome);
  return true;
}

bool sistema_fundo_parar(const fundo_def_t* d)
{
  fundo_s* f = achar(d);
  if (!f || f->parando || f->abandonada) { return false; }   // abandonada: ja desistimos dela

  f->parando = true;
  // O pedido ANTES de acordar: o loop acordado tem que ja ver "pare", senao
  // volta a dormir a espera inteira dele (fundo_esperar pode ser de minutos).
  f->parada.pedido = true;
  xSemaphoreGive(f->acordar);
  if (parada_pedir(&f->parada, FUNDO_PARAR_MS)) {   // espera no semaforo, sem perguntar
    finalizar(f);
    return true;
  }
  // Nao matamos a tarefa (ver o porque em kernel/app_task.h): o pedido fica
  // de pe, ela sai quando puder, e fundo_manter() limpa nessa hora. A
  // pastilha diz a verdade enquanto isso.
  ESP_LOGE(TAG, "'%s' nao saiu em %d ms - fica parando em fundo", d->nome, FUNDO_PARAR_MS);
  notif_publicar(f->notif, 1, NOTIF_ESTADO, "parando...");
  f->abandonada = true;
  f->parando = false;
  return false;
}

bool sistema_fundo_ativo(const fundo_def_t* d)
{
  bool ativo = false;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < FUNDO_MAX; ++i) {
    const fundo_s* f = &s_f[i];
    if (f->em_uso && f->def == d && !f->abandonada && !parada_pedida(&f->parada) && !saiu(f)) { ativo = true; }
  }
  taskEXIT_CRITICAL(&s_mux);
  return ativo;
}

int sistema_fundo_listar(fundo_info_t* out, int max)
{
  int n = 0;
  const int64_t agora = esp_timer_get_time();
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < FUNDO_MAX && n < max; ++i) {
    const fundo_s* f = &s_f[i];
    if (!f->em_uso) { continue; }
    out[n].nome = f->def->nome;
    out[n].ha_s = (uint32_t)((agora - f->desde_us) / 1000000);
    memcpy(out[n].resumo, f->resumo, sizeof(out[n].resumo));
    ++n;
  }
  taskEXIT_CRITICAL(&s_mux);
  return n;
}

// De QUALQUER tarefa (o console): so marca, e a interface para de verdade em
// fundo_manter() - assim ao_parar() roda sempre na interface, como promete o
// contrato.
bool sistema_fundo_parar_nome(const char* nome)
{
  // Sob a trava: roda no console, e a interface pode estar zerando a vaga.
  bool achou = false;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < FUNDO_MAX; ++i) {
    fundo_s* f = &s_f[i];
    if (f->em_uso && !f->abandonada && strcmp(f->def->nome, nome) == 0) {
      f->parar_pedido = true;
      achou = true;
    }
  }
  taskEXIT_CRITICAL(&s_mux);
  if (achou) { sistema_acordar_interface(); }
  return achou;
}

// ---- o loop ---------------------------------------------------------------

bool fundo_esperar(fundo_t* f, uint32_t ms)
{
  if (parada_pedida(&f->parada)) { return false; }
  xSemaphoreTake(f->acordar, pdMS_TO_TICKS(ms));
  f->cedeu_us = esp_timer_get_time();
  return !parada_pedida(&f->parada);
}

bool fundo_ceder(fundo_t* f)
{
  const int64_t agora = esp_timer_get_time();
  if (agora - f->cedeu_us >= FUNDO_CEDER_US) {
    vTaskDelay(1);   // o IDLE deste core roda: watchdog e energia em dia
    f->cedeu_us = esp_timer_get_time();
  }
  return !parada_pedida(&f->parada);
}

bool fundo_rodando(const fundo_t* f) { return !parada_pedida(&f->parada); }

void fundo_estado(fundo_t* f, int quantidade, const char* resumo)
{
  char r[NOTIF_RESUMO];
  snprintf(r, sizeof(r), "%s", resumo ? resumo : "");
  taskENTER_CRITICAL(&s_mux);
  memcpy(f->resumo, r, sizeof(r));
  taskEXIT_CRITICAL(&s_mux);
  // Quantidade minima 1: a pastilha nunca some enquanto o fundo roda.
  notif_publicar(f->notif, quantidade > 0 ? quantidade : 1, NOTIF_ESTADO, r);
}

// ---- o kernel ---------------------------------------------------------------

// Sob a trava: sistema_impor_preferencias() pode ser chamada fora da
// interface. So SOMA - e o teto de clock (cpu_max) fica com a tela: um fundo
// com o perfil padrao (360) apagaria o teto mais baixo que um app pediu.
void fundo_somar_perfil(tab5_perfil_t& p)
{
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < FUNDO_MAX; ++i) {
    const fundo_s* f = &s_f[i];
    if (!f->em_uso) { continue; }
    const tab5_perfil_t& q = f->def->perfil;
    p.imu          = p.imu          || q.imu;
    p.microfone    = p.microfone    || q.microfone;
    p.alto_falante = p.alto_falante || q.alto_falante;
    p.grove_5v     = p.grove_5v     || q.grove_5v;
    p.usb_a_5v     = p.usb_a_5v     || q.usb_a_5v;
    p.camera       = p.camera       || q.camera;
    p.radio        = p.radio        || q.radio;
    if (q.cpu_min_mhz > p.cpu_min_mhz) { p.cpu_min_mhz = q.cpu_min_mhz; }
  }
  taskEXIT_CRITICAL(&s_mux);
}

int fundo_piso_repouso(void)
{
  int piso = 0;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < FUNDO_MAX; ++i) {
    if (s_f[i].em_uso && s_f[i].def->piso_repouso_mhz > piso) {
      piso = s_f[i].def->piso_repouso_mhz;
    }
  }
  taskEXIT_CRITICAL(&s_mux);
  return piso;
}

void fundo_manter(void)
{
  for (int i = 0; i < FUNDO_MAX; ++i) {
    fundo_s* f = &s_f[i];
    if (!f->em_uso || f->parando) { continue; }
    if (saiu(f)) {
      // O loop terminou sozinho, ou a abandonada por fim saiu: recolhe o
      // semaforo (ele foi dado) e limpa.
      parada_pedir(&f->parada, 0);
      finalizar(f);
    } else if (f->parar_pedido && !f->abandonada) {
      f->parar_pedido = false;
      sistema_fundo_parar(f->def);
    }
  }
  // O conjunto mudou: o perfil efetivo (tela + fundos) e reaplicado aqui, na
  // interface, e so aqui. EM REPOUSO ESPERA: aplicar perfil mexe em brilho e
  // teto de clock, que o repouso esta segurando - vale ao acordar.
  if (!tab5_power_em_repouso() && s_perfil_sujo.exchange(false)) { sistema_reaplicar_perfil(); }
}
