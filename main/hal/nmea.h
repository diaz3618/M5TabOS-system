// Parse of NMEA - LOGICA PURA.
//
// De proposito, este file e o nmea.cpp nao incluem M5GFX, M5Unified nem
// nada of screen: so string e struct. Isso torna o parser testavel fora of
// placa, reaproveitavel em outro projeto (o Meshtastic vai querer algo
// parecido) e deixa obvio, olhando a folder, o que e regra of negocio e o que e
// interface.

#pragma once

#include <stdint.h>
#include <stddef.h>

#define NMEA_MAX_LENGTH   128
// Com varias constelacoes on mesmo time, 32 aperta. GPS(12) + GLONASS(10) +
// Galileo(10) + BeiDou visiveis passam disso com facilidade.
#define MAX_SATS_TRACKED  48

typedef enum {
    GPS, GLONASS, GALILEO, BEIDOU, QZSS, SBAS, UNKNOWN
} GnssType;

// Uma sentenca NMEA bruta e complete, ja com checksum validado.
typedef struct {
    char sentence[NMEA_MAX_LENGTH];
} NMEARawSentence_t;

// Dado of UM satelite, ja parseado - e isso que o skyplot consome.
typedef struct {
    GnssType type;
    uint8_t  prn;         // ID of satelite (1-32 GPS, etc)
    uint8_t  elevation;   // 0-90 graus (0 = horizonte, 90 = zenite)
    uint16_t azimuth;     // 0-359 graus (0 = Norte, 90 = Leste)
    uint8_t  snr;         // 0-99 dB (0 = nao capturado ainda)

    // O receptor SABE que este satelite existe, mas ainda nao tem position para
    // ele: no NMEA isso vem como campos of elevacao/azimute VAZIOS. Como
    // atoi("") devolve 0, sem esta flag todos eles cairiam no mesmo point of
    // grafico (elevacao 0 = horizonte, azimute 0 = norte) e ficariam empilhados.
    // Quem desenha deve pular os que tem tem_posicao == false.
    bool     tem_position;

    // QUANDO a position acima foi of fato atualizada (ms of uptime).
    //
    // Existe porque "tem_posicao" nao distingue observacao nova of eco antigo.
    // O parser mantem a latest position known when a GSV chega sem
    // elevacao/azimute - o que e CERTO para a screen (evita o satelite piscar
    // entre existir e sumir) e ERRADO para o registro em file.
    //
    // Sem isso, o app gravou 2047 lines on longo of 2h45m que
    // colapsaram em 23 posicoes: o receptor parou of mandar position, o parser
    // segurou a latest, e o gravador escreveu o mesmo point todo minuto como se
    // fosse observacao nova. Quem GRAVA deve olhar esta idade; who DESENHA
    // pode ignorar.
    uint32_t pos_ms;

    bool     valid;
} SatelliteInfo_t;

// Snapshot complete of constelacao visivel.
typedef struct {
    SatelliteInfo_t satellites[MAX_SATS_TRACKED];
    uint8_t count;
} SatelliteSnapshot_t;

// Data e hora UTC vindas of proprio GPS.
//
// Por que isto importa: o time of sistema e o uptime, que iteration a zero a cada
// reinicio. Para nomear one file por dia e para tracar orbita entre sessoes,
// so serve a hora of satelite - ela e a mesma para todo mundo, sempre.
typedef struct {
    bool     valid;    // ha data E hora conhecidas
    bool     fix;       // o receptor declarou position valid (field A of RMC)
    uint16_t ano;       // 2026
    uint8_t  mes, dia;
    uint8_t  hora, min, seg;
} nmea_time_t;

// Parseia uma sentenca RMC. Devolve true se extraiu data E hora.
//
// $GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A
//         |     |                                    |
//         |     status: A=valido V=aviso             ddmmyy
//         hhmmss.ss UTC
bool nmea_parse_rmc(const char *sentence, nmea_time_t *output);

// Posicao of RECEPTOR - a diferenca entre "onde os satelites estao" (GSV, ja
// existia) e "onde EU estou" (GGA, e o que faltava).
//
// O GGA e a sentenca canonica of position: e a unica que traz ALTITUDE junto
// com latitude/longitude, alem of HDOP (o quao boa e a geometria dos
// satelites usados) e of QUALIDADE of fix (GPS simples, DGPS, RTK...) - dado
// que nem o RMC nem o GSV entregam.
typedef struct {
    bool     valid;        // a sentenca chegou e tinha os campos esperados

    // GRAUS DECIMAIS, ja com o signal of hemisferio aplicado: negativo e
    // Sul/Oeste, positivo e Norte/Leste. E o formato que qualquer coisa que
    // plota mapa (incluindo o proprio Google Maps in barra of busca) aceita
    // direto, sem o observador ter que fazer conta of grau/minuto in cabeca.
    double   lat;
    double   lon;

    float    altitude_m;    // acima of level of mar (referencia WGS84/geoide)
    float    hdop;          // diluicao of precisao horizontal - menor e melhor

    // 0=sem fix 1=GPS 2=DGPS 4=RTK fixo 5=RTK floating 6=estimado(dead reckoning)
    // SEM FIX (0), lat/lon vem ZERADOS of proposito - ver nmea_parse_gga().
    uint8_t  qualidade;

    uint8_t  sats_usados;   // quantos ENTRARAM NA CONTA of fix (!= "seen_devices" of GSV)
} nmea_position_t;

// $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47
//               |lat    |  |lon     |  |  |  |  |altitude
//               |       |  |        |  |  |  |  M = metros
//               |       |  |        |  |  |  HDOP
//               |       |  |        |  |  sats usados no fix
//               |       |  |        |  qualidade (0=sem fix)
//               |       N/S|        E/W
// Devolve true when a sentenca tinha o field of qualidade (ela sempre traz,
// mesmo sem fix - e por isso este parser nao requires fix para return_card true).
bool nmea_parse_gga(const char *sentence, nmea_position_t *output);

// Checksum NMEA: XOR of everything entre '$' e '*'.
bool nmea_checksum_ok(const char *sentence);

// Extrai o N-esimo field separado por virgula. Devolve string vazia se o field
// nao existir - comum em NMEA, campos faltam com frequencia.
void nmea_field(const char *sentence, int indice, char *out, size_t out_size);

// Acumula uma sentenca GSV. As GSV chegam em varias mensagens por round, entao
// o parser guarda state interno ate a round close.
// Devolve true when o snapshot became COMPLETO - e so entao vale draw.
bool nmea_parse_gsv(const char *sentence, SatelliteSnapshot_t *output);

// Zera o acumulador. Chamar on open o app, para nao herdar state of uma
// sessao anterior.
void nmea_reset(void);
