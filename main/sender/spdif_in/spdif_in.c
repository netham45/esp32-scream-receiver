#include "spdif_in.h"
#include "histogram.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include "string.h"
#include "math.h"
#include "sdkconfig.h"
#include "decoder.h"

// Global state
static rmt_channel_handle_t g_rx_channel = NULL;
static TaskHandle_t g_decoder_task = NULL;
static TaskHandle_t g_init_task = NULL;
static RingbufHandle_t g_symbol_buffer = NULL;
static rmt_symbol_word_t *g_rmt_buffer = NULL;
static rmt_receive_config_t g_rx_config;
RingbufHandle_t pcm_buffer = NULL;

// Statistics
static struct
{
    uint32_t rmt_callbacks;
    uint32_t symbols_received;
    uint32_t buffer_overflows;
    uint32_t decode_errors;
    uint32_t last_report_time;
    uint32_t parity_errors;
    uint32_t parity_checks;
} g_stats = {0};

// RMT receive callback (ISR context)
static bool IRAM_ATTR rmt_rx_done_callback(
    rmt_channel_handle_t channel,
    const rmt_rx_done_event_data_t *edata,
    void *user_ctx)
{
    BaseType_t task_woken = pdFALSE;
    g_stats.rmt_callbacks++;

    if (edata->flags.is_last)
    {
        rmt_receive(g_rx_channel, g_rmt_buffer,
                    RMT_MEM_BLOCK_SYMBOLS * sizeof(rmt_symbol_word_t),
                    &g_rx_config);
    }

    if (edata->num_symbols > 0)
    {
        g_stats.symbols_received += edata->num_symbols;
        if (xRingbufferSendFromISR(g_symbol_buffer,
                                   edata->received_symbols,
                                   edata->num_symbols * sizeof(rmt_symbol_word_t),
                                   &task_woken) != pdTRUE)
        {
            g_stats.buffer_overflows++;
        }
    }

    vTaskNotifyGiveFromISR(g_decoder_task, &task_woken);
    return task_woken == pdTRUE;
}

// Main decoder task
static void spdif_decoder_task(void *arg)
{
    g_rx_config = (rmt_receive_config_t){
        .signal_range_min_ns = 10,
        .signal_range_max_ns = 10000,
        .flags.en_partial_rx = true,
    };

    ESP_ERROR_CHECK(rmt_enable(g_rx_channel));
    ESP_ERROR_CHECK(rmt_receive(g_rx_channel, g_rmt_buffer,
                                RMT_MEM_BLOCK_SYMBOLS * sizeof(rmt_symbol_word_t),
                                &g_rx_config));

    size_t rx_size = 0;
    rmt_symbol_word_t *symbols = NULL;

    ESP_LOGI("TAG", "Decoder task started");
    while (1)
    {
        //ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(0);
        while (1)
        {
            symbols = (rmt_symbol_word_t *)xRingbufferReceive(
                g_symbol_buffer, &rx_size, 0);
            if (symbols)
            {
                size_t num_symbols = rx_size / sizeof(rmt_symbol_word_t);
                if (!g_timing.timing_discovered)
                {
                    collect_pulse_histogram(symbols, num_symbols);
                    if (g_timing.total_samples >= MIN_SAMPLES_FOR_ANALYSIS)
                    {
                        analyze_pulse_timing();
                    }
                }
                else
                {
                    process_spdif_symbols(symbols, num_symbols);
                }
                vRingbufferReturnItem(g_symbol_buffer, (void *)symbols);
            }
            else
            {
                break;
            }
        }
    }
}

    // Initialize S/PDIF receiver
    void spdif_receiver_init_task(void *pvParameters)
    {
        ESP_LOGI("TAG", "Init task started");
        int input_pin = (int)pvParameters;
        rmt_rx_channel_config_t rx_channel_cfg = {
            .gpio_num = input_pin,
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = RMT_RESOLUTION_HZ,
            .mem_block_symbols = RMT_MEM_BLOCK_SYMBOLS,
            .flags.with_dma = true,
        };

        esp_err_t ret = rmt_new_rx_channel(&rx_channel_cfg, &g_rx_channel);
        if (ret != ESP_OK)
        {
            return;
        }

        rmt_rx_event_callbacks_t cbs = {
            .on_recv_done = rmt_rx_done_callback};
        ret = rmt_rx_register_event_callbacks(g_rx_channel, &cbs, NULL);
        if (ret != ESP_OK)
        {
            return;
        }
        vTaskDelete(NULL);
    }

    esp_err_t spdif_receiver_init(int input_pin, void (*init_done_cb)(void))
    {
        ESP_LOGI("TAG", "SPDIF Init Called");
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << input_pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);

        pcm_buffer = xRingbufferCreate(PCM_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
        if (!pcm_buffer)
        {
            return ESP_FAIL;
        }

        g_symbol_buffer = xRingbufferCreate(SYMBOL_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
        if (!g_symbol_buffer)
        {
            return ESP_FAIL;
        }

        g_rmt_buffer = heap_caps_malloc(
            RMT_MEM_BLOCK_SYMBOLS * sizeof(rmt_symbol_word_t),
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!g_rmt_buffer)
        {
            return ESP_FAIL;
        }

        if (xTaskCreatePinnedToCore(spdif_receiver_init_task, "spdif_init",
                                    DECODER_TASK_STACK, (void *)input_pin,
                                    15, &g_init_task, 0) != pdPASS)
        {
            return ESP_FAIL;
        }
        if (init_done_cb) {
            init_done_cb();
        }

        if (xTaskCreatePinnedToCore(spdif_decoder_task, "spdif_decoder",
                                    DECODER_TASK_STACK, NULL,
                                    DECODER_TASK_PRIORITY, &g_decoder_task, 1) != pdPASS)
        {
            return ESP_FAIL;
        }

        return ESP_OK;
    }

    esp_err_t spdif_receiver_start(void)
    {
        return ESP_OK;
    }

    esp_err_t spdif_receiver_stop(void)
    {
        if (g_rx_channel)
        {
            return rmt_disable(g_rx_channel);
        }
        return ESP_OK;
    }

    void spdif_receiver_deinit(void)
    {
        if (g_rx_channel)
        {
            rmt_disable(g_rx_channel);
            rmt_del_channel(g_rx_channel);
            g_rx_channel = NULL;
        }
        if (g_rmt_buffer)
        {
            heap_caps_free(g_rmt_buffer);
            g_rmt_buffer = NULL;
        }
        if (g_symbol_buffer)
        {
            vRingbufferDelete(g_symbol_buffer);
            g_symbol_buffer = NULL;
        }
    }

    uint32_t spdif_receiver_get_sample_rate(void)
    {
        // TODO
        return 0;
    }