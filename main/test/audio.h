
#ifndef AUDIO_H
#define AUDIO_H
#include <stddef.h>
void audio_init_dma(void);
void audio_start_dma_transfer(void *buf, size_t len);
#endif
