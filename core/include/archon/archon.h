/*
 * Archon controller C API — used by the iOS (Swift) and Android (JNI) apps.
 *
 * Threading: callbacks run on an internal network thread. Every pointer inside
 * an arc_event is valid only during the callback: copy what you need. Never
 * call arc_client_destroy() from inside a callback.
 */
#ifndef ARCHON_H
#define ARCHON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARC_DEFAULT_PORT 47321
#define ARC_MAX_VOLUME 10000
#define ARC_SERVICE_TYPE "_archon._tcp"

typedef struct arc_client arc_client;

typedef enum {
    ARC_STATUS_IDLE = 0,
    ARC_STATUS_CONNECTING = 1,
    ARC_STATUS_AWAITING_CODE = 2,
    ARC_STATUS_VERIFYING = 3,
    ARC_STATUS_CONNECTED = 4,
    ARC_STATUS_RECONNECTING = 5,
    ARC_STATUS_FAILED = 6
} arc_status;

typedef enum {
    ARC_ERR_NONE = 0,
    ARC_ERR_PROTOCOL = 1,
    ARC_ERR_UNSUPPORTED_VERSION = 2,
    ARC_ERR_UNKNOWN_CONTROLLER = 3, /* (only seen as ARC_ERR_NOT_RECOGNIZED by apps) */
    ARC_ERR_WRONG_CODE = 4,
    ARC_ERR_PAIRING_LOCKED = 5,
    ARC_ERR_PAIRING_REJECTED = 6,
    ARC_ERR_BUSY = 7,
    ARC_ERR_AUTH_FAILED = 8,        /* pairing confirmation failed */
    ARC_ERR_TIMEOUT = 9,
    ARC_ERR_REVOKED = 10,           /* authenticated revocation: forget the pairing */
    ARC_ERR_CONNECT_FAILED = 100,
    ARC_ERR_CONNECTION_LOST = 101,
    /* The PC (or something pretending to be it) refused this phone without
     * proving its identity. NEVER delete the pairing automatically: ask the
     * user whether to pair again. */
    ARC_ERR_NOT_RECOGNIZED = 102,
    ARC_ERR_WRONG_AGENT = 103       /* not the paired PC at that address; retried */
} arc_error;

typedef enum {
    ARC_EV_STATUS = 0,           /* status, error, message (CONNECTED: message = "host|port" that worked) */
    ARC_EV_PAIRED = 1,           /* data/data_len = pairing record to persist securely; name = PC name */
    ARC_EV_STATE_RESET = 2,      /* clear the model */
    ARC_EV_ENDPOINT = 3,         /* id, name, volume, muted, is_default */
    ARC_EV_ENDPOINT_REMOVED = 4, /* id */
    ARC_EV_SOURCE = 5,           /* id, endpoint_id, name, icon_key, pid, volume, muted, playing, system_sounds */
    ARC_EV_SOURCE_REMOVED = 6,   /* id */
    ARC_EV_STATE_SYNCED = 7,     /* full dump finished */
    ARC_EV_ICON = 8,             /* icon_key, data/data_len = PNG (empty = no icon) */
    ARC_EV_METERS = 9            /* data = n x {u8 slot_hi, u8 slot_lo, u8 level 0..255}, data_len = 3n */
} arc_event_type;

typedef struct {
    arc_event_type type;
    arc_status status;
    arc_error error;
    const char* message;
    const char* id;
    const char* endpoint_id;
    const char* name;
    const char* icon_key;
    uint32_t pid;
    uint16_t volume; /* 0..10000 */
    uint8_t muted;
    uint8_t is_default;
    uint8_t playing;
    uint8_t system_sounds;
    const uint8_t* data;
    size_t data_len;
    uint16_t meter; /* ENDPOINT/SOURCE: level-meter slot (0 = none) */
} arc_event;

typedef void (*arc_event_cb)(void* user, const arc_event* event);

/* Generates a new random 16-byte controller id (persist it once per install). */
void arc_generate_controller_id(uint8_t out_id[16]);

/* controller_id: 16 bytes; name: UTF-8 shown on the PC (e.g. "Meu iPhone"). */
arc_client* arc_client_create(const uint8_t controller_id[16], const char* name, arc_event_cb cb, void* user);
void arc_client_destroy(arc_client* c);

void arc_client_pair(arc_client* c, const char* host, uint16_t port);
/* record: bytes previously delivered by ARC_EV_PAIRED. Returns 0 if malformed. */
int arc_client_resume(arc_client* c, const char* host, uint16_t port, const uint8_t* record, size_t record_len);
/* Same, trying several addresses in turn (e.g. freshly discovered, then last known). */
int arc_client_resume_multi(arc_client* c, const char* const* hosts, const uint16_t* ports, size_t count,
                            const uint8_t* record, size_t record_len);
void arc_client_submit_code(arc_client* c, const char* code);
void arc_client_disconnect(arc_client* c);

void arc_client_set_endpoint_volume(arc_client* c, const char* endpoint_id, uint16_t volume);
void arc_client_set_endpoint_mute(arc_client* c, const char* endpoint_id, int muted);
void arc_client_set_source_volume(arc_client* c, const char* source_id, uint16_t volume);
void arc_client_set_source_mute(arc_client* c, const char* source_id, int muted);
void arc_client_request_icon(arc_client* c, const char* icon_key);
void arc_client_refresh(arc_client* c);
/* Live level meters while the mixer is visible (costs ~1 KB/s). */
void arc_client_set_meters(arc_client* c, int on);

/* Agent id (16 bytes) and PC name stored in a pairing record. Returns 0 if malformed. */
int arc_record_info(const uint8_t* record, size_t record_len, uint8_t out_agent_id[16], char* out_name, size_t name_cap);

#ifdef __cplusplus
}
#endif

#endif
