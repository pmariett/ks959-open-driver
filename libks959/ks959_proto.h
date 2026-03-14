#ifndef KS959_PROTO_H
#define KS959_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "ks959.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KS-959 internal protocol model
 *
 * This header captures the protocol constants and helper contracts derived
 * from the Linux ks959-sir behavior.
 *
 * Scope:
 * - EP0 control transfers only
 * - SIR-oriented operation first
 * - TX obfuscation / RX de-obfuscation helpers
 */

/* --------------------------------------------------------------------------
 * USB request model
 * -------------------------------------------------------------------------- */

#define KS959_REQTYPE_OUT_CLASS_IFACE 0x21u
#define KS959_REQTYPE_IN_CLASS_IFACE  0xA1u

#define KS959_REQ_RX_POLL  0x01u
#define KS959_REQ_OUT_8    0x09u

#define KS959_RX_WVALUE    0x0200u
#define KS959_RX_WINDEX    0x0000u

#define KS959_SPEED_WVALUE 0x0200u
#define KS959_SPEED_WINDEX 0x0001u

#define KS959_OUT8_WINDEX  0x0000u

/* --------------------------------------------------------------------------
 * Buffer and fragment sizing
 * -------------------------------------------------------------------------- */

#define KS959_RCV_FIFO_SIZE       2048u
#define KS959_SND_FIFO_SIZE       2048u
#define KS959_SND_PACKET_SIZE      256u

/*
 * Linux computes obfuscated TX packet size as:
 *   ((clear_len + 7) & ~0x7) + 0x10
 *
 * Given a hard USB packet ceiling of 256 bytes, the maximum clear fragment
 * size that still fits is 240 bytes.
 */
#define KS959_TX_PADDED_OVERHEAD    16u
#define KS959_TX_FRAGMENT_MAX_CLEAR 240u

/* --------------------------------------------------------------------------
 * Speed flags
 * -------------------------------------------------------------------------- */

#define KS959_SPEED_DATA_5_BITS 0x00u
#define KS959_SPEED_DATA_6_BITS 0x01u
#define KS959_SPEED_DATA_7_BITS 0x02u
#define KS959_SPEED_DATA_8_BITS 0x03u

#define KS959_SPEED_STOP_BITS_1 0x00u
#define KS959_SPEED_STOP_BITS_2 0x08u

#define KS959_SPEED_PAR_DISABLE 0x00u
#define KS959_SPEED_PAR_EVEN    0x10u
#define KS959_SPEED_PAR_ODD     0x30u

#define KS959_SPEED_RESET       0x80u

/*
 * Practical default used by Linux for normal SIR operation:
 * 8 data bits, no parity, 1 stop bit.
 */
#define KS959_SPEED_FLAGS_DEFAULT \
    (KS959_SPEED_DATA_8_BITS | KS959_SPEED_STOP_BITS_1 | KS959_SPEED_PAR_DISABLE)

/* --------------------------------------------------------------------------
 * Packed payloads
 * -------------------------------------------------------------------------- */
#pragma pack(push, 1)
typedef struct ks959_out8_payload {
    uint32_t param0_le;
    uint8_t  param1;
    uint8_t  reserved[3];
} ks959_out8_payload_t;
#pragma pack(pop)

/* --------------------------------------------------------------------------
 * RX decode session state
 * -------------------------------------------------------------------------- */

typedef struct ks959_rx_decode_state {
    /*
     * Session-wide variable XOR state.
     * Linux increments this for every received byte and does not reset it
     * per frame.
     */
    uint8_t variable_xormask;
} ks959_rx_decode_state_t;

/* --------------------------------------------------------------------------
 * Setup-packet builders
 * -------------------------------------------------------------------------- */

void ks959_proto_build_rx_setup(ks959_setup_packet_t *setup);
void ks959_proto_build_speed_setup(ks959_setup_packet_t *setup);
void ks959_proto_build_tx_setup(ks959_setup_packet_t *setup,
                                uint16_t clear_len,
                                uint16_t padded_len);

/* --------------------------------------------------------------------------
 * Speed payload helper
 * -------------------------------------------------------------------------- */

void ks959_proto_fill_speed_params(ks959_out8_payload_t *params,
                                   uint32_t baudrate,
                                   uint8_t flags);

/* --------------------------------------------------------------------------
 * TX obfuscation helpers
 * -------------------------------------------------------------------------- */

/*
 * Returns the padded/obfuscated packet length required for a clear fragment,
 * or 0 if clear_len cannot fit inside max_out_len.
 */
size_t ks959_proto_get_tx_padded_length(size_t clear_len, size_t max_out_len);

/*
 * Obfuscates one already-wrapped clear fragment into the USB payload format
 * expected by the dongle.
 *
 * Returns KS959_OK on success.
 */
int ks959_proto_obfuscate_tx_fragment(const uint8_t *clear_buf,
                                      size_t clear_len,
                                      uint8_t *out_buf,
                                      size_t out_buf_size,
                                      size_t *out_len);

/* --------------------------------------------------------------------------
 * RX de-obfuscation helpers
 * -------------------------------------------------------------------------- */

void ks959_proto_rx_state_init(ks959_rx_decode_state_t *state);

/*
 * De-obfuscates one chunk returned by the dongle.
 *
 * Some input bytes may be discarded by design when the session XOR state wraps,
 * so out_len may be smaller than in_len.
 */
int ks959_proto_deobfuscate_rx_chunk(ks959_rx_decode_state_t *state,
                                     const uint8_t *in_buf,
                                     size_t in_len,
                                     uint8_t *out_buf,
                                     size_t out_buf_size,
                                     size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* KS959_PROTO_H */