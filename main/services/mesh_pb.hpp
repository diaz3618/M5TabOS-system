// MESHTASTIC PROTOBUF - reading and writing, ONE place.
//
// ------------------------------------------------------------------------
// WHY DOES THIS EXIST
// ------------------------------------------------------------------------
// The same codec was written THREE times: tab5_mesh.cpp (own reader),
// tab5_mesh_admin.cpp ("the same drawing of tab5_mesh.cpp") and
// tab5_mesh_mqtt.cpp ("same path of tab5_mesh.cpp") - each one with its own
// signature, and only one of them explained the of limit trap (pb_restam,
// below). Worse, the WRITING of two of them was with a released pointer and bytes
// magicos ("*q++ = 0x22"): no limit checked, and the size of buffer
// was an account of head ("GROSS_MAX + 64 should be left") - which overflows in
// day in which a section gains one more negative field (10 bytes each).
//
// Here the reader never adds to the pointer, and the writer never goes past end:
// overflow becomes 'ok = false', and the caller refuses send_state instead of
// corrupt the stack. The Meshtastic messages that the Tab5 ASSEMBLES (the
// ToRadio/MeshPacket/Data envelope and Position) also live here, because
// were assembled byte by byte into three almost identical functions.
//
// Without anything of ESP-IDF: compiles on Mac (tests/rodar_mesh_pb.sh).
//
// ------------------------------------------------------------------------
// THE FORMAT, in one line
// ------------------------------------------------------------------------
// Each field is <varint key><value>; key = (number << 3) | type, with type
// 0 = varint | 1 = 64 bits | 2 = bytes/text/submessage | 5 = 32 bits.
// That's why of is for read without the .proto: every field that doesn't matter can
// be SKIPED without knowing what it means.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum { PB_VARINT = 0, PB_FIXO64 = 1, PB_LEN = 2, PB_FIXO32 = 5 };

// ========================================================================
// READING
// =========================================================================
typedef struct { const uint8_t* p; const uint8_t* end; } pb_leitor_t;

static inline pb_leitor_t pb_leitor(const uint8_t* b, size_t n) { return { b, b + n }; }

// HOW MANY BYTES ARE THERE STILL. No of limit checks on this file add up to the
// pointer, and that's not style - it's the correction of one crash.
//
// "if (p + n > end)" seems safe and is not: 'n' comes from one varint READ DO
// ENLACE and can be worth up to 2^64-1. Added to pointer it overflows, iteration for
// one address bottom, the comparison passes and the next reading exits of memory
// (LoadProhibited). It happened: switch the C6L via GPS with the service in the air
// plays NMEA at 9600 baud on a UART of 115200 - noise that, from time to time,
// contains "0x94 0xC3" and becomes a random varint of 10 bytes.
// Comparing from WHAT REMAINS, both sides are valid sizes.
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

// Reads the next key and places it in the value. false = end of message (or garbage).
static inline bool pb_next_field(pb_leitor_t* l, int* num, int* tipo)
{
  uint64_t chave;
  if (!pb_restam(l) || !pb_varint(l, &chave)) { return false; }
  *num  = (int)(chave >> 3);
  *tipo = (int)(chave & 7);
  return true;
}

// Skips the current field value - which allows you to safely ignore
// everything we don't understand (on Meshtastic, most of it).
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

// Type 2 field as its own reader (submessage, or raw bytes in
// in.p .. in.end). The outside reader advances through its top.
static inline bool pb_sub(pb_leitor_t* l, pb_leitor_t* dentro)
{
  uint64_t n;
  if (!pb_varint(l, &n) || n > pb_restam(l)) { return false; }
  dentro->p   = l->p;
  dentro->end = l->p + (size_t)n;
  l->p += (size_t)n;
  return true;
}

// Field type 2 as zero-terminated text, cut to 'size' - 1.
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

// A WHOLE FIELD of once, with the raw excerpt from it - for those who need it
// COPY fields forward without understanding them (MQTT bridge repackages packets).
typedef struct {
  const uint8_t* ini;      // key + value, for raw copy
  const uint8_t* val;      // the value (type 2: the bytes; others: the start)
  size_t         n_val;
  size_t         n_total;
  int            num, type;
  uint64_t       v;        // varint / fixed32 / fixed64 already read
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

// The field 'num' of a flat message - the LAST occurrence, like nanopb
// decodes. false = missing (the zero of proto3) or malformed message.
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

// ========================================================================
// WRITING
// ========================================================================
// The writer NEVER goes beyond the end: what doesn't fit drops 'ok' and the rest of the
// writing becomes nothing. The caller checks pb_size() (0 = did not fit) one
// instead, at the end, instead of checking each field.
typedef struct { uint8_t* ini; uint8_t* p; uint8_t* end; bool ok; } pb_escritor_t;

static inline pb_escritor_t pb_escritor(uint8_t* b, size_t n) { return { b, b, b + n, true }; }

// Bytes written, or 0 if something didn't fit.
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

// negative int32 is extended to 64 bits (10 bytes): this is what protobuf says.
static inline void pb_w_int32(pb_escritor_t* w, int num, int32_t v) { pb_w_num(w, num, (uint64_t)(int64_t)v); }

// Explicit little-endian, not memcpy of one uint32_t: the format fixes the order.
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

// Submessage already assembled in another writer (the size must come before
// content, so the assembly is from inside out). Interior that didn't fit
// contaminates the outside: the error is not lost along the way.
static inline void pb_w_sub(pb_escritor_t* w, int num, const pb_escritor_t* dentro)
{
  if (!dentro->ok) { w->ok = false; return; }
  pb_w_bytes(w, num, dentro->ini, (size_t)(dentro->p - dentro->ini));
}

// ====================================================================
// MESHTASTIC MESSAGES THAT TAB5 ASSEMBLES
// =========================================================================
// Position{ 1: latitude_i sfixed32, 2: longitude_i sfixed32, 3: altitude
// int32, 4: time fixed32 }. epoch 0 = no valid time (the field does not go).
static inline void mesh_pb_position(pb_escritor_t* w, int32_t lat_e7, int32_t lon_e7,
                                   int32_t altitude_m, uint32_t epoch)
{
  pb_w_fixo32(w, 1, (uint32_t)lat_e7);
  pb_w_fixo32(w, 2, (uint32_t)lon_e7);
  pb_w_int32(w, 3, altitude_m);
  if (epoch) { pb_w_fixo32(w, 4, epoch); }
}

#define MESH_PB_DIFUSAO 0xFFFFFFFFu

// What goes inside one ToRadio{ packet }. Fields with a value of "none" do not
// are written.
typedef struct {
  uint32_t       para;       // MeshPacket.to (fixed32)
  int            channel;    // MeshPacket.channel; -1 = do not write
  uint32_t       porta;      // Data.portnum
  const uint8_t* carga;      // Data.payload
  size_t         n_carga;
  uint32_t       id;         // MeshPacket.id (fixed32); 0 = let the no choose
  uint8_t        saltos;     // MeshPacket.hop_limit; 0 = do not write
  bool           quer_ack;   // MeshPacket.want_ack
} mesh_pb_envelope_t;

// ToRadio{ 1: packet MeshPacket{ 2: to, 3: channel, 4: decoded Data{ 1:
// portnum, 2: payload }, 6: id, 9: hop_limit, 10: want_ack } }. Without 'from':
// and what does not understand as coming from the connected client. Returns the size
// in 'out', or 0 if it didn't fit.
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
