/*
 * cdda.c -- CD-DA (Redbook audio) playback. The loader is the drive.
 *
 * A title plays its disc's audio tracks with CMD_PLAY_TRACKS/CMD_PLAY_SECTORS.
 * There is no drive, so this file fetches the audio from the host over UDP and
 * plays it on AICA channels 62 (left) and 63 (right), on the model of
 * DreamShell isoldr's CD-DA emulation (SH4 side only: the title owns the ARM).
 *
 * RING AND LEAD. Each channel loops over one ring in sound RAM, written by a
 * single write head that is kept a fixed distance -- the LEAD -- ahead of the
 * AICA. The play position is not read from the AICA (its monitor-select
 * register is shared with the title's sound driver): TMU1, started in the
 * key-on critical section and reloading once per loop, stands in for it.
 * cdda_service() fetches only what the lead is short of, so the fetches come
 * one every 53 ms, at the rate the audio is consumed.
 *
 * WHY NOT A DOUBLE BUFFER. isoldr fills half a ring at a time because it reads
 * from the drive; here every sub-fetch is a UDP round trip the title is frozen
 * for (~3 ms). Filling a half on its boundary meant 13 of them inside ~216 ms
 * and then 477 ms of nothing, which cost Snow Surfers 4-5 dropped frames once
 * a second. The same work spread evenly is ~3 ms every third frame.
 *
 * WHO CALLS IT. Only the top level of a GD syscall (the GD server loop and
 * gdGdcGetDrvStat) and cdda_service_between_chunks() inside a long disc read.
 * Never an interrupt: a fetch transmits, and pkt_buf and the LoadBinary window
 * are shared. It declines while a disc read owns them (g_gd_in_transfer). A
 * title that stops calling the GD driver stops feeding the music (isoldr too).
 *
 * FORMAT. 4-bit Yamaha ADPCM by default (DC24: the host sends the left block
 * then the right), a quarter of PCM's bytes on the wire and on G2, which is
 * time the title is frozen. CDDA_ADPCM=0 selects 16-bit PCM (DC23). ADPCM is
 * differential: the host's encoder and the AICA's decoder share state, so each
 * key-on (which resets the decoder) restarts the encoder (cd.restart), and
 * every byte the AICA plays must be the continuation of the one before.
 *
 * GEOMETRY (ADPCM). Sub-fetch: 4 sectors = 2352 frames = 53 ms, one round
 * trip. Ring: 26 sub-fetches = 61152 samples per channel = 1.39 s (LEA is 16
 * bits). Lead: 20 sub-fetches. At most 2 sub-fetches per service call, which
 * is the catch-up rate after the title has kept the loader off the ring.
 *
 * CLOCK AND PHASE. TMU1's period comes from TICKS_X8192 and the host's trim (a
 * ppm scale in every audio ReturnValue). The fill trigger reads the model an
 * eighth of a loop late (CDDA_LAG_SHIFT): fetching late only eats into the
 * lead, fetching early overwrites audio that is still playing. So the lead is
 * really 20 sub-fetches less the lag -- 893 ms of audio ahead of the AICA, and
 * 493 ms of already-played ring behind the write head.
 *
 * SILENCE, NEVER GARBAGE. The channels are keyed on only over the lead, laid
 * whole; if the lead falls to within one sub-fetch of nothing, or a service
 * gap was long enough that the (wrapping) model cannot be trusted, they are
 * keyed off and the stream restarts at what was last heard; key-off silences
 * because the release rate is programmed.
 *
 * AGENTS.md 4.13 has the rules, the counters and the history.
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
#include "g2dma.h"
#include "memfuncs.h"

#if WITH_CDDA

/* ------------------------------------------------------------------ AICA */

/* Sound registers and sound RAM, through the uncached G2 window. */
#define SNDREG32(x)        (*(volatile unsigned int *)(0xa0700000 + (x)))
#define CHNREG32(ch, x)    SNDREG32(0x80 * (ch) + (x))
#define AICA_RAM(off)      (*(volatile unsigned int *)(0xa0800000 + (off)))

/* G2 FIFO status: no more than eight 32-bit writes may be queued on G2
 * without waiting for the FIFO to drain; extra writes are lost silently. */
#define G2_FIFO_STATUS     (*(volatile unsigned int *)0xa05f688c)
#define G2_FIFO_AICA       (1u << 0)
#define G2_FIFO_G2         (1u << 4)
#define G2_FIFO_SH4        (1u << 5)
/* Suspend registers of the three G2 DMA channels (SPU, BBA, CH2), KOS g2bus.h. */
/* G2 DMA channel 0 (the AICA) is busy: a title is writing sound RAM. The
 * loader keeps off the AICA meanwhile, as isoldr does. */
#define aica_dma_busy()    (((*(volatile unsigned int *)0xa05f7818) & 1u) != 0u)

/* Every hardware wait is bounded (~2 ms): a FIFO that never drains costs a
 * dropped store, not a hung console. */
#define G2_FIFO_SPIN_LIMIT 200000

/* The AICA's CD input mixer, programmed by the game for the real drive:
 * EFSDL (send level) in bits 8..11. */
#define AICA_CDDA_INPUT_L  0x2040
#define AICA_CDDA_INPUT_R  0x2044

/* ------------------------------------------------------------------ timers */

/*
 * TMU2 is the deadline clock (adapter.h): Pck/4 = 12.5 MHz, free running from
 * 0xffffffff, so `start - TCNT2` is elapsed ticks. TMU1 is the loop clock:
 * Pck/16, TCOR = one AICA loop (end_tm), so `end_tm - TCNT1` is ticks into the
 * loop.
 */
#define TMU_TSTR           (*(volatile unsigned char *)0xffd80004)
#define TMU_TCOR2          (*(volatile unsigned int *)0xffd80020)
#define TMU_TCNT2          TMU2_COUNT       /* adapter.h -- one definition */
#define TMU_TCR2           (*(volatile unsigned short *)0xffd80028)
#define TMU_START_TMU2     0x04
#define TMU_TCOR1          (*(volatile unsigned int *)0xffd80014)
#define TMU_TCNT1          (*(volatile unsigned int *)0xffd80018)
#define TMU_TCR1           (*(volatile unsigned short *)0xffd8001c)
#define TMU_START_TMU1     0x02
#define TMU_TCR_PCK4       0                /* TMU2, UNIE clear */
#define TMU_TCR_PCK16      1                /* TMU1, UNIE clear */

/*
 * Pck/16 ticks per 8192 AICA samples. The console's is isoldr's VA1 constant
 * times 16; the host's trim corrects the rest (~160-185 ppm on the test
 * console). flycast's clocks are exact -- TMU Pck/16 = 3 125 000 Hz, one AICA
 * sample every 4535 SH4 cycles -- which gives 4535 * 128 = 580480; the
 * console's value is 2622 ppm short there, enough to spend the early margin in
 * 66 s. The flycast sets are built with CDDA_TICKS_X8192=580480.
 */
#ifndef CDDA_TICKS_X8192
#define CDDA_TICKS_X8192   578960u        /* 36185 * 16 */
#endif
#define TICKS_X8192        CDDA_TICKS_X8192

/* Ticks per frame in sixteenths: a constant divisor (a variable one pulls in
 * libgcc's divider, AGENTS.md 14.15). */
#define TICKS_PER_FRAME_X16 ((TICKS_X8192 + 256u) / 512u)

/* A key-off needs a moment to take effect (isoldr waits too): 1 ms on TMU2. */
#define KEYOFF_SPIN_TICKS  12500u

/*
 * How late the fill trigger reads the position model, as a right shift of one
 * loop. An eighth (173 ms) is the early margin: it is subtracted from the lead
 * and added to the already-played ring behind the write head, which is what
 * the model being ahead of the AICA would eat into.
 */
#ifndef CDDA_LAG_SHIFT
#define CDDA_LAG_SHIFT     3u
#endif

/* ------------------------------------------------------------------ channels */

#define CDDA_CH_LEFT       62
#define CDDA_CH_RIGHT      63

/* The rings sit just below CDDA_RING_TOP in sound RAM (block 20). The top of
 * sound RAM, isoldr's place, reaches into Snow Surfers' samples at our size. */
#ifndef CDDA_RING_TOP
#define CDDA_RING_TOP      0x00150000
#endif

/* ------------------------------------------------------------------ geometry */

/* One raw CD audio sector is 2352 bytes = 588 stereo frames. */
#define RAW_SECTOR_SIZE    2352u
#define FRAMES_PER_SECTOR  588u

#if CDDA_ADPCM
/* 4-bit Yamaha ADPCM, long-stream mode (the decoder keeps its state across the
 * loop back to the start of the ring). One 32-bit store is eight samples. */
#define AICA_FMT           3
#define FETCH_SECTORS      4u
#define SAMPLE_BYTES(n)    ((n) >> 1)
#define RING_IDLE_BYTE     0x80            /* one step up, one down: quiet floor */
#else
/* 16-bit linear PCM, interleaved L/R from the host. */
#define AICA_FMT           0
#define FETCH_SECTORS      3u
#define SAMPLE_BYTES(n)    ((n) << 1)
#define RING_IDLE_BYTE     0x00
#endif
#define IDLE_WORD          (((unsigned int)RING_IDLE_BYTE) * 0x01010101u)

/* Sub-fetches in the ring: as many as LEA allows, since the ring is the whole
 * margin for the loader not being called in time. */
#ifndef CDDA_RING_FETCHES
#define CDDA_RING_FETCHES 26u
#endif

/*
 * How far ahead of the AICA the write head is kept. What is left of the ring
 * behind it (6 sub-fetches, plus the lag the trigger reads late: 493 ms) is
 * the margin against the model running ahead of the AICA; the lead itself
 * (893 ms) is the margin against the title not calling the loader (305 ms once
 * in Snow Surfers). Both used to be one half, 693 ms, once a cycle.
 */
#ifndef CDDA_LEAD_FETCHES
#define CDDA_LEAD_FETCHES 20u
#endif

/* Sub-fetches one service call may do. Each is a round trip the title waits
 * for (~3 ms). In a steady stream one call in three does one; this is the rate
 * the lead is won back at after the title has kept the loader off the ring. */
#ifndef CDDA_FETCHES_PER_SERVICE
#define CDDA_FETCHES_PER_SERVICE 2u
#endif

/* Sub-fetches between two channel-theft checks (~693 ms, as when a half ended
 * one). */
#define CDDA_CHECK_FETCHES 13u

#define FETCH_FRAMES       (FETCH_SECTORS * FRAMES_PER_SECTOR)
/* One sub-fetch of audio in TMU2 ticks (Pck/4 = 12.5 MHz): 44100 Hz. */
#define CDDA_FETCH_TMU2    (FETCH_FRAMES * 2834u / 10u)
#define RING_SAMPLES       (CDDA_RING_FETCHES * FETCH_FRAMES)   /* per channel */
#define RING_BYTES         SAMPLE_BYTES(RING_SAMPLES)
#define LEAD_FRAMES        (CDDA_LEAD_FETCHES * FETCH_FRAMES)

/* The staging buffer holds one sub-fetch: one byte per frame in ADPCM (two mono
 * blocks), four in PCM (interleaved 16-bit). */
#if CDDA_ADPCM
#define FETCH_BYTES        FETCH_FRAMES
#else
#define FETCH_BYTES        (FETCH_FRAMES * 4u)
#endif

/* Where each channel's ring lives (AICA/ARM view). */
#define AICA_LEFT_BASE     (CDDA_RING_TOP - 2u * RING_BYTES)
#define AICA_RIGHT_BASE    (CDDA_RING_TOP - 1u * RING_BYTES)

#define AICA_PITCH_44100   0

/* Ceiling for the channels' direct-mixer send level (0..0xf, ~3 dB a step).
 * The level used mirrors the game's CD input; see cdda_read_game_level(). */
#ifndef CDDA_DISDL
#define CDDA_DISDL 0xf
#endif

/* Hard left / hard right (KOS calc_aica_pan() for 0 / 255), with the send
 * level from cd.disdl[]. */
#define CDDA_DIPAN(ch)     ((ch) == CDDA_CH_LEFT ? 0x1fu : 0x0fu)
#define CDDA_REG36(ch, disdl) (CDDA_DIPAN(ch) | ((unsigned int)(disdl) << 8))

/* What register 0 reads back once armed: KYONB (14), LPCTL (9), PCMS (8:7) and
 * SA high (6:0). The mask drops the self-clearing strobe (15) and bits 11-13,
 * which read back set on hardware once a channel has run. */
#define CDDA_CTRL(base)    (0x4000u | (AICA_FMT << 7) | 0x200u | ((base) >> 16))
#define CDDA_CTRL_MASK     0x47ffu

/* Audio status reported to the title (SPI 1.30). */
#define SCD_AUDIO_PLAYING  0x11u
#define SCD_AUDIO_PAUSED   0x12u
#define SCD_AUDIO_ENDED    0x13u
#define SCD_AUDIO_NO_INFO  0x15u

/* Repeat: 0x0f is the BIOS "forever". */
#define CDDA_LOOP_FOREVER  0x0fu

/* The adapter loop's seconds timeout: a backstop only (it counts whole seconds
 * on the PMCR). The TMU2 deadlines below are the ones that matter. */
#define CDDA_TIMEOUT_SECONDS       2

/*
 * How long one sub-fetch may take: 20 ms (normal: ~3 ms). The host drops any
 * answer it took longer than CDDA_GIVE_UP (15 ms) to produce, which must stay
 * below this, and answers a re-ask from its cache.
 */
#define CDDA_FETCH_DEADLINE_TICKS  250000u        /* 20 ms at Pck/4 */

/* After a failed sub-fetch, drain for 10 ms so an answer still on its way is
 * consumed (and refused) instead of landing in the next fetch. */
#define CDDA_DRAIN_DEADLINE_TICKS  125000u        /* 10 ms at Pck/4 */
#define CDDA_DRAIN_ITERS           1000000

/* The idle listening window of a service with nothing to fill: at most this
 * many polls and 1 ms, and at most once every 20 ms, which keeps --diag
 * answered without freezing the title on every GD call. 0 removes it. */
#ifndef CDDA_SERVICE_DRAIN_ITERS
#define CDDA_SERVICE_DRAIN_ITERS   256
#endif
#define CDDA_SERVICE_DRAIN_DEADLINE_TICKS 12500u  /* 1 ms */
#define CDDA_LISTEN_PERIOD_TICKS   250000u        /* 20 ms */

#define CDDA_TOC_RETRIES           4
#define CDDA_TOC_DEADLINE_TICKS    6250000u       /* 500 ms */

/* Weighted register mismatches before a channel is judged taken. */
#define CDDA_CH_BAD_LIMIT  2

/* Frames as model ticks (Pck/16). The trim moves the real period by at most
 * 1.5 %, which is nothing against these margins, so the lengths below are the
 * untrimmed ones. */
#define TICKS_OF(frames)   (((frames) * TICKS_PER_FRAME_X16) >> 4)
#define LEAD_TM            TICKS_OF((unsigned int)LEAD_FRAMES)
#define RING_TM            TICKS_OF((unsigned int)RING_SAMPLES)

/* Key off before the AICA comes within one sub-fetch (53 ms) of the write
 * head, that is, of a part of the ring the fill never reached. */
#define CDDA_MUTE_GUARD    TICKS_OF(FETCH_FRAMES)

/*
 * A service gap this long has drained more of the lead than is left: the
 * position model wraps, so past it a stale ring reads exactly like a fresh one
 * (AGENTS.md 4.13) and only TMU2 -- the clock the model is not derived from --
 * can tell. Start over instead. In Pck/4 ticks, hence the * 4.
 */
#define CDDA_GAP_LIMIT_TICKS \
	((LEAD_TM - (RING_TM >> CDDA_LAG_SHIFT) - CDDA_MUTE_GUARD) * 4u)

/* State. */
#define CDDA_STATE_STOPPED 0
#define CDDA_STATE_PLAYING 1
#define CDDA_STATE_PAUSED  2

/* Compile-time geometry checks. */
/* The write head must never catch the play head up from behind. */
typedef char cdda_assert_lead[
	(LEAD_FRAMES + FETCH_FRAMES < RING_SAMPLES) ? 1 : -1];
#if CDDA_ADPCM
/* One store is eight samples, and a sub-fetch is a whole number of stores; the
 * ring must be too, or the wrap would land mid-store. */
typedef char cdda_assert_fetch8[(FETCH_FRAMES % 8u == 0u) ? 1 : -1];
typedef char cdda_assert_ring8[(RING_SAMPLES % 8u == 0u) ? 1 : -1];
#endif
/* LEA (channel register 12) is 16 bits and holds the last sample index. */
typedef char cdda_assert_lea16[(RING_SAMPLES <= 65536u) ? 1 : -1];
/* (e << 4) must stay inside 32 bits: e < end_tm = RING_SAMPLES * ticks/frame. */
typedef char cdda_assert_lba_fits[
	((unsigned int)RING_SAMPLES < (0xffffffffu >> 4) / (TICKS_PER_FRAME_X16 / 16u))
	? 1 : -1];
typedef char cdda_assert_ring_fits[(2u * RING_BYTES < CDDA_RING_TOP) ? 1 : -1];

/* The staging buffer -- one sub-fetch, in .hiram, outside the loader image. */
#if CDDA_ADPCM
/* 32-byte aligned, with room for the DMA's alignment shift: the left block is
 * received at cdda_pcm + a (a < 32, see cdda_push()) and the right one is
 * moved up to 31 bytes further on. */
#define STAGE_BYTES        (FETCH_BYTES + 64u)
#else
#define STAGE_BYTES        FETCH_BYTES
#endif
HIRAM_BUF static unsigned char cdda_pcm[STAGE_BYTES] __attribute__((aligned(32)));

/* The disc's table of contents, cached for the session. */
static unsigned int cdda_toc[102];
static int cdda_toc_valid;

static struct {
	/* --- the stream --- */
	unsigned int state;         /* CDDA_STATE_* */
	unsigned int first_lba;     /* FAD of the start of the play range */
	unsigned int last_lba;      /* FAD one past the end of the play range */
	unsigned int next_lba;      /* FAD of the next sector to fetch */
	unsigned int loop;          /* remaining repeats, 0x0f = forever */
	unsigned int track;         /* current track number, or 0 for sector play */
	unsigned int running;       /* channels keyed on */
	unsigned int priming;       /* cdda_prime() armed: laying the lead, keyed off */
	unsigned int drained;       /* source exhausted, ring playing out */
	unsigned int end_left;      /* Pck/4 ticks of real audio still to be heard */
	unsigned int write_pos;     /* frames: where the next sub-fetch goes */
	unsigned int check_in;      /* sub-fetches until the next channel check */
	unsigned int audio_stat;    /* SCD_AUDIO_* reported to the title */
	unsigned int restart;       /* ADPCM: ask the host to reset its encoder */

	/* --- position model --- */
	unsigned int end_tm;        /* TMU1 reload = one AICA loop, in Pck/16 ticks */
	unsigned int svc_mark;      /* TMU2 at the last service that ran */
	unsigned int svc_marked;    /* that mark is valid */
	unsigned int svc_gap;       /* TMU2 ticks the last service came after */
	unsigned int listen_mark;   /* TMU2 at the last listening window */

	/* --- channels: [0] = left, [1] = right --- */
	unsigned int disdl[2];      /* send level mirrored from the game */
	unsigned int level_seen;    /* the game has set a CD input level */
	unsigned int zero_run[2];   /* consecutive reads of a zero level */
	unsigned int ch_check_ok;   /* the watchdog read back clean at key-on */
	unsigned int ch_bad_run;    /* consecutive checks that found a theft */
	unsigned int need_restream; /* channel stolen: start over */
} cd;

/* The send level for a channel. */
#define CD_DISDL(ch)   (cd.disdl[(ch) == CDDA_CH_RIGHT])

/* ------------------------------------------------------------------ counters */

/* Read by name from the ELF (dcload-ip-rs --diag, scripts/dc-counters.py):
 * renaming one breaks those readers. */
unsigned int g_cdda_plays;          /* PLAY commands accepted */
unsigned int g_cdda_fetches;        /* sub-fetches served */
unsigned int g_cdda_fetch_fails;    /* sub-fetches that failed, each asked again */
unsigned int g_cdda_wrong_lba;      /* answers to another request, refused */
unsigned int g_cdda_retv_nodata;    /* our LBA came back without our data */
unsigned int g_cdda_mutes;          /* keyed off before an unfilled part: a silence */
unsigned int g_cdda_ch_stolen;      /* channels taken by the title, restarted */
unsigned int g_cdda_room_min = 0xffffffffu; /* least lead ever seen, TMU1 ticks */
unsigned int g_cdda_svc_gap_max;    /* longest gap between services, TMU2 ticks */
unsigned int g_cdda_toc_fails;      /* failed DC22 attempts */
unsigned int g_cdda_last_lba;       /* FAD of the last sub-fetch served */
unsigned int g_cdda_end_tm;         /* the loop period in use, TMU1 ticks */
unsigned int g_cdda_scale_ppm = 1000000u; /* the host's trim (1000000 = none) */

/* ------------------------------------------------------------------ G2 lock */

static void g2_fifo_wait(void)
{
	int spin = G2_FIFO_SPIN_LIMIT;

	while ((G2_FIFO_STATUS & (G2_FIFO_G2 | G2_FIFO_AICA)) && spin--)
	{
		/* spin */
	}
}

/*
 * KOS's G2 critical section (g2bus.h): interrupts masked so the CPU is not
 * taken away mid-burst, and the three G2 DMA channels suspended so a title's
 * own DMA does not interleave writes into the same FIFO. The eight-write rule
 * still applies inside it.
 */
static unsigned int g2_lock(void)
{
	unsigned int sr, tmp;

	g2dma_quiesce();		/* no CPU access to G2 over our own DMA */
	__asm__ __volatile__("stc\tsr,%0\n\t"
	                     "mov\t%0,%1\n\t"
	                     "or\t%2,%1\n\t"
	                     "ldc\t%1,sr\n"
	                     : "=&r"(sr), "=&r"(tmp)
	                     : "r"(0xf0u)
	                     : "memory");
	g2dma_hold();		/* nested: the tick may already hold (g2dma.h) */
	return sr;
}

static void g2_unlock(unsigned int sr)
{
	g2dma_release();
	__asm__ __volatile__("ldc\t%0,sr\n" : : "r"(sr) : "memory");
}

/* ------------------------------------------------------------------ clock */

/* The loop period, end_tm, in Pck/16 ticks for this ring, with the host's
 * trim. RING_SAMPLES * TICKS_X8192 / 8192 is split so the product stays in
 * 32 bits, and so is the trim (tm ~4.3 million, deviation at most 15000 ppm). */
static void cdda_timer_calibrate(void)
{
	unsigned int tm = (TICKS_X8192 >> 13) * (unsigned int)RING_SAMPLES
	                + (((TICKS_X8192 & 8191u) * (unsigned int)RING_SAMPLES) >> 13);
	unsigned int up = (g_cdda_scale_ppm > 1000000u);
	unsigned int d = up ? g_cdda_scale_ppm - 1000000u : 1000000u - g_cdda_scale_ppm;
	unsigned int adj = (tm * (d / 100u)) / 10000u + (tm * (d % 100u)) / 1000000u;

	tm = up ? tm + adj : tm - adj;
	cd.end_tm = tm;
	g_cdda_end_tm = tm;
}

/*
 * The host's clock trim, in the `size` field of the ReturnValue that ends an
 * audio fetch: an absolute scale on the compiled-in period (1000000 = none).
 * Values outside +/-1.5 % are a broken measurement (or a 0 / 0xffffffff) and
 * are ignored. TMU1 picks the new TCOR up at its next reload.
 */
static void cdda_scale_from_host(unsigned int ppm)
{
	if (ppm < 985000u || ppm > 1015000u || ppm == g_cdda_scale_ppm)
	{
		return;
	}
	g_cdda_scale_ppm = ppm;
	cdda_timer_calibrate();
	if (cd.running)
	{
		TMU_TCOR1 = cd.end_tm;
	}
}

/* The deadline clock is started at boot now and owned by cdfs_syscalls.c, so
 * that a title which never plays CD-DA still gets a working read deadline --
 * which is what Crazy Taxi did not have (AGENTS.md 4.5). Kept as a call rather
 * than dropped: these three sites are the ones that must not run before it. */
#define cdda_deadline_timer_start() gd_deadline_timer_start()

/* Start TMU1 as the loop clock, in the key-on critical section. The first
 * count starts one lag above TCOR, which cdda_elapsed() reads as "the loop has
 * not begun yet": that is how the lag is applied. */
static void cdda_loop_timer_start(void)
{
	cdda_timer_calibrate();
	TMU_TSTR = (unsigned char)(TMU_TSTR & ~TMU_START_TMU1);
	TMU_TCR1 = TMU_TCR_PCK16;
	TMU_TCOR1 = cd.end_tm;
	TMU_TCNT1 = cd.end_tm + (cd.end_tm >> CDDA_LAG_SHIFT);
	TMU_TSTR = (unsigned char)(TMU_TSTR | TMU_START_TMU1);
}

/* Model ticks since the loop last wrapped, 0 .. end_tm - 1: the LAGGED model,
 * the refill trigger. Above TCOR (the head start, or briefly after a trim
 * shortened TCOR) the loop has not started. */
static unsigned int cdda_elapsed(void)
{
	unsigned int t = TMU_TCNT1;

	if (t > cd.end_tm)
	{
		return 0u;
	}
	return (t != 0u) ? cd.end_tm - t : cd.end_tm - 1u;
}

/* Where the AICA really is: the lagged model plus the lag. During the head
 * start, the time since key-on. Everything that measures uses this. */
static unsigned int cdda_true_elapsed(void)
{
	unsigned int lag = cd.end_tm >> CDDA_LAG_SHIFT;
	unsigned int t = TMU_TCNT1;
	unsigned int e;

	if (t > cd.end_tm)
	{
		t -= cd.end_tm;
		return (t < lag) ? lag - t : 0u;
	}
	e = ((t != 0u) ? cd.end_tm - t : cd.end_tm - 1u) + lag;
	return (e >= cd.end_tm) ? e - cd.end_tm : e;
}

/*
 * Ticks of audio between the AICA and the write head, as model `e` sees it:
 * the LAGGED model for the fill trigger (fetching early is the direction that
 * overwrites audio still playing), the TRUE one for everything that measures.
 *
 * It is a difference modulo one loop, so it cannot see a lead that has gone
 * past zero: a whole loop late reads as a full ring. The service gap on TMU2
 * (CDDA_GAP_LIMIT_TICKS) is what covers that.
 */
static unsigned int cdda_lead(unsigned int e)
{
	unsigned int w = TICKS_OF(cd.write_pos);

	if (w >= cd.end_tm)
	{
		w = cd.end_tm - 1u;       /* the trim can shorten the loop under it */
	}
	return (w >= e) ? w - e : w + cd.end_tm - e;
}

/* TMU1 ticks of audio as whole sectors, for the reported play position. */
static unsigned int cdda_ticks_sectors(unsigned int tm)
{
	return ((tm << 4) / TICKS_PER_FRAME_X16) / FRAMES_PER_SECTOR;
}

/* ------------------------------------------------------------ channels */

static void aica_channel_off(unsigned int ch)
{
	unsigned int sr = g2_lock();
	unsigned int val = CHNREG32(ch, 0);

	g2_fifo_wait();
	/* KEY OFF: drop bit 14 (key-on request), keep the execute strobe. */
	CHNREG32(ch, 0) = (val & ~0x4000u) | 0x8000u;
	g2_unlock(sr);
}

/*
 * Program a channel completely but do not start it: register 0 is written with
 * KYONB set and KYONEX clear, and cdda_channels_start() strobes both channels
 * so both ears start on the same sample. The caller holds the G2 lock.
 * TL (byte 41) is ATTENUATION (0 = full scale). RR = 0x1f makes a key-off
 * silence at once (RR = 0 holds the level). The title may have used these
 * channels, so every field we rely on is written.
 */
static void aica_channel_arm(unsigned int ch, unsigned int base)
{
	CHNREG32(ch, 40) = 0x24u;                        /* Q: LPF off, TL: 0 */
	CHNREG32(ch, 36) = CDDA_REG36(ch, CD_DISDL(ch)); /* DIPAN | DISDL */
	g2_fifo_wait();
	CHNREG32(ch, 20) = 0x1fu;                        /* RR fastest */
	CHNREG32(ch, 28) = 0;                            /* LFO off */
	CHNREG32(ch, 32) = 0;                            /* no DSP send */
	CHNREG32(ch, 8) = 0;                             /* loop start */
	CHNREG32(ch, 12) = (RING_SAMPLES - 1u) & 0xffffu;/* loop end, in samples */
	CHNREG32(ch, 16) = 0x1f;                         /* AEG: no envelope */
	CHNREG32(ch, 24) = AICA_PITCH_44100;
	CHNREG32(ch, 4) = base & 0xffffu;
	g2_fifo_wait();
	CHNREG32(ch, 0) = CDDA_CTRL(base);               /* no KYONEX yet */
}

/*
 * A title's sound driver may take channels 62/63 (isoldr's aica_check_cdda
 * does the same check). Structural mismatches -- control word, sample address,
 * loop, pitch -- register 0 counting double. An all-zero control word is a
 * failed read (AICA reads from the SH4 sometimes return 0), not a theft.
 */
static int aica_channel_bad(unsigned int ch, unsigned int base)
{
	unsigned int sr = g2_lock();
	unsigned int ctrl;
	int bad = 0;

	g2_fifo_wait();
	ctrl = CHNREG32(ch, 0) & CDDA_CTRL_MASK;
	if (ctrl != 0u)
	{
		bad += (ctrl != (CDDA_CTRL(base) & CDDA_CTRL_MASK)) ? 2 : 0;
		bad += ((CHNREG32(ch, 4) & 0xffffu) != (base & 0xffffu));
		bad += ((CHNREG32(ch, 8) & 0xffffu) != 0u);
		bad += ((CHNREG32(ch, 12) & 0xffffu) != ((RING_SAMPLES - 1u) & 0xffffu));
		bad += ((CHNREG32(ch, 24) & 0xffffu) != AICA_PITCH_44100);
	}
	g2_unlock(sr);
	return bad;
}

/* The output group (pan/send, filter/attenuation) can be rewritten mid-stream
 * without a glitch; the envelope, LFO and DSP send go with it. */
static void aica_channel_mix(unsigned int ch)
{
	unsigned int sr = g2_lock();

	if ((CHNREG32(ch, 36) & 0xffffu) != CDDA_REG36(ch, CD_DISDL(ch))
	    || (CHNREG32(ch, 40) & 0xffffu) != 0x24u)
	{
		g2_fifo_wait();
		CHNREG32(ch, 20) = 0x1fu;
		CHNREG32(ch, 28) = 0;
		CHNREG32(ch, 32) = 0;
		CHNREG32(ch, 40) = 0x24u;
		CHNREG32(ch, 36) = CDDA_REG36(ch, CD_DISDL(ch));
	}
	g2_unlock(sr);
}

static void cdda_read_game_level(int at_key_on);

/* Every CDDA_CHECK_FETCHES sub-fetches: a theft confirmed twice in a row
 * restarts the stream (a bad read does not persist); a drifted mix is
 * rewritten. Disabled
 * for this key-on if the check did not read back clean right after it, or it
 * would restart forever. */
static void cdda_check_channels(void)
{
	if (!cd.ch_check_ok)
	{
		return;
	}
	if (aica_channel_bad(CDDA_CH_LEFT, AICA_LEFT_BASE) >= CDDA_CH_BAD_LIMIT
	    || aica_channel_bad(CDDA_CH_RIGHT, AICA_RIGHT_BASE) >= CDDA_CH_BAD_LIMIT)
	{
		if (++cd.ch_bad_run >= 2u)
		{
			cd.ch_bad_run = 0;
			g_cdda_ch_stolen++;
			cd.need_restream = 1;
		}
		return;
	}
	cd.ch_bad_run = 0;
	cdda_read_game_level(0);
	aica_channel_mix(CDDA_CH_LEFT);
	aica_channel_mix(CDDA_CH_RIGHT);
}

/*
 * Real CD-DA enters the AICA through its CD input, whose level the game sets in
 * 0x2040/0x2044. Mirror the game's EFSDL as our send level (clamped to
 * CDDA_DISDL), so a title that turns its CD input down does not get
 * full-scale music summing into clipping.
 *
 * FOLLOWED, NOT SAMPLED, AND 0 IS A LEVEL (2026-09-27). This read the level
 * at key-on only and took 0 for "never set" = full. A title that fades its
 * music by lowering the CD input and starts the next track from 0 then got
 * the next track at full volume, and a change between two key-ons was
 * undone by the channel watchdog -- Sega Rally 2 under Windows CE: "the
 * volume gets reset to the maximum at some random time". Now the watchdog
 * re-reads it every CDDA_CHECK_FETCHES sub-fetches, and 0 means full only
 * until the game has set any level at all. A zero must be read twice in a
 * row before it mutes: AICA reads from the SH4 sometimes return 0 (§4.13
 * rule 8). The master volume is raised only at key-on and only while the
 * game has set nothing, i.e. before its sound driver is up.
 */
static void cdda_read_game_level(int at_key_on)
{
	unsigned int sr = g2_lock();
	unsigned int mvol = SNDREG32(0x2800);
	unsigned int in[2];
	unsigned int i;

	if (at_key_on && !cd.level_seen && ((mvol & 0x0fu) == 0u))
	{
		g2_fifo_wait();
		SNDREG32(0x2800) = mvol | 0x0fu;
	}
	in[0] = SNDREG32(AICA_CDDA_INPUT_L);
	in[1] = SNDREG32(AICA_CDDA_INPUT_R);
	g2_unlock(sr);

	for (i = 0; i < 2u; i++)
	{
		unsigned int dl = (in[i] >> 8) & 0x0fu;

		if (dl != 0u)
		{
			cd.level_seen = 1;
			cd.zero_run[i] = 0;
		}
		else if (!cd.level_seen)
		{
			dl = 0x0fu;
		}
		else if (++cd.zero_run[i] < 2u)
		{
			continue;	/* keep the level until a second zero */
		}
		cd.disdl[i] = (dl > (unsigned int)CDDA_DISDL) ? (unsigned int)CDDA_DISDL : dl;
	}
}

#if CDDA_ADPCM
/*
 * G2 DMA WRITES OF THE RING (docs/g2-dma-investigation.md, step 3).
 *
 * The bus moves ~7.5 MB/s to the AICA either way (measured on the console: CPU
 * 336 us, DMA 314 us for 2368 bytes, every channel, no wrong word), so the
 * gain is not the transfer's length but the CPU: a DMA left running lets the
 * title execute meanwhile. It needs 32-byte alignment of both ends and a
 * length in 32s, and a sub-fetch is 1176 bytes at ring offsets that are not:
 * so the (at most 28-byte) unaligned edges are written by the CPU, before the
 * DMA starts, and only the aligned body goes by DMA. Nothing outside the
 * range is touched -- no carried-over bytes, no spill over the ring's end.
 *
 * ORDER, because the CPU may not use G2 while one of our DMAs does
 * (g2dma.h): the edges are written under g2_lock() (which first waits for the
 * previous DMAs), then both channels start and the caller returns. The next
 * use of the staging buffer (cdda_fetch), of G2 (g2_lock) or of the adapter
 * (g2dma_quiesce in the drivers) waits for them first.
 */
/* n bytes from `l` and `r` (each already congruent to its destination modulo
 * 32) to both rings at ring offset `off`; left running. The source lines must
 * be in RAM. Per ring: the head up to the first 32-byte boundary and the tail
 * past the last one by the CPU, then the body by DMA. */
static void cdda_put2(unsigned int off, const unsigned char *l, const unsigned char *r,
                      unsigned int n)
{
	unsigned int d[2], h[2], b[2], k, i, w = 0;
	const unsigned char *s[2];
	unsigned int sr;

	/* n is at least two sectors' worth (588 bytes), so a head and a body always
	 * exist; anything shorter is a bug elsewhere and is not written. */
	if (n < 64u)
	{
		return;
	}
	d[0] = AICA_LEFT_BASE + off;
	d[1] = AICA_RIGHT_BASE + off;
	s[0] = l;
	s[1] = r;
	sr = g2_lock();
	for (k = 0; k < 2u; k++)
	{
		h[k] = (0u - d[k]) & 31u;
		b[k] = (n - h[k]) & ~31u;
		for (i = 0; i < n; i += 4u)
		{
			if (i == h[k])
			{
				i += b[k];
				if (i >= n)
				{
					break;
				}
			}
			if ((w++ & 3u) == 0u)
			{
				g2_fifo_wait();
			}
			AICA_RAM(d[k] + i) = *(const unsigned int *)(const void *)(s[k] + i);
		}
	}
	g2_unlock(sr);
	for (k = 0; k < 2u; k++)
	{
		g2dma_start(G2DMA_CDDA_L + k, s[k] + h[k], 0x00800000u + d[k] + h[k],
		            b[k], G2DMA_TO_G2);
	}
}

/* Write the quiet floor over `bytes` bytes of both rings from byte `off`, so
 * audio that has not arrived plays as silence. The staging buffer is the
 * source, filled with the idle byte; a chunk is at most half a sub-fetch, so
 * that its shift by the destination's alignment stays inside the buffer. Left
 * running. */
static void cdda_floor(unsigned int off, unsigned int bytes)
{
	unsigned int *w = (unsigned int *)(void *)cdda_pcm;
	unsigned int i;

	g2dma_quiesce();
	for (i = 0; i < STAGE_BYTES / 4u; i++)
	{
		w[i] = IDLE_WORD;
	}
	CacheBlockWriteBack(cdda_pcm, (STAGE_BYTES + 31u) / 32u);
	/* At most half a sub-fetch: the drain's cell. */
	cdda_put2(off, cdda_pcm + ((AICA_LEFT_BASE + off) & 31u),
	          cdda_pcm + ((AICA_RIGHT_BASE + off) & 31u), bytes);
}
#else
/* Write the quiet floor over `bytes` bytes of both rings from byte `off`, so
 * audio that has not arrived plays as silence. 256 bytes per lock. */
static void cdda_floor(unsigned int off, unsigned int bytes)
{
	unsigned int end = off + bytes;

	while (off < end)
	{
		unsigned int sr = g2_lock();
		unsigned int k;

		for (k = 0; k < 64u && off < end; k++, off += 4u)
		{
			if ((k & 3u) == 0u)
			{
				g2_fifo_wait();
			}
			AICA_RAM(AICA_LEFT_BASE + off) = IDLE_WORD;
			AICA_RAM(AICA_RIGHT_BASE + off) = IDLE_WORD;
		}
		g2_unlock(sr);
	}
}
#endif

/* The same, `frames` frames from ring position `pos`, wrapping at the end. */
static void cdda_floor_frames(unsigned int pos, unsigned int frames)
{
	unsigned int n = (pos + frames > (unsigned int)RING_SAMPLES)
		? (unsigned int)RING_SAMPLES - pos : frames;

	cdda_floor(SAMPLE_BYTES(pos), SAMPLE_BYTES(n));
	if (n != frames)
	{
		cdda_floor(0, SAMPLE_BYTES(frames - n));
	}
}

/* Key both channels on and start TMU1 in one critical section: if the title's
 * interrupts ran in between, the ears would start apart, and the timer's origin
 * must be the strobe (isoldr's aica_setup_cdda does the same). KYONEX is a
 * global strobe; both channels are strobed anyway. */
static void cdda_channels_start(void)
{
	unsigned int sr;

	cdda_read_game_level(1);
	cdda_deadline_timer_start();
	cd.need_restream = 0;
	cd.ch_bad_run = 0;
	cd.check_in = CDDA_CHECK_FETCHES;
	cd.svc_marked = 0;          /* the drain countdown starts at the strobe */

	sr = g2_lock();
	aica_channel_arm(CDDA_CH_LEFT, AICA_LEFT_BASE);
	aica_channel_arm(CDDA_CH_RIGHT, AICA_RIGHT_BASE);
	g2_fifo_wait();
	CHNREG32(CDDA_CH_LEFT, 0) = 0x8000u | CDDA_CTRL(AICA_LEFT_BASE);
	CHNREG32(CDDA_CH_RIGHT, 0) = 0x8000u | CDDA_CTRL(AICA_RIGHT_BASE);
	cdda_loop_timer_start();
	g2_unlock(sr);

	g2_fifo_wait();
	cd.ch_check_ok = (aica_channel_bad(CDDA_CH_LEFT, AICA_LEFT_BASE) == 0
	                  && aica_channel_bad(CDDA_CH_RIGHT, AICA_RIGHT_BASE) == 0);
	cd.running = 1;
}

static void cdda_channels_stop(void)
{
	unsigned int t0;

	if (!cd.running)
	{
		return;
	}
	aica_channel_off(CDDA_CH_LEFT);
	aica_channel_off(CDDA_CH_RIGHT);
	g2_fifo_wait();
	t0 = TMU_TCNT2;
	while (tmu2_since(t0) < KEYOFF_SPIN_TICKS)
	{
		/* spin */
	}
	TMU_TSTR = (unsigned char)(TMU_TSTR & ~TMU_START_TMU1);
	cd.running = 0;
}

/* ------------------------------------------------------------------ TOC */

/* Ask the host for the whole disc's table of contents (DC22, area 2), once per
 * session. Runs before any key-on, so it starts TMU2 itself. */
static int cdda_load_toc(void)
{
	command_3int_t *command =
		(command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
	unsigned int try;

	if (cdda_toc_valid)
	{
		return 0;
	}
	cdda_deadline_timer_start();
	cdda_timer_calibrate();

	for (try = 0; try < CDDA_TOC_RETRIES; try++)
	{
		memcpy(command->id, CMD_CDFSTOC, 4);
		command->value0 = htonl(2);                     /* whole disc */
		command->value1 = htonl((unsigned int)cdda_toc);
		command->value2 = 0;

		syscall_retval = (unsigned int)-1;
		timeout_loop = CDDA_TIMEOUT_SECONDS;
		fine_deadline_start = TMU_TCNT2;
		fine_deadline_ticks = CDDA_TOC_DEADLINE_TICKS;
		build_send_packet(sizeof(command_3int_t));
		bb->loop(0);
		fine_deadline_ticks = 0;
		timeout_loop = 0;

		if ((int)syscall_retval >= 0)
		{
			cdda_toc_valid = 1;
			return 0;
		}
		g_cdda_toc_fails++;
	}
	return -1;
}

/*
 * TOC layout, as the host's build_dc_toc writes it (KOS cd_toc_t): entries
 * 0..98 are tracks 1..99 (lba | adr<<24 | ctrl<<28, absent = 0xffffffff),
 * entry 99 = first track number, 100 = last, 101 = lead-out.
 */
#define toc_track_lba(t)   (cdda_toc[(t) - 1] & 0x00ffffffu)
#define toc_first_track()  ((cdda_toc[99] >> 16) & 0xffu)
#define toc_last_track()   ((cdda_toc[100] >> 16) & 0xffu)
#define toc_leadout_lba()  (cdda_toc[101] & 0x00ffffffu)

/* Track containing a given FAD, or 0. Only used to report a track number. */
static unsigned int cdda_find_track(unsigned int lba)
{
	unsigned int first = toc_first_track();
	unsigned int last = toc_last_track();
	unsigned int t;

	if (first < 1u || last > 99u || first > last)
	{
		return 0;
	}
	for (t = first; t <= last; t++)
	{
		unsigned int e;

		if (cdda_toc[t - 1] == 0xffffffffu)
		{
			continue;
		}
		e = (t < last && cdda_toc[t] != 0xffffffffu)
			? toc_track_lba(t + 1) : toc_leadout_lba();
		if (lba >= toc_track_lba(t) && lba < e)
		{
			return t;
		}
	}
	return 0;
}

/* ------------------------------------------------------------ one sub-fetch */

/* cdda_fetch()'s exchange, run through gd_exchange(): the request is in
 * pkt_buf and the door is open. */
static void cdda_exchange(void)
{
	build_send_packet(sizeof(command_3int_t));
	bb->loop(0);
	/* Shut the door as soon as the wait ends: the extra wait, the drain, the
	 * listening window and later disc reads then discard stragglers. */
	g_bin_stage_want = 0;
	bin_complete_escape(0);

	/* The wait usually ends on the last part (cmd_partbin() sets 0 when the
	 * window completes) with the ReturnValue processed in the same pass, but
	 * not always: if the window is complete and the echo is not in yet, wait
	 * for it under the same deadline. */
	if ((timeout_loop >= 0) && (syscall_retval == 0u) && bin_window_complete())
	{
		syscall_retval = (unsigned int)-1;
		bb->loop(0);
	}
}

/*
 * Fetch `sectors` audio sectors starting at `lba` into the staging buffer.
 * Returns 0 on success, -1 if the answer was late, refused, incomplete or not
 * ours; the caller then leaves next_lba alone and asks again later.
 *
 * The answer comes as a LoadBinary window (whose echo is not sent: the host
 * does not wait for it, and it would collide with the burst), its parts, and a
 * ReturnValue whose address echoes the LBA served. Answers come without
 * acknowledgement round trips, so a late answer to an abandoned sub-fetch can
 * meet a later one, naming the same buffer and size. So:
 *
 *  - the window is CLOSED before the request, and a fetch is complete only on
 *    its own window. The previous answer's window is still installed, already
 *    complete: when an answer's LoadBinary and parts were lost and only its
 *    ReturnValue arrived, the fetch used to pass on the old map and push the
 *    previous sub-fetch again, which in ADPCM is heard for seconds (found
 *    2026-09-19 in a recording). g_cdda_retv_nodata counts it now;
 *  - the ReturnValue must echo this LBA (g_cdda_wrong_lba);
 *  - cmd_loadbin() refuses any LoadBinary into the staging buffer while no
 *    fetch waits for one (the "door", g_bin_stage_*);
 *  - after a failure, a drain consumes whatever is still on its way.
 */
static int cdda_fetch(unsigned int lba, unsigned int sectors, unsigned int stage_off)
{
	command_3int_t *command =
		(command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
	int timed_out;
	unsigned char *stage = cdda_pcm + stage_off;

	g2dma_quiesce();	/* the last push's DMA reads this buffer */
#if CDDA_ADPCM
	/* Bit 31 resets the host's encoder: set on the first fetch after a key-on,
	 * the only thing that resets the AICA's decoder. */
	memcpy(command->id, CMD_CDDAREAD_ADPCM, 4);
	command->value2 = htonl((sectors * FRAMES_PER_SECTOR)
	                        | (cd.restart ? 0x80000000u : 0u));
#else
	memcpy(command->id, CMD_CDDAREAD, 4);
	command->value2 = htonl(sectors * RAW_SECTOR_SIZE);
#endif
	command->value0 = htonl(lba);
	command->value1 = htonl((unsigned int)stage);

	syscall_retval = (unsigned int)-1;
	timeout_loop = CDDA_TIMEOUT_SECONDS;
	fine_deadline_start = TMU_TCNT2;
	fine_deadline_ticks = CDDA_FETCH_DEADLINE_TICKS;
	/* Open the door for this answer only. */
	g_bin_stage_lo = (unsigned int)cdda_pcm;
	g_bin_stage_hi = (unsigned int)cdda_pcm + STAGE_BYTES;
	g_bin_stage_want = (unsigned int)stage;
	bin_window_close();
	bin_echo_suppress(1);
	bin_complete_escape(1);
	/* Without a thread switch, off the title's stack (cdfs_syscalls.c). */
	gd_exchange(cdda_exchange);
	bin_echo_suppress(0);
	fine_deadline_ticks = 0;
	timed_out = (timeout_loop < 0);
	timeout_loop = 0;

	if (!timed_out && (int)syscall_retval >= 0 && syscall_retval != lba)
	{
		/* Somebody else's answer: a straggler, or the ReturnValue of a disc
		 * read (GD_READ_TAG | its LBA) or TOC request (address 0). Fail, and drain. */
		g_cdda_wrong_lba++;
		syscall_retval = (unsigned int)-1;
		timed_out = 1;
	}

	if ((int)syscall_retval < 0)
	{
		g_cdda_fetch_fails++;
		if (timed_out)
		{
			bin_window_close();
			fine_deadline_start = TMU_TCNT2;
			fine_deadline_ticks = CDDA_DRAIN_DEADLINE_TICKS;
			drain_iters = CDDA_DRAIN_ITERS;
			bin_echo_suppress(1);
			bb->loop(0);
			bin_echo_suppress(0);
			drain_iters = 0;
			fine_deadline_ticks = 0;
			timeout_loop = 0;
		}
		return -1;
	}
	if (!bin_window_complete())
	{
		g_cdda_retv_nodata++;
		g_cdda_fetch_fails++;
		return -1;
	}
	cdda_scale_from_host(syscall_retsize);   /* the trim rides in `size` */
	g_cdda_last_lba = lba;
	cd.restart = 0;
	return 0;
}

#if CDDA_ADPCM
#define CDDA_STAGE_OFF(pos) cdda_stage_off(pos)
/* Where in the staging buffer a sub-fetch written at write head `pos` is
 * received: congruent to its left destination modulo 32. */
static unsigned int cdda_stage_off(unsigned int pos)
{
	return (AICA_LEFT_BASE + SAMPLE_BYTES(pos)) & 31u;
}

/*
 * The host sends the left block at `stage`, then the right one right after it,
 * at frames / 2 (not the middle of the buffer: a short sub-fetch at the end of
 * a range would otherwise leave the right ear stale bytes and a
 * desynchronised decoder at every loop of a repeating track). The right block
 * is then moved forward, at most 31 bytes, to where it is congruent to the
 * right ring's destination: a CPU copy in RAM of ~1 KB, and the lines written
 * back for the DMA.
 */
static void cdda_push(unsigned int base_sample, unsigned int frames)
{
	unsigned int off = SAMPLE_BYTES(base_sample);
	unsigned int n = SAMPLE_BYTES(frames);
	unsigned char *l = cdda_pcm + cdda_stage_off(base_sample);
	unsigned char *r = l + n;
	unsigned int shift = ((AICA_RIGHT_BASE + off) - (unsigned int)r) & 31u;
	unsigned int *d = (unsigned int *)(void *)(r + shift + n);
	const unsigned int *e = (const unsigned int *)(const void *)(r + n);

	while (e > (const unsigned int *)(const void *)r)
	{
		*--d = *--e;
	}
	/* Every line of the buffer: the left ones are clean already. */
	CacheBlockWriteBack(cdda_pcm, (STAGE_BYTES + 31u) / 32u);
	cdda_put2(off, l, r + shift, n);
}
#else
#define CDDA_STAGE_OFF(pos) 0u
/*
 * Copy the staging buffer into both rings at sample `base_sample`, one 32-bit
 * store per channel per step: two interleaved frames in PCM. A FIFO wait every
 * four steps keeps to the eight-write rule. A sub-fetch is truncated at the
 * end of the ring, so nothing wraps.
 */
static void cdda_push(unsigned int base_sample, unsigned int frames)
{
	unsigned int sr = g2_lock();
	unsigned int off = SAMPLE_BYTES(base_sample);
	unsigned int end = off + SAMPLE_BYTES(frames);
	unsigned int k;
	const unsigned int *f = (const unsigned int *)(const void *)cdda_pcm;

	for (k = 0; off < end; k++, off += 4u)
	{
		unsigned int f0 = *f++;
		unsigned int f1 = *f++;

		if ((k & 3u) == 0u)
		{
			g2_fifo_wait();
		}
		AICA_RAM(AICA_LEFT_BASE + off) = (f0 & 0xffffu) | (f1 << 16);
		AICA_RAM(AICA_RIGHT_BASE + off) = (f0 >> 16) | (f1 & 0xffff0000u);
	}
	g2_unlock(sr);
}
#endif

/* ------------------------------------------------------------ fill the ring */

/*
 * How many sectors to fetch next: at most one sub-fetch, no more than what is
 * left before the ring wraps and of the play range. At the end of the range, a repeat
 * rewinds to first_lba (0x0f forever, 1..0x0e counted down). Returns 0 when the
 * range has ended.
 */
static unsigned int cdda_next_sectors(unsigned int want_frames)
{
	unsigned int sectors;

#if CDDA_ADPCM
	/* A store is eight samples, so ADPCM fetches go in pairs of sectors
	 * (2 * 588 = 147 * 8). A lone final sector is skipped, ~13 ms. It used to
	 * round down to 0, which read as the end of the range BEFORE the repeat
	 * below was reached: an odd-length track never looped (6 of Snow Surfers'
	 * 14 audio tracks), the ring played out and the level went silent. */
	if (cd.last_lba - cd.next_lba == 1u)
	{
		cd.next_lba = cd.last_lba;
	}
#endif
	if (cd.next_lba >= cd.last_lba)
	{
		if (cd.loop == 0u)
		{
			return 0;
		}
		if (cd.loop != CDDA_LOOP_FOREVER)
		{
			cd.loop--;
		}
		cd.next_lba = cd.first_lba;
	}
	sectors = cd.last_lba - cd.next_lba;
	if (sectors > FETCH_SECTORS)
	{
		sectors = FETCH_SECTORS;
	}
	if (sectors * FRAMES_PER_SECTOR > want_frames)
	{
		sectors = want_frames / FRAMES_PER_SECTOR;
	}
#if CDDA_ADPCM
	sectors &= ~1u;     /* pairs of sectors, see above */
#endif
	return sectors;
}

/*
 * The source has run out: note how much real audio is still ahead of the AICA,
 * counted down afterwards on TMU2, and let the fill carry on laying silence so
 * the AICA can never reach the ring as it was a loop ago.
 */
static void cdda_drain(void)
{
	unsigned int ahead = cd.priming ? TICKS_OF(cd.write_pos)
	                                : cdda_lead(cdda_true_elapsed());

	cd.end_left = ahead << 2;         /* Pck/16 -> Pck/4 */
	cd.drained = 1;
}

/* Is another sub-fetch due? While priming there is no play position yet: the
 * lead is simply laid from the start of the ring. */
static int cdda_fill_due(void)
{
	if (cd.priming)
	{
		return cd.write_pos < (unsigned int)LEAD_FRAMES;
	}
	return cdda_lead(cdda_elapsed()) < LEAD_TM;
}

/*
 * Fetch what the lead is short of, at most CDDA_FETCHES_PER_SERVICE sub-fetches
 * (each is a round trip the title is frozen for). Returns non-zero if anything
 * was written.
 *
 * A failed sub-fetch leaves next_lba and write_pos alone, so a later call asks
 * for the same sectors (the host answers a re-ask with the same bytes); so does
 * a title DMA into sound RAM that began while we were on the network.
 */
/* Set around cdda_service_between_chunks() and cdda_service_tick(): no
 * listening window; and, from the tick only, one sub-fetch per call. */
static unsigned int svc_no_listen, svc_one;

static unsigned int cdda_fill(void)
{
	unsigned int budget = CDDA_FETCHES_PER_SERVICE;
	unsigned int did = 0;

	/*
	 * CATCH UP BY WHAT THE GAP COST, NOT BY A FIXED TWO.
	 *
	 * Two sub-fetches a call assumes a call every frame, which a Katana
	 * title's GetDrvStat gives. Windows CE never calls GetDrvStat: the
	 * services come from ExecServer, when CE happens to use the drive,
	 * hundreds of ms apart. Two sub-fetches (107 ms) per call then fell
	 * behind the AICA, the lead ran out, and the model -- modulo one loop --
	 * went on reading "full" while the AICA replayed the ring: recorded on
	 * Sega Rally 2's menu (2026-09-27) as 1.386 s of track 8 repeated with the
	 * start advancing 0.1135 s each loop (docs/wince-investigation.md 7t).
	 * Budget the sub-fetches the gap consumed, plus the usual two, up to the
	 * whole lead. The gap limit (CDDA_GAP_LIMIT_TICKS) still restarts the
	 * stream if a gap outran the lead itself.
	 */
	if (cd.svc_gap > CDDA_FETCH_TMU2)
	{
		unsigned int catch_up = cd.svc_gap / CDDA_FETCH_TMU2 + budget;

		budget = (catch_up > (unsigned int)CDDA_LEAD_FETCHES)
			 ? (unsigned int)CDDA_LEAD_FETCHES : catch_up;
	}

	if (svc_one)
	{
		budget = 1u;	/* from the interrupt hook: called every few ms */
	}
	while (budget != 0u && cdda_fill_due())
	{
		unsigned int sectors =
			cdda_next_sectors((unsigned int)RING_SAMPLES - cd.write_pos);
		unsigned int frames;

		if (sectors == 0u)
		{
			if (!cd.drained)
			{
				cdda_drain();
			}
			frames = FETCH_FRAMES;
			cdda_floor_frames(cd.write_pos, frames);
		}
		else
		{
			if (cdda_fetch(cd.next_lba, sectors, CDDA_STAGE_OFF(cd.write_pos)) < 0
			    || aica_dma_busy())
			{
				break;
			}
			frames = sectors * FRAMES_PER_SECTOR;
			cdda_push(cd.write_pos, frames);
			cd.next_lba += sectors;
			g_cdda_fetches++;
		}
		cd.write_pos += frames;
		if (cd.write_pos >= (unsigned int)RING_SAMPLES)
		{
			cd.write_pos -= (unsigned int)RING_SAMPLES;   /* the floor may wrap */
		}
		budget--;
		did = 1;
		if (cd.running && cd.check_in != 0u && --cd.check_in == 0u)
		{
			cd.check_in = CDDA_CHECK_FETCHES;
			cdda_check_channels();
		}
	}
	return did;
}

/* ------------------------------------------------------------ start / restart */

/*
 * Start (or restart) the stream at cd.next_lba: key off, lay the quiet floor
 * over the whole ring, and arm the fill. Used by PLAY, RELEASE, SEEK, the
 * theft repair and the overrun mute. The services lay the lead like any other
 * audio and key on only once it is whole (cdda_prime_step), so the channels
 * never start over a hole.
 */
static void cdda_prime(void)
{
	cd.write_pos = 0;
	cd.drained = 0;
	cd.end_left = 0;
	cd.svc_marked = 0;
	cd.restart = 1;
	cdda_channels_stop();
	cdda_deadline_timer_start();
#if !CDDA_ADPCM
	cdda_floor(0, RING_BYTES);
#endif
	/* ADPCM: no floor over the ring. The lead is laid, with audio or with the
	 * floor (cdda_fill's drain), before the channels key on, and what follows is
	 * laid before the AICA gets there -- the mute covers a service that is late.
	 * It was the whole ring, 61 KB of CPU writes: ~9 ms with the title frozen. */
	cd.priming = 1;
}

static void cdda_prime_step(void)
{
	cdda_fill();
	/* A range shorter than the lead has nothing left to lay: key on anyway. */
	if (cd.write_pos < (unsigned int)LEAD_FRAMES && !cd.drained)
	{
		return;
	}
	cd.priming = 0;
	cdda_channels_start();
}

/* ------------------------------------------------------------ the service */


static void cdda_service_body(void)
{
	unsigned int filled;

	if (cd.need_restream)
	{
		/* A title took the channels. Re-keying onto the ring as it stands
		 * would decode it from a reset decoder: start over instead. */
		cd.need_restream = 0;
		cdda_prime();
		return;
	}
	if (cd.priming)
	{
		cdda_prime_step();
		return;
	}

	filled = cdda_fill();

	if (cd.drained && cd.end_left == 0u)
	{
		/* The last real frame has been heard. */
		cdda_channels_stop();
		cd.state = CDDA_STATE_STOPPED;
		cd.audio_stat = SCD_AUDIO_ENDED;
		return;
	}

	if (cd.running && !cd.drained)
	{
		/* After the fill, which may have won some lead back. A lead within a
		 * sub-fetch of nothing, or a service gap longer than the lead could
		 * cover, means what comes next is not the continuation: silence, and
		 * start over from what was last heard rather than from the write head,
		 * which is a second ahead of it. */
		unsigned int room = cdda_lead(cdda_true_elapsed());

		if (room < g_cdda_room_min)
		{
			g_cdda_room_min = room;
		}
		if (room <= CDDA_MUTE_GUARD || cd.svc_gap >= CDDA_GAP_LIMIT_TICKS)
		{
			g_cdda_mutes++;
			cd.next_lba = cdda_current_lba();
			cdda_prime();
			return;
		}
	}

#if CDDA_SERVICE_DRAIN_ITERS
	/*
	 * Nothing to fill: listen to the network briefly, at most every 20 ms. Once
	 * a title has loaded, the audio fetches are nearly the only time the loader
	 * looks at the network, so a counter read from the host would otherwise
	 * wait for one. Skipped between the chunks of a disc read, which is on the
	 * network continuously anyway.
	 */
	if (filled || svc_no_listen
	    || tmu2_since(cd.listen_mark) < CDDA_LISTEN_PERIOD_TICKS)
	{
		return;
	}
	cd.listen_mark = TMU_TCNT2;
	drain_iters = CDDA_SERVICE_DRAIN_ITERS;
	fine_deadline_start = TMU_TCNT2;
	fine_deadline_ticks = CDDA_SERVICE_DRAIN_DEADLINE_TICKS;
	bb->loop(0);
	fine_deadline_ticks = 0;
	drain_iters = 0;
	timeout_loop = 0;
#endif
}

/* Not re-entrant: a fetch runs bb->loop(), which can dispatch a title's
 * GetDrvStat that calls back into here (isoldr's lock_cdda()). Public so the
 * interrupt hook's tick keeps off the network while it is set (irq.c). */
volatile unsigned int cdda_busy;

void cdda_service(void)
{
	unsigned int now;

	/*
	 * Not while a disc read waits (g_gd_in_transfer): a title's interrupt
	 * handler calling GetDrvStat would start a fetch inside the read's wait,
	 * replace its LoadBinary window and clear its deadline, and the read would
	 * never finish. The read loop feeds the ring between chunks instead. And
	 * not while a title DMA writes sound RAM.
	 */
	if (g_gd_in_transfer || cdda_busy || cd.state != CDDA_STATE_PLAYING
	    || (!cd.running && !cd.priming) || aica_dma_busy())
	{
		return;
	}

	/* How long the ring went unattended: the music only moves when the title
	 * calls the GD driver. TMU2 also counts the drain out, because the model
	 * the rest of the file uses wraps and the end of a track does not. */
	now = TMU_TCNT2;
	cd.svc_gap = cd.svc_marked ? (unsigned int)(cd.svc_mark - now) : 0u;
	if ((int)cd.svc_gap < 0)
	{
		/* The tick's service ran between reading `now` and here and moved
		 * svc_mark past it: a negative gap, which read as 343597 ms and muted
		 * the stream twice (Shenmue II, g_cdda_svc_gap_max 0xffffffc8). */
		cd.svc_gap = 0u;
	}
	if (cd.svc_gap > g_cdda_svc_gap_max)
	{
		g_cdda_svc_gap_max = cd.svc_gap;
	}
	if (cd.drained && cd.running)
	{
		cd.end_left = (cd.end_left > cd.svc_gap) ? cd.end_left - cd.svc_gap : 0u;
	}
	cd.svc_mark = now;
	cd.svc_marked = 1;

	cdda_busy = 1;
	cdda_service_body();
	cdda_busy = 0;
}

/* Feed the ring from inside a long disc read, which does not return to the
 * title and can last seconds; without it the ring runs dry during level loads.
 * The call site has no transfer of the read in flight (cdfs_syscalls.c). */
void cdda_service_between_chunks(void)
{
	svc_no_listen = 1;
	cdda_service();
	svc_no_listen = 0;
}

/*
 * From the interrupt hook (irq.c), with SR.BL set, on the hook's own stack,
 * and only while the GD lock is free -- i.e. while nothing of the GD path owns
 * pkt_buf, bin_info or the ring. So each call stays short: one sub-fetch at
 * most (~3 ms, the exchange itself on gd_on_loader_stack()'s stack), and no
 * listening window, which would run the whole network path on the hook's 1 KB.
 * Called every few ms, it keeps the lead whole without the title's help.
 */
void cdda_service_tick(void)
{
#if WITH_IRQ_HOOK
	/* A fetch waits for its whole answer inside bb->loop(). With the tick's RX
	 * DMA armed, the loop stops at the first PartBinary (the frame is left to
	 * rx_settle()) and the fetch judges a window that is only begun: Aqua GT
	 * failed 5 fetches in 6, each up to 20 ms with SR.BL set. The fetch takes
	 * its frames by CPU, as outside the tick (AGENTS.md 4.16). */
	extern volatile unsigned int g_rx_dma_tick;
	unsigned int dma = g_rx_dma_tick;

	g_rx_dma_tick = 0;
#endif
	svc_no_listen = 1;
	svc_one = 1;
	cdda_service();
	svc_one = 0;
	svc_no_listen = 0;
#if WITH_IRQ_HOOK
	g_rx_dma_tick = dma;
#endif
}

/* ------------------------------------------------------------ public API */

static void cdda_begin(unsigned int first, unsigned int last, unsigned int loop,
                       unsigned int track)
{
	cd.first_lba = first;
	cd.last_lba = last;
	cd.next_lba = first;
	cd.loop = loop;
	cd.track = track;
	cd.state = CDDA_STATE_PLAYING;
	cd.audio_stat = SCD_AUDIO_PLAYING;
	g_cdda_plays++;
	cdda_prime();
}

int cdda_play_sectors(unsigned int first, unsigned int last, unsigned int loop)
{
	if (cdda_load_toc() < 0)
	{
		return -1;
	}
	if (first == 0u)
	{
		first = toc_track_lba(toc_first_track());
	}
	if (first >= last)
	{
		return -1;
	}
	cdda_begin(first, last, loop, cdda_find_track(first));
	return 0;
}

int cdda_play_tracks(unsigned int first, unsigned int last, unsigned int loop)
{
	unsigned int flo, lhi;

	if (cdda_load_toc() < 0)
	{
		return -1;
	}
	flo = toc_first_track();
	lhi = toc_last_track();
	if (first < flo || first > lhi || last < flo || last > lhi || first > last)
	{
		return -1;
	}
	cdda_begin(toc_track_lba(first),
	           (last < lhi) ? toc_track_lba(last + 1) : toc_leadout_lba(),
	           loop, first);
	return 0;
}

int cdda_pause(void)
{
	if (cd.state != CDDA_STATE_PLAYING)
	{
		return -1;
	}
	cdda_channels_stop();
	cd.priming = 0;
	cd.state = CDDA_STATE_PAUSED;
	cd.audio_stat = SCD_AUDIO_PAUSED;
	return 0;
}

int cdda_release(void)
{
	if (cd.state != CDDA_STATE_PAUSED)
	{
		return -1;
	}
	/* The ring is stale: restart from where the read head stopped. */
	cd.state = CDDA_STATE_PLAYING;
	cd.audio_stat = SCD_AUDIO_PLAYING;
	cdda_prime();
	return 0;
}

int cdda_stop(void)
{
	cdda_channels_stop();
	cd.priming = 0;
	cd.state = CDDA_STATE_STOPPED;
	cd.audio_stat = SCD_AUDIO_NO_INFO;
	cd.track = 0;
	cd.drained = 0;
	return 0;
}

int cdda_seek(unsigned int lba)
{
	if (cd.state == CDDA_STATE_STOPPED)
	{
		return -1;
	}
	cd.next_lba = lba;
	cd.first_lba = lba;
	cd.track = cdda_find_track(lba);
	cd.state = CDDA_STATE_PLAYING;
	cd.audio_stat = SCD_AUDIO_PLAYING;
	cdda_prime();
	return 0;
}

int cdda_state(void)
{
	if (cd.state == CDDA_STATE_PLAYING)
	{
		return CDDA_PLAYING;
	}
	return (cd.state == CDDA_STATE_PAUSED) ? CDDA_PAUSED : CDDA_STOPPED;
}

/*
 * The FAD the listener is hearing, for CMD_REQ_STAT / CMD_GETSCD: the write
 * head less the lead, the audio buffered between the two. Once the source has
 * run out the lead is silence the fill laid, so what is left to hear is the
 * drain countdown instead. Exact to a few frames; isoldr reports the read head.
 */
unsigned int cdda_current_lba(void)
{
	unsigned int behind;

	if (!cd.running)
	{
		/* Starting (or stopped): where the key-on will begin. */
		return cd.next_lba - cd.write_pos / FRAMES_PER_SECTOR;
	}
	/* The host's trim is ignored here (0.02 %). */
	behind = cdda_ticks_sectors(cd.drained ? (cd.end_left >> 2)
	                                       : cdda_lead(cdda_true_elapsed()));
	if (cd.next_lba < cd.first_lba + behind)
	{
		return cd.first_lba;
	}
	return cd.next_lba - behind;
}

unsigned int cdda_track(void)
{
	return cd.track;
}

unsigned int cdda_repeat(void)
{
	return cd.loop;
}

unsigned int cdda_audio_status(void)
{
	return cd.audio_stat;
}

#endif /* WITH_CDDA */
