#include "journal_core.hpp"

#include <string.h>

// ---------------------------------------------------------------------------
// O registro na flash
// ---------------------------------------------------------------------------
//   [header 36 B][app][file][header do file][data][0xFF ate multiplo de 4]
//
// O CRC cobre o header ate 'crc' (exclusive) e o resto do registro, menos o
// enchimento. 'aplicado' fica de fora: e o unico byte regravado depois.
#define RESERVA_MAGIA 0x31565352u   // "RSV1" em little endian

typedef struct {
  uint32_t magia;
  uint8_t  type, flags, n_app, n_arq;
  uint16_t n_cab, livre1;
  uint32_t data_size;
  uint32_t base_tam, base_data;
  uint8_t  base_existia, livre2[3];
  uint32_t crc;
  uint8_t  aplicado;      // 0xFF = pending, 0x00 = ja esta no cartao
  uint8_t  livre3[3];
} reserva_cab_t;
static_assert(sizeof(reserva_cab_t) == 36, "o formato na flash nao pode mudar sem mudar a magia");

#define CRC_ATE    ((uint32_t)offsetof(reserva_cab_t, crc))
#define POS_APLIC  ((uint32_t)offsetof(reserva_cab_t, aplicado))

static uint32_t crc32_mais(uint32_t crc, const void* data, size_t n)
{
  const uint8_t* p = (const uint8_t*)data;
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; ++k) { crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u))); }
  }
  return ~crc;
}

static uint32_t arredonda4(uint32_t n) { return (n + 3u) & ~3u; }
static uint32_t prox_setor(uint32_t p) { return (p / JOURNAL_SECTOR + 1u) * JOURNAL_SECTOR; }

static uint32_t corpo(const reserva_cab_t* c)
{
  return (uint32_t)c->n_app + c->n_arq + c->n_cab + c->data_size;
}

static bool tudo_ff(const void* b, size_t n)
{
  const uint8_t* p = (const uint8_t*)b;
  for (size_t i = 0; i < n; ++i) { if (p[i] != 0xFF) { return false; } }
  return true;
}

// Cabecalho com cara de registro e cabendo no storage a partir de 'pos'.
static bool cab_plausivel(const journal_t* r, uint32_t pos, const reserva_cab_t* c)
{
  if (c->magia != RESERVA_MAGIA) { return false; }
  if (c->type != JOURNAL_APPEND && c->type != JOURNAL_REPLACE) { return false; }
  if (c->n_app == 0 || c->n_arq == 0) { return false; }
  if (c->aplicado != 0xFF && c->aplicado != 0x00) { return false; }
  const uint64_t size = (uint64_t)sizeof(reserva_cab_t) + corpo(c);
  return (uint64_t)pos + size <= r->storage.size;
}

// CRC do corpo lendo a flash em blocos (sem precisar do registro inteiro na RAM).
static bool crc_confere(const journal_t* r, uint32_t pos, const reserva_cab_t* c)
{
  uint32_t crc = crc32_mais(0, c, CRC_ATE);
  uint8_t bloco[256];
  uint32_t falta = corpo(c), p = pos + (uint32_t)sizeof(reserva_cab_t);
  while (falta) {
    const uint32_t k = falta < sizeof(bloco) ? falta : (uint32_t)sizeof(bloco);
    if (!r->storage.read(r->storage.ctx, p, bloco, k)) { return false; }
    crc = crc32_mais(crc, bloco, k);
    p += k; falta -= k;
  }
  return crc == c->crc;
}

static bool apagar_tudo(journal_t* r)
{
  if (r->end == 0) { return true; }
  const uint32_t ate = (r->end % JOURNAL_SECTOR) ? prox_setor(r->end) : r->end;
  const uint32_t n = ate < r->storage.size ? ate : r->storage.size;
  if (!r->storage.erase(r->storage.ctx, 0, n)) { return false; }
  r->end = 0; r->valid_entries = 0; r->pending = 0; r->dirty = false;
  return true;
}

// ---------------------------------------------------------------------------
// Varredura
// ---------------------------------------------------------------------------
// Para cada position: header todo 0xFF = end do diario; registro inteiro e
// com CRC certo = vale; qualquer outra coisa = lixo de uma gravacao
// interrompida, e a busca segue no proximo setor (onde a gravacao seguinte
// recomecou). Termina com 'end' sempre numa area apagada.
static bool scan(journal_t* r, uint32_t* valid_entries, uint32_t* pending, uint32_t* end, bool* dirty)
{
  *valid_entries = 0; *pending = 0; *dirty = false;
  uint32_t pos = 0;
  while (pos + sizeof(reserva_cab_t) <= r->storage.size) {
    reserva_cab_t c;
    if (!r->storage.read(r->storage.ctx, pos, &c, sizeof(c))) { return false; }
    if (tudo_ff(&c, sizeof(c))) { break; }
    if (cab_plausivel(r, pos, &c) && crc_confere(r, pos, &c)) {
      ++*valid_entries;
      if (c.aplicado == 0xFF) { ++*pending; }
      pos += arredonda4((uint32_t)sizeof(reserva_cab_t) + corpo(&c));
      continue;
    }
    *dirty = true;
    pos = prox_setor(pos);
  }
  *end = pos < r->storage.size ? pos : r->storage.size;
  return true;
}

bool journal_open(journal_t* r, const journal_storage_t* storage)
{
  memset(r, 0, sizeof(*r));
  r->storage = *storage;
  if (!scan(r, &r->valid_entries, &r->pending, &r->end, &r->dirty)) { return false; }
  // Nada a aplicar mas algo escrito (tudo ja aplicado antes de um reinicio,
  // ou so lixo): apaga ja - o diario vazio e o state normal.
  if (r->pending == 0 && r->end > 0 && !apagar_tudo(r)) { return false; }
  r->ready = true;
  return true;
}

// ---------------------------------------------------------------------------
// Gravar
// ---------------------------------------------------------------------------
bool journal_write(journal_t* r, const journal_entry_t* reg)
{
  if (!r->ready || !reg || !reg->app || !reg->file) { return false; }
  const size_t n_app = strlen(reg->app), n_arq = strlen(reg->file);
  if (n_app == 0 || n_app > 255 || n_arq == 0 || n_arq > 255) { return false; }
  if (reg->type != JOURNAL_APPEND && reg->type != JOURNAL_REPLACE) { return false; }

  reserva_cab_t c;
  memset(&c, 0xFF, sizeof(c));   // os campos livres ficam como a flash apagada
  c.magia = RESERVA_MAGIA;
  c.type = reg->type; c.flags = reg->flags;
  c.n_app = (uint8_t)n_app; c.n_arq = (uint8_t)n_arq;
  c.n_cab = reg->header ? reg->header_size : 0;
  c.livre1 = 0xFFFF;
  c.data_size = reg->data ? reg->data_size : 0;
  c.base_tam = reg->base_tamanho; c.base_data = reg->base_data;
  c.base_existia = reg->base_existia ? 1 : 0;
  c.aplicado = 0xFF;

  const uint32_t size = arredonda4((uint32_t)sizeof(c) + corpo(&c));
  if ((uint64_t)r->end + size > r->storage.size) { return false; }   // cheio

  uint32_t crc = crc32_mais(0, &c, CRC_ATE);
  crc = crc32_mais(crc, reg->app, n_app);
  crc = crc32_mais(crc, reg->file, n_arq);
  if (c.n_cab)   { crc = crc32_mais(crc, reg->header, c.n_cab); }
  if (c.data_size) { crc = crc32_mais(crc, reg->data, c.data_size); }
  c.crc = crc;

  // Em sequencia: uma queda em qualquer point deixa um registro que failure o
  // CRC, e a varredura pula para o proximo setor.
  uint32_t p = r->end;
  bool ok = r->storage.write(r->storage.ctx, p, &c, sizeof(c));
  p += sizeof(c);
  if (ok) { ok = r->storage.write(r->storage.ctx, p, reg->app, (uint32_t)n_app); p += (uint32_t)n_app; }
  if (ok) { ok = r->storage.write(r->storage.ctx, p, reg->file, (uint32_t)n_arq); p += (uint32_t)n_arq; }
  if (ok && c.n_cab)   { ok = r->storage.write(r->storage.ctx, p, reg->header, c.n_cab); p += c.n_cab; }
  if (ok && c.data_size) { ok = r->storage.write(r->storage.ctx, p, reg->data, c.data_size); }
  if (!ok) {
    // O que chegou a ser gravado nao volta a 0xFF: o proximo registro
    // comeca no primeiro setor ainda apagado, como depois de uma queda (um
    // registro grande pode ter sujado mais de um).
    r->dirty = true;
    uint32_t q = prox_setor(r->end);
    while (q + sizeof(reserva_cab_t) <= r->storage.size) {
      reserva_cab_t t;
      if (!r->storage.read(r->storage.ctx, q, &t, sizeof(t)) || tudo_ff(&t, sizeof(t))) { break; }
      q = prox_setor(q);
    }
    r->end = q < r->storage.size ? q : r->storage.size;
    return false;
  }
  r->end += size;
  r->valid_entries++;
  r->pending++;
  return true;
}

// ---------------------------------------------------------------------------
// Aplicar
// ---------------------------------------------------------------------------
int journal_apply(journal_t* r, journal_apply_fn fn, void* ctx)
{
  if (!r->ready) { return -1; }
  int aplicados = 0;
  uint32_t pos = 0;
  while (r->pending > 0 && pos + sizeof(reserva_cab_t) <= r->end) {
    reserva_cab_t c;
    if (!r->storage.read(r->storage.ctx, pos, &c, sizeof(c))) { return -1; }
    if (!cab_plausivel(r, pos, &c) || !crc_confere(r, pos, &c)) { pos = prox_setor(pos); continue; }
    const uint32_t size = arredonda4((uint32_t)sizeof(c) + corpo(&c));
    if (c.aplicado != 0xFF) { pos += size; continue; }

    // O registro inteiro na memoria, com NUL depois de cada text:
    //   [app][0][file][0][header][data]
    // Os nomes ficam no MESMO bloco, nao na stack: who aplica pode ser
    // qualquer tarefa que le o cartao (tab5_sd_open aplica antes de abrir),
    // e a descida ate o FAT ja e funda - 512 B a menos nela.
    const uint32_t n = corpo(&c);
    uint8_t* b = (uint8_t*)r->storage.allocate((size_t)n + 2);
    if (!b) { return aplicados; }   // sem memoria agora: tenta depois, na mesma ordem
    const uint32_t ini = pos + (uint32_t)sizeof(c);
    char* app = (char*)b;
    char* arq = app + c.n_app + 1;
    uint8_t* resto = (uint8_t*)arq + c.n_arq + 1;
    const uint32_t n_resto = n - c.n_app - c.n_arq;
    if (!r->storage.read(r->storage.ctx, ini, app, c.n_app)
     || !r->storage.read(r->storage.ctx, ini + c.n_app, arq, c.n_arq)
     || (n_resto && !r->storage.read(r->storage.ctx, ini + c.n_app + c.n_arq, resto, n_resto))) {
      r->storage.release(b);
      return -1;
    }
    app[c.n_app] = 0;
    arq[c.n_arq] = 0;
    journal_entry_t reg;
    reg.type = c.type; reg.flags = c.flags;
    reg.app = app; reg.file = arq;
    reg.header   = c.n_cab ? resto : nullptr;
    reg.header_size = c.n_cab;
    reg.data       = resto + c.n_cab;
    reg.data_size     = c.data_size;
    reg.base_existia = c.base_existia == 1;
    reg.base_tamanho = c.base_tam;
    reg.base_data    = c.base_data;
    const bool ok = fn(&reg, ctx);
    r->storage.release(b);
    if (!ok) { return aplicados; }

    static const uint8_t ZERO = 0x00;
    if (!r->storage.write(r->storage.ctx, pos + POS_APLIC, &ZERO, 1)) { return -1; }
    r->pending--;
    aplicados++;
    pos += size;
  }
  if (r->pending == 0 && !apagar_tudo(r)) { return -1; }
  return aplicados;
}
