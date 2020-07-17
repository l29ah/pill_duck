// vim: tabstop=8 softtabstop=8 shiftwidth=8 noexpandtab
/*
 * This file is part of the libopencm3 project.
 *
 * Copyright (C) 2010 Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/cdc.h>
#include <string.h>

#include "cdcacm.h"
#include "version.h"

/* Serial ACM interface */
#define CDCACM_PACKET_SIZE 	128
#define CDCACM_UART_ENDPOINT	0x03
#define CDCACM_INTR_ENDPOINT	0x84

static const struct usb_endpoint_descriptor uart_comm_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = CDCACM_INTR_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_ATTR_INTERRUPT,
	.wMaxPacketSize = 16,
	.bInterval = 255,
}};

static const struct usb_endpoint_descriptor uart_data_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = CDCACM_UART_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = CDCACM_PACKET_SIZE,
	.bInterval = 1,
}, {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x80 | CDCACM_UART_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = CDCACM_PACKET_SIZE,
	.bInterval = 1,
}};

static const struct {
	struct usb_cdc_header_descriptor header;
	struct usb_cdc_call_management_descriptor call_mgmt;
	struct usb_cdc_acm_descriptor acm;
	struct usb_cdc_union_descriptor cdc_union;
} __attribute__((packed)) uart_cdcacm_functional_descriptors = {
	.header = {
		.bFunctionLength = sizeof(struct usb_cdc_header_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_HEADER,
		.bcdCDC = 0x0110,
	},
	.call_mgmt = {
		.bFunctionLength =
			sizeof(struct usb_cdc_call_management_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_CALL_MANAGEMENT,
		.bmCapabilities = 0,
		.bDataInterface = 2,
	},
	.acm = {
		.bFunctionLength = sizeof(struct usb_cdc_acm_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_ACM,
		.bmCapabilities = 2, /* SET_LINE_CODING supported*/
	},
	.cdc_union = {
		.bFunctionLength = sizeof(struct usb_cdc_union_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_UNION,
		.bControlInterface = 1,
		.bSubordinateInterface0 = 2,
	 }
};

const struct usb_interface_descriptor uart_comm_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 1,
	.bAlternateSetting = 0,
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_CDC,
	.bInterfaceSubClass = USB_CDC_SUBCLASS_ACM,
	.bInterfaceProtocol = USB_CDC_PROTOCOL_AT,
	.iInterface = 4,

	.endpoint = uart_comm_endp,

	.extra = &uart_cdcacm_functional_descriptors,
	.extralen = sizeof(uart_cdcacm_functional_descriptors)
}};

const struct usb_interface_descriptor uart_data_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 2,
	.bAlternateSetting = 0,
	.bNumEndpoints = 2,
	.bInterfaceClass = USB_CLASS_DATA,
	.bInterfaceSubClass = 0,
	.bInterfaceProtocol = 0,
	.iInterface = 0,

	.endpoint = uart_data_endp,
}};

const struct usb_iface_assoc_descriptor uart_assoc = {
	.bLength = USB_DT_INTERFACE_ASSOCIATION_SIZE,
	.bDescriptorType = USB_DT_INTERFACE_ASSOCIATION,
	.bFirstInterface = 1,
	.bInterfaceCount = 2,
	.bFunctionClass = USB_CLASS_CDC,
	.bFunctionSubClass = USB_CDC_SUBCLASS_ACM,
	.bFunctionProtocol = USB_CDC_PROTOCOL_AT,
	.iFunction = 0,
};

static void cdcacm_set_modem_state(usbd_device *dev, int iface, bool dsr, bool dcd)
{
	char buf[10];
	struct usb_cdc_notification *notif = (void*)buf;
	/* We echo signals back to host as notification */
	notif->bmRequestType = 0xA1;
	notif->bNotification = USB_CDC_NOTIFY_SERIAL_STATE;
	notif->wValue = 0;
	notif->wIndex = iface;
	notif->wLength = 2;
	buf[8] = (dsr ? 2 : 0) | (dcd ? 1 : 0);
	buf[9] = 0;
	usbd_ep_write_packet(dev, 0x82 + iface, buf, 10);
}

static enum usbd_request_return_codes cdcacm_control_request(usbd_device *dev,
		struct usb_setup_data *req, uint8_t **buf, uint16_t *len,
		usbd_control_complete_callback *complete)
{
	(void)dev;
	(void)complete;
	(void)buf;
	(void)len;

	switch(req->bRequest) {
	case USB_CDC_REQ_SET_CONTROL_LINE_STATE:
		cdcacm_set_modem_state(dev, req->wIndex, true, true);
		return 1;
	case USB_CDC_REQ_SET_LINE_CODING:
		if(*len < sizeof(struct usb_cdc_line_coding))
			return 0;

		return 1;
	}
	return 0;
}

static void send_chunked_blocking(char *buf, int len, usbd_device *dev, int endpoint, int max_packet_length) {
	uint16_t bytes_written = 0;
	uint16_t total_bytes_written = 0;
	uint16_t bytes_remaining = len;

	do {
		uint16_t this_length = bytes_remaining;
		if (this_length > max_packet_length) this_length = max_packet_length;

		bytes_written = usbd_ep_write_packet(dev, endpoint, buf + total_bytes_written, this_length);
		bytes_remaining -= bytes_written;
		total_bytes_written += bytes_written;
	} while (bytes_remaining > 0);
}

extern char *process_serial_command(char *buf, int len);

static void usbuart_usb_out_cb(usbd_device *dev, uint8_t ep)
{
	(void)ep;

	char buf[CDCACM_PACKET_SIZE];
	char reply_buf[256];

	static char typing_buf[2048] = {0};
	static int typing_index = 0;

	int len = usbd_ep_read_packet(dev, CDCACM_UART_ENDPOINT,
					buf, CDCACM_PACKET_SIZE);


	int j = 0;
	for(int i = 0; i < len; i++) {
		gpio_toggle(GPIOC, GPIO13);

		// Echo back what was typed
		// Enter sends a CR, but an LF is needed to advance to next line
		if (buf[i] == '\r') reply_buf[j++] = '\n';
		reply_buf[j++] = buf[i];

		typing_buf[typing_index++] = buf[i];

		if (buf[i] == '\r' || buf[i] == '\n') {
			char *response = process_serial_command(typing_buf, typing_index);
			typing_index = 0;

			for (size_t k = 0; k < strlen(response); ++k) {
				reply_buf[j++] = response[k];
			}

			// prompt
			reply_buf[j++] = '\r';
			reply_buf[j++] = '\n';
			reply_buf[j++] = 'd';
			reply_buf[j++] = 'u';
			reply_buf[j++] = 'c';
			reply_buf[j++] = 'k';
			reply_buf[j++] = '>';
			reply_buf[j++] = ' ';
		}
	}

	send_chunked_blocking(reply_buf, j, dev, CDCACM_UART_ENDPOINT, CDCACM_PACKET_SIZE);
}

static void usbuart_usb_in_cb(usbd_device *dev, uint8_t ep)
{
	(void) dev;
	(void) ep;
}

void cdcacm_set_config(usbd_device *dev, uint16_t wValue)
{
	(void) wValue;

	/* Serial interface */
	usbd_ep_setup(dev, CDCACM_UART_ENDPOINT, USB_ENDPOINT_ATTR_BULK,
	              CDCACM_PACKET_SIZE, usbuart_usb_out_cb);
	usbd_ep_setup(dev, 0x80 | CDCACM_UART_ENDPOINT, USB_ENDPOINT_ATTR_BULK,
	              CDCACM_PACKET_SIZE, usbuart_usb_in_cb);
	usbd_ep_setup(dev, CDCACM_INTR_ENDPOINT, USB_ENDPOINT_ATTR_INTERRUPT, 16, NULL);

	usbd_register_control_callback(dev,
			USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE,
			USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT,
			cdcacm_control_request);

	/* Notify the host that DCD is asserted.
	 * Allows the use of /dev/tty* devices on *BSD/MacOS
	 */
	cdcacm_set_modem_state(dev, 2, true, true);
}


