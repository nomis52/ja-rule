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
 * flash.c
 * Copyright (C) 2015 Simon Newton
 */

#include "flash.h"
#include "spi.h"
#include "syslog.h"
#include "utils.h"

#include <stdint.h>
#include <stdbool.h>

enum {
  OP_WRITE_STATUS_REGISTER = 0x01,
  OP_BYTE_PROGRAM = 0x02,
  OP_READ = 0x03,
  OP_WRITE_DISABLE = 0x04,
  OP_STATUS_READ = 0x05,
  OP_WRITE_ENABLE = 0x06,
  OP_HS_READ = 0x0b,
  OP_SECTOR_ERASE = 0x20,
  OP_SOFTWARE_STATUS_READ = 0x35,
  OP_ENABLE_WRITE_STATUS_REGISTER = 0x50,
  OP_ENABLE_SOFTWARE_EOW = 0x70,
  OP_DISBLE_SOFTWARE_EOW = 0x80,
  OP_AUTO_INCREMENT_WRITE = 0xad
};

typedef enum {
  ACTION_IDLE,  // Idle mode
  ACTION_READ,  // Read data
  ACTION_UNLOCK,  // Unlocking flash
  ACTION_ERASE,  // Erasing flash sector
  ACTION_WRITE,  // Writing flash sector
  ACTION_STATUS_READ,
  ACTION_STATUS1_READ
} Action;

enum {
  BUSY = 0x01,
  WRITE_ENABLED = 0x02,
  BLOCK_PROTECT_0 = 0x04,
  BLOCK_PROTECT_1 = 0x08,
  AUTO_ADDRESS_INCREMENT = 0x40,
  BLOCK_PROTECT_LOCK_DOWN = 0x80
};

// The largest cmd op is the initial auto-increment command.
enum { CMD_MAX_LENGTH = 6 };

typedef struct {
  PORTS_CHANNEL ce_port;  //!< The port to use for chip-enable.
  PORTS_BIT_POS ce_bit;  //!< The port bit to use for chip-enable.
  PORTS_CHANNEL wp_port;  //!< The port to use for write-protect.
  PORTS_BIT_POS wp_bit;  //!< The port bit to use for write-protect.
  PORTS_CHANNEL hold_port;  //!< The port to use for hold.
  PORTS_BIT_POS hold_bit;  //!< The port bit to use for hold.
  uint32_t flash_sectors;  //!< The number of sectors in the flash
  bool is_unlocked;  //!< True if we've completed the unlock process.

  Action action;
  Flash_Callback callback;

  uint32_t sector;
  const uint8_t *data;
  uint32_t data_size;
} FlashState;

static FlashState g_flash_state;

static uint8_t cmd_buffer[CMD_MAX_LENGTH];

static uint8_t g_input[4];

static const uint32_t SECTOR_SIZE = 1 << 12;
static const uint32_t FLASH_SIZE = 1 << 18;

// Private Functions
// -----------------------------------------------------------------------------

void Flash_SPIComplete(SPIEventType event);

// TODO: get rid of hold?
static inline void DisableHold() {
  SysLog_Message(SYSLOG_INFO, "Disable hold");
  PLIB_PORTS_PinSet(PORTS_ID_0, g_flash_state.hold_port,
                    g_flash_state.hold_bit);
}

static inline void EnableHold() {
  SysLog_Message(SYSLOG_INFO, "Enable hold");
  PLIB_PORTS_PinClear(PORTS_ID_0, g_flash_state.hold_port,
                      g_flash_state.hold_bit);
}

static inline void DisableWP() {
  PLIB_PORTS_PinSet(PORTS_ID_0, g_flash_state.wp_port, g_flash_state.wp_port);
}

static inline void EnableWP() {
  PLIB_PORTS_PinClear(PORTS_ID_0, g_flash_state.wp_port, g_flash_state.wp_port);
}

static inline void ChipEnable() {
  PLIB_PORTS_PinClear(PORTS_ID_0, g_flash_state.ce_port, g_flash_state.ce_bit);
}

static inline void ChipDisable() {
  PLIB_PORTS_PinSet(PORTS_ID_0, g_flash_state.ce_port, g_flash_state.ce_bit);
}

static void RunCallback(bool result) {
  if (g_flash_state.callback) {
    g_flash_state.callback(result);
    g_flash_state.callback = NULL;
  }
}

static bool BeginErase() {
  g_flash_state.action = ACTION_ERASE;
  cmd_buffer[0] = OP_WRITE_ENABLE;
  return SPI_QueueTransfer(cmd_buffer, 1, NULL, 0, Flash_SPIComplete);
}

static bool SendReadStatus() {
  cmd_buffer[0] = OP_STATUS_READ;
  return SPI_QueueTransfer(cmd_buffer, 1, g_input, 1, Flash_SPIComplete);
}

void Flash_SPIComplete(SPIEventType event) {
  if (event == SPI_BEGIN_TRANSFER) {
    ChipEnable();
    return;
  }

  ChipDisable();
  bool ok = false;

  if (g_flash_state.action == ACTION_READ)  {
    g_flash_state.action = ACTION_IDLE;
    RunCallback(true);
  } else if (g_flash_state.action == ACTION_UNLOCK) {
    if (cmd_buffer[0] == OP_ENABLE_WRITE_STATUS_REGISTER) {
      SysLog_Message(SYSLOG_INFO, "OP_ENABLE_WRITE_STATUS_REGISTER done");
      cmd_buffer[0] = OP_WRITE_STATUS_REGISTER;
      cmd_buffer[1] = 0;
      ok = SPI_QueueTransfer(cmd_buffer, 2, NULL, 0, Flash_SPIComplete);
    } else if (cmd_buffer[0] == OP_WRITE_STATUS_REGISTER) {
      SysLog_Message(SYSLOG_INFO, "OP_WRITE_STATUS_REGISTER done");
      ok = SendReadStatus();
    } else {
      SysLog_Print(SYSLOG_INFO, "OP_STATUS_READ done: %d", g_input[0]);
      EnableWP();
      if (!(g_input[0] & BLOCK_PROTECT_0 || g_input[0] & BLOCK_PROTECT_1)) {
        g_flash_state.is_unlocked = true;
        ok = BeginErase();
      }
    }
    if (!ok) {
      g_flash_state.action = ACTION_IDLE;
      RunCallback(false);
    }

  } else if (g_flash_state.action == ACTION_ERASE) {
    if (cmd_buffer[0] == OP_WRITE_ENABLE) {
      SysLog_Message(SYSLOG_INFO, "OP_WRITE_ENABLE done");
      uint32_t address = g_flash_state.sector * SECTOR_SIZE;
      cmd_buffer[0] = OP_SECTOR_ERASE;
      cmd_buffer[1] = UInt32Byte1(address);
      cmd_buffer[2] = UInt32Byte2(address);
      cmd_buffer[3] = UInt32Byte3(address);
      ok = SPI_QueueTransfer(cmd_buffer, 4, NULL, 0, Flash_SPIComplete);
    } else if (cmd_buffer[0] == OP_SECTOR_ERASE) {
      SysLog_Message(SYSLOG_INFO, "OP_SECTOR_ERASE done");
      ok = SendReadStatus();
    } else if (cmd_buffer[0] == OP_STATUS_READ) {
      if (g_input[0] & BUSY) {
        ok = SendReadStatus();
      } else {
        SysLog_Message(SYSLOG_INFO, "Erase done!");
        g_flash_state.action = ACTION_WRITE;
        cmd_buffer[0] = OP_WRITE_ENABLE;
        ok = SPI_QueueTransfer(cmd_buffer, 1, NULL, 0, Flash_SPIComplete);
        /*
        g_flash_state.action = ACTION_WRITE;
        cmd_buffer[0] = OP_ENABLE_SOFTWARE_EOW;
        ok = SPI_QueueTransfer(cmd_buffer, 1, NULL, 0, Flash_SPIComplete);
        */
      }
    }
    if (!ok) {
      g_flash_state.action = ACTION_IDLE;
      RunCallback(false);
    }
  } else if (g_flash_state.action == ACTION_WRITE) {
    if (cmd_buffer[0] == OP_ENABLE_SOFTWARE_EOW) {
      SysLog_Message(SYSLOG_INFO, "OP_ENABLE_SOFTWARE_EOW done");
      cmd_buffer[0] = OP_WRITE_ENABLE;
      ok = SPI_QueueTransfer(cmd_buffer, 1, NULL, 0, Flash_SPIComplete);
    } else if (cmd_buffer[0] == OP_WRITE_ENABLE) {
      SysLog_Message(SYSLOG_INFO, "OP_WRITE_ENABLE done");

      uint32_t address = g_flash_state.sector * SECTOR_SIZE;
      cmd_buffer[0] = OP_AUTO_INCREMENT_WRITE;
      cmd_buffer[1] = UInt32Byte1(address);
      cmd_buffer[2] = UInt32Byte2(address);
      cmd_buffer[3] = UInt32Byte3(address);
      cmd_buffer[4] = g_flash_state.data[0];
      cmd_buffer[5] = g_flash_state.data[1];
      ok = SPI_QueueTransfer(cmd_buffer, 6, NULL, 0, Flash_SPIComplete);
    } else if (cmd_buffer[0] == OP_AUTO_INCREMENT_WRITE) {
      SysLog_Message(SYSLOG_INFO, "OP_AUTO_INCREMENT_WRITE done");
      /*
      cmd_buffer[0] = OP_WRITE_DISABLE;
      ok = SPI_QueueTransfer(cmd_buffer, 1, NULL, 0, Flash_SPIComplete);
      */
      cmd_buffer[0] = OP_STATUS_READ;
      ok = SPI_QueueTransfer(cmd_buffer, 1, g_input, 1, Flash_SPIComplete);
    } else if (cmd_buffer[0] == OP_WRITE_DISABLE) {
      SysLog_Message(SYSLOG_INFO, "OP_WRITE_DISABLE done");
      g_flash_state.action = ACTION_IDLE;
      RunCallback(true);

      /*
      cmd_buffer[0] = OP_DISBLE_SOFTWARE_EOW;
      ok = SPI_QueueTransfer(cmd_buffer, 1, NULL, 0, Flash_SPIComplete);
      */

    } else if (cmd_buffer[0] == OP_DISBLE_SOFTWARE_EOW) {
      SysLog_Message(SYSLOG_INFO, "OP_DISBLE_SOFTWARE_EOW done");

      cmd_buffer[0] = OP_STATUS_READ;
      g_flash_state.action = ACTION_STATUS_READ;
      SPI_QueueTransfer(cmd_buffer, 1, g_input, 1, Flash_SPIComplete);
    } else if (cmd_buffer[0] == OP_STATUS_READ) {
      if (g_input[0] & BUSY) {
        ok = SendReadStatus();
      } else {
        SysLog_Message(SYSLOG_INFO, "Write done!");

        cmd_buffer[0] = OP_WRITE_DISABLE;
        ok = SPI_QueueTransfer(cmd_buffer, 1, NULL, 0, Flash_SPIComplete);
      }
    }
  } else if (g_flash_state.action == ACTION_STATUS_READ) {
    SysLog_Print(SYSLOG_INFO, "Status %d", g_input[0]);
    g_flash_state.action = ACTION_IDLE;
  } else if (g_flash_state.action == ACTION_STATUS1_READ) {
    SysLog_Print(SYSLOG_INFO, "Status1 %d", g_input[0]);
    g_flash_state.action = ACTION_IDLE;
  }
}

// Public Functions
// -----------------------------------------------------------------------------
void Flash_Initialize(FlashHardwareSettings *settings) {
  g_flash_state.ce_port = settings->ce_port;
  g_flash_state.ce_bit = settings->ce_bit;
  g_flash_state.wp_port = settings->wp_port;
  g_flash_state.wp_bit = settings->wp_bit;
  g_flash_state.hold_port = settings->hold_port;
  g_flash_state.hold_bit = settings->hold_bit;
  g_flash_state.flash_sectors = FLASH_SIZE / SECTOR_SIZE;
  g_flash_state.is_unlocked = false;

  g_flash_state.action = ACTION_IDLE;
  g_flash_state.callback = NULL;

  // TODO(simon): remove this in favor of harmony configs.
  PLIB_PORTS_PinDirectionOutputSet(PORTS_ID_0, settings->ce_port,
                                   settings->ce_bit);
  PLIB_PORTS_PinDirectionOutputSet(PORTS_ID_0, settings->hold_port,
                                   settings->hold_bit);
  PLIB_PORTS_PinDirectionOutputSet(PORTS_ID_0, settings->wp_port,
                                   settings->wp_bit);

  PLIB_PORTS_PinSet(PORTS_ID_0, settings->ce_port, settings->ce_bit);
  // Disable write
  DisableHold();
  EnableWP();
}

uint32_t Flash_SectorSize() {
  return SECTOR_SIZE;
}

bool Flash_Read(uint32_t sector, uint8_t *input, unsigned int size,
                Flash_Callback callback) {
  if (g_flash_state.action != ACTION_IDLE ||
      sector >= g_flash_state.flash_sectors) {
    return false;
  }

  uint32_t address = sector * SECTOR_SIZE;
  cmd_buffer[0] = OP_READ;
  cmd_buffer[1] = UInt32Byte1(address);
  cmd_buffer[2] = UInt32Byte2(address);
  cmd_buffer[3] = UInt32Byte3(address);
  g_flash_state.callback = callback;
  g_flash_state.action = ACTION_READ;

  bool ok = SPI_QueueTransfer(cmd_buffer, 4, input, size, Flash_SPIComplete);
  if (!ok) {
    g_flash_state.action = ACTION_IDLE;
  }
  return ok;
}

bool Flash_Write(uint32_t sector, const uint8_t *output, unsigned int size,
                 Flash_Callback callback) {
  if (g_flash_state.action != ACTION_IDLE ||
      sector >= g_flash_state.flash_sectors ||
      size > SECTOR_SIZE) {
    return false;
  }

  g_flash_state.sector = sector;
  g_flash_state.data = output;
  g_flash_state.data_size = size;

  bool ok = false;
  if (g_flash_state.is_unlocked) {
    ok = BeginErase();
  } else {
    SysLog_Message(SYSLOG_INFO, "Will unlock");
    DisableWP();
    cmd_buffer[0] = OP_ENABLE_WRITE_STATUS_REGISTER;
    g_flash_state.action = ACTION_UNLOCK;
    ok = SPI_QueueTransfer(cmd_buffer, 1, NULL, 0, Flash_SPIComplete);
  }
  if (!ok) {
    g_flash_state.action = ACTION_IDLE;
  }
  return ok;

}

// TODO: Remove these
// ----------------------------------------------------------------------------

bool Flash_ReadStatus() {
  if (g_flash_state.action != ACTION_IDLE) {
    return false;
  }

  cmd_buffer[0] = OP_STATUS_READ;
  g_flash_state.action = ACTION_STATUS_READ;

  return SPI_QueueTransfer(cmd_buffer, 1, g_input, 1, Flash_SPIComplete);
}

bool Flash_ReadStatus1() {
  if (g_flash_state.action != ACTION_IDLE) {
    return false;
  }

  cmd_buffer[0] = OP_SOFTWARE_STATUS_READ;
  g_flash_state.action = ACTION_STATUS1_READ;

  return SPI_QueueTransfer(cmd_buffer, 1, g_input, 1, Flash_SPIComplete);
}
