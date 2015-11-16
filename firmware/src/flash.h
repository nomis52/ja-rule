/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * flash.h
 * Copyright (C) 2015 Simon Newton
 */

#ifndef BOOTLOADER_FIRMWARE_SRC_FLASH_H_
#define BOOTLOADER_FIRMWARE_SRC_FLASH_H_

#include <stdint.h>
#include <stdbool.h>

#include "peripheral/ports/plib_ports.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @{
 * @file flash.h
 * @brief Flash memory operations.
 *
 * Read / write from the flash. Only a single operation is allowed at once.
 * For simplicity we operate in sectors which for a SST25VF020B is 4k.
 */

/**
 * @brief The callback run when the flash operation completes.
 * @param ok True if the operation completed, false if it failed.
 */
typedef void (*Flash_Callback)(bool ok);

/**
 * @brief The hardware settings for the flash.
 *
 * This assumes the underlying SPI module has been initialized.
 */
typedef struct {
  PORTS_CHANNEL ce_port;  //!< The port to use for chip-enable.
  PORTS_BIT_POS ce_bit;  //!< The port bit to use for chip-enable.
  PORTS_CHANNEL hold_port;  //!< The port to use for hold.
  PORTS_BIT_POS hold_bit;  //!< The port bit to use for hold
  PORTS_CHANNEL wp_port;  //!< The port to use for write-protect.
  PORTS_BIT_POS wp_bit;  //!< The port bit to use for write-protect
} FlashHardwareSettings;

/**
 * @brief Initialize the flash module.
 */
void Flash_Initialize(FlashHardwareSettings *settings);

/**
 * @brief Return the sector-size.
 * @returns The sector size.
 */
uint32_t Flash_SectorSize();

/**
 * @brief Read data from flash.
 * @param sector The sector to read from.
 * @param input The buffer to read data to.
 * @param size The amount of data to read.
 * @param callback The callback to run once the read operation is complete.
 * @return True if the operation was started, false if another operation was in
 *   progress.
 */
bool Flash_Read(uint32_t sector, uint8_t *input, unsigned int size,
                Flash_Callback callback);

/**
 * @brief Write one sector worth of data to flash.
 * @param sector The sector to start writing at.
 * @param input The buffer to write.
 * @param size The amount of data to write.
 * @param callback The callback to run once the read operation is complete.
 * @return True if the operation was started, false if another operation was in
 *   progress or the arguments were invalid.
 */
bool Flash_Write(uint32_t sector, const uint8_t *output, unsigned int size,
                 Flash_Callback callback);

bool Flash_ReadStatus();
bool Flash_ReadStatus1();

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif  // BOOTLOADER_FIRMWARE_SRC_FLASH_H_
