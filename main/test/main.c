
#include "audio.h"
#include "network.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

int main(void) {
    init_peripherals();
    vTaskStartScheduler();
    while(1){}
    return 0;
}
