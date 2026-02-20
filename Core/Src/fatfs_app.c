/**
 * fatfs_app.c -- Demo: mount, create, write, read, append, unmount.
 *
 * Call SD_App_Run() from main() after all peripherals are initialised.
 * Returns 0 on full success, negative error code otherwise.
 */
#include "fatfs_app.h"
#include "ff.h"
#include <string.h>
#include <stdio.h>

#define BUF_SIZE 512

/* Align to 32 bytes for Cortex-M7 D-Cache line size (harmless on M4/M0) */
__attribute__((aligned(32))) static uint8_t wbuf[BUF_SIZE];
__attribute__((aligned(32))) static uint8_t rbuf[BUF_SIZE];

static FATFS fs;
static FIL   fil;
static char  path[] = "0:";

static void log_err(const char *ctx, FRESULT fr)
{
    (void)ctx; (void)fr;
    /* Uncomment to enable UART debug output:
     * printf("[SD] %s failed: %d\r\n", ctx, fr); */
}

int SD_App_Run(void)
{
    FRESULT fr;
    UINT    bw, br;

    /* 1. Mount */
    fr = f_mount(&fs, path, 1);
    if (fr != FR_OK) { log_err("f_mount", fr); return -1; }

    /* 2. Create and write */
    fr = f_open(&fil, "0:TEST.TXT", FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) { log_err("f_open(write)", fr); return -2; }

    memset(wbuf, 0, BUF_SIZE);
    snprintf((char *)wbuf, BUF_SIZE,
             "STM32 SD Test\r\nLine 1 of data\r\nLine 2 of data\r\n");

    fr = f_write(&fil, wbuf, strlen((char *)wbuf), &bw);
    if (fr != FR_OK || bw == 0) { log_err("f_write", fr); f_close(&fil); return -3; }
    f_close(&fil); /* Always close -- flushes directory entry */

    /* 3. Read back and verify */
    fr = f_open(&fil, "0:TEST.TXT", FA_READ);
    if (fr != FR_OK) { log_err("f_open(read)", fr); return -4; }

    fr = f_read(&fil, rbuf, BUF_SIZE, &br);
    if (fr != FR_OK) { log_err("f_read", fr); f_close(&fil); return -5; }
    f_close(&fil);

    if (memcmp(wbuf, rbuf, bw) != 0) return -6; /* Content mismatch */

    /* 4. Append */
    fr = f_open(&fil, "0:TEST.TXT", FA_OPEN_APPEND | FA_WRITE);
    if (fr != FR_OK) { log_err("f_open(append)", fr); return -7; }

    const char *append_data = "Appended line\r\n";
    fr = f_write(&fil, append_data, strlen(append_data), &bw);
    if (fr != FR_OK || bw == 0) { log_err("f_write(append)", fr); f_close(&fil); return -8; }
    f_close(&fil);

    /* 5. Unmount */
    f_mount(NULL, path, 0);

    return 0; /* Success */
}
