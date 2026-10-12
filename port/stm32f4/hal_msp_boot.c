/**
 * @file hal_msp_boot.c
 * @brief Minimal HAL MSP for the FastBoot bootloader.
 *
 * Only HAL_MspInit() is required. UART/SPI GPIO and clocks are configured
 * inline by the bootloader drivers (fastboot_uart.c / fastboot_w25q64.c),
 * so their MSP callbacks are intentionally left as HAL weak defaults.
 */
#include "main.h"

/**
 * @brief 初始化 HAL 全局时钟依赖并设置 PendSV 为最低优先级。
 * @note 由 HAL_Init 调用；UART/SPI 的 GPIO 和时钟由各端口驱动初始化。
 */
void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();

    /* PendSV at lowest priority — same as CubeMX default */
    HAL_NVIC_SetPriority(PendSV_IRQn, 15, 0);
}
