#pragma once
// TCP port for Scream server data, configurable
#define PORT 4010
// Scream server IP, configurable
#define SERVER "192.168.3.114"

// Number of chunks to be buffered before playback starts, configurable
#define INITIAL_BUFFER_SIZE 48
// Number of chunks to add each underflow, configurable
#define BUFFER_GROW_STEP_SIZE 16
// Max number of chunks to be buffered before packets are dropped, configurable
#define  MAX_BUFFER_SIZE 128

// Sample rate for incoming PCM, configurable
#define SAMPLE_RATE 48000
// Bit depth for incoming PCM, non-configurable (does not implement 24->32 bit padding)
#define BIT_DEPTH 16
//Volume 0.0f-1.0f
#define VOLUME 1.0f

#define TAG "scream_receiver"