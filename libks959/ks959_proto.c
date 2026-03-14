#include "ks959_proto.h"

#include <string.h>

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static void ks959_zero_setup(ks959_setup_packet_t *setup)
{
    if (setup) {
        memset(setup, 0, sizeof(*setup));
    }
}

static void ks959_write_le16(uint16_t value, uint8_t out[2])
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void ks959_write_le32(uint32_t value, uint8_t out[4])
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
}

/* --------------------------------------------------------------------------
 * Setup builders
 * -------------------------------------------------------------------------- */

void ks959_proto_build_rx_setup(ks959_setup_packet_t *setup)
{
    if (!setup) {
        return;
    }

    ks959_zero_setup(setup);
    setup->bmRequestType = KS959_REQTYPE_IN_CLASS_IFACE;
    setup->bRequest      = KS959_REQ_RECV;
    setup->wValue        = KS959_RX_WVALUE;
    setup->wIndex        = KS959_RX_WINDEX;
    setup->wLength       = KS959_RCV_FIFO_SIZE;
}

void ks959_proto_build_speed_setup(ks959_setup_packet_t *setup)
{
    if (!setup) {
        return;
    }

    ks959_zero_setup(setup);
    setup->bmRequestType = KS959_REQTYPE_OUT_CLASS_IFACE;
    setup->bRequest      = KS959_REQ_SEND;
    setup->wValue        = KS959_SPEED_WVALUE;
    setup->wIndex        = KS959_SPEED_WINDEX;
    setup->wLength       = (uint16_t)sizeof(ks959_speed_params_t);
}

void ks959_proto_build_tx_setup(ks959_setup_packet_t *setup,
                                uint16_t clear_len,
                                uint16_t padded_len)
{
    if (!setup) {
        return;
    }

    ks959_zero_setup(setup);
    setup->bmRequestType = KS959_REQTYPE_OUT_CLASS_IFACE;
    setup->bRequest      = KS959_REQ_SEND;
    setup->wValue        = clear_len;
    setup->wIndex        = 0u;
    setup->wLength       = padded_len;
}

/* --------------------------------------------------------------------------
 * Speed payload helper
 * -------------------------------------------------------------------------- */

void ks959_proto_fill_speed_params(ks959_speed_params_t *params,
                                   uint32_t baudrate,
                                   uint8_t flags)
{
    if (!params) {
        return;
    }

    memset(params, 0, sizeof(*params));
    params->baudrate_le = baudrate;
    params->flags = flags;
}

/* --------------------------------------------------------------------------
 * TX obfuscation helpers
 * -------------------------------------------------------------------------- */

size_t ks959_proto_get_tx_padded_length(size_t clear_len, size_t max_out_len)
{
    size_t padded_len;

    if (clear_len == 0) {
        return 0;
    }

    padded_len = ((clear_len + 7u) & ~(size_t)7u) + KS959_TX_PADDED_OVERHEAD;

    if (padded_len > max_out_len) {
        return 0;
    }

    return padded_len;
}

int ks959_proto_obfuscate_tx_fragment(const uint8_t *clear_buf,
                                      size_t clear_len,
                                      uint8_t *out_buf,
                                      size_t out_buf_size,
                                      size_t *out_len)
{
    static const uint8_t lookup_string[] = "wangshuofei19710";
    size_t padded_len;
    uint8_t xor_mask;
    size_t i;

    if (!clear_buf || !out_buf || !out_len) {
        return KS959_ERR_INVALID_PARAM;
    }

    padded_len = ks959_proto_get_tx_padded_length(clear_len, out_buf_size);
    if (padded_len == 0) {
        return KS959_ERR_BUFFER_TOO_SMALL;
    }

    memset(out_buf, 0, padded_len);

    xor_mask = (uint8_t)(lookup_string[(clear_len & 0x0Fu) ^ 0x06u] ^ 0x55u);

    for (i = 0; i < clear_len; ++i) {
        out_buf[i] = (uint8_t)(clear_buf[i] ^ xor_mask);
    }

    *out_len = padded_len;
    return KS959_OK;
}

/* --------------------------------------------------------------------------
 * RX de-obfuscation helpers
 * -------------------------------------------------------------------------- */

void ks959_proto_rx_state_init(ks959_rx_decode_state_t *state)
{
    if (!state) {
        return;
    }

    state->variable_xormask = 0u;
}

int ks959_proto_deobfuscate_rx_chunk(ks959_rx_decode_state_t *state,
                                     const uint8_t *in_buf,
                                     size_t in_len,
                                     uint8_t *out_buf,
                                     size_t out_buf_size,
                                     size_t *out_len)
{
    size_t i;
    size_t produced = 0;

    if (!state || !in_buf || !out_buf || !out_len) {
        return KS959_ERR_INVALID_PARAM;
    }

    for (i = 0; i < in_len; ++i) {
        uint8_t decoded;

        state->variable_xormask++;
        decoded = (uint8_t)(in_buf[i] ^ state->variable_xormask ^ 0x55u);

        /*
         * Linux skips the byte when the session xormask wraps to 0.
         * This corresponds to the extra garbage byte inserted by the dongle.
         */
        if (state->variable_xormask == 0u) {
            continue;
        }

        if (produced >= out_buf_size) {
            return KS959_ERR_BUFFER_TOO_SMALL;
        }

        out_buf[produced++] = decoded;
    }

    *out_len = produced;
    return KS959_OK;
}