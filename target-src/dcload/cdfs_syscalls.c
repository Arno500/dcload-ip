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
#include "hiram.h"
#include "irq.h"
#include "memfuncs.h"
#include "g2dma.h"

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
#define CMD_DMAREAD_STREAM     28
#define CMD_NOP                29
#define CMD_REQ_MODE           30
#define CMD_SET_MODE           31
#define CMD_STOP               33
#define CMD_GETSCD             34
#define CMD_GETSES             35
#define CMD_REQ_STAT           36
#define CMD_PIOREAD_STREAM     37
#define CMD_DMAREAD_STREAM_EX  38
#define CMD_PIOREAD_STREAM_EX  39
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
 * 250 ms, AND IT IS NOW THE PRIMARY RECOVERY, not a backstop (2026-09-20).
 *
 * It used to be 1.2 s, sized to "wait past the host rather than race it"
 * because the host gave up on a transfer after ~0.7 s. That premise is gone:
 * since send_sectors() the host does not wait for anything on this path, so
 * there is no give-up to wait past -- it sends the window, the parts and the
 * ReturnValue and moves on. Nothing but this deadline notices an answer that
 * never arrives.
 *
 * A healthy 16 KB chunk takes ~1.4 ms, so 250 ms is 175x the normal case and
 * still leaves four retries inside a second. Measured on Crazy Taxi,
 * 2026-09-20: with the deadline unable to fire (see below) one lost answer
 * froze the title for 7.0 s, which is the coarse backstop, and the player saw
 * the game stop dead. Lower it with the same acceptance test as the host's
 * pacing (g_cdfs_read_fails, _holes, _retries, g_rx_overflow, g_rx_missed).
 *
 * THIS DEADLINE ONLY WORKS IF TMU2 RUNS. Until 2026-09-20 nothing started it
 * unless a title played CD-DA (cdda.c) or ISOLDR_SETUP_MACHINE was set, so for
 * a title that streams its music as data -- Crazy Taxi -- neither this nor the
 * lock watchdog could ever expire, and every lost chunk cost the full 7 s.
 * gd_deadline_timer_start() is called from main() now.
 */
#ifndef GD_READ_DEADLINE_TICKS
#define GD_READ_DEADLINE_TICKS 3125000u
#endif

/* The adapter loop's seconds timeout for GD waits: a coarse backstop behind
 * GD_READ_DEADLINE_TICKS. */
#define GD_SYSCALL_TIMEOUT_SECONDS 6

/* How many times a chunk may be re-requested before the read is failed. */
#define GD_READ_RETRIES 4

/*
 * The same with the MMU on, i.e. under Windows CE: 20 x 250 ms = 5 s. A Katana
 * title handed FAILED asks again; CE's GD driver gives the read up, and the
 * title stops with a black screen. The ring has been measured delivering
 * nothing for ~1 s under CE (docs/wince-investigation.md 7l), which four
 * retries barely outlast.
 */
#define GD_READ_RETRIES_MMU 20

/*
 * TMU2, the free-running deadline clock: Pck/4 = 12.5 MHz counting down from
 * 0xffffffff, so `start - TCNT2` is elapsed ticks (adapter.h).
 *
 * It lives here rather than in cdda.c because the read deadline and the lock
 * watchdog depend on it and both are always compiled, while CD-DA is optional
 * -- which is exactly how a title that plays no music came to have no working
 * deadline at all. cdda.c calls this instead of keeping its own copy.
 *
 * Idempotent on purpose: restarting it between the chunks of a disc read would
 * break that read's deadline and every mark in flight, so a timer already
 * running as programmed is left alone.
 *
 * AND A KOS TITLE'S TIMER IS LEFT ALONE TOO (2026-10-02). KOS runs TMU2 at
 * Pck/4 with a 1 s reload for its millisecond clock; reprogramming it from
 * cdda.c would stop KOS's seconds and break its timer_ms_gettime64(). Any
 * Pck/4 timer whose period outlasts every interval measured here (838 ms,
 * CDDA_GAP_LIMIT_TICKS) will do: tmu2_since() follows the reload.
 */
#define TMU2_MIN_PERIOD 12000000u	/* 0.96 s */
#define TMU_TSTR       (*(volatile unsigned char *)0xffd80004)
#define TMU_TCOR2      (*(volatile unsigned int *)0xffd80020)
#define TMU_TCR2       (*(volatile unsigned short *)0xffd80028)
#define TMU_START_TMU2 0x04
#define TMU_TCR_PCK4   0

void gd_deadline_timer_start(void)
{
	if ((TMU_TSTR & TMU_START_TMU2) != 0u && TMU_TCOR2 >= TMU2_MIN_PERIOD
	    && (TMU_TCR2 & 0x7u) == TMU_TCR_PCK4)
	{
		return;
	}
	TMU_TSTR = (unsigned char)(TMU_TSTR & ~TMU_START_TMU2);
	TMU_TCR2 = TMU_TCR_PCK4;
	TMU_TCOR2 = 0xffffffffu;
	TMU2_COUNT = 0xffffffffu;
	TMU_TSTR = (unsigned char)(TMU_TSTR | TMU_START_TMU2);
}

/*
 * STOP THE REAL DRIVE ONCE, AT BOOT (WITH_GD_SPINDOWN).
 *
 * Nothing in a dcload session reads the disc again. A title's GD syscalls are
 * answered from the host (cdfs_redir.s), CD-DA comes off the network too, and
 * a homebrew uploaded over UDP never touches the drive at all. The disc the
 * BIOS spun up to boot 1st_read.bin therefore keeps turning for nothing.
 *
 * WHAT THIS IS WORTH, honestly: the drive's own firmware already stops it.
 * The SPI mode page's Standby Time defaults to B4h = 180 s of <PAUSE> before
 * the unit goes to <STANDBY> (Cdif131e.txt, "Standby Time (Byte 4-5)"), and
 * the spec's own note is that a large value hurts MTBF. So this buys three
 * minutes of rotation per boot, immediately and audibly -- not hours -- and it
 * makes the stopped state deterministic instead of something the next command
 * would postpone.
 *
 * HOW. Through the BIOS syscall vector, which main() has just pointed back at
 * the BIOS (cdfs_redir_save/cdfs_redir_disable) -- so this must be called
 * after those two, and it is the only place in the loader that calls the real
 * driver. The vector's ABI is the one cdfs_redir.s decodes for a title:
 * r7 = function index (0 ReqCmd, 1 GetCmdStat, 2 ExecServer, 3 InitSystem),
 * r6 = 0, because r6 = -1 selects a misc syscall.
 *
 * The BIOS driver is a coroutine (see the header of this file): ReqCmd only
 * queues the command, and it progresses only while we call ExecServer. The
 * loop below is therefore not a poll of the drive, it is the driver's own
 * scheduler -- and leaving before the command completes leaves it queued in a
 * driver nobody will ever run again.
 *
 * InitSystem IS THE FALLBACK, NOT THE FIRST MOVE. KOS's cdrom_init() opens
 * with it and loader.s does zero-fill 0x8c004000-0x8c010000 behind the BIOS,
 * so there is a case for it -- but ReqCmd, ExecServer and GetCmdStat are
 * non-blocking by construction (a title calls ExecServer every frame), and
 * InitSystem is the one call here that may bring hardware up. The BIOS just
 * read the disc with this driver, so the ordinary path never needs it; it is
 * tried only if the driver refuses the command.
 *
 * BOUNDED, like every other hardware wait (AGENTS.md 4.8) -- on a spin count,
 * in the style of RTL_LINK_SPIN_LIMIT and MAPLE_DMA_SPIN_LIMIT, rather than on
 * TMU2, which is not running this early and whose deadline would cost more of
 * the footprint (4.6) than this whole function is worth. What one ExecServer
 * costs here has not been measured: the bound is on iterations, not on time.
 * A drive that will not answer is left spinning rather than spun on -- three
 * minutes of firmware standby is a better outcome than a loader that never
 * reaches its command loop.
 *
 * REVERSIBLE. <STANDBY> is a drive state, not a shutdown: a title run without
 * a disc image, or a KOS program calling cdrom_init(), spins it back up, at
 * the cost of that first read's spin-up latency.
 */
#if WITH_GD_SPINDOWN

typedef int (*gd_syscall_t)(int, int, int, int);

/* The BIOS GD syscall entry point, read through P2: the word is written by
 * the BIOS and by cdfs_redir_enable/disable, and the caches are off here
 * anyway. */
#define GD_SYSCALL_VECTOR (*(volatile gd_syscall_t *)0xac0000bcu)

/* ExecServer calls one STOP may take. It answers in a handful; this is the
 * "the drive is not answering at all" bound, in the style of the loader's
 * other hardware waits (RTL_LINK_SPIN_LIMIT, MAPLE_DMA_SPIN_LIMIT). */
#define GD_SPINDOWN_SPIN_LIMIT 200000u

/*
 * How it went, for --diag: the final command status + 2, so that 0 still
 * means the call never ran -- 1 FAILED, 2 IDLE, 4 COMPLETED (what a drive
 * that stopped reports), 5 STREAMING -- plus 7 "the driver would not take the
 * command" and 8 "it never finished". A loader built with WITH_GD_SPINDOWN=0
 * reports 0, as does one whose syscall vector was empty.
 */
unsigned int g_gd_spindown;

void gd_spin_down_drive(void)
{
	gd_syscall_t gd = GD_SYSCALL_VECTOR;
	int status[GDC_PARAMS_COUNT];
	unsigned int spins = GD_SPINDOWN_SPIN_LIMIT;
	int chn;
	int st;

	if (!gd)
		return;

	chn = gd(CMD_STOP, 0, 0, 0);		/* gdGdcReqCmd(CMD_STOP, NULL) */
	if (chn == GDC_CHN_ERROR)
	{
		(void)gd(0, 0, 0, 3);		/* gdGdcInitSystem(), see above */
		chn = gd(CMD_STOP, 0, 0, 0);
	}
	if (chn == GDC_CHN_ERROR)
	{
		g_gd_spindown = 7;
		return;
	}

	do
	{
		(void)gd(0, 0, 0, 2);		/* gdGdcExecServer() */
		st = gd(chn, (int)status, 0, 1);	/* gdGdcGetCmdStat() */
		if ((st != CMD_STAT_PROCESSING) && (st != CMD_STAT_BUSY))
		{
			g_gd_spindown = (unsigned int)(st + 2);
			return;
		}
	}
	while (--spins);

	g_gd_spindown = 8;
}

#endif /* WITH_GD_SPINDOWN */

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

/* With GD_TRACE, also trace where a title's virtual TOC buffer lands (the
 * M/U/P/V lines, below GetTOC's staging buffer). Eight more round trips per
 * boot, each a write() that waits for its RETV with no deadline: one lost
 * packet there hung a Sega Rally 2 run for good (2026-09-27). */
#ifndef GD_TRACE_VADDR
#define GD_TRACE_VADDR 0
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

/* What a disc read must never land on is the loader serving it -- and where
 * that is comes from the linker (overlaps_dcload()), not from constants. */

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
/*
 * Chunks whose ReturnValue arrived over a window with a hole in it.
 *
 * The host no longer waits for the LoadBinary echo and no longer probes with
 * DoneBinary before answering a disc read (both were ~1.9 ms of frozen title
 * per 16 KB chunk out of 5). What it gave up is the only two ways IT could
 * have learned that a packet went missing, so the loader has to say so itself
 * -- which costs nothing, because bin_window_complete() reads a map that is
 * maintained anyway. A hole here fails the chunk and data_transfer_emu_async()
 * asks for it again.
 *
 * Distinct from g_cdfs_read_fails on purpose: fails means the host never
 * answered, holes means it answered and the answer was short. Different ends
 * of the link, different fix.
 */
unsigned int g_cdfs_read_holes;
/* ReturnValues that arrived over an incomplete window and were waited past
 * (ReadSectors): the late answer to an earlier attempt, most likely. */
unsigned int g_cdfs_read_stale;
/* Longs of server stack parked at the last yield. saved_regs[] holds 96, of
 * which 11 go to registers and the count, so anything approaching 80 here
 * means the buffer needs enlarging before it silently overruns. */
unsigned int g_gd_park_longs;

/*
 * Non-zero for a KOS binary: written by the host before EXEC, by symbol name
 * (dcload-ip-rs). A read then starts in the ExecServer that picks it up,
 * without the first yield -- as isoldr does for BIN_TYPE_KOS (syscalls.c).
 *
 * KOS's DMA read (cdrom_read_sectors_dma_irq) polls once; on PROCESSING it
 * sleeps on a semaphore only the G1 DMA-end interrupt or its vblank handler
 * signals. Nothing raises the first, so every read ran from the second: the
 * network exchange inside a vblank interrupt. Measured 2026-10-02 on the GTA
 * III port: two reads served that way, then the title never asked again.
 *
 * It also sends KOS's console writes as DC25 (syscalls.c) and masks the
 * adapter waits (bb_irq_hold(), adapter.h): the host points KOS's console here.
 */
unsigned int g_gd_kos;

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

	/* TMU2 counts DOWN; tmu2_since() is right across its reload (adapter.h). */
	if (tmu2_since(gd_stuck_since) < GD_LOCK_STUCK_TICKS)
	{
		return;
	}

	g_gd_lock_stuck++;
	g_gd_lock_stuck_owner = g_gd_lock_owner;
	g_gd_lock_stuck_ticks = tmu2_since(gd_stuck_since);
	gd_stuck_armed = 0;
	gd_give();
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

static int is_stream_cmd(int cmd)
{
	return (cmd == CMD_DMAREAD_STREAM) || (cmd == CMD_PIOREAD_STREAM)
	    || (cmd == CMD_DMAREAD_STREAM_EX) || (cmd == CMD_PIOREAD_STREAM_EX);
}

static int is_transfer_cmd(int cmd)
{
	return (cmd == CMD_PIOREAD) || (cmd == CMD_DMAREAD) || is_stream_cmd(cmd);
}

static unsigned int sh4_phys_addr(unsigned int addr)
{
	return addr & 0x1fffffffU;
}

/*
 * WHERE DISC DATA LANDS WHEN THE TITLE'S BUFFER CANNOT BE HANDED TO THE HOST
 * (2026-09-27).
 *
 * The host writes through cmd_partbin, whose copies (memfuncs.c) store at
 * src + memdiff(dst, src) with both addresses masked to 29 bits: into the
 * SOURCE's segment. With the MMU off that reaches the right RAM -- every
 * address a Katana title hands us is RAM in any segment -- but through P1's
 * cache, which cmd_partbin purges for a P2 buffer too (GTA2, 2026-10-02). With
 * it on (Windows CE), a
 * virtual buffer becomes P1 of a number that is not its physical address:
 * 0x080df2e0 went to area 2, which is empty (docs/wince-investigation.md 7f).
 * So a read into a translated address is received here and copied to the
 * title with memcpy.S, whose stores go through the title's own translation,
 * as the BIOS's PIO copy does. The TOC comes through here too.
 *
 * Three sectors: .hiram holds 12 KB and the packet and CD-DA buffers take 5.3
 * of them. overlaps_dcload() lets a disc read land on exactly this range.
 * Every chunk is one host round trip, and under Windows CE the title waits for
 * the whole read: Sega Rally 2's menu music is 604 KB read every 3.5 s, which
 * at two sectors a trip stopped the menu for ~0.45 s each time
 * (docs/wince-investigation.md 7r). Letting CE's thread sleep between chunks
 * only made the read, and the stop, longer (7s): throughput is what counts.
 */
#ifndef GD_STAGE_SECTORS
#define GD_STAGE_SECTORS 3
#endif
HIRAM_BUF static unsigned int gd_stage[GD_STAGE_SECTORS * 512]
	__attribute__((aligned(32)));

/*
 * THE BIG STAGE, FOR A TITLE THAT RUNS WITH THE MMU ON.
 *
 * gd_stage above is all .hiram has room for, and every chunk is one host round
 * trip whose fixed cost (~1.5 ms) dwarfs its wire time (~0.17 ms a sector):
 * three sectors a trip still stopped Sega Rally 2's menu for ~0.25 s every
 * time it read its 604 KB (docs/wince-investigation.md 7u). This one lives in
 * the loader's own stack region above _end (dcload.x.in, .gdstage), which is
 * dead while a title runs -- so only a title under the MMU may use it: in the
 * LOW family that range is inside a Katana title's stack. The network exchange
 * no longer runs there either (gd_on_loader_stack uses the Maple page).
 * Five sectors was what fitted under _stack in the HIGH family; four since the
 * asynchronous reads (2026-09-27), whose code needed 1 KB of that room. A read
 * served that way is one chunk per CE poll (5 ms), so four sectors is ~1.6
 * MB/s -- a real drive's rate -- with the CPU free; the stream path, still
 * synchronous, pays one round trip more per 20 sectors.
 */
#ifndef GD_STAGE_BIG_SECTORS
#define GD_STAGE_BIG_SECTORS 4
#endif
static unsigned int gd_stage_big[GD_STAGE_BIG_SECTORS * 512]
	__attribute__((section(".gdstage"), aligned(32)));

/* The title runs with address translation on (MMUCR.AT): Windows CE. */
static int gd_mmu_on(void)
{
	return *(volatile unsigned int *)0xff000010U & 1U;
}

static int gd_retries_left(int retries)
{
	return retries <= (gd_mmu_on() ? GD_READ_RETRIES_MMU : GD_READ_RETRIES);
}

/* An address the title's MMU translates: P0/U0 with MMUCR.AT set. */
static int gd_is_virtual(unsigned int addr)
{
	return (addr < 0x80000000U) && gd_mmu_on();
}

/*
 * The loader's footprint, from the symbols the linker owns (AGENTS.md 14.11):
 * the image [dcload_base, end) and the packet and CD-DA buffers in .hiram. Not
 * the Maple DMA buffer: it is written only while a MAPL command runs, which
 * never happens under a title, and each cycle rewrites it whole.
 *
 * This used to be the constant range 0x0c004000..0x0c010000, the stock base's
 * image and stack -- and therefore wrong for every relocated session: it
 * protected nothing of a loader at 0x8ce00000, and it refused reads into
 * 0x8c008000..0x8c010000 (IP.BIN, the guest VBR and, at a low base, the
 * title's own stack) that nothing of ours occupies.
 */
extern char dcload_base[];
extern char end[];
extern char _hiram_start[], _hiram_end[];

static int overlaps_dcload(unsigned int addr, unsigned int size)
{
	unsigned int a;
	unsigned int e;

	if (!size)
	{
		return 0;
	}

	/* In P1, where the linker put every symbol compared against below. */
	a = sh4_phys_addr(addr) | 0x80000000U;
	e = a + size;
	/* Overflow means an invalid range; treat as a protection hit. */
	if (e < a)
	{
		return 1;
	}
	/* The one range of ours a disc read is meant to land on. */
	if ((a == (unsigned int)gd_stage) && (size <= sizeof(gd_stage)))
	{
		return 0;
	}

	return (a < (unsigned int)end && e > (unsigned int)dcload_base)
	    || (a < (unsigned int)_hiram_end && e > (unsigned int)_hiram_start);
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
 * ONE NETWORK EXCHANGE, WITHOUT A THREAD SWITCH AND OFF THE TITLE'S STACK.
 *
 * A Katana title's interrupt handlers run on top of a GD syscall and return.
 * Windows CE's timer interrupt enters its scheduler, which runs other threads
 * -- for as long as a busier one wants the CPU -- while the loader is half way
 * through an exchange; the host's answer and the LAN's broadcasts meanwhile
 * fill the ring, and the attempt dies of an RX overflow. Caught under flycast
 * on Sega Rally 2 three times (docs/wince-investigation.md 7o-7q): the GD
 * thread parked with g_gd_in_transfer 1, and every failed read one overflow.
 * Masking interrupts was not enough, because CE calls the driver on a thread
 * stack at a VIRTUAL address, and a TLB miss or an uncommitted stack page
 * there enters CE's kernel with interrupts back on. So under the MMU the
 * exchange runs masked AND on the loader's own stack (P1, no TLB). Reception
 * goes off at the end whatever happened: after a deadline it used to stay on
 * while the title ran, and the ring filled for the next attempt to overflow.
 * A Katana title sees only that bb->stop(), which a successful answer's
 * cmd_retval() already did.
 */
/* From the interrupt hook (SR.BL): its own stack is 1 KB, the loader's the
 * rest of the Maple page -- free then, since the tick runs only while no GD
 * syscall owns the network. */
static int gd_in_irq(void)
{
	unsigned int sr;

	__asm__ volatile ("stc sr,%0" : "=r" (sr));
	return (sr >> 28) & 1U;
}

/*
 * A KOS TITLE'S G2 DMA IS SUSPENDED FOR THE EXCHANGE (2026-10-02), as KOS
 * itself does around every CPU access to G2 (g2_lock()) and as the tick does
 * for Katana titles (g2dma_hold(), g2dma.h). A KOS title moves its sound to
 * the AICA by G2 DMA whenever it likes, and here the CPU reads the BBA's ring
 * through the same bus. Seen on the GTA III port, on the console: a stream
 * read failed five attempts in a row, 250 ms each, the host answering every
 * one, and the title asserted on EIO -- the bus collision is the suspect (no
 * counter could be read: --diag is blind under KOS between reads). The
 * interrupts are masked for the exchange anyway, so the title's sound waits
 * for it either way: ~3 ms a read, 250 ms on a failed attempt.
 */
#if WITH_CDDA
#define gd_g2_hold()    do { if (g_gd_kos) g2dma_hold(); } while (0)
#define gd_g2_release() do { if (g_gd_kos) g2dma_release(); } while (0)
#else
#define gd_g2_hold()    do { } while (0)
#define gd_g2_release() do { } while (0)
#endif

void gd_exchange(void (*fn)(void))
{
	unsigned int irq = bb_irq_hold();

	gd_g2_hold();
	if (irq || gd_in_irq())
	{
		gd_on_loader_stack(fn);
	}
	else
	{
		fn();
	}
	bb->stop();
	gd_g2_release();
	bb_irq_restore(irq);
}

/* ReadSectors' and GetTOC's exchange. The command is in pkt_buf already. */
static void gd_read_exchange(void)
{
	syscall_retval = (unsigned int)-1;
	timeout_loop = GD_SYSCALL_TIMEOUT_SECONDS;
	fine_deadline_start = TMU2_COUNT;
	fine_deadline_ticks = GD_READ_DEADLINE_TICKS;
	build_send_packet(sizeof(command_3int_t));
	bb->loop(0);
	/*
	 * A RETURNVALUE OVER A HOLE IS NOT NECESSARILY OURS: WAIT OUT THE DEADLINE.
	 *
	 * A disc read's ReturnValue carries nothing (address 0), so the one that
	 * ends this wait can be the late answer to an EARLIER attempt, queued in
	 * the ring while nothing was listening. Our own window is then still
	 * filling, and failing it here fails the attempt in a millisecond -- and
	 * the next one on the next stale answer. Measured 2026-09-27 on Sega Rally
	 * 2: the ring delivered nothing for ~1 s, then four stale answers burnt
	 * four retries in 5 ms and the stream failed; Windows CE never recovers
	 * from that (docs/wince-investigation.md 7l). A stale answer for the same
	 * LBA brings the same bytes to the same place, so its parts count; ours
	 * complete the window when they come. cmd_retval() stopped RX: restart it.
	 */
	while (((int)syscall_retval >= 0) && !bin_window_complete()
	       && (tmu2_since(fine_deadline_start) <= fine_deadline_ticks))
	{
		g_cdfs_read_stale++;
		syscall_retval = (unsigned int)-1;
		bb->start();
		bb->loop(0);
	}
}

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

	/*
	 * JUDGE THIS CHUNK ON ITS OWN WINDOW.
	 *
	 * bin_window_complete() below is the only thing that now notices a lost
	 * packet, and a window left installed by the PREVIOUS chunk is already
	 * complete. So if this chunk's LoadBinary were the packet that went
	 * missing, every part would be refused and the stale window would still
	 * answer "complete" -- the read would pass having delivered nothing, and
	 * the title would run the previous chunk's bytes.
	 *
	 * This is the same defect the CD-DA path carried until 2026-09-19
	 * (AGENTS.md 4.13, "Fetch integrity", item 1), where it replayed a stale
	 * sub-fetch for weeks with every counter clean. Closing the window costs
	 * one store.
	 */
	bin_window_close();
	/* And no other read's answer may replace it (g_bin_read_want). */
	g_bin_read_want = dest;

	/*
	 * AND DO NOT ECHO THE LoadBinary BACK.
	 *
	 * The host no longer waits for it, so it is read by nobody -- and sending
	 * it from cmd_loadbin() puts one of our frames on the wire in the middle
	 * of the host's incoming burst. That collision is already on record on the
	 * audio path, where a fetch received its LoadBinary and one part and then
	 * nothing, with no receive error counted (commands.c, bin_echo_suppress).
	 * Cleared before we return, so the upload path keeps its flow control.
	 */
	bin_echo_suppress(1);

	g_gd_in_transfer++;
	gd_exchange(gd_read_exchange);
	g_gd_in_transfer--;
	g_bin_read_want = 0;
	bin_echo_suppress(0);
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

	/*
	 * THE HOST NO LONGER CHECKS, SO WE DO.
	 *
	 * Until 2026-09-20 the host waited for the LoadBinary echo before sending
	 * the parts and probed with DoneBinary after them, which cost two round
	 * trips -- about 1.9 ms of the 5 ms a 16 KB chunk froze the title, on
	 * every chunk forever, to catch a loss this link does not have
	 * (g_rx_missed, g_rx_overflow, g_pbin_rejected and g_cdfs_read_retries
	 * were all 0 across a Crazy Taxi session serving 4000 chunks).
	 *
	 * It now sends the request, the parts and the ReturnValue without pausing
	 * for either. The map that cmd_partbin maintains says whether the chunk is
	 * whole, at no network cost, and a hole simply fails the chunk: the caller
	 * yields to the title and asks again, which is one extra round trip on a
	 * loss instead of two on every chunk.
	 *
	 * A SHORT CHUNK MUST NEVER BE REPORTED COMPLETED. The title would run the
	 * bytes, and a title that executes short data is far worse off than one
	 * that waits (the host's own comment on this path, for the same reason).
	 */
	if (!bin_window_complete())
	{
		g_cdfs_read_holes++;
		return CMD_STAT_FAILED;
	}

	g_cdfs_sync_chunks++;
	return CMD_STAT_COMPLETED;
}

#if GD_TRACE && GD_TRACE_VADDR
/*
 * THE UTLB ENTRY MAPPING A VIRTUAL ADDRESS (2026-09-26 for the trace below,
 * the asynchronous reads used it until 2026-09-28). Measured on Windows CE's buffers:
 * 4 KB copy-back pages, found where expected (docs/wince-investigation.md 7f).
 * Call it through its P2 alias -- the TLB arrays are read from uncached code
 * -- with interrupts masked, right after touching the page so the entry is
 * there. gd_utlb_d is 0 if no valid entry for the current ASID maps va.
 */
static unsigned int gd_utlb_a, gd_utlb_d;

/* log2 of the page size, by the data word's SZ1:SZ0 (1 KB, 4 KB, 64 KB, 1 MB) */
static const unsigned char gd_page_shift[4] = { 10, 12, 16, 20 };

static inline unsigned int gd_page_size(unsigned int d)
{
	return 1U << gd_page_shift[((d >> 6) & 2U) | ((d >> 4) & 1U)];
}

/* Runs from its P2 alias: the TLB arrays are read from uncached code. */
static void gd_utlb_probe(unsigned int va)
{
	unsigned int asid = *(volatile unsigned int *)0xff000000U & 0xffU;
	unsigned int i;

	gd_utlb_a = 0;
	gd_utlb_d = 0;
	for (i = 0; i < 64; i++)
	{
		unsigned int a = *(volatile unsigned int *)(0xf6000000U | (i << 8));
		unsigned int d = *(volatile unsigned int *)(0xf7000000U | (i << 8));
		unsigned int mask = ~(gd_page_size(d) - 1U);

		if (!(a & 0x100U) || ((a ^ va) & mask)
		    || (!(d & 2U) && ((a & 0xffU) != asid)))
		{
			continue;
		}
		gd_utlb_a = a;
		gd_utlb_d = d;
		return;
	}
}

#endif

#if WITH_IRQ_HOOK
/*
 * ASYNCHRONOUS READS UNDER WINDOWS CE (2026-09-27, docs/wince-investigation.md
 * 9f-9g).
 *
 * WHY. A real drive moves a read with the CPU free -- DMA, or interrupts --
 * and Windows CE's GD thread sleeps meanwhile, so the menu keeps drawing
 * through a 604 KB read that takes the drive ~0.4 s. Served synchronously the
 * same read was 61 masked exchanges inside one ExecServer: the CPU was ours
 * for ~0.1 s and the menu stopped.
 *
 * WHO MOVES IT. The interrupt hook's tick, at the network's pace: it drains
 * the ring in bounded passes, gives each chunk its verdict (whole window =
 * done, 250 ms = failed, a ReturnValue over a hole = stale, keep listening),
 * copies a done chunk into the title's buffer and posts the next. The GD
 * thread only starts the read and sees it end. It has to be that way round:
 * CE's GD thread wakes about ten times a second while the menu draws -- a
 * first version that had the THREAD copy and post each chunk moved 8 KB per
 * wake, 72 KB/s, and the menu's stream starved (9f).
 *
 * THE BUFFER IS VIRTUAL, AND THE TICK CANNOT TAKE A TLB MISS (SR.BL is set:
 * a miss there resets the CPU). So on each wake the thread translates the
 * next GA_XLAT_PAGES pages of the buffer into physical ones -- by CE's own
 * page tables (ga_walk), checked with a byte written through the virtual
 * address and read back uncached at the physical one -- and purges the
 * buffer's lines through the virtual address so no dirty line of CE's is
 * later written back over what the tick put in RAM; the tick writes through
 * P1 and writes its own lines back. A chunk whose pages are not translated
 * yet waits for the thread, which copies it the old way. A physical
 * destination (DMAREAD) needs none of this.
 *
 * WHO OWNS WHAT. The thread works with the GD lock held; the tick only while
 * it is free (irq.c): never both, so _GDS and the table need no other lock.
 * g_gd_in_transfer is up from a post to its verdict, so CD-DA keeps off the
 * network and the window; it plays between chunks. BBA only: the LAN
 * adapter's loop honours neither drain_iters nor the fine deadline.
 */
#define GA_IDLE   0U
#define GA_POSTED 1U
#define GA_DONE   2U
#define GA_FAILED 3U
/*
 * Loop turns per look: take what the ring holds and leave. A turn drains
 * every frame already queued; more turns only wait for the wire with SR.BL
 * set. At 64 a look that met a chunk's first frame rode the whole burst
 * (~1.3 ms, g_irq_tick_max 1.8 ms), the title's own interrupts held off all
 * that time; the BBA's RX interrupt (irq.c) brings the tick back for the rest
 * (9r). The second turn is for a frame whose header was not written yet.
 */
#ifndef GA_POLL_ITERS
#define GA_POLL_ITERS 2
#endif
#ifndef GA_XLAT_PAGES
#define GA_XLAT_PAGES 32		/* 128 KB of buffer ahead */
#endif
/*
 * CE on the SH4 maps its memory in 4 KB pages, 16 to a 64 KB MemBlock
 * (measured, 9i: the UTLB entries of a menu read were all 4 KB). 128 KB a
 * wake, ten wakes a second, is ~1.3 MB/s: a real drive's rate.
 */
#define GA_XPAGE 0x1000U
/*
 * A chunk into a physical buffer (DMAREAD, Katana) is not bounded by the
 * stage, only by the RX ring, which must be able to hold a whole answer while
 * nobody drains it: 6 sectors are ~13.3 KB of the 16 KB with the headers
 * (9 data packets, the LoadBinary, the ReturnValue). A third fewer round
 * trips than 4 (9o).
 */
#ifndef GA_PHYS_SECTORS
#define GA_PHYS_SECTORS 6
#endif
static volatile unsigned int ga_state;
static unsigned int ga_start;		/* TMU2 at the post */
static unsigned int ga_sc;		/* sectors in the chunk on the wire */
static unsigned int ga_fail_run;	/* chunks failed since one last got through */
static unsigned int ga_virt;		/* the title's buffer is translated */
static unsigned int ga_req_end;		/* one past the buffer (virtual) */
static unsigned int ga_xbase;		/* virtual page ga_xpa[0] maps */
static unsigned int ga_xcount;		/* valid entries */
HIRAM_BUF static unsigned int ga_xpa[GA_XLAT_PAGES];	/* P1 page addresses */
volatile unsigned int g_ga_posts;	/* chunks requested this way */
volatile unsigned int g_ga_irq_done;	/* ... finished by the tick, copy included */
volatile unsigned int g_ga_xlat_miss;	/* pages the walk could not translate */
volatile unsigned int g_ga_sync;	/* reads finished synchronously: nothing translated */
volatile unsigned int g_ga_wakes;	/* the thread resumed with a chunk on the wire */

/*
 * The hook's tick moves the read: under Windows CE, and under a Katana title
 * with GD_ASYNC_KATANA (its buffer is physical: nothing to translate).
 */
#ifndef GA_KATANA_READS
#define GA_KATANA_READS 1	/* 0: the hook in Katana titles, their reads synchronous */
#endif
static int gd_async_on(void)
{
	return bb == &adapter_bba && g_irq_hooked
	    && ((GD_ASYNC_KATANA && GA_KATANA_READS) || gd_mmu_on());
}

static void ga_send(void)
{
	build_send_packet(sizeof(command_3int_t));
}

static void ga_drain(void)
{
	drain_iters = GA_POLL_ITERS;
	bb->loop(0);
	drain_iters = 0;
}

/* Masked and on the loader's stack, FPU usable: gd_exchange() without the
 * bb->stop() at the end, because the answer is still to come. */
static void ga_run(void (*fn)(void))
{
	unsigned int irq = bb_irq_hold();

	if (irq || gd_in_irq())
	{
		gd_on_loader_stack(fn);
	}
	else
	{
		fn();
	}
	bb_irq_restore(irq);
}

/* Post the next chunk of the read _GDS describes. */
static void ga_next(void)
{
	command_3int_t *command =
		(command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
	unsigned int most = ga_virt ? GD_STAGE_BIG_SECTORS : GA_PHYS_SECTORS;

	ga_sc = (_GDS.param[1] < most) ? _GDS.param[1] : most;
	memcpy(command->id, CMD_CDFSREAD, 4);
	command->value0 = htonl(_GDS.param[0]);
	g_bin_read_want = ga_virt ? (unsigned int)gd_stage_big : _GDS.param[2];
	command->value1 = htonl(g_bin_read_want);
	command->value2 = htonl(ga_sc * _GDS.sec_size);
	bin_window_close();	/* judged on its own window: see ReadSectors() */
	bin_echo_suppress(1);
	syscall_retval = (unsigned int)-1;
	g_gd_in_transfer++;
	ga_state = GA_POSTED;
	ga_start = TMU2_COUNT;
	g_ga_posts++;
	irq_rx_arm(1);
	ga_run(ga_send);
}

static void ga_end(unsigned int verdict)
{
	irq_rx_arm(0);
	bb->stop();
	g_bin_read_want = 0;
	bin_echo_suppress(0);
	g_gd_in_transfer--;
	ga_state = verdict;
	if (verdict == GA_DONE)
	{
		g_cdfs_sync_chunks++;
	}
	else
	{
		g_cdfs_read_fails++;
	}
}

static void ga_poll(void)
{
	unsigned int late;

	if (ga_state != GA_POSTED)
	{
		return;
	}

	ga_run(ga_drain);
	late = tmu2_since(ga_start) > GD_READ_DEADLINE_TICKS;
	if (bin_window_complete())
	{
		/* Done with its ReturnValue, not before: left in the ring, it met the
		 * next CD-DA fetch as a wrong LBA (9h). Whole and past the deadline
		 * without it is done too -- the bytes are all there. */
		if ((int)syscall_retval >= 0 || late)
		{
			ga_end(GA_DONE);
		}
	}
	else if (late)
	{
		g_fine_timeouts++;
		ga_end(GA_FAILED);
	}
	else if ((int)syscall_retval >= 0)
	{
		/* A ReturnValue over a hole: an earlier attempt's, most likely.
		 * cmd_retval() stopped reception; keep listening. */
		g_cdfs_read_stale++;
		syscall_retval = (unsigned int)-1;
		bb->start();
	}
}

/* The done chunk goes to param[2]: through the translated pages, from any
 * context. 0 if a page it needs is not translated (yet). */
static int ga_copy_out(void)
{
	unsigned int va = _GDS.param[2];
	unsigned int n = ga_sc * _GDS.sec_size;
	unsigned int off = 0;

	if (va < ga_xbase || va + n > ga_xbase + ga_xcount * GA_XPAGE)
	{
		return 0;
	}
	while (off < n)
	{
		unsigned int in = (va + off) & (GA_XPAGE - 1U);
		unsigned int k = GA_XPAGE - in;
		unsigned char *p = (unsigned char *)
			(ga_xpa[(va + off - ga_xbase) / GA_XPAGE] | in);

		if (k > n - off)
		{
			k = n - off;
		}
		memcpy(p, (unsigned char *)gd_stage_big + off, k);
		CacheBlockWriteBack((unsigned char *)((unsigned int)p & ~31U),
				    ((((unsigned int)p & 31U) + k + 31U) >> 5));
		off += k;
	}
	return 1;
}

/* A done chunk counted in: _GDS moves on, and the read with it. */
static void ga_commit(void)
{
	unsigned int bytes = ga_sc * _GDS.sec_size;

	_GDS.param[1] -= ga_sc;
	_GDS.param[0] += ga_sc;
	_GDS.param[2] += bytes;
	_GDS.transfered += bytes;
	_GDS.lba = _GDS.param[0];
	ga_fail_run = 0;	/* the tick commits most chunks: the thread never sees them */
	ga_state = GA_IDLE;
}

/*
 * A page's P1 address by Windows CE's page tables, walked as its TLB refill
 * does -- and as flycast's USE_WINCE_HACK does (core/hw/sh4/modules/wince.h),
 * which is why this works there too: flycast's FAST_MMU serves translations
 * from a cache of its own, so a UTLB probe misses pages a real SH4 would hold
 * (9i). TTB holds 64 section pointers (32 MB each), a section 512 MemBlock
 * pointers (64 KB each; 0 and 1 are the empty and reserved blocks), and a
 * MemBlock, from +12, one entry per 4 KB page: its PTEL plus one, 0 while
 * uncommitted. 0 if va has no page in main RAM.
 */
#define GA_P1(x) (((x) & 0x1fffffffU) | 0x80000000U)
static unsigned int ga_walk(unsigned int va)
{
	unsigned int p = GA_P1(*(volatile unsigned int *)0xff000008U);

	p = GA_P1(*(unsigned int *)(p + ((va >> 25) << 2)));
	p = ((unsigned int *)p)[(va >> 16) & 0x1ffU];
	if (!(p & 0x80000000U))
	{
		return 0;
	}
	p = ((unsigned int *)GA_P1(p + 12U))[(va >> 12) & 0xfU] - 1U;
	if ((p & 0x1f000000U) != 0x0c000000U)
	{
		return 0;
	}
	return GA_P1(p & ~(GA_XPAGE - 1U));
}

/*
 * The thread's part: translate the buffer's next pages, from param[2] on.
 * Each is touched (CE commits it if it has not), walked, and checked: a byte
 * of the buffer -- which the read overwrites anyway -- is inverted through
 * the virtual address, the lines purged through it, and the byte read back
 * uncached at the physical address the walk gave. A walk that is wrong on
 * some CE stops the translation there, and the read goes the old way.
 */
static void ga_translate(void)
{
	unsigned int first = _GDS.param[2];
	unsigned int va, lo, hi, pa, i;
	unsigned char b;

	ga_xcount = 0;
	ga_xbase = first & ~(GA_XPAGE - 1U);
	for (i = 0; i < GA_XLAT_PAGES; i++)
	{
		va = ga_xbase + i * GA_XPAGE;
		if (va >= ga_req_end)
		{
			break;
		}
		lo = (va < first) ? first : va;
		hi = (va + GA_XPAGE < ga_req_end) ? va + GA_XPAGE : ga_req_end;

		b = (unsigned char)~*(volatile unsigned char *)lo;
		*(volatile unsigned char *)lo = b;
		pa = ga_walk(va);
		CacheBlockPurge((unsigned char *)(lo & ~31U), (hi - (lo & ~31U) + 31U) >> 5);
		if (!pa || *(volatile unsigned char *)((pa | 0x20000000U) + (lo & (GA_XPAGE - 1U))) != b)
		{
			g_ga_xlat_miss++;
			break;
		}
		ga_xpa[i] = pa;
		ga_xcount = i + 1;
	}
}

#if IRQ_IDLE_LISTEN
static void ga_listen(void)
{
	extern volatile unsigned int g_rx_dma_tick;

	/* By CPU, as a tick's CD-DA fetch (cdda.c): a late PBIN taken by DMA here
	 * would be settled with irq_rx_arm(1), the chip's interrupt armed with no
	 * chunk on the wire. irq_tick() clears it again on the way out. */
	g_rx_dma_tick = 0;
	/* Every exchange ends with reception off (cmd_retval, ga_end): without
	 * this the chip dropped every request and the listen heard nothing. Left
	 * on, so that a request landing between two listens waits in the ring
	 * (50 ms) instead of being dropped; the next exchange starts it anyway. */
	bb->start();
	drain_iters = 256;
	fine_deadline_start = TMU2_COUNT;
	fine_deadline_ticks = 12500U;	/* 1 ms */
	bb->loop(0);
	fine_deadline_ticks = 0;
	drain_iters = 0;
	timeout_loop = 0;
}

/* From the tick, GD lock free and no CD-DA fetch: answer whatever the host
 * asked (--diag, the stack watch, memory marks) even though the title is not
 * reading its disc -- a GD wait is otherwise the only time the loader looks
 * at the wire (measured on Shenmue II, 2026-09-30: a cinematic answered
 * nothing for its whole length). */
int gd_idle_listen(void)
{
	if (ga_state != GA_IDLE || g_gd_in_transfer || bb != &adapter_bba)
	{
		return 0;
	}
	ga_run(ga_listen);
	return 1;
}
#endif

/* A chunk is on the wire (irq.c paces its looks at it). */
int gd_async_busy(void)
{
	return ga_state == GA_POSTED;
}

/* From the tick, GD lock free (irq.c). Non-zero if a chunk was on the wire. */
int gd_async_tick(void)
{
	if (ga_state != GA_POSTED)
	{
		return 0;
	}
	ga_poll();
	if (ga_state == GA_DONE && (!ga_virt || ga_copy_out()))
	{
		g_ga_irq_done++;
		ga_commit();
		/* Nothing on the wire between two chunks: feed CD-DA, as the
		 * synchronous loop does. A load is seconds of chunks back to
		 * back, and irq_tick() services CD-DA only when no read is in
		 * flight -- without this the ring (1.39 s) played itself over
		 * and over for the whole load, and nothing muted it (9k). */
		cdda_service_tick();
		if (_GDS.param[1] && !_GDS.cmd_abort)
		{
			ga_next();
		}
	}
	return 1;
}

/*
 * The thread's side of a read under CE with the hook in. It sleeps (CE's
 * Sleep(5) on WAIT_INTERNAL, 7e) while the tick moves the chunks; on each wake
 * it translates ahead, finishes a chunk the tick could not copy, posts one if
 * none is on the wire, and retries a failed one. Only values live across the
 * yields (cdfs_redir.s: the parked frame is relocatable).
 *
 * Under a Katana title the same tick runs (irq.c: their entry is three nops),
 * so a chunk moves between frames as soon as it is back, and a wake -- an
 * ExecServer, once a frame -- only sees the read end. One chunk per wake,
 * without it, ran menu loads at ~350 KB/s (9m). g_ga_wakes counts the resumes.
 */
static void data_transfer_emu_async(void);

static void data_transfer_async(void)
{
	ga_fail_run = 0;
	ga_virt = gd_is_virtual(_GDS.param[2]);
	ga_req_end = _GDS.param[2] + _GDS.param[1] * _GDS.sec_size;
	ga_xcount = 0;
	ga_state = GA_IDLE;
	if (!ga_virt && overlaps_dcload(_GDS.param[2], ga_req_end - _GDS.param[2]))
	{
		_GDS.status = CMD_STAT_FAILED;
	}
	_GDS.ata_status = CMD_WAIT_INTERNAL;

	while (_GDS.param[1] && !_GDS.cmd_abort && _GDS.status != CMD_STAT_FAILED)
	{
		if (ga_virt)
		{
			ga_translate();
		}
		if (ga_state == GA_IDLE && ga_virt && !ga_xcount)
		{
			/*
			 * NOTHING TRANSLATED: FINISH THE WAY WE USED TO (9i).
			 *
			 * Without a page the tick can write, every chunk waits for a
			 * wake -- 20 a second at best -- which is several times slower
			 * than the synchronous loop and starved the menu's stream for
			 * seconds. flycast gets here: built with FAST_MMU, it serves a
			 * translation from its own cache without reloading the UTLB, so
			 * the probe finds nothing; a real SH4 must reload it to access
			 * the page at all.
			 */
			g_ga_sync++;
			data_transfer_emu_async();
			return;
		}
		if (ga_state == GA_IDLE)
		{
			ga_next();
		}
		ga_poll();
		if (ga_state == GA_DONE)
		{
			if (ga_virt && !ga_copy_out())
			{
				memcpy((void *)_GDS.param[2], gd_stage_big,
				       ga_sc * _GDS.sec_size);
			}
			ga_commit();
#if GD_CDDA_BETWEEN_CHUNKS
			/* Nothing on the wire: the music's turn, as in the tick. */
			cdda_service_between_chunks();
#endif
			continue;
		}
		if (ga_state == GA_FAILED)
		{
			ga_state = GA_IDLE;	/* asked again at once: the deadline has passed */
			g_cdfs_read_retries++;
			if (!gd_retries_left(++ga_fail_run))
			{
				_GDS.status = CMD_STAT_FAILED;
			}
			continue;
		}
		gdcExitToGame();
		g_ga_wakes++;
	}

	if (ga_state == GA_POSTED)
	{
		ga_end(GA_FAILED);	/* aborted in flight */
	}
	ga_state = GA_IDLE;
	ga_xcount = 0;
	if (_GDS.cmd_abort)
	{
		_GDS.transfered = 0;
		_GDS.status = CMD_STAT_IDLE;
	}
	else if (_GDS.status != CMD_STAT_FAILED)
	{
		_GDS.status = CMD_STAT_COMPLETED;
		_GDS.requested -= _GDS.transfered;
	}
	_GDS.drv_stat = CD_STATUS_PAUSED;
}
#endif

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
	/* A translated buffer goes through gd_stage_big, a stage at a time. */
	unsigned int virt = gd_is_virtual(_GDS.param[2]);
	unsigned int most = virt ? GD_STAGE_BIG_SECTORS : (unsigned int)GD_EMU_ASYNC;

	while ((_GDS.param[1] > 0) && (_GDS.cmd_abort == 0))
	{
		sc = (_GDS.param[1] <= most) ? _GDS.param[1] : most;
		sc_size = _GDS.sec_size * sc;

		if (ReadSectors(virt ? (unsigned int)gd_stage_big : _GDS.param[2],
				_GDS.param[0], sc) == CMD_STAT_FAILED)
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
			/*
			 * NOT gd_trace_always. AN INSTRUMENT INSIDE THE BLAST RADIUS.
			 *
			 * This used to report every failed chunk unconditionally, and it
			 * is reached exactly when the link has just failed to deliver one.
			 * The cost is not one packet: write() is a syscall that transmits
			 * and waits, and the host fetches the written bytes BACK off the
			 * console with SendBinQ (fs.rs, download_data) -- so a read that
			 * could not be answered spawned an instrument read that could not
			 * be answered either. On 2026-09-20 that pair wedged the host in an
			 * unbounded re-request loop and froze the title for good.
			 *
			 * The counters already say all of it, at no network cost:
			 * g_cdfs_read_retries here, g_cdfs_read_fails (the host never
			 * answered) and g_cdfs_read_holes (it answered short). Build with
			 * GD_TRACE=1 when the sequence itself is wanted.
			 */
			gd_trace('F', _GDS.param[0], _GDS.param[2],
				 (unsigned int)retries);
			if (!gd_retries_left(++retries))
			{
				_GDS.status = CMD_STAT_FAILED;
				_GDS.drv_stat = CD_STATUS_PAUSED;
				return;
			}
			/*
			 * NOT "WAITING FOR AN INTERRUPT" WHILE WE WAIT FOR A RETRY
			 * (2026-09-27). Windows CE's GD thread reads PROCESSING with
			 * status[3] == CMD_WAIT_IRQ as "the drive will interrupt",
			 * returns STATUS_PENDING and sleeps on the GD interrupt -- which
			 * this transport never raises -- so it never called ExecServer
			 * again and the retry never ran: one lost request, black screen
			 * (docs/wince-investigation.md 7i). With status[3] clear it
			 * sleeps 5 ms and polls, which is the retry. Katana titles keep
			 * what they have always seen.
			 */
			if (gd_mmu_on())
			{
				_GDS.ata_status = CMD_WAIT_INTERNAL;
			}
			gdcExitToGame();
			continue;
		}
		retries = 0;
		if (virt)
		{
			memcpy((void *)_GDS.param[2], gd_stage_big, sc_size);
		}

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

#if WITH_IRQ_HOOK
	/* Posted at once, not after the yield below: the chunk is on the wire
	 * while the title runs, and a read costs one frame less. The title still
	 * sees PROCESSING first -- the answer takes ~2 ms, the loop yields long
	 * before. */
	if (_GDS.param[1] && gd_async_on()
	    && (_GDS.param[1] > 1 || gd_is_virtual(_GDS.param[2])))
	{
		data_transfer_async();
		_GDS.ata_status = CMD_WAIT_INTERNAL;
		return;
	}
#endif

	/*
	 * isoldr yields once before starting for non-KOS binaries, so the title
	 * observes PROCESSING for at least one poll instead of seeing a request
	 * satisfied within its own ReqCmd frame. Retail code is written against a
	 * drive that cannot possibly be that fast. Not for KOS (g_gd_kos).
	 */
	if (!g_gd_kos)
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
	 *
	 * NOT FOR KOS (g_gd_kos): this path never retries, and KOS does not
	 * either -- a FAILED read is EIO, a short fread, and the GTA III port's
	 * librw asserts and exits (2026-10-02, one lost request under flycast, in
	 * MISC.TXD). Its one-sector reads take data_transfer_emu_async() and its
	 * retries; KOS's vblank handler resumes the server after the yield.
	 */
#if GD_BULK_SECTORS > 0
	if (((GD_EMU_ASYNC == 0) || (_GDS.param[1] == 1) ||
	     (_GDS.param[1] >= (unsigned int)GD_BULK_SECTORS))
	    && !gd_is_virtual(_GDS.param[2]) && !g_gd_kos)
#else
	if (((GD_EMU_ASYNC == 0) || (_GDS.param[1] == 1))
	    && !gd_is_virtual(_GDS.param[2]) && !g_gd_kos)
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

/*
 * STREAMS (2026-09-27): *READ_STREAM and *READ_STREAM_EX.
 *
 * The command names a run of sectors and nothing else; the title then hands
 * over one piece at a time -- ReqPioTrans / ReqDmaTrans, {address, bytes} --
 * and polls CheckPioTrans / CheckDmaTrans for it. A piece need not be a whole
 * number of sectors, so the stream is a byte stream from param[0]'s first
 * byte, read a stage at a time into gd_stage and copied out: to the address
 * as given for PIO (a virtual one, for Windows CE), to the physical page
 * through P2 for DMA, since the title will read what a DMA wrote without
 * trusting its cache.
 *
 * WHO ASKS FOR THE NEXT PIECE. For PIO, the callback the title registered
 * with SetPioCallback, which the BIOS calls when a piece is done -- and which
 * is called here, from the server, while more is owed (isoldr does the same,
 * loader/syscalls.c data_transfer_pio_stream). Windows CE's callback just
 * requests the next piece, so a whole stream is served inside the ExecServer
 * that finds the first one. For DMA, the title's G1 DMA-end interrupt, which
 * nothing here raises: a DMA stream finishes only if its first piece is all
 * of it. Windows CE chains DMA pieces from that interrupt (wsegacd.dll), and
 * the host steers it to PIO instead (dcload-ip-rs, dispatch.rs).
 *
 * Until 2026-09-27 these commands were force-completed with nothing
 * delivered, which is how CE came to stop after its first executable header.
 */
static unsigned int gd_piece_addr;	/* the piece asked for and not yet served */
static unsigned int gd_piece_size;	/* its bytes; 0 = none pending */

static void data_stream(void)
{
	unsigned int dma = (_GDS.cmd == CMD_DMAREAD_STREAM)
			|| (_GDS.cmd == CMD_DMAREAD_STREAM_EX);
	unsigned int pos = 0;		/* stream bytes handed over */
	unsigned int base = 0;		/* stream byte stage[0] holds */
	unsigned int have = 0;		/* stream bytes stage holds */
	unsigned int sectors = _GDS.requested >> 11;
	unsigned int dst, left, n, sec;
	int retries = 0;
	/* The big stage only under the MMU: see gd_stage_big. */
	unsigned int *stage = gd_mmu_on() ? gd_stage_big : gd_stage;
	unsigned int cap = gd_mmu_on() ? GD_STAGE_BIG_SECTORS : GD_STAGE_SECTORS;

	gd_piece_size = 0;
	_GDS.status = CMD_STAT_STREAMING;

	while (_GDS.requested && !_GDS.cmd_abort)
	{
		if (!gd_piece_size)
		{
			gdcExitToGame();
			continue;
		}
		gd_trace('Q', gd_piece_addr, gd_piece_size, _GDS.requested);
		dst = dma ? (sh4_phys_addr(gd_piece_addr) | 0xa0000000U)
			  : gd_piece_addr;
		left = gd_piece_size;
		while (left && !_GDS.cmd_abort)
		{
			if (pos >= base + have)
			{
				sec = pos >> 11;
				n = sectors - sec;
				if (n > cap)
				{
					n = cap;
				}
				if (ReadSectors((unsigned int)stage,
						_GDS.param[0] + sec, n) == CMD_STAT_FAILED)
				{
					/* Asked again AT ONCE, not from the next ExecServer
					 * as data_transfer_emu_async() does: a title polls a
					 * STREAMING channel only when the drive interrupts
					 * (Windows CE does), so a yield here would be the
					 * last thing it ever saw. The failed wait (250 ms)
					 * has already let the host go idle. */
					g_cdfs_read_retries++;
					if (!gd_retries_left(++retries))
					{
						_GDS.status = CMD_STAT_FAILED;
						break;
					}
					continue;
				}
				retries = 0;
				base = sec << 11;
				have = n << 11;
#if GD_CDDA_BETWEEN_CHUNKS
				cdda_service_between_chunks();
#endif
			}
			n = base + have - pos;
			if (n > left)
			{
				n = left;
			}
			memcpy((void *)dst, (char *)stage + (pos - base), n);
			dst += n;
			pos += n;
			left -= n;
		}
		if (_GDS.status == CMD_STAT_FAILED)
		{
			break;
		}
		_GDS.transfered += gd_piece_size - left;
		_GDS.requested -= gd_piece_size - left;
		_GDS.lba = _GDS.param[0] + ((pos + 2047) >> 11);
		gd_piece_size = 0;
		if (_GDS.requested && !dma && _GDS.callback)
		{
			((void (*)(unsigned int))_GDS.callback)(_GDS.callback_param);
		}
	}

	gd_piece_size = 0;
	if (_GDS.cmd_abort)
	{
		_GDS.status = CMD_STAT_IDLE;
	}
	else if (_GDS.status != CMD_STAT_FAILED)
	{
		_GDS.status = CMD_STAT_COMPLETED;
	}
	_GDS.drv_stat = CD_STATUS_PAUSED;
	_GDS.ata_status = CMD_WAIT_INTERNAL;
}

/*
 * THE TABLE OF CONTENTS COMES INTO gd_stage, AND THE TITLE GETS A CPU COPY OF
 * IT -- WHICH IS WHAT THE BIOS DOES (2026-09-26).
 *
 * GETTOC2 is not a DMA: the BIOS reads the table into its own work area and
 * copies it into the caller's buffer with ordinary stores, in the caller's
 * context. Windows CE hands the driver a buffer on a thread stack by VIRTUAL
 * address (0x080df2e0). Written there by the host, the table never arrived
 * (see gd_stage for why), CE's mount took the 0xff its driver had pre-filled,
 * and 0xffffff read as MSF 144:16:15 is exactly the FAD 0x9e80f it then asked
 * for (docs/wince-investigation.md 7e, 7f). Copied with a plain loop.
 */

#if GD_TRACE && GD_TRACE_VADDR
/*
 * WHERE A TITLE'S VIRTUAL ADDRESS LANDS (GD_TRACE_VADDR, 2026-09-26).
 *
 *   'M' MMUCR, SR, PTEH   -- translation on (AT, bit 0)? FD/BL/IMASK? ASID?
 *   'U' va, UTLB address word, UTLB data word -- the entry mapping va, or 0 0
 *   'P' words 0, 2 and 99 of the page behind it, read through P2 (memory)
 *   'V' the same three words through va (what the title sees)
 *
 * 'P' and 'V' are read only when the UTLB already maps the page, and with
 * interrupts masked from the probe to the last read, so the instrument itself
 * can take no TLB miss inside the loader. Everything is read first and traced
 * afterwards: each trace is a network round trip, long enough for the title's
 * interrupts to replace the entry.
 */
static void gd_trace_vaddr(unsigned int va, int with_mmu)
{
	void (*probe)(unsigned int) = (void (*)(unsigned int))
		((unsigned int)gd_utlb_probe | 0xa0000000U);
	unsigned int sr, masked, pa;
	unsigned int w[6] = { 0, 0, 0, 0, 0, 0 };

	__asm__ volatile ("stc sr,%0" : "=r" (sr));
	masked = sr | 0xf0U;
	__asm__ volatile ("ldc %0,sr" : : "r" (masked));
	probe(va);
	if (gd_utlb_d)
	{
		unsigned int d = gd_utlb_d;
		unsigned int size = gd_page_size(d);
		volatile unsigned int *p, *v = (volatile unsigned int *)va;

		pa = (d & 0x1ffffc00U & ~(size - 1U)) | (va & (size - 1U));
		p = (volatile unsigned int *)(pa | 0xa0000000U);
		w[0] = p[0]; w[1] = p[2]; w[2] = p[99];
		w[3] = v[0]; w[4] = v[2]; w[5] = v[99];
	}
	__asm__ volatile ("ldc %0,sr" : : "r" (sr));

	if (with_mmu)
	{
		gd_trace_always('M', *(volatile unsigned int *)0xff000010U, sr,
				*(volatile unsigned int *)0xff000000U);
	}
	gd_trace_always('U', va, gd_utlb_a, gd_utlb_d);
	if (gd_utlb_d)
	{
		gd_trace_always('P', w[0], w[1], w[2]);
		gd_trace_always('V', w[3], w[4], w[5]);
	}
}
#endif

/* Read TOC: into gd_stage, then copied to the title (see above). */
static void GetTOC(void)
{
	command_3int_t *command =
		(command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

	memcpy(command->id, CMD_CDFSTOC, 4);
	command->value0 = htonl(_GDS.param[0]);	/* session / area */
	command->value1 = htonl((unsigned int)gd_stage);
	command->value2 = 0;

	/* The same exchange as a sector read (gd_exchange), window and all. */
	g_gd_in_transfer++;
	gd_exchange(gd_read_exchange);
	g_gd_in_transfer--;
	fine_deadline_ticks = 0;
	timeout_loop = 0;

	if ((int)syscall_retval < 0)
	{
		_GDS.err = CMD_ERR_HARDWARE;
		_GDS.status = CMD_STAT_FAILED;
		return;
	}

#if GD_TRACE && GD_TRACE_VADDR
	gd_trace_vaddr(_GDS.param[1], 1);
#endif
	/* 99 entries + first + last + leadout. Not memcpy_32bit: see above. */
	{
		volatile unsigned int *toc = (volatile unsigned int *)_GDS.param[1];
		unsigned int i;

		for (i = 0; i < 102; i++)
			toc[i] = gd_stage[i];
	}
#if GD_TRACE && GD_TRACE_VADDR
	gd_trace_vaddr(_GDS.param[1], 0);
#endif
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
			case CMD_DMAREAD_STREAM:
			case CMD_PIOREAD_STREAM:
			case CMD_DMAREAD_STREAM_EX:
			case CMD_PIOREAD_STREAM_EX:
				data_stream();
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
	if (GD_ASYNC_KATANA || gd_mmu_on())
	{
		irq_hook_check();
	}

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

		/* A DMA destination is PHYSICAL: the BIOS hands it to the G1 DMA,
		 * which knows nothing of the MMU. Windows CE computes it that way
		 * (the page frames of a locked buffer, 0x0ce99000), and with its MMU
		 * on, the same number used as a CPU address is a virtual one in
		 * process slot 6. P1 reaches the physical page whatever the title
		 * does with the MMU; with the MMU off it is P0's twin. */
		if (cmd == CMD_DMAREAD)
		{
			_GDS.param[2] = sh4_phys_addr(_GDS.param[2]) | 0x80000000U;
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
	if (GD_ASYNC_KATANA || gd_mmu_on())
	{
		irq_hook_check();
	}

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

	case CMD_STAT_STREAMING:
		status[2] = _GDS.transfered;
		status[3] = _GDS.ata_status;
		rv = CMD_STAT_STREAMING;
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
 * The transfer half of a stream (see data_stream()). A piece is accepted only
 * while the channel's stream is running, none is pending, and it fits in what
 * is still owed; the server serves it at its next ExecServer. Check*Trans
 * answers 1 while a piece is pending, else 0 with the bytes still owed -- the
 * BIOS contract Windows CE's driver sizes its next piece from. Signatures
 * must stay BIOS-compatible; titles do validate the channel.
 */
static int gd_piece_request(int gd_chn, unsigned int *buf)
{
	if (!buf || (gd_chn != _GDS.req_count)
	    || (_GDS.status != CMD_STAT_STREAMING) || gd_piece_size
	    || (buf[1] > _GDS.requested))
	{
		return -1;
	}
	gd_piece_addr = buf[0];
	gd_piece_size = buf[1];
	return 0;
}

static int gd_piece_check(int gd_chn, unsigned int *size)
{
	if ((gd_chn != _GDS.req_count) || (_GDS.status != CMD_STAT_STREAMING))
	{
		return -1;
	}
	if (size)
	{
		*size = gd_piece_size ? _GDS.transfered : _GDS.requested;
	}
	return gd_piece_size ? 1 : 0;
}

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
	return gd_piece_request(gd_chn, dmabuf);
}

int gdGdcCheckDmaTrans(int gd_chn, unsigned int *size)
{
	g_gd_idx_counts[7]++;
	return gd_piece_check(gd_chn, size);
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
	return gd_piece_request(gd_chn, (unsigned int *)piobuf);
}

int gdGdcCheckPioTrans(int gd_chn, int *size)
{
	g_gd_idx_counts[13]++;
	return gd_piece_check(gd_chn, (unsigned int *)size);
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

