#include "journal_core.hpp"

#include <string.h>

// ---------------------------------------------------------------------------
// O registro na flash
// ---------------------------------------------------------------------------
//   [cabecalho 36 B][app][arquivo][cabecalho do arquivo][dados][0xFF ate multiplo de 4]
//
// O CRC cobre o cabecalho ate 'crc' (exclusive) e o resto do registro, menos o
// enchimento. 'aplicado' fica de fora: e o unico byte regravado depois.
#define RESERVA_MAGIA 0x31565352u   // "RSV1" em little endian

typedef struct {
  uint32_t magia;
  uint8_t  tipo, flags, n_app, n_arq;
  uint16_t n_cab, livre1;
  uint32_t n_dados;
  uint32_t base_tam, base_data;
  uint8_t  base_existia, livre2[3];
  uint32_t crc;
  uint8_t  aplicado;      // 0xFF = pendente, 0x00 = ja esta no cartao
  uint8_t  livre3[3];
} reserva_cab_t;
static_assert(sizeof(reserva_cab_t) == 36, "o formato na flash nao pode mudar sem mudar a magia");

#define CRC_ATE    ((uint32_t)offsetof(reserva_cab_t, crc))
#define POS_APLIC  ((uint32_t)offsetof(reserva_cab_t, aplicado))

static uint32_t crc32_mais(uint32_t crc, const void* dados, size_t n)
{
  const uint8_t* p = (const uint8_t*)dados;
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; ++k) { crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u))); }
  }
  return ~crc;
}

static uint32_t arredonda4(uint32_t n) { return (n + 3u) & ~3u; }
static uint32_t prox_setor(uint32_t p) { return (p / RESERVA_SETOR + 1u) * RESERVA_SETOR; }

static uint32_t corpo(const reserva_cab_t* c)
{
  return (uint32_t)c->n_app + c->n_arq + c->n_cab + c->n_dados;
}

static bool tudo_ff(const void* b, size_t n)
{
  const uint8_t* p = (const uint8_t*)b;
  for (size_t i = 0; i < n; ++i) { if (p[i] != 0xFF) { return false; } }
  return true;
}

// Cabecalho com cara de registro e cabendo no meio a partir de 'pos'.
static bool cab_plausivel(const reserva_t* r, uint32_t pos, const reserva_cab_t* c)
{
  if (c->magia != RESERVA_MAGIA) { return false; }
  if (c->tipo != RESERVA_ACRESCENTAR && c->tipo != RESERVA_SUBSTITUIR) { return false; }
  if (c->n_app == 0 || c->n_arq == 0) { return false; }
  if (c->aplicado != 0xFF && c->aplicado != 0x00) { return false; }
  const uint64_t tam = (uint64_t)sizeof(reserva_cab_t) + corpo(c);
  return (uint64_t)pos + tam <= r->meio.tamanho;
}

// CRC do corpo lendo a flash em blocos (sem precisar do registro inteiro na RAM).
static bool crc_confere(const reserva_t* r, uint32_t pos, const reserva_cab_t* c)
{
  uint32_t crc = crc32_mais(0, c, CRC_ATE);
  uint8_t bloco[256];
  uint32_t falta = corpo(c), p = pos + (uint32_t)sizeof(reserva_cab_t);
  while (falta) {
    const uint32_t k = falta < sizeof(bloco) ? falta : (uint32_t)sizeof(bloco);
    if (!r->meio.ler(r->meio.ctx, p, bloco, k)) { return false; }
    crc = crc32_mais(crc, bloco, k);
    p += k; falta -= k;
  }
  return crc == c->crc;
}

static bool apagar_tudo(reserva_t* r)
{
  if (r->fim == 0) { return true; }
  const uint32_t ate = (r->fim % RESERVA_SETOR) ? prox_setor(r->fim) : r->fim;
  const uint32_t n = ate < r->meio.tamanho ? ate : r->meio.tamanho;
  if (!r->meio.apagar(r->meio.ctx, 0, n)) { return false; }
  r->fim = 0; r->validos = 0; r->pendentes = 0; r->sujo = false;
  return true;
}

// ---------------------------------------------------------------------------
// Varredura
// ---------------------------------------------------------------------------
// Para cada posicao: cabecalho todo 0xFF = fim do diario; registro inteiro e
// com CRC certo = vale; qualquer outra coisa = lixo de uma gravacao
// interrompida, e a busca segue no proximo setor (onde a gravacao seguinte
// recomecou). Termina com 'fim' sempre numa area apagada.
static bool varrer(reserva_t* r, uint32_t* validos, uint32_t* pendentes, uint32_t* fim, bool* sujo)
{
  *validos = 0; *pendentes = 0; *sujo = false;
  uint32_t pos = 0;
  while (pos + sizeof(reserva_cab_t) <= r->meio.tamanho) {
    reserva_cab_t c;
    if (!r->meio.ler(r->meio.ctx, pos, &c, sizeof(c))) { return false; }
    if (tudo_ff(&c, sizeof(c))) { break; }
    if (cab_plausivel(r, pos, &c) && crc_confere(r, pos, &c)) {
      ++*validos;
      if (c.aplicado == 0xFF) { ++*pendentes; }
      pos += arredonda4((uint32_t)sizeof(reserva_cab_t) + corpo(&c));
      continue;
    }
    *sujo = true;
    pos = prox_setor(pos);
  }
  *fim = pos < r->meio.tamanho ? pos : r->meio.tamanho;
  return true;
}

bool reserva_abrir(reserva_t* r, const reserva_meio_t* meio)
{
  memset(r, 0, sizeof(*r));
  r->meio = *meio;
  if (!varrer(r, &r->validos, &r->pendentes, &r->fim, &r->sujo)) { return false; }
  // Nada a aplicar mas algo escrito (tudo ja aplicado antes de um reinicio,
  // ou so lixo): apaga ja - o diario vazio e o estado normal.
  if (r->pendentes == 0 && r->fim > 0 && !apagar_tudo(r)) { return false; }
  r->pronto = true;
  return true;
}

// ---------------------------------------------------------------------------
// Gravar
// ---------------------------------------------------------------------------
bool reserva_gravar(reserva_t* r, const reserva_reg_t* reg)
{
  if (!r->pronto || !reg || !reg->app || !reg->arquivo) { return false; }
  const size_t n_app = strlen(reg->app), n_arq = strlen(reg->arquivo);
  if (n_app == 0 || n_app > 255 || n_arq == 0 || n_arq > 255) { return false; }
  if (reg->tipo != RESERVA_ACRESCENTAR && reg->tipo != RESERVA_SUBSTITUIR) { return false; }

  reserva_cab_t c;
  memset(&c, 0xFF, sizeof(c));   // os campos livres ficam como a flash apagada
  c.magia = RESERVA_MAGIA;
  c.tipo = reg->tipo; c.flags = reg->flags;
  c.n_app = (uint8_t)n_app; c.n_arq = (uint8_t)n_arq;
  c.n_cab = reg->cabecalho ? reg->n_cabecalho : 0;
  c.livre1 = 0xFFFF;
  c.n_dados = reg->dados ? reg->n_dados : 0;
  c.base_tam = reg->base_tamanho; c.base_data = reg->base_data;
  c.base_existia = reg->base_existia ? 1 : 0;
  c.aplicado = 0xFF;

  const uint32_t tam = arredonda4((uint32_t)sizeof(c) + corpo(&c));
  if ((uint64_t)r->fim + tam > r->meio.tamanho) { return false; }   // cheio

  uint32_t crc = crc32_mais(0, &c, CRC_ATE);
  crc = crc32_mais(crc, reg->app, n_app);
  crc = crc32_mais(crc, reg->arquivo, n_arq);
  if (c.n_cab)   { crc = crc32_mais(crc, reg->cabecalho, c.n_cab); }
  if (c.n_dados) { crc = crc32_mais(crc, reg->dados, c.n_dados); }
  c.crc = crc;

  // Em sequencia: uma queda em qualquer ponto deixa um registro que falha o
  // CRC, e a varredura pula para o proximo setor.
  uint32_t p = r->fim;
  bool ok = r->meio.gravar(r->meio.ctx, p, &c, sizeof(c));
  p += sizeof(c);
  if (ok) { ok = r->meio.gravar(r->meio.ctx, p, reg->app, (uint32_t)n_app); p += (uint32_t)n_app; }
  if (ok) { ok = r->meio.gravar(r->meio.ctx, p, reg->arquivo, (uint32_t)n_arq); p += (uint32_t)n_arq; }
  if (ok && c.n_cab)   { ok = r->meio.gravar(r->meio.ctx, p, reg->cabecalho, c.n_cab); p += c.n_cab; }
  if (ok && c.n_dados) { ok = r->meio.gravar(r->meio.ctx, p, reg->dados, c.n_dados); }
  if (!ok) {
    // O que chegou a ser gravado nao volta a 0xFF: o proximo registro
    // comeca no primeiro setor ainda apagado, como depois de uma queda (um
    // registro grande pode ter sujado mais de um).
    r->sujo = true;
    uint32_t q = prox_setor(r->fim);
    while (q + sizeof(reserva_cab_t) <= r->meio.tamanho) {
      reserva_cab_t t;
      if (!r->meio.ler(r->meio.ctx, q, &t, sizeof(t)) || tudo_ff(&t, sizeof(t))) { break; }
      q = prox_setor(q);
    }
    r->fim = q < r->meio.tamanho ? q : r->meio.tamanho;
    return false;
  }
  r->fim += tam;
  r->validos++;
  r->pendentes++;
  return true;
}

// ---------------------------------------------------------------------------
// Aplicar
// ---------------------------------------------------------------------------
int reserva_aplicar(reserva_t* r, reserva_aplicar_fn fn, void* ctx)
{
  if (!r->pronto) { return -1; }
  int aplicados = 0;
  uint32_t pos = 0;
  while (r->pendentes > 0 && pos + sizeof(reserva_cab_t) <= r->fim) {
    reserva_cab_t c;
    if (!r->meio.ler(r->meio.ctx, pos, &c, sizeof(c))) { return -1; }
    if (!cab_plausivel(r, pos, &c) || !crc_confere(r, pos, &c)) { pos = prox_setor(pos); continue; }
    const uint32_t tam = arredonda4((uint32_t)sizeof(c) + corpo(&c));
    if (c.aplicado != 0xFF) { pos += tam; continue; }

    // O registro inteiro na memoria, com NUL depois de cada texto:
    //   [app][0][arquivo][0][cabecalho][dados]
    // Os nomes ficam no MESMO bloco, nao na pilha: quem aplica pode ser
    // qualquer tarefa que le o cartao (tab5_sd_abrir aplica antes de abrir),
    // e a descida ate o FAT ja e funda - 512 B a menos nela.
    const uint32_t n = corpo(&c);
    uint8_t* b = (uint8_t*)r->meio.alocar((size_t)n + 2);
    if (!b) { return aplicados; }   // sem memoria agora: tenta depois, na mesma ordem
    const uint32_t ini = pos + (uint32_t)sizeof(c);
    char* app = (char*)b;
    char* arq = app + c.n_app + 1;
    uint8_t* resto = (uint8_t*)arq + c.n_arq + 1;
    const uint32_t n_resto = n - c.n_app - c.n_arq;
    if (!r->meio.ler(r->meio.ctx, ini, app, c.n_app)
     || !r->meio.ler(r->meio.ctx, ini + c.n_app, arq, c.n_arq)
     || (n_resto && !r->meio.ler(r->meio.ctx, ini + c.n_app + c.n_arq, resto, n_resto))) {
      r->meio.liberar(b);
      return -1;
    }
    app[c.n_app] = 0;
    arq[c.n_arq] = 0;
    reserva_reg_t reg;
    reg.tipo = c.tipo; reg.flags = c.flags;
    reg.app = app; reg.arquivo = arq;
    reg.cabecalho   = c.n_cab ? resto : nullptr;
    reg.n_cabecalho = c.n_cab;
    reg.dados       = resto + c.n_cab;
    reg.n_dados     = c.n_dados;
    reg.base_existia = c.base_existia == 1;
    reg.base_tamanho = c.base_tam;
    reg.base_data    = c.base_data;
    const bool ok = fn(&reg, ctx);
    r->meio.liberar(b);
    if (!ok) { return aplicados; }

    static const uint8_t ZERO = 0x00;
    if (!r->meio.gravar(r->meio.ctx, pos + POS_APLIC, &ZERO, 1)) { return -1; }
    r->pendentes--;
    aplicados++;
    pos += tam;
  }
  if (r->pendentes == 0 && !apagar_tudo(r)) { return -1; }
  return aplicados;
}
