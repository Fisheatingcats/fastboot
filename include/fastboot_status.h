#ifndef FASTBOOT_STATUS_H
#define FASTBOOT_STATUS_H

typedef enum {
    FB_OK = 0,
    FB_NO_UPDATE = 1,
    FB_BUSY = 2,
    FB_ERR_PARAM = -1,
    FB_ERR_RANGE = -2,
    FB_ERR_FLASH = -3,
    FB_ERR_VERIFY = -4,
    FB_ERR_FORMAT = -5,
    FB_ERR_CRC = -6,
    FB_ERR_IO = -7,
    FB_ERR_ID = -8,
} fboot_status_t;

#endif /* FASTBOOT_STATUS_H */
