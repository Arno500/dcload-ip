/*
 * The title's Ethernet driver, served by the loader (AGENTS.md 4.21).
 *
 * A title that plays on the network (Unreal Tournament; Phantasy Star Online
 * carries the same core behind another API, see "THE OTHER API" below)
 * carries Sega's own Broadband Adapter driver, the ETD library:
 * its init powers the GAPS bridge down and up, clears the adapter's 32 KB of
 * SRAM -- the loader's RX ring -- and resets the chip, and the loader, whose
 * disc reads travel through that same chip, is deaf from then on. The host
 * used to stop it by making the driver find no adapter (gaps_control_patches),
 * which is the title's offline path.
 *
 * ONE OWNER, ONE CHIP. Two drivers cannot share an RTL8139: the RX ring has a
 * single read pointer (CAPR) and the four TX descriptors are taken in turn, so
 * one frame sent by the other driver desynchronises both. The loader keeps the
 * chip, and the host points the title's ETD entry points here instead
 * (dcload-ip-rs src/vnic.rs): the library's API is the boundary, not the chip's
 * registers, which an MMU-less title touches directly.
 *
 * THE CONTRACT, as Unreal Tournament PAL's copy of ETD shows it (2026-10-11):
 *   ETD_Init()                      0, or an adapter error
 *   ETD_Stop()                      bridge off (here: the title's traffic off)
 *   four setters (cb, ctx)          the RX, TX, (unused), link callbacks; the
 *                                   loader does not replace them and reads the
 *                                   driver's own words (g_vnic_slot, from the host)
 *   ETD_GetEthAddr(mac)             the six bytes of the station address
 *   ETD_SetCtlPrm(prm)              filters and modes; accepted, not applied
 *   link(state *)                   1 = up, state 1..4; calls the link callback
 *   ETD_CtlXmit(on)                 the title's traffic on or off
 *   ETD_SetRecvBuf(buf)             called by the RX callback, see below
 *   ETD_SetSendBuf(buf, len)        one frame; the TX callback gives buf back
 * A frame for the title is delivered the way ETD's interrupt handler did it:
 * RX(ctx, len, 0) -- "a frame of len bytes, where?" -- to which the title
 * answers with SetRecvBuf(buf), or SetRecvBuf(0) to drop it; then the bytes are
 * copied and RX(ctx, 0, buf) says they are there: the title finds the frame by
 * that address (2026-10-12: passing the length instead, every frame was lost,
 * ARP replies included, and Unreal Tournament never resolved its gateway).
 * The title's callbacks only
 * move a buffer between two lists under its own lock (IMASK 15, SR.BL left
 * alone), so they may be called from the tick or from inside a GD wait, as
 * ETD's handler called them from an interrupt.
 *
 * WHICH FRAMES ARE THE LOADER'S: UDP to our address and the port the host
 * talks to (dcload_syscall_port), and ICMP echo to our address. ARP goes to
 * both, each answering for its own address. Everything else is the title's:
 * its own address (the same, if its DHCP is answered for our MAC), broadcasts,
 * DHCP.
 *
 * WHEN. Reception stays on while the title's driver is (rtl_bb_stop() leaves it
 * so), the chip's RX interrupt is routed to the hook, and the tick drains the
 * ring when it fires -- or every VNIC_POLL_TICKS, a safety net. A frame the
 * title sends waits for the next tick that owns the network (GD lock free, no
 * CD-DA fetch): one interrupt away, ~1 ms under a Katana title.
 */

#include <string.h>
#include "vnic.h"
#include "adapter.h"
#include "cdfs.h"
#include "memfuncs.h"
#include "net.h"
#include "packet.h"
#include "syscalls.h"
#include "irq.h"

#if WITH_VNIC

/* The ETD driver's own words, written by the host before EXEC: the four
 * callbacks, then their four contexts. 0 = not told (an older host). */
enum { VNIC_CB_RX, VNIC_CB_TX, VNIC_CB_UNUSED, VNIC_CB_LINK, VNIC_CTX = 4 };
unsigned int g_vnic_slot[8];

volatile unsigned int g_vnic_on;
static volatile unsigned int vnic_start;	/* reception to be switched on by the tick */
static unsigned char *volatile vnic_rbuf;	/* SetRecvBuf()'s answer to the RX callback */
static volatile unsigned int vnic_tx_buf;	/* the frame SetSendBuf() queued, 0 = none */
static volatile unsigned int vnic_tx_len;
/* PSO's send() gives its buffer back on return: its frame waits here. */
static unsigned char vnic_txcopy[RX_PKT_BUF_SIZE] __attribute__((aligned(4)));

unsigned int g_vnic_rx;		/* frames handed to the title */
unsigned int g_vnic_rx_drop;	/* the title had no callback or no buffer for one */
unsigned int g_vnic_tx;		/* the title's frames sent */
unsigned int g_vnic_tx_fail;	/* refused by the adapter, or longer than a frame */

/* Ticks between two looks at the ring when no RX interrupt came: 5 ms. The
 * interrupt is the normal way in; this catches a frame it did not announce. */
#define VNIC_POLL_TICKS 62500U
/* Frames sent per tick: each goes as soon as the title's TX callback hands the
 * next one over. */
#define VNIC_TX_PER_TICK 4U

typedef unsigned int (*vnic_cb_t)(unsigned int ctx, unsigned int a, unsigned int b, unsigned int c);

/* Which driver the title carries: ETD's API (0), or Phantasy Star Online's
 * (1, set by its init, index 8 -- see "THE OTHER API" below). */
static unsigned int vnic_api_pso;

static unsigned int vnic_word(unsigned int i)
{
	unsigned int at = g_vnic_slot[i];

	return at ? *(volatile unsigned int *)at : 0U;
}

/* Call one of the title's callbacks, if it set one; what it returns, else 0. */
static unsigned int vnic_cb(unsigned int i, unsigned int a, unsigned int b)
{
	vnic_cb_t f = (vnic_cb_t)vnic_word(i);

	return f ? f(vnic_word(VNIC_CTX + i), a, b, 0) : 0U;
}

#define VNIC_TITLE 0
#define VNIC_OURS  1
#define VNIC_BOTH  2

static int vnic_whose(const unsigned char *f, unsigned int len)
{
	const ip_header_t *ip = (const ip_header_t *)(f + ETHER_H_LEN);
	const unsigned char *l4;

	if (f[12] == 0x08 && f[13] == 0x06)
	{
		return VNIC_BOTH;
	}
	if (f[12] != 0x08 || f[13] != 0x00 || len < ETHER_H_LEN + IP_H_LEN + UDP_H_LEN
	    || ntohl(ip->dest) != our_ip || (ip->flags_frag_offset & 0xff3f))
	{
		return VNIC_TITLE;
	}
	l4 = (const unsigned char *)ip + 4 * (ip->version_ihl & 0x0f);
	if (ip->protocol == IP_ICMP_PROTOCOL)
	{
		return l4[0] == 8 ? VNIC_OURS : VNIC_TITLE;	/* echo request */
	}
	return ip->protocol == IP_UDP_PROTOCOL
		&& ((l4[2] << 8) | l4[3]) == dcload_syscall_port ? VNIC_OURS : VNIC_TITLE;
}

int vnic_rx(unsigned char *frame, unsigned int len)
{
	int whose;
	unsigned int i;
	unsigned char *buf;

	if (!g_vnic_on || (whose = vnic_whose(frame, len)) == VNIC_OURS)
	{
		return 0;
	}
	/* Before process_pkt(), which builds the loader's answers in place.
	 * PSO: one callback per frame type, ARP or IP, which returns the buffer.
	 * ETD: one callback, which answers with SetRecvBuf(). */
	i = VNIC_CB_RX;
	vnic_rbuf = 0;
	if (vnic_api_pso)
	{
		i = frame[13] != 0x06;
		if (frame[12] == 0x08 && (frame[13] == 0x06 || !frame[13]))
		{
			vnic_rbuf = (unsigned char *)vnic_cb(i, 0, len);
		}
	}
	else
	{
		vnic_cb(i, len, 0);
	}
	buf = vnic_rbuf;
	if (!buf)
	{
		g_vnic_rx_drop++;
	}
	else
	{
		vnic_rbuf = 0;
		memcpy(buf, frame, len);
		/* In RAM too, as the driver's DMA left it: the title may invalidate
		 * the buffer before it hands it to the hardware again. */
		CacheBlockWriteBack((unsigned char *)((unsigned int)buf & ~31U),
				    (((unsigned int)buf & 31U) + len + 31U) / 32U);
		/* "It is there": ETD's (ctx, 0, buf) -- the title looks the frame
		 * up by that address among those it gave out (UT NTSC 0x8c1c9c06;
		 * ETD itself at 0x8c1c5146) --, PSO's (handle, 1, len). */
		vnic_cb(i, vnic_api_pso, vnic_api_pso ? len : (unsigned int)buf);
		g_vnic_rx++;
	}
	return whose == VNIC_TITLE;
}

/*
 * The ETD entry points, through _vnic_etd (vnic_entry.S): the title's
 * arguments, then the function's index in the stub the host wrote over it.
 * Runs on the title's stack, in whatever context it called from: it only
 * records. Nothing here touches the adapter -- the title's own G2 DMA may be
 * running (g2dma_hold(), AGENTS.md 4.16), and the tick does that work.
 * Return values are ETD's.
 */
int vnic_call(unsigned int a, unsigned int b, unsigned int c, unsigned int fn)
{
	unsigned int i;

	/* THE OTHER API (cases 8..12): Phantasy Star Online v2.011's driver
	 * (2026-10-11), the same core as ETD behind a table of seven functions
	 * its network layer fills (dcload-ip-rs src/dispatch/vnic.rs). Its
	 * open/close/info touch no hardware and stay the title's: open keeps one
	 * callback per frame type (ARP, IP) in two words the loader reads
	 * (g_vnic_slot 0 and 1, which are also their own "contexts": the title
	 * passes the word as the handle). Cases share their tails: ETD's and
	 * PSO's calls that end alike fall through to one another. */
	switch (fn)
	{
	case 0:	/* ETD_Init: no callbacks until the title sets them again */
		vnic_api_pso = 0;
		for (i = 0; i < 4; i++)
		{
			if (g_vnic_slot[i])
			{
				*(volatile unsigned int *)g_vnic_slot[i] = 0;
			}
		}
		/* fall through: the callbacks are gone, so this passes */
	case 10:	/* PSO terminate: refused while a frame type is open, as the title's */
		if (vnic_word(0) || vnic_word(1))
		{
			return -2;
		}
		/* fall through */
	case 1:	/* ETD_Stop: ETD switched the bridge off here */
		g_vnic_on = 0;
		vnic_tx_buf = 0;
		return 0;
	case 11:	/* PSO getmac(handle, mac, &len) */
		*(volatile unsigned int *)c = 6;
		a = b;
		/* fall through */
	case 2:	/* ETD_GetEthAddr */
		if (!a)
		{
			return -12;
		}
		memcpy((void *)a, bb->mac, 6);
		return 0;
	case 3:	/* ETD_SetCtlPrm: the loader's filters stay as they are */
		return a ? 0 : -14;
	case 4:	/* the link: up, 100 Mbit full duplex -- the loader needs it anyway */
		if (a)
		{
			*(volatile unsigned int *)a = 4;
		}
		vnic_cb(VNIC_CB_LINK, 4, 0);
		/* fall through */
	case 12:	/* PSO's link: up */
		return 1;
	case 8:	/* PSO init: traffic on */
		vnic_api_pso = 1;
		a = 1;
		/* fall through */
	case 5:	/* ETD_CtlXmit */
		if (a > 1)
		{
			return -24;
		}
		vnic_tx_buf = 0;
		vnic_start = a;
		g_vnic_on = a;
		return 0;
	case 6:	/* ETD_SetRecvBuf: inside the RX callback */
		vnic_rbuf = (unsigned char *)a;
		return 0;
	case 7:	/* ETD_SetSendBuf */
		if (!g_vnic_on || vnic_tx_buf)
		{
			return -6;	/* "send packet busy" */
		}
		if (b - 1U >= 0x700U)
		{
			return -10;
		}
		if (a & 1U)
		{
			return -9;
		}
		if (!a)
		{
			return -11;
		}
		vnic_tx_len = b;
		vnic_tx_buf = a;	/* last: it is what the tick looks at */
		return 0;
	case 9:	/* PSO send(buf, len): synchronous for the title, so copied now */
		if (!g_vnic_on || vnic_tx_buf)
		{
			return -1;	/* its "transmit queue full" */
		}
		if (b - 1U >= RX_PKT_BUF_SIZE)
		{
			return -2;
		}
		memcpy(vnic_txcopy, (void *)a, b);
		vnic_tx_len = b;
		vnic_tx_buf = (unsigned int)vnic_txcopy;
		return 0;
	}
	return -4;
}

/*
 * LATE PATCHES: A PROGRAM THE TITLE DECODES ITSELF (Phantasy Star Online
 * Ver.2, 2026-10-11). Its 1ST_READ.BIN only loads IWASHI.SEA, KATSUO.SEA and
 * DP_ADDRESS.JPN; IWASHI decrypts and decompresses the last into 0x8c010000,
 * and KATSUO enters it through the word at 0x8c01002c. Nothing on the disc
 * holds the driver in clear, so the host decodes it itself, works out the
 * words, and leaves them here; and while serving KATSUO.SEA it points the
 * literal KATSUO jumps through at g_vnic_late_entry. _vnic_late
 * (vnic_entry.S) then writes them and enters the program as KATSUO would.
 * g_vnic_late: [0] how many pairs, [1] the word holding the entry address,
 * then (address, value) pairs. g_vnic_late_done: how many were written.
 */
#define VNIC_LATE_MAX 10U
unsigned int g_vnic_late[2 + 2 * VNIC_LATE_MAX];
unsigned int g_vnic_late_done;
void vnic_late(void);
void (*const g_vnic_late_entry)(void) = vnic_late;

unsigned int vnic_late_apply(void)
{
	unsigned int i;

	for (i = 0; i < g_vnic_late[0] && i < VNIC_LATE_MAX; i++)
	{
		volatile unsigned int *at = (volatile unsigned int *)g_vnic_late[2 + 2 * i];

		*at = g_vnic_late[3 + 2 * i];
		CacheBlockWriteBack((unsigned char *)((unsigned int)at & ~31U), 1);
	}
	g_vnic_late_done = i;
	return *(volatile unsigned int *)g_vnic_late[1];
}

/*
 * THE NETWORK SETTINGS IN FLASH (2026-10-12). A network title does not use the
 * adapter unless the console's flash says how: Unreal Tournament reads the ISP
 * blocks 0x80..0xEB of partition 2 (KOS dc/flashrom.h), and without block
 * 0xE0 -- the adapter's IP settings, written by DreamKey or a web browser --
 * it only knows the modem. The loader adds that block to what the title
 * reads, and writes nothing: the title's flash reads go through the BIOS
 * vector 0x8c0000b8 (_vnic_flash, vnic_entry.S), which calls the BIOS and
 * then lays two pieces over the answer -- the first free block, holding a
 * 0xE0 block with the loader's own address (static IP), and the bitmap byte
 * that says it is in use. The block library reads the newest block of an ID
 * first, so this one wins over any older 0xE0. A write or a delete by the
 * title is answered as done and goes nowhere (vnic_entry.S): its flash
 * library works from its own copy in RAM and checks only the return value,
 * so a save holds for the session and the console's flash never changes. A
 * later re-read of the flash in the same session sees what it held before.
 * g_vnic_isp, from the host before EXEC: address, netmask, gateway, two DNS
 * servers, each as the number a.b.c.d (like our_ip); 0 = no overlay.
 *
 * Unreal Tournament NTSC keeps its own copy of the ISP settings (2026-10-12):
 * blocks 0x60..0x69, ten times 60 bytes laid end to end in the layout of the
 * 0xE0..0xE9 blocks (address at +8, netmask +12, DNS +20 and +24, gateway
 * +28, PPP login +0x190 and password +0x1b0), read with the Katana flash
 * library at boot (UTDC.BIN 0x8c08d960) and written back when its LAN screen
 * changes them. Missing, they read as 0xFF and it clears them: empty fields.
 * So 0x60 is laid too, with 0xE0's data, and 0x61..0x69 as zeros (empty
 * strings, not 0xFF ones). Eleven blocks in the first free ones, in order,
 * their bitmap bits cleared; each is made on the fly from its ID, 0xE0's
 * data or zeros, and its CRC, the CRCs computed at EXEC.
 *
 * ONE BLOCK IS KEPT (2026-10-14): g_vnic_flash_keep, from the host, names a
 * block ID whose writes reach the flash -- PSO Ver.2's serial number and
 * access key (block 7), typed again every session otherwise. The title's
 * library writes its bitmap first, then the block in the slot after the last
 * one it sees, which is after our overlay: neither is free in the flash. So
 * the bitmap write still goes nowhere, and the block goes to the first free
 * slot of the real flash, the overlay's first (hidden by the overlay for the
 * rest of the session), then its bit cleared (its bitmap byte read back, less
 * that bit: a flash write may only clear bits). The used
 * blocks stay a run from the start, the next session's overlay goes after it,
 * and the library finds the block there. At most VF_BLOCKS a session, the
 * slots arming found free.
 */
#define FLASH_VEC     (*(volatile unsigned int *)0xac0000b8U)
#define FLASH_PART    2U	/* "block 1": system and ISP settings, 16 KB */
#define FLASH_ISP_IP  0xe0U
#define FLASH_STATIC  3U	/* the 0xE0 block's method byte: 0 modem, 1 DHCP, 2 PPPoE, 3 static (KOS) */
#define FLASH_UT_ISP  0x60U	/* Unreal Tournament NTSC's copy, 0x60..0x69 */
#define VF_BLOCKS     11U	/* 0xE0, then 0x60..0x69 */

unsigned int g_vnic_isp[5];
unsigned int vnic_flash_saved;		/* the BIOS's vector; 0 = no overlay */
unsigned int g_vnic_flash_keep;		/* host: a block ID written to the flash, 0 none */
unsigned int g_vnic_flash_kept;		/* blocks of that ID written this session */
static unsigned int vf_bm_at;		/* flash offset of the bitmap */
static unsigned int vf_bit;		/* its bit for our first block */
static unsigned int vf_blk_at;		/* flash offset of our first block */
static unsigned int vf_data_at;		/* flash offset of the partition's first block */
static unsigned char vf_blk[48] __attribute__((aligned(4)));	/* 0xE0's first 48 bytes, the rest 0 */
static unsigned short vf_crc[VF_BLOCKS];
void vnic_flash(void);

typedef int (*flash_fn)(unsigned int, void *, unsigned int, unsigned int);

/* Byte j of our blocks laid end to end: block j / 64's ID, then 0xE0's data
 * for 0xE0 and 0x60, zeros for 0x61..0x69, then its CRC. */
static unsigned int vf_byte(unsigned int j)
{
	unsigned int n = j >> 6, i = j & 63U;

	if (i == 0)
	{
		return n ? FLASH_UT_ISP - 1U + n : FLASH_ISP_IP;
	}
	if (i >= 62U)
	{
		return vf_crc[n] >> ((i - 62U) * 8U) & 0xffU;
	}
	return n < 2U && i < sizeof(vf_blk) ? vf_blk[i] : 0;
}

/* At EXEC, while the BIOS vector is still the BIOS's: find the first free
 * block, build ours, take the vector. */
void vnic_flash_arm(void)
{
	flash_fn bios = (flash_fn)FLASH_VEC;
	unsigned int part[2], bm[16], bmb, i, bit, crc, v, n;
	unsigned char *b = (unsigned char *)bm;

	if (!g_vnic_isp[0] || vnic_flash_saved || bios(FLASH_PART, part, 0, 0))
	{
		return;
	}
	/* One bit a 64-byte block, rounded up to 512 bits, at the partition's end
	 * (over 64 bytes, over 32 KB: not partition 2 as we know it). */
	bmb = (((part[1] >> 6) + 511U) & ~511U) >> 3;
	if (bmb > sizeof(bm) || bios(part[0] + part[1] - bmb, bm, bmb, 1))
	{
		return;
	}
	/* Bit n is physical block n + 1; the bitmap's own blocks are not free.
	 * Ours are the first VF_BLOCKS free ones, which must follow each other:
	 * past the used ones, a run of free ones, broken by nothing. */
	v = ((part[1] - bmb) >> 6) - 1U;	/* bits of data blocks */
	for (bit = n = 0; n < VF_BLOCKS; bit++)
	{
		if (bit >= v)
		{
			return;
		}
		if (b[bit >> 3] & (0x80U >> (bit & 7U)))
		{
			n++;
		}
		else if (n)
		{
			return;
		}
	}
	bit -= VF_BLOCKS;
	vf_bm_at = part[0] + part[1] - bmb;
	vf_bit = bit;
	vf_data_at = part[0] + 64U;
	vf_blk_at = vf_data_at + bit * 64U;

	/* 0xE0: "SEGA", the method, then six words from +10, big-endian: address,
	 * netmask, broadcast, two DNS, gateway; then the PPP login. */
	memset(vf_blk, 0, sizeof(vf_blk));
	memcpy(vf_blk + 2, "SEGA", 4);
	vf_blk[6] = FLASH_STATIC;
	for (n = 0; n < 6U; n++)
	{
		v = n == 2U ? g_vnic_isp[0] | ~g_vnic_isp[1] : g_vnic_isp[(unsigned char)"\0\1\0\3\4\2"[n]];
		for (i = 0; i < 4U; i++)
		{
			vf_blk[10U + 4U * n + i] = v >> (24U - 8U * i);
		}
	}
	memcpy(vf_blk + 38, "dreamcast", 9);
	/* CRC-16/CCITT over each block's first 62 bytes, inverted (KOS
	 * flashrom_calc_crc). */
	for (crc = 0xffffU, n = 0; n < VF_BLOCKS * 64U; n++)
	{
		i = n & 63U;
		if (i == 62U)
		{
			vf_crc[n >> 6] = ~crc;
			crc = 0xffffU;
			n++;
			continue;
		}
		crc ^= vf_byte(n) << 8;
		for (v = 0; v < 8U; v++)
		{
			crc = (crc & 0x8000U) ? (crc << 1) ^ 0x1021U : crc << 1;
		}
	}

	vnic_flash_saved = (unsigned int)bios;
	FLASH_VEC = (unsigned int)vnic_flash;
}

/* A read or a write of the title's, [off, off + len) (vnic_entry.S). A read
 * goes to the BIOS and gets the overlay laid over it. A write is answered as
 * done; only the kept block reaches the flash ("ONE BLOCK IS KEPT"). */
unsigned int vnic_flash_rw(unsigned int off, unsigned char *buf, unsigned int len, unsigned int op)
{
	flash_fn bios = (flash_fn)vnic_flash_saved;
	unsigned int n, i, at, r;
	unsigned char bm;

	if (op == 1U)
	{
		r = bios(off, buf, len, 1);
		for (n = 0; n < VF_BLOCKS * 64U; n++)
		{
			if (vf_blk_at + n - off < len)
			{
				buf[vf_blk_at + n - off] = vf_byte(n);
			}
			i = vf_bit + (n >> 6);
			at = vf_bm_at + (i >> 3);
			if (at - off < len)
			{
				buf[at - off] &= ~(0x80U >> (i & 7U));
			}
		}
		return r;
	}
	n = g_vnic_flash_kept;
	at = vf_bit + n;
	if (len == 64U && n < VF_BLOCKS && off - vf_data_at < vf_bm_at - vf_data_at
	    && g_vnic_flash_keep && buf[0] == g_vnic_flash_keep && !buf[1]
	    && bios(vf_blk_at + n * 64U, buf, 64, 2) == 64)
	{
		/* As the flash has it, less this bit: a write may only clear bits. */
		i = vf_bm_at + (at >> 3);
		bios(i, &bm, 1, 1);
		bm &= ~(0x80U >> (at & 7U));
		bios(i, &bm, 1, 2);
		g_vnic_flash_kept = n + 1U;
	}
	return len;
}

static void vnic_send(void)
{
	unsigned int len = vnic_tx_len;

	if (len > RX_PKT_BUF_SIZE)
	{
		g_vnic_tx_fail++;
		return;
	}
	/* pkt_buf: bb->tx() wants the loader's alignment and pads in place. */
	memcpy(pkt_buf, (void *)vnic_tx_buf, len);
	if (bb->tx(pkt_buf, (int)len))
	{
		g_vnic_tx++;
	}
	else
	{
		g_vnic_tx_fail++;
	}
}

int vnic_tick(unsigned int t0)
{
	static unsigned int mark, rx_seen;
	extern volatile unsigned int g_irq_rx;
	unsigned int n, buf;

	if (!g_vnic_on)
	{
		return 0;
	}
	if (vnic_start)
	{
		vnic_start = 0;
		bb->start();
		irq_rx_arm(0);	/* = armed while g_vnic_on (irq.c) */
	}
	if (vnic_tx_buf)
	{
		for (n = 0; n < VNIC_TX_PER_TICK && (buf = vnic_tx_buf); n++)
		{
			gd_exchange(vnic_send);
			vnic_tx_buf = 0;
			/* ETD: the buffer is the title's again; its next frame, if it
			 * has one, comes back through SetSendBuf from inside the
			 * callback. PSO's send() has no such callback. */
			if (!vnic_api_pso)
			{
				vnic_cb(VNIC_CB_TX, buf, 0);
			}
		}
		return 1;
	}
	/* A read on the wire drains the ring itself (ga_poll()). */
	if (!gd_async_busy() && (g_irq_rx != rx_seen || ((t0 - mark) >> 4) >= VNIC_POLL_TICKS))
	{
		rx_seen = g_irq_rx;
		mark = t0;
		return gd_vnic_drain();
	}
	return 0;
}

#endif /* WITH_VNIC */
