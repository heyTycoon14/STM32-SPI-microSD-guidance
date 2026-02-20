/**
 * diskio.c -- FatFs platform-dependent disk I/O module.
 * Routes all calls to sd_spi_diskio.
 */
#include "ff.h"
#include "diskio.h"
#include "sd_spi_diskio.h"

static DSTATUS Stat = STA_NOINIT;

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    Stat = SD_SPI_Init();
    return Stat;
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    return Stat;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;
    return SD_SPI_ReadSectors(buff, sector, count);
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;
    if (Stat & STA_PROTECT) return RES_WRPRT;
    return SD_SPI_WriteSectors(buff, sector, count);
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;
    return SD_SPI_Ioctl(cmd, buff);
}

/* Returns a fixed timestamp (2025-01-01 00:00:00).
 * Integrate an RTC and replace this function if accurate file timestamps
 * are required. */
DWORD get_fattime(void)
{
    return ((DWORD)(2025 - 1980) << 25)
         | ((DWORD)1  << 21)
         | ((DWORD)1  << 16)
         | ((DWORD)0  << 11)
         | ((DWORD)0  << 5)
         | ((DWORD)0  >> 1);
}
