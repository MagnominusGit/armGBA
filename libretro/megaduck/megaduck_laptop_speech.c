#include "gb.h"
#include <time.h>
#include <string.h>
#include <stdio.h>

#include "megaduck_laptop_periph.h"


// For Write to peripheral RTC (from Duck ROM perspective)
void MD_speech_playback(GB_megaduck_laptop_t * periph) {

    if (periph->rx_buffer_size == MEGADUCK_BUF_SZ_PLAYSPEECH) {

        int speech_phrase_num = periph->rx_buffer[MEGADUCK_PLAYSPEECH_IDX_SAMPLE_NUM];

        if (speech_phrase_num < MEGADUCK_SPEECH_CMD_MIN) {
            printf("MD_speech_playback() = %d: Invalid value, no playback\n", speech_phrase_num);
        } else {
            // Values above max (6) seem to wrap back around to 1
            speech_phrase_num = (speech_phrase_num % MEGADUCK_SPEECH_CMD_MAX) + 1;
            printf("MD_speech_playback() = %d\n", speech_phrase_num);
            duckLaptopSpeak(speech_phrase_num);  // armGBA
            // Does not require GB Audio to be enabled
        }
    }
}
