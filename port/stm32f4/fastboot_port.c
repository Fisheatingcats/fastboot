#include "fastboot_port.h"
#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/* ── Tick / Delay ────────────────────────────────────────────────────────── */

uint32_t fastboot_port_tick_ms(void)
{
    return HAL_GetTick();
}

void fastboot_port_delay_ms(uint32_t ms)
{
    HAL_Delay(ms);
}

/* ── Watchdog ─────────────────────────────────────────────────────────────── */

/* IWDG disabled for now – enable after UART debug is confirmed. */
void fastboot_port_feed_watchdog(void)
{
    /* no-op */
}

/* ── System ───────────────────────────────────────────────────────────────── */

void fastboot_port_reset(void)
{
    NVIC_SystemReset();
    __builtin_unreachable();
}

/* ── GPIO ─────────────────────────────────────────────────────────────────── */

#include "main.h"

bool fastboot_port_button_pressed(void)
{
    return HAL_GPIO_ReadPin(PB1_USER_KEY1_GPIO_Port,
                            PB1_USER_KEY1_Pin) == GPIO_PIN_RESET;
}

void fastboot_port_led_set(bool on)
{
    HAL_GPIO_WritePin(PC13_LED_EN_GPIO_Port, PC13_LED_EN_Pin,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void fastboot_port_led_toggle(void)
{
    HAL_GPIO_TogglePin(PC13_LED_EN_GPIO_Port, PC13_LED_EN_Pin);
}
