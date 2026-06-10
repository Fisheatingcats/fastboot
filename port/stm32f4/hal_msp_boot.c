/**
 * @file hal_msp_boot.c
 * @brief Minimal HAL MSP for the FastBoot bootloader.
 *
 * Only HAL_MspInit() is required. UART/SPI GPIO and clocks are configured
 * inline by the bootloader drivers (fastboot_uart.c / fastboot_w25q64.c),
 * so their MSP callbacks are intentionally left as HAL weak defaults.
 */
#include "main.h"

void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();

    /* PendSV at lowest priority — same as CubeMX default */
    HAL_NVIC_SetPriority(PendSV_IRQn, 15, 0);
}
