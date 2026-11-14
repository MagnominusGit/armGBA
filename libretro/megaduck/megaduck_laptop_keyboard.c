#include "gb.h"
#include <time.h>
#include <string.h>
#include <stdio.h>

#include "megaduck_laptop_periph.h"

// Key Repeat reply data packets look like this:
//
// - First  packet after key press: Repeat Flag *NOT* set, Scan code matches key
// - N+1   packets after key press: Repeat Flag *IS*  set, Scan code set to 0x00 (None)
//
// * <len> <flags> <scancode> <checksum>
// * .. 0x00 0x00 ..
// * .. 0x00 0x96 ..    ("t" key pressed and held)
// * .. 0x01 0x00 ..    ("t" key still pressed but value cleared, Repeat flag set)
// * .. 0x01 0x00 ..    ("t" key still pressed but value cleared, Repeat flag set)
// * .. 0x01 0x00 ..    ("t" key still pressed but value cleared, Repeat flag set)
// * .. 0x01 0x00 ..    ("t" key still pressed but value cleared, Repeat flag set)
// * .. 0x00 0x00 ..    ("t" key was released, Repeat flag cleared)


// Enqueue keyboard reply data
// [0]: Length of reply (always 4)
// [1]: Keyboard modifier keys + left printscreen
//         KEY REPEAT : |= 0x01 (so far looks like with no key value set in 3rd Byte)
//         CAPS_LOCK: |= 0x02
//         SHIFT: |= 0x04
//         LEFT_PRINTSCREEN: |= 0x08
// [2]: Keyboard scan codes
// [3]: checksum
void MD_keyboard_enqueue_reply(GB_megaduck_laptop_t * periph) {

    uint8_t send_key = periph->key;
    uint8_t send_key_modifiers = periph->key_modifiers;

    // Check if a key is being held down. If so and it's unchanged since last time
    // then set the key repeat flag and clear the key value (System ROM expects that from hardware)
    //
    // last_key gets cleared in the KEYUP handler
    if ((periph->key == periph->last_key) && (periph->key != MEGADUCK_KBD_CODE_NONE)) {
        send_key            = MEGADUCK_KBD_CODE_NONE;
        send_key_modifiers |= MEGADUCK_KBD_FLAG_KEY_REPEAT;
    }
    // Update key repeat tracking
    periph->last_key = periph->key;
    periph->last_key_modifiers = periph->key_modifiers;


    #ifdef MEGADUCK_SYS_KEYBOARD_LOG_SEND_NON_NULL
        if (periph->key != MEGADUCK_KBD_CODE_NONE)  // Comment this line to capture all transfers instead of masking empty key presses
             printf("   **** Sending keyboard reply **** (Cur key: 0x%02x /mod: 0x%02x) -> (Sent: 0x%02x /mod: 0x%02x)\n",
                periph->key, periph->key_modifiers,
                send_key, send_key_modifiers);
    #endif

    MD_send_buf_enqueue(periph, MEGADUCK_KBD_BUF_REPLY_LEN);
    MD_send_buf_enqueue(periph, send_key_modifiers);
    MD_send_buf_enqueue(periph, send_key);

    MD_send_buf_calc_enqueue_checksum(periph);
    MD_send_buf_finalize_and_transmit(periph);
    // Override with longer delay for first reply byte
    // since command that initiated the buffer requires 2+ msec delay
    // for unknown reasons (maybe extra delay for RTC latch on reads?)
    periph->t_states_till_update = MEGADUCK_LAPTOP_TICK_COUNT_TX_BUF_REPLY_START;
}

