#pragma once
#include "stdint.h"
#include "config.h"
#include "stdbool.h"
// PCM Bytes per chunk, non-configurable (Part of Scream)
#define PCM_CHUNK_SIZE 1152

// Global sleep state
extern bool device_sleeping;
