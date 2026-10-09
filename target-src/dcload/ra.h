#ifndef __RA_H__
#define __RA_H__

/*
 * RetroAchievements sampler (AGENTS.md 4.20). The loader only READS the
 * words the host asked for, once per frame, and sends what changed; every
 * condition of every achievement is evaluated on the host (rcheevos).
 */

#ifndef WITH_RCHEEVOS
#define WITH_RCHEEVOS 0
#endif

#if WITH_RCHEEVOS
#include "commands.h"
#include "packet.h"

/* Sizes of the table. Sonic Adventure 2's set reads 324 distinct words in 133
 * ranges directly, before any pointer (`dcload-ip-rs ra-count`, 2026-10-06);
 * Shenmue II's 179 in 56; Sega Rally 2's 1775 in 337, seven copies of every
 * condition (2026-10-08). The ranges (4 B each) live in BSS; the words in dead
 * RAM, whose room (ra.c, ra_max_words()) is the real limit: RA_WORDS only caps
 * it, and must stay a multiple of RA_WIN and at most 1024 (a chain names its
 * parent in 10 bits, a range packs its length in 10). Fewer ranges than a set
 * needs means merging them over gaps: words the set does not read. */
#ifndef RA_RANGES
#define RA_RANGES 384U
#endif
#ifndef RA_WORDS
#define RA_WORDS  1024U	/* words of a table: its ranges' and its chains' */
#endif
/* Words the loader works out itself from a pointer it has just sampled (AGENTS.md
 * 4.20, `chains.rs` on the host): they count in RA_WORDS. */
#ifndef RA_CHAINS
#define RA_CHAINS 64U
#endif
/* A frame travels as records of at most RA_WIN words, so that a keyframe of the
 * whole table can be cut into datagrams. */
#define RA_WIN     128U
#define RA_WIN_MAP (RA_WIN / 32U)

/* RACT: host -> DC. address = generation (1..). A table may take several
 * datagrams (the loader takes no IP fragment); each part's data, big-endian
 * 32-bit words:
 *   ranges in all, chains in all, first range of this part, its ranges, first
 *   chain of this part, its chains,
 *   then {RAM offset, words} per range (ascending and apart over the whole
 *   table), then per chain `parent index << 22 | shift flag << 21 | offset in
 *   words` (signed, 21 bits): the word at ((word[parent] >> 8 * flag) & 0xffffff)
 *   + offset. A parent is an earlier word of the table.
 * Parts come in order, the chains after the last range. A part with both firsts
 * 0 starts a table (sampling stops until it is whole); a part that does not follow
 * the last one received is ignored, and the host sends the whole table again. Only
 * the part that completes a table is answered: size 0 accepted, 0xffffffff refused
 * (too many ranges, words or chains, ranges not ascending and apart, a chain whose
 * parent is not earlier, or no room under either kind of title); a refusal may
 * also answer an earlier part. The data of the answer is the room, in table words,
 * under a Katana title and under Windows CE. The same table again asks for a
 * keyframe. Under the title that runs, ra_attach() checks the room again
 * (g_ra_nofit). */
void cmd_ract(ip_header_t * ip, udp_header_t * udp, command_t * command);

/* irq_tick(): called on every interrupt. Samples at the start of a vertical
 * blank (at most once per 8 ms). Touches no network. */
void ra_tick(unsigned int t0);
/* irq_tick(), GD lock free and no read on the wire: send the batch when it is
 * due. */
int ra_flush_due(unsigned int t0);
void ra_flush(void);
#else
#define ra_tick(t0) do { } while (0)
#endif

#endif
