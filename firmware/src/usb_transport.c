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
 * usb_transport.c
 * Copyright (C) 2015 Simon Newton
 */

#include "usb_transport.h"

#include <string.h>

#include "bootloader_options.h"
#include "constants.h"
#include "dfu_properties.h"
#include "dfu_spec.h"
#include "flags.h"
#include "reset.h"
#include "stream_decoder.h"
#include "system_config.h"
#include "system_definitions.h"
#include "system_pipeline.h"
#include "transport.h"
#include "usb/usb_device.h"
#include "utils.h"

#include "syslog.h"

typedef enum {
  USB_STATE_INIT = 0,
  USB_STATE_WAIT_FOR_CONFIGURATION,
  USB_STATE_MAIN_TASK,
  USB_STATE_ERROR
} USBTransportState;

typedef struct {
  TransportRxFunction rx_cb;
  USB_DEVICE_HANDLE usb_device;  //!< The USB Device layer handle.
  USBTransportState state;
  bool is_configured;  //!< Keep track of whether the device is configured.

  uint16_t tx_data_pending;
  bool tx_in_progress;
  // bool tx_in_progress;  //!< True if there is a TX in progress
  bool rx_in_progress;  //!< True if there is a RX in progress.
  bool dfu_detach;  //!< True if we've received a DFU detach.

  /*
  USB_DEVICE_TRANSFER_HANDLE write_transfer;
  USB_DEVICE_TRANSFER_HANDLE read_transfer;
  */

  /* The transmit endpoint address */
  USB_ENDPOINT_ADDRESS tx_endpoint;

  /* The receive endpoint address */
  USB_ENDPOINT_ADDRESS rx_endpoint;

  /* Tracks the alternate setting */
  uint8_t altSetting;

  int rx_data_size;
} USBTransportData;

USBTransportData g_usb_transport_data;

// Receive data buffer
uint8_t receivedDataBuffer[USB_READ_BUFFER_SIZE];

// Transmit data buffer
uint8_t transmitDataBuffer[USB_READ_BUFFER_SIZE];

// The buffer that holds the DFU Status response.
static uint8_t g_status_response[GET_STATUS_RESPONSE_SIZE];

static inline void DFUGetStatus() {
  g_status_response[0] = DFU_STATUS_OK;
  g_status_response[1] = 0u;
  g_status_response[2] = 0u;
  g_status_response[3] = 0u;
  g_status_response[4] = APP_STATE_IDLE;
  g_status_response[5] = 0u;

  USB_DEVICE_ControlSend(g_usb_transport_data.usb_device,
                         g_status_response,
                         GET_STATUS_RESPONSE_SIZE);
}

/**
 * @brief Called when device events occur.
 * @param event The type of event
 * @param event_data Data associated with the event
 * @param context The context (unused).
 */
void USBTransport_EventHandler(USB_DEVICE_EVENT event, void* event_data,
                               uintptr_t context) {
  uint8_t* configurationValue;
  USB_SETUP_PACKET* setup_packet;

  switch (event) {
    case USB_DEVICE_EVENT_RESET:
    case USB_DEVICE_EVENT_DECONFIGURED:
      g_usb_transport_data.is_configured = false;
      break;

    case USB_DEVICE_EVENT_CONFIGURED:
      // Check the configuration
      configurationValue = (uint8_t*) event_data;
      if (*configurationValue == 1) {
        // Reset endpoint data send & receive flag
        g_usb_transport_data.is_configured = true;
      }
      break;

    case USB_DEVICE_EVENT_SUSPENDED:
      break;

    case USB_DEVICE_EVENT_POWER_DETECTED:
      /* VBUS is detected. Attach the device */
      USB_DEVICE_Attach(g_usb_transport_data.usb_device);
      break;

    case USB_DEVICE_EVENT_POWER_REMOVED:
      /* VBUS is removed. Detach the device */
      USB_DEVICE_Detach(g_usb_transport_data.usb_device);
      break;

    case USB_DEVICE_EVENT_CONTROL_TRANSFER_SETUP_REQUEST:
      // This means we have received a setup packet
      setup_packet = (USB_SETUP_PACKET*) event_data;
      if (setup_packet->RequestType == USB_SETUP_REQUEST_TYPE_CLASS &&
          setup_packet->Recipient == USB_SETUP_REQUEST_RECIPIENT_INTERFACE &&
          setup_packet->DataDir == USB_SETUP_REQUEST_DIRECTION_HOST_TO_DEVICE &&
          setup_packet->bRequest == DFU_DETATCH &&
          setup_packet->wIndex == RUNTIME_MODE_DFU_INTERFACE_INDEX) {
        g_usb_transport_data.dfu_detach = true;
        USB_DEVICE_ControlStatus(g_usb_transport_data.usb_device,
                                 USB_DEVICE_CONTROL_STATUS_OK);
      } else if (setup_packet->RequestType == USB_SETUP_REQUEST_TYPE_CLASS &&
          setup_packet->Recipient == USB_SETUP_REQUEST_RECIPIENT_INTERFACE &&
          setup_packet->DataDir == USB_SETUP_REQUEST_DIRECTION_DEVICE_TO_HOST &&
          setup_packet->bRequest == DFU_GETSTATUS &&
          setup_packet->wIndex == RUNTIME_MODE_DFU_INTERFACE_INDEX &&
          setup_packet->wLength == GET_STATUS_RESPONSE_SIZE) {
        // We don't have to support GET_STATUS here but 0.7 of dfu-util won't
        // work without it.
        DFUGetStatus();
      } else if (setup_packet->bRequest == USB_REQUEST_SET_INTERFACE) {
        /* If we have got the SET_INTERFACE request, we just acknowledge
         for now. This demo has only one alternate setting which is already
         active. */
        USB_DEVICE_ControlStatus(g_usb_transport_data.usb_device,
                                 USB_DEVICE_CONTROL_STATUS_OK);
      } else if (setup_packet->bRequest == USB_REQUEST_GET_INTERFACE) {
        /* We have only one alternate setting and this setting 0. So
         * we send this information to the host. */
        USB_DEVICE_ControlSend(g_usb_transport_data.usb_device,
                               &g_usb_transport_data.altSetting, 1);
      } else if (setup_packet->bRequest == 0x20) {
        // in
        g_usb_transport_data.rx_data_size = setup_packet->wLength;
        g_usb_transport_data.rx_in_progress = true;
        USB_DEVICE_ControlReceive(g_usb_transport_data.usb_device,
                                  receivedDataBuffer,
                                  setup_packet->wLength);
      } else if (setup_packet->bRequest == 0x21) {
        g_usb_transport_data.tx_in_progress = true;
        
        /*
        if (g_usb_transport_data.tx_data_pending) {
          USB_DEVICE_ControlSend(g_usb_transport_data.usb_device,
                                 transmitDataBuffer,
                                 g_usb_transport_data.tx_data_pending);
        } else {
            USB_DEVICE_ControlStatus(g_usb_transport_data.usb_device,
                                     USB_DEVICE_CONTROL_STATUS_ERROR);
        }
        */
      } else {
        // Unknown request.
        USB_DEVICE_ControlStatus(g_usb_transport_data.usb_device,
                                 USB_DEVICE_CONTROL_STATUS_ERROR);
      }
      break;

    case USB_DEVICE_EVENT_ENDPOINT_READ_COMPLETE:
      // Endpoint read is complete
      /*
      g_usb_transport_data.rx_in_progress = false;
      g_usb_transport_data.rx_data_size =
          ((USB_DEVICE_EVENT_DATA_ENDPOINT_WRITE_COMPLETE*) event_data)->length;
      */
      break;

    case USB_DEVICE_EVENT_ENDPOINT_WRITE_COMPLETE:
      // Endpoint write is complete
      // g_usb_transport_data.tx_in_progress = false;
      break;
    case USB_DEVICE_EVENT_CONTROL_TRANSFER_DATA_RECEIVED:
      BSP_LEDToggle(BSP_LED_2);;
      USB_DEVICE_ControlStatus(g_usb_transport_data.usb_device,
                               USB_DEVICE_CONTROL_STATUS_OK);
      g_usb_transport_data.rx_in_progress = false;
      break;
    case USB_DEVICE_EVENT_CONTROL_TRANSFER_DATA_SENT:
      g_usb_transport_data.tx_data_pending = 0u;
      break;

    case USB_DEVICE_EVENT_CONTROL_TRANSFER_ABORTED:
      g_usb_transport_data.tx_in_progress = false;

    case USB_DEVICE_EVENT_RESUMED:
    case USB_DEVICE_EVENT_ERROR:
    default:
      break;
  }
  (void) context;
}

void USBTransport_Initialize(TransportRxFunction rx_cb) {
  g_usb_transport_data.rx_cb = rx_cb;
  g_usb_transport_data.state = USB_STATE_INIT;
  g_usb_transport_data.usb_device = USB_DEVICE_HANDLE_INVALID;
  g_usb_transport_data.is_configured = false;
  g_usb_transport_data.rx_endpoint = 0x01;
  g_usb_transport_data.tx_endpoint = 0x81;
  g_usb_transport_data.rx_in_progress = false;
  g_usb_transport_data.tx_in_progress = false;
  // g_usb_transport_data.tx_in_progress = false;
  g_usb_transport_data.tx_data_pending = 0u;
  g_usb_transport_data.dfu_detach = false;
  g_usb_transport_data.altSetting = 0;
  g_usb_transport_data.rx_data_size = 0;
}

void USBTransport_Tasks() {
  uint16_t endpointSize = 64;
  switch (g_usb_transport_data.state) {
    case USB_STATE_INIT:
      // Try to open the device layer.
      g_usb_transport_data.usb_device = USB_DEVICE_Open(
          USB_DEVICE_INDEX_0, DRV_IO_INTENT_READWRITE);
      if (g_usb_transport_data.usb_device != USB_DEVICE_HANDLE_INVALID) {
        // Register a callback with device layer to get event notification
        // for end point 0.
        USB_DEVICE_EventHandlerSet(g_usb_transport_data.usb_device,
                                   USBTransport_EventHandler, 0);

        g_usb_transport_data.state = USB_STATE_WAIT_FOR_CONFIGURATION;
      } else {
        // The Device Layer is not ready yet. Try again later.
      }
      break;

    case USB_STATE_WAIT_FOR_CONFIGURATION:
      if (g_usb_transport_data.is_configured == true) {
        if (USB_DEVICE_ActiveSpeedGet(g_usb_transport_data.usb_device)
             == USB_SPEED_FULL) {
          endpointSize = 64;
        } else if (USB_DEVICE_ActiveSpeedGet(g_usb_transport_data.usb_device)
                   == USB_SPEED_HIGH) {
          endpointSize = 512;
        }
        if (USB_DEVICE_EndpointIsEnabled(g_usb_transport_data.usb_device,
                                         g_usb_transport_data.rx_endpoint)
            == false) {
          // Enable Read Endpoint
          USB_DEVICE_EndpointEnable(g_usb_transport_data.usb_device, 0,
                                    g_usb_transport_data.rx_endpoint,
                                    USB_TRANSFER_TYPE_BULK, endpointSize);
        }
        if (USB_DEVICE_EndpointIsEnabled(g_usb_transport_data.usb_device,
                                         g_usb_transport_data.tx_endpoint)
            == false) {
          // Enable Write Endpoint
          USB_DEVICE_EndpointEnable(g_usb_transport_data.usb_device, 0,
                                    g_usb_transport_data.tx_endpoint,
                                    USB_TRANSFER_TYPE_BULK, endpointSize);
        }

        // Indicate that we are waiting for read
        /*
        g_usb_transport_data.rx_in_progress = true;

        // Place a new read request.
        USB_DEVICE_RESULT result = USB_DEVICE_EndpointRead(
            g_usb_transport_data.usb_device,
            &g_usb_transport_data.read_transfer,
            g_usb_transport_data.rx_endpoint,
            &receivedDataBuffer[0],
            sizeof(receivedDataBuffer));

        (void) result;
        */

        // Device is ready to run the main task
        g_usb_transport_data.state = USB_STATE_MAIN_TASK;
      }
      break;

    case USB_STATE_MAIN_TASK:
      if (g_usb_transport_data.dfu_detach) {
        BootloaderOptions_SetBootOption(BOOT_BOOTLOADER);
        Reset_SoftReset();
      }

      if (!g_usb_transport_data.is_configured) {
        /* This means the device got deconfigured. Change the
         * application state back to waiting for configuration. */
        g_usb_transport_data.state = USB_STATE_WAIT_FOR_CONFIGURATION;

        // Disable the endpoints
        USB_DEVICE_EndpointDisable(g_usb_transport_data.usb_device,
                                   g_usb_transport_data.rx_endpoint);
        USB_DEVICE_EndpointDisable(g_usb_transport_data.usb_device,
                                   g_usb_transport_data.tx_endpoint);
        g_usb_transport_data.rx_in_progress = false;
        // g_usb_transport_data.tx_in_progress = false;
      } else if (g_usb_transport_data.rx_in_progress == false) {

        if (g_usb_transport_data.rx_in_progress == false &&
            g_usb_transport_data.rx_data_size) {
          SysLog_Print(SYSLOG_INFO, "rx %d, last: %d",
                       g_usb_transport_data.rx_data_size,
                       receivedDataBuffer[g_usb_transport_data.rx_data_size - 1]);
#ifdef PIPELINE_TRANSPORT_RX
          PIPELINE_TRANSPORT_RX(receivedDataBuffer,
                                g_usb_transport_data.rx_data_size);
#else
          g_usb_transport_data.rx_cb(receivedDataBuffer,
                                     g_usb_transport_data.rx_data_size);
#endif
          g_usb_transport_data.rx_data_size = 0;
        }

        if (g_usb_transport_data.tx_in_progress &&
            g_usb_transport_data.tx_data_pending) {
          SysLog_Print(SYSLOG_INFO, "sending %d",
                       g_usb_transport_data.tx_data_pending);
          USB_DEVICE_ControlSend(g_usb_transport_data.usb_device,
                                 transmitDataBuffer,
                                 g_usb_transport_data.tx_data_pending);
          g_usb_transport_data.tx_in_progress = false;
        }

        // We have received data.
        /*
        if (g_usb_transport_data.tx_in_progress == false) {
          // we only go ahead and process the data if we can respond.
#ifdef PIPELINE_TRANSPORT_RX
          PIPELINE_TRANSPORT_RX(receivedDataBuffer,
                                g_usb_transport_data.rx_data_size);
#else
          g_usb_transport_data.rx_cb(receivedDataBuffer,
                                     g_usb_transport_data.rx_data_size);
#endif
          // USB_DEVICE_EndpointStall(g_usb_transport_data.usb_device,
          //                          g_usb_transport_data.rx_endpoint);
          // schedule the next read
          g_usb_transport_data.rx_in_progress = true;
          USB_DEVICE_EndpointRead(g_usb_transport_data.usb_device,
                                  &g_usb_transport_data.read_transfer,
                                  g_usb_transport_data.rx_endpoint,
                                  &receivedDataBuffer[0],
                                  sizeof (receivedDataBuffer));
        }
        */
      }
      break;

    case USB_STATE_ERROR:
      break;
    default:
      break;
  }
}

bool USBTransport_SendResponse(uint8_t token, Command command, uint8_t rc,
                               const IOVec* data, unsigned int iov_count) {
  BSP_LEDToggle(BSP_LED_3);
  /*
  if (g_usb_transport_data.tx_in_progress ||
      !g_usb_transport_data.is_configured) {
    return false;
  }
  */
  SysLog_Print(SYSLOG_INFO, "send res");

  if (g_usb_transport_data.tx_data_pending) {
    SysLog_Print(SYSLOG_INFO, "already pending %d",
                 g_usb_transport_data.tx_data_pending);
  }

  transmitDataBuffer[0] = START_OF_MESSAGE_ID;
  transmitDataBuffer[1] = token;
  transmitDataBuffer[2] = ShortLSB(command);
  transmitDataBuffer[3] = ShortMSB(command);
  // 4 & 5 are the length.
  transmitDataBuffer[6] = rc;

  // Set appropriate flags.
  transmitDataBuffer[7] = 0;
  if (Flags_HasChanged()) {
    transmitDataBuffer[7] |= TRANSPORT_FLAGS_CHANGED;
  }

  unsigned int i = 0;
  uint16_t offset = 0;
  for (; i != iov_count; i++) {
    if (offset + data[i].length > PAYLOAD_SIZE) {
      memcpy(transmitDataBuffer + offset + 8, data[i].base,
             PAYLOAD_SIZE - offset);
      offset = PAYLOAD_SIZE;
      transmitDataBuffer[7] |= TRANSPORT_MSG_TRUNCATED;
      break;
    } else {
      memcpy(transmitDataBuffer + offset + 8, data[i].base, data[i].length);
      offset += data[i].length;
    }
  }

  transmitDataBuffer[4] = ShortLSB(offset);
  transmitDataBuffer[5] = ShortMSB(offset);
  transmitDataBuffer[8 + offset] = END_OF_MESSAGE_ID;

  g_usb_transport_data.tx_data_pending = offset + 9u;
  SysLog_Print(SYSLOG_INFO, "tx pending %d",
               g_usb_transport_data.tx_data_pending);

  /*
  USB_DEVICE_RESULT result = USB_DEVICE_EndpointWrite(
      g_usb_transport_data.usb_device,
      &g_usb_transport_data.write_transfer,
      g_usb_transport_data.tx_endpoint, transmitDataBuffer,
      offset + 9,
      USB_DEVICE_TRANSFER_FLAGS_DATA_COMPLETE);
  if (result != USB_DEVICE_RESULT_OK) {
    g_usb_transport_data.tx_in_progress = false;
  }
  */
  //return result == USB_DEVICE_RESULT_OK;
  return true;
}

bool USBTransport_WritePending() {
  return g_usb_transport_data.tx_data_pending;
  // return g_usb_transport_data.tx_in_progress;
}

USB_DEVICE_HANDLE USBTransport_GetHandle() {
  return g_usb_transport_data.usb_device;
}

bool USBTransport_IsConfigured() {
  return g_usb_transport_data.is_configured;
}


void USBTransport_SoftReset() {
  /*
  if (g_usb_transport_data.tx_in_progress) {
    USB_DEVICE_EndpointTransferCancel(
        g_usb_transport_data.usb_device,
        g_usb_transport_data.tx_endpoint,
        g_usb_transport_data.write_transfer);
  }
  */
}
