#include "nmea.h"

// O nmea.c e parser PURO - nao conhece M5GFX nem hardware. O FreeRTOS entra
// so pelo clock de ticks, usado para carimbar QUANDO a position foi observada.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>

// TABELA PERSISTENTE de satelites, com switch_number (constelacao, PRN).
//
// A version anterior tinha um bug serio: ela zerava o acumulador sempre que
// chegava uma sentenca com msgNum == 1. Acontece que CADA CONSTELACAO tem a sua
// propria numeracao - o $GLGSV,2,1 do GLONASS tem msgNum 1 e apagava tudo que o
// $GPGSV acabara de preencher. O mapa alternava entre "so GPS" e "so GLONASS",
// com satelites aparecendo e sumindo a cada segundo.
//
// Agora cada constelacao tem a sua propria rodada, e a tabela guarda todas
// juntas. Um satelite que para de ser reportado some depois de algumas rodadas,
// em vez de ficar preso na tela para sempre.
#define AUSENCIAS_PARA_SUMIR 3

typedef struct {
    SatelliteInfo_t sat;
    uint8_t ausencias;         // rodadas seguidas em que nao apareceu
    bool    visto_na_rodada;   // marcado durante a rodada corrente do talker
    bool    ocupado;
} entrada_t;

static entrada_t s_tab[MAX_SATS_TRACKED];

// Constelacao pelo prefixo do talker: $GPGSV, $GLGSV, $GAGSV, $GBGSV...
static GnssType tipo_do_talker(const char *sentence)
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

static entrada_t* achar_ou_criar(GnssType type, uint8_t prn)
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
    if (*p != '*') { return false; }       // sentenca sem campo de checksum

    // Os dois caracteres depois do '*' sao o checksum em hexadecimal.
    char hex[3] = { p[1], p[2], 0 };
    if (hex[0] == 0 || hex[1] == 0) { return false; }
    return (uint8_t)strtol(hex, NULL, 16) == calc;
}

void nmea_field(const char *sentence, int indice, char *out, size_t out_tam)
{
    out[0] = '\0';
    int atual = 0;
    const char *inicio = sentence;
    const char *p = sentence;

    while (*p) {
        if (*p == ',' || *p == '*') {
            if (atual == indice) {
                size_t len = (size_t)(p - inicio);
                if (len >= out_tam) { len = out_tam - 1; }
                strncpy(out, inicio, len);
                out[len] = '\0';
                return;
            }
            atual++;
            inicio = p + 1;
        }
        p++;
    }
}

// $GPGSV,3,1,11,10,63,137,42,07,61,098,45,...*7F
//         |  | |  |------ sat 1 ------|
//         |  | total de satelites da constelacao
//         |  numero desta sentenca
//         total de sentencas da rodada
bool nmea_parse_rmc(const char *sentence, nmea_time_t *output)
{
    char campo[16];

    // Campo 1: hhmmss.ss - so os seis primeiros digitos interessam.
    nmea_field(sentence, 1, campo, sizeof(campo));
    if (strlen(campo) < 6) { return false; }

    const int hora = (campo[0]-'0')*10 + (campo[1]-'0');
    const int min  = (campo[2]-'0')*10 + (campo[3]-'0');
    const int seg  = (campo[4]-'0')*10 + (campo[5]-'0');

    // Campo 2: status. 'A' = position valid, 'V' = aviso. Guardamos o state,
    // mas NAO exigimos 'A' para aceitar a data: muitos receptores ja entregam
    // hora e data corretas antes de fechar o fix, e para nomear o file do
    // dia isso basta.
    nmea_field(sentence, 2, campo, sizeof(campo));
    const bool fix = (campo[0] == 'A');

    // Campo 9: ddmmyy.
    nmea_field(sentence, 9, campo, sizeof(campo));
    if (strlen(campo) < 6) { return false; }

    const int dia = (campo[0]-'0')*10 + (campo[1]-'0');
    const int mes = (campo[2]-'0')*10 + (campo[3]-'0');
    const int ano = (campo[4]-'0')*10 + (campo[5]-'0');

    // Faixas fisicas. Linha ruidosa passa pelo checksum de vez em quando, e um
    // mes 47 viraria um name de file absurdo que ficaria la para sempre.
    if (hora > 23 || min > 59 || seg > 60) { return false; }
    if (dia < 1 || dia > 31 || mes < 1 || mes > 12) { return false; }

    output->valid = true;
    output->fix    = fix;
    output->ano    = (uint16_t)(2000 + ano);   // NMEA da o ano com 2 digitos
    output->mes    = (uint8_t)mes;
    output->dia    = (uint8_t)dia;
    output->hora   = (uint8_t)hora;
    output->min    = (uint8_t)min;
    output->seg    = (uint8_t)seg;
    return true;
}

// Converte um campo de coordenada NMEA (ddmm.mmmm ou dddmm.mmmm) para graus
// decimais com signal. 'digitos_grau' e 2 para latitude, 3 para longitude - a
// diferenca entre os dois formatos e exatamente essa, e e por isso que a
// funcao pede o numero em vez de adivinhar pelo size da string.
static double nmea_coord(const char *campo, char hemisferio, int digitos_grau)
{
    if ((int)strlen(campo) <= digitos_grau) { return 0.0; }

    char graus_txt[4] = { 0, 0, 0, 0 };
    memcpy(graus_txt, campo, (size_t)digitos_grau);
    const double graus   = atof(graus_txt);
    const double minutos = atof(campo + digitos_grau);

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

    // O campo de qualidade e a UNICA coisa que o GGA garante mandar sempre,
    // com ou sem fix. Sem ele, a sentenca esta truncada ou nao e um GGA de
    // verdade - o resto dos campos nao tem em que confiar.
    if (qual[0] == '\0') { return false; }

    output->qualidade   = (uint8_t)atoi(qual);
    output->sats_usados = (nsat[0] != '\0') ? (uint8_t)atoi(nsat) : 0;
    output->hdop         = (hdop[0] != '\0') ? (float)atof(hdop)  : 0.0f;
    output->altitude_m   = (alt[0]  != '\0') ? (float)atof(alt)   : 0.0f;

    // SEM FIX, LAT/LON NAO EXISTEM DE VERDADE.
    //
    // Com qualidade 0 o receptor manda os campos de position VAZIOS, nao "0.0" -
    // e a mesma logica do tem_posicao no GSV (ver nmea.h). Zerar aqui em vez de
    // deixar o atof("") passar batido evita que um (0,0) - o storage do Oceano
    // Atlantico - seja confundido com uma leitura de verdade por who desenha.
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
    char campo[16];

    const GnssType type = tipo_do_talker(sentence);

    nmea_field(sentence, 1, campo, sizeof(campo));
    const int total_msgs = atoi(campo);

    nmea_field(sentence, 2, campo, sizeof(campo));
    const int num_msg = atoi(campo);

    if (total_msgs <= 0 || num_msg <= 0) { return false; }   // sentenca malformada

    // Comeco da rodada DESTA constelacao: desmarca so os satelites dela.
    if (num_msg == 1) {
        for (int i = 0; i < MAX_SATS_TRACKED; ++i) {
            if (s_tab[i].ocupado && s_tab[i].sat.type == type) {
                s_tab[i].visto_na_rodada = false;
            }
        }
    }

    // Cada GSV traz ate 4 satelites, em blocos de 4 campos a partir do campo 4.
    for (int bloco = 0; bloco < 4; ++bloco) {
        const int base = 4 + bloco * 4;

        char prn[8], elev[8], az[8], snr[8];
        nmea_field(sentence, base + 0, prn,  sizeof(prn));
        nmea_field(sentence, base + 1, elev, sizeof(elev));
        nmea_field(sentence, base + 2, az,   sizeof(az));
        nmea_field(sentence, base + 3, snr,  sizeof(snr));

        if (prn[0] == '\0') { continue; }              // bloco vazio

        entrada_t *e = achar_ou_criar(type, (uint8_t)atoi(prn));
        if (e == NULL) { continue; }                   // tabela cheia

        // AQUI o filtro que faltava. Campo vazio nao e zero: e "nao sei".
        // Testamos a STRING, nao o resultado do atoi.
        const bool tem_pos = (elev[0] != '\0') && (az[0] != '\0');

        if (tem_pos) {
            const int  el = atoi(elev);
            const int  azi = atoi(az);
            // Descarta value fora da faixa fisica - linha ruidosa produz isso.
            if (el >= 0 && el <= 90 && azi >= 0 && azi <= 359) {
                e->sat.elevation   = (uint8_t)el;
                e->sat.azimuth     = (uint16_t)azi;
                e->sat.tem_posicao = true;
                // Carimba SO aqui: e o unico point em que a position vem de uma
                // observacao de verdade.
                e->sat.pos_ms      = pdTICKS_TO_MS(xTaskGetTickCount());
            }
        }
        // Se nao veio position agora, MANTEM a latest known. O satelite nao
        // se move de repente, e piscar entre "tem" e "nao tem" seria pior.

        e->sat.snr = (snr[0] == '\0') ? 0 : (uint8_t)atoi(snr);
        e->visto_na_rodada = true;
        e->ausencias = 0;
    }

    if (num_msg != total_msgs) { return false; }   // rodada ainda nao fechou

    // Fim da rodada desta constelacao: envelhece who nao apareceu e remove
    // who sumiu ha varias rodadas.
    for (int i = 0; i < MAX_SATS_TRACKED; ++i) {
        if (!s_tab[i].ocupado || s_tab[i].sat.type != type) { continue; }
        if (s_tab[i].visto_na_rodada) { continue; }
        if (++s_tab[i].ausencias >= AUSENCIAS_PARA_SUMIR) {
            memset(&s_tab[i], 0, sizeof(s_tab[i]));
        }
    }

    // Publica a tabela INTEIRA - todas as constelacoes juntas, nao so a que
    // acabou de fechar a rodada.
    uint8_t n = 0;
    memset(output, 0, sizeof(*output));
    for (int i = 0; i < MAX_SATS_TRACKED && n < MAX_SATS_TRACKED; ++i) {
        if (s_tab[i].ocupado) { output->satellites[n++] = s_tab[i].sat; }
    }
    output->count = n;
    return true;
}
