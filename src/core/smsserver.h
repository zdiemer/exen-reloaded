/* The mock In-Fusio SMS server (docs/re/SMS.md).
 *
 * The original simulator sent each SMS over TCP to a separate In-Fusio PC tool and injected the
 * tool's answers as received SMS. We have no such tool, so the core plays its part: it decodes
 * what the game sends, reports the send, and for the requests that expect an answer (score and
 * ranking requests, the "Your opinion" vote, unlocks, downloads, restores) queues the answer
 * the game's own SMS code expects, delivered later through Bootstrap.smsReceived.
 *
 * The codec and the server logic below are plain functions on plain structs (no VM), so they
 * can be tested on synthetic buffers; smsv_send / smsv_take_array are the VM glue (sms.c, api.c).
 */
#ifndef EXEN_SMSSERVER_H
#define EXEN_SMSSERVER_H

#include <stddef.h>
#include <stdint.h>

#define SMSV_PAYLOAD 136u      /* SMS payload bytes: Sms.SIZE_SMS, the byte[] onSmsReceived gets */
#define SMSV_HEADER_BITS 128u  /* createSms header, up to the 0x60 container block */
#define SMSV_BLOCKS_BIT 147u   /* first game block: header, 0x60, its 11-bit length */
#define SMSV_MAX_BLOCKS 16
#define SMSV_RANKS 5           /* entries in a ranking reply */
#define SMSV_NAME 15           /* nickname characters (7-bit) */
#define SMSV_UNLOCK_BITS 400u  /* the unlock bitmap of MO/MT_UNLOCK_ITEM and MT_LOAD */
#define SMSV_REPLY_DELAY_MS 2000u

/* Block ids: exen.Sms constants (framework <clinit>) and the ones the games' library adds. */
enum {
    SMSB_MT_ERROR = 0,
    SMSB_MO_UNLOCK_ITEM = 1,
    SMSB_MT_UNLOCK_ITEM = 2,
    SMSB_MO_SCORE = 3,
    SMSB_MT_RANKING = 4,
    SMSB_MO_RANKING = 5,
    SMSB_MO_DOWNLOAD_ITEM = 6,
    SMSB_MT_REQUESTED_ITEM = 7,
    SMSB_MO_LOAD = 8,
    SMSB_MT_LOAD = 9,
    SMSB_MO_SAVE = 10,
    SMSB_MT_SAVE = 11,
    SMSB_MO_RESTORE = 12,
    SMSB_MT_RESTORE = 13,
    SMSB_MO_NICKNAME = 14,  /* IFRacing2: nickname + a 16-bit 0 */
    SMSB_MO_VOTE = 23,      /* "Your opinion": 3-bit mark + nickname */
    SMSB_MT_VOTES = 24,     /* the average mark and the favourite games */
    SMSB_MO_PAY = 25,       /* pay-per-use (continue, extra lives, ...): no answer */
    SMSB_MO_TIME_TRIAL = 27, /* IFRacing2 "Top chrono" upload */
    SMSB_CONTAINER = 0x60,  /* the gamelet message block that holds the game's blocks */
};

/* Bit access, MSB first, as the Sms natives address the payload. Out-of-range bits read as 0
 * and are not written. */
uint32_t smsv_getbits(const uint8_t *p, size_t bytes, uint32_t pos, uint32_t n);
void smsv_putbits(uint8_t *p, size_t bytes, uint32_t pos, uint32_t n, uint32_t v);

/* A sent SMS, decoded. `payload` is the 136-byte payload (the native buffer after "EXEN"),
 * `end` the bit position the game reached. */
typedef struct {
    uint8_t ver_hi, ver_lo;    /* cfg+5, cfg+7 */
    uint16_t gamelet;          /* gamelet record id */
    uint8_t reg[4];            /* registration bytes */
    uint16_t mcc;
    uint8_t mnc, lang;
    uint16_t cfg164;
    uint32_t end;              /* bit position after the last block */
    int nblocks;
    struct {
        uint8_t id;
        uint32_t pos;          /* bit position of its payload */
        uint32_t len;          /* payload length in bits */
    } blocks[SMSV_MAX_BLOCKS];
} SmsvMessage;

/* Decodes the header and the block list. Returns 0, or -1 if the buffer does not hold a
 * well-formed message (blocks running past `end` or the payload). */
int smsv_decode(const uint8_t payload[SMSV_PAYLOAD], uint32_t end, SmsvMessage *m);

/* Writes a block (id, 11-bit length, payload bits) at bit `pos` of `out`; returns the position
 * after it, or 0 if it does not fit. */
uint32_t smsv_put_block(uint8_t *out, size_t bytes, uint32_t pos, uint8_t id, const uint8_t *src,
                        uint32_t src_pos, uint32_t nbits);

typedef struct {
    int32_t score;
    int32_t level;
    uint8_t name[SMSV_NAME];
    uint8_t name_len;
} SmsvRank;

typedef struct ExSmsServer {
    /* host settings (exen.h), kept across state loads */
    int mode;                  /* EXEN_SMS_* */
    int verbose;               /* log every message in hex as well */
    int32_t year, month, day;  /* the server's date, for ranking replies */
    /* server state, saved in the SMSV chunk */
    uint32_t pending;          /* a reply is queued */
    uint32_t due;              /* VM clock (ms) at which it is delivered */
    uint8_t reply[SMSV_PAYLOAD];
    uint32_t nranks;           /* scores received, best first */
    SmsvRank ranks[SMSV_RANKS];
    uint8_t nick[SMSV_NAME];   /* the last nickname received */
    uint32_t nick_len;
    uint8_t unlocked[SMSV_UNLOCK_BITS / 8]; /* every unlock granted */
    uint32_t votes, vote_sum;
    uint32_t sent, replies;    /* counters, for the log */
} ExSmsServer;

/* Server logic, VM-free. Handles a sent message at VM time `now`: updates the records and
 * queues the reply, if the request has one. `title` names the game in the votes reply (may be
 * NULL). Writes a one-line summary to `log`. Returns 1 if a reply was queued (a reply still
 * pending is replaced), 0 if the request has no answer, -1 if the message is malformed. */
int smsv_handle(ExSmsServer *s, const uint8_t payload[SMSV_PAYLOAD], uint32_t end, uint32_t now,
                const char *title, char *log, size_t logcap);
/* The queued reply, if it is due at `now`: copies the 136-byte message the game receives and
 * clears it. Returns 1 if a reply was taken. */
int smsv_take(ExSmsServer *s, uint32_t now, uint8_t out[SMSV_PAYLOAD]);
/* Resets the server state (not the host settings). */
void smsv_reset(ExSmsServer *s);

/* Save states: the saved part of the server as a versioned byte string (state.c, SMSV). */
#define SMSV_STATE_MAX 512u
size_t smsv_save(const ExSmsServer *s, uint8_t *out, size_t cap);
int smsv_load(ExSmsServer *s, const uint8_t *in, size_t len); /* 0, or -1 if malformed */

/* VM glue. smsv_send: Gamelet.sendSms with the Sms native buffer (0x94 bytes: "EXEN", the
 * payload, the bit position); returns 1 if the send is reported as successful (sms.c).
 * smsv_take_array: a reply that is due on the VM clock, as the byte[] for
 * Bootstrap.smsReceived (api.c delivers it after a host callback), or 0. */
struct ExenVM;
int smsv_send(struct ExenVM *vm, const uint8_t *buf);
uint32_t smsv_take_array(struct ExenVM *vm);

#endif
