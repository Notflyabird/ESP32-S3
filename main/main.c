#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 项目启动成功！");

    while (1) {
        printf("Hello from ESP32-S3!\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
