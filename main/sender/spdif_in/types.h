#ifndef SPDIF_TYPES_H
#define SPDIF_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/rmt_rx.h"

// Validation result structure
typedef struct
{
    bool groups_identified;   // Three pulse groups found
    bool ratios_valid;        // Ratios match 1:2:3 within tolerance
    bool distribution_valid;  // Distribution matches expected percentages
    float ratio_error;        // Error from ideal 1:2:3 ratio
    float short_pulse_pct;    // Actual short pulse percentage
    float medium_pulse_pct;   // Actual medium pulse percentage
    float long_pulse_pct;     // Actual long pulse percentage
    float distribution_error; // Total distribution error
} timing_validation_t;

// Pulse classification types
typedef enum
{
    PULSE_TYPE_UNKNOWN = 0,
    PULSE_TYPE_SHORT,  // 0.5T pulse
    PULSE_TYPE_MEDIUM, // 1.0T pulse
    PULSE_TYPE_LONG,   // 1.5T pulse
} pulse_type_t;

// Preamble patterns according to IEC 60958-1
typedef enum
{
    PREAMBLE_NONE = 0,
    PREAMBLE_B,       // Block start (frame 0, channel 0)
    PREAMBLE_M,       // Left channel (not frame 0)
    PREAMBLE_W,       // Right channel
    PREAMBLE_UNKNOWN, // 1.5T pulse detected but pattern not yet identified
} preamble_type_t;

// Preamble pattern definitions
typedef struct {
    uint8_t pattern_0;  // Pattern when previous bit was 0
    uint8_t pattern_1;  // Pattern when previous bit was 1
} preamble_pattern_t;

// Preamble capture state for pattern recognition
typedef struct {
    bool capturing;           // True when capturing preamble bits
    uint8_t bit_buffer;      // Buffer for captured bits
    uint8_t bits_captured;   // Number of bits captured so far
    bool last_bit_state;     // State of the bit before preamble (for pattern selection)
    uint32_t capture_start_time; // When capture started (for timeout)
} preamble_capture_t;

// BMC Decoder State Machine States
typedef enum
{
    BMC_STATE_IDLE = 0,
    BMC_STATE_EXPECTING_SECOND_HALF,
    BMC_STATE_SYNCED,
    BMC_STATE_PREAMBLE_DETECTED,
    BMC_STATE_ERROR,
} bmc_state_t;

// BMC Decoder Context
typedef struct
{
    bmc_state_t state;
    uint32_t last_pulse_width;
    pulse_type_t last_pulse_type;
    bool expecting_transition;
    uint32_t bit_count;
    uint32_t subframe_count;
    uint32_t frame_count;
    uint32_t block_count;
    uint32_t sync_errors;
    uint32_t decode_errors;
    preamble_type_t last_preamble;
    uint32_t preamble_b_count;
    uint32_t preamble_m_count;
    uint32_t preamble_w_count;
    uint32_t total_preambles;
    uint8_t current_channel;
    bool last_bit_value;
    preamble_capture_t preamble_capture;
} bmc_decoder_t;

// Bit Accumulator for Subframe Data
typedef struct
{
    uint32_t data;
    uint8_t bit_position;
    bool parity_accumulator;
    bool valid;
} bit_accumulator_t;

// S/PDIF Subframe Structure
typedef struct
{
    uint32_t preamble : 4;
    uint32_t aux_data : 4;
    uint32_t audio_sample : 20;
    uint32_t validity : 1;
    uint32_t user_data : 1;
    uint32_t channel_status : 1;
    uint32_t parity : 1;
} spdif_subframe_t;

// Parsed S/PDIF Subframe Structure
typedef struct
{
    uint32_t audio_sample;
    uint8_t  aux_data;
    bool     validity;
    bool     user_bit;
    bool     channel_status;
    bool     parity;
    uint8_t  channel;
    uint32_t frame_number;
    uint32_t block_number;
    bool     parity_valid;
} parsed_subframe_t;

// Channel Status Block Structure
typedef struct
{
    uint8_t bits[24];
    uint32_t current_bit;
    bool complete;
    uint32_t block_count;
} channel_status_block_t;

// Channel Status Control Structure
typedef struct
{
    bool     professional;
    bool     non_pcm;
    bool     copy_permit;
    uint8_t  emphasis;
    uint8_t  mode;
} channel_status_control_t;

// Parsed Channel Status Structure
typedef struct
{
    channel_status_control_t control;
    uint8_t  category_code;
    uint32_t sample_rate;
    uint8_t  word_length;
    uint8_t  source_number;
    uint8_t  channel_number;
} parsed_channel_status_t;

// Peak detection structure for histogram analysis
typedef struct
{
    uint32_t bin;
    uint32_t count;
    float center;
    uint32_t width;
} peak_t;

// Public-facing structs from spdif_receiver.h
typedef struct
{
    uint16_t pcm_sample;
    uint8_t channel;
    uint8_t padding;
} pcm_sample_with_channel_t;

typedef struct {
    bool valid;
    uint32_t sample_rate;
    uint8_t bit_depth;
    bool copy_protected;
    uint8_t category_code;
} spdif_channel_status_t;

typedef struct {
    bool locked;
    uint32_t frame_number;
    uint32_t block_count;
    uint32_t parity_errors;
} spdif_sync_status_t;

#endif // SPDIF_TYPES_H