// Descoberta do que esta plugado no Grove.
//
// O Grove do Tab5 e um so e cabe um dispositivo de cada vez. Hoje disputam:
//   - o modulo GPS (NMEA, 9600 baud)
//   - o M5 Unit C6L rodando Meshtastic (protobuf enquadrado, 115200)
// e no futuro qualquer outro. Em vez de o usuario escolher na mao qual app pode
// abrir, o sistema PERGUNTA AO FIO.
//
// Os dois falam UART, mas se denunciam de longe:
//
//   GPS         9600    linhas ASCII comecando em '$' (ex: $GPGSV,...)
//   Meshtastic  115200  frames binarios comecando em 0x94 0xC3 + size
//
// A diferenca de baud e o que torna isto confiavel: no baud errado o outro
// aparece como lixo, que nao casa com assinatura nenhuma.
//
// UMA ASSIMETRIA IMPORTANTE: o GPS fala sozinho, sem ser perguntado. O
// Meshtastic em mode PROTO fica CALADO ate um cliente pedir - por isso a sonda
// dele e ativa (mandamos um ToRadio{want_config_id} e vemos se volta frame).
// Procurar so escutando encontraria o GPS e nunca o C6L.

#pragma once

typedef enum {
  GROVE_NOTHING = 0,       // ninguem respondeu
  GROVE_GPS,            // modulo NMEA
  GROVE_MESHTASTIC,     // Unit C6L em mode PROTO
  GROVE_UNKNOWN,   // chegou byte, mas nao bate com nenhuma assinatura
} grove_type_t;

const char* grove_name(grove_type_t t);

// ---------------------------------------------------------------------------
// POSSE DO GROVE
// ---------------------------------------------------------------------------
// O Grove tem UM par de pins e cabe UM dispositivo. Hoje disputam o service
// Meshtastic (permanente, enquanto o C6L estiver plugado) e o app Azimutal
// (enquanto estiver aberto). Os dois querem a mesma UART.
//
// Sem arbitro, o segundo a chegar levava "UART driver already installed" e
// falhava - foi o que aconteceu ao abrir o Azimutal com o
// Meshtastic no ar. Consertar isso dentro de cada app espalharia a regra;
// aqui ela mora num lugar so.
//
// Quem toma DEVE release. O service toma no inicio e nunca solta; o app toma no
// ao_abrir() e solta no ao_fechar().
typedef enum { GROVE_OWNER_NONE = 0, GROVE_OWNER_MESH, GROVE_OWNER_GPS } grove_owner_t;

// Devolve false se ja houver outro owner. NAO tira de ninguem: who chegou
// primeiro fica, e who chegou depois precisa avisar o usuario.
bool         tab5_grove_acquire(grove_owner_t who);
void         tab5_grove_release(grove_owner_t who);
grove_owner_t tab5_grove_owner(void);
const char*  grove_owner_name(grove_owner_t d);

// Roda a sondagem. Bloqueia por alguns segundos (liga o trilho, espera o
// modulo start, testa os dois baud). SO da tarefa de interface, e SO com o
// Grove free_space - se um app estiver usando a UART, o resultado nao presta.
grove_type_t tab5_grove_detect(void);

// Escuta um baud especifico e DESPEJA os bytes crus (hex + ASCII) no console.
//
// A contagem de bytes nao basta para diagnosticar: a sonda achou
// "GPS" num Grove onde estava o C6L, porque 209 bytes de lixo continham um '$'
// seguido de duas maiusculas. Ver o conteudo desfaz qualquer duvida - NMEA e
// text legivel, protobuf enquadrado comeca em 94 C3, e lixo nao e nem um nem
// outro.
void tab5_grove_dump(int baud, int ms);

// Ultimo resultado, sem sondar de novo.
grove_type_t tab5_grove_type(void);

// Ha ALGUM modulo no cabo? Leitura de nivel do pino de data: barata o
// bastante para rodar em loop, ao contrario da sondagem. Nao diz QUEM e.
// SO vale com a porta free_space - com a UART instalada o pino e do driver.
bool tab5_grove_present(void);

// Zera o type detectado, para quando o cabo esvazia.
void tab5_grove_forget(void);
