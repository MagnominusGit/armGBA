#include "gb.h"
#include <time.h>
#include <string.h>
#include <stdio.h>

#include "megaduck_laptop_periph.h"

// Power-on init sequence:
//
// # MEGADUCK_SYS_STATE_INIT_1_WAIT_RX_COUNTER,
// 1. Peripheral waits for System ROM to send count up sequence of 0..255 with ~1msec per byte
//    - Reply bytes are ignored
//    - 1msec spacing includes AFTER last byte sent
//
// # MEGADUCK_SYS_STATE_INIT_2_SEND_RX_COUNT_ACK  // External Clock
// 2. Peripheral expected to send a 1 byte response after minimum ~1msec (no timeout in System ROM code):
//    - Success: "0x01" (MEGADUCK_SYS_REPLY_BOOT_OK)
//    - Failure: Any other value reply is interpreted as failure
//
// # MEGADUCK_SYS_STATE_INIT_3_WAIT_TX_COUNTER_REQ
// 3. Peripheral waits for system ROM to send a command 0x00 (MEGADUCK_SYS_CMD_INIT_SEQ_REQUEST)
//    - Reply byte is ignored
//
// # MEGADUCK_SYS_STATE_INIT_4_SEND_TX_COUNTER,      // External Clock
// 4. Peripheral expected to send countdown 255..0 sequence with 1msec per byte
//
// # MEGADUCK_SYS_STATE_INIT_5_WAIT_TX_COUNT_ACK,
// 5. Peripheral waits for System ROM to send ack for sequence
//    - Success: 0x01: MEGADUCK_SYS_CMD_DONE_OR_OK, only happens after last byte sent
//    - Failure: 0x04: MEGADUCK_SYS_CMD_ABORT_OR_FAIL, can happen any time in sequence if byte System ROM receives doesn't match it's parallel counter
//
// # Should be done, now some misc commands will come in, such as unknown 0x09
//   then optionally an RTC reset

// Receive the 0 - 255 counter sent by the System ROM
// Then send an ACK once it's done
void MD_init_stage_1_rx_counter(GB_megaduck_laptop_t * periph) {

    // Once final count value is received move to next state
    if (periph->byte_being_received == MEGADUCK_SYS_INIT_COUNTER_DONE) {

        // Counter done, move to next state and use ext clock serial to send reply byte to GB
        if (periph->init_counter == MEGADUCK_SYS_INIT_COUNTER_DONE) {
            // Success: received counter bytes all matched expected increment
            periph->state = MEGADUCK_SYS_STATE_INIT_2_SEND_RX_COUNT_ACK;
            MD_send_buf_enqueue(periph, MEGADUCK_SYS_REPLY_BOOT_OK);
            MD_send_buf_finalize_and_transmit(periph);
        } else {
            // Failure: some counter bytes sent by System ROM either dropped or corrupted
            // TODO: What happens here is not really known yet, for now assuming it reverts to power-on Reset
            periph->state = MEGADUCK_SYS_STATE_INIT_1_WAIT_RX_COUNTER;
            MD_send_buf_enqueue(periph, MEGADUCK_SYS_REPLY_BOOT_FAIL);
            MD_send_buf_finalize_and_transmit(periph);
        }
    }
    else if (periph->init_counter == periph->byte_being_received) {
        // If the received counter byte matched the expected value then increment it
        periph->init_counter++;
    }

    // TX/Reply byte is ignored by System ROM
    periph->byte_to_send = 0x00; // TODO: Also not yet known what value it typically is
}


// Wait for request to send counter from 0 - 255
// Then send the counter
//
// TODO: Does it need a larger delay between send counter bytes to allow for the System ROM to send
//       MEGADUCK_SYS_CMD_ABORT_OR_FAIL reply (without serial IO dual internal clock contention)
void MD_init_stage_3_wait_tx_count_request(GB_megaduck_laptop_t * periph) {

    if (periph->byte_being_received == MEGADUCK_SYS_CMD_INIT_SEQ_REQUEST) {

        // Got expected command, send countdown sequence
        periph->state = MEGADUCK_SYS_STATE_INIT_4_SEND_TX_COUNTER;

        // Load up 0..255 counter to send
        // It will get sent via GB_megaduck_laptop_peripheral_update()
        for (int c = MEGADUCK_SYS_INIT_COUNTER_DONE; c >= MEGADUCK_SYS_INIT_COUNTER_RESET; c--) {
            MD_send_buf_enqueue(periph, c);
        }
        MD_send_buf_finalize_and_transmit(periph);
    }

    // TX/Reply byte is ignored by System ROM
    periph->byte_to_send = 0x00; // TODO: Also not yet known what value it typically is
}


// Wait for confirmation of sent counter data
// Then send the final status acknowledgement
void MD_init_stage_5_wait_tx_count_ack(GB_megaduck_laptop_t * periph) {

    if (periph->byte_being_received == MEGADUCK_SYS_CMD_DONE_OR_OK) {
        periph->state = MEGADUCK_SYS_STATE_INIT_OK_READY;
    }
    // Failure, reset (System ROM will hang forever in busy loop at 0x0108 instead of retrying)
    else if (periph->byte_being_received == MEGADUCK_SYS_CMD_ABORT_OR_FAIL) {
        // TODO: for now assuming init failure results in a reset of the perihperhal
        MD_periph_reset(periph, MEGADUCK_SYS_POWER_ON_RESET);
    }

    // TX/Reply byte is ignored by System ROM
    periph->byte_to_send = 0x00; // TODO: Also not yet known what value it typically is
}

