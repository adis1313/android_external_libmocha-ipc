/**
 * This file is part of libsamsung-ipc.
 *
 * Copyright (C) 2011 Paul Kocialkowski <contact@paulk.fr>
 *                    Joerie de Gram <j.de.gram@gmail.com>
 *                    Simon Busch <morphis@gravedo.de>
 *
 * Modified for Jet & Wave - 	KB <kbjetdroid@gmail.com>
 *								Dominik Marszk <dmarszk@gmail.com>
 *
 * libsamsung-ipc is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libsamsung-ipc is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libsamsung-ipc.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <termios.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <radio.h>

#include "ipc_private.h"
#include "wave_ipc.h"

#define LOG_TAG "RIL_WaveIPC"
#include <utils/Log.h>

int32_t wave_modem_bootstrap(struct ipc_client *client)
{

	int32_t rc = 0;

    int32_t dpram_fd = -1;

	int32_t fd, retval;

    DEBUG_I("wave_ipc_bootstrap: enter\n");

    DEBUG_I("wave_ipc_bootstrap: open modem_ctl\n");

    dpram_fd = open(DPRAM_TTY, O_RDWR | O_NDELAY);

    if(dpram_fd < 0) {
    	DEBUG_I("wave_ipc_bootstrap: failed to open dev/dpram0\n");
    	return 1;
    }

    DEBUG_I("wave_ipc_bootstrap: send amss_run_request\n");

    ioctl(dpram_fd, IOCTL_MODEM_AMSSRUNREQ);

    DEBUG_I("wave_ipc_bootstrap: closing dev/modem_ctl\n");

    close(dpram_fd);

    DEBUG_I("wave_ipc_bootstrap: exit\n");

    return 0;
}

int32_t wave_ipc_open(void *data, uint32_t size, void *io_data)
{
    struct termios termios;

    int32_t fd = -1;

    if(io_data == NULL)
        return -1;

    fd = *((int32_t *) io_data);

    fd = open(DPRAM_TTY, O_RDWR);

    DEBUG_I("dpram fd = 0x%x\n", fd);

    if(fd < 0) {
        return 1;
    }

    tcgetattr(fd, &termios);
    cfmakeraw(&termios);
    tcsetattr(fd, TCSANOW, &termios);

    memcpy(io_data, &fd, sizeof(int32_t));

    return 0;
}

int32_t wave_ipc_close(void *data, uint32_t size, void *io_data)
{
    int32_t fd = -1;

    if(io_data == NULL)
        return -1;

    fd = *((int32_t *) io_data);

    if(fd) {
        return close(fd);
    }

    return 0;
}

int32_t wave_ipc_power_on(void *data)
{
    int32_t fd = -1;

    if(data == NULL)
        return -1;

    fd = *((int32_t *) data);

    ioctl(fd, IOCTL_PHONE_ON);

    return 0;
}

int32_t wave_ipc_power_off(void *data)
{
    int32_t fd = -1;

    if(data == NULL)
        return -1;

    fd = *((int32_t *) data);

    ioctl(fd, IOCTL_PHONE_OFF);

    return 0;
}

int32_t send_packet(struct ipc_client *client, struct modem_io *request)
{
	int32_t retval;
    struct fifoPacketHeader *ipc;
    uint8_t *frame;
    uint8_t *payload;
    int32_t frame_length;

    /* Frame length: FIFO header + payload length */
    frame_length = (sizeof(*ipc) + request->datasize);

    frame = (uint8_t*)malloc(frame_length);

    /* FIFO header */
    ipc = (struct fifoPacketHeader*)(frame);

    ipc->magic = request->magic;
    ipc->cmd = request->cmd;
    ipc->datasize = request->datasize;

    /* FIFO payload */
    payload = (frame + sizeof(*ipc));
    memcpy(payload, request->data, request->datasize);

	retval = client->handlers->write(frame, frame_length, client->handlers->write_data);

    free(frame);

    return 0;
}

int32_t wave_ipc_send(struct ipc_client *client, struct modem_io *request)
{
	int32_t left_data;
	struct modem_io multi_request;
	struct multiPacketHeader *multiHeader;

	if (request->datasize > MAX_SINGLE_FRAME_DATA)
	{
		DEBUG_I("packet to send is larger than 0x1000\n");

		multi_request.magic = 0xCAFECAFE;
		multi_request.cmd = FIFO_PKT_FIFO_INTERNAL;
		multi_request.datasize = 0x0C;

		multiHeader = (struct multiPacketHeader *)malloc(sizeof(struct multiPacketHeader));

		multiHeader->command = 0x02;
		multiHeader->packtLen = request->datasize;
		multiHeader->packetType = request->cmd;

		multi_request.data = (uint8_t *)multiHeader;

		send_packet(client, &multi_request);

		left_data = request->datasize;

		multi_request.data = request->data;

		while (left_data > 0)
		{
			if (left_data > MAX_SINGLE_FRAME_DATA)
			{
				multi_request.datasize = MAX_SINGLE_FRAME_DATA;
			}
			else
			{
				multi_request.datasize = left_data;
			}

			send_packet(client, &multi_request);

			multi_request.data += MAX_SINGLE_FRAME_DATA;

			left_data -= MAX_SINGLE_FRAME_DATA;
		}
	}
	else
	{
		send_packet(client, request);
	}

	return 0;
}

int32_t wave_ipc_recv(struct ipc_client *client, struct modem_io *response)
{
    uint8_t buf[SIZ_PACKET_HEADER];
    uint8_t *data;
    uint32_t frame_length;
    struct fifoPacketHeader *ipc;
    struct modem_io modem_packet;
    uint32_t num_read;
    uint32_t left;

    num_read = client->handlers->read((void*)buf, sizeof(buf), client->handlers->read_data);

    ipc = (struct fifoPacketHeader *)buf;

    if(num_read == sizeof(buf) && ipc->magic == 0xCAFECAFE) {

        frame_length = ipc->datasize;
        left = frame_length;

        data = (uint8_t*)malloc(left);
        num_read = client->handlers->read((void*)data, left, client->handlers->read_data);

        if(num_read == left) {
            response->magic = ipc->magic;
            response->cmd = ipc->cmd;
            response->datasize = ipc->datasize;

            response->data = (uint8_t*)malloc(left);
            memcpy(response->data, data , response->datasize);

            return 0;
        }
    }

    return 0;
}

int32_t wave_ipc_read(void *data, uint32_t size, void *io_data)
{
    int32_t fd = -1;

    if(io_data == NULL)
        return -1;

    fd = *((int32_t *) io_data);

    if(fd < 0)
        return -1;

    return read(fd, data, size);
}

int32_t wave_ipc_write(void *data, uint32_t size, void *io_data)
{
    int32_t fd = -1;

    if(io_data == NULL)
        return -1;

    fd = *((int32_t *) io_data);

    if(fd < 0)
        return -1;

    return write(fd, data, size);
}

int32_t wave_modem_operations(struct ipc_client *client, void *data, uint32_t cmd)
{
    int32_t fd = -1;

    fd = *((int32_t *)client->handlers->write_data);

    if(fd < 0)
        return -1;
    DEBUG_I("modem_operations ioctl cmd = 0x%x\n", cmd);

    return ioctl(fd, cmd, data);
}

void *wave_ipc_common_data_create(void)
{
    void *io_data;
    int io_data_len;

    io_data_len = sizeof(int);
    io_data = malloc(io_data_len);

    if(io_data == NULL)
        return NULL;

    memset(io_data, 0, io_data_len);

    return io_data;
}

int wave_ipc_common_data_destroy(void *io_data)
{
    // This was already done, not an error but we need to return
    if(io_data == NULL)
        return 0;

    free(io_data);

    return 0;
}

int wave_ipc_common_data_set_fd(void *io_data, int fd)
{
    int *common_data;

    if(io_data == NULL)
        return -1;

    common_data = (int *) io_data;
    common_data = &fd;

    return 0;
}

int wave_ipc_common_data_get_fd(void *io_data)
{
    int *common_data;

    if(io_data == NULL)
        return -1;

    common_data = (int *) io_data;

    return (int) *(common_data);
}

struct ipc_handlers wave_default_handlers = {
    .open = wave_ipc_open,
    .close = wave_ipc_close,
    .power_on = wave_ipc_power_on,
    .power_off = wave_ipc_power_off,
    .read = wave_ipc_read,
    .write = wave_ipc_write,
    .common_data = NULL,
    .common_data_create = wave_ipc_common_data_create,
    .common_data_destroy = wave_ipc_common_data_destroy,
    .common_data_set_fd = wave_ipc_common_data_set_fd,
    .common_data_get_fd = wave_ipc_common_data_get_fd,
};

struct ipc_ops wave_fmt_ops = {
    .send = wave_ipc_send,
    .recv = wave_ipc_recv,
    .bootstrap = wave_modem_bootstrap,
    .modem_operations = wave_modem_operations,
};

void wave_ipc_register(void)
{
    ipc_register_device_client_handlers(IPC_DEVICE_WAVE, &wave_fmt_ops,
                                        NULL, &wave_default_handlers);
}
