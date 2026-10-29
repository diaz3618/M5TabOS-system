#include "nmea.h"

// nmea.c is a PURE parser - it does not know M5GFX or hardware. FreeRTOS enters
// only for the clock of ticks, used to stamp WHEN the position was observed.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>

// PERSISTENT TABLE of satellites, with switch_number (constellation, PRN).
//
// The previous version had a serious bug: it reset the accumulator whenever
// a sentence arrived with msgNum == 1. It turns out that EACH CONSTELLATION has its own
// own numbering - the $GLGSV,2,1 of GLONASS has msgNum 1 and erased everything that the
// $GPGSV had just been filled. The map alternated between "only GPS" and "only GLONASS",
// with satellites appearing and disappearing every second.
//
// Now each constellation has its own round, and the table stores all
// together. A satellite that disappears after a few rounds to be reported,
// instead of being stuck in screen forever.
#define AUSENCIAS_PARA_SUMIR 3

typedef struct {
    SatelliteInfo_t sat;
    uint8_t ausencias;         // consecutive rounds in which he did not appear
    bool    visto_in_round;   // scored during the current round of talker
    bool    occupied;
} satellite_entry_t;

static satellite_entry_t s_tab[MAX_SATS_TRACKED];

// Constellation by the prefix of talker: $GPGSV, $GLGSV, $GAGSV, $GBGSV...
static GnssType constellation_from_talker(const char *sentence)
{
    const char a = sentence[1];
    const char b = sentence[2];
    if (a == 'G' && b == 'P') { return GPS;     }
    if (a == 'G' && b == 'L') { return GLONASS; }
    if (a == 'G' && b == 'A') { return GALILEO; }
    if (a == 'G' && b == 'B') { return BEIDOU;  }
    if (a == 'B' && b == 'D') { return BEIDOU;  }
    if (a == 'G' && b == 'Q') { return QZSS;    }
    if (a == 'Q' && b == 'Z') { return QZSS;    }
    return UNKNOWN;
}

static satellite_entry_t* find_or_add_satellite(GnssType type, uint8_t prn)
{
    satellite_entry_t *free_space = NULL;
    for (int i = 0; i < MAX_SATS_TRACKED; ++i) {
        if (s_tab[i].occupied) {
            if (s_tab[i].sat.type == type && s_tab[i].sat.prn == prn) { return &s_tab[i]; }
        } else if (free_space == NULL) {
            free_space = &s_tab[i];
        }
    }
    if (free_space) {
        memset(free_space, 0, sizeof(*free_space));
        free_space->occupied   = true;
        free_space->sat.type  = type;
        free_space->sat.prn   = prn;
        free_space->sat.valid = true;
    }
    return free_space;   // NULL = table full, ignore this satellite
}

void nmea_reset(void)
{
    memset(s_tab, 0, sizeof(s_tab));
}

bool nmea_checksum_ok(const char *sentence)
{
    if (sentence == NULL || sentence[0] != '$') { return false; }

    uint8_t calc = 0;
    const char *p = sentence + 1;          // skips the '$'
    while (*p && *p != '*') { calc ^= (uint8_t)*p++; }
    if (*p != '*') { return false; }       // sentence without field of checksum

    // The two characters after '*' are the checksum in hexadecimal.
    char hex[3] = { p[1], p[2], 0 };
    if (hex[0] == 0 || hex[1] == 0) { return false; }
    return (uint8_t)strtol(hex, NULL, 16) == calc;
}

void nmea_field(const char *sentence, int indice, char *out, size_t out_size)
{
    out[0] = '\0';
    int current = 0;
    const char *start = sentence;
    const char *p = sentence;

    while (*p) {
        if (*p == ',' || *p == '*') {
            if (current == indice) {
                size_t len = (size_t)(p - start);
                if (len >= out_size) { len = out_size - 1; }
                strncpy(out, start, len);
                out[len] = '\0';
                return;
            }
            current++;
            start = p + 1;
        }
        p++;
    }
}

// $GPGSV,3,1,11,10,63,137,42,07,61,098,45,...*7F
// |  | |  |------ sat 1 ------|
// |  | total of satellites of constelacao
// |  number of this sentence
// total of sentences of round
bool nmea_parse_rmc(const char *sentence, nmea_time_t *output)
{
    char field[16];

    // Field 1: hhmmss.ss - only the first six digits matter.
    nmea_field(sentence, 1, field, sizeof(field));
    if (strlen(field) < 6) { return false; }

    const int hora = (field[0]-'0')*10 + (field[1]-'0');
    const int min  = (field[2]-'0')*10 + (field[3]-'0');
    const int seg  = (field[4]-'0')*10 + (field[5]-'0');

    // Field 2: status. 'A' = position valid, 'V' = warning. We keep the state,
    // but we DO NOT require 'A' to accept the date: many receivers already deliver
    // correct time and date before closing the fix, and to name the file of
    // day that's enough.
    nmea_field(sentence, 2, field, sizeof(field));
    const bool fix = (field[0] == 'A');

    // Field 9: ddmmyy.
    nmea_field(sentence, 9, field, sizeof(field));
    if (strlen(field) < 6) { return false; }

    const int dia = (field[0]-'0')*10 + (field[1]-'0');
    const int mes = (field[2]-'0')*10 + (field[3]-'0');
    const int ano = (field[4]-'0')*10 + (field[5]-'0');

    // Physical tracks. Noisy line goes through the checksum of once in when, and one
    // month 47 would become an absurd name of file that would stay there forever.
    if (hora > 23 || min > 59 || seg > 60) { return false; }
    if (dia < 1 || dia > 31 || mes < 1 || mes > 12) { return false; }

    output->valid = true;
    output->fix    = fix;
    output->ano    = (uint16_t)(2000 + ano);   // NMEA of the year with 2 digits
    output->mes    = (uint8_t)mes;
    output->dia    = (uint8_t)dia;
    output->hora   = (uint8_t)hora;
    output->min    = (uint8_t)min;
    output->seg    = (uint8_t)seg;
    return true;
}

// Converts one field of NMEA coordinate (ddmm.mmmm or dddmm.mmmm) to degrees
// decimals with signal. 'degree_digits' and 2 for latitude, 3 for longitude - a
// difference between the two formats is exactly this, and that is why the
// function asks for the number instead of guessing by the size of string.
static double nmea_coord(const char *field, char hemisferio, int digitos_grau)
{
    if ((int)strlen(field) <= digitos_grau) { return 0.0; }

    char graus_txt[4] = { 0, 0, 0, 0 };
    memcpy(graus_txt, field, (size_t)digitos_grau);
    const double graus   = atof(graus_txt);
    const double minutos = atof(field + digitos_grau);

    double v = graus + minutos / 60.0;
    if (hemisferio == 'S' || hemisferio == 'W') { v = -v; }
    return v;
}

bool nmea_parse_gga(const char *sentence, nmea_position_t *output)
{
    char lat[16], lat_h[4], lon[16], lon_h[4];
    char qual[4], nsat[4], hdop[16], alt[16];

    nmea_field(sentence, 2, lat,  sizeof(lat));
    nmea_field(sentence, 3, lat_h, sizeof(lat_h));
    nmea_field(sentence, 4, lon,  sizeof(lon));
    nmea_field(sentence, 5, lon_h, sizeof(lon_h));
    nmea_field(sentence, 6, qual, sizeof(qual));
    nmea_field(sentence, 7, nsat, sizeof(nsat));
    nmea_field(sentence, 8, hdop, sizeof(hdop));
    nmea_field(sentence, 9, alt,  sizeof(alt));

    // The field of quality is the ONLY thing that GGA guarantees to always deliver,
    // with or without fix. Without it, the sentence is truncated or not one GGA of
    // true - the rest of the fields have nothing to trust.
    if (qual[0] == '\0') { return false; }

    output->qualidade   = (uint8_t)atoi(qual);
    output->sats_usados = (nsat[0] != '\0') ? (uint8_t)atoi(nsat) : 0;
    output->hdop         = (hdop[0] != '\0') ? (float)atof(hdop)  : 0.0f;
    output->altitude_m   = (alt[0]  != '\0') ? (float)atof(alt)   : 0.0f;

    // NO FIX, LAT/LON DOESN'T REALLY EXIST.
    //
    // With quality 0, the receiver sends the fields of position EMPTY, not "0.0" -
    // and the same logic of tem_posicao in GSV (see nmea.h). Zero here instead of
    // leaving atof("") overlooked prevents one (0,0) - the storage of Oceano
    // Atlantico - be confused with a reading of truth by whoever draws.
    if (output->qualidade > 0 && lat[0] && lon[0]) {
        output->lat = nmea_coord(lat, lat_h[0], 2);
        output->lon = nmea_coord(lon, lon_h[0], 3);
    } else {
        output->lat = 0.0;
        output->lon = 0.0;
    }

    output->valid = true;
    return true;
}

bool nmea_parse_gsv(const char *sentence, SatelliteSnapshot_t *output)
{
    char field[16];

const GnssType type = constellation_from_talker(sentence);

    nmea_field(sentence, 1, field, sizeof(field));
    const int total_msgs = atoi(field);

    nmea_field(sentence, 2, field, sizeof(field));
    const int num_msg = atoi(field);

    if (total_msgs <= 0 || num_msg <= 0) { return false; }   // malformed sentence

    // Start of round THIS constellation: deselect only its satellites.
    if (num_msg == 1) {
        for (int i = 0; i < MAX_SATS_TRACKED; ++i) {
            if (s_tab[i].occupied && s_tab[i].sat.type == type) {
                s_tab[i].visto_in_round = false;
            }
        }
    }

    // Each GSV brings up to 4 satellites, in blocks of 4 fields starting from field 4.
    for (int block = 0; block < 4; ++block) {
        const int base = 4 + block * 4;

        char prn[8], elev[8], az[8], snr[8];
        nmea_field(sentence, base + 0, prn,  sizeof(prn));
        nmea_field(sentence, base + 1, elev, sizeof(elev));
        nmea_field(sentence, base + 2, az,   sizeof(az));
        nmea_field(sentence, base + 3, snr,  sizeof(snr));

        if (prn[0] == '\0') { continue; }              // empty block

        satellite_entry_t *e = find_or_add_satellite(type, (uint8_t)atoi(prn));
        if (e == NULL) { continue; }                   // table full

        // HERE is the missing filter. Empty field is not zero: it is "I don't know".
        // We test the STRING, not the result of atoi.
        const bool has_position = (elev[0] != '\0') && (az[0] != '\0');

        if (has_position) {
            const int  el = atoi(elev);
            const int  azi = atoi(az);
            // Discards value outside of physical range - noisy line produces this.
            if (el >= 0 && el <= 90 && azi >= 0 && azi <= 359) {
                e->sat.elevation   = (uint8_t)el;
                e->sat.azimuth     = (uint16_t)azi;
                e->sat.tem_position = true;
                // Stamp SO here: it is the only point at which the position comes from a
                // observation of truth.
                e->sat.pos_ms      = pdTICKS_TO_MS(xTaskGetTickCount());
            }
        }
        // If position now didn't come, KEEP the latest known. The satellite does not
        // moves suddenly, and flashing between "has" and "doesn't have" would be worse.

        e->sat.snr = (snr[0] == '\0') ? 0 : (uint8_t)atoi(snr);
        e->visto_in_round = true;
        e->ausencias = 0;
    }

    if (num_msg != total_msgs) { return false; }   // round hasn't closed yet

    // End of round of this constellation: age who did not appear and remove
    // who has been missing for several rounds.
    for (int i = 0; i < MAX_SATS_TRACKED; ++i) {
        if (!s_tab[i].occupied || s_tab[i].sat.type != type) { continue; }
        if (s_tab[i].visto_in_round) { continue; }
        if (++s_tab[i].ausencias >= AUSENCIAS_PARA_SUMIR) {
            memset(&s_tab[i], 0, sizeof(s_tab[i]));
        }
    }

    // Publishes the ENTIRE table - all constellations together, not just the one that
    // finished close a round.
    uint8_t n = 0;
    memset(output, 0, sizeof(*output));
    for (int i = 0; i < MAX_SATS_TRACKED && n < MAX_SATS_TRACKED; ++i) {
        if (s_tab[i].occupied) { output->satellites[n++] = s_tab[i].sat; }
    }
    output->count = n;
    return true;
}
