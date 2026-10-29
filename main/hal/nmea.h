// Parse of NMEA - PURE LOGIC.
//
// By design, this file and nmea.cpp do not include M5GFX, M5Unified or
// nothing of screen: just string and struct. This makes the parser testable outside of
// board, reusable in another project (Meshtastic will want something
// similar) and makes it obvious, looking at the folder, what is a rule of business and what is
// interface.

#pragma once

#include <stdint.h>
#include <stddef.h>

#define NMEA_MAX_LENGTH   128
// With several constellations on the same team, 32 presses. GPS(12) + GLONASS(10) +
// Galileo(10) + BeiDou visible pass this easily.
#define MAX_SATS_TRACKED  48

typedef enum {
    GPS, GLONASS, GALILEO, BEIDOU, QZSS, SBAS, UNKNOWN
} GnssType;

// A raw and complete NMEA sentence, with a validated checksum.
typedef struct {
    char sentence[NMEA_MAX_LENGTH];
} NMEARawSentence_t;

// Data from ONE satellite, already parsed - and this is what skyplot consumes.
typedef struct {
    GnssType type;
    uint8_t  prn;         // ID of satellite (1-32 GPS, etc)
    uint8_t  elevation;   // 0-90 degrees (0 = horizon, 90 = zenith)
    uint16_t azimuth;     // 0-359 degrees (0 = North, 90 = East)
    uint8_t  snr;         // 0-99 dB (0 = not captured yet)

    // The receiver KNOWS that this satellite exists, but does not yet have a position to
    // he: in NMEA this comes as EMPTY elevation/azimuth fields. How
    // atoi("") returns 0, without this flag they would all fall at the same point of
    // graph (elevation 0 = horizon, azimuth 0 = north) and would be stacked.
    // Whoever draws must skip those with tem_posicao == false.
    bool     tem_position;

    // WHEN the position above was actually updated (ms of uptime).
    //
    // It exists because "tem_posicao" does not distinguish new observation from old observation.
    // The parser maintains the latest known position when the GSV arrives without
    // elevation/azimuth - what is RIGHT for the screen (prevents the satellite from blinking
    // between existing and disappearing) and WRONG for the record in file.
    //
    // Without this, the app recorded 2047 lines over 2h45m which
    // collapsed into 23 positions: the receiver stopped sending position, the parser
    // held the latest, and the recorder wrote the same point every minute as if
    // was a new observation. Whoever RECORDS must look at this age; who DRAWS
    // can ignore.
    uint32_t pos_ms;

    bool     valid;
} SatelliteInfo_t;

// Complete snapshot of visible constellation.
typedef struct {
    SatelliteInfo_t satellites[MAX_SATS_TRACKED];
    uint8_t count;
} SatelliteSnapshot_t;

// UTC date and time coming from GPS itself.
//
// Why this matters: the time of system and the uptime, which iteration to zero every
// restart. To name one file per day and to plot orbits between sessions,
// only the satellite time is used - it is the same for everyone, always.
typedef struct {
    bool     valid;    // date and time are known
    bool     fix;       // receiver declared position valid (field A of RMC)
    uint16_t ano;       // 2026
    uint8_t  mes, dia;
    uint8_t  hora, min, seg;
} nmea_time_t;

// Parse an RMC sentence. Returns true if you extracted date AND time.
//
// $GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A
// |     |                                    |
// |     status: A=valid V=ddmmyy warning
// hhmmss.ss UTC
bool nmea_parse_rmc(const char *sentence, nmea_time_t *output);

// Position of RECEIVER - the difference between "where the satellites are" (GSV, already
// existed) and "where I am" (GGA, and what was missing).
//
// GGA is the canonical sentence of position: it is the only one that brings ALTITUDE with it
// with latitude/longitude, in addition to HDOP (how good is the geometry of
// used satellites) and QUALITY of fix (simple GPS, DGPS, RTK...) - data
// which neither RMC nor GSV deliver.
typedef struct {
    bool     valid;        // the sentence arrived and had the expected fields

    // DECIMAL DEGREES, with the sign of hemisphere applied: negative and
    // South/West, positive and North/East. And the format that anything that
    // map plot (including Google Maps itself in search bar) accepts
    // directly, without the observer having to count the degree/minute in head.
    double   lat;
    double   lon;

    float    altitude_m;    // above sea level (reference WGS84/geoid)
    float    hdop;          // dilution of horizontal precision - smaller is better

    // 0=no fix 1=GPS 2=DGPS 4=RTK fixed 5=RTK floating 6=estimated(dead reckoning)
    // NO FIX (0), lat/lon is ZERATED of purpose - see nmea_parse_gga().
    uint8_t  qualidade;

    uint8_t  sats_usados;   // how many ENTERED THE ACCOUNT of fix (!= "seen_devices" of GSV)
} nmea_position_t;

// $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47
// |lat |  |lon |  |  |  |  |altitude
// |       |  |        |  |  |  |  M = meters
// |       |  |        |  |  |  HDOP
// |       |  |        |  |  sats used in the fix
// |       |  |        |  quality (0=no fix)
// |       N/S|        E/W
// Returns true when the sentence had the field of quality (it always has,
// even without fix - which is why this parser does not require fix for return_card true).
bool nmea_parse_gga(const char *sentence, nmea_position_t *output);

// NMEA Checksum: XOR of everything between '$' and '*'.
bool nmea_checksum_ok(const char *sentence);

// Extracts the Nth field separated by a comma. Returns empty string if the field
// does not exist - common in NMEA, fields are frequently missing.
void nmea_field(const char *sentence, int indice, char *out, size_t out_size);

// Accumulates a GSV sentence. GSV arrive in several messages per round, so
// the parser stores internal state until round close.
// Returns true when the snapshot became COMPLETE - and only then is it worth drawing.
bool nmea_parse_gsv(const char *sentence, SatelliteSnapshot_t *output);

// Reset the accumulator. Call on open the app, so as not to inherit state from a
// previous session.
void nmea_reset(void);
