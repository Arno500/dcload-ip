/*
 * RetroAchievements sampler (AGENTS.md 4.20).
 *
 * The host sends a table of RAM ranges (RACT). At the start of each vertical
 * blank the interrupt tick reads them, compares with the previous frame and
 * appends what changed to a batch; the batch goes out as RAFR datagrams,
 * unacknowledged (a lost one costs a keyframe, not a hang), from the tick when
 * the GD lock is free. Every condition is evaluated on the host.
 *
 * READ IN P1: the title's own cache holds its dirty lines; a P2 read would see
 * stale RAM. P1 never takes a TLB miss, which the tick (SR.BL = 1) may not.
 *
 * MEMORY. The previous frame (RA_WORDS words), the overlay hits, a window of
 * scratch and the batch buffer live in dead RAM (gd_ra_scratch): the big GD
 * stage under a Katana title, the loader's own dead stack under Windows CE, whose
 * title reads into the stage. That is also why nothing here may touch them before
 * the title runs: until EXEC the loader's own stack overlaps that range.
 * cmd_ract() only writes the table; ra_attach() lays the memory out on the first
 * tick, and again if the kind of title changes (the MMU came on).
 *
 * WINDOWS CE AND THE CACHE. CE runs with the MMU on and copy-back pages, so a word
 * the title wrote may sit in a DIRTY LINE of the operand cache that RAM does not
 * have yet -- and, the cache being indexed by virtual address, in a line that a
 * read through the physical (P1) address never finds (docs/wince-investigation.md
 * 6: one buffer was VPN 0x080df000, PPN 0x0ce98000). So under the MMU every word is
 * read from RAM through P2 (uncached, whatever the cache holds) and, if a dirty
 * line of the operand cache holds it, from that line (ra_oc_word()): the tag is
 * PHYSICAL, so this holds for any alias. A line can only be in one of the 4 entries
 * its address bits 11:5 allow (4 KB pages), and consecutive words share a line, so the lookup is
 * made once per line (the last one is remembered). Needs the cache on and not in
 * RAM mode (CCR.ORA); otherwise g_ra_oc_skip counts the frames sampled from RAM
 * alone. (An earlier version scanned the whole 512-entry array into a list of hits:
 * faster on paper, and it cost 1.3 KB of Windows CE's scarce room.)
 *
 * RAFR payload (after the 12-byte command: address = sequence, size = bytes):
 *   word  table generation the records were sampled with
 *   word  number of records
 *   per record (a frame is one or more, in window order):
 *     word  frame << 16 | 0x8000 key | 0x4000 last | window << 9 | changed words
 *     RA_WIN_MAP words  bitmap of the window (bit i = word window*RA_WIN + i)
 *     one word per set bit
 * A keyframe has every bit of every window set. A frame is complete at its
 * `last` record; one with no change is a single empty record of window 0.
 * Records are cut into datagrams at record boundaries, so a frame may span two.
 *
 * Nothing here waits: a batch that cannot go (GD lock held for a load) is
 * dropped when full, counted, and the next frame is a keyframe.
 */

#include <string.h>
#include "ra.h"

#if WITH_RCHEEVOS
#include "adapter.h"
#include "cdfs.h"
#include "memfuncs.h"
#include "net.h"
#include "syscalls.h"
#include "g2dma.h"

#define RA_PERIOD      100000U	/* 8 ms, Pck/4 ticks: one sample per frame */
#define RA_BATCH       6U	/* frames per datagram, about */
#define RA_MAX_AGE     625000U	/* 50 ms: send a short batch after this */
#define RA_KEY_PERIOD  (2U * 60U)	/* a keyframe every ~2 s */
#define RA_PAYLOAD     1400U
#define RA_PER_TICK    2U	/* datagrams per tick, at most */
#define RA_VBLANK_IN   0x08U	/* SB_ISTNRM bit 3 */

#define RA_WINDOWS       ((RA_WORDS + RA_WIN - 1U) / RA_WIN)
#define RA_REC_MAX       (1U + RA_WIN_MAP + RA_WIN)	/* words of a full record */

/* The hardware accesses, which a host test (scripts/ra-sampler-harness)
 * replaces: a word of RAM `off` bytes from its start (through P1, or P2 under the
 * MMU), the operand cache's address and data arrays, CCR. */
#ifndef RA_PEEK
#define RA_PEEK(off, ce) \
	(*(const volatile unsigned int *)(((ce) ? 0xac000000U : 0x8c000000U) + (off)))
#define RA_OC_ADDR(e)    (*(volatile unsigned int *)(0xf4000000U + ((e) << 5)))
#define RA_OC_DATA(e, w) (*(volatile unsigned int *)(0xf5000000U + ((e) << 5) + ((w) << 2)))
#define RA_CCR()         (*(volatile unsigned int *)0xff00001cU)
#endif

#define RA_CHUNK       16U	/* words read at a time: the scratch window */
#define RA_CCR_OCE     0x1U
#define RA_CCR_ORA     0x20U

#define RA_HDR_KEY     0x8000U
#define RA_HDR_LAST    0x4000U

static unsigned int ra_tab[RA_RANGES];	/* (offset >> 2) << 10 | words */
static unsigned int ra_chain[RA_CHAINS];	/* parent << 22 | shift << 21 | offset in words */
static unsigned int ra_direct;		/* words of the ranges; the chains' come after */
static unsigned int ra_oc_ready;	/* the operand cache can be read this frame */
static unsigned int ra_words;		/* all the table's words: the ranges', then the chains' */
static volatile unsigned int ra_on;	/* the table is whole */
static unsigned int ra_gen;
static unsigned int ra_need_key = 1;
static unsigned int ra_since_key;
static unsigned int ra_frame;
static unsigned int ra_last_t;
static unsigned int ra_seq;

/* Where the memory is, once attached (ra_attach). */
static unsigned int *ra_base;		/* the scratch it was laid out in; 0 = not yet */
static int ra_ce;			/* ... and whether that was under the MMU */
static unsigned int *ra_prev, *ra_tmp, *ra_buf;
static unsigned int ra_rd, ra_used;	/* batch: words read out / written */
static unsigned int ra_cap;		/* words of batch buffer */
static unsigned int ra_pending;		/* frames in the batch not yet sent */
static unsigned int ra_more;		/* records left over after a send */
static unsigned int ra_first_t;		/* clock when the oldest unsent frame was sampled */

unsigned int g_ra_frames;	/* frames sampled */
unsigned int g_ra_sent;		/* datagrams sent */
unsigned int g_ra_dropped;	/* frames dropped: batch full, network busy */
unsigned int g_ra_keys;		/* keyframes */
unsigned int g_ra_tick_max;	/* longest ra_tick(), Pck/4 ticks */
unsigned int g_ra_gen;		/* table generation in use */
unsigned int g_ra_nofit;	/* the scratch cannot hold this table (Windows CE's is smaller) */
unsigned int g_ra_oc_words;	/* words taken from a dirty cache line instead of RAM */
unsigned int g_ra_oc_skip;	/* frames sampled under the MMU with the cache unreadable */

/* Words of a keyframe over `words` table words. */
static unsigned int ra_worst(unsigned int words)
{
	unsigned int rem = words % RA_WIN;

	return (words / RA_WIN) * RA_REC_MAX + (rem ? 1U + RA_WIN_MAP + rem : 0U);
}

/* Words of scratch the layout needs besides the batch buffer. */
#define RA_FIXED(words) ((words) + RA_CHUNK)

/* The most words a table may have for `scratch` words of room. */
static unsigned int ra_max_words(unsigned int scratch)
{
	unsigned int w;

	for (w = RA_WORDS; w > RA_WIN_MAP * 32U; w -= 8U)
	{
		if (scratch >= RA_FIXED(w) + ra_worst(w))
		{
			return w;
		}
	}
	return 0;
}

/* Lay the memory out in whatever the scratch is now; false if it cannot hold
 * the table. Run on every tick: a different base means the title's MMU came on
 * (or it went away), and what was in the old one is gone. */
static int ra_attach(void)
{
	unsigned int scratch;
	int ce;
	unsigned int *base = gd_ra_scratch(&scratch, &ce);

	if (base == ra_base && ce == ra_ce)
	{
		return ra_on;
	}
	ra_base = base;
	ra_ce = ce;
	if (scratch < RA_FIXED(ra_words) + ra_worst(ra_words))
	{
		ra_on = 0;	/* a table too big for Windows CE's smaller room */
		g_ra_nofit++;
		return 0;
	}
	ra_prev = base;
	ra_tmp = ra_prev + ra_words;
	ra_buf = ra_tmp + RA_CHUNK;
	ra_cap = scratch - RA_FIXED(ra_words);
	ra_rd = ra_used = ra_pending = ra_more = 0;
	ra_need_key = 1;
	return 1;
}

/* The operand cache, under the MMU only. The last line looked for is remembered:
 * a range's words are consecutive, eight to a line. */
static unsigned int ra_oc_line, ra_oc_have;
static int ra_oc_entry;

/* Start of a frame sampled under the MMU: can the cache be read at all? */
static void ra_oc_begin(void)
{
	unsigned int ccr = RA_CCR();

	ra_oc_ready = 0;
	ra_oc_have = 0;
	if (!(ccr & RA_CCR_OCE))
	{
		return;		/* no cache, so RAM is the truth */
	}
	if (ccr & RA_CCR_ORA)
	{
		g_ra_oc_skip++;	/* half the array is RAM, not lines */
		return;
	}
	ra_oc_ready = 1;
}

/* The entry holding the dirty line at RAM offset `line` (32-byte aligned), or -1.
 * The entry is bits 13:5 of the VIRTUAL address; CE maps 4 KB pages
 * (docs/wince-investigation.md 9i), whose bits 11:5 are the physical address's, so
 * a line can only be in one of the 4 entries bits 13:12 choose. Looking in the 16
 * that 1 KB pages would allow kept the tick near 2 ms with interrupts blocked at
 * every vblank, and Sega Rally 2 stopped for good (2026-10-09): every address
 * array read is slow. */
static int ra_oc_find(unsigned int line)
{
	unsigned int hi;

	for (hi = 0; hi < 4U; hi++)
	{
		unsigned int e = (hi << 7) | ((line >> 5) & 127U);
		unsigned int a = RA_OC_ADDR(e);

		/* valid and dirty, and the tag is this address's (RAM mirrors 0x0c..0x0f) */
		if ((a & 3U) == 3U && !((a ^ (0x0c000000U | line)) & 0x1cfffc00U))
		{
			return (int)e;
		}
	}
	return -1;
}

/* The word at RAM offset `ro` as the title sees it, `v` being what RAM has: that of
 * a dirty line of the operand cache if one holds it. */
static unsigned int ra_oc_word(unsigned int ro, unsigned int v)
{
	unsigned int line = ro & ~31U;

	if (!ra_oc_ready)
	{
		return v;
	}
	if (!ra_oc_have || line != ra_oc_line)
	{
		ra_oc_entry = ra_oc_find(line);
		ra_oc_line = line;
		ra_oc_have = 1;
	}
	if (ra_oc_entry < 0)
	{
		return v;
	}
	g_ra_oc_words++;
	return RA_OC_DATA((unsigned int)ra_oc_entry, (ro >> 2) & 7U);
}

/* The table being received: a RACT comes in parts, in order (ra.h). */
static unsigned int ra_in_gen;		/* generation being received; 0 = none */
static unsigned int ra_in_n, ra_in_nc;	/* its ranges and chains, in all */
static unsigned int ra_in_r, ra_in_c;	/* ... received so far */
static unsigned int ra_in_end;		/* end of the last range received */
static unsigned int ra_in_total;	/* words of the ranges received */

static void ra_reply(ip_header_t * ip, udp_header_t * udp, command_t * command, unsigned int rv)
{
	command_t * response = (command_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
	unsigned int *lim = (unsigned int *)response->data;

	memcpy(response, command, COMMAND_LEN);
	response->size = htonl(rv);
	/* What the host may ask for: the largest table under a Katana title, and
	 * under Windows CE (the block the host lent, or the loader's dead stack). */
	lim[0] = htonl(ra_max_words(gd_ra_katana_words()));
	lim[1] = htonl(ra_max_words(gd_ra_ce_words()));
	make_ip(ntohl(ip->src), ntohl(ip->dest), UDP_H_LEN + COMMAND_LEN + 8U, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
	make_udp(ntohs(udp->src), ntohs(udp->dest), COMMAND_LEN + 8U, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
	bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN + 8U);
}

void cmd_ract(ip_header_t * ip, udp_header_t * udp, command_t * command)
{
	const unsigned int *d = (const unsigned int *)command->data;
	unsigned int size = ntohl(command->size);
	unsigned int gen = ntohl(command->address);
	unsigned int n, nc, r0, rc, c0, cc, i, room;

	if (size < 24U)
	{
		ra_in_gen = 0;
		ra_reply(ip, udp, command, 0xffffffffU);
		return;
	}
	n = ntohl(d[0]);
	nc = ntohl(d[1]);
	r0 = ntohl(d[2]);
	rc = ntohl(d[3]);
	c0 = ntohl(d[4]);
	cc = ntohl(d[5]);
	d += 6;

	if (r0 == 0U && c0 == 0U)
	{
		/* The first part: whatever was being received is forgotten, and the
		 * table in use stops here (a tick may land anywhere below: it sees it
		 * off). */
		ra_on = 0;
		ra_in_gen = gen;
		ra_in_n = n;
		ra_in_nc = nc;
		ra_in_r = ra_in_c = 0;
		ra_in_end = ra_in_total = 0;
	}
	else if (gen != ra_in_gen || n != ra_in_n || nc != ra_in_nc
		 || r0 != ra_in_r || c0 != ra_in_c)
	{
		/* A part after a lost one, or of a table given up: nothing is said, the
		 * host sends the whole table again until it is answered. */
		return;
	}
	if (gen == 0U || n > RA_RANGES || nc > RA_CHAINS || rc > n - r0 || cc > nc - c0
	    || (cc != 0U && r0 + rc != n) || size < 24U + 8U * rc + 4U * cc)
	{
		goto refuse;
	}
	for (i = 0; i < rc; i++)
	{
		unsigned int off = ntohl(d[2 * i]);
		unsigned int len = ntohl(d[2 * i + 1]);

		/* Ascending and apart, as the host builds them; a length fits 10 bits. */
		if (len == 0U || len > 1023U || (off & 3U) || off < ra_in_end
		    || off >= 0x01000000U || off + 4U * len > 0x01000000U
		    || ra_in_total + len > RA_WORDS)
		{
			goto refuse;
		}
		ra_tab[r0 + i] = (off >> 2) << 10 | len;
		ra_in_end = off + 4U * len;
		ra_in_total += len;
	}
	ra_in_r += rc;
	d += 2U * rc;
	for (i = 0; i < cc; i++)
	{
		unsigned int spec = ntohl(d[i]);

		/* a parent is an earlier word, so one pass resolves a chain */
		if ((spec >> 22) >= ra_in_total + c0 + i)
		{
			goto refuse;
		}
		ra_chain[c0 + i] = spec;
	}
	ra_in_c += cc;
	if (ra_in_r != n || ra_in_c != nc)
	{
		return;		/* more parts to come; only the last is answered */
	}

	/* Whole. Refused if neither kind of title has room for it; under the one
	 * that runs, ra_attach() checks again (g_ra_nofit), and the host, which
	 * knows which it is, checks the reply's limits. */
	room = gd_ra_katana_words();
	if (gd_ra_ce_words() > room)
	{
		room = gd_ra_ce_words();
	}
	if (ra_in_total + nc > RA_WORDS || ra_in_total + nc > ra_max_words(room))
	{
		goto refuse;
	}
	ra_direct = ra_in_total;
	ra_words = ra_in_total + nc;
	ra_gen = gen;
	g_ra_gen = gen;
	ra_base = (unsigned int *)0;	/* ra_attach() lays the memory out again */
	ra_need_key = 1;
	ra_in_gen = 0;
	ra_on = (n != 0U);
	ra_reply(ip, udp, command, 0);
	return;

refuse:
	ra_in_gen = 0;		/* ra_on stays 0 if a first part had turned it off */
	ra_reply(ip, udp, command, 0xffffffffU);
}

/* Sample one frame; `key` = every word goes. Returns 0 if it did not fit. */
static int ra_sample(unsigned int key, unsigned int t0)
{
	unsigned int *prev = ra_prev;
	unsigned int *buf = ra_buf;
	unsigned int start = ra_used;
	unsigned int *last_hdr = (unsigned int *)0;
	unsigned int r = 0, j = 0;	/* cursor over the ranges: windows are consecutive */
	unsigned int w, i, k, c;

	if (ra_ce)
	{
		ra_oc_begin();
	}
	for (w = 0; w < RA_WINDOWS && w * RA_WIN < ra_words; w++)
	{
		unsigned int base = w * RA_WIN;
		unsigned int cnt = ra_words - base;
		unsigned int *rec = &buf[ra_used];
		unsigned int *map = rec + 1;
		unsigned int *vals = rec + 1 + RA_WIN_MAP;
		unsigned int n = 0;

		if (cnt > RA_WIN)
		{
			cnt = RA_WIN;
		}
		/* Only a keyframe had its whole size reserved (ra_tick): a frame of
		 * changes that outgrows the room is given up, and `prev`, already
		 * partly updated, is put right by the keyframe that follows. */
		if (ra_used + 1U + RA_WIN_MAP + cnt > ra_cap)
		{
			ra_used = start;
			return 0;
		}
		for (i = 0; i < RA_WIN_MAP; i++)
		{
			map[i] = 0;
		}
		/* RA_CHUNK words at a time: RAM, then the dirty cache lines over them,
		 * then what changed since the last frame. */
		for (c = 0; c < cnt; c += RA_CHUNK)
		{
			unsigned int len = cnt - c;

			if (len > RA_CHUNK)
			{
				len = RA_CHUNK;
			}
			unsigned int gs = base + c;		/* table index of ra_tmp[0] */
			unsigned int nd = (ra_direct > gs) ? ra_direct - gs : 0U;

			if (nd > len)
			{
				nd = len;
			}
			for (k = 0; k < nd; k++)
			{
				unsigned int spec = ra_tab[r];
				unsigned int ro = ((spec >> 10) << 2) + 4U * j;

				ra_tmp[k] = ra_oc_word(ro, RA_PEEK(ro, ra_ce));
				if (++j >= (spec & 0x3ffU))
				{
					r++;
					j = 0;
				}
			}
			/* The chains: a word the pointer it has just been sampled in leads
			 * to. Its parent is an earlier word, in this chunk or already in
			 * `prev`, where a word that did not change is what it was. */
			for (; k < len; k++)
			{
				unsigned int spec = ra_chain[gs + k - ra_direct];
				unsigned int pi = spec >> 22;
				unsigned int pv = (pi >= gs) ? ra_tmp[pi - gs] : prev[pi];
				unsigned int ro;

				pv = (pv >> ((spec >> 21 & 1U) ? 8U : 0U)) & 0x00ffffffU;
				ro = (pv + (((unsigned int)((int)(spec << 11) >> 11)) << 2)) & 0x00fffffcU;
				ra_tmp[k] = ra_oc_word(ro, RA_PEEK(ro, ra_ce));
			}
			for (k = 0; k < len; k++)
			{
				unsigned int cur = ra_tmp[k];

				if (key || cur != prev[base + c + k])
				{
					map[(c + k) >> 5] |= 1U << ((c + k) & 31U);
					vals[n++] = htonl(cur);
					prev[base + c + k] = cur;
				}
			}
		}
		if (n != 0U)
		{
			for (i = 0; i < RA_WIN_MAP; i++)
			{
				map[i] = htonl(map[i]);
			}
			rec[0] = htonl((ra_frame & 0xffffU) << 16 | (key ? RA_HDR_KEY : 0U)
				       | w << 9 | n);
			last_hdr = rec;
			ra_used += 1U + RA_WIN_MAP + n;
		}
	}
	if (last_hdr == (unsigned int *)0)
	{
		/* Nothing changed: the frame still counts (hit counts run on frames). */
		unsigned int *rec = &buf[ra_used];

		rec[0] = htonl((ra_frame & 0xffffU) << 16);
		for (i = 0; i < RA_WIN_MAP; i++)
		{
			rec[1 + i] = 0;
		}
		last_hdr = rec;
		ra_used += 1U + RA_WIN_MAP;
	}
	*last_hdr = htonl(ntohl(*last_hdr) | RA_HDR_LAST);
	if (ra_used == start)
	{
		return 0;
	}
	if (ra_pending == 0U)
	{
		ra_first_t = t0;
	}
	ra_pending++;
	return 1;
}

void ra_tick(unsigned int t0)
{
	unsigned int key, dt, worst;

	if (!ra_on || !(SB_ISTNRM & RA_VBLANK_IN)
	    || ((t0 - ra_last_t) >> 4) < RA_PERIOD)
	{
		return;
	}
	if (!ra_attach())
	{
		return;
	}
	ra_last_t = t0;
	ra_frame++;
	g_ra_frames++;

	worst = ra_worst(ra_words);
	if (ra_used + worst > ra_cap && ra_rd != 0U)
	{
		/* Make room: the unsent records move down. */
		unsigned int left = ra_used - ra_rd;

		memmove(ra_buf, ra_buf + ra_rd, left * 4U);
		ra_used = left;
		ra_rd = 0;
	}

	/* A keyframe needs its whole size; a frame of changes is checked window by
	 * window (ra_sample). Reserving a keyframe for every frame starved a table
	 * near the room's size: a keyframe still being sent left no room for the next
	 * frame, which was dropped, and asked for another keyframe. */
	key = ra_need_key || ra_since_key + 1U >= RA_KEY_PERIOD;
	if ((key && ra_used + worst > ra_cap) || !ra_sample(key, t0))
	{
		/* Full, and the network is not ours to use here: this frame is lost. */
		g_ra_dropped++;
		ra_need_key = 1;
		return;
	}
	if (key)
	{
		g_ra_keys++;
		ra_need_key = 0;
		ra_since_key = 0;
	}
	else
	{
		ra_since_key++;
	}

	dt = clk_since(t0);
	if (dt > g_ra_tick_max)
	{
		g_ra_tick_max = dt;
	}
}

/* Is a datagram worth sending now? */
int ra_flush_due(unsigned int t0)
{
	if (ra_used == ra_rd)
	{
		return 0;
	}
	return ra_more
	       || ra_pending >= RA_BATCH
	       || ((t0 - ra_first_t) >> 4) >= RA_MAX_AGE
	       /* a keyframe is wanted and would not fit; or, for a table near the
	        * room's size, which never leaves room for one, half the room used */
	       || (ra_used + ra_worst(ra_words) > ra_cap
		   && (ra_need_key || ra_used > ra_cap / 2U));
}

/* One datagram of whole records from the batch. */
static void ra_send_one(void)
{
	command_t *command =
		(command_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
	ip_header_t *ip = (ip_header_t *)(pkt_buf + ETHER_H_LEN);
	udp_header_t *udp = (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN);
	unsigned int *buf = ra_buf;
	unsigned int *out = (unsigned int *)command->data;
	unsigned int from = ra_rd, words = 0, nrec = 0, bytes, len;

	while (ra_rd < ra_used)
	{
		unsigned int rec = 1U + RA_WIN_MAP + (ntohl(buf[ra_rd]) & 0x1ffU);

		if (nrec != 0U && (2U + words + rec) * 4U > RA_PAYLOAD)
		{
			break;
		}
		words += rec;
		ra_rd += rec;
		nrec++;
	}
	out[0] = htonl(ra_gen);
	out[1] = htonl(nrec);
	memcpy(out + 2, buf + from, words * 4U);
	bytes = (2U + words) * 4U;
	len = COMMAND_LEN + bytes;

	memcpy(command->id, CMD_RAFR, 4);
	command->address = htonl(ra_seq++);
	command->size = htonl(bytes);
	make_ether(tool_mac, bb->mac, (ether_header_t *)pkt_buf);
	make_ip(tool_ip, our_ip, UDP_H_LEN + len, IP_UDP_PROTOCOL, ip, 0);
	make_udp(tool_port, dcload_syscall_port, len, ip, udp);
	bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + len);
	g_ra_sent++;
}

static void ra_send(void)
{
	unsigned int i;

	for (i = 0; i < RA_PER_TICK && ra_rd < ra_used; i++)
	{
		ra_send_one();
	}
	ra_pending = 0;
	if (ra_rd >= ra_used)
	{
		ra_rd = ra_used = 0;
		ra_more = 0;
	}
	else
	{
		ra_more = 1;	/* the rest goes on the next tick */
	}
}

void ra_flush(void)
{
	gd_exchange(ra_send);
}
#endif /* WITH_RCHEEVOS */
