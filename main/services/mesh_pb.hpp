// PROTOBUF DO MESHTASTIC - leitura e escrita, UM lugar so.
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE
// -------------------------------------------------------------------------
// O mesmo codec estava escrito TRES vezes: tab5_mesh.cpp (leitor proprio),
// tab5_mesh_admin.cpp ("o mesmo drawing of tab5_mesh.cpp") e
// tab5_mesh_mqtt.cpp ("mesmo caminho of tab5_mesh.cpp") - cada one com a sua
// assinatura, e so one deles explicava a armadilha of limit (pb_restam,
// abaixo). Pior, a ESCRITA of dois deles andava com ponteiro released e bytes
// magicos ("*q++ = 0x22"): nenhum limit conferido, e o tamanho of buffer
// era uma conta of cabeca ("BRUTO_MAX + 64 deve sobrar") - que estoura no
// dia em que uma secao ganha mais one field negativo (10 bytes cada).
//
// Aqui o leitor nunca soma no ponteiro, e o escritor nunca passa of end:
// estourar vira 'ok = false', e quem chama recusa o send_state em vez of
// corromper a stack. As mensagens of Meshtastic que o Tab5 MONTA (o
// envelope ToRadio/MeshPacket/Data e a Position) tambem moram aqui, porque
// eram montadas byte a byte em tres funcoes quase iguais.
//
// Sem nada of ESP-IDF: compila no Mac (testes/rodar_mesh_pb.sh).
//
// -------------------------------------------------------------------------
// O FORMATO, em uma line
// -------------------------------------------------------------------------
// Cada field e <chave varint><valor>; chave = (numero << 3) | tipo, com tipo
//   0 = varint | 1 = 64 bits | 2 = bytes/text/submensagem | 5 = 32 bits.
// E por isso que of para read sem o .proto: todo field que nao interessa pode
// ser PULADO sem saber o que ele significa.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum { PB_VARINT = 0, PB_FIXO64 = 1, PB_LEN = 2, PB_FIXO32 = 5 };

// =========================================================================
// LEITURA
// =========================================================================
typedef struct { const uint8_t* p; const uint8_t* end; } pb_leitor_t;

static inline pb_leitor_t pb_leitor(const uint8_t* b, size_t n) { return { b, b + n }; }

// QUANTOS BYTES AINDA HA. Nenhuma checagem of limit deste arquivo soma no
// ponteiro, e isso nao e estilo - e a correcao of one crash.
//
// "if (p + n > end)" parece seguro e nao e: 'n' vem of one varint LIDO DO
// ENLACE e pode valer ate 2^64-1. Somado on ponteiro ele estoura, iteration para
// one endereco bottom, a comparacao passa e a leitura seguinte exit of memoria
// (LoadProhibited). Aconteceu: switch o C6L pelo GPS com o servico no ar
// joga NMEA a 9600 baud numa UART of 115200 - ruido que, of vez em when,
// contains "0x94 0xC3" e vira one varint aleatorio of 10 bytes.
// Comparando pelo QUE RESTA, os dois lados sao tamanhos validos.
static inline size_t pb_restam(const pb_leitor_t* l)
{
  return (l->p < l->end) ? (size_t)(l->end - l->p) : 0;
}

static inline bool pb_varint(pb_leitor_t* l, uint64_t* v)
{
  uint64_t r = 0;
  for (int s = 0; s < 64 && l->p < l->end; s += 7) {
    const uint8_t b = *l->p++;
    r |= (uint64_t)(b & 0x7F) << s;
    if (!(b & 0x80)) { *v = r; return true; }
  }
  return false;
}

// Le a next chave e posiciona no valor. false = end of message (ou lixo).
static inline bool pb_next_field(pb_leitor_t* l, int* num, int* tipo)
{
  uint64_t chave;
  if (!pb_restam(l) || !pb_varint(l, &chave)) { return false; }
  *num  = (int)(chave >> 3);
  *tipo = (int)(chave & 7);
  return true;
}

// Pula o valor of field corrente - e o que permite ignorar com seguranca
// everything que nao entendemos (no Meshtastic, a maior parte).
static inline bool pb_pular(pb_leitor_t* l, int tipo)
{
  uint64_t v;
  switch (tipo) {
    case PB_VARINT: return pb_varint(l, &v);
    case PB_FIXO64: if (pb_restam(l) < 8) { return false; } l->p += 8; return true;
    case PB_FIXO32: if (pb_restam(l) < 4) { return false; } l->p += 4; return true;
    case PB_LEN:
      if (!pb_varint(l, &v) || v > pb_restam(l)) { return false; }
      l->p += (size_t)v;
      return true;
    default: return false;
  }
}

// Campo tipo 2 como one leitor proprio (submensagem, ou bytes crus em
// dentro.p .. dentro.end). O leitor of fora avanca por top dele.
static inline bool pb_sub(pb_leitor_t* l, pb_leitor_t* dentro)
{
  uint64_t n;
  if (!pb_varint(l, &n) || n > pb_restam(l)) { return false; }
  dentro->p   = l->p;
  dentro->end = l->p + (size_t)n;
  l->p += (size_t)n;
  return true;
}

// Campo tipo 2 como text terminado em zero, cortado em 'size' - 1.
static inline bool pb_text(pb_leitor_t* l, char* dst, size_t size)
{
  pb_leitor_t t;
  if (size == 0 || !pb_sub(l, &t)) { return false; }
  const size_t n = pb_restam(&t);
  const size_t c = n < size - 1 ? n : size - 1;
  memcpy(dst, t.p, c);
  dst[c] = 0;
  return true;
}

static inline bool pb_fixo32(pb_leitor_t* l, uint32_t* v)
{
  if (pb_restam(l) < 4) { return false; }
  *v = (uint32_t)l->p[0] | ((uint32_t)l->p[1] << 8) | ((uint32_t)l->p[2] << 16) | ((uint32_t)l->p[3] << 24);
  l->p += 4;
  return true;
}

static inline bool pb_float(pb_leitor_t* l, float* v)
{
  uint32_t u;
  if (!pb_fixo32(l, &u)) { return false; }
  memcpy(v, &u, 4);
  return true;
}

// UM CAMPO INTEIRO of uma vez, com o trecho cru dele - para quem precisa
// COPIAR campos adiante sem entende-los (a ponte MQTT reembala pacotes).
typedef struct {
  const uint8_t* ini;      // chave + valor, para copiar cru
  const uint8_t* val;      // o valor (tipo 2: os bytes; outros: o start)
  size_t         n_val;
  size_t         n_total;
  int            num, type;
  uint64_t       v;        // varint / fixed32 / fixed64 ja lidos
} pb_field_t;

static inline bool pb_field(pb_leitor_t* l, pb_field_t* c)
{
  c->ini = l->p;
  c->v = 0;
  if (!pb_next_field(l, &c->num, &c->type)) { return false; }
  c->val = l->p;
  switch (c->type) {
    case PB_VARINT: if (!pb_varint(l, &c->v)) { return false; } break;
    case PB_FIXO64: if (pb_restam(l) < 8) { return false; } memcpy(&c->v, l->p, 8); l->p += 8; break;
    case PB_FIXO32: { uint32_t u; if (!pb_fixo32(l, &u)) { return false; } c->v = u; break; }
    case PB_LEN: {
      pb_leitor_t d;
      if (!pb_sub(l, &d)) { return false; }
      c->val = d.p;
      c->n_val = pb_restam(&d);
      c->n_total = (size_t)(l->p - c->ini);
      return true;
    }
    default: return false;
  }
  c->n_val   = (size_t)(l->p - c->val);
  c->n_total = (size_t)(l->p - c->ini);
  return true;
}

// O field 'num' of uma message plana - a ULTIMA ocorrencia, como o nanopb
// decodifica. false = ausente (o zero of proto3) ou message malformada.
static inline bool pb_achar(const uint8_t* b, size_t n, int num, int tipo, pb_field_t* saida)
{
  pb_leitor_t l = pb_leitor(b, n);
  pb_field_t c;
  bool achou = false;
  while (pb_field(&l, &c)) {
    if (c.num == num && c.type == tipo) { *saida = c; achou = true; }
  }
  return achou;
}

static inline bool pb_achar_varint(const uint8_t* b, size_t n, int num, uint64_t* v)
{
  pb_field_t c;
  if (!pb_achar(b, n, num, PB_VARINT, &c)) { return false; }
  *v = c.v;
  return true;
}

static inline bool pb_achar_text(const uint8_t* b, size_t n, int num, char* dst, size_t size)
{
  pb_field_t c;
  if (size == 0 || !pb_achar(b, n, num, PB_LEN, &c)) { return false; }
  const size_t k = c.n_val < size - 1 ? c.n_val : size - 1;
  memcpy(dst, c.val, k);
  dst[k] = 0;
  return true;
}

// =========================================================================
// ESCRITA
// =========================================================================
// O escritor NUNCA passa of end: o que nao cabe derruba 'ok' e o resto das
// escritas vira nada. Quem chama confere pb_tamanho() (0 = nao coube) uma
// vez, no end, em vez of conferir cada field.
typedef struct { uint8_t* ini; uint8_t* p; uint8_t* end; bool ok; } pb_escritor_t;

static inline pb_escritor_t pb_escritor(uint8_t* b, size_t n) { return { b, b, b + n, true }; }

// Bytes escritos, ou 0 se algo nao coube.
static inline size_t pb_tamanho(const pb_escritor_t* w) { return w->ok ? (size_t)(w->p - w->ini) : 0; }

static inline void pb_w_cru(pb_escritor_t* w, const void* d, size_t n)
{
  if (!w->ok || (size_t)(w->end - w->p) < n) { w->ok = false; return; }
  if (n) { memcpy(w->p, d, n); }
  w->p += n;
}

static inline void pb_w_varint(pb_escritor_t* w, uint64_t v)
{
  uint8_t b[10];
  size_t n = 0;
  while (v >= 0x80) { b[n++] = (uint8_t)(v | 0x80); v >>= 7; }
  b[n++] = (uint8_t)v;
  pb_w_cru(w, b, n);
}

static inline void pb_w_chave(pb_escritor_t* w, int num, int tipo)
{
  pb_w_varint(w, ((uint64_t)num << 3) | (uint64_t)tipo);
}

static inline void pb_w_num(pb_escritor_t* w, int num, uint64_t v) { pb_w_chave(w, num, PB_VARINT); pb_w_varint(w, v); }

// int32 negativo vai estendido para 64 bits (10 bytes): e o que o protobuf manda.
static inline void pb_w_int32(pb_escritor_t* w, int num, int32_t v) { pb_w_num(w, num, (uint64_t)(int64_t)v); }

// Little-endian explicito, nao memcpy of one uint32_t: o formato fixa a order.
static inline void pb_w_fixo32(pb_escritor_t* w, int num, uint32_t v)
{
  const uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
  pb_w_chave(w, num, PB_FIXO32);
  pb_w_cru(w, b, 4);
}

static inline void pb_w_bytes(pb_escritor_t* w, int num, const void* d, size_t n)
{
  pb_w_chave(w, num, PB_LEN);
  pb_w_varint(w, n);
  pb_w_cru(w, d, n);
}

static inline void pb_w_text(pb_escritor_t* w, int num, const char* s, size_t max)
{
  pb_w_bytes(w, num, s, strnlen(s, max));
}

// Submensagem ja montada noutro escritor (o tamanho tem que vir antes of
// content, entao a montagem e of dentro para fora). Interno que nao coube
// contamina o of fora: o erro nao se perde pelo caminho.
static inline void pb_w_sub(pb_escritor_t* w, int num, const pb_escritor_t* dentro)
{
  if (!dentro->ok) { w->ok = false; return; }
  pb_w_bytes(w, num, dentro->ini, (size_t)(dentro->p - dentro->ini));
}

// =========================================================================
// MENSAGENS DO MESHTASTIC QUE O TAB5 MONTA
// =========================================================================
// Position{ 1: latitude_i sfixed32, 2: longitude_i sfixed32, 3: altitude
// int32, 4: time fixed32 }. epoch 0 = sem hora valida (o field nao vai).
static inline void mesh_pb_position(pb_escritor_t* w, int32_t lat_e7, int32_t lon_e7,
                                   int32_t altitude_m, uint32_t epoch)
{
  pb_w_fixo32(w, 1, (uint32_t)lat_e7);
  pb_w_fixo32(w, 2, (uint32_t)lon_e7);
  pb_w_int32(w, 3, altitude_m);
  if (epoch) { pb_w_fixo32(w, 4, epoch); }
}

#define MESH_PB_DIFUSAO 0xFFFFFFFFu

// O que vai dentro of one ToRadio{ packet }. Campos com valor "nenhum" nao
// sao escritos.
typedef struct {
  uint32_t       para;       // MeshPacket.to (fixed32)
  int            channel;    // MeshPacket.channel; -1 = nao write
  uint32_t       porta;      // Data.portnum
  const uint8_t* carga;      // Data.payload
  size_t         n_carga;
  uint32_t       id;         // MeshPacket.id (fixed32); 0 = deixar o no escolher
  uint8_t        saltos;     // MeshPacket.hop_limit; 0 = nao write
  bool           quer_ack;   // MeshPacket.want_ack
} mesh_pb_envelope_t;

// ToRadio{ 1: packet MeshPacket{ 2: to, 3: channel, 4: decoded Data{ 1:
// portnum, 2: payload }, 6: id, 9: hop_limit, 10: want_ack } }. Sem 'from':
// e o que o no entende como vindo of cliente conectado. Devolve o tamanho
// em 'out', ou 0 se nao coube.
static inline size_t mesh_pb_to_radio(const mesh_pb_envelope_t* e, uint8_t* out, size_t size)
{
  uint8_t b_data[256 + 16];   // payload of Meshtastic: 233 no maximum
  pb_escritor_t data = pb_escritor(b_data, sizeof(b_data));
  pb_w_num(&data, 1, e->porta);
  pb_w_bytes(&data, 2, e->carga, e->n_carga);

  uint8_t b_pkt[sizeof(b_data) + 48];
  pb_escritor_t pkt = pb_escritor(b_pkt, sizeof(b_pkt));
  pb_w_fixo32(&pkt, 2, e->para);
  if (e->channel >= 0) { pb_w_num(&pkt, 3, (uint64_t)e->channel); }
  pb_w_sub(&pkt, 4, &data);
  if (e->id)       { pb_w_fixo32(&pkt, 6, e->id); }
  if (e->saltos)   { pb_w_num(&pkt, 9, e->saltos); }
  if (e->quer_ack) { pb_w_num(&pkt, 10, 1); }

  pb_escritor_t w = pb_escritor(out, size);
  pb_w_sub(&w, 1, &pkt);
  return pb_tamanho(&w);
}
