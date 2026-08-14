#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "led.h"

void led_init(void)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);
}

int led_toggle(void)
{
    static int level = 0;               /* static：跨次调用保留状态 */
    level = !level;
    gpio_set_level(LED_GPIO, level);
    vTaskDelay(pdMS_TO_TICKS(BLINK_PERIOD_MS / 2));
    return level;
}
