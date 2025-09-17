#include "freertos/queue.h"
#include "driver/rmt_rx.h"
#include "string.h"
#include "types.h"
#include "histogram.h"

// Pre-cached thresholds for speed
static uint32_t thresh_sm;
static uint32_t thresh_ml;

// 256-byte LUT for pulse classification
static uint8_t pulse_lut[256];

// Initialize thresholds and build LUT
void decoder_init_thresholds(void) {
    thresh_sm = g_timing.short_medium_threshold;
    thresh_ml = g_timing.medium_long_threshold;
    
    for (int i = 0; i < 256; i++) {
        if (i == 0) {
            pulse_lut[i] = 3;  // UNKNOWN
        } else if (i < thresh_sm) {
            pulse_lut[i] = 0;  // SHORT
        } else if (i < thresh_ml) {
            pulse_lut[i] = 1;  // MEDIUM
        } else {
            pulse_lut[i] = 2;  // LONG
        }
    }
}

// Preamble patterns - both normal and inverted
#define PREAMBLE_B_0  0xE8
#define PREAMBLE_B_1  0x17
#define PREAMBLE_M_0  0xE2
#define PREAMBLE_M_1  0x1D
#define PREAMBLE_W_0  0xE4
#define PREAMBLE_W_1  0x1B

static inline void process_spdif_symbols(rmt_symbol_word_t *symbols, size_t num_symbols)
{
    if (!g_timing.timing_discovered) return;
    
    if (!pulse_lut[1]) {
        decoder_init_thresholds();
    }
    
    // Static state - keep minimal for cache efficiency
    static uint32_t state = 0;  // Bit 0: expecting_short, Bit 1: in_preamble, Bit 2: last_data_bit, Bit 3: last_level
    static uint32_t bit_count = 0;
    static uint32_t subframe_data = 0;
    static uint32_t preamble_data = 0;  // Bits 0-7: pattern, Bits 8-11: bit_index
    static uint32_t channel = 0;  // 0=left, 1=right
    static int16_t left_sample;
    
    // Process all symbols
    for (size_t i = 0; i < num_symbols; i++)
    {
        uint32_t dur0 = symbols[i].duration0;
        uint32_t dur1 = symbols[i].duration1;
        
        // Macro to process each duration - uses LUT for pulse classification
        #define PROCESS(dur) \
        { \
            uint32_t ptype = pulse_lut[dur & 0xFF]; \
            if (ptype < 3) { \
                \
                if (state & 2) { /* in_preamble */ \
                    state ^= 8; /* Toggle level */ \
                    \
                    uint32_t bits_to_add = ptype + 1; \
                    uint32_t pindex = (preamble_data >> 8) & 0xF; \
                    uint32_t pattern = preamble_data & 0xFF; \
                    \
                    for (uint32_t j = 0; j < bits_to_add && pindex < 8; j++) { \
                        if (state & 8) pattern |= (1 << (7 - pindex)); \
                        pindex++; \
                    } \
                    \
                    if (pindex >= 8) { \
                        state &= ~2; /* Clear in_preamble */ \
                        \
                        /* Check all 6 valid preambles */ \
                        if (pattern == PREAMBLE_B_0 || pattern == PREAMBLE_B_1) { \
                            channel = 0; \
                        } else if (pattern == PREAMBLE_M_0 || pattern == PREAMBLE_M_1) { \
                            channel = 0; \
                        } else if (pattern == PREAMBLE_W_0 || pattern == PREAMBLE_W_1) { \
                            channel = 1; \
                        } \
                    } else { \
                        preamble_data = pattern | (pindex << 8); \
                    } \
                } \
                else if (ptype == 2 && !(state & 1)) { /* LONG pulse, not expecting_short */ \
                    /* Start preamble */ \
                    state |= 2; /* Set in_preamble */ \
                    preamble_data = 0; \
                    \
                    /* Set initial level based on last_data_bit and toggle */ \
                    state = (state & ~8) | ((state & 4) << 1); /* Copy bit 2 to bit 3 */ \
                    state ^= 8; /* Toggle */ \
                    \
                    /* Add 3 bits for LONG */ \
                    uint32_t pattern = 0; \
                    if (state & 8) pattern = 0xE0; /* 111 in top 3 bits */ \
                    preamble_data = pattern | (3 << 8); \
                    \
                    /* Reset subframe */ \
                    bit_count = 0; \
                    subframe_data = 0; \
                    state &= ~1; /* Clear expecting_short */ \
                } \
                else if (bit_count < 28) { /* Normal data */ \
                    if (state & 1) { /* expecting_short */ \
                        if (ptype == 0) { /* SHORT - completes '1' bit */ \
                            subframe_data |= (1UL << bit_count); \
                        } \
                        bit_count++; \
                        state &= ~1; /* Clear expecting_short */ \
                    } else { \
                        if (ptype == 1) { /* MEDIUM - '0' bit */ \
                            bit_count++; \
                        } else if (ptype == 0) { /* SHORT - first half of '1' */ \
                            state |= 1; /* Set expecting_short */ \
                        } \
                    } \
                    \
                    if (bit_count == 28) { \
                        /* Track last bit for next preamble */ \
                        state = (state & ~4) | ((subframe_data & (1UL << 27)) ? 4 : 0); \
                        \
                        /* Extract audio */ \
                        int32_t sample = (int32_t)(subframe_data & 0xFFFFFF); \
                        \
                        /* Sign extend from 24-bit to 32-bit */ \
                        if (sample & 0x800000) { \
                            sample |= 0xFF000000; \
                        } \
                        \
                        /* Convert to 16-bit */ \
                        int16_t s16 = (int16_t)(sample >> 8); \
                        \
                        if (channel == 0) { \
                            left_sample = s16; \
                        } else { \
                            int16_t stereo[2] = {left_sample, s16}; \
                            xRingbufferSend(pcm_buffer, stereo, sizeof(stereo), 0); \
                        } \
                    } \
                } \
            } \
        }
        
        PROCESS(dur0);
        PROCESS(dur1);
        
        #undef PROCESS
    }
}