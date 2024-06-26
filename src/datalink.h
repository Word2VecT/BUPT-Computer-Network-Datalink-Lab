#include "protocol.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * should be contained in protocol.h
 * START
 */
/* sequence or ack numbers */
typedef unsigned char seq_nr;

/* frame_kind definition */
typedef enum { FRAME_DATA,
    FRAME_ACK,
    FRAME_NAK } frame_kind;

typedef struct { /* frames are transported in this layer */
    unsigned char kind; /* what kind of frame is it? */
    seq_nr ack; /* sequence number */
    seq_nr seq; /* acknowledgement number */
    unsigned char data[PKT_LEN]; /* the network layer packet */
    unsigned int padding; /* CRC padding */
} frame;

typedef struct {
    unsigned char buf[PKT_LEN];
    size_t length;
} packet;
/*
 * END
 */
/*
 * datalink.h START
 */
enum {
    MAX_SEQ = 31, /* should be 2^n - 1 */
    ACK_TIMER = 211,
    DATA_TIMER = 4096
};
#define NR_BUFS ((MAX_SEQ + 1) / 2)
// NOLINTBEGIN(readability-identifier-length)
static bool between(seq_nr a, seq_nr b, seq_nr c);
static void put_frame(unsigned char* frame, int len);
static void send_datalink_frame(unsigned char fk, seq_nr frame_nr, seq_nr frame_expected, packet packet[]);
// NOLINTEND(readability-identifier-length)

/* Macro inc is expanded in-line: increment k circularly */
#define inc(k)         \
    if ((k) < MAX_SEQ) \
        (k)++;         \
    else               \
        (k) = 0
/*
 * datalink.h END
 */

/*
    DATA Frame
    +=========+========+========+===============+========+
    | KIND(1) | SEQ(1) | ACK(1) | DATA(256) | CRC(4) |
    +=========+========+========+===============+========+

    ACK Frame
    +=========+========+
    | KIND(1) | ACK(1) |
    +=========+========+

    NAK Frame
    +=========+========+
    | KIND(1) | ACK(1) |
    +=========+========+
*/
