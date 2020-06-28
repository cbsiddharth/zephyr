/*
 * Copyright (c) 2020 Siddharth Chandrasekaran <siddharth@embedjournal.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <init.h>
#include <device.h>
#include <drivers/uart.h>
#include <sys/ring_buffer.h>
#include <logging/log.h>

#include "osdp_common.h"

LOG_MODULE_REGISTER(osdp, CONFIG_OSDP_LOG_LEVEL);

struct osdp osdp_ctx;
struct osdp_cp osdp_cp_ctx;
struct osdp_pd osdp_pd_ctx[CONFIG_OSDP_NUM_CONNECTED_PD];

static struct k_thread osdp_refresh_thread;
static K_THREAD_STACK_DEFINE(osdp_thread_stack, 256);

struct osdp_device {
	struct ring_buf rx_buf;
	struct ring_buf tx_buf;
	uint8_t rx_fbuf[CONFIG_OSDP_UART_BUFFER_LENGTH];
	uint8_t tx_fbuf[CONFIG_OSDP_UART_BUFFER_LENGTH];
	struct device *dev;
} osdp_device;

/* implemented by CP and PD */
void osdp_refresh(void *arg1, void *arg2, void *arg3);
int osdp_setup(struct osdp_channel *channel);

static void osdp_uart_isr(void *user_data)
{
	uint8_t buf[64];
	size_t read, wrote, len;
	struct osdp_device *p = user_data;

	while (uart_irq_update(p->dev) && uart_irq_is_pending(p->dev)) {

		if (uart_irq_rx_ready(p->dev)) {
			read = uart_fifo_read(p->dev, buf, sizeof(buf));
			if (read) {
				wrote = ring_buf_put(&p->rx_buf, buf, read);
				if (wrote < read) {
					LOG_ERR("RX: Drop %u", read - wrote);
				}
			}
		}

		if (uart_irq_tx_ready(p->dev)) {
			len = ring_buf_get(&p->tx_buf, buf, 1);
			if (!len) {
				uart_irq_tx_disable(p->dev);
			} else {
				wrote = uart_fifo_fill(p->dev, buf, 1);
			}
		}
	}
}

static int osdp_uart_receive(void *data, uint8_t *buf, int len)
{
	struct osdp_device *p = data;

	return (int)ring_buf_get(&p->rx_buf, buf, len);
}

static int osdp_uart_send(void *data, uint8_t *buf, int len)
{
	int sent = 0;
	struct osdp_device *p = data;

	sent = (int)ring_buf_put(&p->tx_buf, buf, len);
	uart_irq_tx_enable(p->dev);
	return sent;
}

static void osdp_uart_flush(void *data)
{
	struct osdp_device *p = data;

	ring_buf_reset(&p->tx_buf);
	ring_buf_reset(&p->rx_buf);
}

static int osdp_init(struct device *arg)
{
	ARG_UNUSED(arg);
	uint8_t c;
	struct osdp_device *p = &osdp_device;

	ring_buf_init(&p->rx_buf, sizeof(p->rx_fbuf), p->rx_fbuf);
	ring_buf_init(&p->tx_buf, sizeof(p->tx_fbuf), p->tx_fbuf);

	p->dev = device_get_binding(CONFIG_OSDP_UART_DEV_NAME);
	if (p->dev == NULL) {
		LOG_ERR("Failed to get UART dev binding");
		return -1;
	}

	uart_irq_rx_disable(p->dev);
	uart_irq_tx_disable(p->dev);
	uart_irq_callback_user_data_set(p->dev, osdp_uart_isr, p);

	/* Drain UART fifo */
	while (uart_irq_rx_ready(p->dev)) {
		uart_fifo_read(p->dev, &c, 1);
	}

	/* Both TX and RX are interrupt driven */
	uart_irq_rx_enable(p->dev);

	struct osdp_channel channel = {
		.send = osdp_uart_send,
		.recv = osdp_uart_receive,
		.flush = osdp_uart_flush,
		.data = p,
	};

	if (osdp_setup(&channel)) {
		LOG_ERR("Failed to setup OSDP device!");
		return -1;
	}

	/* kick off refresh thread */
	k_thread_create(&osdp_refresh_thread, osdp_thread_stack,
			CONFIG_OSDP_THREAD_STACK_SIZE, osdp_refresh,
			NULL, NULL, NULL, K_PRIO_COOP(2), 0, K_NO_WAIT);
	return 0;
}

SYS_INIT(osdp_init, POST_KERNEL, 10);
