#include "datalink.h"
#include "lprintf.h"
#include "protocol.h"
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables, readability-identifier-length, bugprone-easily-swappable-parameters, readability-function-cognitive-complexity)
static bool no_nak = true; /* no nak has been sent yet */
static bool phl_ready = false;

static bool between(seq_nr a, seq_nr b, seq_nr c)
{
    return ((a <= b) && (b < c)) || ((c < a) && (a <= b)) || ((b < c) && (c < a));
}

static void put_frame(unsigned char* frame, int len)
{
    *(unsigned int*)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = false;
}

static void send_datalink_frame(unsigned char fk, seq_nr frame_nr, seq_nr frame_expected, packet packet[])
{
    /* Construct and send a data, ack, or nak frame */
    frame s; /* scratch variable */

    s.kind = fk; /* kind == FRAME_DATA, FRAME_ACK, or FRAME_NAK */
    if (fk == FRAME_DATA) {
        memcpy(s.data, packet[frame_nr % NR_BUFS].buf, packet[frame_nr % NR_BUFS].length);
        s.seq = frame_nr; /* only meaningful for data frames */
        dbg_frame("Send DATA %d %d, ID %d\n", frame_nr,
            ((frame_expected + MAX_SEQ) % (MAX_SEQ + 1)), *(short*)s.data);
    }
    s.ack = ((frame_expected + MAX_SEQ) % (MAX_SEQ + 1));
    if (fk == FRAME_NAK) {
        dbg_frame("Send NAK %d\n", frame_expected);
        no_nak = false; /* one nak per frame. please */
    }
    if (fk == FRAME_DATA) {
        put_frame((unsigned char*)&s, 3 + PKT_LEN);
        start_timer(frame_nr % NR_BUFS, DATA_TIMER);
    } else {
        put_frame((unsigned char*)&s, 2); /* transmit the frame */
    }
    stop_ack_timer(); /* no need for separate ack frame */
}

int main(int argc, char** argv)
{
    seq_nr ack_expected = 0; /* lower edge of sender's window, next ack expected on the inbound stream*/
    seq_nr next_frame_to_send = 0; /* upper edge of sender's window + 1*/
    seq_nr frame_expected = 0; /* lower edge of receiver's window, number of next outgoing frame*/
    seq_nr too_far = NR_BUFS; /* upper edge of receiver's window + 1*/
    int i = 0; /* index into packet pool */
    frame r; /* scratch variable */
    packet out_buf[NR_BUFS]; /* buffers for the outbound stream */
    packet in_buf[NR_BUFS]; /* buffers for the inbound stream */
    bool arrived[NR_BUFS]; /* inbound bit map */
    seq_nr nbuffered = 0; /* how many output packets currently used, initially no packets are packeted*/

    int event = 0;
    int arg = 0;
    int len = 0;

    protocol_init(argc, argv);
    lprintf("Designed by Tang Zinan & Liu Rui, build: " __DATE__ "  "__TIME__
            "\n");
    enable_network_layer();
    for (i = 0; i < NR_BUFS; ++i) {
        arrived[i] = false;
    }

    while (true) {
        event = wait_for_event(&arg); /* five possibilities: see event_type above */

        switch (event) {
        case NETWORK_LAYER_READY: /* accept, save, and transimit a new frame */
            nbuffered++; /* expand the window */
            out_buf[next_frame_to_send % NR_BUFS].length = get_packet(out_buf[next_frame_to_send % NR_BUFS].buf); /* fetch new packet */
            send_datalink_frame((unsigned char)FRAME_DATA, next_frame_to_send, frame_expected, out_buf); /* transmit the frame */
            inc(next_frame_to_send); /* advance upper windows edge */
            break;

        case PHYSICAL_LAYER_READY:
            phl_ready = true;
            break;

        case FRAME_RECEIVED: /* a data or control frame has arrived */
            len = recv_frame((unsigned char*)&r, sizeof r); /* fetch incoming frame from physical layer */
            if (len < 5 || crc32((unsigned char*)&r, len) != 0) {
                dbg_event("****RECEIVER ERROR, BAD CRC CHECKSUM****\n");
                if (no_nak) {
                    send_datalink_frame((unsigned char)FRAME_NAK, 0, frame_expected, out_buf);
                }
                break;
            }

            if (r.kind == FRAME_ACK) {
                dbg_frame("Recv ACK  %d\n", r.ack);
            }

            if (r.kind == FRAME_DATA) {
                /* An undamaged frame has arrived */
                dbg_frame("Recv DATA %d %d, ID %d\n", r.seq, r.ack, *(short*)r.data);
                if ((r.seq != frame_expected) && no_nak) {
                    send_datalink_frame((unsigned char)FRAME_NAK, 0, frame_expected, out_buf);
                }
                if (between(frame_expected, r.seq, too_far) && (arrived[r.seq % NR_BUFS] == false)) {
                    arrived[r.seq % NR_BUFS] = true; /* mark packet as full */
                    memcpy(in_buf[r.seq % NR_BUFS].buf, r.data, len - 7);
                    in_buf[r.seq % NR_BUFS].length = len - 7; /* insert data into packet */
                    while (arrived[frame_expected % NR_BUFS]) {
                        /* Pass frames and advance window. */
                        put_packet(in_buf[frame_expected % NR_BUFS].buf, (int)in_buf[frame_expected % NR_BUFS].length);
                        no_nak = true;
                        arrived[frame_expected % NR_BUFS] = false;
                        inc(frame_expected); /* advance lower edge of receiver's window */
                        inc(too_far); /* advance upper edge of receiver's window */
                        start_ack_timer(ACK_TIMER); /* to see if a separate ack is needed */
                    }
                }
            }

            if (r.kind == FRAME_NAK) {
                dbg_frame("Recv NAK %d\n", r.ack);
                if (between(ack_expected, (r.ack + 1) % (MAX_SEQ + 1), next_frame_to_send)) {
                    send_datalink_frame((unsigned char)FRAME_DATA, (r.ack + 1) % (MAX_SEQ + 1), frame_expected, out_buf);
                }
            }

            while (between(ack_expected, r.ack, next_frame_to_send)) {
                nbuffered--; /* handle piggybacked ack */
                stop_timer(ack_expected % NR_BUFS); /* frame arrived intact */
                inc(ack_expected); /* advance lower edge of sender's window */
            }
            break;

        case DATA_TIMEOUT:
            dbg_event("---- DATA %d timeout\n", arg);
            if (!between(ack_expected, arg, next_frame_to_send)) {
                arg += NR_BUFS;
            }
            send_datalink_frame((unsigned char)FRAME_DATA, arg, frame_expected, out_buf); /* we timed out */
            break;

        case ACK_TIMEOUT:
            dbg_event("----  ACK %d timeout\n", frame_expected);
            send_datalink_frame((unsigned char)FRAME_ACK, 0, frame_expected, out_buf); /* ack timer expired; send ack */
            break;

        default:
            break;
        }
        if (nbuffered < NR_BUFS && phl_ready == true) {
            enable_network_layer();
        } else {
            disable_network_layer();
        }
    }
}
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables, readability-identifier-length, bugprone-easily-swappable-parameters, readability-function-cognitive-complexity)