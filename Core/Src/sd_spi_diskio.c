/**
 * sd_spi_diskio.c
 * Low-level SPI SD Card disk I/O driver for FatFs on STM32 HAL.
 *
 * Timing model:
 *  - Init phase : SPI clock <= 400 kHz (SD spec requirement)
 *  - Data phase : SPI clock <= 25 MHz (SPI mode) or card-specific max
 *
 * Signal integrity notes:
 *  - Keep SPI traces short (<5 cm ideally)
 *  - 10k pull-up on MISO is mandatory (card releases bus as open-drain)
 *  - 33-100 Ohm series resistors on SCK/MOSI reduce ringing at high speed
 *  - Decouple card VDD with 100 nF + 10 uF close to card connector
 */

#include "sd_spi_diskio.h"
#include "diskio.h"
#include "stm32xxxx_hal.h"  /* Replace xxxx with your series: f4, l4, h7, etc. */
#include <string.h>

/* ---- USER CONFIG ---------------------------------------------------------- */
extern SPI_HandleTypeDef hspi1;          /* SPI handle from CubeMX            */
#define SD_SPI              (&hspi1)

#define SD_CS_PORT          GPIOA
#define SD_CS_PIN           GPIO_PIN_4

/* SPI prescalers: adapt to your APB clock to hit target frequencies.
 * Example: APB1 = 42 MHz  ->  /128 = 328 kHz (init),  /4 = 10.5 MHz (data) */
#define SPI_BAUDRATEPRESCALER_INIT   SPI_BAUDRATEPRESCALER_128
#define SPI_BAUDRATEPRESCALER_DATA   SPI_BAUDRATEPRESCALER_4

#define SD_TIMEOUT_MS       500          /* Per-command watchdog (ms)          */
#define SD_INIT_RETRY       10           /* ACMD41 poll retries during init    */
/* ---- END USER CONFIG ------------------------------------------------------ */

#define CMD0    (0x40 | 0)
#define CMD1    (0x40 | 1)
#define CMD8    (0x40 | 8)
#define CMD9    (0x40 | 9)
#define CMD10   (0x40 | 10)
#define CMD12   (0x40 | 12)
#define CMD16   (0x40 | 16)
#define CMD17   (0x40 | 17)
#define CMD18   (0x40 | 18)
#define CMD24   (0x40 | 24)
#define CMD25   (0x40 | 25)
#define CMD41   (0x40 | 41)
#define CMD55   (0x40 | 55)
#define CMD58   (0x40 | 58)

#define SD_DATA_TOKEN           0xFE
#define SD_MULTI_DATA_TOKEN     0xFC
#define SD_STOP_TOKEN           0xFD
#define SD_R1_IDLE              0x01
#define SD_R1_ILLEGAL_CMD       0x04

static uint8_t CardType;
#define CT_MMC   0x01
#define CT_SD1   0x02
#define CT_SD2   0x04
#define CT_BLOCK 0x08   /* Block-addressed (SDHC/SDXC) */

static void SPI_SetSpeed(uint32_t prescaler)
{
    SD_SPI->Init.BaudRatePrescaler = prescaler;
    HAL_SPI_Init(SD_SPI);
}

static inline void CS_Assert(void)
{
    HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_RESET);
}

static inline void CS_Deassert(void)
{
    HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_SET);
}

static uint8_t SPI_TxRx(uint8_t data)
{
    uint8_t rx = 0xFF;
    HAL_SPI_TransmitReceive(SD_SPI, &data, &rx, 1, SD_TIMEOUT_MS);
    return rx;
}

static uint8_t WaitReady(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint8_t  res;
    do {
        res = SPI_TxRx(0xFF);
    } while (res != 0xFF && (HAL_GetTick() - start) < timeout_ms);
    return res;
}

static uint8_t SendCmd(uint8_t cmd, uint32_t arg)
{
    uint8_t crc = 0x01;
    if (cmd == CMD0) crc = 0x95;
    if (cmd == CMD8) crc = 0x87;

    WaitReady(SD_TIMEOUT_MS);
    CS_Assert();
    SPI_TxRx(0xFF);

    SPI_TxRx(cmd);
    SPI_TxRx((uint8_t)(arg >> 24));
    SPI_TxRx((uint8_t)(arg >> 16));
    SPI_TxRx((uint8_t)(arg >> 8));
    SPI_TxRx((uint8_t)(arg));
    SPI_TxRx(crc);

    if (cmd == CMD12) SPI_TxRx(0xFF);

    uint8_t r1 = 0xFF;
    for (int retry = 8; retry; retry--) {
        r1 = SPI_TxRx(0xFF);
        if (!(r1 & 0x80)) break;
    }
    return r1;
}

DSTATUS SD_SPI_Init(void)
{
    CardType = 0;

    SPI_SetSpeed(SPI_BAUDRATEPRESCALER_INIT);
    CS_Deassert();
    HAL_Delay(10);
    for (int i = 0; i < 10; i++) SPI_TxRx(0xFF); /* 80 dummy clocks */

    if (SendCmd(CMD0, 0) != SD_R1_IDLE) {
        CS_Deassert(); SPI_TxRx(0xFF);
        return STA_NOINIT;
    }

    uint8_t ocr[4];
    if (SendCmd(CMD8, 0x1AA) == SD_R1_IDLE) {
        for (int i = 0; i < 4; i++) ocr[i] = SPI_TxRx(0xFF);
        CS_Deassert(); SPI_TxRx(0xFF);

        if (ocr[2] == 0x01 && ocr[3] == 0xAA) {
            uint32_t t = HAL_GetTick();
            while (HAL_GetTick() - t < 1000) {
                if (SendCmd(CMD55, 0) <= 1 &&
                    SendCmd(CMD41, 0x40000000) == 0) {
                    CS_Deassert(); SPI_TxRx(0xFF);
                    break;
                }
                CS_Deassert(); SPI_TxRx(0xFF);
            }
            if (SendCmd(CMD58, 0) == 0) {
                for (int i = 0; i < 4; i++) ocr[i] = SPI_TxRx(0xFF);
                CardType = (ocr[0] & 0x40) ? CT_SD2 | CT_BLOCK : CT_SD2;
            }
            CS_Deassert(); SPI_TxRx(0xFF);
        }
    } else {
        CS_Deassert(); SPI_TxRx(0xFF);
        uint8_t cmd;
        if (SendCmd(CMD55, 0) <= 1 && SendCmd(CMD41, 0) <= 1) {
            CardType = CT_SD1; cmd = CMD41;
        } else {
            CardType = CT_MMC; cmd = CMD1;
        }
        CS_Deassert(); SPI_TxRx(0xFF);

        uint32_t t = HAL_GetTick();
        while (HAL_GetTick() - t < 1000) {
            if (CardType == CT_SD1) SendCmd(CMD55, 0), CS_Deassert();
            if (SendCmd(cmd, 0) == 0) { CS_Deassert(); SPI_TxRx(0xFF); break; }
            CS_Deassert(); SPI_TxRx(0xFF);
        }
        if (!(CardType & CT_BLOCK)) {
            if (SendCmd(CMD16, 512) != 0) CardType = 0;
            CS_Deassert(); SPI_TxRx(0xFF);
        }
    }

    if (CardType == 0) return STA_NOINIT;

    SPI_SetSpeed(SPI_BAUDRATEPRESCALER_DATA);
    return 0;
}

DRESULT SD_SPI_ReadSectors(BYTE *buff, LBA_t sector, UINT count)
{
    if (!buff || count == 0) return RES_PARERR;
    if (!(CardType & CT_BLOCK)) sector <<= 9;

    DRESULT res = RES_ERROR;

    if (count == 1) {
        if (SendCmd(CMD17, sector) == 0) {
            uint32_t t = HAL_GetTick();
            uint8_t token;
            do { token = SPI_TxRx(0xFF); }
            while (token == 0xFF && (HAL_GetTick() - t) < SD_TIMEOUT_MS);

            if (token == SD_DATA_TOKEN) {
                HAL_SPI_Receive(SD_SPI, buff, 512, SD_TIMEOUT_MS);
                SPI_TxRx(0xFF); SPI_TxRx(0xFF); /* discard CRC */
                res = RES_OK;
            }
        }
    } else {
        if (SendCmd(CMD18, sector) == 0) {
            while (count--) {
                uint32_t t = HAL_GetTick();
                uint8_t token;
                do { token = SPI_TxRx(0xFF); }
                while (token == 0xFF && (HAL_GetTick() - t) < SD_TIMEOUT_MS);

                if (token != SD_DATA_TOKEN) { res = RES_ERROR; break; }
                HAL_SPI_Receive(SD_SPI, buff, 512, SD_TIMEOUT_MS);
                SPI_TxRx(0xFF); SPI_TxRx(0xFF);
                buff += 512;
                if (count == 0) res = RES_OK;
            }
            SendCmd(CMD12, 0);
        }
    }

    CS_Deassert();
    SPI_TxRx(0xFF);
    return res;
}

DRESULT SD_SPI_WriteSectors(const BYTE *buff, LBA_t sector, UINT count)
{
    if (!buff || count == 0) return RES_PARERR;
    if (!(CardType & CT_BLOCK)) sector <<= 9;

    DRESULT res = RES_ERROR;

    if (count == 1) {
        if (SendCmd(CMD24, sector) == 0) {
            SPI_TxRx(0xFF);
            SPI_TxRx(SD_DATA_TOKEN);
            HAL_SPI_Transmit(SD_SPI, (uint8_t *)buff, 512, SD_TIMEOUT_MS);
            SPI_TxRx(0xFF); SPI_TxRx(0xFF); /* dummy CRC */

            uint8_t dr = SPI_TxRx(0xFF) & 0x1F;
            if (dr == 0x05) {
                if (WaitReady(SD_TIMEOUT_MS) == 0xFF) res = RES_OK;
            }
        }
    } else {
        if (SendCmd(CMD25, sector) == 0) {
            while (count--) {
                if (WaitReady(SD_TIMEOUT_MS) != 0xFF) break;
                SPI_TxRx(SD_MULTI_DATA_TOKEN);
                HAL_SPI_Transmit(SD_SPI, (uint8_t *)buff, 512, SD_TIMEOUT_MS);
                SPI_TxRx(0xFF); SPI_TxRx(0xFF);
                uint8_t dr = SPI_TxRx(0xFF) & 0x1F;
                if (dr != 0x05) break;
                buff += 512;
                if (count == 0) res = RES_OK;
            }
            SPI_TxRx(SD_STOP_TOKEN);
            SPI_TxRx(0xFF);
            WaitReady(SD_TIMEOUT_MS);
        }
    }

    CS_Deassert();
    SPI_TxRx(0xFF);
    return res;
}

DRESULT SD_SPI_Ioctl(BYTE cmd, void *buff)
{
    DRESULT res = RES_ERROR;

    switch (cmd) {
    case CTRL_SYNC:
        CS_Assert();
        if (WaitReady(SD_TIMEOUT_MS) == 0xFF) res = RES_OK;
        CS_Deassert();
        break;

    case GET_SECTOR_COUNT: {
        if (SendCmd(CMD9, 0) == 0) {
            uint8_t csd[16];
            uint32_t t = HAL_GetTick();
            uint8_t token;
            do { token = SPI_TxRx(0xFF); }
            while (token == 0xFF && (HAL_GetTick() - t) < SD_TIMEOUT_MS);
            if (token == SD_DATA_TOKEN) {
                for (int i = 0; i < 16; i++) csd[i] = SPI_TxRx(0xFF);
                SPI_TxRx(0xFF); SPI_TxRx(0xFF);
                LBA_t csize;
                if ((csd[0] >> 6) == 1) {
                    /* CSD v2.0 (SDHC/SDXC) */
                    csize = (uint32_t)(csd[7] & 0x3F) << 16 |
                            (uint32_t)csd[8] << 8 | csd[9];
                    *(LBA_t *)buff = (csize + 1) << 10;
                } else {
                    /* CSD v1.0 (SDSC) */
                    uint32_t n = (csd[5] & 0x0F) +
                                 ((csd[10] & 0x80) >> 7) +
                                 ((csd[9]  & 0x03) << 1) + 2;
                    csize = ((uint32_t)(csd[6] & 0x03) << 10) |
                             (uint32_t)csd[7] << 2 |
                             (csd[8] >> 6);
                    *(LBA_t *)buff = (csize + 1) << (n - 9);
                }
                res = RES_OK;
            }
        }
        CS_Deassert(); SPI_TxRx(0xFF);
        break;
    }

    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        res = RES_OK;
        break;

    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 128;
        res = RES_OK;
        break;

    default:
        res = RES_PARERR;
        break;
    }
    return res;
}
