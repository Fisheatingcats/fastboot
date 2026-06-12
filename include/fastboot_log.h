#ifndef FASTBOOT_LOG_H
#define FASTBOOT_LOG_H

#include <stdint.h>

typedef struct {
    void (*puts)(void *ctx, const char *s);
    void (*dec32)(void *ctx, const char *label, uint32_t value);
    void *ctx;
} fboot_log_t;

static inline void fboot_log_puts(const fboot_log_t *log, const char *s)
{
    if (log && log->puts) {
        log->puts(log->ctx, s);
    }
}

static inline void fboot_log_dec32(const fboot_log_t *log, const char *label,
                                   uint32_t value)
{
    if (log && log->dec32) {
        log->dec32(log->ctx, label, value);
    }
}

#endif /* FASTBOOT_LOG_H */
