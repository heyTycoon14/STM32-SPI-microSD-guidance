# STM32 HAL + SPI MicroSD + FatFs Integration Guide

## Prerequisites
- STM32CubeIDE >= 1.13
- STM32 HAL drivers for your target MCU
- ChaN FatFs (included via CubeMX Middleware tab)

---

## Step 1 - CubeMX / .ioc Setup

### SPI Peripheral
| Setting | Value |
|---------|-------|
| Mode | Full-Duplex Master |
| Data Size | 8 bits |
| First Bit | MSB First |
| CPOL | Low |
| CPHA | 1 Edge (Mode 0) |
| NSS | Disabled (software CS) |
| Baud Rate | Start low (~328 kHz for init) |

SD cards run in SPI Mode 0 (CPOL=0, CPHA=0). Some cards also accept Mode 3 - Mode 0 is universally safe.

### CS GPIO
- Set the CS pin as GPIO_Output, default HIGH (card deselected).
- Label it SD_CS in CubeMX for clarity.

### FatFs Middleware
1. Middleware -> FatFs -> Enable
2. Set Interface to User-defined
3. Ensure _USE_WRITE = 1, _USE_IOCTL = 1
4. Set FF_MAX_SS = 512, FF_MIN_SS = 512
5. Optionally enable FF_USE_STRFUNC for f_printf / f_gets

---

## Step 2 - Add Source Files

Copy into your project:
- Core/Src/sd_spi_diskio.c
- Core/Inc/sd_spi_diskio.h
- Core/Src/fatfs_app.c
- Core/Inc/fatfs_app.h
- Replace the generated diskio.c body with the provided version.

Edit the USER CONFIG section in sd_spi_diskio.c:

```c
extern SPI_HandleTypeDef hspi1;  // match your CubeMX SPI instance
#define SD_CS_PORT  GPIOA         // your CS GPIO port
#define SD_CS_PIN   GPIO_PIN_4    // your CS GPIO pin
```

---

## Step 3 - Call from main.c

```c
#include "fatfs_app.h"

int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_SPI1_Init();
    MX_FATFS_Init();    // Registers the FatFs driver

    HAL_Delay(100);     // Allow card power rail to stabilise

    if (SD_App_Run() == 0) {
        // Success - green LED, UART message, etc.
    } else {
        // Failure - red LED, UART error code, etc.
    }
    while (1) {}
}
```

---

## Timing and Signal Integrity Notes

### Clock Speed
| Phase | Target | Why |
|-------|--------|-----|
| Init (CMD0 to ready) | <= 400 kHz | SD spec mandatory |
| Data transfer | 8-25 MHz | Card-dependent |

Never exceed 25 MHz in SPI mode - the SD spec caps it there.

### Common Pitfalls
| Problem | Root Cause | Fix |
|---------|-----------|-----|
| CMD0 never returns 0x01 | VDD not stable / no dummy clocks | Add HAL_Delay(10) + 80 dummy clocks before CMD0 |
| Reads return 0xFF forever | No 10k pull-up on MISO | Add 10k resistor MISO to 3.3V |
| Corrupted writes | CS toggled during multi-block | Never toggle CS until CMD12/stop token sent |
| Zero-byte files | f_close() never called | Always close, even on error paths |
| Works on one card, fails on another | No ACMD41 timeout loop | Poll ACMD41 for up to 1 second |
| M7 cache coherency | D-Cache returns stale data | Align DMA buffers to 32B, invalidate after DMA RX |

### Series Resistors
At >= 10 MHz, add 33 Ohm in series on SCK and MOSI close to the MCU to dampen ringing.

---

## DMA Considerations

1. Replace HAL_SPI_TransmitReceive() calls with the DMA variant.
2. Use a binary semaphore to block until HAL_SPI_TxRxCpltCallback fires.
3. Ensure DMA buffers are in non-cached SRAM or flush/invalidate D-Cache on Cortex-M7.

```c
static volatile uint8_t spi_done;
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *h) { spi_done = 1; }

static void SPI_TxRx_DMA(uint8_t *tx, uint8_t *rx, uint16_t len) {
    spi_done = 0;
    HAL_SPI_TransmitReceive_DMA(SD_SPI, tx, rx, len);
    while (!spi_done); // Replace with xSemaphoreTake in FreeRTOS
}
```

---

## FreeRTOS Considerations

- Wrap every CS_Assert...CS_Deassert block with a mutex if multiple tasks share the SPI bus.
- Replace spinlocks with xSemaphoreTake on a binary semaphore given from the DMA callback.
- Call SD_App_Run() from a task with >= 1 KB stack.
- Never call f_mount, f_open, f_read, f_write from an ISR.

---

## Sector Alignment and FAT32 Tips

- Format the card as FAT32 with allocation unit = 32 KB for best sequential performance.
- Use f_sync(&fil) periodically during long writes to protect against power-loss corruption.
- Keep filenames to 8.3 format unless you enable FF_USE_LFN in ffconf.h.
- Write in multiples of the allocation unit (32 KB) to avoid partial-cluster penalties.

---

## Testing Checklist

- [ ] Tested on >= 3 cards (different brands/capacities)
- [ ] Tested cold start (power cycle, not just reset)
- [ ] Verified file size on PC after test
- [ ] Verified appended content in a text editor
- [ ] Pulled power mid-write and confirmed FatFs recovers on next mount
- [ ] Checked for CS floating when card is not selected (pull-up to 3.3V)

---

## Quick Reference

| Parameter | Value | Notes |
|-----------|-------|-------|
| SPI init clock | <= 400 kHz | Hard SD spec requirement |
| SPI data clock | <= 25 MHz | SPI-mode ceiling |
| Dummy clocks before CMD0 | >= 74 (use 80) | Some cards need more |
| ACMD41 timeout | up to 1000 ms | Cold-start cards are slow |
| Power-up delay | >= 1 ms (use 10 ms) | VDD stabilisation |
| MISO pull-up | 10k to 3.3V | Mandatory - card is open-drain |
| Series resistors | 33 Ohm on SCK/MOSI | For >= 10 MHz board layouts |
| Write buffer alignment (M7) | 32 bytes | D-Cache line size |
