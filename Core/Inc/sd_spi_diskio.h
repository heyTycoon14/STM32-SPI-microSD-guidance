/**
 * sd_spi_diskio.h -- SD SPI disk I/O driver interface
 */
#pragma once
#include "diskio.h"

DSTATUS SD_SPI_Init(void);
DRESULT SD_SPI_ReadSectors (BYTE *buff, LBA_t sector, UINT count);
DRESULT SD_SPI_WriteSectors(const BYTE *buff, LBA_t sector, UINT count);
DRESULT SD_SPI_Ioctl(BYTE cmd, void *buff);
