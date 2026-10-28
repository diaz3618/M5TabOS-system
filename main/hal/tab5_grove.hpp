// Descoberta of que esta plugado no Grove.
//
// O Grove of Tab5 e one so e cabe one dispositivo of cada vez. Hoje disputam:
//   - o modulo GPS (NMEA, 9600 baud)
//   - o M5 Unit C6L running Meshtastic (protobuf enquadrado, 115200)
// e no futuro qualquer outro. Em vez of o user escolher in mao qual app pode
// open, o sistema PERGUNTA AO FIO.
//
// Os dois falam UART, mas se denunciam of longe:
//
//   GPS         9600    lines ASCII comecando em '$' (ex: $GPGSV,...)
//   Meshtastic  115200  frames binarios comecando em 0x94 0xC3 + size
//
// A diferenca of baud e o que torna isto confiavel: no baud errado o outro
// aparece como lixo, que nao casa com assinatura none.
//
// UMA ASSIMETRIA IMPORTANTE: o GPS fala sozinho, sem ser perguntado. O
// Meshtastic em mode PROTO stays CALADO ate one cliente request - por isso a sonda
// dele e active (mandamos one ToRadio{want_config_id} e vemos se iteration frame).
// Procurar so escutando encontraria o GPS e nunca o C6L.

#pragma once

typedef enum {
  GROVE_NOTHING = 0,       // ninguem respondeu
  GROVE_GPS,            // modulo NMEA
  GROVE_MESHTASTIC,     // Unit C6L em mode PROTO
  GROVE_UNKNOWN,   // chegou byte, mas nao bate com none assinatura
} grove_type_t;

const char* grove_name(grove_type_t t);

// ---------------------------------------------------------------------------
// POSSE DO GROVE
// ---------------------------------------------------------------------------
// O Grove tem UM par of pins e cabe UM dispositivo. Hoje disputam o service
// Meshtastic (permanente, enquanto o C6L estiver plugado) e o app Azimutal
// (enquanto estiver open). Os dois querem a mesma UART.
//
// Sem arbitro, o segundo a chegar levava "UART driver already installed" e
// falhava - foi o que aconteceu on open o Azimutal com o
// Meshtastic no ar. Consertar isso dentro of cada app espalharia a regra;
// aqui ela mora num lugar so.
//
// Quem toma DEVE release. O service toma no start e nunca released; o app toma no
// ao_abrir() e released no ao_fechar().
typedef enum { GROVE_OWNER_NONE = 0, GROVE_OWNER_MESH, GROVE_OWNER_GPS } grove_owner_t;

// Devolve false se ja houver outro owner. NAO tira of ninguem: who chegou
// first stays, e who chegou depois precisa notify o user.
bool         tab5_grove_acquire(grove_owner_t who);
void         tab5_grove_release(grove_owner_t who);
grove_owner_t tab5_grove_owner(void);
const char*  grove_owner_name(grove_owner_t d);

// Roda a sondagem. Bloqueia por alguns segundos (liga o trilho, espera o
// modulo start, testa os dois baud). SO of tarefa of interface, e SO com o
// Grove free_space - se one app estiver usando a UART, o resultado nao presta.
grove_type_t tab5_grove_detect(void);

// Escuta one baud especifico e DESPEJA os bytes crus (hex + ASCII) no console.
//
// A contagem of bytes nao basta para diagnosticar: a sonda achou
// "GPS" num Grove onde estava o C6L, porque 209 bytes of lixo continham one '$'
// seguido of duas maiusculas. Ver o content desfaz qualquer duvida - NMEA e
// text legivel, protobuf enquadrado comeca em 94 C3, e lixo nao e nem one nem
// outro.
void tab5_grove_dump(int baud, int ms);

// Ultimo resultado, sem sondar of novo.
grove_type_t tab5_grove_type(void);

// Ha ALGUM modulo no cabo? Leitura of level of pino of data: barata o
// bastante para rodar em loop, on contrario of sondagem. Nao diz QUEM e.
// SO vale com a porta free_space - com a UART instalada o pino e of driver.
bool tab5_grove_present(void);

// Zera o type detectado, para when o cabo esvazia.
void tab5_grove_forget(void);
