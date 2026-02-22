/*
 * This file is part of the dcload Dreamcast ethernet loader
 *
 * Copyright (C) 2001 Andrew Kieschnick <andrewk@austin.rr.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <string.h>
#include "syscalls.h"
#include "packet.h"
#include "net.h"
#include "adapter.h"
#include "commands.h"

// Leave this as an int.
static int gdStatus = 0;
static int gdCmdId = 1;
static int gdLastCmdId = 0;
static int gdCmdStat[4] = {0, 0, 0, 0};
static int gdDmaInProgress = 0;
static int gdDmaChannel = 0;
static unsigned int gdDmaRemaining = 0;

#define GDROM_MAX_CMD 47
#define GDROM_CMD_ID_MAX 8
#define GD_CMD_HISTORY 8
#define GD_SYSCALL_TIMEOUT_SECONDS 20
/* dcload resident region, in physical (29-bit) SH4 address space. */
#define DCLOAD_RESIDENT_START_PHYS 0x0c004000U
#define DCLOAD_RESIDENT_END_PHYS   0x0c010000U

typedef struct gd_cmd_entry {
	int id;
	int status;
	int stat[4];
} gd_cmd_entry_t;

static gd_cmd_entry_t gdCmdHistory[GD_CMD_HISTORY];
static unsigned int gdCmdHistoryNext = 0;

static unsigned int sh4_phys_addr(unsigned int addr)
{
	return addr & 0x1fffffffU;
}

static void write_hex8(unsigned int value)
{
	static const char hex[] = "0123456789abcdef";
	char out[9];
	int i;

	for (i = 7; i >= 0; i--)
	{
		out[i] = hex[value & 0x0fU];
		value >>= 4;
	}
	out[8] = '\0';
	write(1, out, 8);
}

static int overlaps_region_phys(unsigned int addr, unsigned int size, unsigned int start_phys, unsigned int end_phys)
{
	unsigned int phys_addr;
	unsigned int addr_end;

	if (!size)
	{
		return 0;
	}

	phys_addr = sh4_phys_addr(addr);
	addr_end = phys_addr + size;
	/* Overflow means invalid range; treat as overlap/protection hit. */
	if (addr_end < phys_addr)
	{
		return 1;
	}

	return (phys_addr < end_phys) && (addr_end > start_phys);
}

static void gd_store_cmd_status(int id, int status, int s0, int s1, int s2, int s3)
{
	gd_cmd_entry_t *entry = &gdCmdHistory[gdCmdHistoryNext];
	entry->id = id;
	entry->status = status;
	entry->stat[0] = s0;
	entry->stat[1] = s1;
	entry->stat[2] = s2;
	entry->stat[3] = s3;
	gdCmdHistoryNext = (gdCmdHistoryNext + 1U) % GD_CMD_HISTORY;
}

struct TOC {
	unsigned int entry[99];
	unsigned int first, last;
	unsigned int dunno;
};

int gdGdcReqCmd(int cmd, int *param)
{
	command_3int_t * command = (command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
	int cmd_id = gdCmdId;
	struct TOC *toc;
	int i;

	/* Match dc-virtcd target behavior: unsupported IDs outside this range
	 * are rejected up-front and return 0 (no command handle). */
	if ((cmd < 0) || (cmd > GDROM_MAX_CMD))
	{
		return 0;
	}

	gdCmdId++;
	if (gdCmdId > GDROM_CMD_ID_MAX)
	{
		gdCmdId = 1;
	}

	switch (cmd) {
	case 16: /* read sectors */
	case 17: /* dma read sectors */
	{
		unsigned int dc_addr = (unsigned int)param[2];
		unsigned int byte_count = (unsigned int)param[1] * 2048U;

		if (overlaps_region_phys(dc_addr, byte_count, DCLOAD_RESIDENT_START_PHYS, DCLOAD_RESIDENT_END_PHYS))
		{
			gdCmdStat[0] = 1;
			gdCmdStat[1] = 15; /* data read error */
			gdCmdStat[2] = 0;
			gdCmdStat[3] = 0;
			gdStatus = -1;
			gdLastCmdId = cmd_id;
			gd_store_cmd_status(cmd_id, gdStatus, gdCmdStat[0], gdCmdStat[1], gdCmdStat[2], gdCmdStat[3]);
			write(1, "CDFS read blocked: overlap with dcload resident memory at ", 59);
			write_hex8(dc_addr);
			write(1, "\r\n", 2);
			return cmd_id;
		}
	}

		memcpy(command->id, CMD_CDFSREAD, 4);
		command->value0 = htonl(param[0]);
		command->value1 = htonl(param[2]);
		command->value2 = htonl(param[1]*2048);
		gdCmdStat[0] = 1;
		gdCmdStat[1] = 0;
		gdCmdStat[2] = 0;
		gdCmdStat[3] = 0;
		gdStatus = 1;
		syscall_retval = (unsigned int)-1;
		timeout_loop = GD_SYSCALL_TIMEOUT_SECONDS;
		build_send_packet(sizeof(command_3int_t));
		bb->loop(0);
		timeout_loop = 0;
		if ((int)syscall_retval < 0)
		{
			gdCmdStat[1] = 15; /* data read error */
			gdStatus = -1;
			gdLastCmdId = cmd_id;
			gd_store_cmd_status(cmd_id, gdStatus, gdCmdStat[0], gdCmdStat[1], gdCmdStat[2], gdCmdStat[3]);
			return cmd_id;
		}
		gdCmdStat[2] = param[1] * 2048; /* number of bytes transferred */
		gdStatus = 2;
		gdLastCmdId = cmd_id;
		if (cmd == 17)
		{
			gdDmaChannel = cmd_id;
			gdDmaInProgress = 0;
			gdDmaRemaining = 0;
		}
		gd_store_cmd_status(cmd_id, gdStatus, gdCmdStat[0], gdCmdStat[1], gdCmdStat[2], gdCmdStat[3]);
		return cmd_id;
	case 19: /* read toc */
		(void)toc; /* host writes directly to target memory */
		(void)i;
		memcpy(command->id, CMD_CDFSTOC, 4);
		command->value0 = htonl(param[0]); /* session */
		command->value1 = htonl(param[1]); /* TOC dest addr */
		command->value2 = 0;
		gdCmdStat[0] = 1;
		gdCmdStat[1] = 0;
		gdCmdStat[2] = 0;
		gdCmdStat[3] = 0;
		gdStatus = 1;
		syscall_retval = (unsigned int)-1;
		timeout_loop = GD_SYSCALL_TIMEOUT_SECONDS;
		build_send_packet(sizeof(command_3int_t));
		bb->loop(0);
		timeout_loop = 0;
		if ((int)syscall_retval < 0)
		{
			gdCmdStat[1] = 15; /* TOC read error */
			gdStatus = -1;
			gdLastCmdId = cmd_id;
			gd_store_cmd_status(cmd_id, gdStatus, gdCmdStat[0], gdCmdStat[1], gdCmdStat[2], gdCmdStat[3]);
			return cmd_id;
		}
		gdCmdStat[2] = sizeof(struct TOC);
		gdStatus = 2;
		gdLastCmdId = cmd_id;
		gd_store_cmd_status(cmd_id, gdStatus, gdCmdStat[0], gdCmdStat[1], gdCmdStat[2], gdCmdStat[3]);
		return cmd_id;
	case 24: /* init disc */
		gdCmdStat[0] = 1;
		gdCmdStat[1] = 0;
		gdCmdStat[2] = 0;
		gdCmdStat[3] = 0;
		gdStatus = 2;
		gdLastCmdId = cmd_id;
		gd_store_cmd_status(cmd_id, gdStatus, gdCmdStat[0], gdCmdStat[1], gdCmdStat[2], gdCmdStat[3]);
		return cmd_id;
	case 40: /* get driver version */
		if (param && param[0])
		{
			static const char drv_ver[] = "GDC Version 1.10 1999-03-31\2";
			/* Match dc-virtcd payload length (without trailing NUL). */
			memcpy((void *)param[0], drv_ver, sizeof(drv_ver) - 1);
		}
		gdCmdStat[0] = 1;
		gdCmdStat[1] = 0;
		gdCmdStat[2] = 28;
		gdCmdStat[3] = 0;
		gdStatus = 2;
		gdLastCmdId = cmd_id;
		gd_store_cmd_status(cmd_id, gdStatus, gdCmdStat[0], gdCmdStat[1], gdCmdStat[2], gdCmdStat[3]);
		return cmd_id;
	default:
		write(1, "GD unsupported cmd=", 19);
		write_hex8((unsigned int)cmd);
		write(1, "\r\n", 2);
		/* Mirror dc-virtcd behavior for unsupported commands: return a
		 * valid command handle and report error through GetCmdStat. */
		gdCmdStat[0] = 1;
		gdCmdStat[1] = 32; /* invalid command */
		gdCmdStat[2] = 0;
		gdCmdStat[3] = 0;
		gdStatus = -1;
		gdLastCmdId = cmd_id;
		gd_store_cmd_status(cmd_id, gdStatus, gdCmdStat[0], gdCmdStat[1], gdCmdStat[2], gdCmdStat[3]);
		return cmd_id;
	}

}

int gdGdcExecServer(void)
{
	return 0;
}

int gdGdcGetCmdStat(int f, int *status)
{
	unsigned int idx;

	/* Match status to the command handle. Commercial games may have
	 * multiple command IDs in flight and poll by ID. */
	for (idx = 0; idx < GD_CMD_HISTORY; idx++)
	{
		if (gdCmdHistory[idx].id == f)
		{
			status[0] = gdCmdHistory[idx].stat[0];
			status[1] = gdCmdHistory[idx].stat[1];
			status[2] = gdCmdHistory[idx].stat[2];
			status[3] = gdCmdHistory[idx].stat[3];
			return gdCmdHistory[idx].status;
		}
	}

	status[0] = 0;
	status[1] = 0;
	status[2] = 0;
	status[3] = 0;
	return 0;
}

int gdGdcGetDrvStat(int *param)
{
	param[0] = 1;
	param[1] = 0x80;
	return 0;
}

/* Commercial titles may use these DMA-related GD entry points around
 * command 17. Keep ABI/signatures compatible with BIOS expectations. */
void gdGdcG1DmaEnd(unsigned int func, unsigned int param)
{
	gdDmaInProgress = 0;
	gdDmaRemaining = 0;
	if (func)
	{
		void (*callback)(unsigned int) = (void (*)(unsigned int))func;
		callback(param);
	}
}

int gdGdcReqDmaTrans(int gd_chn, unsigned int *params)
{
	if (!params)
	{
		return -1;
	}
	/* Keep expected channel semantics for games that validate it. */
	if ((gdDmaChannel != 0) && (gd_chn != gdDmaChannel))
	{
		return -1;
	}
	/* params[0] = destination, params[1] = bytes (KOS/isoldr ABI). Data is
	 * already in place from cmd 17, so DMA completion is immediate. */
	(void)params[0];
	gdDmaRemaining = 0;
	gdDmaInProgress = 0;
	return 0;
}

int gdGdcCheckDmaTrans(int gd_chn, unsigned int *size)
{
	if ((gdDmaChannel != 0) && (gd_chn != gdDmaChannel))
	{
		return -1;
	}
	if (size)
	{
		*size = gdDmaRemaining;
	}
	/* 0 means transfer complete, 1 means in progress. */
	return gdDmaInProgress ? 1 : 0;
}

int gdGdcReadAbort(int gd_chn)
{
	(void)gd_chn;
	gdDmaInProgress = 0;
	gdDmaRemaining = 0;
	return 0;
}

int gdGdcChangeDataType(int *param)
{
	/* Match dc-virtcd target behavior (gdrom_sector_mode).
	 * Some commercial games query this and expect these values. */
	if (!param)
	{
		return -1;
	}

	if (param[0] == 0)
	{
		return 0;
	}
	else if (param[0] == 1)
	{
		param[1] = 8192;
		param[2] = 1024;
		param[3] = 2048;
		return 0;
	}

	return -1;
}

int gdGdcInitSystem(void)
{
	return 0;
}

int gdGdcReset(void)
{
	gdStatus = 2;
	gdDmaInProgress = 0;
	gdDmaChannel = 0;
	gdDmaRemaining = 0;
	return 0;
}
