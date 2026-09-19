# CD-DA double-buffer investigation, 2026-09-06 to 2026-09-12

**History, not a description of the tree.** This is the CD-DA material that
AGENTS.md carried while the double-buffer engine was being brought up on real
hardware (Snow Surfers PAL, BBA), moved here verbatim on 2026-09-12 when
AGENTS.md was cleaned up. The earlier investigation (the integrator engine,
2026-08-29 to 09-05) is `cdda-crackle-investigation.md`. The current design is
the header of `target-src/dcload/cdda.c` and AGENTS.md §4.13.

It was written as the work went, so later paragraphs often correct earlier
ones. What ended up superseded, so that nothing below is taken for current:

| Topic | Said at some point below | What the code does now |
| --- | --- | --- |
| Default format | 16-bit PCM | 4-bit ADPCM since 2026-09-09 (`CDDA_ADPCM=1`) |
| Half of the ring | 80, then 160, then 213 ms | 693 ms (`CDDA_FETCHES_PER_HALF` 13), filled 2 sub-fetches per service |
| Loop clock | TMU1 on Pck/256, per-Holly-revision constant | TMU1 on Pck/16, one constant (isoldr VA1 x 16) plus the host's ppm trim |
| Fill lag | a quarter loop, then a sixteenth | a quarter loop (`CDDA_LAG_SHIFT` 2) |
| Sub-fetch deadline | 50, 20, then 60 ms | 20 ms; failure drain 10 ms; host give-up (`CDDA_GIVE_UP`) 15 ms |
| Clock error | "+11700 ppm", hand-measured | retracted: unaligned windows. The trim measured 184 ppm, 159 ppm independently |
| `CDDA_TRIM_FIRST_S` | 20 s | 90 s, then 300 s windows |
| Host encoder history (`RECENT`) | 8 requests | 48 |
| Ring placement | top of sound RAM, then 64 KB below, then the middle | `CDDA_RING_TOP` 0x1a0000, from the activity map |
| "5-13 % of halves clobbered" | the title writing our ring | a probe bug (offset advanced on a repeated check) |
| `g_cdda_wrong_lba` | late answers from abandoned fetches | mostly our own complete windows read before the echo arrived; now waited for (`g_cdda_retv_late`) |
| "Discontinuity leaving this host" (host slew test) | corrupt audio at LBA 0x4e6ce | loud music; `audit-audio` finds the image clean |
| Title freezes (ExecServer == sync_reentered) | a stuck GD lock / an unexplained fault | attributed to a CD-DA fetch nesting inside a disc read's wait; fixed by `g_gd_in_transfer` + `GD_CDDA_BETWEEN_CHUNKS` |
| "TMU2 is started in dcload.c's timer setup" | at boot | only in `setup_machine()`, i.e. only with `ISOLDR_SETUP_MACHINE=1`; otherwise CD-DA starts it |
| `CDDA_PUSH_SQ` | a build flag | removed |

---

## As AGENTS.md §4.13 stood on 2026-09-12

### 4.13 CD-DA: the loader is the drive (double-buffer, rewritten 2026-09-06)

A GD-ROM carries its music as ordinary audio tracks and a title plays them by
asking the GD driver for `CMD_PLAY_TRACKS` / `CMD_PLAY_SECTORS`. There is no
drive here, so `cdda.c` **is** the drive: it reads the audio sectors over the
network and feeds two AICA channels. `cdda.c` and `cdda.h` carry the full
design; read them first.

**This is a rewrite.** It replaces an integrate-from-key-on pacing model, whose
write position drifted over a whole track until the writer lapped the reader,
and ~2/3 of the file that was instrumentation for a crackle investigation. That
investigation is `docs/cdda-crackle-investigation.md` — history, not the current
code — and the design comparison that motivated the rewrite is in the plan the
work was done from. The engine now follows **isoldr's and KOS's double-buffer**,
which both use for exactly this problem and which our old model was the only one
to diverge from.

**The double buffer, which is the whole idea.** Each channel's ring is two
halves. One half is refilled while the AICA plays the other, and the trigger is
a single question asked of a timer: *has the play head crossed into the other
half yet?* The timer is TMU1 at Pck/16, started in the same critical section as
the key-on with `TCOR = end_tm` (one AICA loop), so it **auto-reloads once per
loop** and `end_tm - TCNT1` is ticks since the loop wrapped, in lockstep with
the AICA -- deliberately a quarter loop behind it, see below. Because the write position is *discrete* (half 0 or half 1) and
snapped to the AICA's actual half, it can never drift more than a quarter buffer
from the reader — the timer only has to be right **within one half** (~160 ms),
where SH4/AICA clock drift is negligible. `cdda_service()` fills `cur_buff` when
`cdda_aica_half() != cur_buff`, then toggles.

**`end_tm` is one measured constant**, scaled to our ring:
`RING_SAMPLES * 578960 / 8192` (isoldr's VA1 figure on Pck/16), split into two
terms so the product stays in 32 bits, then trimmed by the host. The
per-revision lookup is gone -- see below for why it was the error.

**MEASURED 2026-09-09, ONCE THE ESTIMATOR WAS RIGHT: 159 ppm.** The single
constant is therefore correct for this console to within a sixth of a
thousandth, and what remains is not the constant but the two things that bound
how well any constant can work here:

- **THE QUANTISATION WAS 32 PPM, AND 32 PPM IS NOT SMALL.** On isoldr's Pck/256
  the loop is ~31000 ticks, so one tick is 32 ppm -- the smallest correction the
  model can express, and larger than what the host can now measure. 32 ppm walks
  the model's phase across a whole half in 42 minutes, which a looping track
  reaches. **TMU1 is on Pck/16**: the loop is ~499000 ticks, one tick is 2 ppm,
  and the quantisation stops being part of the answer. `end_tm` and
  `g_cdda_room_min` are in those ticks now (~3117 to the millisecond), and with
  the doubled ring `end_tm` is ~997000 for a 320 ms loop.
- **THE TOLERANCE WAS ZERO ON ONE SIDE.** A half is refilled the instant the
  model says the AICA left it, so a model running even slightly EARLY writes into
  the tail of the half still being played: the last phi milliseconds are replaced
  by audio from one revolution later, a splice every loop, audible from a
  millisecond or two. Late is safe all the way to a half. Starting at dead centre
  of a 0-to-80 ms tolerance means any drift at all is audible half the time.
  **TMU1 starts a sixteenth of a loop behind** — `TCNT1 = end_tm +
  end_tm/16`, which `cdda_elapsed()` already reads as "the loop has not begun" —
  so the first fill lands 20 ms after the AICA leaves the half. Both halves stay
  primed; nothing else moves.
  **A QUARTER WAS TRIED FIRST AND WAS A NET LOSS, WHICH IS THE WHOLE LESSON
  HERE: THE LAG IS SPENT OUT OF THE SERVICE BUDGET.** Every millisecond of lag is
  a millisecond less for the fill to happen in, and this console's ordinary
  service gaps reach 64 ms. Measured 2026-09-09 with a 40 ms lag against an 80 ms
  half: `g_cdda_room_min` came back at **15.9 ms of model time, i.e. -24 ms of
  real clearance** — a genuine overrun — and `g_cdda_underruns` stayed at 0
  throughout, because the underrun test was asking the same lagged model. The lag
  is now small against the half, the half is twice as big (above), and
  **everything that measures uses `cdda_true_half()` / `cdda_true_elapsed()`**,
  which add the lag back; only the refill trigger reads the lagged one. An
  accounting built on the model that the design deliberately skews is §11's rule
  inside the engine itself.

**Two timers, and the conflict that forces it.** TMU2 is the loader's
fine-deadline / timeout clock (`adapter.h`) — Pck/4, free-running from
`0xffffffff`, so `(start - TCNT2)` is a plain elapsed count that cannot wrap
inside a fetch, and the fetch deadlines (`CDDA_FETCH_DEADLINE_TICKS` = 50 ms,
etc.) are Pck/4 values. **The loop clock cannot share it**: an auto-reload at
`end_tm` (~320 ms) would make those deltas wrap. TMU1 is free in this loader
(`dcload.c` primes it, never starts it), so it is the loop clock, on Pck/16, and
TMU2 stays the deadline clock. CD-DA starts both.

**Geometry (PCM default).** Ring 14112 samples/channel (28224 B, 56 KB of sound
RAM for both ears), loop 320 ms, half 160 ms = 12 raw sectors, filled by **four**
3-sector sub-fetches into the 7056-byte `.hiram` staging buffer, split L/R and
written with plain P2 stores (2 samples a 32-bit word). `.hiram` is unchanged
from the old build — the staging buffer is one sub-fetch, not one half. On PLAY
the whole ring is prefilled (24 sectors, ~10 ms of wire) and the channels keyed
on, so there is **no startup silence**.

**THE HALF IS TWICE ISOLDR'S BECAUSE THE SERVICE INTERVAL IS NOT OURS TO
CHOOSE.** `cdda_service()` runs from the GD server loop, so a disc read is a
stretch in which nothing feeds the ring — and this transport's reads are over a
network. Measured 2026-09-09 on Snow Surfers, twice: the worst clearance in a
session was **3.7 ms out of an 80 ms half**, i.e. the loader went ~76 ms without
servicing, and the host has been logged taking **275 ms** to answer a single
read. isoldr's 80 ms half leaves nothing for that; 160 ms is the difference
between "occasionally overruns" and "does not". `CDDA_FETCHES_PER_HALF` is the
knob (4 by default, 2 restores isoldr's geometry) and the price is sound RAM at
the top, where a title may also be storing samples — which is what
`g_cdda_ram_clobber` and `CDDA_RING_TOP` are for.

**Format: 4-bit ADPCM by default since 2026-09-09** (`CMD_CDDAREAD_ADPCM` /
`DC24`), on a report rather than a measurement: the PCM build was making the
**title itself** lag on this console. A sub-fetch is 4 sectors instead of 3, so
it is ~25 % fewer round trips as well as a quarter of the bytes on both G2
crossings, and the ring costs 18816 B of sound RAM instead of 56448.
`CDDA_ADPCM=0` is 16-bit PCM (`DC23`, raw 2352-byte sectors), lossless and
per-sample independent, and remains the better answer for *audio* correctness —
4 bits is audibly lossier, and the format is differential, so one lost store is
eight samples on one ear plus a poisoned predictor until the next key-on. The two
paths are the same code either side of the flag; this is a trade, not a finding.

**The mixer level mirrors the game's CD input.** Real CD-DA enters the AICA's
EXTS input, mixed by `0x2040` (left) / `0x2044` (right), and the **game** sets
that level for the CD it expects. We come in on channels 62/63, so
`cdda_read_game_level()` reads the game's EFSDL out of those two registers and
uses it as our channels' send level (DISDL), clamped to `CDDA_DISDL`. A title
that attenuates its CD input — expecting a disc mastered near 0 dBFS — then does
not make our full-scale channels sum past clipping. Falls back to full (0xf) when
the game has not set it. `g_cdda_disdl` is `(left<<8)|right`.

**Position is reported to the title.** `get_stat()` (`CMD_REQ_STAT`) and a new
`get_scd()` (`CMD_GETSCD`, `cdfs_syscalls.c`) answer the audio FAD, track, repeat
count and audio status from the engine while music plays — the Q-subcode format
isoldr uses — so a title that synchronises to the disc position tracks the music.
`cdda_current_lba()` reports one ring behind the read head, which is what the
listener is hearing.

**A HALF IS NEVER HANDED OVER INCOMPLETE**, and getting that wrong is worth a
paragraph because the first version did. A half is two sub-fetches; if the second
one fails, the second 40 ms of that half still holds audio from one ring
revolution ago. Toggling `cur_buff` there hands the AICA a **splice of stale
audio** — measured 2026-09-06 on Snow Surfers as 9 sub-fetch failures in ~1355,
i.e. nine audible events in a session, with every other counter clean. The guard
that looked like it covered this (`if (done == 0) return;`) only fires when
*nothing* was written, which is the rare case; a *partial* fill sailed through it.
So `cd.fill_pos` keeps the progress, `cur_buff` does not move, and a later
service resumes the same half — there is most of a half-period to do it in.
`g_cdda_partials` counts them, `g_cdda_underruns` says whether resuming was ever
too late. `CDDA_FETCH_DEADLINE_TICKS` was cut from 50 ms to **20 ms** in the same
pass: a normal sub-fetch is ~1.5 ms, and a failure must not eat the 80 ms window
the fill has to complete in.

**AND THE DOUBLE BUFFER IS ONLY AS GOOD AS ITS CLOCK, WHICH IS THE REAL LIMIT.**
"Which half is the AICA playing" is answered by TMU1, running off the SH4's
crystal, with a period computed from an *assumed* ticks-per-sample. It is
therefore **still open loop**: if that constant is off by eps, the model's phase
walks through the AICA's by a whole loop every `loop / eps` seconds, and while it
walks past the read pointer we write into the half being played. That is
crackling that grows, peaks, recedes and repeats -- and **no counter here can see
it**, because `cdda_aica_half()` and the underrun test are computed from the same
drifting model and agree with each other whatever the truth is. Same blind spot
as the old integrator, reached a different way.

Measured 2026-09-06 on this console (Snow Surfers, PAL), by the **host**, whose
clock is independent of both: four consecutive windows of 250 sub-fetches took
10.02-10.03 s against the 10.00 s real time demands, i.e. the model running
**0.2-0.3 % slow**, predicting a 53-64 s beat. The reported symptom was a cycle
of about a minute, growing then receding. **CONFIRMED BY ITS OWN FALSIFIER:**
`CDDA_TICK_ADJ=25` (0.25 %) took it to "mostly perfect, only a few artifacts",
and a 0.05 % residual is exactly a 320 s beat, which is what a few artifacts
sounds like. Nothing else in the engine changed between the two sets.

**THE PER-REVISION TABLE WAS THE ERROR, AND IT IS GONE.** isoldr carries two
constants -- 36185 (VA1) and 36277 (VA0) per 8192 samples -- and picks on the
Holly revision. They differ by 0.25 %, and **no pair of crystals explains
0.25 %**: an SH4 and an AICA are both crystal-clocked to a hundred parts per
million or so, which is what makes this ratio a physical constant rather than a
per-console reading. This console reports revision `0x0b`, which selected the
VA0 entry -- 0.30 % too large -- while the VA1 entry is within 0.05 % of what
measurement asks for. So the lookup was picking the worse of two numbers, one of
which is simply wrong. `cdda.c` now carries **one** constant, 36185, and the
`(HOLLY_ID <= 0x0b) ? VA0 : VA1` test is deleted.

**THE CONSOLE CANNOT MEASURE ITS WAY OUT OF THIS, AND THAT WAS TESTED TO
DESTRUCTION.** The AICA's three timers (`0x2890`/`0x2894`/`0x2898`) step every
`2^md` **samples** off the chip's own clock, with no shared select register in
the way (unlike the play position) -- exactly the reference this engine lacks.
All three read `md = 0` here, so a first attempt that sampled them once per
service (~9.5 ms) aliased against the 5.8 ms wrap and returned noise (ratios
5.32 and 4.09 against a nominal 4.42). The answer looked like a **shorter
interval**: read the count every millisecond, where the delta is at most ~44 and
unambiguous. Measured 2026-09-07, that burst accepted **not one delta in 250**:
`g_cdda_cal_smp` came back at its "this ran" sentinel and `g_cdda_cal_tck` at 0.
Settled 2026-09-09 by the raw read that replaced it -- `g_cdda_atmr0` and
`g_cdda_atmr` are **both `0x00000000`** after minutes of play, while `0x2800`
(master volume) and `0x2040`/`0x2044` read back sensibly on the same path in the
same session: **the AICA's timer registers are not readable from the SH4**, so
the prescale we thought we were reading was the zero as well. The calibration
declined, as its falsifier told it to, and was removed rather than kept as a
250 ms freeze at every first PLAY that measures nothing; the two raw reads stay,
because they are what makes the claim checkable in one glance.
Everything else on the SH4 side of the wire runs off the SH4's own crystal and
can only confirm the model against itself.

**THE HOST CAN, AND NOW DOES: THE CLOCK TRIM.** dcload-ip-rs has a clock of its
own and serves every sub-fetch, and "audio staged against real time" **is** the
quantity that has to be 1.0. It measures it from **the disc position, not from a
fetch count** -- a sub-fetch that misses its deadline is re-asked at the same
LBA, and 11 such in 756 fetches is 1.5 %, against the 0.03 % this is trying to
resolve -- and returns the correction in parts per million in the **`size` field
of the ReturnValue that already ends every audio fetch** (§8). No extra packet,
no round trip, no new command, and idempotent: 1000000 means "no opinion", the
same number is repeated until there is a better one, and a host that does not
implement this sends 0, which the loader ignores. `cdda_scale_from_host()`
applies it to the compiled-in constant and rewrites `TCOR1`, which TMU1 picks up
at its next auto-reload, so the phase is not disturbed.

The estimate is of **one constant, not of a moving target**: the host keeps total
audio staged and the sum of each epoch's real seconds divided by the scale that
was in force, and their ratio is the scale the loader *should* be using. Each
correction is therefore a fresh estimate over a longer window rather than an
increment on the last one, so measurement noise cannot random-walk the answer.
The first window is 90 s and every window after it is 300 s. One tick of `end_tm`
is 32 ppm, which is the floor and is a ~90-minute beat.
`DCLOAD_CDDA_TRIM=0` turns the trim off for an A/B without rebuilding anything;
`CDDA_TICK_ADJ` (parts per 10000, subtracted) remains the manual override.

**AND THE TRIM HAD BEEN DEAD SINCE THE RING WAS ENLARGED, WHICH IS WHY NONE OF
THE ABOVE EVER APPLIED.** `g_cdda_scale_sets` has read **0** in every session on
record, and that was read as "the window has not elapsed yet" for several of
them. It was not: the printed estimate, once there was one, said
`window has 0.0s of 20s free-running stream banked` after **66 s** of play. The
estimator was accepting nothing at all, and could not.

The thresholds are documented against the geometry of 2026-09-06: *"a half of
the ring is 80 ms and is fetched in two back-to-back sub-fetches, so the gaps
alternate ~1.5 ms and ~78 ms"*. A window may only open and close on a half
boundary, and a boundary was a gap in **[40, 120] ms** — which that 78 ms sat
inside. The half is **693 ms of 13 sub-fetches** now, filled two at a time by a
service that runs every ~17 ms, so the real gaps are ~1.5 ms inside a service,
~17 ms between services, and then **~570 ms of idle**. **Nothing lands between
40 and 120 ms.** `boundary` was never true, `pend_*` was never banked,
`win_time_s` stayed 0.0 for ever, and the host has owned nothing since
`CDDA_FETCHES_PER_HALF` went to 13 on 2026-09-10.

`CDDA_TRIM_HALF_GAP_MIN` is 100 ms (above any service cadence, below an idle
tail) and `CDDA_TRIM_GAP_MAX` is 1.0 s (an idle tail approaches a whole half).
Admitting a gap that long costs the stall rejection that GAP_MAX used to do, so
that moves to the granularity which can actually tell the two apart:
`CDDA_TRIM_HALF_SANE` drops a **banked half** whose own rate is more than 20 %
from real time — a stall is many-fold behind, a half carrying two failed
sub-fetches is ~11 %.

**The five green tests were the real defect.** `feed_stream()` modelled two
sub-fetches per 80 ms half — hardware that stopped existing on 2026-09-10 — so
every test passed on a shape the console never produces. §14.9 exactly: a guard
that cannot fire. The helper models the real geometry now, and
`a_free_running_stream_banks_time_at_all` asserts the thing that was actually
broken — not "does it measure correctly" but **"does it measure at all"**.
Verified by reverting the two thresholds: that test fails with
`banked only 0s of a 14 s stream`, which is the hardware symptom to the word.
`a_half_that_took_too_long_is_not_banked` is the positive control for the new
stall guard.

**AND ONCE IT RAN, IT SAID 184 PPM — WHICH RETRACTS A 1.16 % FIGURE MEASURED BY
HAND THE SAME DAY.** With the thresholds fixed the trim banked a window for the
first time since the ring was enlarged and applied `1000000 -> 1000184 ppm`;
`g_cdda_scale_ppm` and `g_cdda_scale_sets` moved and `g_cdda_end_tm` went
4321845 → 4322640. 184 ppm agrees with the **159 ppm** measured independently on
2026-09-09, so the constant in `cdda.c` was right all along.

The hand figure that disagreed was **+11700 ppm**, taken from five consecutive
periodic log lines (LBA advance against the host's clock) and briefly believed
enough to cut `CDDA_TRIM_FIRST_S` to 20 s. It was wrong in exactly the way this
section documents. **The periodic line fires every 250 fetches, which is 19.23
halves, not an integer**, so each of those windows began and ended mid-fill. A
half is 13 sub-fetches laid down in ~102 ms of a 693 ms period, so the fractional
0.23 of a half is three sub-fetches — 160 ms of audio against ~34 ms of wall —
and that alone is ~9600 ppm of the 11700. **A measurement on an unaligned window
reproduced by hand the bias `CDDA_TRIM_HALF_GAP_MIN` exists to prevent.**
`CDDA_TRIM_FIRST_S` is back to 90 s: 184 ppm walks the phase across a half in
about an hour, so there is nothing to chase quickly, and a 20 s window carries
~500 ppm of service jitter — the partial windows in that session read +1613 ppm
at 5.5 s and +281 ppm at 18.7 s — so correcting there applies more noise than it
removes.

**The instrument defect behind the wrong number is real and is fixed.** The
periodic line printed `1.01x real time` from a **fetch count**, which a re-ask
inflates — the very thing this section says not to measure — and the estimator's
own disc-position view was printed nowhere until it committed. So "the trim has
not fired yet", "the trim sees nothing wrong" and "the trim can never fire" were
all indistinguishable from outside. `CddaClock::progress()` now puts the banked
seconds, the window's ppm, the trims applied and the scale in force on that line
every 250 fetches, and it is what caught both the dead estimator and the bad
arithmetic.

**AND TWO THINGS ABOUT THAT MEASUREMENT WERE WRONG ON THE FIRST TRY, BOTH
MEASURED, BOTH NOW TESTED** (`CddaClock`, `src/dispatch.rs`, five unit tests):

- **A LOADING STALL IS NOT A CLOCK.** While a title loads, the loader stops
  servicing the ring on time and the stream falls behind real time *for good* --
  it never catches up, since one service fills at most one half. Measured
  2026-09-09 on a Snow Surfers boot: **250 fetches in 37.27 s** against the
  usual 9.0, i.e. the audio running 25 % behind the wall. Read as a clock, that
  drove the trim to its clamp and left `end_tm` 10 % fast. The estimator now
  accepts an interval only if the gap between two fetches is under 120 ms (a
  free-running half is 80 ms, fetched as two back-to-back sub-fetches) and the
  disc advanced at most 12 sectors. **Dropping an interval from both sums is
  unbiased**; keeping it was not. A window whose own answer is more than 3 % out
  is discarded whole, and an estimate outside 1.5 % is refused rather than
  applied -- crystals cannot be that far apart, so such a number is a broken
  measurement, and applying one is worse than applying nothing.
- **A WINDOW MUST BEGIN AND END ON A HALF BOUNDARY.** Sub-fetches arrive in
  pairs -- ~1.5 ms apart, then ~78 ms of nothing -- so a window that ends in the
  middle of a pair has counted 40 ms of audio that no elapsed time answers for.
  That is a **bias, not noise, so it does not average out**: over 90 s it reads
  as 428 ppm, on a stream constructed to be exactly right. The gap that precedes
  a fetch identifies its phase, so the accumulator commits only at gaps over
  40 ms, and the first such gap is where the window starts.

**Kept from the proven code, verbatim:** `g2_lock()` (IRQ mask + all three G2
DMA-suspend registers + FIFO drain, the fix that cost a session), the eight-store
FIFO rule, the AICA register values (arm/strobe, TL=0 attenuation, DISDL/DIPAN,
LEA in samples, `AICA_SM_ADPCM_LS`), the channel watchdog
(`cdda_check_channels`, which is proved to fire on real channel theft and which
grades pan/send repairs apart from structural re-keys), and `cdda_load_toc`.
`cdda_service()` carries a re-entrancy guard, since a fetch runs `bb->loop()`.

**AND `g_cdda_underruns` HAD A BLIND SPOT EXACTLY WHERE THE NOISE IS.** It is
computed only where a fill COMPLETES, so a half that stays partial — the failure
mode the `fill_pos` guard above creates on purpose — never reaches the code that
counts it, however long the AICA spends playing the stale part of it. Measured
2026-09-09 on a heavy-disc session: **342 partials, 393 sub-fetch failures, ONE
underrun and a clean bill of health**, while the listener heard something every
few seconds. `g_cdda_stale` counts the event that is actually audible — the AICA
reached a half that is still partial — once per partial half. Read it against
`g_cdda_partials`: partials are halves that had to be resumed, `stale` is the
subset that was heard. In the same pass `CDDA_FETCH_DEADLINE_TICKS` went **20 ms
to 60 ms**: 20 was sized against isoldr's 80 ms half, the half is now 160-213 ms,
and giving up is not cheaper than waiting — a failure costs the time already
spent, a 5 ms drain, and a re-ask that pays the whole latency again.

**WHAT THE OTHER TWO DO, RE-READ IN FULL 2026-09-10, AND THE ONE THING IT
CHANGES.** KOS and DreamShell both run the double buffer against the AICA's
*real* play position — and neither reads it from a register. Their ARM firmware
does (`aica_get_pos()`: byte-write the channel to `0x280d`, settle, read
`0x2814`) and publishes it into sound RAM at `AICA_MEM_CHANNELS`, which is what
`snd_stream_poll()` reads over G2. **A title owns the ARM here, so that door is
closed to us, and that — not a missing technique — is why they carry no clock
constant and we do.** isoldr is in exactly our position (SH4 only, no ARM) and
does exactly what this file does: `aica_get_pseudo_pos()` from an auto-reloading
timer, `pos >= end_pos/2` as the trigger, one measured constant. Its constants
are **ear-tuned**, per buffer size and per board, and its own comments grade
them "no error" / "small cumulative error" — the host's 159 ppm measurement is
finer than any of the ten.

So there is no better clock to be had. **What there is, is buffer.** LEA
(channel register 12) is 16 bits and holds the last sample index, so a channel
can address **65536 samples** — and KOS states that ceiling three times without
naming it: `SND_STREAM_BUFFER_MAX_PCM16` (128 KB), `_PCM8` (64 KB) and `_ADPCM`
(32 KB) are one number in three sample widths. isoldr's largest is 32768 and
ours was 18816, **29 % of what the hardware allows**. Neither of them needs the
room: KOS can chase a true position at 2 KB granularity, isoldr reads a local
disc. **We are the only one of the three whose refill interval is a network
round trip**, and the host has been logged taking **275 ms to answer one read**
— longer than the 213 ms half it had to survive, so a slow read did not risk a
stale half, it *guaranteed* one. `CDDA_FETCHES_PER_HALF` is 13 now: 61152
samples, a 1.39 s loop, a **693 ms half**, 60 KB of sound RAM in ADPCM.

**And a bigger half must not become a bigger freeze, which is the one thing
taken from KOS rather than isoldr.** isoldr fills a whole buffer in one
`CDDA_MainLoop()` pass; `snd_stream_poll()` refills only what the play head has
freed, rounded down to a sector. Thirteen sub-fetches back to back is 43 ms —
two PAL frames — twice a second, which is the "the game is lagging" ADPCM was
adopted to fix. `CDDA_FETCHES_PER_SERVICE` (2) caps one call; `cd.fill_pos`
already carried the resume state, since it exists for a failed sub-fetch.
**A cap-stop is not a partial**: `g_cdda_partials` counts only halves a *failed*
sub-fetch left short, or the counter would fire several times a half and mean
nothing.

**THE PREFILL WAS BEING THROWN AWAY, ON EVERY PLAY.** `cdda_prime()` filled both
halves and then called `cdda_channels_start()` — which began with
`cdda_clear_rings()`. So every PLAY, RELEASE and SEEK wrote a whole ring of
audio into sound RAM, wiped it, keyed on silence, and left `next_lba` past audio
nobody heard: **a loop of silence at every track start, and the first 320 ms of
every track missing.** The clear now happens in `cdda_prime()` *before* the fill.
In the same pass the prefill dropped to **one half** — half 1 is the quiet floor
and `cur_buff = 1` makes the first service fill it, with 693 ms to do so — which
halves the freeze at a track change even though the ring tripled.

**AND A KEY-ON RESETS THE AICA'S ADPCM DECODER, SO IT MUST RESET THE HOST'S
ENCODER TOO.** `cdda_prime()` always did (`cd.restart`); `cdda_restream()`, the
channel-theft repair, did not — it re-keyed and left the host encoding
mid-stream against a decoder back at its initial predictor and step, which
reconverges only when a step happens to hit its clamp. Not reached in any
session on record (`g_cdda_ch_stolen` has stayed 0), which is why it survived.

**THE SOUND-RAM PROBE WATCHED ONE WORD IN 9408 AND STILL FOUND A HIT.** That is
a defect measured, not a rate: `g_cdda_ram_clobber = 1` at `0x1ffb68` holding
`"SEGA"` — a bank header, so a CPU or G2-DMA store, not the AICA. The probe now
XORs **32 sample points spread across a half**, taken when the half is written
and again one loop later just before it is overwritten, with the offset rotating
each visit so the whole ring is covered over about a minute; 32 G2 reads, ~10 µs.
`CDDA_RING_TOP` also dropped by 64 KB, because both writes measured until then
(`0x1f4e04` and `0x1ffb68`) were inside the top 64 KB. Measured 2026-09-10 with
the rings at `0x1e1120` and the wide probe: `g_cdda_stale 0`,
`g_cdda_underruns 0`, `g_cdda_room_min` 364284 ticks = **117 ms of real
clearance** -- the geometry and the clock are sound -- against
`g_cdda_ram_clobber 18` in 356 halves, 5 %, `g_cdda_rbp` reading 0.

**AND THAT 5 % WAS THE PROBE, NOT THE TITLE.** `cdda_probe_check()` advanced the
rotating offset on every call, which is right exactly once per half -- but a
half whose FIRST sub-fetch fails leaves `fill_pos` at 0, so the next service
checks it again, at an offset the stored XOR was never taken at. A guaranteed
mismatch, reported as somebody clobbering our ring. The arithmetic is exact:
**18 clobbers against 18 excess check calls** (`g_cdda_ram_checks` minus
`g_cdda_fetches`, plus the ones the armed bit skips after each PLAY), and it
reproduced to the unit in the next session at a completely different address
(18 in 138, "13 %"). The offset now belongs to the armed sample
(`cd_probe_ph`), advanced only by `cdda_probe_arm()`, so a repeated check is
idempotent instead of self-fulfilling. §11 in the instrument written for §11.

**WHAT ACTUALLY SETTLED IT WAS THE MAP, WHICH DISAGREED WITH THE PROBE.** With
the rings at `0x0f1120`, `g_cdda_srdirty` came back **`0x00008000` after 122
block visits** -- bit 15 alone, which is the block the rings themselves are in.
Not one of the other 31 blocks of the 2 MB changed. Two instruments looking at
the same sound RAM cannot both be right, and the one with a rotating offset was
the one at fault. `g_cdda_srself` is the map's positive control, added in the
same pass: the block holding our rings is rewritten every half by construction,
so it must read changed on every sweep, and it is now scanned rather than
latched blind. If it stays 0 while `g_cdda_srpasses` climbs, every clear bit in
the map is worthless.

**AND THE MAP THEN ANSWERED THE QUESTION IT WAS BUILT FOR.** Measured
2026-09-10 over 12 full sweeps with the control passing (`g_cdda_srself` 11
against 12 visits, the first having no baseline): `g_cdda_srdirty = 0x0003fe03`
— blocks 0-1 (`0x000000-0x01ffff`, the ARM program and its driver) and **blocks
9-17 (`0x090000-0x11ffff`, 576 KB the title works in)**, everything else clean.
The "middle of sound RAM" chosen by reasoning the round before, `0x0f1120`, is
block 15: **inside the title's own working set**, which is why moving there took
the measured clobber rate up rather than down. The rings now sit at the **centre
of the longest clean run** (blocks 18-31, 896 KB) — block 25,
`CDDA_RING_TOP = 0x1a0000` — six blocks clear of the working set below and six
clear of the top, where the one genuine "SEGA" was seen on 2026-09-04. That is
§16's **"biggest hole, centred"** rule, written for main RAM against exactly
this problem, applied to sound RAM: a run is bounded at both ends by blocks
something really wrote, so its middle is the furthest point from anything known.
What a clear bit does *not* mean: the map sees changes **between visits**, so a
block written once before playback began and never again reads clean.

In the same session, with the probe fixed: `g_cdda_fetch_fails` **9 in 5135
sub-fetches (0.18 %**, from 1.6 %), `g_cdda_partials` 7, `stale` 0, `underruns`
0, `g_cdda_room_min` 1121268 ticks = **360 ms of clearance out of a 693 ms
half**, and `g_cdda_ram_clobber` 2 in 390 — the residue of sitting one block
inside the working set.

**AND READS OF THE AICA FROM THE SH4 COME BACK AS ZERO SOMETIMES.** One counter
dump (2026-09-11) caught three at once: the ring probe (`g_cdda_ram_got`
`0x00000000`), `g_cdda_atmr`, and — the one that proves it — **channel 62's
register 0**, read back as `0x00000000` against an expected `0x4399` while that
channel was audibly playing. Zero is not a value a keyed-on channel can hold, so
that read failed rather than found anything. This reframes two older
conclusions: the "AICA timers are not readable from the SH4" finding is
plausibly the permanent form of the same effect, and **a `g_cdda_ram_clobber`
whose `got` is `0x00000000` is an unreadable scan, not a clobbered ring** — the
probe now counts non-zero words and files an all-zero scan under
`g_cdda_ram_unread` instead. The channel watchdog was already protected by its
two-in-a-row rule (`g_cdda_ch_stolen` stayed 0), which is why a false re-key
never happened; it is left alone deliberately, being proved code.

**A fixed address is still a guess wherever the map has not run**,
so `cdda_srmap_step()` maps sound RAM instead: 32 blocks of 64 KB -- one block
is exactly what a ring needs -- one sampled per completed half at 64 words on a
**fixed** block-dependent offset (rotating it is exactly the bug above), XORed,
and `g_cdda_srdirty` latches a bit when a block changes under us. It reads only, so it cannot damage the title's own
audio, which is what rules out stamping a pattern over sound RAM and seeing
what survives; the price is that a clear bit means little until
`g_cdda_srpasses` is well past 32 (a full sweep is ~22 s). The rings moved to
the middle of sound RAM in the same pass -- the last address chosen by
reasoning rather than by reading the map. This matters more in ADPCM
than it ever did in PCM: the format is differential, so an overwritten byte is
not one click but a poisoned predictor on that ear until the next key-on — **it
sounds exactly like a clock that is out**, which is why the ring is checked
before the clock is blamed. `g_cdda_rbp` publishes AICA `0x2804` (RBP/RBL) in the
same spirit: if a title points the DSP's output ring at our sound RAM, *the
hardware* is the writer and no amount of servicing wins.

**The reported FAD is computed now, not assumed.** `cdda_current_lba()` used to
answer one whole ring behind the fetch head, which was honest at 320 ms and is
not at 1.39 s; it takes the model's play position and the write head and reports
what is really being heard. Its divisor is a **compile-time constant** on
purpose — a variable divisor pulls in libgcc's `__udivsi3_i4i`, 1060 bytes,
measured (§14.15's rule, one width down).

**AN ANSWER WAS NOT TIED TO ITS QUESTION, ON THE ONE PATH THAT DROPPED ITS
ACKNOWLEDGEMENTS.** `send_audio()` deliberately sends LoadBinary, the parts and
the ReturnValue with no round trips (§16), which is worth 2.4 ms of frozen title
per fetch. The cost nobody had priced: a sub-fetch that misses its deadline is
**re-asked while the first answer is still on the wire**, and both answers carry
the same destination address and the same size, so `cdda_fetch()` -- which
accepted on `syscall_retval >= 0` plus `bin_window_complete()` -- could not tell
them apart. A straggler consumed as the answer to a LATER block puts a repeated
four sectors into the ring and skips the ones that belonged there. **Because
ADPCM is differential and per channel, the encoder and the decoder then part
company on ONE EAR**, heard as that side dropping samples or changing level
while the other stays right, which is exactly what was reported. The same class
of bug as the host's own stale-`SendBinQ` (§16), on the console side.

The fix costs nothing: **the host echoes the LBA it answered in the
ReturnValue's `address` field**, which was 0 and unused, and the loader refuses
anything else (`g_cdda_wrong_lba`, and the straggler is drained before the
re-ask). It is backward compatible in the direction that matters -- an LBA is
positive and an older loader only tests this field for `< 0`.

**AND A ZERO IN THAT FIELD IS SOMEBODY ELSE'S ANSWER, NOT AN OLD HOST.**
Accepting 0 as "a host too old to echo" was the first version and it was worth
seven corruptions in a two-minute session, measured 2026-09-11 alongside
`g_cdda_wrong_lba 7` on a rebuilt host: **every other ReturnValue this host
sends carries address 0** -- a disc read's, ~290 of them every two seconds while
a level streams, and the TOC's -- and one landing inside the audio fetch's
`bb->loop()` released it early with a buffer that was never filled.
`cd.host_echoes` latches on the first correctly echoed LBA, and after that a 0
is refused like any other wrong answer. `g_cdda_noid` can now only count
answers taken *before* that first proof, which is the only window in which the
two readings of 0 are genuinely ambiguous.

**AND THE IDENTITY CHECK WAS ON THE END OF THE ANSWER, WHICH IS HALF OF IT.**
The LBA echo is in the ReturnValue, and the ReturnValue is the *last* packet of
an answer. So it catches a straggler whose ReturnValue beats the live one home
— `g_cdda_wrong_lba`, 19 in a two-minute session measured 2026-09-11 — and is
blind to the mirror case, which is exactly as likely: **the straggler's
LoadBinary arrives first, resets the window and refills part of the map, and the
live answer's own ReturnValue then completes a buffer that is a splice of two
blocks.** Accepted, pushed, counted nowhere. One ear drifting while the other
stays right, at about the rate `wrong_lba` was quietly reporting. That is the
residue the user heard after the echo went in ("99 % okay, recovers fast").

**A destination address cannot be that discriminator, and the arithmetic says
why.** Every sub-fetch names the same staging buffer and the same size;
alternating between two buffers only separates *adjacent* requests, and a
straggler abandoned at 60 ms against a host logged at 275 ms is ten to twenty
sub-fetches old. So the fix is at both ends, and neither half is a heuristic:

- **The loader shuts the door.** `cdda_fetch()` publishes the staging range and
  the one destination it is waiting for (`g_bin_stage_lo`/`_hi`/`_want`), and
  `cmd_loadbin()` refuses any LoadBinary into that range that is not it,
  **without touching the window** (`g_cdda_stale_lbin`). `want` is cleared the
  instant `bb->loop()` returns, so the failure drain, the idle listening window
  and every disc read after them run with the door shut: an abandoned answer is
  consumed and discarded instead of reopening a window. That second part
  protects the **title's** data — between audio fetches that window belongs to a
  disc read, and a straggler resetting it there is a read served with holes
  (`g_dbin_incomplete` was reading 118).
- **The host does not send it.** Only the host knows an answer is late. It
  times each audio read from the request's arrival and, past **50 ms** against
  the loader's 60 ms deadline, **drops the answer entirely — the ReturnValue
  included**, since that is what would release a *later* fetch early. What is
  dropped is already 25× a normal answer, and the re-ask is served from the
  encoder's byte cache in ~2 ms, so the conservative threshold costs nothing and
  keeps "dropped" and "arrived in time" disjoint rather than adjacent.

The two work the same seam from opposite ends, like the echo and the listening
window before them: the host stops manufacturing stragglers, and the loader
stops being able to act on one that exists anyway.

**AND THE DRAIN THAT WAS SUPPOSED TO CATCH THE ABANDONED ANSWER ENDED IN
MICROSECONDS.** After a failed or refused sub-fetch, `cdda_fetch()` closes the
window and drains — bounded by **both** an iteration count and a deadline, and
the shorter one wins. 4096 poll iterations is microseconds, so the 5 ms clock
was never reached, and 5 ms would not have been enough either. The window an
abandoned answer can still arrive in is arithmetic, not guesswork: the loader
gives up at 60 ms and the host refuses to send an answer it took more than
50 ms to produce, so one can legitimately leave the host **~10 ms after we
stopped waiting**.

A straggler that survives the drain lands in the NEXT fetch's `bb->loop()`,
where its LoadBinary names the **same** staging address that fetch is waiting
for — so the door-guard above accepts it, the window is reset, and the buffer
that completes is a splice of two blocks. Counted nowhere, audible, and the one
path left after the two fixes above. The drain is 15 ms now with the **clock**
authoritative (`CDDA_DRAIN_ITERS` raised until it cannot end it first); it costs
only on the failure path — measured 2026-09-12, 24 failures in ~7 minutes is
0.36 s, **0.09 % of wall time**, against a 693 ms half with 85 ms of measured
clearance (`g_cdda_room_min` 266662 ticks).

It buys a measurement as well as a fix: during the drain `g_bin_stage_want` is
0, so a straggler's LoadBinary is refused at the door and **counted**. If this
is the remaining mechanism, `g_cdda_stale_lbin` climbs to meet
`g_cdda_wrong_lba`; if it stays put while the glitches do, the mechanism is
wrong and the next session says so. That session's baseline: `stale 0`,
`underruns 0`, `ram_clobber 0` in 245 checks, `srdirty` our own block only,
`fetch_fails` 24 in 3120 sub-fetches (0.77 %), `wrong_lba` 12,
`stale_lbin` 2 — **every counter clean while the listener heard three or four
events in seven minutes**, which is the shape §4.13 has been caught by before.

**AND `g_cdda_wrong_lba` WAS NOT COUNTING STRAGGLERS AT ALL — IT WAS COUNTING
OUR OWN COMPLETE ANSWERS.** `bin_complete_escape()` exists so an audio fetch
need not wait for the ReturnValue: `cmd_partbin()` ends the wait on the **last
PartBinary**, doing "exactly what cmd_retval would have done, a packet earlier"
— `syscall_retval = 0`. So the value the identity check reads is 0 on **every**
sub-fetch, and it is only ever the echoed LBA because the ReturnValue normally
lands in the same drain pass and overwrites it before the loop notices
`escape_loop`.

Normally. Measured 2026-09-12: **`g_bin_data_done` 977 against 975 sub-fetches
— that IS the normal exit** — with `g_cdda_wrong_lba` 3. Those three are the
pass that ended in the gap between the last part and the echo: a window we
filled ourselves, complete and correct, read as somebody else's answer, failed,
and left as a partial half. 0.3 % of sub-fetches, and **proportional to fetches
in every session on record** (19, 12, 8, 3) — which is the shape of a race, not
of a collision with the disc path.

That falsifies what the four paragraphs above were built on. `stale_lbin`
staying 0 while `wrong_lba` moved was read as "the straggler's LoadBinary is not
being refused"; the simpler reading, and the right one, is **that there were no
stragglers** — the host's 50 ms drop had already removed them, and the counter
still moving was this race. The door-guard, the drop and the 15 ms drain are all
still correct and still worth having; what is withdrawn is the claim that
`wrong_lba` measured them.

The fix gives the check its input instead of guessing from its absence: when the
window is complete and `syscall_retval` is still 0 and the deadline has not
expired, `cdda_fetch()` re-enters `bb->loop()` to collect the echo, under the
**same** `fine_deadline_start` — so the whole wait is still 60 ms — with the
staging door already shut so nothing can reopen a window while it listens. If
the echo really never arrives the deadline fires and the fetch fails as it
should. `g_cdda_retv_late` counts the re-entry, and the two are now disjoint:
**`wrong_lba` staying non-zero while `retv_late` climbs would mean there really
are foreign ReturnValues**, which is the question that was never actually asked.

**And the host's encoder kept one step of history where it needed a few.** The
ADPCM `Stream` rewound to the state as it was before the *last* request; a
re-ask one step older re-encoded from a state the console had already moved
past, silently. It now keeps the **bytes it actually sent** for the last eight
requests and answers a repeat from that -- identical by construction rather than
by a derivation that has to be right -- and counts an out-of-order re-ask
(`Stream::out_of_order`) instead of quietly diverging. The user asked for
exactly this.

**AND THE ENCODER'S HISTORY WAS SHALLOWER THAN THE THING THAT GETS RE-ASKED.**
`RECENT` was 8 requests. `CDDA_FETCHES_PER_HALF` is **13**, and `cdda_prime()`
issues all thirteen back-to-back at every PLAY — so a sub-fetch that fails
inside a prime is re-asked with twelve others already sent, **13 requests back,
past an 8-deep history**. The lookup misses, and a miss is indistinguishable
from "the next block": the encoder continues from where it now is, and the
console's decoder — still at the state that block left — diverges on **both**
ears. Nothing counted it; the window completes, the LBA echo matches, and it is
audible.

Measured 2026-09-12 on Snow Surfers, the session the between-chunks feed fixed
the stalls in: a 3 s window carrying a track change (`g_cdda_plays` +1) had
`g_cdda_fetch_fails` +4 and `g_cdda_partials` +3 with **`g_cdda_stale` 0,
`g_cdda_underruns` 0 and `g_cdda_room_min` 1677357 ticks (538 ms of 693)** —
three audible glitches and not one loader counter to show them, which is the
shape §4.13 keeps being caught by. The loader cannot see this one even in
principle: the fault is in bytes it received correctly.

`RECENT` is 48 now — both halves of the ring plus margin, 113 KB — and
`Stream::lost_replay` counts a re-ask the history can no longer answer, with a
unit test that fires it (`a_re_ask_older_than_the_history_is_counted_and_one_
inside_it_is_not`), because a counter nobody has seen fire is worth nothing.
**`replays` and `out_of_order` had never been printed anywhere**; the CDDA log
line carries all three now, and `lost_replay` is a warning rather than a number
in a list: there is no correct answer to give such a request, so the only
acceptable reading is 0.

**The PCM push had been draining the G2 FIFO every SIXTEEN stores**, twice the
rule the rest of the file keeps (`guard & 7u` with two stores an iteration).
A dropped store is four bytes -- two samples on one ear in PCM, eight plus a
poisoned predictor in ADPCM. The ADPCM path always had it right, which is why
this survived: it cannot explain an ADPCM session, and it is a real defect
regardless.

**A FAILED AICA READ COULD CONVICT A CHANNEL OF BEING STOLEN, AND THE REPAIR
WAS WORSE THAN THE CRIME.** Measured 2026-09-11 on Snow Surfers:
`g_cdda_ch_stolen` went to **1 for the first time on record**, the music turned
"loud and saturated", and the title stopped on one frame. Nothing had been
stolen. The chain, all of it visible in the counters:

- **Register 0 carried the severe verdict on its own.** `CH_TEST` weighs it 2
  against `CDDA_CH_BAD_LIMIT` 2, so one mismatch is a structural theft, and the
  two-in-a-row rule is the only thing in the way — which a burst of failed reads
  walks straight through. And reads of the AICA from the SH4 **come back as
  `0x00000000` sometimes**, measured above, with channel 62's register 0 the
  reading that proved it. The previous session's dump carries the fingerprint
  exactly: `g_cdda_ch_bad_reg 0x00003e00`, `got 0x00000000`, `want 0x00004399`.
  Zero is not a value that word can hold — we set the format, loop and KYONB
  bits, and a title that really took the channel writes its own sample address
  into the same field — so an all-zero control word is now `g_cdda_ch_unread`
  and declines to have an opinion.
- **`cdda_restream()` re-keyed onto the ring that was already there.** Setting
  `cd.restart` is right and is not enough: it resets the HOST's encoder at the
  next fetch, while the key-on has already reset the AICA's decoder, which then
  decodes a ring encoded against a predictor and step size it no longer has. A
  differential format decoded from the wrong state does not drift, **it
  diverges** — the step clamps at maximum and the ring is full-scale noise on
  both ears until the service overwrites it. "Loud and saturated", in the
  reporter's words, is what the arithmetic says must happen. It calls
  `cdda_prime()` now: the path every PLAY proves, which lays the quiet floor
  over both halves and prefills from `cd.next_lba`.
- **And the repair erased its own evidence.** `cdda_channels_start()` runs
  `cdda_calibrate_channel_check()`, which zeroes `g_cdda_ch_bad_reg/_got/_want`
  — so the dump taken after a theft shows an empty note and `ch_stolen 1`, and
  the register that convicted is gone. That is why the fingerprint above had to
  come from the session *before*.

**THE TITLE STOPPING IS A SEPARATE FAULT AND IT IS NOT EXPLAINED.** Its
signature is exact and worth knowing: **`g_gd_idx_counts[ExecServer]` and
`g_cdfs_sync_reentered` advanced by the same 706 in 10.38 s** — every single
ExecServer declined, so `gd_lock_byte` was held for the whole window and the
server context never reached `gdcExitToGame()`. Everything else was healthy:
CD-DA fetched 15 halves (1.44/s, exactly the free-running rate), `--diag`
answered, `GetDrvStat` ran at 68/s. So **the loader was alive and the title was
spinning on a server that never came back** — not a crash, not a corrupted
`_end` (`g_gd_sp_min 0x8c00ef0c`, nowhere near the loader at `0x8ce00000`), and
not a disc read (`g_cdfs_sync_chunks` and `ReqCmd` both flat, retries 0).
Reading the code does not produce a mechanism: every path through
`cdda_service_body()` returns, and `gdcServerMain` reaches `gdcExitToGame()` on
every iteration. The one thing that had never executed before is the repair
above, which is why `CDDA_CH_RESTREAM` exists: **0 counts a theft and does not
act on it**, and `loaders-norekey` is that build, so the A/B is exact.

**AND THE CONSOLE STOPPED ANSWERING THE INSTRUMENTS, WHICH IS ITS OWN BUG.**
Once a title has finished loading, the only thing that enters `bb->loop()` is
the CD-DA fetch: 13 sub-fetches of ~3 ms per 693 ms half, **a 5.6 % duty
cycle**. So a counter read waits many fetch windows to be heard, and when it is
heard its 1440-byte answer is transmitted **into the middle of the host's own
burst** -- the half-duplex collision `bin_echo_suppress()` was written for,
aimed at the reply instead of the echo. That is why `--diag` times out with
"0 replies taken, 0 turned away as somebody else's" rather than arriving late,
and it is a debugging tax on every remaining question about this engine.

`cdda_service_body()` now spends a bounded window listening **when it has
nothing to fill**: `CDDA_SERVICE_DRAIN_ITERS` (256) poll iterations, capped at
1 ms, from the same call site that already transmits a fetch request -- so it
adds no hazard §4.5 does not already cover, and unlike `GD_SERVICE_EVERY_SYSCALL`
(the flag that killed Sonic Adventure) **it never hands the title control**. It
runs ~50 times a second on this title, evenly spread, with no transfer of ours
in flight, which is the one place a full-size answer can go out without
colliding. `CDDA_SERVICE_DRAIN_ITERS=0` removes it, and that build is byte-for-
byte the one that preceded it, so the A/B is exact.
**It also drains late audio answers before the next fetch asks** -- the same
straggler `g_cdda_wrong_lba` counts -- so the two fixes work the same seam from
opposite ends.

**Instrumentation is the minimal set** (`src/diag.rs` matches it): `g_cdda_plays`
`_fetches` `_fetch_fails` `_partials` `_stale` `_underruns` `_room_min` (now TMU1 ticks of clearance)
`_toc_fails` `_last_lba`, the channel-theft group (`_ch_stolen` `_ch_repairs`
`_ch_check_ok` `_ch_bad_reg`/`_got`/`_want`), a light one-word-per-fill sound-RAM
clobber probe (`_ram_clobber` `_checks` `_addr`/`_got`/`_want`), and the mixer
(`_mvol` `_mvol_raised` `_disdl`). `struct cd` is 72 bytes (was 356).

**Build flags** (`target-src/dcload/Makefile`): `CDDA_ADPCM` (default **0** =
PCM), `CDDA_TEST_TONE` (fill both rings with a fixed triangle and never fetch —
validates the geometry, the timer and the AICA in isolation; a clean tone means
the loop machinery is sound), `CDDA_RING_TOP` (lower it to move the rings out of
a title's sound RAM), `CDDA_DISDL` (send-level ceiling). The relocatable loader
`make loaders` builds is the PCM double-buffer image; a tone set is a separate
`CDDA_TEST_TONE=1` build.

**A LEVEL LOAD IS THE CONCRETE CASE OF "STOPS CALLING THE GD DRIVER", AND IT IS
SECONDS LONG.** `cdda_service()` runs from the GD server loop and from
`GetDrvStat`, both of which a title reaches only *between* commands — and
`data_transfer_emu_async` serves a whole read without ever yielding
(`GD_YIELD_BETWEEN_CHUNKS = 0`, §4.5, and for good reason). So one read of a
level file is a stretch in which nothing feeds the ring at all: the AICA loops
what it has and the music stalls on a fragment until the title asks for something
else. Reported 2026-09-09 on Snow Surfers, and it is not a clock or a buffer
problem — no ring is big enough for a multi-second read. `GD_CDDA_BETWEEN_CHUNKS`
is the answer and is **on** since 2026-09-12; §4.5 has the argument for why its
call site is safe and why it is not the two flags that killed Sonic Adventure.

**And it stopped being optional the moment the nesting was closed.** Until then
a read was quietly fed anyway, by the interrupt-driven `GetDrvStat` that
`g_gd_in_transfer` now refuses (§4.5) — which is to say the music was being kept
alive by the very bug that froze the console. Measured the session the guard
first ran: `g_cdda_room_min` **0** against 1680349 the run before,
`g_cdda_underruns` 1, `g_cdda_stale` 1, and the listener heard three events, two
of which "struggled to recover". The guard and this flag are one change in two
halves; shipping either alone is worse than shipping neither.

**THE DISC IS CLEAN, AND THE WARNING THAT SAID OTHERWISE FIRED FOR FIVE
SESSIONS.** `audit-audio` was finally run on the image (2026-09-12): *"track 10:
clean (102.6 s, 213 big step(s), largest 54259) — spread evenly across all 588
sector offsets (6.0 sigma at the busiest) — the fingerprint of loud music, not of
a defect"*, and *"no ALIGNED discontinuity in this image's audio"*. The
cross-check is what makes it conclusive: the **live** scan counts the same
**213** steps, so the bytes leaving the host are the image's bytes and every one
of those steps is music. `SlewWatch::verdict()`'s 8-sigma test had always known
this — `describe()` applied it — but the live dump fired on `runs > 0`, so it
named LBA `0x0004e6ce` in every session. It is gated on the verdict now. **A
warning that fires on every session is indistinguishable from one that fires on a
fault.**

**AND WHAT IS LEFT IS A PHASE GAP, WHICH NOTHING MEASURES AND NOTHING
CORRECTS.** The host's trim fixes the **rate**; the **phase** is established once,
at key-on (`CDDA_LAG_SHIFT`), and whatever the residual drift consumes of it never
comes back. Past it the model runs EARLY — the one direction with no tolerance,
overwriting the tail of the half still playing — and no counter here can see it,
because `cdda_aica_half()` and the underrun test both ask the same model. At the
~184 ppm the trim measured, the phase slips 0.26 ms per 1.39 s loop, so a
sixteenth of a loop (86.7 ms) is gone in about **eight minutes of play**.

A sixteenth of a *loop* is an eighth of a *half*, so the tolerance was split
12.5 % early against 87.5 % late — off centre in the direction that hurts.
`CDDA_LAG_SHIFT` is **2** now: a quarter of a loop, half of a half, 50/50, four
times the margin. **This reverses a measured decision and the measurement does
not transfer**: the 2026-09-09 test that rejected a quarter-loop lag was taken
against an **80 ms half**, where 40 ms of lag left 40 ms for a fill facing 64 ms
service gaps and `g_cdda_room_min` went to −24 ms of real clearance. The half is
**693 ms**: the fill is triggered 347 ms in and has 346 ms left for ~100 ms of
fetching, and the run this was changed on measured 532 ms of clearance, so it
becomes ~270 ms. `loaders-lag16` (`CDDA_LAG_SHIFT=4`) is the exact A/B and is
**byte for byte the image that preceded this change** — the md5 matches the
previously shipped `loaders` — so the lag is the only DC-side delta.

**And `CDDA_LAG_SHIFT` did not reach the compiler on the first try.** Its `CFLAGS`
line landed where an `ifneq` on an empty variable swallowed it, and the *only*
thing that caught it was `loaders` and `loaders-lag16` coming out with the same
md5 — §14.19, second time in this file's history, same shape. Build two sets and
compare, every time.

**AND NOTHING EVER ASKED WHETHER THE STORE LANDED, WHICH IS THE LAST
UNACKNOWLEDGED SURFACE IN THE PATH.** Everything else here is arrival accounting
(§11): the fetch counters say the bytes reached the staging buffer, the window
machinery says they were the right bytes, the clock says they were staged at the
right rate. What happens after that is **~3700 P2 stores per half across G2 into
sound RAM, with a FIFO drain every eight and no acknowledgement of any kind** —
and a dropped store in ADPCM is eight samples plus a poisoned predictor on one
ear until the next key-on, which is what a glitch sounds like.

**The sound-RAM probe cannot see it, by construction.** `cdda_probe_arm()` reads
sound RAM *after* the write and keeps that as its baseline, so it answers "did
anyone else change this". A store that never landed is absent from the baseline
**and** from the later read, the XORs agree, and the probe reports clean. That is
why `g_cdda_ram_clobber` sat at 0-1 across eight sessions while the listener kept
hearing events.

`cdda_push_verify()` compares against the **source**, which is still in the
staging buffer at that moment and costs nothing to re-derive: eight word pairs
per sub-fetch, rotating so coverage accumulates over a half, read back inside the
same `g2_lock()` so nothing else touches G2 in between. ~200 µs per half, 0.03 %
of wall time. A **zero** read goes to `g_cdda_push_unread` rather than the
verdict — reads of the AICA from the SH4 come back as `0x00000000` sometimes, and
a dropped store leaves the previous revolution's audio there, which is not zero.
`g_cdda_push_bad` must read 0; the addresses are division-free by construction
(§14.15 — `grep libgcc dcload.map` confirms nothing was pulled in, and `_end`
grew 268 B). `loaders-noverify` is the A/B and is **byte for byte the image that
preceded it**, so the check is the only DC-side delta.

**AND THE COUNTERS FINALLY AGREED WITH THE LISTENER, WHICH NAMES THE TERM TO
CUT.** Measured 2026-09-12 with the store-verify in: `g_cdda_push_bad` **0 in
12896 checks** — the G2 write path is exonerated by a live instrument, not by
silence — while `g_cdda_stale` **2**, `g_cdda_underruns` **2** and
`g_cdda_room_min` **0** against **exactly two glitches heard**. First session in
the whole investigation where the instruments and the ear match. The chain is: a
lost UDP packet (audio ships with no acknowledgement, §16) leaves the window
short, the fetch waits out its whole deadline, the half stays partial, and the
AICA reaches it — 18 failures in 1612 sub-fetches, ~1 %.

The budget says which term is responsible, and it is not the loss rate. A half is
693 ms; `CDDA_LAG_SHIFT` takes 347 of it, so the fill has 346 ms, of which a
healthy fill spends ~120 (13 sub-fetches, two to a service, seven services at the
~17 ms cadence), leaving **226 ms of spare**. A failure cost **60 ms of deadline
plus a 15 ms drain**, so **three failures in one half exhausted it** — and that is
`room_min 0`. The lag increase is what made this binding; the margin is bought
back from the failure path rather than by giving the phase margin up again.

**And the 2026-09-09 reasoning for 60 ms is wrong in its decisive claim.**
*"Giving up is not cheaper than waiting"* assumes the answer is on its way. Since
the host's own give-up went in, it provably is not: the host **refuses** to send
an answer it took longer than `CDDA_GIVE_UP` to produce, so past that point
waiting is not patience, it is pure loss. And a re-ask does not "pay the whole
latency again" — the latency is ~3 ms, with the host's own half logged at 0.18 ms
and `0 over 5 ms` in every window of every session.

So **20 ms** (six times a normal round trip; a failure costs 30 ms against 75, and
the same 226 ms absorbs seven losses instead of three) and a **10 ms** drain,
which is derived rather than tuned: a straggler's window is the loader's deadline
minus the host's give-up, 20 − 15 = 5 ms, covered twice. **`CDDA_GIVE_UP` must
stay below `CDDA_FETCH_DEADLINE_TICKS` — that ordering is an invariant**, and 15 ms
is seventeen times the worst production time this host has ever logged.

**Known limitations, all acceptable for this transport:** a title that stops
calling the GD driver stops feeding the music (isoldr's limit too); the mixer
level is read at each key-on, not tracked live mid-track; the reported FAD leads
by up to one ring (isoldr's leads too); an ADPCM track's odd final sector is
dropped to the silence tail (~13 ms); a play range shorter than two halves with
repeat is an untested edge. And the deep footprint rule (§4.6) still applies: at
the stock base `_end` is above Sonic Adventure's syscall SP, so titles that need
a low base are relocated by the host as before.

---

## The freeze narrative, as AGENTS.md §4.5 stood on 2026-09-12

**A WAIT WITH NO WORKING DEADLINE IS HOW A LOST PACKET BECAME A DEAD CONSOLE.**
`ReadSectors()` armed `timeout_loop` and sat in `bb->loop()`. Two things have to
be true for that to terminate, and in game neither was: the seconds branch is
compared against `PMCR_Delta_Seconds()` and **the PMCR reads 0 once a title
runs**, and the surviving bound, `RTL_IDLE_POLL_LIMIT`, counts poll iterations
**with no frame arriving** — a bound on silence. A stuck read is precisely when
the wire is at its busiest (the host retrying its LoadBinary, CD-DA fetching,
`--diag` polling, late audio answers), so `idle_polls` is reset before it can
approach the limit and **the wait never ends**.

Measured 2026-09-11 on Snow Surfers, pressing Start into a level load. The host
logged five retries of `LBIN 0x0cbe4540 +16384`, then gave up with *"Staying up;
the DC will time out and re-request"* — and the DC never did. Through the whole
freeze: `g_cdfs_read_fails` **0**, `g_cdfs_read_retries` **0**,
`g_idle_polls_max` **0**, `g_cdfs_sync_chunks` flat. `gd_lock_byte` stayed held,
so **every `gdGdcExecServer` was declined — `g_cdfs_sync_reentered` advanced
one-for-one with `g_gd_idx_counts[ExecServer]`, 169 against 169** — and the
title spun on a server that never came back. Frozen on one frame, unrecoverable,
**with the loader itself alive**: CD-DA kept fetching at its natural rate (fed
from `GetDrvStat`, which needs no lock) and `--diag` kept answering. That pair —
a healthy loader and a dead title — is the signature, and
`ExecServer == sync_reentered` is how to read it in one line.

**AND THE FREEZE CAME BACK WITH THE DEADLINE IN PLACE, BECAUSE THE SECOND ONE
IS NOT A WAIT — IT IS A LOCK NOBODY HOLDS.** Measured 2026-09-12 on Snow
Surfers, at the end of the intro: `ExecServer` +582 against
`g_cdfs_sync_reentered` +578 in 11.81 s, the same one-for-one signature — but
this time with `g_cdfs_read_fails` 0, `g_cdfs_read_retries` 0 and **no read in
flight at all** (one non-audio `LBIN` and one `DBIN` in the whole window
against 185 audio ones). The loader was not waiting for anything: CD-DA fetched
at exactly its free-running rate, 17 halves at 1.44/s, and `g_rx_polls` +4.19 M
accounts for only ~0.9 s of the 11.81 — so dcload spent ~92 % of the window
**inside the title**, and the title was polling a server that would never come
back.

Read the two facts together and the state is decidable. `gdGdcGetDrvStat` calls
`cdda_service()` **before** `gd_lock()`, so the music survives a held lock —
which is why every audio counter stayed clean. And `saved_regs_ptr` against
`saved_regs_end` says whether the server context is PARKED (a frame is stored)
or RUNNING (`es_enter` emptied the buffer). **Held while parked is a state no
path in this file can produce**: `gdcExitToGame()` parks and *then* releases,
and every C holder releases within microseconds. It is also unrecoverable by
construction — a failed `gd_lock()` does not clear the byte, and the one
context that would have is the one being declined.

So `gd_lock_watchdog()` detects the state rather than the cause: held, parked,
and `g_gd_lock_gen` — bumped on every C acquire **and** every release —
unmoved for 250 ms. That third clause is what makes it exact, and it is not
decoration: without it the few instructions inside `gdcExitToGame()` and
`es_enter` where the two words are legitimately inconsistent would be
indistinguishable from the deadlock. Breaking it is safe for the same reason it
is detectable — the server is parked, so no stack swap is in flight and
resuming it is what `ExecServer` was already trying to do. It runs from
`GetDrvStat` and `GetCmdStat`, before either takes the lock, so it costs two
loads and a compare per frame. `g_gd_lock_stuck` is both the repair and the
verdict: **if it never moves and the title still freezes, this is not the
mechanism**, and `g_gd_lock_stuck_owner` names who left the byte set (0 = the
server itself, which takes it in `es_enter` and writes no owner).

**The trigger, on the same dump, is the G2 bus and it is NOT fixed.** Three
instruments went to zero or nonsense at once, all of them on G2:
`g_cdda_ch_unread` **+34** (structural AICA scans answered with nothing but
zeros, against 0 for the whole session before), `g_cdda_ram_got 0x00000000`,
and the BBA's own RX ring — `g_rx_hdr_defer` **+6147** with `g_rx_resync` +3
and `g_rx_overflow` +5, i.e. the ring header read back implausible until the
defer limit forced three resyncs. §4.5's known gap, in one measurement: **there
is no `g2_lock()` around CPU reads of the BBA**, so a title's G2 burst and the
loader's ring polling collide, and the ring desynchronises. The watchdog turns
the consequence from a dead console into a quarter-second hiccup; it does not
address this.

**AND THE ANSWER TO BOTH FREEZES IS THE SAME ONE, FOUND 2026-09-12: A CD-DA
FETCH RUNS *INSIDE* THE DISC READ'S WAIT, FROM THE TITLE'S OWN INTERRUPT.**
`bin_info` is ONE window and `pkt_buf` is ONE buffer. `gdGdcGetDrvStat` calls
`cdda_service()` **before** the lock — deliberately, so the music survives a
held lock — and the title runs with IMASK 0 (`go.S`) with its own handlers in
the VBR we hand it, calling that syscall sixty times a second. So while the
loader sits in `ReadSectors()`'s `bb->loop()`, a VBlank handler reaches
`cdda_service()` and starts an audio fetch. `cdda_service()`'s `busy` flag
cannot see it: the outer context is a disc read, not an audio fetch.

Two wounds from the one nesting, and they are exactly the two symptoms:

- **The audio fetch's LoadBinary resets `bin_info`** to the staging buffer, so
  the disc read's window is gone and the host's parts land nowhere. Measured on
  Snow Surfers leaving the intro video: the host asked five times for the
  LoadBinary echo of `0x0cbe4540`, never got one, and gave up with *"the DC will
  time out and re-request"*.
- **`cdda_fetch()` ends by CLEARING `fine_deadline_ticks` and `timeout_loop`,
  not restoring them**, so the disc read's 1.2 s deadline is disarmed under it —
  and `RTL_IDLE_POLL_LIMIT` is gated on `timeout_loop > 0`, so that bound goes
  with it. The wait then has no end at all. **That is why the deadline above,
  which is correct, did not fire**: `g_cdfs_read_fails` 0 and
  `g_cdfs_read_retries` 0 through both freezes were telling the truth.

And it explains the counters that made the first diagnosis so hard. The lock was
never contended this time (`g_cdfs_sync_reentered` flat against `ExecServer`
+646) because the server was not holding it — the loader was parked in a read
with interrupts live; CD-DA kept fetching at exactly its free-running rate
because *it* was the thing still running; and `--diag` timed out against the
counter range while the **control read of the loader base still answered**,
which is a loader answering from inside a wait it can never leave.

`g_gd_in_transfer` is the fix: non-zero across both GD waits, and
`cdda_service()` declines while it is set (`g_cdda_svc_in_gd`). The music stops
for the duration of a read, which is the limitation this transport already has
(§4.13) and what `GD_CDDA_BETWEEN_CHUNKS` exists to soften — rather than the
read stopping forever. The **stuck-lock watchdog stays and is not what fixed
this**: `g_gd_lock_stuck` read 0 through this freeze, correctly, because the
lock was not the problem here. Its falsifier held.

Both waits here now arm a TMU2 fine deadline (`GD_READ_DEADLINE_TICKS`), the
clock that demonstrably works in game — CD-DA's fetch deadlines ride it and
`g_fine_timeouts` proves they fire. Expiring costs a failed chunk, which
`data_transfer_emu_async()` retries **after handing the title its frame back**,
which is exactly what the host announces. TMU2 is started in `dcload.c`'s timer
setup now rather than by CD-DA alone, since the read path must not depend on
music playing to have a clock.

---

## CD-DA notes from AGENTS.md §16 (the Rust host), as they stood on 2026-09-12

- **Audio ships with no acknowledgement round trips, and that is an asymmetry,
  not a shortcut.** `send_data_one` waits for the LoadBinary echo and then
  probes with DoneBinary; both earn their keep for a disc read, where a lost
  LoadBinary silently voids the whole window. Measured 2026-08-31 on Snow
  Surfers, with the host timing the freeze from its own side: a 7056-byte audio
  fetch froze the title **3.01 ms (disc 0.04, wire 2.98), 24.9 times a second =
  7.5 % of wall time** — and the payload is 0.56 ms of wire time, so **2.4 ms of
  the 3.0 was the two round trips**. `send_audio()` drops both, because every
  failure that guards against degrades to a click: a lost part leaves ~1.4 ms of
  the previous ring revolution, a lost LoadBinary makes dcload refuse the parts
  and replay ~40 ms of stale ring. **The one packet it does not drop is the
  ReturnValue** — that is what releases dcload from `bb->loop()`, and losing it
  costs `RTL_IDLE_POLL_LIMIT` idle polls, seconds rather than milliseconds; it
  is exactly as unreliable as it was before, which is the point.
  **The number is the host's own half** -- request in to ReturnValue out -- not
  the whole freeze, which also carries both flight times and whatever dcload
  drains from its RX ring first. While the path had blocking round trips the two
  nearly coincided; now they do not, so 0.3 % is a floor on the freeze rather
  than a measurement of it. And the saving **shrank the loader's listening
  window by the same factor**: `bb->loop()` is now entered 0.13 ms in every 40,
  a 0.3 % duty cycle, so `--diag` and anything else the host asks now waits many
  fetch windows to be heard and is served inside an audio fetch.
  One probe every 256 fetches still asks, so the host is not blind to a link
  that has begun dropping half the audio — a permanent crackle is otherwise
  blamed on the ring, the AICA, or whatever encoding is in use.
  **The instrument had to move to the host to measure any of this.** dcload only
  looks at the wire from inside `bb->loop()`, and once a title has finished
  loading the only thing that enters it is the audio fetch — 25 windows a
  second, a few percent duty cycle — so `--diag` and `dc-counters.py` sample a
  console that is not listening and time out (`0/1464 bytes landed`). §11's rule
  in its purest form; the host is the other end of the freeze and can time it
  with a clock the console never touches.

- **THE HOST OWNS THE CONSOLE'S AUDIO CLOCK, BECAUSE THE CONSOLE HAS NOTHING TO
  MEASURE IT WITH.** The loader paces its ring off a TMU on the SH4's crystal
  with a compiled-in period, and the AICA's own timers do not advance as read
  from the SH4 (§4.13, measured). So this host times the audio it stages against
  its own clock -- **by disc position, not by fetch count**, since a re-asked
  sub-fetch repeats an LBA -- and returns the correction in parts per million in
  the `size` field of the audio fetch's ReturnValue. The estimate is of one
  constant: total audio over the sum of each epoch's seconds divided by the scale
  then in force, so a longer session is strictly a better one and noise does not
  random-walk the answer. First window 90 s, then 300 s; `DCLOAD_CDDA_TRIM=0`
  turns it off. The line to read is `CDDA clock: the loader's model ran N ppm
  slow over T s of free-running stream`.
  **Only free-running stream counts, and only whole halves of it** -- §4.13 has
  the two measurements that forced each, and `CddaClock` carries a unit test per
  defect. An estimate outside 1.5 % is refused with a warning rather than sent:
  a number that far out is not a clock, and the loader gates it again at the same
  1.5 % in case this side ever sends one anyway.
- **THE PANEL'S HEADER IS THE MEASURED INTERVAL, NOT THE ONE THAT WAS ASKED
  FOR.** Every rate anyone reads off `--diag` is a delta divided by that number,
  and the setting is not that number: a sample is answered from inside
  `bb->loop()`, which during CD-DA is a fraction of a percent duty cycle, so a
  request made on a 2 s timer is regularly served most of a second late.
  Measured 2026-09-03 on Snow Surfers: 49 audio fetches in a header reading
  "2.0s" is 24.5/s against the 18.75/s real time needs — an apparent **31 %
  over-fetch**, which would mean the writer laps the ring every fifth of a
  second. The real gap was 2.61 s and the loader was exactly in real time.
  `Probe` now timestamps each decode and prints the gap the deltas are really
  against (`2.61s`; `2.0s set` before the first pair). §11's rule applied to
  the instrument panel itself — and the fix is what makes `g_cdda_ticks` /
  `g_cdda_played` (§4.13) mean anything, since those exist precisely to be
  divided by a clock the console does not own.
- **`d` switches the counter panel on and off mid-session, and off means the
  console is not asked anything.** `--diag` now only chooses the starting state;
  the probe is built in every session, because none of it can be built later —
  its image check is a blocking round trip and the console is idle only in the
  seconds before the title starts. Off used to mean *folded away*, which was the
  worst of both: every sample is a `SBIQ` dcload answers from inside
  `bb->loop()`, so a hidden panel went on perturbing exactly the window someone
  hides it in order to measure (§11's warning, in the instrument itself).
  The panel and `stackwatch` follow the switch in opposite directions — their
  ranges overlap, both sinks claim by address, so **an inactive sink claims
  nothing** and exactly one is ever on the wire.
  **`w` is how the numbers leave the machine.** The panel is redrawn on every
  sample and before every log record, and a terminal drops a selection whenever
  the cells under it change — so selecting it is a race against the next
  repaint. `w` prints the whole set as plain text above the live region, where
  nothing redraws it, and appends it to `dcload-diag.txt`. **Zeros included**,
  because in a paste of the panel "absent" and "zero" look the same, and the
  difference is a feature compiled out versus a hazard that is not happening.

- **The link's round trip is measured at rest, once, and logged.** dcload times
  a CD-DA fetch with TMU2 and the host times its own half of the same fetch;
  measured 2026-09-01 on Snow Surfers those are **3.31 ms and 0.13 ms**, and
  nothing said whether the 3.2 ms in between is what this link costs or
  something the host is doing. `measure_rtt()` is the third number: five
  four-byte `SBIQ` round trips on the idle console, minimum reported, before
  anything is uploaded. **`ping` cannot do this job**, although dcload does
  answer ICMP (`net.c`, `process_icmp`, and it is correct): the loader only
  looks at the wire from inside `bb->loop()`, so once a title runs a reply waits
  for the next fetch — up to 40 ms of the loader's own scheduling, reported as
  if it were the link.

- **THE IN-SESSION AUDIO AUDIT WAS INSIDE ITS OWN BLAST RADIUS, AND IT IS GONE.**
  When `SlewWatch` flagged a discontinuity, the `ReadAudio` arm re-read the
  suspect LBA **on the syscall path**, with the title frozen waiting for that
  very fetch. Re-reading a distant LBA can build a whole deflate index:
  measured 2026-09-11 on Snow Surfers at **223 ms**, which then tripped this
  host's own late-answer guard and made it drop the audio it was producing
  ("CDDA read of LBA 0x0004e6ce took 230 ms, past the loader's deadline"). A
  diagnostic that freezes the title and destroys the sample it is examining is
  §11's rule in its purest form.
  **AND WHAT REPLACED IT PRINTED AN EMPTY FIELD FOR FIVE SESSIONS.** The byte
  dump indexed `bad_frame * 4` — a **PCM** frame offset from the scan — into
  `answer`, which is the **ADPCM** payload: one byte per frame against four. So
  for any hit past frame 588 the slice clamped to nothing and the warning read
  `the discontinuity, as bytes: LBA 0x0004e6ce +2352  ||` with both fields
  blank, the `+2352` being `pcm.len()` after the clamp rather than an offset.
  An instrument that prints an empty field looks exactly like one that found
  nothing, and this is the instrument aimed at the last unexplained glitch.
  `SlewWatch` captures the 32 bytes **where the scan is**, the only place a PCM
  frame number can index the right buffer, and the line names `bad_lba` and the
  length it actually has.
  `the_discontinuity_dump_is_not_empty_past_the_adpcm_length` places its hit at
  frame 1200 on purpose — past the ADPCM length, which is where the old code
  failed — and asserts 32 bytes come back.

  **And its verdict was not evidence — the fourth instrument here falsified by
  its own control.** It warned *"this host's random-access read of the image is
  wrong"*. The deflate reader is now tested against **ground truth** over an
  8 MB stream, read warm and again after the cursor pool has been forced to turn
  over so the next read must restart from a checkpoint
  (`a_warm_read_and_a_checkpoint_restart_agree_with_the_original`,
  `src/disc_formats/deflate.rs`): both match the original byte for byte. What
  differed was the second **reader** the re-read built — the session log shows a
  fresh index being built right there — so the comparison was never about the
  deflate path at all. The live path now names the LBA and points at
  `dcload-ip-rs audit-audio`, which maps the whole disc offline with no console
  attached and nothing frozen.
- **`read_audio()` serves raw 2352-byte sectors** for `DC23`, refusing a data
  track rather than handing back sync bytes and ECC as if they were samples.
  A refusal is answered −1 and the loader turns that into silence: a title
  that asks for audio this image cannot serve should lose its music, not its
  disc. `--no-cdda` refuses every one of them, which is the A/B switch.
  **CDDA reads are excluded from `game-memory.tsv`** — their destination is
  dcload's own staging buffer, and marking it would teach the map that the
  title writes where the loader lives.


## Status log, 2026-09-12 to 2026-09-19 (moved from AGENTS.md 4.13)

Kept verbatim when the engine was simplified on 2026-09-19. Several instruments
named here (the store read-back, the clobber probe, the sound-RAM activity map,
the register watch, `CDDA_POS_PROBE`, the TMU1 check, the half-gap statistics,
`CDDA_DMA_DEFER`/`CDDA_OVERRUN_MUTE` as flags) were removed that day, once the
cause of the glitches was found; `loaders-prerefactor` (`3a455282`) is the last
build that has them, and git history has the code.


Tested on one console (BBA) with Snow Surfers PAL, loader relocated.

- Music plays. No freeze has been recorded since `g_gd_in_transfer` +
  `GD_CDDA_BETWEEN_CHUNKS`.
- **The 60 ms fetch deadline was the term that cost halves, and cutting it
  worked.** At 60 ms: `g_cdda_stale` 2, `g_cdda_underruns` 2,
  `g_cdda_room_min` 0, two glitches heard, 18 sub-fetch failures in 1612.
  At 20 ms, same ~1 % failure rate (6 in 500): `stale` 0, `underruns` 0,
  `room_min` 866642 ticks ≈ **277 ms of clearance**. `g_cdda_push_bad` 0 in
  12896 checks says the G2 write path is sound.
- **One glitch survived that, and it was in `cdda_prime()`.** 6 failures
  against 5 partials: one failure was prime's, which keyed on with a hole
  (prime counts no partial). The host log names the cause — `CDDA read of LBA
  0x0004e352 took 140 ms, past the loader's deadline` — the host was building
  the audio track's deflate index *inside the first audio read of the track*,
  so the first sub-fetch of every track failed and the music began with 693 ms
  of quiet floor. Fixed at both ends on 2026-09-13: prime retries
  (`CDDA_PRIME_RETRIES`), and the host warms the audio tracks' indexes on a
  thread (§16). **Not yet measured on hardware.**
- **Both fixes held, and the transport is now flawless**: a session with
  `g_cdda_fetch_fails` **0** in 520 sub-fetches, `partials` 0, `prime_holes` 0,
  `stale` 0, `underruns` 0, `g_fine_timeouts` 0, `g_rx_missed` 0,
  `room_min` 278 ms, and no `past the loader's deadline` line in the host log.
  Fetches ran 1.00× real time end to end (500 reads = 26.6 s of audio in 27 s).
- **One glitch is still heard, at ~25 s into the music each run, while the
  title renders 3D**, and the TMU2 counters named the mechanism at the first
  attempt: `g_cdda_svc_gap_max` **304.7 ms** — the title makes no GD call at
  all for that long, so nothing in the loader runs — one half completing
  1048.5 ms after the previous instead of 693, and `g_cdda_room_min` down to
  **61.8 ms** of the 346 ms budget. `g_cdda_tmu1_bad` 0 with `tcor` = `end_tm`
  and `tstr` 0x07 clears the model itself. Response: `CDDA_LAG_SHIFT` 2 → 3,
  which moves 174 ms from the early margin to the fill budget (`loaders-lag4`
  is the revert). **Not yet measured on hardware.**
  - That session also produced the first `g_cdda_ram_clobber` (1 in 49, at
    `0x00194cd8`, want `0xf100eb4c`, got 0) alongside two `g_cdda_ch_unread` —
    so it may be a partial bus failure rather than a foreign write. The probe
    now re-scans before deciding, and covers the whole half.
  - A `PAUSE` in both sessions was ruled out, not assumed: `cdda_service()`
    requires `state == PLAYING`, so a PAUSE during playback would have stopped
    the fetches, and they never stopped — it precedes the PLAY.
- **With the lag at an eighth, the starvation is absorbed and the glitch
  remained** (third 2026-09-13 session): `g_cdda_svc_gap_max` 305.5 ms (the
  title's gap, unchanged), `g_cdda_room_min` **271.4 ms** (was 61.8),
  `half_skips` 0 — and one glitch that "struggled to recover", with
  `g_cdda_ram_clobber` 1 at the same `0x194cd8`, `want 0x9c07831c`, `got` 0,
  **`again` 0**: the second scan confirmed stable wrong bytes, not a bus fault.
  A zero XOR over both ears with non-idle words is a constant fill. So the
  starvation was a real fragility but not the glitch, and the glitch is the
  title writing into block 25. Response: `CDDA_RING_TOP` `0x1a0000` →
  `0x150000`, `g_cdda_ram_when` to time the clobber, the DSP ring read every
  half. **Not yet measured on hardware.** Expected: `ram_clobber` 0 on the
  default set, back to 1 near 36 halves on `loaders-ring1a`, and `srdirty` bit
  25 set once the map revisits block 25 (pass 57, ~40 s of music).
- **Moving the ring removed the clobber and not the glitch** (2026-09-14):
  default set, `g_cdda_ram_clobber` 0 in 46 checks, `room_min` 450.7 ms, every
  engine counter clean, `svc_gap_max` 305.2 ms as always — and the glitch
  heard. So the title does write block 25 at that scene, but that was not what
  was heard (or not all of it). Ruled out the same day, from code: the AICA
  restoring the ADPCM decoder state at every loop wrap — flycast's `sgc_if.cpp`
  does that only for PCMS 2, and the rings use PCMS 3 ("long stream"), as
  isoldr does. What is left unobserved is the title writing our channels or
  the mixer between two halves, hence the register watch. **Not yet measured.**
  The decisive control has not been run either: whether the glitch is there
  with no dcload at all (the disc, or flycast playing the GDI) — §14.18.
- **The dump taken during the glitch named it** (2026-09-14, halves 41-42):
  `g_cdda_ram_clobber` 1 at the rings' **new** address `0x141120` (`got` 0,
  `again` 0 — the same signature as at `0x194cd8`), `g_cdda_srdirty` newly set
  for a block the map was scanning at that half, `g_cdda_reg_first` `0x1f00`
  (channel 62's control word) reading 0 against `0x4394` on two compares 50 ms
  apart, `g_cdda_ch_unread` 2. Three unrelated places wrong at once, following
  the rings wherever they go, is one event: the title uploads to sound RAM by
  G2 DMA during its 305 ms without GD calls, and SH4 accesses to the AICA do
  not reach it meanwhile. Reads echo a stale bus value (hence a zero XOR over
  non-zero words — the "constant fill"), and stores are presumably lost, which
  `cdda_push_verify()` could not see because its read-back echoed the word just
  written: so "the G2 write path is sound" (12 896 clean checks) held only
  outside DMAs. Response: `CDDA_DMA_DEFER` (rule 6), and a register read
  between each store and its read-back so an echo can no longer pass.
  **Not yet measured.** Expected: `g_cdda_dma_defer` > 0 around half 36-42 and
  no glitch on `loaders`; on `loaders-nodefer`, the glitch back, with
  `g_cdda_push_bad` > 0 at the same halves.
- **The deferral never engaged, and the probes saw nothing** (2026-09-16,
  `loaders`, the user did not say whether the glitch was heard):
  `g_cdda_dma_defer` 0, `_push_defer` 0, `g_cdda_push_bad` 0 in 7904 checks
  that an echo can no longer pass, `g_cdda_ram_clobber` 0 in 73,
  `g_cdda_srdirty` only our own block, `g_cdda_ch_unread` 0, and the register
  watch's first confirmed change at half 75 — where the title stopped the music
  at 52.6 s (the fetches stop there, track 10 is 102.6 s long), which is
  harmless. `svc_gap_max` 305.3 ms and `room_min` 446.8 ms as before. So the
  DMA reading of the previous dump is unconfirmed, and the one event those
  probes caught is not what is heard every session.
- **What is in every session is in the stream itself.** A glitch at the same
  point of the music each run, with every counter clean, fits a defect in the
  bytes the host sends — which no loader instrument can see, since sound RAM
  holds exactly those bytes. The host's encoder assumed a decoder that applies
  a nibble's contribution unclamped; flycast (`sgc_if.cpp`, `DecodeADPCM`) and
  Sega's own encoder clamp it to `0x7fff`. Simulating the host's encoder
  against both decoders on the disc's own track 10 (the intro, FAD `0x4e352`):
  the two readings part twice, at **11.9 s** (both ears; the right stays offset
  for 2.7 s, 4-9 dB SNR over the first ~0.5 s) and at **46.2 s** (left, 0.4
  s). ADPCM's own error never drops below 20 dB SNR anywhere in the track,
  and nothing happens at 25 s. In-game tracks are far worse (track 14: 14 260
  such nibbles). Response, on the host: never emit a nibble the two readings
  disagree on (0 differing samples on tracks 10 and 14 afterwards, ~0.5 dB rms
  cost on the loudest tracks only). **Not yet measured on hardware.** If the
  silicon clamps, the glitch was at ~12 s (and ~46 s) and is gone with the new
  host; if it is still there, the next suspects are the ones above.
- **With that host, more glitches, and a transport stall** (2026-09-16, second
  session, intro then the character select): 3-4 glitches heard in the intro
  and one in the character select music. `g_cdda_fetch_fails` **22** (0 in the
  previous sessions), `partials` 22, `stale` 1, `half_skips` 1,
  **`half_gap_max` 2063.6 ms** — three half-periods between two completions,
  while `svc_gap_max` stayed at 305.9 ms: the services ran, and the fetches
  failed for ~1.4 s. `g_fine_timeouts` 44 = 22 fetch deadlines + 22 drains,
  `g_rx_missed` 0: the answers did not come in time, host or network side. Not
  the new encoder (0.39 ms a request in the debug build, against 0.28 ms for
  the crate's). First title DMA seen (`g_cdda_dma_defer` 5, during the
  character select load, with `srdirty` blocks 0-10), `push_bad` 0 in 23 192.
  The trim landed at 314 ppm (184 before). Two findings follow. (1) One stall is
  several glitches in ADPCM (§ Silence, never garbage). (2) The key-off never
  silenced (RR = 0). Response: the overrun mute, the asynchronous prime and
  RR = `0x1f`. **Not yet measured**; the host log of that session was not seen,
  so what stalled is unknown.
- **The mute works, and the stall is one host-side LBA** (2026-09-16, three
  runs, `loaders` `14bf6448…`). Heard: run 1 a glitch at ~12 s, not silent
  (unsure); run 2 a glitch with a short silence at 40 s; run 3 a short
  silence at 16 s and an audible glitch at 58 s. Run 3's dump: `fetch_fails`
  19, **every one on the same LBA** (`g_cdda_fail_lba` = `g_cdda_mute_lba` =
  `0x4e8ce`, 18.7 s into track 10), `g_cdda_mutes` 1 with `mute_late_max` **0**
  (caught before anything wrong played) and `start_max` 105.8 ms (the
  silence), `stale` 0, `svc_gap_max` 307.9 ms, `push_bad` 0 in 11 336. So the
  silences are the mute doing its job on a transport stall, and the stall
  repeated on one LBA for ~570 ms. The host explains the repetition: it read
  the disc **before** consulting the encoder's history, so a slow read made
  every re-ask slow, and each was dropped past `CDDA_GIVE_UP`. Response on the
  host: re-asks answered from the history first, the late-answer warning split
  into disc and total time, and the memory-map file written outside the lock
  the disc-read path takes. The glitch at 58 s matched no counter: the store
  read-back sampled 2.7 % of the words, so the loader now reads back and
  repairs every word, and counts G2 FIFO waits that give up (after which
  stores are lost). **Not yet measured.** The host log of these runs was not
  seen.
- **Flycast glitches too** (2026-09-17, the user, with the GDI image below and
  `loaders-flycast`; no dump or timings yet). That rules out everything
  hardware-only: the G2 bus and DMA read artefacts, the silicon's ADPCM
  clamp, the release rate's analogue behaviour. Checked in flycast's source
  instead: the PCMS 3 loop plays all 61 152 samples (`ca & ~3 >= LEA`) and
  resets the decoder only at key-on, as the loader assumes. **But flycast's
  clocks are exact** — TMU Pck/16 = 200 MHz >> 6 = 3 125 000 Hz, AICA = 200 MHz
  / 4535 = 44 101.4 Hz — so `TICKS_X8192` is **2622 ppm short** there (KOS's
  measured 199 499 520 Hz main clock puts the console at ~150 ppm): the model
  gains 3.6 ms a loop, exhausts the 173 ms early margin after ~66 s and
  starts refilling the playing half after ~105 s, before the host's trim can
  land at 90 s of a stable run. That explains glitches late in long tracks
  under flycast, not the intro's. Response: `CDDA_POS_PROBE` to measure the
  model against CA directly, and static A/B sets for flycast
  (`loaders-flycast`, `-pos`, `-pcm`). The cheapest decisive control is now
  also the easiest: the original GDI in flycast, without dcload (§14.18).
- **The control is clean, the loader is clean, and the glitch is still
  heard** (2026-09-19, flycast). The original GDI without dcload: no glitch.
  With `loaders-flycast-pos`: a glitch at ~30 s "with audible corruption
  staying for multiple seconds", and corruption near the end of the intro.
  Dump at ~44 s of music: `fetch_fails` 0, `mutes` 0, `stale` 0, `room_min`
  450.5 ms, **`svc_gap_max` 16.6 ms** (flycast never starves the loader),
  `push_bad` 0 in **248 724** words read back (every word), `fifo_timeouts`
  0, `reg_changes` 0, `ch_stolen` 0, `ram_clobber` 0 in 63. The probe
  confirmed the clock arithmetic to the sample: `pos_last_err` **5140**
  (116.6 ms) at 44 s against 5122 predicted from 2622 ppm; its
  `ahead_max` 7641 was an artefact — `cdda_true_elapsed()` returned the lag
  during the key-on head start instead of the time since key-on, fixed. At
  30 s the drift was 79 ms of the 173 ms margin, so it is not the 30 s
  glitch; the flycast sets now carry the exact constant. Ruled out offline
  the same day: the codec (the host's own `adpcm.rs` fed the loader's
  4-sector requests, decoded with flycast's `DecodeADPCM` verbatim: 23-29 dB
  SNR from 26 to 36 s, no 10 ms window within 10 dB of its signal anywhere),
  the host's zip reader (1923 requests byte-exact against the loose
  `track10.raw`, with data reads interleaved), and an overlap with the
  title's sound banks (every `.MLT` unit table on the disc: the highest,
  `PFM1.MLT`'s stream buffers, ends at `0x13e0c0`; the rings start at
  `0x141120`). What remains is outside every instrument: flycast's own
  speed, the host's stream (its log), or an event nothing samples.
- **A recording named it** (2026-09-19, same day). The user confirmed the
  offline decode sounds clean and `loaders-flycast-pcm` does not glitch, and
  recorded flycast's output (WASAPI loopback, 44.1 kHz). Aligned against the
  exact decode it matches to −25..−35 dB with a **constant** lag — flycast
  dropped no sample — until **25.653 s = 37 × 30 576 samples, the start of a
  half**. From the second sub-fetch of that half on, the right ear is
  explained to −33..−45 dB by the right bytes decoded from a wrong state
  (step 675 against the encoder's 1970: a tenth of the level plus an offset,
  for seconds); the left falls back onto the encoder's state after 250 ms.
  The first slot held, in both ears, **the last sub-fetch of the previous
  half** (−27/−34 dB; every other candidate — the previous loop's content,
  the quiet floor, zeros, the other ear — scores −7 dB or worse). So the
  fetch for that slot "succeeded" without its data: the host had encoded it
  (the left re-converges only onto a state that includes it), the loader
  kept the staging buffer from the fetch before, and the store read-back
  compared the ring with that same buffer. It is the first fetch after the
  idle gap between halves, when the RX ring is fullest (the dump before had
  `rx_overflow` 10, `rx_missed` 19). Response: fetch integrity item 5 above.
  **Not yet measured.** It also fits every hardware glitch that came with a
  clean dump.
- **Flycast image**: flycast opens no bare `.iso` (only cdi/chd/gdi/cue), so the
  image is a GDI whose track 3 is an ISO at LBA 45000, with IP.BIN `GD-ROM1/1`
  and the **unscrambled** bootstrap (`target-src/1st_read/loader.bin`: a GD
  boots 1ST_READ.BIN as is), static IP 192.168.1.130, built in a copy of the
  tree. Deployed as `C:\Users\arnod\Documents\Dreamcast\dc-load\dcload-ip-gdi\`.
- The trim applied 184 ppm after its first window.
- The disc's audio is clean (`dcload-ip-rs audit-audio`); the host's live
  "discontinuity" warnings were loud music.

Open questions and limits:

- No phase servo (above). Long sessions without a PLAY/seek rely on the trim.
- A title that stops calling the GD driver stops the music (isoldr too).
- The mixer level is read at key-on only.
- An ADPCM track's odd final sector is dropped (~13 ms). A play range shorter
  than two halves with repeat is untested.
- Code defects noted in `cdda.c`, not fixed: `CDDA_TICK_ADJ` overflows;
  `cdda_clear_rings()` and the PCM `cdda_silence()` wait for the G2 FIFO every
  16 stores instead of 8.


## 2026-09-20 -- the end of the double buffer

With the loop fixed, a level played its track for more than one pass for the
first time, and the user heard what that costs: "a small lag spike every ~1
second... I miss like 4-5 frames". Not a fault -- every CD-DA counter was
clean -- but the schedule.

**The measurement was the period.** 693 ms is the half. The double buffer only
refilled on a half boundary, so the 13 round trips of a half (~3 ms each, 2 per
service call) landed in ~7 consecutive frames at ~6 ms apiece, then 477 ms with
no network at all. A frame with less than 6 ms of slack is dropped whole, which
is how 39 ms of work per half becomes 4-5 dropped frames.

**Why pacing alone could not fix it.** While the fill is triggered by a buffer
boundary, the margin and the burst are the same quantity: the fill of a half
may start only when the AICA leaves it and must be complete before the AICA
returns, one half later, so filling at exactly the audio rate finishes with
zero margin and every millisecond of margin has to be fetched ahead of time.
With N buffers the same holds; only the granularity changes.

**What replaced it.** One write head (`cd.write_pos`) and a target lead
(`CDDA_LEAD_FETCHES` = 20 of `CDDA_RING_FETCHES` = 26). A sub-fetch is due when
the lead is short, so the fetches come one every 53 ms -- one service call in
three -- and the margins are constant instead of oscillating: 893 ms of audio
ahead of the AICA (the lag comes off it) and 493 ms of already-played ring
behind the write head. The ring did not change size. Gone with the halves:
`cur_buff`, `fill_pos`, `half_tm`, `half_mark`/`half_marked`, `CDDA_VISIT_SLACK`,
`cdda_overrun_near()`'s three cases and the drain's crossing counter. Added: a
service-gap limit on TMU2 (838 ms), because the lead is a difference modulo one
loop and cannot see itself gone past zero.

**How it was checked.** The real `cdda.c` was compiled for the PC against a
simulated AICA, TMU1/TMU2 and host. The harness was disposable and is not
kept; rebuilding it is a script that rewrites the `#define` lines of the
hardware macros (`SNDREG32`, `AICA_RAM`, `G2_*`, `aica_dma_busy`, the `TMU_*`
registers) to point at arrays and variables, plus a `bb->loop()` that answers
the LBA it finds in `pkt_buf` after a chosen delay and a virtual Pck/4 clock
the whole thing is driven from. Nothing in the logic is re-typed. Two traps:
reading TMU2 has to advance that clock, or the key-off spin in
`cdda_channels_stop()` never ends, and the TOC request passes a truncated
64-bit pointer, so `cdda_toc` has to be made non-static and filled directly. Over 60 s of
virtual time at one service call per 16 ms: 1210 sub-fetches, at most 1 per
call in the steady state (2 only while catching up), inter-fetch gap 22..179 ms
(179 is the key-on head start, once), true lead 891..1064 ms, 0 mutes.
A 305 ms service stall every 2 s: lead down to 671 ms, 0 mutes. 700 ms every
5 s: 279 ms, 0 mutes. 1000 ms every 5 s: the TMU2 gap limit fires, as designed.
One fetch in seven failing: lead 872 ms, 0 mutes. Track ends are reported
within 1.3 ms of the audio's length, and three passes of a 200-sector range
end at 7993 ms against 8000.

**The host had to be re-taught.** `CddaClock` found its window boundaries in
the idle tail between two halves; a paced loader never produces one, so it
would have banked nothing and never trimmed. It now banks 2 s segments, each
of which must itself look like real time. That test is also what refuses a
**catch-up**: an interval longer than `CDDA_TRIM_GAP_MAX` is dropped, but the
audio it owed arrives in a burst just afterwards, and counted it is worth
3000 ppm -- ten times the error being resolved
(`the_catch_up_after_a_long_gap_is_not_free_audio`, mutation-checked).

Not yet measured on hardware. The A/B loader sets, including the last one with
the double buffer (`38e5cfcd`), were deleted the same day at the user's
request: only `loaders` is kept, and a variant is one `make loaders` away.
