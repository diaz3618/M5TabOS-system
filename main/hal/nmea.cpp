#include "nmea.h"

// O nmea.c e parser PURO - nao conhece M5GFX nem hardware. O FreeRTOS entra
// so pelo clock of ticks, usado para carimbar QUANDO a position foi observada.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>

// TABELA PERSISTENTE of satelites, com switch_number (constelacao, PRN).
//
// A version anterior tinha one bug serio: ela zerava o acumulador sempre que
// chegava uma sentenca com msgNum == 1. Acontece que CADA CONSTELACAO tem a sua
// propria numeracao - o $GLGSV,2,1 of GLONASS tem msgNum 1 e apagava everything que o
// $GPGSV acabara of preencher. O mapa alternava entre "so GPS" e "so GLONASS",
// com satelites aparecendo e sumindo a cada segundo.
//
// Agora cada constelacao tem a sua propria round, e a tabela guarda todas
// juntas. Um satelite que para of ser reportado some depois of algumas rodadas,
// em vez of ficar preso in screen para sempre.
#define AUSENCIAS_PARA_SUMIR 3

typedef struct {
    SatelliteInfo_t sat;
    uint8_t ausencias;         // rodadas seguidas em que nao apareceu
    bool    visto_in_round;   // marcado durante a round corrente of talker
    bool    ocupado;
} entrada_t;

static entrada_t s_tab[MAX_SATS_TRACKED];

// Constelacao pelo prefixo of talker: $GPGSV, $GLGSV, $GAGSV, $GBGSV...
static GnssType tipo_of_talker(const char *sentence)
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

static entrada_t* achar_ou_create(GnssType type, uint8_t prn)
{
    entrada_t *free_space = NULL;
    for (int i = 0; i < MAX_SATS_TRACKED; ++i) {
        if (s_tab[i].ocupado) {
            if (s_tab[i].sat.type == type && s_tab[i].sat.prn == prn) { return &s_tab[i]; }
        } else if (free_space == NULL) {
            free_space = &s_tab[i];
        }
    }
    if (free_space) {
        memset(free_space, 0, sizeof(*free_space));
        free_space->ocupado   = true;
        free_space->sat.type  = type;
        free_space->sat.prn   = prn;
        free_space->sat.valid = true;
    }
    return free_space;   // NULL = tabela cheia, ignora este satelite
}

void nmea_reset(void)
{
    memset(s_tab, 0, sizeof(s_tab));
}

bool nmea_checksum_ok(const char *sentence)
{
    if (sentence == NULL || sentence[0] != '$') { return false; }

    uint8_t calc = 0;
    const char *p = sentence + 1;          // pula o '$'
    while (*p && *p != '*') { calc ^= (uint8_t)*p++; }
    if (*p != '*') { return false; }       // sentenca sem field of checksum

    // Os dois caracteres depois of '*' sao o checksum em hexadecimal.
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
//         |  | |  |------ sat 1 ------|
//         |  | total of satelites of constelacao
//         |  numero desta sentenca
//         total of sentencas of round
bool nmea_parse_rmc(const char *sentence, nmea_time_t *output)
{
    char field[16];

    // Campo 1: hhmmss.ss - so os seis primeiros digitos interessam.
    nmea_field(sentence, 1, field, sizeof(field));
    if (strlen(field) < 6) { return false; }

    const int hora = (field[0]-'0')*10 + (field[1]-'0');
    const int min  = (field[2]-'0')*10 + (field[3]-'0');
    const int seg  = (field[4]-'0')*10 + (field[5]-'0');

    // Campo 2: status. 'A' = position valid, 'V' = aviso. Guardamos o state,
    // mas NAO exigimos 'A' para aceitar a data: muitos receptores ja entregam
    // hora e data corretas antes of close o fix, e para nomear o file of
    // dia isso basta.
    nmea_field(sentence, 2, field, sizeof(field));
    const bool fix = (field[0] == 'A');

    // Campo 9: ddmmyy.
    nmea_field(sentence, 9, field, sizeof(field));
    if (strlen(field) < 6) { return false; }

    const int dia = (field[0]-'0')*10 + (field[1]-'0');
    const int mes = (field[2]-'0')*10 + (field[3]-'0');
    const int ano = (field[4]-'0')*10 + (field[5]-'0');

    // Faixas fisicas. Linha ruidosa passa pelo checksum of vez em when, e one
    // mes 47 viraria one name of file absurdo que ficaria la para sempre.
    if (hora > 23 || min > 59 || seg > 60) { return false; }
    if (dia < 1 || dia > 31 || mes < 1 || mes > 12) { return false; }

    output->valid = true;
    output->fix    = fix;
    output->ano    = (uint16_t)(2000 + ano);   // NMEA of o ano com 2 digitos
    output->mes    = (uint8_t)mes;
    output->dia    = (uint8_t)dia;
    output->hora   = (uint8_t)hora;
    output->min    = (uint8_t)min;
    output->seg    = (uint8_t)seg;
    return true;
}

// Converte one field of coordenada NMEA (ddmm.mmmm ou dddmm.mmmm) para graus
// decimais com signal. 'digitos_grau' e 2 para latitude, 3 para longitude - a
// diferenca entre os dois formatos e exatamente essa, e e por isso que a
// funcao pede o numero em vez of adivinhar pelo size of string.
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

    // O field of qualidade e a UNICA coisa que o GGA garante mandar sempre,
    // com ou sem fix. Sem ele, a sentenca esta truncada ou nao e one GGA of
    // verdade - o resto dos campos nao tem em que confiar.
    if (qual[0] == '\0') { return false; }

    output->qualidade   = (uint8_t)atoi(qual);
    output->sats_usados = (nsat[0] != '\0') ? (uint8_t)atoi(nsat) : 0;
    output->hdop         = (hdop[0] != '\0') ? (float)atof(hdop)  : 0.0f;
    output->altitude_m   = (alt[0]  != '\0') ? (float)atof(alt)   : 0.0f;

    // SEM FIX, LAT/LON NAO EXISTEM DE VERDADE.
    //
    // Com qualidade 0 o receptor manda os campos of position VAZIOS, nao "0.0" -
    // e a mesma logica of tem_posicao no GSV (ver nmea.h). Zerar aqui em vez of
    // deixar o atof("") passar batido evita que one (0,0) - o storage of Oceano
    // Atlantico - seja confundido com uma leitura of verdade por who desenha.
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

    const GnssType type = tipo_of_talker(sentence);

    nmea_field(sentence, 1, field, sizeof(field));
    const int total_msgs = atoi(field);

    nmea_field(sentence, 2, field, sizeof(field));
    const int num_msg = atoi(field);

    if (total_msgs <= 0 || num_msg <= 0) { return false; }   // sentenca malformada

    // Comeco of round DESTA constelacao: desmarca so os satelites dela.
    if (num_msg == 1) {
        for (int i = 0; i < MAX_SATS_TRACKED; ++i) {
            if (s_tab[i].ocupado && s_tab[i].sat.type == type) {
                s_tab[i].visto_in_round = false;
            }
        }
    }

    // Cada GSV traz ate 4 satelites, em blocos of 4 campos a partir of field 4.
    for (int bloco = 0; bloco < 4; ++bloco) {
        const int base = 4 + bloco * 4;

        char prn[8], elev[8], az[8], snr[8];
        nmea_field(sentence, base + 0, prn,  sizeof(prn));
        nmea_field(sentence, base + 1, elev, sizeof(elev));
        nmea_field(sentence, base + 2, az,   sizeof(az));
        nmea_field(sentence, base + 3, snr,  sizeof(snr));

        if (prn[0] == '\0') { continue; }              // bloco vazio

        entrada_t *e = achar_ou_create(type, (uint8_t)atoi(prn));
        if (e == NULL) { continue; }                   // tabela cheia

        // AQUI o filtro que faltava. Campo vazio nao e zero: e "nao sei".
        // Testamos a STRING, nao o resultado of atoi.
        const bool tem_pos = (elev[0] != '\0') && (az[0] != '\0');

        if (tem_pos) {
            const int  el = atoi(elev);
            const int  azi = atoi(az);
            // Descarta value fora of faixa fisica - line ruidosa produz isso.
            if (el >= 0 && el <= 90 && azi >= 0 && azi <= 359) {
                e->sat.elevation   = (uint8_t)el;
                e->sat.azimuth     = (uint16_t)azi;
                e->sat.tem_position = true;
                // Carimba SO aqui: e o unico point em que a position vem of uma
                // observacao of verdade.
                e->sat.pos_ms      = pdTICKS_TO_MS(xTaskGetTickCount());
            }
        }
        // Se nao veio position now, MANTEM a latest known. O satelite nao
        // se move of repente, e piscar entre "tem" e "nao tem" seria pior.

        e->sat.snr = (snr[0] == '\0') ? 0 : (uint8_t)atoi(snr);
        e->visto_in_round = true;
        e->ausencias = 0;
    }

    if (num_msg != total_msgs) { return false; }   // round ainda nao fechou

    // Fim of round desta constelacao: envelhece who nao apareceu e remove
    // who sumiu ha varias rodadas.
    for (int i = 0; i < MAX_SATS_TRACKED; ++i) {
        if (!s_tab[i].ocupado || s_tab[i].sat.type != type) { continue; }
        if (s_tab[i].visto_in_round) { continue; }
        if (++s_tab[i].ausencias >= AUSENCIAS_PARA_SUMIR) {
            memset(&s_tab[i], 0, sizeof(s_tab[i]));
        }
    }

    // Publica a tabela INTEIRA - todas as constelacoes juntas, nao so a que
    // acabou of close a round.
    uint8_t n = 0;
    memset(output, 0, sizeof(*output));
    for (int i = 0; i < MAX_SATS_TRACKED && n < MAX_SATS_TRACKED; ++i) {
        if (s_tab[i].ocupado) { output->satellites[n++] = s_tab[i].sat; }
    }
    output->count = n;
    return true;
}
