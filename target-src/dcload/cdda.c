/*
 * cdda.c -- CD-DA playback. Read cdda.h first: it says what this is modelled
 * on, which of isoldr's options it keeps, and which it replaces and why.
 *
 * THE SHAPE OF IT
 *
 *   host -- CMD_CDDAREAD --> raw 2352-byte sectors --> staging buffer (.hiram)
 *        -- split L/R, 32 bits at a time --> two AICA ring buffers in sound RAM
 *        -- two AICA channels loop over those rings forever
 *
 * The rings are what makes this survivable. A channel set to loop over a
 * 32 KB buffer plays for 0.37 s per lap without anyone touching it, so the
 * engine only has to get back before the play position laps the write
 * position. Everything below is bookkeeping around that one race.
 *
 * TWO INVARIANTS, BOTH PAID FOR ELSEWHERE IN THIS TREE
 *
 *   - A TRANSMIT IS ONLY SAFE AT THE TOP LEVEL OF A SYSCALL (AGENTS.md 4.5):
 *     pkt_buf is single and shared, and bb->loop() dispatches commands that
 *     build into it. cdda_service() therefore transmits, and
 *     cdda_service_irq() -- which can land anywhere, including inside a
 *     bb->loop() -- never does. That split is not a nicety, it is the reason
 *     the interrupt path exists in a separate function.
 *
 *   - EVERY HARDWARE WAIT IS BOUNDED (AGENTS.md 4.8). The G2 FIFO waits below
 *     spin on a status register, and a G2 bus that never drains would
 *     otherwise hang the loader inside a title's frame. They give up instead.
 */

#include <string.h>
#include "syscalls.h"
#include "packet.h"
#include "net.h"
#include "adapter.h"
#include "commands.h"
#include "cdfs.h"
#include "cdda.h"
#include "hiram.h"

#if WITH_CDDA

/* ------------------------------------------------------------------ AICA */

/* Sound registers and sound RAM, through the uncached G2 window. */
#define SNDREG32(x)        (*(volatile unsigned int *)(0xa0700000 + (x)))
#define CHNREG32(ch, x)    SNDREG32(0x80 * (ch) + (x))
#define AICA_RAM(off)      (*(volatile unsigned int *)(0xa0800000 + (off)))

/* G2 FIFO status. The CPU must not push more than eight 32-bit writes into
 * the G2 pipe without letting it drain, or they are lost -- silently. */
#define G2_FIFO_STATUS     (*(volatile unsigned int *)0xa05f688c)
#define G2_FIFO_AICA       (1u << 0)
#define G2_FIFO_G2         (1u << 4)

/* Bounded, per the rule above: a G2 bus that never reports empty gets a
 * dropped sample rather than a hung console. 10000 is four orders of
 * magnitude more than the handful of cycles it normally takes. */
#define G2_FIFO_SPIN_LIMIT 10000

/*
 * THE PACING CLOCK IS A TIMER, NOT THE AICA'S PLAY POSITION.
 *
 * Reading the channel's own position (registers 0x280c/0x2814) looks like the
 * obvious answer and is the wrong one -- isoldr compiles that read only under
 * HAVE_CDDA_TEST and paces production playback from an SH4 timer
 * (`aica_get_pseudo_pos`, cdda.c:659). Measured here on Snow Surfers, on
 * console: with the position as the flow control the engine fetched ~85 times
 * a second where 25 was right, **3.4x real time**, and starved the title until
 * it stopped answering. The arithmetic says why. 0x280c is the AICA's monitor
 * SELECT register and the game's own sound driver writes it too, so what comes
 * back between our write and our read is some other channel's position, or
 * nothing meaningful. A junk position makes `room >= headroom` true about 86 %
 * of the time, and the fetch rate becomes the SERVICE CALL rate times 0.86 --
 * which is exactly the 85/s that was observed.
 *
 * So: TMU2, which `setup_machine()` (dcload.c) already programmes and leaves
 * stopped with TCR2 = 0, i.e. the peripheral clock divided by 4. DreamShell's
 * preset for this very title asks for POS_TMU2, which is isoldr saying that
 * this game does not use that timer. A game that does would be a conflict, and
 * that is what isoldr's POS_TMU1 alternative exists for.
 */
#define TMU_TSTR           (*(volatile unsigned char *)0xffd80004)
#define TMU_TCOR2          (*(volatile unsigned int *)0xffd80020)
#define TMU_TCNT2          (*(volatile unsigned int *)0xffd80024)
#define TMU_TCR2           (*(volatile unsigned short *)0xffd80028)
#define TMU_START_TMU2     0x04

/* Peripheral clock / 4 = 12.5 MHz, and 44100 samples/s of it is
 * 44100 / 12500000 = 0.0035280 samples per tick. As 20-bit fixed point that is
 * 3699 / 2^20, which is 0.0035276 -- one part in a million, and the remainder
 * is carried so it does not accumulate at all. */
#define TICK_TO_SAMPLE_Q20 3699
/* Ticks per service that the multiply above stays inside 32 bits for: 80 ms,
 * far longer than the ~10 ms between two GD syscalls. Anything longer is an
 * underrun, and is counted as one rather than wrapping the arithmetic. */
#define TICK_CLAMP         1000000

/* Never let one service call empty the host's patience or the title's frame.
 * Belt and braces after the runaway above: correct pacing already holds this
 * to about one fetch per two calls, and if the pacing is ever wrong again the
 * damage is bounded to two round trips per syscall instead of a burst that
 * starves the game. */
#define MAX_FETCH_PER_SERVICE 2

/* isoldr's CH_FIXED: the last two channels, which no BIOS driver hands out
 * first and which its own default picks. */
#define CDDA_CH_LEFT       62
#define CDDA_CH_RIGHT      63

/* Sound RAM is 2 MB on a retail console; the rings go at the very top, the
 * furthest point from anything a game's sound driver allocates upwards. */
#define AICA_RAM_END       0x00200000

/* Per channel. 32 KB = 16384 samples = 0.372 s at 44.1 kHz, so the engine has
 * to be called back roughly three times a second to keep the stream unbroken
 * -- against the ~60 GD syscalls a second a title's frame loop makes. */
#define RING_BYTES         (32 * 1024)
#define RING_SAMPLES       (RING_BYTES / 2)

#define AICA_LEFT_BASE     (AICA_RAM_END - 2 * RING_BYTES)
#define AICA_RIGHT_BASE    (AICA_RAM_END - 1 * RING_BYTES)

/* AICA_SM_16BIT: linear PCM, which is what a CD audio track already is. */
#define AICA_FMT_16BIT     0
/* Register 24 is the pitch, and 0 IS 44100 Hz -- the AICA's own base rate, and
 * exactly what isoldr writes (cdda.c:1021, `cdda->aica_freq = 0`). */
#define AICA_PITCH_44100   0

/* --------------------------------------------------------------- streaming */

#define RAW_SECTOR_SIZE    2352
/* Frames per raw sector: 2352 / 4. Exactly 1/75 of a second, by construction
 * of the CD format. */
#define FRAMES_PER_SECTOR  588

/* How much is fetched per host round trip. Three sectors is 40 ms of audio,
 * so the stream costs ~25 requests a second -- an order of magnitude below
 * what a disc read burst does, and it only happens while music is playing.
 * The buffer is the only large object this file adds and it is in .hiram, so
 * it costs nothing in _end (AGENTS.md 4.6). Raising it costs .hiram, which is
 * bounded by the Maple DMA buffer above it and asserted at link time. */
#define FETCH_SECTORS      3
#define FETCH_BYTES        (FETCH_SECTORS * RAW_SECTOR_SIZE)
#define FETCH_FRAMES       (FETCH_SECTORS * FRAMES_PER_SECTOR)

/* Refuse to start a fetch unless this much of the ring is free, so a fetch
 * never has to be split across the wrap. One fetch plus a sector of slack. */
#define FETCH_HEADROOM     (FETCH_FRAMES + FRAMES_PER_SECTOR)

/* The BIOS "repeat forever" value. Anything else is a play count. */
#define CDDA_LOOP_FOREVER  0x0f

/* Deliberately shorter than the data path's GD_SYSCALL_TIMEOUT_SECONDS.
 * A late audio fetch is a gap in the music; blocking a title's frame loop for
 * six seconds to avoid one would trade an unnoticeable glitch for a visible
 * freeze. The stream gives up quickly and catches up on the next service. */
#ifndef CDDA_TIMEOUT_SECONDS
#define CDDA_TIMEOUT_SECONDS 2
#endif

/* TWO SAMPLES GO INTO ONE 32-BIT WORD, so the write cursor moves in steps of
 * two from zero and a word never straddles the end of the ring. That holds
 * only while both the ring and the fetch are an even number of samples, which
 * is worth having the compiler check rather than reasoning about again: an
 * odd fetch would put a four-byte store two bytes past the left ring and into
 * the right one, which is a click in one ear and nothing else to see. */
typedef char cdda_ring_is_even[(RING_SAMPLES % 2 == 0) ? 1 : -1];
typedef char cdda_fetch_is_even[(FETCH_FRAMES % 2 == 0) ? 1 : -1];

HIRAM_BUF static unsigned char cdda_pcm[FETCH_BYTES];

/* The disc's table of contents, whole. 408 bytes of BSS, which buys not having
 * to ask the host again every time a title changes track -- and a title that
 * plays music changes track often. */
static unsigned int cdda_toc[102];
static int cdda_toc_valid;

static struct {
	unsigned int state;
	unsigned int first_lba;   /* start of the range being played */
	unsigned int last_lba;    /* inclusive end */
	unsigned int next_lba;    /* the sector the next fetch will ask for */
	unsigned int loop;
	unsigned int write_pos;   /* samples written into each ring, mod RING_SAMPLES */
	unsigned int written;     /* samples written since the stream started */
	unsigned int played;      /* samples the timer says have been consumed */
	unsigned int frac;        /* 20-bit remainder of the tick conversion */
	unsigned int last_tick;   /* TCNT2 at the previous service */
	unsigned int running;     /* channels are keyed on */
	unsigned int drained;     /* the source ran out; let the ring play out */
} cd;

/* Always-compiled counters, same contract as the rest of AGENTS.md 11: an
 * engine whose only symptom is silence needs something to say WHERE it
 * stopped. Read them with scripts/dc-counters.py. */
unsigned int g_cdda_plays;
unsigned int g_cdda_fetches;
unsigned int g_cdda_fetch_fails;
unsigned int g_cdda_underruns;
unsigned int g_cdda_toc_fails;
unsigned int g_cdda_irq_pushes;
unsigned int g_cdda_last_lba;
/* The AICA's own idea of the play position, sampled but NOT acted on -- see the
 * timer comment above. Kept because "would the hardware position have worked on
 * this console" is a question worth one G2 read to answer, and because a value
 * that never moves is how you tell a channel that never started from one that
 * is playing into a muted mixer. */
unsigned int g_cdda_aica_pos;
/* Services that arrived while another transfer was waiting for its answer. A
 * few are normal; a lot means the stream is being starved by the data path. */
unsigned int g_cdda_deferred;
/* The AICA master volume as it was found, and whether this loader had to raise
 * it. A title that has not started its own sound driver leaves it at 0, and
 * two perfectly correct channels are then perfectly inaudible. */
unsigned int g_cdda_mvol;
unsigned int g_cdda_mvol_raised;

/* ------------------------------------------------------------- G2 plumbing */

static void g2_fifo_wait(void)
{
	int spin = G2_FIFO_SPIN_LIMIT;

	while ((G2_FIFO_STATUS & (G2_FIFO_G2 | G2_FIFO_AICA)) && spin--)
	{
		/* spin */
	}
}

/* ------------------------------------------------------------------ pacing */

static void cdda_timer_start(void)
{
	TMU_TSTR = (unsigned char)(TMU_TSTR & ~TMU_START_TMU2);
	TMU_TCR2 = 0;              /* Pck/4, and UNIE clear: no interrupt */
	TMU_TCOR2 = 0xffffffff;
	TMU_TCNT2 = 0xffffffff;
	TMU_TSTR = (unsigned char)(TMU_TSTR | TMU_START_TMU2);
	cd.last_tick = TMU_TCNT2;
	cd.frac = 0;
}

static void cdda_timer_stop(void)
{
	TMU_TSTR = (unsigned char)(TMU_TSTR & ~TMU_START_TMU2);
}

/* Advance the played-sample count by however much time has passed. TCNT2
 * counts DOWN, so the delta is previous minus current, which is also what
 * makes the wrap at zero come out right on its own. */
static void cdda_advance(void)
{
	unsigned int now = TMU_TCNT2;
	unsigned int dt = cd.last_tick - now;
	unsigned int acc;

	cd.last_tick = now;
	if (dt > TICK_CLAMP)
	{
		dt = TICK_CLAMP;
	}
	acc = cd.frac + dt * TICK_TO_SAMPLE_Q20;
	cd.played += acc >> 20;
	cd.frac = acc & 0xfffffu;
}

/* ------------------------------------------------------------ AICA channels */

static unsigned int aica_play_pos(void)
{
	unsigned int pos;

	/* Point the monitor register at our channel, then read its position.
	 * Two accesses with a drain between them, exactly as isoldr does
	 * (cdda.c:599). */
	SNDREG32(0x280c) = (SNDREG32(0x280c) & 0xffff00ffu) | (CDDA_CH_LEFT << 8);
	g2_fifo_wait();
	pos = SNDREG32(0x2814) & 0xffffu;
	return pos;
}

static void aica_channel_off(unsigned int ch)
{
	unsigned int val = CHNREG32(ch, 0);

	g2_fifo_wait();
	/* KEY OFF: drop bit 14 (key-on-execute), keep bit 15. isoldr's
	 * aica_stop_cdda(), cdda.c:330. */
	CHNREG32(ch, 0) = (val & ~0x4000u) | 0x8000u;
}

static void aica_channel_on(unsigned int ch, unsigned int base)
{
	/* Volume and pan first, then the sample description, then the key-on --
	 * the control word is written last on purpose, because writing it is
	 * what starts the channel. */
	CHNREG32(ch, 40) = 0x24u | (255u << 8);          /* volume */
	CHNREG32(ch, 36) = (ch == CDDA_CH_LEFT ? 0x1fu : 0x0fu) | (0xfu << 8);
	g2_fifo_wait();
	CHNREG32(ch, 8) = 0;                             /* loop start */
	CHNREG32(ch, 12) = (RING_SAMPLES - 1) & 0xffffu; /* loop end */
	CHNREG32(ch, 16) = 0x1f;                         /* AEG: no envelope */
	CHNREG32(ch, 24) = AICA_PITCH_44100;
	CHNREG32(ch, 4) = base & 0xffffu;
	g2_fifo_wait();
	/* start | format | loop | high bits of the sample address */
	CHNREG32(ch, 0) = 0xc000u | (AICA_FMT_16BIT << 7) | 0x200u | (base >> 16);
}

/* Silence the ring before keying on, so a channel that starts before the
 * first fetch lands plays nothing rather than whatever the previous owner of
 * that sound RAM left there. */
static void aica_clear_rings(void)
{
	unsigned int i;

	for (i = 0; i < RING_BYTES; i += 4)
	{
		if ((i & 0x1c) == 0)
		{
			g2_fifo_wait();
		}
		AICA_RAM(AICA_LEFT_BASE + i) = 0;
		AICA_RAM(AICA_RIGHT_BASE + i) = 0;
	}
}

/*
 * Make sure the mixer is actually on, and NOTHING ELSE.
 *
 * Measured 2026-08-29 on Snow Surfers: `g_cdda_aica_pos` stayed at 0 for a
 * whole stream, so the channels were keyed on into an AICA that plays nothing.
 * A title that has not started its own sound driver yet leaves the master
 * volume at 0, and our two channels are then perfectly correct and perfectly
 * inaudible.
 *
 * isoldr's `aica_init()` keys off all 64 channels, parks the ARM and rewrites
 * the start of sound RAM -- and it compiles that only under HAVE_CDDA_TEST,
 * because doing it to a game that HAS started its sound driver destroys it.
 * So this does the one thing that is safe in both cases: raise the master
 * volume if and only if it is zero. A game that has set it keeps whatever it
 * chose, the channels are left alone, and the ARM is never touched.
 */
static void aica_mixer_on(void)
{
	unsigned int mvol;

	g2_fifo_wait();
	mvol = SNDREG32(0x2800);
	g_cdda_mvol = mvol;

	if ((mvol & 0x0fu) == 0)
	{
		g2_fifo_wait();
		SNDREG32(0x2800) = (mvol & ~0x0fu) | 0x0fu;
		g_cdda_mvol_raised++;
	}
}

static void cdda_channels_start(void)
{
	aica_mixer_on();
	aica_clear_rings();
	g2_fifo_wait();
	aica_channel_on(CDDA_CH_LEFT, AICA_LEFT_BASE);
	aica_channel_on(CDDA_CH_RIGHT, AICA_RIGHT_BASE);
	g2_fifo_wait();
	cd.running = 1;
	cd.write_pos = 0;
	cd.written = 0;
	cd.played = 0;
	cdda_timer_start();
}

static void cdda_channels_stop(void)
{
	if (!cd.running)
	{
		return;
	}
	aica_channel_off(CDDA_CH_LEFT);
	aica_channel_off(CDDA_CH_RIGHT);
	g2_fifo_wait();
	cdda_timer_stop();
	cd.running = 0;
}

/* ------------------------------------------------------------- host fetches */

/* Ask the host for the whole disc's table of contents.
 *
 * Area 2 is this loader's own request and means "every track, both areas at
 * once". A title only ever asks for area 0 or 1 -- the CD part or the GD part
 * -- and merging two of those here would need a second 408-byte buffer to
 * merge into. The host answers it; see build_dc_toc().
 */
static int cdda_load_toc(void)
{
	command_3int_t *command =
		(command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

	if (cdda_toc_valid)
	{
		return 0;
	}

	memcpy(command->id, CMD_CDFSTOC, 4);
	command->value0 = htonl(2);                             /* whole disc */
	command->value1 = htonl((unsigned int)cdda_toc);
	command->value2 = 0;

	syscall_retval = (unsigned int)-1;
	timeout_loop = CDDA_TIMEOUT_SECONDS;
	build_send_packet(sizeof(command_3int_t));
	bb->loop(0);
	timeout_loop = 0;

	if ((int)syscall_retval < 0)
	{
		g_cdda_toc_fails++;
		return -1;
	}
	cdda_toc_valid = 1;
	return 0;
}

/* One transfer of FETCH_SECTORS raw audio sectors into the staging buffer.
 * Transmits, so top level of a syscall only -- see the header. */
static int cdda_fetch(unsigned int lba)
{
	command_3int_t *command =
		(command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

	memcpy(command->id, CMD_CDDAREAD, 4);
	command->value0 = htonl(lba);
	command->value1 = htonl((unsigned int)cdda_pcm);
	command->value2 = htonl(FETCH_BYTES);

	syscall_retval = (unsigned int)-1;
	timeout_loop = CDDA_TIMEOUT_SECONDS;
	build_send_packet(sizeof(command_3int_t));
	bb->loop(0);
	timeout_loop = 0;

	if ((int)syscall_retval < 0)
	{
		g_cdda_fetch_fails++;
		return -1;
	}
	g_cdda_fetches++;
	g_cdda_last_lba = lba;
	return 0;
}

/* --------------------------------------------------------------- the split */

/*
 * Interleaved stereo in, two mono rings out, 32 bits at a time.
 *
 * A CD frame is L,R as two signed 16-bit samples, and an AICA channel wants
 * one contiguous mono stream. Taking TWO frames at a time means the two
 * left samples pack into one 32-bit word and the two right samples into
 * another, so the whole split is two loads and two stores per frame pair with
 * no intermediate buffer -- which matters, because G2 wants 32-bit accesses
 * and a 16-bit store to sound RAM is not one.
 *
 * `frames` is always even here: FETCH_FRAMES is 1764.
 */
static void cdda_push_frames(const unsigned char *src, unsigned int frames)
{
	unsigned int pairs = frames >> 1;
	unsigned int pos = cd.write_pos;
	unsigned int i;
	unsigned int guard = 0;

	for (i = 0; i < pairs; i++)
	{
		unsigned int f0 = ((const unsigned int *)src)[0];
		unsigned int f1 = ((const unsigned int *)src)[1];
		unsigned int left = (f0 & 0xffffu) | (f1 << 16);
		unsigned int right = (f0 >> 16) | (f1 & 0xffff0000u);
		unsigned int off = pos << 1;

		/* One drain every eight words pushed, which is four iterations. */
		if ((guard++ & 3) == 0)
		{
			g2_fifo_wait();
		}
		AICA_RAM(AICA_LEFT_BASE + off) = left;
		AICA_RAM(AICA_RIGHT_BASE + off) = right;

		src += 8;
		pos += 2;
		if (pos >= RING_SAMPLES)
		{
			pos -= RING_SAMPLES;
		}
	}
	g2_fifo_wait();
	cd.write_pos = pos;
	cd.written += frames;
}

/* How many samples may be written before the writer laps the reader. One
 * sample of slack so the two never sit on top of each other, which is
 * indistinguishable from a full lap.
 *
 * If the reader has passed the writer the ring ran dry: the samples that
 * should have been there were played as whatever the buffer still held. There
 * is no undoing it, so it is counted and the two are resynchronised -- leaving
 * `played` ahead would make every later `room` wrong as well. */
static unsigned int cdda_room(void)
{
	unsigned int ahead;

	cdda_advance();
	ahead = cd.written - cd.played;

	if ((int)ahead <= 0)
	{
		/* Not at the very start, where written == played == 0 is simply an
		 * empty ring nobody has fed yet. A counter that fires once on every
		 * stream would make the real thing unreadable. */
		if (cd.written)
		{
			g_cdda_underruns++;
		}
		cd.played = cd.written;
		return RING_SAMPLES - 1;
	}
	if (ahead >= RING_SAMPLES)
	{
		return 0;
	}
	return RING_SAMPLES - ahead - 1;
}

/* ------------------------------------------------------------ the interface */

static void cdda_begin(unsigned int first, unsigned int last, unsigned int loop)
{
	cd.first_lba = first;
	cd.last_lba = last;
	cd.next_lba = first;
	cd.loop = loop;
	cd.drained = 0;
	cd.state = CDDA_PLAYING;
	g_cdda_plays++;
	cdda_channels_start();
}

int cdda_play_sectors(unsigned int first, unsigned int last, unsigned int loop)
{
	if (last < first)
	{
		return -1;
	}
	cdda_begin(first, last, loop);
	return 0;
}

int cdda_play_tracks(unsigned int first, unsigned int last, unsigned int loop)
{
	unsigned int start, end;

	if (first < 1 || first > 99 || last < first || last > 99)
	{
		return -1;
	}
	if (cdda_load_toc() < 0)
	{
		return -1;
	}

	/* A track's entry is its start; its end is the next track's start minus
	 * one, and for the last track that is the lead-out. Entry 101 is the
	 * lead-out and an absent track reads 0xffffffff. */
	if (cdda_toc[first - 1] == 0xffffffffu)
	{
		return -1;
	}
	start = cdda_toc[first - 1] & 0xffffffu;

	end = 0;
	if (last < 99 && cdda_toc[last] != 0xffffffffu)
	{
		end = (cdda_toc[last] & 0xffffffu) - 1;
	}
	else if (cdda_toc[101] != 0xffffffffu)
	{
		end = (cdda_toc[101] & 0xffffffu) - 1;
	}
	if (end <= start)
	{
		return -1;
	}

	cdda_begin(start, end, loop);
	return 0;
}

int cdda_pause(void)
{
	if (cd.state != CDDA_PLAYING)
	{
		return -1;
	}
	cdda_channels_stop();
	cd.state = CDDA_PAUSED;
	return 0;
}

int cdda_release(void)
{
	if (cd.state != CDDA_PAUSED)
	{
		return -1;
	}
	/* Resume from where the source got to, not from where the ring is: the
	 * ring's contents were abandoned when the channels were keyed off. */
	cd.state = CDDA_PLAYING;
	cdda_channels_start();
	return 0;
}

int cdda_stop(void)
{
	cdda_channels_stop();
	cd.state = CDDA_STOPPED;
	cd.drained = 0;
	return 0;
}

int cdda_seek(unsigned int lba)
{
	if (cd.state == CDDA_STOPPED)
	{
		return -1;
	}
	cd.next_lba = lba;
	cd.drained = 0;
	cdda_channels_start();
	return 0;
}

int cdda_state(void)
{
	return (int)cd.state;
}

unsigned int cdda_current_lba(void)
{
	/* What the LISTENER is hearing, not what the fetcher has reached: the
	 * ring holds up to 0.37 s of audio the title has not heard yet, and a
	 * title that uses the position to synchronise anything would be ahead of
	 * its own music by that much. One ring's worth back, in sectors. */
	unsigned int behind = RING_SAMPLES / FRAMES_PER_SECTOR;

	if (cd.next_lba < cd.first_lba + behind)
	{
		return cd.first_lba;
	}
	return cd.next_lba - behind;
}

/* --------------------------------------------------------------- the engine */

void cdda_service(void)
{
	unsigned int room;
	unsigned int fetched = 0;

	if (cd.state != CDDA_PLAYING || !cd.running)
	{
		return;
	}

	/*
	 * NOT WHILE ANOTHER TRANSFER IS ARMED. `timeout_loop` is non-zero exactly
	 * while some other syscall has sent its command and is sitting in
	 * bb->loop() waiting for the answer -- and bb->loop() dispatches whatever
	 * arrives, which is how this function can be reached from inside one.
	 * Transmitting there would build a CDDA request into the pkt_buf that
	 * transfer is still using, which is the failure AGENTS.md 4.5 exists to
	 * forbid.
	 *
	 * isoldr can call CDDA_MainLoop() from anywhere because its fetch is a
	 * read from a local device; ours is a packet, and the buffer is single.
	 * Costs nothing: the next service is at most one syscall away, and the
	 * ring holds 0.37 s.
	 */
	if (timeout_loop)
	{
		g_cdda_deferred++;
		return;
	}

	room = cdda_room();
	/* Diagnostic only, one G2 read: see the header of the timer block. */
	g_cdda_aica_pos = aica_play_pos();

	/* The source is exhausted and the ring is playing out the tail. Stop
	 * once everything written has certainly been heard. */
	if (cd.drained)
	{
		if (cd.played >= cd.written)
		{
			cdda_stop();
		}
		return;
	}

	while (room >= FETCH_HEADROOM && fetched < MAX_FETCH_PER_SERVICE)
	{
		if (cd.next_lba + FETCH_SECTORS - 1 > cd.last_lba)
		{
			if (cd.loop == CDDA_LOOP_FOREVER)
			{
				cd.next_lba = cd.first_lba;
			}
			else if (cd.loop > 0)
			{
				cd.loop--;
				cd.next_lba = cd.first_lba;
			}
			else
			{
				cd.drained = 1;
				return;
			}
		}

		if (cdda_fetch(cd.next_lba) < 0)
		{
			/* A refused read is silence, not a fault: the host says so
			 * when the image cannot serve audio at all, and failing the
			 * title's PLAY command afterwards would be worse than a
			 * quiet game. Give up on this stream. */
			cdda_stop();
			return;
		}
		cd.next_lba += FETCH_SECTORS;
		cdda_push_frames(cdda_pcm, FETCH_FRAMES);
		room -= FETCH_FRAMES;
		fetched++;
	}
}

void cdda_service_irq(void)
{
	/* Nothing to push and nothing that may transmit: the whole point of this
	 * entry is that it can land inside a bb->loop(). It exists so that the
	 * channels are kept honest -- position read, underrun counted -- while a
	 * title is not calling the GD driver, and so the next top-level service
	 * knows how far behind it is. */
	if (cd.state != CDDA_PLAYING || !cd.running)
	{
		return;
	}
	g_cdda_irq_pushes++;
	/* Keeps the played-sample count honest across a stretch in which the
	 * title makes no GD syscall, so the next top-level service knows how far
	 * behind it really is instead of attributing the whole gap to one tick. */
	(void)cdda_room();
}

#endif /* WITH_CDDA */
