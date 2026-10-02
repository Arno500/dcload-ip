#include <string.h>
#include <unistd.h>
#include "commands.h"
//#include "packet.h" // in header now
#include "net.h"
#include "video.h"
#include "adapter.h"
#include "syscalls.h"
#include "cdfs.h"
#include "dcload.h"
#include "go.h"
#include "disable.h"
#include "scif.h"
#include "maple.h"

#include "perfctr.h"
#include "memfuncs.h"
#include "g2dma.h"

__attribute__((aligned(4))) volatile unsigned int our_ip = 0; // To be clear, this needs to be zero for init. Make that explicit here. Also, this value should be kept LE.
unsigned int tool_ip = 0;
unsigned char tool_mac[6] = {0};
unsigned short tool_port = 0;
unsigned int tool_version = 0;

static unsigned int cached_dest = 0;

/*
 * Transfer accounting, for the one read Sonic Adventure never completes.
 * dcload survives that read (it merely times out), so unlike every earlier
 * attempt these CAN be sampled live, during the 20 s the wait lasts. They
 * localise the break: no LBIN means the request never produced one, PBIN
 * accepted stuck below the expected count means packets are being lost or
 * refused, and DBIN seen with incomplete==1 means the map has holes.
 */
unsigned int g_lbin_count = 0;
unsigned int g_pbin_ok = 0;
/* LoadBinary windows opened without echoing the command back -- see
 * bin_echo_suppress(). */
unsigned int g_lbin_noecho = 0;
static unsigned int echo_suppressed = 0;

/*
 * Do not echo LoadBinary commands back to the host while set.
 *
 * On the UPLOAD path the host waits for the echo before sending any part, so
 * there it is flow control and must stay.
 *
 * The audio path (send_audio) and, since 2026-09-20, the disc-read path
 * (send_sectors) both send the whole answer -- LoadBinary, parts, ReturnValue
 * -- in one burst without waiting. The echo is then read by nobody, and
 * transmitting it from inside cmd_loadbin() puts a frame on the wire in the
 * middle of that burst. Failed audio fetches were seen receiving their
 * LoadBinary and one part and then nothing, with no receive errors counted,
 * which is what a collision with that echo would look like. cdda_fetch() and
 * ReadSectors() therefore set this for their own wait, and the two can never
 * overlap (g_gd_in_transfer). g_lbin_noecho counts the echoes skipped.
 */
void bin_echo_suppress(unsigned int on)
{
	echo_suppressed = on;
}

/*
 * While set, cmd_partbin() ends the bb->loop() wait as soon as the LoadBinary
 * window is complete, with syscall_retval = 0, instead of waiting for the
 * host's ReturnValue. Used by cdda_fetch(): the payload is what matters, and a
 * lost ReturnValue should not fail a fetch whose bytes all arrived.
 *
 * Consequence for callers: syscall_retval may still be 0 when the wait ends,
 * because the ReturnValue (which cdda_fetch() needs for its LBA echo) had not
 * been processed yet. cdda_fetch() handles that case.
 */
static unsigned int complete_escape = 0;
/* Transfers finished by their last part rather than by a ReturnValue. In a
 * healthy session this is nearly every CD-DA sub-fetch. */
unsigned int g_bin_data_done = 0;

void bin_complete_escape(unsigned int on)
{
	complete_escape = on;
}
unsigned int g_pbin_rejected = 0;

/*
 * The CD-DA staging "door".
 *
 * The host sends audio without acknowledgement round trips, so an answer to a
 * sub-fetch that already gave up can still arrive, naming the same staging
 * buffer and size as every other audio answer. If its LoadBinary were
 * accepted it would reset the current window: during a later audio fetch that
 * splices two blocks into one buffer, and between fetches it resets a disc
 * read's window.
 *
 * cdda_fetch() publishes the staging range and, while it waits, the one
 * destination it expects. cmd_loadbin() refuses any other LoadBinary into that
 * range without touching the window (g_cdda_stale_lbin). `want` is 0 whenever
 * no audio fetch is waiting, so the door is shut then.
 */
unsigned int g_bin_stage_lo = 0;    /* CD-DA staging buffer, first byte */
unsigned int g_bin_stage_hi = 0;    /* one past its last byte; 0 = no CD-DA yet */
unsigned int g_bin_stage_want = 0;  /* destination of the fetch in flight, 0 = none */
unsigned int g_cdda_stale_lbin = 0; /* abandoned answers refused at the door */
unsigned int g_dbin_count = 0;
unsigned int g_dbin_incomplete = 0;
unsigned int g_last_load_addr = 0;
unsigned int g_last_load_size = 0;
unsigned int g_last_pbin_addr = 0;
unsigned int g_last_reject_addr = 0;
/* The window that was actually in force when a part was refused, and the size
 * the part claimed. Without these, a rejection cannot be told apart from a
 * part that arrived before its own LoadBinary. */
unsigned int g_last_reject_load = 0;
unsigned int g_last_reject_end = 0;
unsigned int g_last_reject_size = 0;
unsigned int g_pbin_clamped = 0;

#define min(a, b) ((a) < (b) ? (a) : (b))

// This giant array keeps track of how many kB have been received, relative to start address of the transmitted data's destination.
// Each packet has a maximum payload size of 1440, and the nearest multiple of 1440 > 16MB is 16,784,640, which would be 11651 map indices.
// 11651 is not a multiple of 8, but 11656 is, so we can just use that.
// The multiple of 8 requirement is because memset_zeroes_64bit() is used as the only memset in this entire program, as it is the smallest way to set the most data.
/*
 * SIZED FOR THE IN-GAME CASE, NOT THE UPLOAD.
 *
 * This was 11656 -- one entry per 1440-byte part of a full 16 MB transfer --
 * and it put 11.6 KB of dcload right where Sonic Adventure puts its stack. The
 * title calls GD syscalls with SP = 0x8c00b9d0, inside this image; its stack
 * grows down through dcload's own state and eventually over `bb`, the adapter
 * pointer, after which dcload's next bb->loop() is an indirect call through
 * garbage and the guest executes at address zero. Shrinking the map is the
 * cheapest large reduction of that overlap.
 *
 * 256 entries covers 368640 bytes in one LoadBinary. The largest transfer a
 * running title asks for here is 215040. The initial upload is much bigger, so
 * the HOST now splits it into several LoadBinary transfers -- see send_data()
 * in dcload-ip-rs, which must stay in step with this number.
 */
#define BIN_INFO_MAP_SIZE 256
// This used to be 16384 for 1024-byte payload size, but by doing it this way instead we can save almost 5kB from the file size and increase the data per packet by 1.4x.
// We can also set a legacy check to use 1024-byte packets for compatibility with old versions of dc-tool (if for some reason someone needs that), although the maximum size
// for such legacy uses would be limited to 11MB. I think the gains made with the new version are definitely worth it.

typedef struct {
	unsigned int load_address;
	unsigned int load_size;
	unsigned char map[BIN_INFO_MAP_SIZE];
} bin_info_t;

// Align huge map array to 8 bytes (it's already after 2x unsigned ints)
__attribute__((aligned(8))) static bin_info_t bin_info; // Here's a global array. This one is massive, but please don't shrink it. It's meant to act as a map where each 1024B maps into 16MB RAM, and 1024B fits into a packet...

/*
 * The loader's own base address, from the linker script (dcload.x.in:
 * PROVIDE(_dcload_base = ORIGIN(ram))). Everything below that used to be
 * written as 0xac004000 / 0xac004004 names it through this instead, because
 * the base is per-title now -- the host relinks and chainloads the loader to
 * whatever address a game's DreamShell preset asks for. A literal here would
 * be a literal the linker owns, which is the shape of bug that once rebooted
 * the machine at EXEC with no breadcrumb (AGENTS.md 14.11).
 */
extern char dcload_base[];
#define DCLOAD_BASE_P2 (((unsigned int)dcload_base) | 0xa0000000U)

void cmd_reboot(void)
{
	booted = 0;
	running = 0;

//	CacheBlockPurge((void*)0x0c004000, 1536);
	asm volatile ("nop\n\t" : : : "memory"); // memory barrier for GCC
	disable_cache();
	go(DCLOAD_BASE_P2);
}

void cmd_execute(ether_header_t * ether, ip_header_t * ip, udp_header_t * udp, command_t * command)
{
	if (!running)
	{
		bb->stop(); // Disable packet RX

		tool_ip = ntohl(ip->src);
		tool_port = ntohs(udp->src);
		memcpy(tool_mac, ether->src, 6);
		our_ip = ntohl(ip->dest);

		unsigned int cmd_size = ntohl(command->size);

		unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
		command_t * response = (command_t *)buffer;
		memcpy(response, command, COMMAND_LEN);

		make_ip(tool_ip, our_ip, UDP_H_LEN + COMMAND_LEN, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
		make_udp(tool_port, ntohs(udp->dest), COMMAND_LEN, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
		bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN);

		if (!booted)
			disp_info();
		else
			disp_status("executing...");

		if (cmd_size&1)
			*((volatile unsigned int *)(DCLOAD_BASE_P2 + 4)) = 0xdeadbeef; /* enable console */
		else
			*((volatile unsigned int *)(DCLOAD_BASE_P2 + 4)) = 0xfeedface; /* disable console */

		if (cmd_size>>1)
		{
			cdfs_redir_enable();
		}

		/* If what we are starting turns out to be another dcload, this is the
		 * only chance to tell it what address we were answering on -- see the
		 * warm-start comment in rtl8139.c. A game will simply never look. */
		adapter_handoff_save(our_ip);

#if WITH_CDDA || WITH_IRQ_HOOK
		/* What a G2 DMA channel's registers say about who used it last must
		 * be about this title, not an earlier loader (g2dma.h). */
		g2dma_forget();
#endif

		running = 1;

#if GUEST_TICK
		/* Last thing before the jump, so the first sample is already the
		 * title's own code and not ours. */
		guest_tick_arm();
#endif
#if ISOLDR_SETUP_MACHINE
		/* isoldr's last step before launch(): timer restarted, ASIC
		 * interrupts quiesced, GD interrupt cleared. Not done for
		 * cmd_reboot() -- that goes back into this loader, which wants
		 * the machine exactly as it is. */
		setup_machine();
#endif
//		CacheBlockPurge((void*)0x0c004000, 1536);
		asm volatile ("nop\n\t" : : : "memory"); // memory barrier for GCC
		disable_cache();
		go(ntohl(command->address) | 0xa0000000);
	}
}

void cmd_loadbin(ip_header_t * ip, udp_header_t * udp, command_t * command)
{
	unsigned int dest = ntohl(command->address);

	/* An abandoned CD-DA answer: refuse it without opening a window (see
	 * g_bin_stage_lo). Inert until the first audio fetch publishes the range,
	 * and always inert without CD-DA, since hi stays 0. */
	if ((dest >= g_bin_stage_lo) && (dest < g_bin_stage_hi)
		&& (dest != g_bin_stage_want))
	{
		g_cdda_stale_lbin++;
		return;
	}

	bin_info.load_address = dest;
	bin_info.load_size = ntohl(command->size);
	g_lbin_count++;
	g_last_load_addr = bin_info.load_address;
	g_last_load_size = bin_info.load_size;

	// Only dc-tool 2.x and later: 1440-byte payloads (the legacy 1024-byte
	// mode was removed, AGENTS.md 4.3).
	// Max size check (16MB, RAM size)
	if(bin_info.load_size > 16777216)
	{
		// Send error, exit, and bail
		write(1, "ERROR: Size >16MB\r\n", 20);
		dcexit();
		bb->start(); // dcexit calls RX stop, so need to re-enable that

		return;
	}

	// Zero out the received packet map
	memset_zeroes_64bit(bin_info.map, BIN_INFO_MAP_SIZE/8);

	our_ip = ntohl(ip->dest);

	unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
	command_t * response = (command_t *)buffer;
	memcpy(response, command, COMMAND_LEN);

	/* Every RAM destination needs the purge, P2 included: cmd_partbin's copy
	 * stores into the P1 alias of the destination (memdiff(), memfuncs.c), so
	 * with a title's P1 in copy-back the bytes sit in dirty lines that a
	 * reader through P2 never sees. GTA2 reads its file headers by PIO into
	 * 0xac37xxxx buffers and hung on garbage (2026-10-02). Only P4 is left
	 * alone: it is not RAM, and the store did not go there anyway. */
	unsigned int cacheable_check = bin_info.load_address >> 29;
	if(cacheable_check != 0x7)
	{
		cached_dest = 1;
	}
	else
	{
		cached_dest = 0;
	}

	if (echo_suppressed)
	{
		/* The response was built into pkt_buf above and is simply not sent.
		 * Building it costs nothing and keeps this function one shape. */
		g_lbin_noecho++;
	}
	else
	{
		make_ip(ntohl(ip->src), our_ip, UDP_H_LEN + COMMAND_LEN, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
		make_udp(ntohs(udp->src), ntohs(udp->dest), COMMAND_LEN, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
		bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN);
	}

	if (!running) {
		if (!booted)
			disp_info();
		disp_status("receiving data...");
	}
}

/* Is every chunk of the current LoadBinary window accounted for?
 *
 * The map is already maintained for cmd_donebin, so this costs a handful of
 * byte tests and NO ROUND TRIP -- which is the whole point: it lets a caller
 * find out that its transfer has a hole in it without asking the host, at the
 * moment it matters rather than several hundred milliseconds later. */
int bin_window_complete(void)
{
	unsigned int chunks;
	unsigned int i;

	if (!bin_info.load_size)
	{
		return 0;
	}
	chunks = (bin_info.load_size + 1439u) / 1440u;
	if (chunks > BIN_INFO_MAP_SIZE)
	{
		return 0;
	}
	for (i = 0; i < chunks; i++)
	{
		if (!bin_info.map[i])
		{
			return 0;
		}
	}
	return 1;
}

void bin_window_close(void)
{
	/* Zero size: every branch of cmd_partbin's window test then refuses, and
	 * counts. Nothing else reads the window until the next cmd_loadbin, which
	 * sets both fields afresh. */
	bin_info.load_size = 0;
}

void cmd_partbin(command_t * command)
{
	int index = 0;
	unsigned int cmd_addr = ntohl(command->address);
	unsigned int cmd_size = ntohl(command->size);
	unsigned int load_start = bin_info.load_address;
	unsigned int load_end = load_start + bin_info.load_size;
	unsigned int cmd_end = cmd_addr + cmd_size;

	/* Drop stale/out-of-window PBIN packets. During runtime CDFS transfers,
	 * late UDP packets from a previous LBIN can otherwise underflow the
	 * packet map index and corrupt memory. */
	if (cmd_size == 0)
	{
		g_pbin_rejected++;
		return;
	}
	if ((cmd_addr < load_start) || (cmd_end < cmd_addr))
	{
		/* Starts before the window, or the length wrapped. Nothing sane to do
		 * with that. */
		g_pbin_rejected++;
		g_last_reject_addr = cmd_addr;
		g_last_reject_load = load_start;
		g_last_reject_end = load_end;
		g_last_reject_size = cmd_size;
		return;
	}
	if (cmd_addr >= load_end)
	{
		/* Entirely past the window: a straggler from a previous transfer. */
		g_pbin_rejected++;
		g_last_reject_addr = cmd_addr;
		g_last_reject_load = load_start;
		g_last_reject_end = load_end;
		g_last_reject_size = cmd_size;
		return;
	}
	if (cmd_end > load_end)
	{
		/*
		 * CLAMP, DO NOT DROP.
		 *
		 * The part starts inside the window and runs past its end. Refusing it
		 * outright throws away bytes the host believes it delivered: the map
		 * entry never gets set, DoneBinary reports the hole, and the same
		 * oversized part is resent and refused again. Measured on Sonic
		 * Adventure's fatal read -- window (0x0cef7000, 16384), a part landing
		 * at 0x0cefade0 whose end reaches 0x0cefb380, 896 bytes past -- and
		 * every one of that chunk's twelve parts was lost this way.
		 *
		 * Taking the portion that fits is safe: the write is bounded by the
		 * window the LoadBinary established, which is exactly the guarantee
		 * this check exists to enforce.
		 */
		g_pbin_clamped++;
		cmd_size = load_end - cmd_addr;
		cmd_end = load_end;
	}

	// Thanks to packet buffer alignment, command->data is guaranteed to be 8-byte aligned.
	// If the destination address is 8-byte aligned, this will be a rocket.
//	memcpy((unsigned char *)cmd_addr, command->data, ntohl(command->size));

	// cmd_addr needs to honor whatever dc-tool sends. Use P0 addresses for cache boost when writing to RAM.
	// Something great for alignment reasons is that, in addition to being the max payload size, 1440 bytes is an even multiple of 32 bytes.
	SH4_aligned_memcpy((void*)cmd_addr, to_p1(command->data), cmd_size);
	if(cached_dest)
	{
		/* Flush exactly the cache lines touched by this packet write, through
		 * the P1 alias the copy stored into (see cmd_loadbin). */
		unsigned int p1_addr = (unsigned int)to_p1((void *)cmd_addr);
		unsigned int purge_base = p1_addr & ~31U;
		unsigned int purge_end = (p1_addr + cmd_size + 31U) & ~31U;

		/* Clamp to the end of the 16MB RAM aperture. */
		if ((p1_addr & 0x1f000000U) == 0x0c000000U)
		{
			if (purge_end > 0x8d000000U)
			{
				purge_end = 0x8d000000U;
			}
		}

		if (purge_end > purge_base)
		{
			CacheBlockPurge((void*)purge_base, (purge_end - purge_base) / 32U);
		}
	}
	// Ensure physical memory is actually written to from the cache, since we don't know how it might be used.
	// Purge instead of writeback to avoid cache conflicts/trashing.

	index = (cmd_addr - bin_info.load_address) / 1440; // /1440 = 64-bit multiplication trick
	if ((unsigned int)index >= BIN_INFO_MAP_SIZE)
	{
		g_pbin_rejected++;
		g_last_reject_addr = cmd_addr;
		return;
	}
	bin_info.map[index] = 1;
	g_pbin_ok++;
	g_last_pbin_addr = cmd_addr;

	if (complete_escape && bin_window_complete())
	{
		/* Exactly what cmd_retval would have done, a packet earlier. */
		g_bin_data_done++;
		syscall_retval = 0;
		escape_loop = 1;
	}
}

void cmd_donebin(ip_header_t * ip, udp_header_t * udp, command_t * command)
{
	unsigned int i;
	unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
	command_t * response = (command_t *)buffer;
	memcpy(response, command, COMMAND_LEN);

	unsigned int map_index_verify, payload_size;

	// Legacy check for versions < 2.0.0
	// Need to hardcode these divides so that GCC can optimize them out (and
	// thankfully it is able to do so in these two scenarios, as it can convert
	// them into 64-bit multiplication)
	map_index_verify = (bin_info.load_size + 1439) / 1440;
	payload_size = 1440;

	g_dbin_count++;
	for(i = 0; i < map_index_verify; i++)
		if (!bin_info.map[i])
			break;
	if (i != map_index_verify)
		g_dbin_incomplete++;

	if(i == map_index_verify)
	{
		response->address = 0;
		response->size = 0;
	}
	else
	{
		response->address = htonl(bin_info.load_address + i * payload_size);
		response->size = htonl(min(bin_info.load_size - i * payload_size, payload_size));
	}

	make_ip(ntohl(ip->src), ntohl(ip->dest), UDP_H_LEN + COMMAND_LEN, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
	make_udp(ntohs(udp->src), ntohs(udp->dest), COMMAND_LEN, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
	bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN);

	if (!running) {
		if (!booted)
			disp_info();
		disp_status("idle...");
	}
}

/*
 * WHY THERE IS NO GD-TRANSFER GUARD HERE, having briefly had one.
 *
 * This builds a reply in pkt_buf and transmits it, and pkt_buf is single and
 * shared -- so refusing to answer while a disc read is in flight looks like the
 * obvious reading of the invariant at the top of cdfs_syscalls.c. It is the
 * wrong reading, and it cost every counter a running title could report.
 *
 * The window that invariant is about is between BUILDING a command in pkt_buf
 * and SENDING it -- the trace bug written up in ReadSectors(), where a write()
 * laid its own command over a read request that had not gone out yet. This
 * function is only ever reached from inside bb->loop(), which a caller enters
 * AFTER build_send_packet() has already transmitted. And a retry does not
 * re-send the buffer: GD_READ_RETRIES calls ReadSectors() again, which rebuilds
 * the request from scratch. So clobbering pkt_buf during the wait costs
 * nothing.
 *
 * Measured, 2026-08-29: with the guard in, a title that never sets
 * GD_SERVICE_EVERY_SYSCALL is answered from data_transfer's bb->loop() and
 * nowhere else -- and the GD lock is held for all of it, so the guard refused
 * 100% of reads and the panel never showed a single sample.
 */
void cmd_sendbinq(ip_header_t * ip, udp_header_t * udp, command_t * command)
{
	our_ip = ntohl(ip->dest);

	unsigned int payload_size, numpackets, i;
	unsigned int bytes_thistime;

	unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
	command_t * response = (command_t *)buffer;

	unsigned int cmd_addr = ntohl(command->address);
	unsigned int bytes_left = ntohl(command->size);

	// KEPT FOR THE TERMINATOR, and it is not cosmetic -- see below.
	const unsigned int req_addr = cmd_addr;
	const unsigned int req_size = bytes_left;

	// Legacy check for versions < 2.0.0
	// Need to hardcode these divides so that GCC can optimize them out (and
	// thankfully it is able to do so in these two scenarios, as it can convert
	// them into 64-bit multiplication)
	payload_size = 1440;
	numpackets = (bytes_left + 1439) / 1440;

	unsigned int ip_src = ntohl(ip->src);
	unsigned short udp_src = ntohs(udp->src);
	unsigned short udp_dest = ntohs(udp->dest);

	memcpy(response->id, CMD_SENDBIN, 4);

	for(i = 0; i < numpackets; i++)
	{
		if (bytes_left >= payload_size)
			bytes_thistime = payload_size;
		else
			bytes_thistime = bytes_left;
		bytes_left -= bytes_thistime;

		// By aligning the transmit buffer, response->data is always aligned to 8 bytes.
		// 'cmd_addr' may or may not be, but if it is, this will be a rocket.
//		memcpy(response->data, (void*)cmd_addr, bytes_thistime);

		// cmd_addr needs to honor whatever dc-tool sends. Use P0 addresses for cache boost when reading from RAM.
		// Something great for alignment reasons is that, in addition to being the max payload size, 1440 bytes is an even multiple of 32 bytes.
		SH4_aligned_memcpy(to_p1(response->data), (void*)cmd_addr, bytes_thistime);

		response->address = htonl(cmd_addr);
		response->size = htonl(bytes_thistime);
		make_ip(ip_src, our_ip, UDP_H_LEN + COMMAND_LEN + bytes_thistime, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
		make_udp(udp_src, udp_dest, COMMAND_LEN + bytes_thistime, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
		bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN + bytes_thistime);
		cmd_addr += bytes_thistime;
	}

	// THE TERMINATOR NAMES THE RANGE IT JUST SERVED. It used to be 0/0, which
	// is byte for byte what `cmd_donebin()` answers when a LoadBinary window is
	// COMPLETE -- so the two were indistinguishable on the wire, and the host
	// has no way to tell "the read you asked for is finished" from "the upload
	// you are pushing has landed". That ambiguity was harmless only for as long
	// as nobody read memory while a transfer was in flight.
	//
	// The counter panel does exactly that (dcload-ip-rs `--diag`), and it broke
	// both ways at once, measured 2026-08-30 on Sonic Adventure: the host's
	// filter swallowed a *transfer's* DoneBinary and the sector read died with
	// "No DoneBinary response received", while this reply leaked the other way
	// and was read as "nothing missing anywhere: done" -- a sector read
	// credited as complete with data still missing. Either way the title
	// froze for the loader's whole 6-second timeout plus the retry.
	//
	// Naming the range costs nothing (the fields are already on the wire and
	// no host reads them here -- `dc-tool-ip` and the Rust host both match on
	// the 4-byte id alone) and makes the two answers disjoint by construction:
	// a SendBinQ terminator names loader RAM, a LoadBinary terminator names a
	// game buffer or nothing at all.
	memcpy(response->id, CMD_DONEBIN, 4);
	response->address = htonl(req_addr);
	response->size = htonl(req_size);
	make_ip(ip_src, our_ip, UDP_H_LEN + COMMAND_LEN, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
	make_udp(udp_src, udp_dest, COMMAND_LEN, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
	bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN);
}

void cmd_sendbin(ip_header_t * ip, udp_header_t * udp, command_t * command)
{
	if (!running) {
		if (!booted)
			disp_info();
		disp_status("sending data...");
	}

	cmd_sendbinq(ip, udp, command);

	if (!running) {
		disp_status("idle...");
	}
}

void cmd_version(ip_header_t * ip, udp_header_t * udp, command_t * command)
{
	int datalength, j;
	unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
	command_t * response = (command_t *)buffer;

	// Address field isn't used in the command, so dc-tool stuffs its version in here now
	// Added in 2.0.0 for dc-load to know what version of dc-tool is being used
	// Format is a uint, encoded like this: (major << 16) | (minor << 8) | patch
	// Old versions of dc-tool will have a version of 0, so it's easy to check for them
	// (and they all expect a packet payload size of 1024 for TX/RX; this version command
	// was added when packet sizes switched to 1440 bytes of payload data)
	tool_version = ntohl(command->address);	// This global variable is used in the major/minor/patch version macros.

	dcload_syscall_port = ntohs(udp->dest);

	datalength = strlen("dcload-ip " DCLOAD_VERSION " using "); // no '+1' because adapter name will be appended
	memcpy(response, command, COMMAND_LEN);
	memcpy(response->data, "dcload-ip " DCLOAD_VERSION " using ", datalength);

	// Append adapter type
	j = strlen(bb->name) + 1;
	// the 'data' member is an unsigned char pointer
	memcpy(response->data + datalength, bb->name, j);
	datalength += j;

	/*
	 * Then four raw bytes: the address this loader was linked at, big-endian
	 * like every other number on the wire.
	 *
	 * The host needs it to decide whether to chainload. A title's DreamShell
	 * preset names the address the loader has to be at to stay out of its way
	 * (`memory`), and the host answers that by uploading a loader relinked for
	 * it -- but only when the one already running is somewhere else. Probing
	 * for the 0xdeadbeef magic at each candidate base instead would be several
	 * round trips and would still not distinguish a live loader from the
	 * remains of a previous one.
	 *
	 * This goes AFTER the string's NUL, and `size` covers it. An older host
	 * treats the payload as a C string and prints it unchanged; there is no
	 * version gate to get wrong.
	 */
	unsigned int base_be = htonl((unsigned int)dcload_base);
	memcpy(response->data + datalength, &base_be, 4);
	datalength += 4;

	/*
	 * And four more: which video cable this console is plugged into
	 * (0 = VGA, 2 = RGB, 3 = composite), read off PDTRA the same way the
	 * BootROM and every Katana title read it.
	 *
	 * The host uses it to decide whether to patch a title's own cable check:
	 * forcing a title to VGA is right when there is a VGA box on the other
	 * end and a black screen when there is a television, and nothing on the
	 * host's side of the wire can see which. Only this end can answer, and
	 * answering costs two register reads.
	 *
	 * FIELDS ARE READ IN ORDER FROM THE STRING'S NUL, not backwards from the
	 * end. A host that took the last four bytes as the base -- which is what
	 * the first version of this did -- reads this word instead the moment a
	 * second field exists, decides the base is implausible, and quietly stops
	 * relocating anything. That failure is silent, so the parser was changed
	 * before this field was added rather than after.
	 */
	unsigned int cable_be = htonl(STARTUP_Get_Cable());
	memcpy(response->data + datalength, &cable_be, 4);
	datalength += 4;

	response->size = htonl(datalength);
	// Stuff the adapter type inside the otherwise unused address field. :)
	// Added in version 2.0.0 for dc-tool-ip to be able to do performance tuning
	// based on which adapter is installed.
	response->address = htonl(installed_adapter);

	make_ip(ntohl(ip->src), ntohl(ip->dest), UDP_H_LEN + COMMAND_LEN + datalength, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
	make_udp(ntohs(udp->src), dcload_syscall_port, COMMAND_LEN + datalength, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
	bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN + datalength);
}

void cmd_retval(ip_header_t * ip, udp_header_t * udp, command_t * command)
{
	if(running)
	{
		bb->stop(); // Disable packet RX

		unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
		command_t * response = (command_t *)buffer;
		memcpy(response, command, COMMAND_LEN);

		make_ip(ntohl(ip->src), ntohl(ip->dest), UDP_H_LEN + COMMAND_LEN, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
		make_udp(ntohs(udp->src), ntohs(udp->dest), COMMAND_LEN, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
		bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN);

		syscall_retval = ntohl(command->address);
		syscall_retsize = ntohl(command->size);
		syscall_data = command->data;
		escape_loop = 1;
	}
}

#if WITH_MAPLE
void cmd_maple(ip_header_t * ip, udp_header_t * udp, command_t * command)
{
	char *res;
	int i, tries;
	unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
	command_t * response = (command_t *)buffer;

	memcpy(response, command, COMMAND_LEN);

	/* MAPLE_RESPONSE_AGAIN is "busy, ask me again", and a VM2/VMUPro answers it
	 * while it swaps memory-card images (AGENTS.md 8). BOUNDED: an unbounded
	 * retry is a hardware wait with no deadline sitting in the command loop,
	 * and a device that never clears would leave the loader deaf with no way
	 * for the host to recover. The host gets the AGAIN and decides. */
	tries = 64;
	do {
		res = maple_docmd(command->data[0], command->data[1], command->data[2], command->data[3], command->data + 4);
	} while ((*res == MAPLE_RESPONSE_AGAIN) && --tries);

	/* Send response back over socket.
	 *
	 * res[3] is UNSIGNED: it is the device's own length field, in longwords,
	 * and `char` is signed on sh-elf. A device claiming 128 or more -- the
	 * protocol allows up to 255 -- made `i` negative, and SH4_aligned_memcpy
	 * takes an unsigned length, so that is a four-gigabyte copy out of a 1 KB
	 * buffer. Cast, and the worst case is the 1024 bytes the receive buffer
	 * holds, which response->data has room for. */
	i = ((res[0] < 0) ? 4 : (((unsigned char)res[3] + 1) << 2));
	response->size = htonl(i);
	// By aligning the transmit buffer, response->data is always aligned to 8 bytes.
	// 'res' may or may not be, but if it is, this will be a rocket.
//	memcpy(response->data, res, i);
	/* READ THE RESPONSE THROUGH P2, NOT to_p1(res).
	 *
	 * `res` points at the buffer the Maple DMA just wrote, and the SH4's
	 * operand cache does not snoop DMA. Reading it through P1 was fine exactly
	 * once: that read left clean lines resident over the receive buffer, and
	 * every later MAPL command hit them and handed the host the PREVIOUS
	 * command's response. Probing four ports for a VM2 would report port A's
	 * device on all of them. Uncached here; the copy is at most 196 bytes on a
	 * host-driven path. */
	SH4_aligned_memcpy(to_p1(response->data), (void *)res, i);

	make_ip(ntohl(ip->src), ntohl(ip->dest), UDP_H_LEN + COMMAND_LEN + i, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
	make_udp(ntohs(udp->src), ntohs(udp->dest), COMMAND_LEN + i, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
	bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN + i);
}
#endif /* WITH_MAPLE */

#if WITH_MARK_CMD
/*
 * MARK: witness words in RAM, so the host learns what the title WRITES.
 *
 * The host's memory map (dcload-ip-rs game-memory.tsv) learned only where
 * disc reads land. What a title writes with the CPU -- a heap, a
 * decompressed file, a table -- it never saw, and that is what overwrote the
 * loader under Shenmue II: ~2 KB at 0x8cfd0000 at the end of a cinematic,
 * in a block no read had ever touched (2026-09-30).
 *
 * So, before EXEC, the host asks for RAM no read has landed in to be painted:
 * one word every MARK_STRIDE bytes, holding its own P1 address ^ MARK_KEY
 * (an address-dependent value, so a title copying painted RAM elsewhere is
 * still caught). While the title runs, it asks which 64 KB blocks still carry
 * every word, and records the others. Done here rather than from the host
 * because painting 16 MB over the network is seconds, here milliseconds, and
 * a check is one datagram each way instead of a SendBinQ per sample.
 *
 *   address  start, 64 KB aligned (any segment)
 *   size     bytes, a multiple of 64 KB, at most 16 MB;
 *            bit 31 clear = paint, set = check
 *   reply    MARK, same address. Paint: size 0. Check: size = the bitmap's
 *            bytes, bit i (LSB first) = block i has a changed word.
 *            Refused: size 0xffffffff, nothing done.
 *
 * Painting goes through P2, so nothing sits dirty in the cache when go.S
 * turns it off. A check purges each sampled line first (ocbp: a write the
 * title left in the cache reaches RAM) and then reads through P2 (a line
 * brought in by an earlier check cannot hide a later DMA or P2 write).
 *
 * A check costs one purge and one uncached read per sample, and stops at the
 * first changed word of a block: ~256 samples per clean 64 KB block. The host
 * asks about a few blocks at a time, because this runs inside the title's GD
 * wait or the interrupt tick.
 *
 * Refused if the range reaches anything of the loader's own: a painted word in
 * its image, stack, .hiram or Maple page would be a loader that stops
 * answering, and only the host's arithmetic stands between the two.
 */
#define MARK_KEY    0x5a3cc3a5U
#define MARK_STRIDE 256U

extern char mark_stack_top[] __asm__("_stack");
extern char _hiram_start[], _hiram_end[];
extern char maple_dma_buffer[];

static int mark_overlaps(unsigned int a, unsigned int e, const char *lo, unsigned int hi)
{
	return a < hi && e > (unsigned int)lo;
}

void cmd_mark(ip_header_t * ip, udp_header_t * udp, command_t * command)
{
	unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
	command_t * response = (command_t *)buffer;
	unsigned int size = ntohl(command->size);
	unsigned int check = size >> 31;
	unsigned int blocks = (size & 0x7fffffffU) >> 16;
	unsigned int a = (ntohl(command->address) & 0x1fff0000U) | 0x80000000U;
	unsigned int e = a + (blocks << 16);
	unsigned int i, n = 0;

	memcpy(response, command, COMMAND_LEN);

	if (blocks > 256 || e > 0x8d000000U
	    || mark_overlaps(a, e, dcload_base, (unsigned int)mark_stack_top)
	    || mark_overlaps(a, e, _hiram_start, (unsigned int)_hiram_end)
	    || mark_overlaps(a, e, maple_dma_buffer, (unsigned int)maple_dma_buffer + 0x1000U))
	{
		response->size = htonl(0xffffffffU);
	}
	else
	{
		for (i = 0; i < blocks; i++)
		{
			unsigned int p = a + (i << 16);
			unsigned int top = p + 0x10000U;

			if (!(i & 7))
			{
				response->data[i >> 3] = 0;
			}
			for (; p < top; p += MARK_STRIDE)
			{
				volatile unsigned int *w = (volatile unsigned int *)(p | 0x20000000U);

				if (!check)
				{
					*w = p ^ MARK_KEY;
					continue;
				}
				__asm__ volatile ("ocbp @%0" : : "r" (p) : "memory");
				if (*w != (p ^ MARK_KEY))
				{
					response->data[i >> 3] |= 1 << (i & 7);
					break;
				}
			}
		}
		if (check)
		{
			n = (blocks + 7) >> 3;
		}
		response->size = htonl(n);
	}

	make_ip(ntohl(ip->src), ntohl(ip->dest), UDP_H_LEN + COMMAND_LEN + n, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
	make_udp(ntohs(udp->src), ntohs(udp->dest), COMMAND_LEN + n, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
	bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN + n);
}
#endif /* WITH_MARK_CMD */

#if WITH_PMCR_CMD
// The 6 performance counter control functions are:
/*
	// (I) Clear counter and enable
	void PMCR_Init(unsigned char which, unsigned char mode, unsigned char count_type);

	// (E) Enable one or both of these "undocumented" performance counters
	void PMCR_Enable(unsigned char which, unsigned char mode, unsigned char count_type, unsigned char reset_counter);

	// (B) Disable, clear, and re-enable with new mode (or same mode)
	void PMCR_Restart(unsigned char which, unsigned char mode, unsigned char count_type);

	// (R) Read a counter
	unsigned long long int PMCR_Read(unsigned char which);

	// (G) Get a counter's current configuration
	unsigned short PMCR_Get_Config(unsigned char which);

	// (S) Stop counter(s) (without clearing)
	void PMCR_Stop(unsigned char which);

	// (D) Disable counter(s) (without clearing)
	void PMCR_Disable(unsigned char which);
*/
// The command is the first letter of the function name (capitalized), followed by each of the function's parameters.
// Note that restart's command letter is 'B'--read is 'R', so restart is 'B' (think reBoot). The command letters are included in parentheses for each function in the above comment block.
//
// Sending command data of 'D' 0x1 (2 bytes) disables ('D') perf counter 1 (0x1)
// Sending command data 'E' 0x3 0x23 0x0 0x1 (5 bytes) enables ('E') both perf counters (0x3) to elapsed time mode (0x23) where count is 1 cpu cycle = 1 count (0x0) and continue the counter from its current value (0x1)
// Sending command data 'B' 0x2 0x23 0x1 (4 bytes) restarts ('B') perf counter 2 (0x2) to elapsed time mode (0x23) and count is CPU/bus ratio method (0x1)
// ...
// etc.
//
// Notes:
// - Remember to disable before leaving DCLOAD to execute a program if needed.
// - See perfctr.h for how to calculate time using the CPU/bus ratio method.
// - PMCR_Init() and PMCR_Enable() will do nothing if the perf counter is already running!

static char * ok_message = "OK";
static char * invalid_read_message = "PMCR: chan 1 or 2 only.";
static char * invalid_function_message = "PMCR: I, E, B, R, G, S, or D only.";
static char * invalid_option_message = "PMCR: chan 1, 2, or 3 (both) only.";
static char * invalid_mode_message = "PMCR modes: 0x1-0x29.";
static char * invalid_count_type_message = "PMCR count: 0 or 1 only.";
static char * invalid_reset_type_message = "PMCR reset: 0 or 1 only.";
static volatile unsigned int read_array[2] = {0};
static volatile unsigned char getconfig_array[2] = {0};

void cmd_pmcr(ip_header_t * ip, udp_header_t * udp, command_t * command)
{
	char * out_message = ok_message;
	unsigned int i = 3;
	unsigned char invalid_pmcr = 0, invalid_mode = 0, invalid_count = 0, invalid_reset = 0;

	unsigned char read = 0;

	unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
	command_t * response = (command_t *)buffer;

	// Size is 2, 4, or 5 bytes depending on the command. Easy!
	// No need for address, it's not used here so it can be whatever.
	// Size isn't actually checked, either...
	memcpy(response, command, COMMAND_LEN);

	if((!command->data[1]) || (command->data[1] > 3))
	{
		invalid_pmcr = 1;
	}
	else if(command->data[0] == 'I') // Init
	{
		if((!command->data[2]) || (command->data[2] > 0x29))
		{
			invalid_mode = 1;
		}
		else if(command->data[3] > 1)
		{
			invalid_count = 1;
		}
		else
		{
			PMCR_Init(command->data[1], command->data[2], command->data[3]);
		}
	}
	else if(command->data[0] == 'E') // Enable
	{
		if((!command->data[2]) || (command->data[2] > 0x29))
		{
			invalid_mode = 1;
		}
		else if(command->data[3] > 1)
		{
			invalid_count = 1;
		}
		else if(command->data[4] > 1)
		{
			invalid_reset = 1;
		}
		else
		{
			PMCR_Enable(command->data[1], command->data[2], command->data[3], command->data[4]);
		}
	}
	else if(command->data[0] == 'B') // Restart
	{
		if((!command->data[2]) || (command->data[2] > 0x29))
		{
			invalid_mode = 1;
		}
		else if(command->data[3] > 1)
		{
			invalid_count = 1;
		}
		else
		{
			PMCR_Restart(command->data[1], command->data[2], command->data[3]);
		}
	}
	else if(command->data[0] == 'R') // Read
	{
		if((!command->data[1]) || (command->data[1] > 2))
		{
			out_message = invalid_read_message;
			i = 24;
		}
		else
		{
			PMCR_Read(command->data[1], read_array);
			read = 1;
		}
	}
	else if(command->data[0] == 'G') // Get Config
	{
		if((!command->data[1]) || (command->data[1] > 2))
		{
			out_message = invalid_read_message;
			i = 24;
		}
		else
		{
			*(unsigned short*)getconfig_array = PMCR_Get_Config(command->data[1]);
			read = 2;
		}
	}
	else if(command->data[0] == 'S') // Stop
	{
		PMCR_Stop(command->data[1]);
	}
	else if(command->data[0] == 'D') // Disable
	{
		PMCR_Disable(command->data[1]);
	}
	else // Respond with invalid perfcounter option
	{
		out_message = invalid_function_message;
		i = 35;
	}

	// Error and read flag checks
	if(invalid_pmcr)
	{
		out_message = invalid_option_message;
		i = 35;
	}
	else if(invalid_mode)
	{
		out_message = invalid_mode_message;
		i = 22;
	}
	else if(invalid_count)
	{
		out_message = invalid_count_type_message;
		i = 25;
	}
	else if(invalid_reset)
	{
		out_message = invalid_reset_type_message;
		i = 25;
	}

	if(read == 1) // This will send little endian perf counter value as the response.
	{
		i = 8; // 64-bit value is 8 bytes
		out_message = (char*)read_array; // C lets you do this :)
	}
	else if(read == 2) // Send little endian config data as response
	{
		i = 2; // Config is 2 bytes
		out_message = (char*)getconfig_array;
	}
	// Make and send response

	memcpy(response->data, out_message, i);
	response->size = htonl(i);

	// make_ether was run in net.c already
	make_ip(ntohl(ip->src), ntohl(ip->dest), UDP_H_LEN + COMMAND_LEN + i, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
	make_udp(ntohs(udp->src), ntohs(udp->dest), COMMAND_LEN + i, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
	bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN + i);
}
#endif /* WITH_PMCR_CMD */

/*
// command_t struct here For reference

typedef struct {
	unsigned char id[4];
	unsigned int address;
	unsigned int size;
	unsigned char data[];
} command_t;
*/
