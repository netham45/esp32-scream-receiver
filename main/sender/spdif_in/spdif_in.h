#ifndef SPDIF_H
#define SPDIF_H

#include "types.h"
#include "esp_err.h"
#include "freertos/ringbuf.h"
#include <string.h>

// Configuration constants
#define RMT_RESOLUTION_HZ 80000000 // 80MHz resolution for maximum accuracy
#define RMT_MEM_BLOCK_SYMBOLS 4096 // Larger buffer for better performance
#define SYMBOL_BUFFER_SIZE (8192) // 64KB symbol buffer
#define PCM_BUFFER_SIZE (4096)     // 8KB PCM output buffer
#define DECODER_TASK_STACK 4096   // 4KB stack for decoder task
#define DECODER_TASK_PRIORITY 10   // High priority for audio
#define MIN_SAMPLES_FOR_ANALYSIS 10000 // Minimum samples before attempting analysis

#ifdef __cplusplus
extern "C" {
#endif

extern RingbufHandle_t pcm_buffer;

esp_err_t spdif_receiver_init(int input_pin, void (*init_done_cb)(void));
esp_err_t spdif_receiver_start(void);
esp_err_t spdif_receiver_stop(void);
void spdif_receiver_deinit(void);
uint32_t spdif_receiver_get_sample_rate(void);

inline int spdif_receiver_read(uint8_t *buffer, size_t size)
{
    if (!pcm_buffer)
    {
        return 0;
    }
    size_t received_size = 0;
    uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(
        pcm_buffer, &received_size, pdMS_TO_TICKS(10), size);
    if (data && received_size > 0)
    {
        memcpy(buffer, data, received_size);
        vRingbufferReturnItem(pcm_buffer, (void *)data);
        return received_size;
    }
    return 0;
}


#ifdef __cplusplus
}
#endif

#endif // SPDIF_H