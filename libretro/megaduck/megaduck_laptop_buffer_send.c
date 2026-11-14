#include "gb.h"
#include <time.h>
#include <string.h>
#include <stdio.h>

#include "megaduck_laptop_periph.h"


// Add a byte to the external clock send queue (FIFO)
void MD_send_buf_enqueue(GB_megaduck_laptop_t * periph, uint8_t byte_to_enqueue) {

    #ifdef MEGADUCK_SYS_SERIAL_LOG_TX_BYTES
        printf("   ..  Periph: Enqueue 0x%02x as [%d]\n", byte_to_enqueue, periph->ext_clk_send_buf_size);
    #endif

    if (periph->ext_clk_send_buf_size <= MEGADUCK_BUF_SZ)
        periph->ext_clk_send_buf[ periph->ext_clk_send_buf_size++ ] = byte_to_enqueue;
}


void MD_send_buf_calc_enqueue_checksum(GB_megaduck_laptop_t * periph) {

    uint8_t checksum = 0;
    for (int c = 0; c < periph->ext_clk_send_buf_size; c++) {
        checksum += periph->ext_clk_send_buf[c];
    }
    checksum = ~checksum + 1;  // 2's complement
    MD_send_buf_enqueue(periph, checksum);
}


// Called after last bytes added to the external clock send queue
// Resets the buffer index and time until next serial clock / bit send is triggered
void MD_send_buf_finalize_and_transmit(GB_megaduck_laptop_t * periph) {

    #ifdef MEGADUCK_SYS_SERIAL_LOG_TX_BUFFER
        printf("-- MEGADUCK_SYS_SERIAL_LOG_TX_BUFFER --\n");
        for (int c = 0; c < periph->ext_clk_send_buf_size; c++) {
            printf("tx buf [%d] = 0x%02x\n", c, periph->ext_clk_send_buf[c]);
        }
    #endif

    // Whenever there is a byte to send, reset and add a longer delay until next serial bit send
    // Then reset to start of send queue
    periph->t_states_till_update = MEGADUCK_LAPTOP_TICK_COUNT_TX_DELAY;
    periph->ext_clk_send_buf_index = MEGADUCK_LAPTOP_EXT_CLOCK_SEND_INDEX_RESET;
}


// Process the command completed reply from the Mega Duck
void MD_send_buf_handle_tx_reply(GB_megaduck_laptop_t * periph) {

    switch (periph->byte_being_received) {

        case 0x81:
        case MEGADUCK_SYS_CMD_DONE_OR_OK:
            // If this is the ack stage then success, transfer is finished. Return to normal state
            // Otherwise ignore
            if ((periph->state == MEGADUCK_SYS_STATE_GET_KEYS_WAIT_ACK) ||
                (periph->state == MEGADUCK_SYS_STATE_GET_RTC_WAIT_ACK))  {
                periph->state = MEGADUCK_SYS_STATE_INIT_OK_READY;
            }
            break;

        // Handle failure scenario, do a soft-reset back to initialized and ready // TODO: maybe this should be a hard reset?
        case MEGADUCK_SYS_CMD_ABORT_OR_FAIL:
            MD_periph_reset(periph, MEGADUCK_SYS_KEEP_INIT_RESET);
            break;

        // Any other unexpected reply value seems like it should be same as explicit abort/fail
        default:
            MD_periph_reset(periph, MEGADUCK_SYS_KEEP_INIT_RESET);
            break;
    }
}
