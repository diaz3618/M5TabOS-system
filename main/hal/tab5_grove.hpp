// Discovery that is plugged into the Grove.
//
// The Grove of Tab5 is only one device and fits one device at a time. Today they compete:
// - the GPS module (NMEA, 9600 baud)
// - the M5 Unit C6L running Meshtastic (protobuf framed, 115200)
// and in the future any other. Instead of the user choosing in hand which app can
// open, the system ASKS THE FIO.
//
// They both speak UART, but they report each other from afar:
//
// GPS 9600 ASCII lines starting with '$' (ex: $GPGSV,...)
// Meshtastic 115200 binary frames starting at 0x94 0xC3 + size
//
// The difference in baud is what makes it reliable: in the wrong baud the other
// appears as garbage, which does not match the signature none.
//
// AN IMPORTANT ASYMMETRY: the GPS speaks to itself, without being asked. O
// Meshtastic in PROTO mode stays QUIET until one client request - hence the probe
// it is active (we send one ToRadio{want_config_id} and see if it is an iteration frame).
// Searching just by listening would find the GPS and never the C6L.

#pragma once

typedef enum {
  GROVE_NOTHING = 0,       // no one responded
  GROVE_GPS,            // NMEA module
  GROVE_MESHTASTIC,     // Unit C6L in PROTO mode
  GROVE_UNKNOWN,   // byte arrived, but it doesn't match none signature
} grove_type_t;

const char* grove_name(grove_type_t t);

// -------------------------------------------------------------------------------
// GROVE POSSESSION
// -------------------------------------------------------------------------------
// The Grove has ONE pair of pins and fits ONE device. Today they compete for the service
// Meshtastic (permanent, while the C6L is plugged in) and the Azimutal app
// (while open). They both want the same UART.
//
// Without referee, the second one to arrive had "UART driver already installed" and
// failed - that's what happened on open the Azimutal with the
// Meshtastic in the air. Fixing this within each app would spread the rule;
// here she lives in a lonely place.
//
// Whoever takes it MUST release. The service takes on start and never released; the app takes
// when_open() and released in when_close().
typedef enum { GROVE_OWNER_NONE = 0, GROVE_OWNER_MESH, GROVE_OWNER_GPS } grove_owner_t;

// Returns false if there is already another owner. DO NOT take away from anyone: who arrived
// first stays, and whoever arrived later needs to notify the user.
bool         tab5_grove_acquire(grove_owner_t who);
void         tab5_grove_release(grove_owner_t who);
grove_owner_t tab5_grove_owner(void);
const char*  grove_owner_name(grove_owner_t d);

// Run the poll. Blocks for a few seconds (turns on the rail, waits for the
// start module, tests both baud). OS of task of interface, and OS with the
// Grove free_space - if an app is using the UART, the result is no good.
grove_type_t tab5_grove_detect(void);

// Listens to a specific baud and DUMP the raw bytes (hex + ASCII) to the console.
//
// The count of bytes is not enough to diagnose: the probe found
// "GPS" in a Grove where the C6L was, because 209 bytes of garbage contained one '$'
// followed by two capital letters. Seeing the content dispels any doubts - NMEA and
// readable text, framed protobuf starts at 94 C3, and garbage is neither one nor
// other.
void tab5_grove_dump(int baud, int ms);

// Last result, without polling again.
grove_type_t tab5_grove_type(void);

// Are there ANY modules in the cable? Reading of level of pin of data: cheap or
// enough to run in a loop, as opposed to polling. It doesn't say WHO it is.
// SO is valid with the free_space port - with the UART installed the pin and of driver.
bool tab5_grove_present(void);

// Reset the detected type, for when the cable empties.
void tab5_grove_forget(void);
