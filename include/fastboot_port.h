#ifndef FASTBOOT_PORT_H
#define FASTBOOT_PORT_H

#include "fastboot_status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file fastboot_port.h
 * @brief Hardware abstraction port interface.
 *
 * Each platform must implement these functions to run FastBoot.
 */

/* ── Tick / Delay ────────────────────────────────────────────────────────── */

/** Return monotonic millisecond tick (wraps at 2^32). */
uint32_t fastboot_port_tick_ms(void);

/** Block for @p ms milliseconds. */
void fastboot_port_delay_ms(uint32_t ms);

/* ── Watchdog ────────────────────────────────────────────────────────────── */

/** Kick the watchdog. Called during long operations. */
void fastboot_port_feed_watchdog(void);

/* ── System ──────────────────────────────────────────────────────────────── */

/** Trigger a full system reset. Never returns. */
void fastboot_port_reset(void) __attribute__((noreturn));

/* ── GPIO (optional – used by bootloader main, not by library core) ──────── */

/** Read a button/input pin. Returns true when asserted (pressed). */
bool fastboot_port_button_pressed(void);

/** Set LED on (true) or off (false). */
void fastboot_port_led_set(bool on);

/** Toggle LED. */
void fastboot_port_led_toggle(void);

#endif /* FASTBOOT_PORT_H */
