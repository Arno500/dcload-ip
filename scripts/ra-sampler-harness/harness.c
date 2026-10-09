/*
 * ra.c, the loader's own source, run on the PC: cmd_ract() takes a table, then
 * ra_tick()/ra_flush() sample a scripted machine and send RAFR datagrams, which
 * are printed so a test on the host side can decode them and compare, frame by
 * frame, with what the title saw at that frame.
 *
 *   harness [ce]        "ce": the title runs under the MMU, with a write-back
 *                       operand cache whose dirty lines RAM does not have yet
 *                       and which are indexed by VIRTUAL address (synonyms).
 *
 *   R <size>|-                                the answer to a RACT part ("-": none)
 *   T <words>                                 the table's size (ranges' words + chains')
 *   D <hex of the datagram: command id, address, size, payload>
 *   E <frame> <hex of every table word, in table order>   (what the title saw)
 *   S <counters>
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern unsigned int g_ram[];			/* RAM as the bus has it */
extern unsigned int g_oc_addr[512], g_oc_data[512 * 8], g_ccr;
#define RA_PEEK(off, ce) (g_ram[(off) >> 2])
#define RA_OC_ADDR(e)    (g_oc_addr[(e)])
#define RA_OC_DATA(e, w) (g_oc_data[(e) * 8U + (w)])
#define RA_CCR()         (g_ccr)

#include "ra.c"

/* All of RAM, 16 MB each: chain targets can be anywhere. */
unsigned int g_ram[0x400000], g_true[0x400000];
unsigned int g_oc_addr[512], g_oc_data[512 * 8], g_ccr = 1;
static int mode_ce;
volatile unsigned int g_istnrm;
volatile unsigned int our_ip;
unsigned short dcload_syscall_port, tool_port;
unsigned int tool_ip;
unsigned char tool_mac[6];
static unsigned char raw[4096] __attribute__((aligned(32)));
unsigned char *pkt_buf = raw + 2;	/* the loader's own: the IP header lands aligned */
static unsigned int scratch[1536];

#define CE_WORDS 688	/* what the loader has under _stack in a Windows CE title (2752 B) */
unsigned int gd_ra_katana_words(void) { return 1536; }
unsigned int gd_ra_ce_words(void) { return CE_WORDS; }
unsigned int *gd_ra_scratch(unsigned int *words, int *ce)
{
	*ce = mode_ce;
	*words = mode_ce ? CE_WORDS : 1536;
	return scratch;
}
void gd_exchange(void (*fn)(void)) { fn(); }
void make_ether(unsigned char *d, unsigned char *s, ether_header_t *e) { (void)d; (void)s; (void)e; }
void make_ip(int d, int s, int l, char p, ip_header_t *ip, unsigned short id) { (void)d; (void)s; (void)l; (void)p; (void)ip; (void)id; }
void make_udp(unsigned short d, unsigned short s, int l, ip_header_t *ip, udp_header_t *u) { (void)d; (void)s; (void)l; (void)ip; (void)u; }

static int tx(unsigned char *pkt, int len)
{
	int i, off = ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;

	printf("D ");
	for (i = off; i < len; i++)
		printf("%02x", pkt[i]);
	printf("\n");
	return 0;
}
static adapter_t adapter = { {0}, {0}, tx };
adapter_t *bb = &adapter;

static unsigned int rng = 12345;
static unsigned int rnd(void) { rng = rng * 1103515245U + 12345U; return rng >> 8; }

/* One part of a RACT (ra.h) in a fake received packet; returns the reply's size
 * field, or NO_REPLY when the loader said nothing (a part that is not the last). */
#define NO_REPLY 0xeeeeeeeeU
static unsigned int ract_part(unsigned int gen, unsigned int n, unsigned int nc,
			      unsigned int r0, unsigned int rc, const unsigned int *spec,
			      unsigned int c0, unsigned int cc, const unsigned int *chain)
{
	static unsigned char in[8192], ipb[64], udpb[64];
	command_t *c = (command_t *)in;
	unsigned int *d = (unsigned int *)c->data, i;
	command_t *reply = (command_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

	memcpy(c->id, "RACT", 4);
	c->address = htonl(gen);
	d[0] = htonl(n);
	d[1] = htonl(nc);
	d[2] = htonl(r0);
	d[3] = htonl(rc);
	d[4] = htonl(c0);
	d[5] = htonl(cc);
	d += 6;
	for (i = 0; i < rc; i++) {
		d[2 * i] = htonl(spec[2 * (r0 + i)]);
		d[2 * i + 1] = htonl(spec[2 * (r0 + i) + 1]);
	}
	d += 2 * rc;
	for (i = 0; i < cc; i++)
		d[i] = htonl(chain[c0 + i]);
	c->size = htonl(24 + 8 * rc + 4 * cc);
	reply->size = htonl(NO_REPLY);
	cmd_ract((ip_header_t *)ipb, (udp_header_t *)udpb, c);
	return ntohl(reply->size);
}

/* a whole table in one part */
static unsigned int ract_c(unsigned int gen, unsigned int n, const unsigned int *spec,
			   unsigned int nc, const unsigned int *chain)
{
	return ract_part(gen, n, nc, 0, n, spec, 0, nc, chain);
}

static unsigned int ract(unsigned int gen, unsigned int n, const unsigned int *spec)
{
	return ract_c(gen, n, spec, 0, 0);
}

static void say(unsigned int v)
{
	if (v == NO_REPLY)
		printf("R -\n");
	else
		printf("R %u\n", v);
}

/* ---- the title's cache: write-back, indexed by a virtual address ---- */
static unsigned int ent_line[512];	/* physical line address held, or ~0 */
static unsigned int tab_base[3], tab_len[3];	/* the table, in words */

static int ent_find(unsigned int line)
{
	unsigned int e;
	for (e = 0; e < 512; e++)
		if (ent_line[e] == line)
			return (int)e;
	return -1;
}

static void ent_fill(unsigned int e, unsigned int line, int dirty)
{
	unsigned int w;
	ent_line[e] = line;
	g_oc_addr[e] = ((0x0c000000U + line) & 0x1ffffc00U) | (dirty ? 3U : 1U);
	for (w = 0; w < 8; w++)
		g_oc_data[e * 8 + w] = g_true[(line >> 2) + w];
}

static void title_writes_v(unsigned int word, unsigned int value)
{
	unsigned int line = (word << 2) & ~31U;
	int e;

	g_true[word] = value;
	if (!mode_ce) {
		g_ram[word] = g_true[word];
		return;
	}
	e = ent_find(line);
	if (e < 0 && (rnd() & 1)) {
		/* a new dirty line, in an entry that a VIRTUAL index chose: bits 13:12
		 * of the entry are anybody's, not the physical address's */
		unsigned int hi, tries;
		for (tries = 0; tries < 16 && e < 0; tries++) {
			hi = rnd() & 3;
			unsigned int cand = (hi << 7) | ((line >> 5) & 127U);
			if (ent_line[cand] == ~0U)
				e = (int)cand;
		}
		if (e >= 0) {
			ent_line[e] = line;	/* filled below from g_true */
		}
	}
	if (e >= 0) {
		ent_fill((unsigned int)e, line, 1);	/* RAM keeps the old value */
	} else {
		g_ram[word] = g_true[word];		/* written through */
	}
}

static void cache_noise(void)
{
	unsigned int e, k;

	if (!mode_ce)
		return;
	/* evict: a dirty line goes back to RAM */
	for (k = 0; k < 4; k++) {
		e = rnd() & 511;
		if (ent_line[e] < 0x100000U && (g_oc_addr[e] & 2U) && (rnd() % 5 == 0)) {
			unsigned int w;
			for (w = 0; w < 8; w++)
				g_ram[(ent_line[e] >> 2) + w] = g_oc_data[e * 8 + w];
			ent_line[e] = ~0U;
			g_oc_addr[e] = 0;
		}
	}
	/* A STALE CLEAN COPY of a line the table does read, in a synonym entry: it must
	 * be ignored (RAM is as good, and newer than it). */
	for (k = 0; k < 3; k++) {
		unsigned int r = rnd() % 3, word = tab_base[r] + rnd() % tab_len[r];
		unsigned int line = (word << 2) & ~31U, hi = rnd() & 3, w;
		e = (hi << 7) | ((line >> 5) & 127U);
		if (ent_line[e] == ~0U && ent_find(line) < 0) {
			g_oc_addr[e] = ((0x0c000000U + line) & 0x1ffffc00U) | 1U;	/* valid, clean */
			for (w = 0; w < 8; w++)
				g_oc_data[e * 8 + w] = rnd();
			ent_line[e] = 0xfffffff0U;
		}
	}
	/* lines the table never asked about, clean or dirty, and one that is not RAM */
	for (k = 0; k < 6; k++) {
		e = rnd() & 511;
		if (ent_line[e] == ~0U && (rnd() & 1)) {
			unsigned int line = ((rnd() % 0x3000) << 5) + 0x40000U;	/* outside the table */
			unsigned int w;
			if (line >= 0x40000U && line < 0x40000U + 0x20000U) {
				g_oc_addr[e] = ((0x0c000000U + line) & 0x1ffffc00U) | ((rnd() & 1) ? 3U : 1U);
				for (w = 0; w < 8; w++)
					g_oc_data[e * 8 + w] = rnd();
				ent_line[e] = 0xfffffff0U;	/* not ours to evict */
			}
		}
	}
	e = rnd() & 511;
	if (ent_line[e] == ~0U) {
		g_oc_addr[e] = 0x10001c00U | 3U | ((e & 31U) << 5) * 0;	/* a non-RAM line */
		ent_line[e] = 0xfffffff0U;
	}
}

static void title_writes(unsigned int word) { title_writes_v(word, rnd()); }

/* ---- the table the title is read through: ranges, then chains ---- */
#define SPEC(parent, shift, off_bytes) \
	(((unsigned int)(parent) << 22) | ((shift) ? 1U << 21 : 0U) | (((unsigned int)((off_bytes) >> 2)) & 0x1fffffU))
#define REGION 0x100000U	/* where the pointers point, in bytes */

static unsigned int nd_words, nchains_n;
static unsigned int chain_spec[8];

/* what the TITLE sees at table word i (the harness's own arithmetic, not ra.c's) */
static void truth(unsigned int *tw)
{
	unsigned int i, k = 0, r;

	for (r = 0; r < 3; r++)
		for (i = 0; i < tab_len[r]; i++)
			tw[k++] = g_true[tab_base[r] + i];
	for (i = 0; i < nchains_n; i++) {
		unsigned int sp = chain_spec[i], pi = sp >> 22, sh = (sp >> 21) & 1;
		int off = (int)(sp << 11) >> 11;
		unsigned int pv = (tw[pi] >> (sh ? 8 : 0)) & 0xffffffU;
		unsigned int a = (pv + ((unsigned int)off << 2)) & 0xfffffcU;
		tw[k++] = g_true[a >> 2];
	}
}

/* a pointer the way a title keeps it: P1 address in the low three bytes' word... */
static unsigned int ptr_word(int high_bytes)
{
	unsigned int p = REGION + 0x200 + 4 * (rnd() % 0x180);
	return high_bytes ? (p << 8) | (rnd() & 0xff) : (0x8c000000U | p);
}

int main(int argc, char **argv)
{
	/* Katana: 700 words in 3 ranges, six windows (more than the 512 words the
	 * loader once took). CE: 300 words, offsets that do not start a cache line,
	 * and a room of 688 words. */
	static const unsigned int spec_k[] = { 0x1000, 100, 0x8000, 200, 0x20000, 400 };
	static const unsigned int spec_c[] = { 0x1008, 100, 0x8010, 120, 0x2000c, 80 };
	const unsigned int *spec;
	unsigned int len[3], base[3], t0 = 0, f, i, k, nw = 0;

	mode_ce = argc > 1 && !strcmp(argv[1], "ce");
	spec = mode_ce ? spec_c : spec_k;
	for (i = 0; i < 3; i++) {
		base[i] = tab_base[i] = spec[2 * i] >> 2;
		len[i] = tab_len[i] = spec[2 * i + 1];
		nw += len[i];
	}
	nd_words = nw;
	/* Chains: word 0 and 1 of the table hold P1 pointers, word 2 one in the HIGH
	 * three bytes of its word (an address 1 mod 4). C2's word is itself a pointer
	 * that C3 follows, and C4 reads through the shifted one. */
	chain_spec[0] = SPEC(0, 0, 0x2c);
	chain_spec[1] = SPEC(0, 0, 0x30);
	chain_spec[2] = SPEC(1, 0, -0x10);
	chain_spec[3] = SPEC(nd_words + 2, 0, 4);
	chain_spec[4] = SPEC(2, 1, 0x20);
	nchains_n = 5;
	/* every word the pointers can lead to holds a pointer, so that C3 can follow C2 */
	for (i = 0; i < 0x1000; i++)
		g_true[(REGION >> 2) + i] = g_ram[(REGION >> 2) + i] = 0x8c000000U | (REGION + 0x200 + 4 * (rnd() % 0x180));
	g_true[base[0] + 0] = g_ram[base[0] + 0] = ptr_word(0);
	g_true[base[0] + 1] = g_ram[base[0] + 1] = ptr_word(0);
	g_true[base[0] + 2] = g_ram[base[0] + 2] = ptr_word(1);
	memset(ent_line, 0xff, sizeof(ent_line));
	g_ccr = 1;	/* OCE */

	/* refusals */
	{
		say(ract_part(1, 385, 0, 0, 0, 0, 0, 0, 0));	/* more ranges than RA_RANGES */
		say(ract(1, 1, (const unsigned int[]){ 0x1002, 4 }));
		say(ract(1, 1, (const unsigned int[]){ 0x1000, 1100 }));
		say(ract(1, 2, (const unsigned int[]){ 0x2000, 4, 0x1000, 4 }));	/* not ascending */
	}
	/* a chain whose parent is not an earlier word is refused; so are too many */
	{
		static unsigned int bad[1] = { 0x7ffU << 22 };
		unsigned int tooMany[65] = { 0 };
		say(ract_c(1, 3, spec, 1, bad));
		say(ract_c(1, 3, spec, 65, tooMany));
	}
	/* The table in parts: a first part of generation 6, then generation 7 starts
	 * over; a late part of 6 and a part of 7 that skips one are ignored; then the
	 * rest of 7, its last range and the chains, completes it. Only that answers. */
	say(ract_part(6, 3, nchains_n, 0, 1, spec, 0, 0, chain_spec));
	say(ract_part(7, 3, nchains_n, 0, 2, spec, 0, 0, chain_spec));
	say(ract_part(6, 3, nchains_n, 1, 2, spec, 0, nchains_n, chain_spec));
	say(ract_part(7, 3, nchains_n, 2, 1, spec, 1, 4, chain_spec));
	say(ract_part(7, 3, nchains_n, 2, 1, spec, 0, nchains_n, chain_spec));
	printf("T %u\n", nw + nchains_n);

	for (f = 1; f <= 400; f++) {
		/* the script: most frames change little, some a lot */
		unsigned int changes = (f % 37 == 0) ? 200 : (f % 3 == 0 ? 0 : 1 + rnd() % 12);
		for (k = 0; k < changes; k++) {
			unsigned int r = rnd() % 3, j = rnd() % len[r];
			title_writes(base[r] + j);
		}
		/* the pointers move now and then (an object list), and what they lead to
		 * changes -- and goes through dirty cache lines under Windows CE */
		if (f % 5 == 0)
			title_writes_v(base[0] + (rnd() & 1), ptr_word(0));
		if (f % 11 == 0)
			title_writes_v(base[0] + 2, ptr_word(1));
		for (k = 0; k < 6; k++)
			title_writes_v((REGION >> 2) + 0x80 + rnd() % 0x400,
				       (rnd() & 1) ? ptr_word(0) : rnd());
		cache_noise();
		g_istnrm = 8;
		t0 += 1600000U;
		ra_tick(t0);
		/* what the title saw at this frame */
		{
			static unsigned int tw[1100];
			truth(tw);
			printf("E %u ", f & 0xffff);
			for (i = 0; i < nd_words + nchains_n; i++)
				printf("%08x", tw[i]);
			printf("\n");
		}
		/* a busy network now and then: no flush, frames may be dropped */
		if (!(f >= 200 && f < 260) && ra_flush_due(t0))
			ra_flush();
	}
	printf("S frames=%u sent=%u dropped=%u keys=%u oc_words=%u nofit=%u\n",
	       g_ra_frames, g_ra_sent, g_ra_dropped, g_ra_keys, g_ra_oc_words, g_ra_nofit);
	return 0;
}
