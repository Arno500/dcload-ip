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

/*
 * GD-ROM drive emulation, modelled on DreamShell's ISO loader (isoldr), SD
 * card path.
 *
 * WHY THIS IS SHAPED LIKE A SERVER AND NOT LIKE A FUNCTION
 *
 * The previous implementation answered a read entirely inside gdGdcReqCmd:
 * it sent the request, sat in bb->loop() until the host had delivered every
 * byte, and returned COMPLETED. Simple, and wrong in a way that matters.
 * The BIOS driver a title is written against is a coroutine (see the long
 * comment in cdfs_redir.s). It answers ReqCmd immediately with PROCESSING and
 * then does the work in slices, one slice per gdGdcExecServer call, giving the
 * CPU back to the game in between. Sonic Adventure issues roughly 45000
 * ExecServer calls against 119 ReqCmd -- it is polling a server, and it is
 * entitled to keep running while its read is outstanding.
 *
 * So the structure here mirrors isoldr exactly:
 *
 *   gdGdcReqCmd      queues, sets PROCESSING, returns a channel. No I/O.
 *   gdcServerMain    endless dispatch loop, entered once and never left.
 *   data_transfer    reads in chunks of GD_EMU_ASYNC sectors, one host round
 *                    trip each (isoldr's emu_async). By default the whole
 *                    read completes in one ExecServer: the game only gets
 *                    control back before a failed chunk is retried
 *                    (GD_YIELD_BETWEEN_CHUNKS=1 would yield after every
 *                    chunk; that broke Sonic Adventure).
 *   gdGdcGetCmdStat  reports progress; COMPLETED is consumed once, then IDLE.
 *   gdGdcGetDrvStat  reports PLAYING while a read or CD-DA is live, PAUSED
 *                    otherwise.
 *
 * The CD-DA engine (cdda.c) is also driven from here: cdda_service() runs at
 * the top of the server loop and of gdGdcGetDrvStat, and between the chunks of
 * a read.
 *
 * WHAT ISOLDR DOES THAT WE DO NOT. Its SD path reaches
 * data_transfer_true_async(), which overlaps an SPI DMA with the game and
 * polls it. Our "device" is a UDP round trip with nothing to poll, so we use
 * the emu_async chunk loop that isoldr falls back to without DMA.
 *
 * WHERE A TX IS SAFE. pkt_buf and the LoadBinary window are single and shared,
 * and bb->loop() dispatches incoming commands that build into pkt_buf. Never
 * transmit between building a command and sending it, and never start a second
 * transfer while one is waiting (g_gd_in_transfer). AGENTS.md 4.5.
 */

#include <string.h>
#include <unistd.h>
#include "syscalls.h"
#include "packet.h"
#include "net.h"
#include "adapter.h"
#include "commands.h"
#include "cdfs.h"
#include "cdda.h"

/* Command codes, from the BIOS GD driver (same numbering as isoldr). */
#define CMD_PIOREAD            16
#define CMD_DMAREAD            17
#define CMD_GETTOC             18
#define CMD_GETTOC2            19
#define CMD_PLAY_TRACKS        20
#define CMD_PLAY_SECTORS       21
#define CMD_PAUSE              22
#define CMD_RELEASE            23
#define CMD_INIT               24
#define CMD_SEEK               27
#define CMD_NOP                29
#define CMD_REQ_MODE           30
#define CMD_SET_MODE           31
#define CMD_STOP               33
#define CMD_GETSCD             34
#define CMD_GETSES             35
#define CMD_REQ_STAT           36
#define CMD_GET_VERS           40
#define CMD_MAX                47

/* Command status, as returned by gdGdcGetCmdStat. */
#define CMD_STAT_FAILED        -1
#define CMD_STAT_IDLE           0
#define CMD_STAT_PROCESSING     1
#define CMD_STAT_COMPLETED      2
#define CMD_STAT_STREAMING      3
#define CMD_STAT_BUSY           4

/* status[3] -- what the drive is waiting on. */
#define CMD_WAIT_INTERNAL       0x00
#define CMD_WAIT_IRQ            0x01

/* Drive status, as returned by gdGdcGetDrvStat. */
#define CD_STATUS_PAUSED        0x01
#define CD_STATUS_PLAYING       0x03

/* Media type. */
#define CD_GDROM                0x80

/* Sense keys the BIOS reports through status[0]/status[1]. */
#define CMD_ERR_NOERROR         0
#define CMD_ERR_HARDWARE        15
#define CMD_ERR_ILLEGALREQUEST  32

#define GDC_CHN_ERROR           0
#define GDC_PARAMS_COUNT        4

/*
 * Deadline for one disc-read wait (ReadSectors, GetTOC): 1.2 s in TMU2 ticks
 * (Pck/4, 12500 per millisecond).
 *
 * The adapter loop's other bounds are poor for this. The seconds timeout counts
 * whole seconds on the performance counter (6 s fires at 7 s, and never under
 * an emulator without PMCR support). RTL_IDLE_POLL_LIMIT only counts polls with
 * no incoming frame, and while a read is stuck the network is usually busy
 * (host retries, CD-DA, --diag). Both are also disarmed if anything clears
 * timeout_loop during the wait, which is how a CD-DA fetch nested in a read
 * once left it waiting forever (see g_gd_in_transfer).
 *
 * A healthy 16 KB chunk takes well under a millisecond and the host gives up on
 * a transfer after ~0.7 s, so 1.2 s waits past the host rather than racing it.
 * On expiry the chunk fails and data_transfer_emu_async() retries it after
 * giving the title a frame.
 *
 * NOTE: in the default build nothing starts TMU2 before CD-DA does (cdda.c);
 * setup_machine() also starts it, but only runs with ISOLDR_SETUP_MACHINE=1.
 * Until then TMU2_COUNT does not move and this deadline cannot expire.
 */
#ifndef GD_READ_DEADLINE_TICKS
#define GD_READ_DEADLINE_TICKS 15000000u
#endif

/* The adapter loop's seconds timeout for GD waits: a coarse backstop behind
 * GD_READ_DEADLINE_TICKS. */
#define GD_SYSCALL_TIMEOUT_SECONDS 6

/* How many times a chunk may be re-requested before the read is failed. */
#define GD_READ_RETRIES 4

/* Trace reads landing at or above this destination to the host console. Set
 * to 0xffffffff to switch that tracing off entirely. */
#ifndef GD_TRACE_DEST_FROM
#define GD_TRACE_DEST_FROM 0xffffffffU
#endif

/* Poll iterations spent flushing the RX ring before each request. 0 = off;
 * measured to make no difference to Sonic Adventure at 256 or 50000. */
#ifndef GD_DRAIN_ITERS
#define GD_DRAIN_ITERS 0
#endif

/* Trace the title's call sites and stacks. Very costly: see gd_note_caller(). */
#ifndef GD_TRACE_CALLER
#define GD_TRACE_CALLER 0
#endif

/* Trace the GD request/answer contract to the host console. */
#ifndef GD_TRACE
#define GD_TRACE 0
#endif

/*
 * Sectors per chunk. DreamShell's per-game database prescribes async = 8 for
 * every Sonic Adventure preset, and 8 sectors is 16 KB -- exactly the BBA's
 * RX ring, so a chunk can never outrun the ring the way a 32 KB read can.
 * 0 disables chunking and reads the whole request in one shot.
 */
#ifndef GD_EMU_ASYNC
#define GD_EMU_ASYNC 8
#endif

/*
 * At or above this many sectors, read in one blocking shot instead of chunking.
 * isoldr uses 100 on its SD path -- and that number DOES NOT TRANSFER HERE.
 *
 * Its reasoning is sound for a card reader: a huge request means a loading
 * screen, so latency stops mattering and one uninterrupted transfer is fastest.
 * But isoldr's devices have no receive ring. Ours does, and it holds 16 KB.
 * Enabling this turned Sonic Adventure's 105-sector read into a single 215040
 * byte burst -- 150 packets back to back into a ring that fits eleven -- and
 * the measurement was unambiguous: 840 packets lost, each one then recovered
 * by its own DoneBinary round trip.
 *
 * 0 disables it. Left in place, and off, because the rule is right for the
 * loader it came from and the reason it is wrong here is worth keeping.
 */
#ifndef GD_BULK_SECTORS
#define GD_BULK_SECTORS 0
#endif

/* dcload's own image, in physical (29-bit) SH4 address space. A title must
 * never be handed disc data on top of the loader serving it. */
#define DCLOAD_RESIDENT_START_PHYS 0x0c004000U
#define DCLOAD_RESIDENT_END_PHYS   0x0c010000U

typedef struct gd_state {
	int req_count;
	int cmd;
	int err;
	int status;
	int ata_status;
	int cmd_abort;

	unsigned int requested;
	unsigned int transfered;

	unsigned int callback;
	unsigned int callback_param;

	unsigned int param[GDC_PARAMS_COUNT];

	unsigned int lba;
	unsigned int data_track;
	int drv_stat;
	int drv_media;

	unsigned int sec_size;
	unsigned int mode;
	unsigned int flags;
} gd_state_t;

/*
 * Statically valid from the moment the image is loaded, NOT zeroed into
 * existence. A title is allowed to issue ReqCmd before it ever calls
 * InitSystem, and a zeroed _GDS would give that read sec_size == 0, i.e. a
 * request for nothing. This also means the lazy server start below can adopt
 * an already-queued command instead of losing it.
 */
static gd_state_t _GDS = {
	.status = CMD_STAT_IDLE,
	.ata_status = CMD_WAIT_INTERNAL,
	.lba = 150,
	.data_track = 1,
	.drv_stat = CD_STATUS_PAUSED,
	.drv_media = CD_GDROM,
	.sec_size = 2048,
	/* 1024, not isoldr's 2048: this is what the real BIOS reports back through
	 * gdGdcChangeDataType, it is what KOS asks for (0, 8192, 1024, 2048), and
	 * it is what dcload answered before this rewrite. A title that reads the
	 * field back must see what hardware would say. */
	.mode = 1024,
	.flags = 8192,
};

/* Forensics. Names are load-bearing: scripts/sa-repeat.sh and dc-peek.py
 * read these symbols out of the ELF by name. */
unsigned int g_gd_idx_counts[18];
/*
 * WHICH GD COMMAND, not just which syscall. g_gd_idx_counts[] says the title
 * called gdGdcReqCmd; it does not say what it asked for, and gdcServerMain
 * force-completes CMD_INIT, CMD_SEEK, CMD_GETSCD, CMD_REQ_MODE and CMD_SET_MODE
 * through one `default:` without recording a thing. So a title stuck on a
 * command we silently answer looks exactly like a title that is busy.
 *
 * 192 bytes of BSS and one increment, no UDP round trip -- none of what makes
 * GD_TRACE too expensive to leave on (AGENTS.md 14.13). Counted BEFORE the
 * lock and the IDLE test, so a command we REFUSE is counted too: a title
 * re-asking because the previous command was never collected is the signal,
 * and it would be invisible if only accepted commands were tallied.
 */
unsigned int g_gd_cmd_counts[CMD_MAX + 1];
unsigned int g_cdfs_sync_chunks;
unsigned int g_cdfs_sync_reentered;
unsigned int g_cdfs_read_fails;
unsigned int g_cdfs_read_retries;
/* Longs of server stack parked at the last yield. saved_regs[] holds 96, of
 * which 11 go to registers and the count, so anything approaching 80 here
 * means the buffer needs enlarging before it silently overruns. */
unsigned int g_gd_park_longs;

/*
 * Non-zero while a disc read (ReadSectors, GetTOC) waits in bb->loop().
 *
 * There is one LoadBinary window (bin_info) and one pkt_buf, so no other
 * transfer may start during that wait. One did: the title runs with
 * interrupts enabled, its handlers call gdGdcGetDrvStat, and that runs
 * cdda_service() before taking the GD lock. An audio fetch started there
 * replaced the read's window (the host's parts then landed nowhere) and cleared
 * the read's deadline on its way out, so the read waited forever and the title
 * froze with the loader still answering. cdda_service() now declines while
 * this is set; the read loop feeds the ring between chunks
 * instead (GD_CDDA_BETWEEN_CHUNKS).
 */
unsigned int g_gd_in_transfer;

/*
 * Stuck GD lock watchdog.
 *
 * A freeze was once read as "every gdGdcExecServer declined because the lock is
 * held, while the server is parked and no C caller is inside" -- a state no
 * path in this file produces and nothing would ever clear. gd_lock_watchdog()
 * detects that state rather than a cause: lock held, server parked, and
 * g_gd_lock_gen (bumped on every C acquire and release) unchanged for 250 ms.
 * The generation test excludes the few instructions in gdcExitToGame() and
 * es_enter where "held" and "parked" legitimately overlap. Releasing the lock
 * then is safe: the server is parked, so resuming it is what ExecServer was
 * trying to do anyway.
 *
 * It has not fired in any recorded session (g_gd_lock_stuck 0). The freezes
 * behind it are attributed to the CD-DA nesting described at g_gd_in_transfer,
 * and none has been recorded since that fix; the watchdog stays as a backstop.
 * Like the read deadline, it needs TMU2 running.
 */
unsigned int g_gd_lock_gen;         /* ++ on every C acquire and release */
unsigned int g_gd_lock_owner;       /* 1 ReqCmd 2 GetCmdStat 3 GetDrvStat
                                     * 4 ChangeDataType 5 Reset; 0 = the
                                     * server itself, which takes the byte in
                                     * es_enter and writes no owner */
unsigned int g_gd_lock_stuck;       /* deadlocks observed and broken */
unsigned int g_gd_lock_stuck_owner; /* the owner latched at the break */
unsigned int g_gd_lock_stuck_ticks; /* how long it had been held, TMU2 ticks */

#ifndef GD_LOCK_STUCK_TICKS
#define GD_LOCK_STUCK_TICKS 3125000u   /* 250 ms at Pck/4 */
#endif

static unsigned int gd_stuck_since;
static unsigned int gd_stuck_gen;
static unsigned int gd_stuck_armed;

/* Take the GD path, recording who has it. Returns 0 on success like gd_lock(). */
static int gd_take(unsigned int who)
{
	if (gd_lock())
	{
		return 1;
	}
	g_gd_lock_owner = who;
	g_gd_lock_gen++;
	return 0;
}

static void gd_give(void)
{
	g_gd_lock_owner = 0;
	g_gd_lock_gen++;
	gd_unlock();
}

/*
 * Runs from the two syscalls a title polls every frame, BEFORE either takes
 * the lock -- so it costs two loads and a compare on the healthy path and
 * needs nothing held to do its job.
 */
static void gd_lock_watchdog(void)
{
	if (!gd_lock_state || (gd_park_ptr == (unsigned int)&gd_park_end))
	{
		gd_stuck_armed = 0;   /* free, or the server is running: nothing to see */
		return;
	}

	if (!gd_stuck_armed || (gd_stuck_gen != g_gd_lock_gen))
	{
		gd_stuck_armed = 1;
		gd_stuck_gen = g_gd_lock_gen;
		gd_stuck_since = TMU2_COUNT;
		return;
	}

	/* TMU2 counts DOWN; the unsigned subtraction is correct across its wrap. */
	if ((unsigned int)(gd_stuck_since - TMU2_COUNT) < GD_LOCK_STUCK_TICKS)
	{
		return;
	}

	g_gd_lock_stuck++;
	g_gd_lock_stuck_owner = g_gd_lock_owner;
	g_gd_lock_stuck_ticks = (unsigned int)(gd_stuck_since - TMU2_COUNT);
	gd_stuck_armed = 0;
	gd_give();
}

/*
 * Post-mortem block, in high RAM.
 *
 * Every counter above lives in dcload's BSS, and that is exactly the memory
 * under suspicion when a title misbehaves: sampled during a failure it can
 * read as foreign data, and a moment later the machine has re-booted and
 * zeroed it. An instrument inside the blast radius cannot report on the blast.
 *
 * CAVEAT, measured 2026-08-10: 0x8cf0c000 is NO LONGER out of reach. It was
 * chosen when dcload lived at 0x8cf00000 and Sonic Adventure's allocator
 * stopped dead at 0x0cf00000; with dcload back at the low base nothing caps
 * the title there any more, and it overwrites this block (it reads back
 * boots = 1, reads = 0, i.e. re-claimed). Treat these values as advisory.
 */
#define PM_BASE   0x8cf0c000U
#define PM_MAGIC  0x33444d50U		/* "PMD3" */
#define PM_SLOTS  12

#define PM_BOOTS      1
#define PM_SP_LAST    2
#define PM_SP_LOW     3
#define PM_READS      4
#define PM_LAST_LBA   5
#define PM_LAST_DEST  6
#define PM_LAST_SECS  7
#define PM_CHUNKS     8
#define PM_PARK_MAX   9

static volatile unsigned int *const pm = (volatile unsigned int *)PM_BASE;

void cdfs_pm_boot(void)
{
	int i;

	if (pm[0] != PM_MAGIC)
	{
		for (i = 0; i < PM_SLOTS; i++)
		{
			pm[i] = 0;
		}
		pm[0] = PM_MAGIC;
		pm[PM_SP_LOW] = 0xffffffffU;
	}
	pm[PM_BOOTS]++;
}

/*
 * Params each command consumes. Reading past this would touch memory the
 * caller never set up, so it is not merely cosmetic. Table covers 16..40.
 */
static const unsigned char cmdp[] = {
	4, 4, 2, 2, 3, 3, 0, 0, 0, 0, 0, 1, 2, 4, 1,
	4, 2, 0, 3, 3, 4, 2, 3, 3, 1
};

static int get_params_count(int cmd)
{
	if (cmd < 16 || cmd > 40)
	{
		return 0;
	}
	return cmdp[cmd - 16];
}

static int is_transfer_cmd(int cmd)
{
	return (cmd == CMD_PIOREAD) || (cmd == CMD_DMAREAD);
}

static unsigned int sh4_phys_addr(unsigned int addr)
{
	return addr & 0x1fffffffU;
}

static int overlaps_dcload(unsigned int addr, unsigned int size)
{
	unsigned int phys_addr;
	unsigned int addr_end;

	if (!size)
	{
		return 0;
	}

	phys_addr = sh4_phys_addr(addr);
	addr_end = phys_addr + size;
	/* Overflow means an invalid range; treat as a protection hit. */
	if (addr_end < phys_addr)
	{
		return 1;
	}

	return (phys_addr < DCLOAD_RESIDENT_END_PHYS) &&
	       (addr_end > DCLOAD_RESIDENT_START_PHYS);
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

/*
 * Compact GD contract trace to the host console (build with GD_TRACE=1).
 *
 * The delivered disc bytes are proven correct by the host's read-back
 * verification, so whatever kills a title is in what we ANSWER, not in what we
 * send. This records the request/answer pairs -- the only part of the contract
 * a title can act on -- and it lands on the host, so it survives the guest
 * reaching address 0 and the reboot that follows.
 *
 * ONE write() per event, not one per field: each write is a full UDP round
 * trip, and a seven-call version of this perturbed the timing enough to change
 * the outcome of the very run it was meant to observe.
 *
 * Only ReqCmd and GetCmdStat are traced. GetDrvStat and ExecServer run at
 * 60 Hz and would drown both the link and the log.
 */
static char gd_trace_buf[48];

static char *gd_trace_hex(char *p, unsigned int v)
{
	static const char hex[] = "0123456789abcdef";
	int i;

	for (i = 7; i >= 0; i--)
	{
		p[i] = hex[v & 0x0fU];
		v >>= 4;
	}
	return p + 8;
}

void gd_trace_always(char tag, unsigned int a, unsigned int b, unsigned int c)
{
	char *p = gd_trace_buf;

	*p++ = tag;
	*p++ = ' ';
	p = gd_trace_hex(p, a);
	*p++ = ' ';
	p = gd_trace_hex(p, b);
	*p++ = ' ';
	p = gd_trace_hex(p, c);
	*p++ = '\r';
	*p++ = '\n';
	write(1, gd_trace_buf, (int)(p - gd_trace_buf));
}

#if GD_TRACE
#define gd_trace(t, a, b, c) gd_trace_always((t), (a), (b), (c))
#else
#define gd_trace(t, a, b, c) do { } while (0)
#endif

/*
 * Ask the host for `count` sectors starting at `lba`, landing at `dest`.
 *
 * This blocks in bb->loop() until the host has delivered the whole chunk and
 * answered RETV. That is the same wait the old single-shot path did, but it
 * now covers one chunk rather than an entire multi-megabyte request, and the
 * caller yields to the game as soon as it returns.
 */
static int ReadSectors(unsigned int dest, unsigned int lba, unsigned int count)
{
	command_3int_t *command =
		(command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
	unsigned int bytes = count * _GDS.sec_size;
	unsigned int sp;

	/* dcload runs on the TITLE's stack, so record where that stack is. If a
	 * disc read ever lands on it, or if the title drives it down into our own
	 * image, we would overwrite our return addresses mid-transfer. Measured
	 * 2026-08-10: it does not -- r15 never leaves 0x8c00f3xx. */
	__asm__ volatile ("mov r15,%0" : "=r" (sp));
	pm[PM_SP_LAST] = sp;
	if (sp < pm[PM_SP_LOW])
	{
		pm[PM_SP_LOW] = sp;
	}
	pm[PM_LAST_LBA] = lba;
	pm[PM_LAST_DEST] = dest;
	pm[PM_LAST_SECS] = count;
	pm[PM_READS]++;
	if (g_gd_park_longs > pm[PM_PARK_MAX])
	{
		pm[PM_PARK_MAX] = g_gd_park_longs;
	}

	if (overlaps_dcload(dest, bytes))
	{
		write(1, "CDFS read blocked: overlaps dcload at ", 38);
		write_hex8(dest);
		write(1, "\r\n", 2);
		return CMD_STAT_FAILED;
	}

	/*
	 * TRACE FIRST, THEN BUILD THE COMMAND. NOT THE OTHER WAY ROUND.
	 *
	 * write() is itself a syscall: it builds ITS command into pkt_buf and
	 * sends it. Tracing after the CDFSREAD has been laid down therefore
	 * overwrites it, and build_send_packet() then transmits the leftovers of
	 * the trace instead of the read request -- the host never sees the read at
	 * all, and dcload waits out its full timeout for an answer to something it
	 * never asked. Cost me a run to spot, in the very function whose header
	 * comment warns that pkt_buf is single and shared.
	 */
	if (dest >= GD_TRACE_DEST_FROM)
	{
		gd_trace_always('D', lba, dest, count);
	}

	memcpy(command->id, CMD_CDFSREAD, 4);
	command->value0 = htonl(lba);
	command->value1 = htonl(dest);
	command->value2 = htonl(bytes);

	/*
	 * FLUSH THE RX RING BEFORE STARTING A BURST.
	 *
	 * Found the hard way. Building with GD_TRACE=1 inserts a few full UDP
	 * round trips just before each request, and that alone carried Sonic
	 * Adventure past the read it had died on for the whole investigation --
	 * the three chunks of LBA 0x811bf went from "first one never arrives" to
	 * all three served and acknowledged with the exact byte count. The tracing
	 * was never the point; spending time inside bb->loop() was, because it
	 * empties the ring.
	 *
	 * Starting a 12-packet burst into a ring that still holds stale frames is
	 * what tips it over: the overflow desynchronises CAPR from CBR and
	 * reception stops for good (measured CAPR 6340 against CBR 2212). So drain
	 * first, deliberately, instead of relying on a diagnostic to do it.
	 *
	 * Bounded and clock-free: drain_iters counts poll iterations, not time,
	 * and nothing is awaited. Done BEFORE syscall_retval is armed, so a late
	 * reply landing during the drain cannot be mistaken for this read's.
	 */
	/* TRIED AND IT DOES NOT HELP: 256 and 50000 iterations both leave the
	 * failure exactly where it was. So the ring being dirty is NOT what the
	 * tracing was compensating for -- what mattered was the round trips
	 * themselves, and that is still unexplained. Left disabled rather than
	 * deleted: the mechanism is sound and costs nothing at 0, and the next
	 * person to suspect the ring should know this was already measured. */
	if (GD_DRAIN_ITERS > 0)
	{
		drain_iters = GD_DRAIN_ITERS;
		bb->loop(0);
		drain_iters = 0;
	}

	syscall_retval = (unsigned int)-1;
	timeout_loop = GD_SYSCALL_TIMEOUT_SECONDS;
	fine_deadline_start = TMU2_COUNT;
	fine_deadline_ticks = GD_READ_DEADLINE_TICKS;
	g_gd_in_transfer++;
	build_send_packet(sizeof(command_3int_t));
	bb->loop(0);
	g_gd_in_transfer--;
	fine_deadline_ticks = 0;
	timeout_loop = 0;

	/* 'E' = the wait returned. If 'D' appears with no 'E', dcload is still
	 * inside bb->loop() and the data never completed. */
	if (dest >= GD_TRACE_DEST_FROM)
	{
		gd_trace_always('E', syscall_retval, (unsigned int)timeout_loop,
				g_cdfs_read_fails);
	}

	if ((int)syscall_retval < 0)
	{
		g_cdfs_read_fails++;
		return CMD_STAT_FAILED;
	}

	g_cdfs_sync_chunks++;
	pm[PM_CHUNKS]++;
	return CMD_STAT_COMPLETED;
}

/*
 * Feed the CD-DA ring between the chunks of a disc read. A read of a level file
 * can take seconds, and cdda_service() declines during the read's own waits
 * (g_gd_in_transfer), so this is the only thing feeding the music meanwhile;
 * with it off the ring runs dry during level loads. 0 for an A/B.
 */
#ifndef GD_CDDA_BETWEEN_CHUNKS
#define GD_CDDA_BETWEEN_CHUNKS 1
#endif

/*
 * Serve a read in chunks of GD_EMU_ASYNC sectors, one host round trip each.
 * This is isoldr's data_transfer_emu_async() with our UDP request standing in
 * for its sector reader. Unlike isoldr, the title does not get control between
 * chunks (GD_YIELD_BETWEEN_CHUNKS is 0); it only does before a failed chunk is
 * retried.
 *
 * param[0] = LBA, param[1] = sectors remaining, param[2] = destination. All
 * three advance as we go, so a resumed transfer needs no side state.
 */
static void data_transfer_emu_async(void)
{
	unsigned int sc;
	unsigned int sc_size;
	int retries = 0;

	while ((_GDS.param[1] > 0) && (_GDS.cmd_abort == 0))
	{
		sc = (_GDS.param[1] <= (unsigned int)GD_EMU_ASYNC)
			? _GDS.param[1]
			: (unsigned int)GD_EMU_ASYNC;
		sc_size = _GDS.sec_size * sc;

		if (ReadSectors(_GDS.param[2], _GDS.param[0], sc) == CMD_STAT_FAILED)
		{
			/*
			 * RE-REQUEST THE SAME CHUNK, FROM THE NEXT ExecServer.
			 *
			 * Retrying inline would put a second build_send_packet() plus
			 * bb->loop() inside one syscall, and pkt_buf is single and shared
			 * -- that shape has reset the machine before. Yielding instead
			 * keeps the invariant of exactly one transmit per syscall, and it
			 * also gives the host time to abandon its own side and return to
			 * listening. Nothing is advanced, so the chunk is simply asked for
			 * again.
			 */
			g_cdfs_read_retries++;
			gd_trace_always('F', _GDS.param[0], _GDS.param[2],
					(unsigned int)retries);
			if (++retries > GD_READ_RETRIES)
			{
				_GDS.status = CMD_STAT_FAILED;
				_GDS.drv_stat = CD_STATUS_PAUSED;
				return;
			}
			gdcExitToGame();
			continue;
		}
		retries = 0;

		_GDS.param[1] -= sc;
		_GDS.transfered += sc_size;
		_GDS.lba = _GDS.param[0] + sc;

		if (_GDS.param[1] == 0)
		{
			_GDS.status = CMD_STAT_COMPLETED;
			_GDS.requested -= _GDS.transfered;
			_GDS.drv_stat = CD_STATUS_PAUSED;
			return;
		}

		_GDS.param[2] += sc_size;
		_GDS.param[0] += sc;

#if GD_CDDA_BETWEEN_CHUNKS
		/*
		 * Keep the music fed during a long read. This is the one safe point
		 * in the loop: the chunk is committed, its window is finished, the
		 * next request is not built yet, and g_gd_in_transfer is 0. It does
		 * not hand the title control, unlike GD_YIELD_BETWEEN_CHUNKS and
		 * GD_SERVICE_EVERY_SYSCALL, which both broke Sonic Adventure.
		 */
		cdda_service_between_chunks();
#endif

#if GD_YIELD_BETWEEN_CHUNKS
		/* isoldr's emu_async: the title gets its frame back here. */
		gdcExitToGame();
#endif
	}

	if (_GDS.cmd_abort)
	{
		_GDS.transfered = 0;
		_GDS.status = CMD_STAT_IDLE;
		_GDS.drv_stat = CD_STATUS_PAUSED;
	}
}

static void data_transfer(void)
{
	_GDS.ata_status = CMD_WAIT_IRQ;

	/*
	 * isoldr yields once before starting for non-KOS binaries, so the title
	 * observes PROCESSING for at least one poll instead of seeing a request
	 * satisfied within its own ReqCmd frame. Retail code is written against a
	 * drive that cannot possibly be that fast.
	 */
	gdcExitToGame();

	if (_GDS.param[1] == 0)
	{
		/* A zero-sector read is not an error; answering FAILED here would
		 * make a title think the drive died. */
		_GDS.status = CMD_STAT_COMPLETED;
		_GDS.drv_stat = CD_STATUS_PAUSED;
		_GDS.ata_status = CMD_WAIT_INTERNAL;
		return;
	}

	/*
	 * Single shot, blocking, in three cases -- isoldr's SD path exactly
	 * (loader/syscalls.c:542-545):
	 *
	 *   - chunking disabled;
	 *   - one sector, where slicing buys nothing;
	 *   - GD_BULK_SECTORS or more, and this is the one that is easy to miss.
	 *     isoldr's own comment: "It's looks like the game in loading state
	 *     (request big data), so we can increase general loading speed if load
	 *     it for one frame." A request that large means the title is sitting on
	 *     a loading screen; latency stops mattering and only throughput does,
	 *     so yielding between chunks is pure overhead.
	 *
	 * Sonic Adventure issues a 105-sector read immediately before the read it
	 * has always died on, so this is not a hypothetical difference.
	 */
#if GD_BULK_SECTORS > 0
	if ((GD_EMU_ASYNC == 0) || (_GDS.param[1] == 1) ||
	    (_GDS.param[1] >= (unsigned int)GD_BULK_SECTORS))
#else
	if ((GD_EMU_ASYNC == 0) || (_GDS.param[1] == 1))
#endif
	{
		_GDS.status = ReadSectors(_GDS.param[2], _GDS.param[0], _GDS.param[1]);
		if (_GDS.status == CMD_STAT_COMPLETED)
		{
			_GDS.transfered = _GDS.param[1] * _GDS.sec_size;
			_GDS.requested -= _GDS.transfered;
			_GDS.lba = _GDS.param[0] + _GDS.param[1];
		}
		_GDS.drv_stat = CD_STATUS_PAUSED;
		_GDS.ata_status = CMD_WAIT_INTERNAL;
		return;
	}

	data_transfer_emu_async();
	_GDS.ata_status = CMD_WAIT_INTERNAL;
}

/* Read TOC. The host writes the table straight into the title's buffer. */
static void GetTOC(void)
{
	command_3int_t *command =
		(command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

	memcpy(command->id, CMD_CDFSTOC, 4);
	command->value0 = htonl(_GDS.param[0]);	/* session / area */
	command->value1 = htonl(_GDS.param[1]);	/* destination */
	command->value2 = 0;

	syscall_retval = (unsigned int)-1;
	timeout_loop = GD_SYSCALL_TIMEOUT_SECONDS;
	fine_deadline_start = TMU2_COUNT;
	fine_deadline_ticks = GD_READ_DEADLINE_TICKS;
	g_gd_in_transfer++;
	build_send_packet(sizeof(command_3int_t));
	bb->loop(0);
	g_gd_in_transfer--;
	fine_deadline_ticks = 0;
	timeout_loop = 0;

	if ((int)syscall_retval < 0)
	{
		_GDS.err = CMD_ERR_HARDWARE;
		_GDS.status = CMD_STAT_FAILED;
		return;
	}

	/* 99 entries + first + last + leadout. */
	_GDS.transfered = 102 * 4;
	_GDS.status = CMD_STAT_COMPLETED;
}

static void get_ver_str(void)
{
	static const char drv_ver[] = "GDC Version 1.10 1999-03-31\2";

	if (_GDS.param[0])
	{
		memcpy((void *)_GDS.param[0], drv_ver, sizeof(drv_ver) - 1);
	}
	_GDS.transfered = sizeof(drv_ver) - 1;
	_GDS.status = CMD_STAT_COMPLETED;
}

/* SCD audio-status values a title reads out of CMD_GETSCD / CMD_REQ_STAT. */
#define SCD_AUDIO_NO_INFO   0x15u

/*
 * CMD_REQ_STAT writes four output words: repeat count, track, (CTRL/ADR<<24)|FAD
 * and index. While CD-DA is playing the FAD, track and repeat come from the
 * audio engine (which is the drive), so a title synchronising to the disc
 * position tracks the music; otherwise they are the data path's, as before.
 * CTRL/ADR is 0x01 for an audio track and 0x41 for data. isoldr's get_stat().
 */
static void get_stat(void)
{
	int st = cdda_state();

	if (st == CDDA_PLAYING || st == CDDA_PAUSED)
	{
		unsigned int lba = cdda_current_lba();
		unsigned int track = cdda_track();

		*((unsigned int *)_GDS.param[0]) = cdda_repeat();
		if (track)
		{
			*((unsigned int *)_GDS.param[1]) = track;
			*((unsigned int *)_GDS.param[2]) = (0x01U << 24) | lba;
		}
		else
		{
			*((unsigned int *)_GDS.param[1]) = _GDS.data_track;
			*((unsigned int *)_GDS.param[2]) = (0x41U << 24) | lba;
		}
		*((unsigned int *)_GDS.param[3]) = 1;
		_GDS.status = CMD_STAT_COMPLETED;
		return;
	}

	*((unsigned int *)_GDS.param[0]) = 0;
	*((unsigned int *)_GDS.param[1]) = _GDS.data_track;
	*((unsigned int *)_GDS.param[2]) = (0x41U << 24) | _GDS.lba;
	*((unsigned int *)_GDS.param[3]) = 1;
	_GDS.status = CMD_STAT_COMPLETED;
}

/*
 * CMD_GETSCD -- the Q subcode of the sector being played. A title that reads
 * its playback position from here, rather than from CMD_REQ_STAT, gets nothing
 * unless this is served: force-completing it (as this loader used to) writes no
 * data and the title sees a stale buffer. Format 1 is the 14-byte Q block;
 * anything else gets a zeroed all-subcode blob with the audio status patched
 * in. isoldr's get_scd() (loader/syscalls.c:247).
 */
static void get_scd(void)
{
	unsigned char *buf = (unsigned char *)_GDS.param[2];
	unsigned int fmt = _GDS.param[0];
	int st = cdda_state();
	int playing = (st == CDDA_PLAYING || st == CDDA_PAUSED);
	unsigned int lba = playing ? cdda_current_lba() : _GDS.lba;
	unsigned int track = playing ? cdda_track() : 0;
	unsigned int astat = playing ? cdda_audio_status() : SCD_AUDIO_NO_INFO;
	unsigned int offset = lba - 150;

	if (fmt == 1)
	{
		buf[0] = 0x00;
		buf[1] = (unsigned char)astat;
		buf[2] = 0x00;
		buf[3] = 0x0e;
		if (track)
		{
			buf[4] = 0x01;              /* audio CTRL/ADR */
			buf[5] = (unsigned char)track;
			buf[6] = (unsigned char)track;
		}
		else
		{
			buf[4] = 0x41;              /* data CTRL/ADR */
			buf[5] = (unsigned char)_GDS.data_track;
			buf[6] = (unsigned char)_GDS.data_track;
		}
		buf[7] = (unsigned char)((offset >> 16) & 0xff);
		buf[8] = (unsigned char)((offset >> 8) & 0xff);
		buf[9] = (unsigned char)(offset & 0xff);
		buf[10] = 0x00;
		buf[11] = (unsigned char)((lba >> 16) & 0xff);
		buf[12] = (unsigned char)((lba >> 8) & 0xff);
		buf[13] = (unsigned char)(lba & 0xff);
		_GDS.transfered = 14;
	}
	else
	{
		unsigned int i;

		for (i = 0; i < 100u; i++)
		{
			buf[i] = 0;
		}
		buf[1] = (unsigned char)astat;
		_GDS.transfered = 100;
	}
	_GDS.status = CMD_STAT_COMPLETED;
}

static void get_session_info(void)
{
	unsigned char *buf = (unsigned char *)_GDS.param[2];
	unsigned int lba = _GDS.lba;

	buf[0] = CD_STATUS_PAUSED;
	buf[1] = 0;
	buf[2] = 1;
	buf[3] = (lba >> 16) & 0xFF;
	buf[4] = (lba >> 8) & 0xFF;
	buf[5] = lba & 0xFF;
	_GDS.transfered = 6;
	_GDS.status = CMD_STAT_COMPLETED;
}

static void reset_GDS(void)
{
	_GDS.cmd = 0;
	_GDS.status = CMD_STAT_IDLE;
	_GDS.ata_status = CMD_WAIT_INTERNAL;
	_GDS.err = CMD_ERR_NOERROR;
	_GDS.requested = 0;
	_GDS.transfered = 0;
	_GDS.cmd_abort = 0;
	_GDS.callback = 0;
	_GDS.callback_param = 0;
	memset(_GDS.param, 0, sizeof(_GDS.param));
	_GDS.lba = 150;
	_GDS.req_count = 0;
	_GDS.data_track = 1;
	_GDS.drv_stat = CD_STATUS_PAUSED;
	_GDS.drv_media = CD_GDROM;
	_GDS.sec_size = 2048;
	_GDS.mode = 1024;
	_GDS.flags = 8192;
}

/*
 * The server task. Entered once from gdGdcInitSystem (or lazily from the
 * first gdGdcExecServer) and never returns -- gdcExitToGame() is how control
 * leaves this function.
 */
void gdcServerMain(void)
{
	/*
	 * Deliberately no reset here. _GDS is already valid (see its initialiser)
	 * and the server can be started lazily by the first ExecServer, by which
	 * time a command may already be queued. Resetting would drop it, and the
	 * title would be left polling a channel that no longer exists. Explicit
	 * re-initialisation is what gdGdcReset is for.
	 */
	while (1)
	{
		/* Where isoldr calls CDDA_MainLoop() from: the top of the
		 * server's dispatch loop (loader/syscalls.c:866). Top level of
		 * a syscall, so the fetch inside it may transmit. */
		cdda_service();

		if (_GDS.status == CMD_STAT_PROCESSING)
		{
			switch (_GDS.cmd)
			{
			case CMD_PIOREAD:
			case CMD_DMAREAD:
				data_transfer();
				break;
			case CMD_GETTOC:
			case CMD_GETTOC2:
				GetTOC();
				break;
			case CMD_GET_VERS:
				get_ver_str();
				break;
			case CMD_REQ_STAT:
				get_stat();
				break;
			case CMD_GETSES:
				get_session_info();
				break;
			/* CDDA. The engine is the drive: see cdda.h.
			 *
			 * A REFUSAL IS STILL COMPLETED. isoldr force-completes what
			 * it does not model rather than failing it, because a title
			 * that gets FAILED for a command it considers routine
			 * usually gives up entirely -- and "the music did not
			 * start" must never become "the game did not start". So a
			 * play the engine cannot honour leaves the drive looking
			 * like it is spinning, exactly as this did before there was
			 * an engine at all. */
			case CMD_PLAY_TRACKS:
				(void)cdda_play_tracks(_GDS.param[0], _GDS.param[1],
						       _GDS.param[2]);
				_GDS.drv_stat = CD_STATUS_PLAYING;
				_GDS.status = CMD_STAT_COMPLETED;
				break;
			case CMD_PLAY_SECTORS:
				(void)cdda_play_sectors(_GDS.param[0], _GDS.param[1],
							_GDS.param[2]);
				_GDS.drv_stat = CD_STATUS_PLAYING;
				_GDS.status = CMD_STAT_COMPLETED;
				break;
			case CMD_RELEASE:
				(void)cdda_release();
				_GDS.drv_stat = CD_STATUS_PLAYING;
				_GDS.status = CMD_STAT_COMPLETED;
				break;
			case CMD_PAUSE:
				(void)cdda_pause();
				_GDS.drv_stat = CD_STATUS_PAUSED;
				_GDS.status = CMD_STAT_COMPLETED;
				break;
			case CMD_STOP:
				(void)cdda_stop();
				_GDS.drv_stat = CD_STATUS_PAUSED;
				_GDS.status = CMD_STAT_COMPLETED;
				break;
			case CMD_SEEK:
				/* A seek while music is playing repositions the
				 * stream; a seek at any other time is the data
				 * path's and costs nothing to complete. */
				(void)cdda_seek(_GDS.param[0]);
				_GDS.status = CMD_STAT_COMPLETED;
				break;
			case CMD_GETSCD:
				/* The Q subcode, served from the audio engine's
				 * position while music plays -- see get_scd(). */
				get_scd();
				break;
			case CMD_INIT:
			case CMD_NOP:
			case CMD_REQ_MODE:
			case CMD_SET_MODE:
			default:
				/* isoldr force-completes anything it does not model rather
				 * than failing it: a title that gets FAILED for a command it
				 * considers routine will usually give up entirely. */
				_GDS.status = CMD_STAT_COMPLETED;
				break;
			}
		}

		gdcExitToGame();
	}
}

/*
 * Queue a command. No I/O happens here -- that is the entire difference from
 * the previous implementation.
 */
/*
 * Where the TITLE calls us from, reported only when it changes.
 *
 * The game dies of its own accord about 200 ms after issuing one particular
 * read, in its own code and on its own stack, so the useful question is which
 * of its call sites is live at that moment. PR at syscall entry is the return
 * address into the game. Emitting it on every call would be 60 events a second
 * and would drown the link; emitting it only on a change is a handful of events
 * per session and names each new path the moment it appears.
 */
static unsigned int gd_last_pr;

/* The PR must be captured in the SYSCALL function itself and passed in. Taken
 * inside this helper, __builtin_return_address(0) yields the helper's own
 * caller -- which is how the first version of this reported addresses inside
 * dcload (gdGdcReqCmd+0x18) instead of inside the game. */
/*
 * Also report the STACK the title called us on, because dcload runs on it.
 * bb->loop() and the whole packet path go several hundred bytes deep, and if a
 * title calls a GD syscall on a small private stack rather than the boot stack
 * we would be writing that depth into memory it owns. Reported on change, same
 * as the call site.
 */
static unsigned int gd_last_sp;

/*
 * THE FOOTPRINT GUARD, always compiled, no round trip.
 *
 * A retail title uses the BIOS work area as a stack because from its point of
 * view that is free memory, and that area is where this loader lives (§4.6).
 * When its stack descends past _end it writes into dcload's BSS, and what comes
 * out is a request this loader built out of clobbered state -- a ReadSector
 * with a size of 0xa2900000, say, which the host cannot even parse.
 *
 * The whole diagnosis used to be inference from a symptom. It is two integers:
 * the lowest stack pointer any GD syscall was entered with, and how many of
 * them landed inside the image. g_gd_sp_min against _end is the margin, and it
 * is readable while the title is still healthy -- which is the point, because
 * after the overlap nothing this loader reports can be trusted.
 */
extern char dcload_base[];
extern char end[];

unsigned int g_gd_sp_min = 0xffffffffU;
unsigned int g_gd_sp_in_image = 0;

static void gd_note_caller(unsigned int pr)
{
	unsigned int sp;

	__asm__ volatile ("mov r15,%0" : "=r" (sp));

	if (sp < g_gd_sp_min)
	{
		g_gd_sp_min = sp;
	}
	if ((sp >= (unsigned int)dcload_base) && (sp < (unsigned int)end))
	{
		g_gd_sp_in_image++;
	}

	/*
	 * OFF unless explicitly asked for. This costs a full UDP round trip per
	 * event, and Sonic Adventure alternates between two stacks on every GD
	 * syscall -- so it fired about 120 times a second and became the thing
	 * holding the title back. It did its job: it is how the stack overlap in
	 * 20.13 was found. Leave it disabled.
	 */
	if (!GD_TRACE_CALLER)
	{
		(void)pr;
		return;
	}

	if (pr != gd_last_pr)
	{
		gd_last_pr = pr;
		gd_trace_always('P', pr, sp, 0);
	}
	/* Coarse: only report a genuinely different stack region. */
	if ((sp & 0xffff0000u) != (gd_last_sp & 0xffff0000u))
	{
		gd_last_sp = sp;
		gd_trace_always('K', sp, pr, 0);
	}
}

/*
 * GD_SERVICE_EVERY_SYSCALL -- A DIAGNOSTIC.
 *
 * bb->loop() is reached from the READ PATH ONLY (data_transfer below). Every
 * other syscall answers out of state and returns, so a title that busy-waits on
 * ReqCmd/GetCmdStat/GetDrvStat without ever completing a read leaves this
 * loader running constantly and ANSWERING NOTHING: no VERS, no ping, no SBIQ.
 * That silence is indistinguishable from a dead loader, which is exactly the
 * hole this closes -- with it on, the counters in g_gd_idx_counts[] become
 * readable in the one situation where they say the most.
 *
 * Bounded by drain_iters so a call costs a fixed number of poll iterations, and
 * skipped whenever the GD path is already held, so it can never insert a
 * transmit into the middle of a live transfer (the pkt_buf invariant at the top
 * of this file).
 *
 * AND THAT REASONING IS NOT SUFFICIENT -- MEASURED, 2026-08-29.
 *
 * Sonic Adventure dies with this on. Isolated to this flag alone: same base
 * (0x8cfe8000), same feature set, same host, two loader sets differing in
 * nothing else. With it off the title boots and plays; with it on the console
 * goes silent a second or two after the title's first big read, with no
 * exception, no further disc request and nothing logged at either end -- the
 * same ending as the GAPS bridge in AGENTS.md 4.12, reached some other way.
 *
 * The mechanism is NOT known. What is known is that "the lock is free, so a
 * transmit here is safe" describes the GD state machine and nothing else: this
 * hands the whole packet path -- link-change handling, ring re-init, any
 * command the host happens to have in flight -- a context at the top of a
 * syscall that a title enters sixty times a second. The claim above is about
 * one hazard out of that set.
 *
 * So: OFF, and it is a last resort rather than a default diagnostic. If it is
 * ever revisited, GD_SERVICE_ITERS (256) and restricting the servicing to a
 * single syscall are the two cheapest things to narrow it with.
 */
#if GD_SERVICE_EVERY_SYSCALL
#ifndef GD_SERVICE_ITERS
#define GD_SERVICE_ITERS 256
#endif
static void gd_service_net(void)
{
	if (gd_lock())
	{
		return; /* a transfer owns the wire -- stay off it */
	}
	gd_unlock();

	drain_iters = GD_SERVICE_ITERS;
	bb->loop(0);
	drain_iters = 0;
}
#else
#define gd_service_net() do { } while (0)
#endif

int gdGdcReqCmd(int cmd, int *param)
{
	int gd_chn = GDC_CHN_ERROR;
	int i;
	int n;

	g_gd_idx_counts[0]++;
	gd_service_net();
	gd_note_caller((unsigned int)__builtin_return_address(0));

	if ((cmd >= 0) && (cmd <= CMD_MAX))
	{
		g_gd_cmd_counts[cmd]++;
	}

	if ((cmd < 0) || (cmd > CMD_MAX) || gd_take(1))
	{
		return gd_chn;
	}

	if (_GDS.status == CMD_STAT_IDLE)
	{
		/* The BIOS never hands out 0 or 1 as a channel. Games compare the
		 * value they were given against the one the driver is serving, so
		 * the exact sequence matters. */
		if (_GDS.req_count++ == 0)
		{
			_GDS.req_count++;
		}

		_GDS.cmd = cmd;
		_GDS.status = CMD_STAT_PROCESSING;
		_GDS.transfered = 0;
		_GDS.ata_status = CMD_WAIT_INTERNAL;
		_GDS.cmd_abort = 0;
		_GDS.err = CMD_ERR_NOERROR;

		gd_chn = _GDS.req_count;

		n = get_params_count(cmd);
		for (i = 0; i < n; i++)
		{
			_GDS.param[i] = (unsigned int)param[i];
		}

		if (is_transfer_cmd(cmd))
		{
			_GDS.requested = _GDS.param[1] * _GDS.sec_size;
			_GDS.drv_stat = CD_STATUS_PLAYING;
		}
		gd_trace('R', (unsigned int)cmd, _GDS.param[0], _GDS.param[1]);
	}
	else
	{
		/* Refused: the previous command has not been collected yet. */
		gd_trace('X', (unsigned int)cmd, (unsigned int)_GDS.status,
			 (unsigned int)_GDS.req_count);
	}

	gd_give();
	return gd_chn;
}

/*
 * Report progress on a channel. Only the most recent command is addressable:
 * that is the BIOS behaviour, and a title that polls a stale channel is
 * telling us it lost track, not that it wants an answer.
 */
int gdGdcGetCmdStat(int gd_chn, int *status)
{
	int rv = CMD_STAT_IDLE;

	g_gd_idx_counts[1]++;
	gd_service_net();
	gd_lock_watchdog();

	if (gd_take(2))
	{
		return CMD_STAT_BUSY;
	}

	status[0] = 0;
	status[1] = 0;
	status[2] = 0;
	status[3] = 0;

	if ((gd_chn == 0) || (gd_chn != _GDS.req_count))
	{
		status[0] = CMD_ERR_ILLEGALREQUEST;
		gd_give();
		return CMD_STAT_FAILED;
	}

	switch (_GDS.status)
	{
	case CMD_STAT_PROCESSING:
		status[2] = _GDS.transfered;
		status[3] = _GDS.ata_status;
		rv = CMD_STAT_PROCESSING;
		break;

	case CMD_STAT_COMPLETED:
		if (_GDS.err)
		{
			status[0] = _GDS.err;
			rv = CMD_STAT_FAILED;
		}
		else
		{
			rv = CMD_STAT_COMPLETED;
			_GDS.ata_status = CMD_WAIT_INTERNAL;
		}
		/* Consumed: the channel goes idle so the next ReqCmd is accepted. */
		_GDS.status = CMD_STAT_IDLE;
		status[2] = _GDS.transfered;
		status[3] = _GDS.ata_status;
		break;

	case CMD_STAT_FAILED:
		status[0] = CMD_ERR_HARDWARE;
		rv = CMD_STAT_FAILED;
		_GDS.status = CMD_STAT_IDLE;
		_GDS.ata_status = CMD_WAIT_INTERNAL;
		break;

	default:
		break;
	}

	gd_give();
	gd_trace('S', (unsigned int)gd_chn, (unsigned int)rv,
		 (unsigned int)status[2]);
	return rv;
}

int gdGdcGetDrvStat(int *status)
{
	g_gd_idx_counts[4]++;
	gd_service_net();
	gd_note_caller((unsigned int)__builtin_return_address(0));
	gd_lock_watchdog();

	/* isoldr's other no-IRQ context (loader/syscalls.c:1067), and the one
	 * that carries the stream on this transport: a title's frame loop calls
	 * this about sixty times a second while it calls the server only when
	 * it wants something. Before the lock, because the lock is the GD
	 * command state and the audio stream is not part of it. */
	cdda_service();

	if (gd_take(3))
	{
		return CMD_STAT_BUSY;
	}

	/* PLAYING while there is music, whatever the data path is doing: a
	 * title polls this to know whether its own PLAY took. */
	if (cdda_state() == CDDA_PLAYING)
	{
		_GDS.drv_stat = CD_STATUS_PLAYING;
	}
	status[0] = _GDS.drv_stat;
	status[1] = _GDS.drv_media;

	gd_give();
	return 0;
}

int gdGdcChangeDataType(int *param)
{
	g_gd_idx_counts[10]++;

	if (!param)
	{
		return -1;
	}
	if (gd_take(4))
	{
		return CMD_STAT_BUSY;
	}

	if (param[0] == 0)
	{
		_GDS.flags = (unsigned int)param[1];
		_GDS.mode = (unsigned int)param[2];
		_GDS.sec_size = (unsigned int)param[3];
	}
	else
	{
		param[1] = (int)_GDS.flags;
		param[2] = (int)_GDS.mode;
		param[3] = (int)_GDS.sec_size;
	}

	gd_give();
	return 0;
}

int gdGdcReset(void)
{
	g_gd_idx_counts[9]++;
	reset_GDS();
	g_gd_lock_owner = 5;
	gd_give();
	return 0;
}

int gdGdcReadAbort(int gd_chn)
{
	g_gd_idx_counts[8]++;

	if (gd_chn != _GDS.req_count)
	{
		return -1;
	}
	if (_GDS.cmd_abort)
	{
		return -1;
	}

	switch (_GDS.status)
	{
	case CMD_STAT_PROCESSING:
	case CMD_STAT_STREAMING:
	case CMD_STAT_BUSY:
		_GDS.cmd_abort = 1;
		return 0;
	default:
		return 0;
	}
}

/*
 * DMA plumbing. Data has already landed at its destination by the time a
 * transfer completes, so a requested DMA is finished the moment it is asked
 * for. Signatures must stay BIOS-compatible; titles do validate the channel.
 */
void gdGdcG1DmaEnd(unsigned int func, unsigned int param)
{
	g_gd_idx_counts[5]++;

	if (func)
	{
		void (*callback)(unsigned int) = (void (*)(unsigned int))func;
		callback(param);
	}
}

int gdGdcReqDmaTrans(int gd_chn, unsigned int *dmabuf)
{
	g_gd_idx_counts[6]++;

	if (!dmabuf || (gd_chn != _GDS.req_count))
	{
		return -1;
	}
	if (_GDS.requested < dmabuf[1])
	{
		return -1;
	}
	_GDS.requested -= dmabuf[1];
	return 0;
}

int gdGdcCheckDmaTrans(int gd_chn, unsigned int *size)
{
	g_gd_idx_counts[7]++;

	if (gd_chn != _GDS.req_count)
	{
		return -1;
	}
	if (size)
	{
		*size = _GDS.requested;
	}
	return 0;
}

void gdGdcSetPioCallback(unsigned int func, unsigned int param)
{
	g_gd_idx_counts[11]++;
	_GDS.callback = func;
	_GDS.callback_param = param;
}

int gdGdcReqPioTrans(int gd_chn, int *piobuf)
{
	g_gd_idx_counts[12]++;

	if (!piobuf || (gd_chn != _GDS.req_count))
	{
		return -1;
	}
	return 0;
}

int gdGdcCheckPioTrans(int gd_chn, int *size)
{
	g_gd_idx_counts[13]++;

	if (gd_chn != _GDS.req_count)
	{
		return -1;
	}
	if (size)
	{
		*size = (int)_GDS.requested;
	}
	return 0;
}

void gdGdcChangeDisc(int disc_num)
{
	g_gd_idx_counts[16]++;
	(void)disc_num;
}

void gdGdcCartRead(void *params)
{
	g_gd_idx_counts[17]++;
	(void)params;
}

int gdGdcDummy(int gd_chn, int *arg2)
{
	(void)gd_chn;
	(void)arg2;
	return 0;
}

