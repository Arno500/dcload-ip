# CD-DA: the crackle investigation (historical)

> This is the record of the 2026-08-29 -> 2026-09-05 investigation into the
> CD-DA crackle, as it stood in AGENTS.md §4.13 before `cdda.c` was rewritten
> on 2026-09-06 to the double-buffer model (see AGENTS.md §4.13 for the current
> code). It is kept because it records what was measured and eliminated -- every
> counter, every false positive, every hypothesis killed by its own falsifier --
> and those measurements are still true of the transport and the AICA. It does
> NOT describe the code that is present: the integrator pacing model, the 53
> counters, the position servo, the slew scan and the tone-stream build it
> discusses were all replaced. Read it for the history, not for the design.

---

### 4.13 CD-DA: the loader is the drive

A GD-ROM carries its music as ordinary audio tracks and a title plays them by
asking the GD driver for `CMD_PLAY_TRACKS` / `CMD_PLAY_SECTORS`. There is no
drive here, so dcload used to answer "COMPLETED, and the drive is spinning" —
honest about the contract, silent about the music. `cdda.c` is isoldr's answer
(`loader/cdda.c`, 1589 lines) for this transport: read the audio track, feed
two AICA channels.

**The table of contents is where a title learns it has music, and ours was a
stub.** `build_dc_toc` on the host reported one data track — two when the
low-density area is separate — from `start_sector()` and `num_sectors()` alone.
Measured 2026-08-29 on Snow Surfers: the disc has **19 tracks, 16 of them
audio**, and the title was being told it had one. The `CTRL` nibble (4 = data,
0 = audio) is the entire signal, so no TOC means no `PLAY` command, ever,
whatever else works. The host now builds the real table from
`DiscFormat::toc_tracks()` (GDI and CDI), **per area** — a GETTOC asks for the
CD part or the GD part, and this disc's music is in the GD part. An image that
cannot enumerate its tracks still gets the old table byte for byte, which is
the regression guard for every title that works today.

What this keeps from isoldr, and what it replaces — `cdda.h` states each with
its reason:

| isoldr's option | here |
| --- | --- |
| `SRC_DMA` / `SRC_PIO` — read the track off IDE/SD | the source is the **network**: `CMD_CDDAREAD` (`DC23`), raw 2352-byte sectors, synchronous like every other read |
| `DST_DMA` / `DST_SQ` / `DST_PIO` — push PCM to sound RAM | **PIO**, 32 bits at a time with a G2 FIFO drain every eight. Needs no setup, cannot collide with the BBA's G2 traffic through a DMA channel we do not own, and 176 KB/s against G2's tens of MB/s is not close |
| `POS_TMU1` / `POS_TMU2` — derive position from a timer | read the AICA's **real** play position (`0x280c`/`0x2814`). No timer to own, no drift — and `perfctr.c` has counter 1 anyway |
| `CH_ADAPT` / `CH_FIXED` | **fixed**, channels 62 and 63 — and note **adapt is isoldr's default** (`CDDA_Init`, `cdda.c:947`: fixed only when the bit is set). Fixed is right here because it is what this title's own preset asks for: RIPPIN' RIDERS (Snow Surfers' US name) carries `cdda = 0x00022110` = `SRC_DMA \| DST_DMA \| POS_TMU2 \| CH_FIXED` |

**Where it runs, and the limit that follows.** `cdda_service()` is called from
exactly isoldr's two no-IRQ contexts: the GD server's dispatch loop and
`gdGdcGetDrvStat` (`loader/syscalls.c:866` and `:1067`). Both are **top level of
a syscall**, which is what lets the fetch transmit — §4.5 forbids transmitting
nested inside a `bb->loop()`. A title's frame loop calls `GetDrvStat` about
sixty times a second and the ring holds 0.37 s, so the margin is two orders of
magnitude. The limit is the mirror of that: **a title that stops calling the GD
driver stops feeding the music**, and after one ring it goes quiet.

**Why this does not need the interrupt hook, which is not a dodge.** isoldr's
`use_irq` exists for CDDA largely because isoldr uses **AICA DMA**: it has to
hide the AICA-DMA completion interrupt from the game and hand it back
(`aica_dma_irq_hide`/`_restore`), and it drives `CDDA_MainLoop()` from the ASIC
handler. Writing sound RAM with the CPU raises no such interrupt, so that whole
class of arbitration does not arise. And an interrupt would not extend playback
anyway: the fetch cannot run there — `pkt_buf` belongs to whatever the interrupt
suspended — so an IRQ-driven service could push only what is already staged,
which is 40 ms. `cdda_service_irq()` is that entry point, deliberately
non-transmitting; §14.20 has what a real VBR hook would still take.

Tuning constants, all in `cdda.c`:

| Constant | Value | Note |
| --- | --- | --- |
| `RING_BYTES` | 32768 | per channel: 0.372 s at 44.1 kHz. In sound RAM, at the top of it, so it costs no main RAM at all |
| `TICK_TO_SAMPLE_Q20` | 3699 | **the pacing clock is TMU2, not the AICA's play position.** Reading the channel position (`0x280c`/`0x2814`) is what isoldr compiles only under `HAVE_CDDA_TEST`; its production path paces from a timer, and this is why. Measured on console: with the position as flow control the engine fetched **3.4× real time** and starved the title. `0x280c` is the monitor *select* register and the game's own sound driver writes it, so what comes back is another channel's position — and a junk position makes `room >= headroom` true 86 % of the time, which turns the fetch rate into the service-call rate. `setup_machine()` already leaves TMU2 programmed at Pck/4 and stopped; DreamShell's preset for this title asks for `POS_TMU2`, which is isoldr saying the game does not use it |
| `MAX_FETCH_PER_SERVICE` | `2` | belt and braces after that runaway: a wrong flow control then costs two round trips per syscall instead of a burst that starves the frame |
| `FETCH_SECTORS` | `3` | 40 ms per host round trip, ~25 requests/s while music plays. Raising it costs `.hiram`, which the link now asserts against the Maple buffer |
| `CDDA_TIMEOUT_SECONDS` | `2` | the backstop, and **it does not mean two seconds**: `rtl_bb_loop` compares it against a whole-second count, so `> 2` fires at **three**. That is what the next line exists for |
| `CDDA_FETCH_DEADLINE_TICKS` | `625000` | **the deadline that applies** — 50 ms in TMU2 ticks, armed around the fetch's `bb->loop()` via `fine_deadline_ticks` (`adapter.h`). Measured 2026-09-01, twice, to 0.007 %: without it a lost ReturnValue froze the title for **3.005 s** and then failed the fetch. A gap in the music beats a freeze; three seconds of frozen title with the music stopped afterwards is the worst of both |
| `CDDA_REFUSE_GIVEUP` / `CDDA_LATE_GIVEUP` | `4` / `64` | consecutive failures before the stream stops, **and the two kinds are different questions**. A refusal is the host saying this image cannot serve audio; it is answered instantly, so four cost four round trips. A timeout costs 50 ms, does not advance `next_lba`, and is simply re-asked — measured 2026-09-01 over 4756 fetches, **seven timeouts and `g_cdda_underruns` stayed at zero**, so not one was audible. Sharing a single threshold of 8 is what silenced a whole session: seven fired in one level and the eighth called `cdda_stop()`. `cdda_fetch()` tells them apart by latching `timeout_loop < 0` before clearing it — `syscall_retval` is −1 for both |
| `G2_FIFO_SPIN_LIMIT` | 10000 | §4.8's rule — every hardware wait bounded. A G2 bus that never drains costs a dropped sample, not a hung console |

**An AICA channel's "volume" register is attenuation, and the port kept
isoldr's *muted* value.** Byte 41 of a channel (bits 15:8 of the 32-bit word at
offset 40) is TL, where **0 is full scale and 0xff is silence**;
`aica_channel_on()` wrote 255 there and called it volume. isoldr says both
halves of that plainly, and it is worth reading the two sites together
(`kos/ds/firmware/isoldr/loader/cdda.c`, which is in this tree):

```c
aica_set_volume(clean == 1 ? 255 : 0, 0);   /* :406  set-up: MUTED while the ring fills */
...
if (cdda->volume && cdda->cur_offset)       /* :1487 once there is data and the position is good */
        aica_set_volume(0, 1);              /*       0 -- audible */
```

So 255 is isoldr's temporary mute and 0 is its playing volume. This port copied
the set-up line and never wrote the unmute, which is why the value looks
deliberate. It does not need the two-step: `aica_clear_rings()` already silences
the ring before the key-on, so TL can be 0 from the start.

Measured 2026-08-31 on Snow Surfers: sound effects audible, music silent, and
**every counter healthy** -- `g_cdda_plays`, `g_cdda_fetches` and
`g_cdda_last_lba` all climbing, no underruns, no fetch failures, the play
position advancing. That is the shape worth remembering: attenuation is applied
on the way *out*, past everything this loader can observe, so no instrument on
the console could have seen it and the host's log looked perfect.

The neighbouring fields were right, and were checked against both references
rather than assumed: byte 40 is Q (`0x24` turns the low-pass filter off), byte
37 is DISDL (`0xf`, full send to the direct mixer) and byte 36 is DIPAN, where
`0x1f` is hard left and `0x0f` hard right -- isoldr's own
`AICA_PAN(x) ((x) == 0x80 ? 0 : ((x) < 0x80 ? 0x1f : 0x0f))` with `AICA_PAN(0)`
on the left channel and `AICA_PAN(255)` on the right, which is what this does.
KOS reaches the same values by a different route: `calc_aica_vol()` in
`kernel/arch/dreamcast/sound/arm/aica.c` is a 256-entry logarithm table with
`logs[255] == 0` and `logs[0] == 255`, and that table is the tell -- **if a
value has to be inverted before it reaches the register, the register is not a
volume.**

**What the music costs the title, and the half that had no instrument.** The
host times its own side of a fetch (§16) and after the fast path that is 0.13 ms,
0.3 % of wall time — but the stores into sound RAM happen *after* the host has
sent the ReturnValue and gone away, so nothing on that side can see them, and
PMCR reads 0 while a game runs. `cdda_push_frames()` is therefore timed with
TMU2, the timer this file already runs for pacing: `g_cdda_push_ticks_last` /
`_max` / `_total`, **12.5 ticks per microsecond**, two register reads and always
compiled. `_total / g_cdda_fetches` is the average; **`_max` is the one to
read**, because it says whether a single push can blow a 16.7 ms frame on its
own — which is what a stutter that comes in bursts looks like when the average
is harmless. Measured 2026-08-31: with the network at 0.3 %, `--no-cdda` is
still visibly smoother, so what remains is here.

**The push is spread across services, because where a cost lands matters more
than how big it is.** Measured 2026-09-01 on Snow Surfers: one
`cdda_push_frames()` of a whole fetch costs **12500 TMU2 ticks = 1.000 ms,
every time** (max 12960, spread 3.7 %), which at 29 fetches/s is 2.9 % of the
machine — a total nobody would chase. But `cdda_service()` runs from
`gdGdcGetDrvStat`, i.e. inside the title's frame loop, and this title is forced
to 60 Hz by its PPF (§16), so a whole millisecond arrives inside a 16.7 ms frame
about half the time. **A frame with 0.7 ms of slack absorbs it; a frame with
0.5 ms misses the flip and loses a whole frame, not a millisecond.** That step
is why 2.9 % is visible, why it comes in bursts, and why it gets worse as scenes
get heavier — and why cutting the *total* (ADPCM, bigger fetches) is not the
first thing to try. `PUSH_FRAMES_PER_SERVICE` (588, a third of a fetch) cuts the
excursion to ~0.33 ms instead.

**BOTH HALVES OF CD-DA ARE G2 BUS TIME, AND NEITHER IS THE NETWORK.** This was
established 2026-09-01 by timing the two ends against each other, and it is the
measurement that closed the whole "the music makes it stutter" investigation.
The host serves an audio fetch in **0.11 ms**, max 0.40 ms, **zero over 5 ms in
2750 reads**; the console reports **1.2 ms** for the same fetch plus 1.02 ms for
the push. The discriminator is `g_rx_polls`: **79 poll iterations per fetch**,
against 1620 per chunk while a disc read really is waiting on the host. Seventy-
nine means the packets were already in the ring — dcload was not waiting, it was
*copying*. 7056 bytes read out of the BBA's SRAM and 7056 written into sound RAM,
at ~0.58 µs per 32-bit access, is 2.05 ms per fetch against 2.23 measured. At
25 fetches/s that is **5.6 % of wall time, all of it inside the title's frame
loop** — and it is why nothing done on the host side ever moved the symptom.

**The lever looked like transactions, and it is not: `CDDA_PUSH_SQ` was tried
and is 10 % SLOWER.** The premise was that 1764 individual stores to the P2
window were stalling the CPU on transaction setup, so eight words written into a
32-byte on-chip queue and sent as one `pref` burst would cut the external count
by eight. Measured 2026-09-01 over two levels: **1.02 ms per fetch became
1.12 ms**, `g_cdda_push_ticks_last` 4170 → 4629. A P2 write is already posted
through the same G2 FIFO the burst uses, so the queue removes no bus time and
adds eight on-chip stores per block on top of identical traffic. **7056 bytes in
1.02 ms is 6.9 MB/s, which is G2 write bandwidth to the AICA: this path was at
the floor before the experiment.** The code is kept, defaulted off, with the
number — so the next reader does not repeat it. **The only lever left is sending
fewer bytes, which means ADPCM** (4 bits per sample against 16): it would cut
*both* G2 halves by four, at the cost of a lossy encode on the host, and isoldr
carries exactly that option (`bitsize == 4` in its `setup_pcm_buffer`).

isoldr reaches the same place from the other side, and its source is the reason
to trust the direction: `PCM_transfer_t` (`isoldr/loader/include/cdda.h`) offers
DMA, SQ and PIO and **defaults to DMA**, and `aica_pcm_split` deinterleaves in
main RAM *before* any transfer rather than word by word into sound RAM. **The
read half cannot be copied from it**: isoldr's sources are IDE and SD and it
reads them by DMA, so its CPU never touches the bytes. Ours is the network, and
dcload has no DMA for the BBA — isoldr's own net backend is `#if 0` (§4.12). That
~1.0 ms per fetch is structural to this transport.

**SO THE BYTES WERE CUT INSTEAD: `CDDA_ADPCM` (default 1) ships the audio as
4-bit Yamaha ADPCM.** Both G2 halves are byte counts, so a quarter of the bytes
is a quarter of the time — the predicted 5.6 % becomes ~0.6 %, the per-fetch
push excursion 0.33 ms becomes ~0.11, and the round trips fall from 25/s to 19.
The wire format is `CMD_CDDAREAD_ADPCM` (`DC24`, §8): one byte per stereo frame,
**already split into the two channels**, left block then right, so the loader
does no deinterleaving at all — two loads and two stores per *eight* frames
against two per two. `.hiram` drops by 4704 bytes; `_end` grows by 76.

Four things make it work, and each of them is a way it could have failed:

1. **`AICA_SM_ADPCM_LS`, not `AICA_SM_ADPCM`.** ADPCM carries a running
   predictor and step size, and mode 2 resets them at the loop back to LSA —
   which for a ring played round and round is a discontinuity 2.7 times a
   second. Mode 3's defining property is that it does not. isoldr streams the
   same way (`cdda.c:1061`), and its ADPCM geometry is *byte for byte ours*:
   8192 bytes a channel, 16384 samples, `end_pos = samples - 1`. **LEA is in
   samples for both formats** — not obvious, and worth the one line it costs.
2. **The encoder state lives on the host and is the decoder's.** `src/adpcm.rs`
   keeps one `Stream` per session: it continues from request to request, never
   resets at a seek or a track loop (the decoder does not either), re-encodes a
   **repeated LBA from the state it had at the start of it** — the loader
   re-asks for a fetch whose answer it never saw — and resets only when bit 31
   of the size field says so. The loader sets that bit at key-on, the one event
   that resets the AICA's decoder, and keeps it set until a fetch actually
   *lands*.
3. **Eight samples go into one 32-bit store**, so the ring, the fetch and the
   slice must all divide by eight. 16384, 2352 and 784 do; `3 * 588 = 1764` does
   not, which is the whole reason `FETCH_SECTORS` is 4 here and 3 for PCM. Three
   `typedef` asserts at the top of `cdda.c` enforce it — a half-word slip is not
   a click, it is the decoder losing its state and never getting it back.
4. **A fresh ring is filled with `0x80`, not zero.** A zero nibble is a
   *positive* step, so an all-zero ring ramps to full scale in 49.5 ms and sits
   there. `0x80` is one step up then one step down, and the step size decays to
   its floor: ±15 LSB at half the sample rate, 66 dB down. It also hands the
   decoder the right state — the channel keys on before the first fetch lands,
   so whatever the idle bytes leave in its predictor is what the host's first
   real nibble is applied to.

The codec is the **`oxideav-adpcm` crate** (`yamaha::Chip::Aica`), and the
reason is provenance rather than speed. It implements the Y8950 Application
Manual's §I-4 recurrence and — the part that matters — it **distinguishes the
AICA rounding from the OPNA one**: both approximate the same `~1.1^M` step
curve, but AICA/Y8950/YMZ280B is `{230,230,230,230,307,409,512,614} >> 8` while
the YM2608 manual's Table 5-1 prints `{57,57,57,57,77,102,128,153} >> 6`. Those
are *different codecs* — 0.8984375 against 0.890625 — and a stream encoded with
one and decoded by the other does not sound obviously wrong, it **drifts**,
which is this file's worst failure mode and precisely what a library advertising
"Yamaha ADPCM" makes easy to get wrong.

A hand-rolled encoder from KOS's `utils/wav2adpcm` (superctr's public-domain
`ymz_codec.c`) shipped first and was **measured against the crate over 200000
samples of full-scale material: rms error 385.4 against 385.2, SNR 30.3 dB
either way**. 44.5 % of the nibbles differ, which is only the two trajectories
diverging after the first disagreement — they are the same encoder in quality.
So the swap buys nothing audible and one thing worth having: the AICA/OPNA
distinction is a *type* rather than a comment, and `src/adpcm.rs` pins the
constants with `the_crate_still_decodes_like_the_aica`, a test that walks the
whole step range against the recurrence written out by hand. If the dependency
ever has to go, that test is the spec.

One difference from KOS is deliberate and recorded at the test: `wav2adpcm`
clamps the per-sample contribution to `[0, 32767]` before applying it —
superctr notes it as "only found in the official AICA *encoder*" — and MAME's
decoder does not. It bites only above step 17475, i.e. on loud transients, and
what must be matched is the hardware rather than a tool, so the unclamped form
is the one to be in step with. (An earlier note here said KOS's closed form
"rounds toward larger deltas". It does not: `floor(4*|d| / step)` is exactly the
nearest-reconstruction rule, the same one the crate's threshold ladder
computes.)

**AND IT EXPOSED A DEFECT THAT WAS ALWAYS THERE: `cmd_loadbin` ECHOES, AND ON
THIS PATH NOBODY IS LISTENING.** Measured 2026-09-02 on Snow Surfers, first
ADPCM run: **73 % of fetches timed out** — 828 failures against 306 successes —
the title fell to a 10 Hz frame loop and the music repeated the same ring over
and over. The counters say precisely what happened, and it is not what any of
the earlier hypotheses predicted:

| | per 2 s |
| --- | --- |
| `g_lbin_count` | 43 — **one per attempt, never lost** |
| `g_pbin_ok` | 50 — two for each of the 7 successes, **exactly one** for each of the 36 failures |
| ReturnValue | ~9 |
| `g_pbin_rejected`, `g_rx_missed`, `g_rx_overflow` | **0** |

So a failed fetch saw its LoadBinary and one part, then nothing for the whole
50 ms deadline. Nothing was refused by `cmd_partbin` and nothing was dropped by
the chip for want of a buffer: **the frames never arrived**. The host was
cleared separately — `an_audio_fetch_is_one_loadbin_two_parts_and_a_returnvalue`
(a unit test with a counting fake `ExternalDcIo`) proves `send_audio` puts
LoadBinary + two parts on the wire with the right addresses and sizes.

**The one thing the console does between the part that arrives and the ones that
do not is transmit `cmd_loadbin`'s echo.** `send_data()` waits for that echo and
needs it; `send_audio()` deliberately waits for nothing, so on the CD-DA path it
is a transmit nobody reads — fired from inside `bb->loop()`, in the middle of a
burst whose packets are ~15 µs apart. That is what a half-duplex collision looks
like from the receiving end, and the RTL8139 counts it nowhere this loader
reads. `bin_echo_suppress()` turns it off for the duration of a CD-DA fetch and
its drain; `g_lbin_noecho` says the switch was in effect.

**It also explains the crackle nobody could find, and that is the reason to
believe it.** With 16-bit PCM the answer was seven packets, so the echo could
only collide with the first part: the ReturnValue still landed, the fetch was
counted a success, and the hole was 1440 bytes — **a click**, rare, following a
lag spike, with every counter healthy because the accounting never knew. Three
sessions were spent on splices, drift and the AICA's position register looking
for it. ADPCM's four-packet answer moved the collision onto the *last* two and
turned the click into a total failure, which is the only reason it became
visible.

**This is a hypothesis with a falsifier, not a proven diagnosis.** If the
failures do not collapse, the collision story is wrong and `g_lbin_noecho` says
the suppression was really in effect while they persisted.

**AND THE TRANSFER IS NOW FAILSAFE RATHER THAN OPTIMISTIC**, because the echo
was one loss and there were three more ways a lost packet became audible. Four
changes, all on the DC side, all costing nothing in the good case:

1. **A hole is never pushed.** `bin_window_complete()` tests the packet map
   `cmd_donebin` already maintains, so the loader learns its transfer is short
   with **no round trip**. This is what explains *one ear working and the other
   sounding like a loose cable*: parts are 1440 bytes and the two channels go
   out as two blocks, so a 2352-byte fetch divides as `part 0 = all of left
   (0..1176) + right's first 264` and `part 1 = right's last 912`. **Either ear
   can be the damaged one, and which it is says which part was lost** — part 1
   takes 912 of the right channel's 1176 bytes, part 0 destroys the left
   outright. (This was first written as "always the right", from the arithmetic
   of one lost part; the left was then reported in practice, which is the same
   mechanism with the other part missing. The check is symmetric and was never
   affected.) With ADPCM the damage is permanent either way — that channel's
   predictor is fed rubbish and never recovers. A hole is now re-asked for, and
   the host's re-encode from the saved start state makes the retry
   byte-identical. `g_cdda_holes`.
2. **A transfer finishes on its last part, not on the ReturnValue.**
   `bin_complete_escape()` lets `cmd_partbin` end the wait the instant the
   window is full, so a lost control packet costs nothing instead of a whole
   50 ms deadline for data the loader already has — measured 2026-09-02, only
   ~9 of 43 ReturnValues arrived. The common case also gets faster by one
   packet. `g_bin_data_done`.
3. **A ring that ran dry re-keys the channels.** An underrun means the AICA read
   samples we never wrote; for PCM that is a click that heals, for ADPCM it is
   permanent. `cdda_restream()` keys off and on — which resets the AICA's
   decoder, clears the rings and asks the host to reset its encoder — so both
   ends start from a state they agree on. **Guarded by progress, not by a
   count**: a restream is allowed only if a fetch has landed since the last one,
   so a link that is simply down cannot make it loop. `g_cdda_restreams`.
4. **Running out of patience restreams instead of stopping.** `cdda_stop()` on
   the late-giveup path turned a bad few seconds into silence for the rest of
   the track with no way back — nothing restarts the stream until the title
   issues another PLAY, which a title that believes its music is playing never
   does. Now it re-keys, and only stops when a restream declines.

**And one ear can go wrong with nothing lost at all, which is a different cause
and now has its own check.** isoldr re-reads its two channels every service
(`aica_check_cdda()`, `loader/cdda.c:474`) because a title's own sound driver
owns the AICA and is free to walk all 64 channels; if it takes one of ours the
result is exactly one ear wrong, no packet lost, and no counter moving. This was
a known gap in §4.13 for weeks and the "loose cable" report is what it would
sound like. `cdda_check_channels()` compares registers 4, 8, 12, 24, 36 and 40
against what was written (0 and 16 are excluded — key-on-execute is consumed by
the hardware and the envelope moves), needs two to disagree, and sets
`need_restream`. `g_cdda_ch_stolen`.

**AND THE REGISTER-0 CHECK PRODUCED A FALSE POSITIVE THAT MADE THINGS WORSE.**
Measured 2026-09-04 on Snow Surfers: `got = 0x6b9f` against `want = 0x439f`.
Every documented field matched — KYONB set, SSCTL clear, LPCTL set, PCMS 3,
SA-hi 0x1f — and the difference was exactly **bits 11 and 13**, which no decode
of this register assigns a meaning to (MAME reads bits 0, 7, 9, 10, 14 and 15
out of it and nothing else) and which come back **set** on real hardware once
the channel has been running. Nothing had touched the channel; the loader
restreamed anyway, several times a minute, each one clearing the ring, re-keying
and resetting the pacing model. `CDDA_CTRL_MASK` (0x47ff) is the comparison
restricted to fields that exist, and a structural mismatch must now be seen
**twice running** before it is paid for.

**The calibration did not catch it, and that is the part worth remembering.** It
runs immediately after key-on and passed, because those bits are still zero
then. §11's "prove the instrument before trusting it" is not sufficient on its
own when the instrument is proved at a moment the thing it measures has not
started happening yet.

(The evidence only survived because `cdda_calibrate_channel_check()` cleared
`g_cdda_ch_bad_reg` and not `_got`/`_want` — a bug that happened to preserve the
one thing needed to diagnose it. It clears all three now.)

**AND THE TWO EARS WERE NEVER STARTED TOGETHER.** `cdda_channels_start()` called
`aica_channel_on()` twice, and each call took and dropped **its own**
`g2_lock()` — so between the left channel's key-on and the right channel's, the
title's interrupts were wide open. A VBlank handler, a Maple completion or the
title's own sound driver landing in that window starts the right ear
**milliseconds** after the left, and `cdda_timer_start()` sat outside both locks
as well, so the pacing model's origin was a third arbitrary point.

**A fixed offset between the ears is not a click and not a dropout — it is comb
filtering**, and how audible that is depends entirely on the material. So it
comes and goes with the music while nothing whatever is wrong: "the volume
weirdly changes between left and right, doing like a stereo pan effect",
sometimes clearing in under a second and sometimes lasting twenty. It is
**format independent**, which is what the 2026-09-04 A/B demands, and it is
**invisible to every instrument in this file** — both channels read back exactly
what was written, the ring never runs dry, no part is lost, `g_cdda_ram_clobber`
stays at 0. Every counter here can say only that each ear is individually
correct, and each ear is.

isoldr has never had it: `aica_setup_cdda()` (`loader/cdda.c:395`) holds **one**
`g2_lock()` across both channels and starts its timer inside it.

Two changes, and the second is the exact one. Both channels are now set up and
started in a single critical section, timer included. And **KYONEX is a global
execute strobe, not a per-channel key-on**: a write with bit 15 set makes the
chip walk all 64 slots and start every one whose KYONB is set (MAME's `aica.c`
is that loop, and it is why a title's own driver strobing does not disturb
channels already running). So `aica_channel_arm()` writes `0x439f` — KYONB
without KYONEX, described and inert — for both ears, and `aica_key_exec()` then
strobes `0xc39f`, which starts them **on the same sample**. Both channels are
strobed, back to back with no branch between them, because that is correct under
either reading of the strobe: global, the first write starts both and the second
is a no-op on two active slots; per-channel, each ear still gets its own. What
that rules out is the worst failure — an ear that never starts at all.

The offset was re-rolled by every re-key, which is why a lag spike — the thing
that provokes a restream — so often preceded a bad stretch.

**AND THE SAME TRAP TOOK THREE MORE REGISTERS.** 20, 28 and 32 were added to the
check group compared against 0 over a full 16 bits. Measured 2026-09-04 on Snow
Surfers: `g_cdda_ch_bad_reg = 0x3f20` — channel 63, register 32 — with
`got = 0x1900` against `want = 0`, i.e. bits 8, 11 and 12, while ISEL occupies
bits 6..3 and IMXL bits 2..0 and **nothing is defined above bit 6**. Undocumented
bits that read back set once a channel has been running, invisible to a
calibration that runs before it has run: the register-0 false positive again, in
the same patch that fixed it. Those three are now **written and not checked**.
isoldr checks 0, 24, 12, 36 and 40 and no others, and that list is not an
oversight — it is the set whose field map two independent references agree on.
**Do not add a register to this check without a reference that documents every
bit of it**; the cost of guessing is a restream or a repair fired at random into
a running title, which manufactures the symptom being hunted.

**AND THE PLACE TO LOOK NEXT IS NOT ANOTHER REGISTER: `CDDA_TEST_TONE=1` TAKES
THE WHOLE DATA PATH OUT.** Every hypothesis on record has died against its own
falsifier — transport, ring geometry, pacing model, play position, decoder state
(PCM and ADPCM glitch identically), channel theft, sound-RAM clobber, and the
two ears not starting on the same sample. What none of them separated is the
half of the machine the fault is in. The tone build fills both rings once with a
fixed triangle — 344.5 Hz left, 689.1 Hz right, an octave apart, both periods
dividing `RING_SAMPLES` exactly so the loop back to LSA is seamless — and then
**never fetches, never pushes, never paces**. The AICA loops that ring off its
own clock with nothing else running. The answer is binary: a steady tone puts
the fault upstream in the fetch, the encode, the push or their timing; a
wandering tone puts it in the AICA, the mixer or the title, and says every
measurement taken on the network path was aimed at the wrong half. PCM only
(`make loaders CDDA_ADPCM=0 CDDA_TEST_TONE=1`); the resulting image contains no
`DC23` string at all, because `--gc-sections` drops a fetch path nothing calls,
which is also how to tell the set apart by content.

**AND THE TONE IS PERFECT.** Measured 2026-09-04 on Snow Surfers: not one glitch
for as long as it was listened to. **That closes the second half of the search
space in one measurement.** The AICA, the mixer, the master volume, the title's
own sound driver, the channel registers, the loop back to LSA, the sample RAM
itself and the two ears' key-on are all exonerated — every one of them is fully
exercised by the tone build, and every one of them is clean. **The fault needs
writing while playing to exist at all**, which is why weeks of register checks,
theft detection and repair paths found nothing to find: there was nothing there.

Note what the tone does **not** settle, so it is not claimed later: the two ears
carry *different* frequencies, so a fixed time offset between them is inaudible
in a tone and would be comb filtering in music. That hypothesis is dead anyway —
it died on its own falsifier when the arm/strobe fix changed nothing — but it did
not die here.

**`CDDA_TONE_STREAM=1` IS THE NEXT RUNG, AND IT SPLITS WHAT IS LEFT IN HALF.**
Three things can still be wrong: **(a)** the data is wrong when it arrives —
fetch, wire, host encoder; **(b)** the data is right but goes to the wrong place
at the wrong time — the pacing model, the seam, the ring arithmetic; **(c)** the
*act* of writing disturbs playback — the G2 critical section, the interrupt mask,
the DMA suspend, 75 times a second. The streamed-tone build synthesises the same
triangle **into the staging buffer**, frame by frame with a phase that never
resets, in place of the host round trip, and hands it to the ordinary machinery:
`cdda_advance()`, `cdda_room()`, the slicing, `cdda_push_frames()`, the wrap,
`g2_lock()`, the channel checks and the restreams all run exactly as they do for
music, while no packet is sent and no encoder exists. A steady result clears (b)
and (c) and puts the fault in the arriving data; a glitching one makes (a)
irrelevant — **and a pure tone makes a seam crossing audible and countable**,
which is precisely what no counter in this file has ever been able to do. Build
it with `make loaders CDDA_ADPCM=0 CDDA_TONE_STREAM=1`; the two tone builds are
mutually exclusive (`#error`), and `sh-elf-nm dcload-relocatable.elf | grep
tone_` names `tone_sample` in either and nothing in any other set.

**AND THE STREAMED TONE IS PERFECT TOO** (measured 2026-09-04, same title, same
session). So (b) and (c) go as well: the pacing model, the seam, the ring
arithmetic, the wrap, the slicing, `g2_lock()` and its interrupt mask and DMA
suspend all run exactly as they do for music, 75 bursts a second into sound RAM
while the title runs, and the result is flawless. **Everything this loader does
with the samples once it has them is correct.**

What is left is the data itself and how it gets here: the disc read on the host,
the encoder, the wire, and the fetch round trip with its duration and its
`bb->loop()` — none of which the streamed tone exercises, because it synthesises
the buffer and returns.

**AND ONE HYPOTHESIS SURVIVES BOTH TONE BUILDS BY CONSTRUCTION, WHICH IS WORTH
SAYING BEFORE IT IS RE-DISCOVERED.** The two ears carry *different* frequencies —
that is what makes the tone unmistakable, and it is also what makes it blind to a
**difference between the two channels' read pointers**. Two ring positions that
have drifted apart would be inaudible on an octave pair and would be comb
filtering on music, which is the reported symptom exactly. It is not the leading
candidate — a re-strobe of an already-active slot is a no-op in MAME's AICA, so
the arm/strobe pair does start both on the same sample, and the fix changed
nothing when it went in — but neither tone result touches it. A third variant
with **the same frequency in both ears** is what would.

**AND THE PCM/ADPCM A/B ABOVE IS NOW IN DOUBT, BECAUSE OF A DEFECT FOUND LATER
THE SAME DAY.** "PCM and ADPCM glitch identically" was measured by running two
loader sets that differ only in `CDDA_ADPCM` — and at that point
`loaders::plan()` returned an empty plan whenever the running loader was already
at the wanted base, so **a rebuilt set at the same address was silently not
uploaded** (§16). The sets were told apart by the content of the *files*, which
proves what was on disk and nothing about what was on the console. That defect is
fixed and the fix is proven — the tone builds only reached the Dreamcast once it
was in — so the A/B is not wrong, it is **unmeasured**, and it has to be re-run.
It matters more than any other open question here: ADPCM is the only path with
per-channel blocks and a running predictor, i.e. the only one that can damage
**one ear** for seconds at a time from a single lost or wrong part, which is the
symptom. If PCM comes back clean the search is over the format's shared state; if
it glitches too, the fault is in the arriving bytes whatever their encoding, and
the next rung is a host that serves the tone.

**AND THE ANSWER IS NEITHER: THE TITLE WRITES INTO OUR RING.** Measured
2026-09-04 on Snow Surfers, PCM, with `--diag` working for the first time:

```
g_cdda_ram_clobber   1
g_cdda_ram_addr      0x001f4e04     19972 bytes into the LEFT ring
g_cdda_ram_got       0x41474553     "SEGA", ASCII, little-endian
g_cdda_ram_want      0xfa0bfa03     -1533 and -1525: two adjacent audio samples
```

The rings are the top 64 KB of sound RAM (`0x1f0000..0x200000` for PCM), the
AICA never writes sample RAM, and nothing in this loader puts ASCII there. So
our samples are overwritten **after we write them and before the AICA reads
them** — which is the one link §4.13 had listed as unchecked for weeks, and the
probe that was added for it caught it.

**IT EXPLAINS BOTH SYMPTOMS AT ONCE, AND THE ARITHMETIC IS WHY IT IS CREDIBLE.**
The old probe watched **one word per push**, out of the ~784 frames a push
writes: roughly 7700 words sampled over the session against ~3 M written, so one
hit means on the order of a few hundred damaged words — a handful of samples a
second, scattered. In PCM that is a crackle that heals instantly and never
stops. In ADPCM each of those words is eight samples and a poisoned predictor
that does *not* heal, so one ear comes out scaled wrong until a step clamps:
seconds of wandering balance. Same event, two symptoms, and every other counter
blind to it because the bytes were delivered, counted and pushed perfectly.

Everything else in the same dump is healthy and worth recording as the
background: 2579 fetches, 4 holes, 8 fetch failures, `g_pbin_rejected` 0,
`g_rx_missed` 0, `g_cdda_underruns` 0, `g_cdda_room_min` 9545, `g_cdda_restreams`
0. The transport is not the problem and has not been for some time.

Two changes follow. **The probe now watches 32 words per push** (~19 µs against
the ~1000 µs a push already costs) and publishes `g_cdda_ram_checks` alongside
the count — a hit count without its denominator cannot be turned into "how much
of the music is damaged", which is exactly the question — plus `g_cdda_ram_lo`
and `_hi`, the extent. The first hit stays latched and is never overwritten.
And the ear now alternates **on the slot index**: the old arm tested `pos & 2`,
which once the sampling became periodic would have watched one ring and only one
ring.

**And `CDDA_RING_TOP` moves the rings.** It is the address just past the right
ring, defaulting to `AICA_RAM_END` — isoldr's choice, and ours until now, on the
reasoning that a title's driver and banks grow up from zero. That reasoning is
wrong for this title. `make loaders CDDA_ADPCM=0 CDDA_RING_TOP=0x1C0000` hands
the top 256 KB back and puts the rings at `0x1b0000`. It is a guess until the
widened probe reports the extent, which is why it is a knob rather than a new
constant.

**AND THE CLOBBER WAS REAL AND WAS NOT THE CAUSE.** Measured 2026-09-05 with the
rings at `0x1b0000` and the widened probe: `g_cdda_ram_clobber` **0** over
`g_cdda_ram_checks` **26133** — so moving them out of the top of sound RAM
really does end it, at 32 times the old sensitivity — **and the crackle is
unchanged**. Keep the move: a title overwriting our samples is indefensible
whatever else is true, and the probe is now a rate rather than a lottery ticket.
But it is not what this is. Everything else in that dump is spotless too:
`g_cdda_holes` 0, `g_pbin_ok` exactly 5.0 per fetch, `g_lbin_count` 1.0 per
fetch, `g_bin_data_done` 1.0 per fetch, `g_pbin_rejected` 0, `g_rx_missed` 0,
`g_rx_overflow` 0, `g_cdda_underruns` 0, `g_cdda_restreams` 0.

**SO THE BYTES ARRIVE COMPLETE, IN THE RIGHT PLACES, EVERY TIME — AND THE MUSIC
IS STILL WRONG.** Two candidates remain and they are on opposite sides of the
host's buffer: this host reads the wrong bytes off the disc image, or the right
bytes are damaged in flight while arriving complete and counting correct. The
second is not exotic here — `docs/read-back-verification.md` records exactly
that failure on the disc path, an RX-ring splice invisible to every counter.

**`--cdda-tone` IS THE OTHER HALF OF THE BRACKET AND IT SEPARATES THEM.**
`CDDA_TONE_STREAM` synthesises the triangle on the console and exercises
everything downstream of the wire; `--cdda-tone` synthesises the same triangle
on the host and exercises everything downstream of the disc read. A clean tone
puts the corruption in the host's reading of the image; a crackling one puts it
in flight. The generator is a function of the LBA alone, so a re-asked sector
comes back byte for byte — a retry that differed would itself be the splice
being hunted — and the phase carries across sector boundaries, so anything
audible came from the wire. It replaces the disc read and nothing else: the
ADPCM encoder still runs, so the switch is meaningful for either loader format.

One thing was ruled out on the way, at no cost: the deflate random-access reader
was suspected on the strength of "184 MiB, 663 checkpoints, 4 reader threads",
and it is **single-threaded per source** (`RefCell`, the threads are the index
build) **and forward-walking** on this path -- a CD-DA stream reads consecutive
LBAs, so its cursor never restarts from a checkpoint after the first. A wrong
dictionary at a checkpoint restart cannot produce a constant crackle. It was
then ruled out **by measurement** rather than by argument:
`a_long_sequential_stream_matches_byte_for_byte` replays exactly the CD-DA
pattern -- ~2400 consecutive 7056-byte reads over a 16 MiB member, so ~60 passes
through the `advance_past` buffer slide, which is the one thing the existing
jumping test barely touches -- and it matches byte for byte. The existing test
could not have caught a slide bug: it reads ten scattered 4 KiB windows.

**AND THE CLOBBER FOLLOWS THE RINGS.** Measured 2026-09-05, `--cdda-tone` with
the rings at `0x1b0000`: `g_cdda_ram_clobber` **19** over **115481** checks, and
`g_cdda_ram_got` is `0x41474553` -- **"SEGA" again**, at `0x1b9f84` this time.
`g_cdda_ram_lo`/`_hi` say `0x1b1fc4..0x1ba384`, a contiguous ~33 KB block
straddling both rings at similar offsets. So it is not the top of sound RAM that
is claimed, it is wherever we go; 1.65e-4 of checked words is ~609 damaged words
in that session, and in bursts that is about one audible event every ten seconds
-- which is what the served tone sounded like.

**AND THAT MAKES THE SERVED-TONE A/B AMBIGUOUS, WHICH IS WORTH SAYING PLAINLY.**
The disc run before it reported **0** clobbers in 26133 checks with the crackle
unchanged, and the tone run reported 19 with the crackle rare. Read one way the
tone cleared the wire and the fault is the disc read; read the other way the two
runs simply sat at different points in the game -- the first was 476 fetches in,
the second 2100 -- and the clobber had not started yet. 0 against an expectation
of 4.3 is a 1.4 % coincidence, so neither reading is safe. **The comparison has
to be redone at the same point in the title with `clobber/checks` on both
sides**; that ratio is exactly what makes two runs comparable, and it is why the
denominator was added.

`g_cdda_ram_ctx[8]` is what should end the guessing about the writer: 32 bytes
around the first hit, latched once, rendered by the panel as hex **and as text**
(`Fmt::Dump`). Four bytes reading "SEGA" twice at two unrelated addresses is a
block whose content repeats, and four bytes cannot name it.

**AND THE AMBIGUITY IS RESOLVED: THEY ARE TWO DIFFERENT FAULTS.** The disc run
was repeated at the same point of the title, and the intro now has **0 clobbers
over 53314 cumulative checks** across two runs -- against 8.8 expected if the
late-game rate held, a coincidence of 1.6e-4. So during the intro the clobber
simply does not happen, and the intro is exactly where the crackle is worst. The
sound-RAM clobber is real, follows the rings, and accounts for the occasional
event later on; **it does not account for the intro crackle, which remains
unexplained.**

**SO STOP ASKING WHETHER IT WAS DELIVERED AND LOOK AT THE SAMPLES.** Every
counter in this file reports on *delivery* -- packets, parts, holes, rings,
registers -- and every one of them has come back clean while the music crackled.
`cdda_slew_scan()` asks the direct question instead, on the staging buffer, the
moment a fetch completes and before anything is pushed: a splice, a lost run, a
repeated sector or foreign bytes from any cause all leave the same fingerprint
in 44.1 kHz PCM, which is a step no acoustic signal makes. `g_cdda_slew_hits`
counts steps past `SLEW_LIMIT` (24000); **`g_cdda_slew_runs` is the
discriminating one**, counting the ones whose predecessor was also past it --
music reaches full scale in one step now and then, but not twice running,
whereas packet headers and neighbouring buffers do it constantly.
`g_cdda_slew_lba`/`_off` locate the first run. **The seam between two fetches is
included**, which is half the point: the last sample of one fetch and the first
of the next are adjacent on the disc, so a repeated or skipped sector shows up
there and nowhere else.

It answers the question that the served-tone A/B could not: **runs > 0 means the
audio was already broken when it arrived**, so the fault is upstream of the
push -- the host's read or the wire -- and everything downstream is confirmed
innocent by the streamed tone. **runs == 0 while it crackles** means the samples
were perfect on arrival and the damage happens after, which given a clean
`g_cdda_ram_clobber` would be genuinely new ground.

**It calibrates itself, per §11.** `--cdda-tone` sends a triangle whose every
step is exactly 512 in the left ear and 1024 in the right, so a tone run MUST
report hits 0, runs 0 and max 1024. Anything else and the instrument is
measuring itself rather than the audio -- which is the trap this section has
fallen into twice already, with the register checks. Run the tone first.

PCM only: the ADPCM staging buffer is nibbles, and decoding it here would cost
more than it could report. ~3500 comparisons a fetch, ~265 µs, 0.7 % of the
machine on a path that already spends 2 % pushing.

**AND IT FIRED IMMEDIATELY: THE AUDIO IS ALREADY BROKEN WHEN IT ARRIVES.**
Measured 2026-09-05 on Snow Surfers' intro, disc audio, rings at `0x1b0000`:
`g_cdda_slew_runs` **26**, `g_cdda_slew_hits` 125, `g_cdda_slew_max` **54259**,
first at LBA `0x0004e6d0` frame 573 — over 875 fetches, which is 35 s of audio,
so **one burst every 1.35 s**. And `g_cdda_ram_clobber` 0 over 48128 checks in
the same run. A step of 54259 is 83 % of the full 16-bit range, and a *run* of
them means the waveform swings past 73 % of full scale twice inside 45 µs: that
is content near 22 kHz at near-full amplitude, and a CD is filtered at 20 kHz.
**It is not music.** So everything downstream of the staging buffer is innocent
— which the streamed tone already said — and the fault is the host's read or the
wire.

**`SlewWatch` IN THE HOST IS THE SAME TEST ON THE OTHER SIDE**, same limit, same
two-in-a-row rule, run on the PCM before the ADPCM encoder so the number is
comparable whatever format the loader asked for. Equal counts mean the bytes
were already wrong when they were sent and the fault is this host's read of the
image; zero here against 26 there means they left clean and arrived broken —
the wire, in a transfer that reports every part delivered and every window
complete, which is `docs/read-back-verification.md`'s RX-ring splice again.
It prints its total every 250 reads **including zero**, because zero is the
informative value and an absent warning is not evidence.

**And it calibrates itself in the test suite rather than on the console.**
`the_continuity_check_is_silent_on_a_clean_signal_and_not_on_a_splice` scans
`tone_sectors()` and asserts silence with `max` exactly 1024 — the right ear's
step — then splices in full-scale bytes and asserts a run. One arithmetic note
worth keeping, because the first version of that test failed: **a phase jump in
this triangle is not enough.** The tone only reaches ±16384, so a splice between
two arbitrary points of it caps at 32768 and the pair first tried came to 22016,
under the limit. That is a property of the test signal, not a hole in the check
— what is being hunted is foreign bytes, and foreign bytes read as audio swing
the whole range.

**AND THE TWO ENDS AGREE EXACTLY, WHICH ACQUITS THE WIRE.** Measured 2026-09-05,
same session, the two independent checks:

| | console | host |
| --- | --- | --- |
| first run | LBA `0x0004e6d0` frame 573 | LBA `0x0004e6d0` frame 573 |
| largest step | 54259 | 54259 |

The console's copy scanned bytes that had crossed the network; the host's scanned
them as they came off the image. Identical location, identical magnitude. **So
the transfer is byte-perfect and the corruption is upstream of it** — and the
first 250 reads were clean (largest step 20014, no runs), so it is not a constant
misreading either: something specific starts at `0x0004e6d0` and recurs. It is
also **deterministic**, two separate runs having picked the same first offset.

Which leaves two possibilities, and the host now settles them in one line each
the next time it fires. **The bytes are dumped** as hex and ASCII around the
offending frame — the same trick that named the sound-RAM writer in one look.
**And the sectors are re-read down a different path**: reading well behind first
leaves the cursor unable to walk forward, so the same range comes back through a
checkpoint restart instead of the forward walk that produced it. Same bytes means
this reader is self-consistent and **the discontinuity is in the image**;
different bytes means the random-access read is wrong, and which of the two paths
is at fault is then a two-line question. It runs once per session, outside the
per-fetch timing, and says so.

**AND THE RE-READ CAME BACK "SAME BYTES", WITH A CAVEAT THE FIRST WORDING GOT
WRONG.** Measured 2026-09-05: the checkpoint-restart re-read of LBA `0x0004e6d0`
is byte-identical, so the deflate path is self-consistent. The message said "the
discontinuity is in the disc image itself" and **that claim was too strong** —
both reads went through the same LBA-to-offset mapping, so what is established
is that the bytes really are at that offset in that member, not that the offset
is the right one. Reworded, and the missing half is what `audit-audio` is for.

The dump is high-entropy: `aa5572f2ff7f5732b04bbd67a0c95442…`, which as samples
is 21930, −3470, **32767**, 12887, 19376 — full-range noise, not music, and not
text either. And the distribution over the session is the informative part:
runs went 0 → 13 → 127 → **129** over four 250-read windows, i.e. **a contiguous
band of roughly ten seconds and then nothing**. A reader bug is periodic or
persistent; a band with clean music on both sides is damage in the dump. That is
a shape, not a proof, which is why the next step measures the whole disc instead
of whatever the title happened to play.

**`dcload-ip-rs audit-audio <image>` is that measurement, and it touches no
network.** It prints the track table with each track's LBA range and length,
then walks every audio track with the same check, coalescing consecutive bad
reads into bands and reporting where each one starts within its track. A dump
whose track 10 has one ten-second band at 11.9 s and is otherwise clean is
answered; so is a reader that fails every 37 reads, which is what a slide bug
would look like.

**AND THE WHOLE SLEW RESULT WAS A FALSE POSITIVE. THE THIRD ONE IN THIS
SECTION.** `track10.raw` was extracted from the archive and scanned directly,
offline: 61 runs, largest step 54259, first band at frame 526245 — and the
loader's `g_cdda_slew_lba` 0x0004e6d0 with `_off` 573 is, once the arithmetic is
done right, **frame 526245 of that same file**. Everything agrees. What it
agrees on is that the bytes on disk are the bytes that arrived, which is a
statement about the transfer and not about the audio.

The premise was wrong. **A step of 54259 is not impossible in music**: a signal
band-limited to 20 kHz at amplitude A can step by up to `2·A·sin(π·f/fs)`, which
at full scale is 64844. So "no acoustic signal makes that step" was simply
false, and 24000 is a level this track's loud passages reach on their own. The
discriminator that settles it is the zero-crossing rate — white noise sits near
0.5, band-limited music near 0.05:

| | ZCR | step/amplitude | peak |
| --- | --- | --- | --- |
| whole track | 0.057 | 0.17 | 32768 |
| flagged band at 11.93 s | 0.048 | 0.20 | 32768 |
| flagged band at 43.63 s | 0.064 | 0.22 | 32767 |
| clean control at 60 s | 0.061 | 0.17 | 32768 |

The flagged bands are **statistically indistinguishable from the rest of the
track**. They are loud, bright music. `g_cdda_slew_*` measures loudness, not
corruption, and must not be read as corruption.

**And the calibration did not save it, for a reason worth writing down.**
`--cdda-tone` proves the check is silent on a *clean* signal; it says nothing
about whether it is silent on a *legitimate* one. The control that was needed
was the music itself — is the flagged part different from the rest of the same
track — and that took four seconds offline once the file was in hand. §11's rule
needs the second half: prove the instrument does not fire on the thing you are
trying to exclude, not only on the thing you know is fine.

**So there is no evidence of audio corruption anywhere.** The transfer is
byte-exact (two independent scans agreeing to the frame), the delivery counters
are clean, the sound-RAM ring is clean during the intro, and the samples on the
disc are the samples that arrive. The intro crackle has no instrumental support
for being a data problem at all.

**WHICH POINTS SOMEWHERE THIS FILE HAS NEVER LOOKED: THE MIXER SUM.** DISDL is
the send level into the AICA's direct mixer and this loader writes 0xf, full,
into a mixer the title is already feeding — at a master volume this loader
itself raised to maximum (`aica_mixer_on`, `g_cdda_mvol_raised` 1, the title
having left MVOL at 0). A commercial CD is mastered near 0 dBFS. Two full-scale
sources summed at full send clip, and clipping sounds exactly like crackling
that is worse in loud passages — which is where the slew check fired, because
that is what it actually measures. Every counter here stays healthy because the
sum happens downstream of everything any of them can read: §14.21 again, the
same blind spot as the attenuation bug.

`CDDA_DISDL` (default 0xf, isoldr's value) is the knob; about 3 dB a step.
`make loaders CDDA_ADPCM=0 CDDA_RING_TOP=0x1C0000 CDDA_DISDL=0xc` is the A/B,
deployed as `loaders-pcm-lowvol`. If the crackle goes with the level, it is the
sum; if it does not, the mixer is cleared too and the knob costs nothing.

One flake was found and fixed on the way, and it was in the instrument's own
tests: `a_reply_is_claimed_in_the_io_layer_wherever_it_is_polled` sends two
datagrams and asserts that ONE wakeup drains both, which is only measurable once
both are actually queued. On loopback they normally are; about one run in fifty
they were not, and the test failed reporting nothing claimed. The product
guarantee is unchanged — that failure was the test measuring before the thing it
measures had happened.

**RE-RUN 2026-09-04, AND THE ANSWER IS THE THIRD ONE: PCM GLITCHES, BUT
DIFFERENTLY.** Reported as "a lot of crackling in the intro sequence music. It
recovers pretty fast, but it's just always occurring" — which is not the ADPCM
symptom at all. **So the underlying event is the same and the format is the
amplifier**: one bad splice is a click PCM shakes off within a sample and a
predictor ADPCM never recovers from, heard as one ear wandering for anything up
to twenty seconds. That reconciles both observations and it settles the
direction: **there is a recurring corruption in the arriving data**, it happens
often, and every one of them is audible in PCM. Chasing the format was chasing
the gain; the event is upstream, and with the streamed tone clean it is in the
fetch, the wire, or the host's read of the disc.

**Register 0 was missing too, and it is the most informative of the lot.**
isoldr checks the control word FIRST and treats a mismatch as its most severe
level (`check_status`, `cdda.c:425`, tested at `:485`): it holds KYONB, the
sample format, the loop flag and the top bits of the sample address, so **a
title that keys our channel off, or points it at its own sample, changes this
word and nothing else** — one ear going completely empty, with 4, 8, 12, 24, 36
and 40 all still perfectly ours and `g_cdda_ch_stolen` reporting nothing. Bit 15
is masked out of the comparison: KYONEX is a self-clearing execute strobe, so
testing it would race the calibration that runs right after key-on. Bit 14,
KYONB, is what says the channel is meant to be playing, and it is kept.

**And the verdict is graded now, because a pan disturbance should not cost a gap
in the music.** isoldr splits its `invalid_level` by severity (`>> 4`,
`cdda.c:563`): pan or attenuation wrong → `aica_set_volume()`, which rewrites
those two registers and nothing else; anything structural → re-setup. Rewriting
an output register is instantaneous, inaudible and idempotent, so a sound driver
sweeping the mixer costs nothing, where restreaming on it would re-key our
channels several times a second and turn a cosmetic disturbance into stuttering
music. `g_cdda_ch_repairs` counts the cheap path, `g_cdda_ch_stolen` the
expensive one.

**AND THE FORMAT IS INNOCENT: PCM AND ADPCM GLITCH IDENTICALLY.** Measured
2026-09-04 with two loader sets differing in nothing but `CDDA_ADPCM`, told
apart by content (`DC23` in one image, `DC24` in the other). That kills every
explanation that turns on the decoder's running state — a lost store costing
eight samples permanently, the shared predictor, the `AICA_SM_ADPCM_LS` loop
assumption, the host's encoder — because in PCM a splice is a click that heals
within a sample and the symptom is *the same*. Whatever is left is
**format-independent and downstream of the sample data**, and
`g_cdda_ram_clobber` at 0 says the sample data is still in sound RAM when the
AICA gets there.

**Which points at the registers nobody writes.** KOS (`aica_play`,
`arm/aica.c:93`) and isoldr (`aica_setup_cdda`) both write exactly 0, 4, 8, 12,
16, 24, 36, 40 and leave **20, 28 and 32** alone — and both are entitled to,
because they own the chip and initialised it. isoldr's `aica_init()` keys off
all 64 channels and is compiled only under `HAVE_CDDA_TEST` *precisely because
doing that to a running game destroys it*, which is this loader's situation
exactly: channels 62 and 63 arrive holding whatever the title's sound driver
last left there.

**Register 28 is the LFO** — LFORE, LFOF, PLFOWS/PLFOS, ALFOWS/ALFOS. A
non-zero ALFOS with LFORE set means the channel's amplitude is being modulated,
which is "the volume weirdly changes between left and right, doing like a stereo
pan effect" with every register we do write still perfectly ours, the samples
intact, nothing lost on the wire, and no counter moving. 20 is the rest of the
envelope (KRS/DL/RR, LPSLNK in bit 14) and 32 is the DSP send (ISEL/IMXL), which
would route our audio through whatever effect the title has programmed. All
three are now written to zero at key-on and joined to the cheap-repair group,
since any of them can be put back mid-stream instantly and inaudibly.

**AND ITS FALSIFIER FIRED TOO: THE CHECK CATCHES A REAL EVENT THAT IS NOT THE
CAUSE.** Measured 2026-09-04 over 17.4 s of music, same build, same title:
`g_cdda_ch_stolen` 0, `g_cdda_ch_bad_reg` 0, `g_cdda_restreams` 0, `holes` 0,
`underruns` 0 — everything clean, and the glitches were unchanged. So a title
writing our channel registers happens, and is worth healing, and is **not** what
this is. Keep the check; stop looking here.

**AND THE CHECK FIRED, WHICH IS THE FIRST TIME ANY INSTRUMENT HAS CAUGHT THE
EVENT.** Measured 2026-09-04 on Snow Surfers, 61 s of music: `g_cdda_ch_stolen`
2, `g_cdda_ch_repairs` **0** — so not pan and not attenuation, something
structural. That rate is in the same order as the reported glitches, and it is
also the rate at which this loader now restreams, which is itself a gap: from
here the question is *what* was written, not whether. `g_cdda_ch_bad_reg`
(`(channel << 8) | offset`), `_got` and `_want` latch the **first**
disagreement, because a later one is usually the same cause seen again. The
calibration clears them, so what a reader finds latched is never the
instrument's own reflection.

A caution that comes with it: the response to a structural mismatch is a full
restream — ring cleared, channels re-keyed, host encoder reset — and for ADPCM
that is not overkill, because a key-on resets the AICA's decoder *and* moves its
read pointer back to the start of the ring, so the pacing model has to be reset
with it. But it costs ~0.19 s of silence, so a check that fires spuriously would
manufacture exactly the symptom it is looking for. `g_cdda_ch_check_ok` and the
latched evidence are what tell the two apart.

**Register 36 was missing from that list until 2026-09-03, and it is the one a
single-ear report points at.** It holds DIPAN and DISDL — the pan and the send
level to the direct mixer — so a title writing it moves the balance with every
other register still perfectly ours, every packet delivered, and every counter
healthy. Reported symptom: "the volume weirdly changes between left and right,
doing like a stereo pan effect", periodic, sometimes clearing in under a second
and sometimes lasting twenty, in the **menu** with no game started, and
`g_cdda_ch_stolen` at 0 throughout — which it would be, since the check did not
look at the pan register. `CDDA_DIPAN_DISDL(ch)` is now one definition used by
both the write and the check, so they cannot drift apart. This is §14.21 again:
the fields past the last thing this loader reads back are exactly the ones
whose corruption no instrument here can see.

**It calibrates itself before it is believed**, per §11's rule: the same
comparison runs immediately after key-on against a channel nothing has had time
to touch, and if it does not come back clean the check is disabled for the
session (`g_cdda_ch_check_ok` = 0) rather than restreaming the music into
uselessness on its own false positives. A register that turns out to be
write-only would otherwise do exactly that.

And the table of contents, which is what "the music started a few seconds late"
was: it is asked for on the first PLAY, nothing plays until it answers, and one
lost packet used to cost the full `timeout_loop` — a whole-second count, so
**three seconds** — and then give up until the next PLAY. It now gets
`CDDA_TOC_RETRIES` (4) tries at `CDDA_TOC_DEADLINE_TICKS` (500 ms). Note that it
runs *before* the channels are keyed on, so it starts TMU2 itself; without that
the fine deadline never fires and four tries would cost twelve seconds.

**What this buys is bus time, and what it costs is fidelity and a shared state.**
4 bits a sample is audibly lossier than a CD, and the failure mode is new: with
PCM a lost or repeated sample is a click that heals itself, while with ADPCM it
desynchronises the predictor and does not. The switch is one rebuild
(`CDDA_ADPCM=0`) and the host serves both — `DC23` is unchanged and still what a
PCM loader asks for. The middle option, if 4 bits is too coarse, is
`AICA_SM_8BIT` (1): half the saving, no shared state, no divergence risk.

**And the slicing is conditional, which is the part that is not optional.**
Three services per fetch caps the fetch rate at the service rate over three —
40/s at 120 services/s, fine — but the service rate *is* the frame rate, and the
frame rate sagging is the whole problem: at 30 fps the ceiling falls to 20/s and
the ring starves. So the slice applies only while the ring is at least half
full; below that the whole staging buffer goes out at once and the 1 ms spike is
taken, which is what this code did unconditionally before. A dropped frame is
cheaper than a gap in the music.

**A late part being spliced into the next fetch was suspected and REFUTED, and
the refutation is worth keeping.** `cmd_partbin` accepts a part on its address
alone and two consecutive audio fetches use byte-for-byte the same window, so a
straggler would be accepted into the wrong fetch. `cdda_fetch()` shuts the
window (`bin_window_close()`) and drains the ring on a timeout, bounded by
`CDDA_DRAIN_ITERS` **and** a 5 ms fine deadline per §4.8. Measured 2026-09-02:
`g_cdda_drains` tracked the timeouts exactly and **`g_pbin_rejected` stayed at
0** — the drains found nothing, and `g_fine_timeouts` came out at exactly twice
`g_cdda_fetch_fails` because the drain's own deadline expired every time. So the
parts are *lost*, not late, and with `g_rx_missed` and `g_rx_overflow` both zero
it is not the chip dropping them. The guard is kept: it costs nothing and it is
the check that makes the answer trustworthy.

**THE CRACKLE IS OPEN-LOOP CLOCK DRIFT, and no counter could see it because
every counter used the drifting model.** `cdda_advance()` integrates TMU2 ticks
from one `cdda_timer_start()` at key-on. TMU2 and the AICA run off different
oscillators; a 0.05 % mismatch — ordinary — is 22 samples a second against a
`FETCH_HEADROOM` cushion of 2352, so the writer reaches the reader in under two
minutes and then writes where the reader already is. The audio is complete,
delivered on time, and wrong. `g_cdda_underruns` reads 0 throughout, because the
underrun test is the same model. A lag spike eats cushion and brings the
crossing forward, which is why the symptom follows one.

**Closing that loop against the AICA's position register was tried, and the
register does not carry usable information here — established twice over, and
the code is gone.** A `cdda_lock_pos()` read it once per fetch and compared it
against the model. Measured 2026-09-02 over 10449 readings: **564 accepted
(5.40 %) against 5.38 % expected if the readings were uniform noise over the
ring** — a match to two decimals — with the largest accepted error pinned at
440, one below the 441-sample trust band. Then the discriminator, over the same
run: **2044 ahead against 2074 behind, 49.6 % / 50.4 %.** A model that had
genuinely drifted would disagree in one direction almost always; a dead-even
split is a coin. The access was byte-for-byte isoldr's `aica_get_pos()`, so the
code was not what was wrong — and **isoldr compiles that function only under
`HAVE_CDDA_TEST`** and paces from a timer in production, which now reads as
isoldr not trusting it either. **Do not re-derive a position from
`0x280c`/`0x2814` here.** What is left is `cdda_sample_pos()`, one G2 read a
fetch into `g_cdda_aica_pos`, because a value that never moves still
distinguishes a channel that never started from one playing into a muted mixer —
which is the failure that cost a session (the TL one, above).

**NOTHING EVER TOOK THE G2 BUS BEFORE WRITING TO IT, AND MASKING INTERRUPTS IS
ONLY TWO THIRDS OF WHAT THAT MEANS.** The rule everybody quotes is "drain the
FIFO, then issue at most eight 32-bit accesses". Here is what KOS actually does
(`kernel/arch/dreamcast/include/dc/g2bus.h:162`):

```c
static inline g2_ctx_t g2_lock(void) {
    ctx.irq_state = irq_disable();
    G2_DMA_SUSPEND_SPU = 1;      /* 0xa05f781c */
    G2_DMA_SUSPEND_BBA = 1;      /* 0xa05f783c */
    G2_DMA_SUSPEND_CH2 = 1;      /* 0xa05f785c */
    while(FIFO_STATUS & (FIFO_SH4 | FIFO_G2));
    return ctx;
}
```

isoldr's is the same three plus `FIFO_AICA` in the wait (`cdda.c:69`). The
drain proves the bus was quiet at that instant; `irq_disable()` stops the CPU
being taken away mid-burst. **Neither of them stops a G2 DMA ENGINE**, which is
autonomous hardware: once a title's sound driver has programmed an AICA DMA it
proceeds on its own, interleaving its transfers with our stores into the same
eight-longword FIFO, whatever SR says. Parking the three channels is the only
thing that does — and it is the step both references write without explaining,
so it is also the easy one to leave out.

Note the two waits are deliberately different masks and both are needed:
`g2_lock()` waits on `SH4|G2` (the CPU's own path must be clear before entering
the section) and the per-eight `g2_fifo_wait()` waits on `AICA|G2` (the
device's FIFO). dcload had the second right and the first absent entirely.

**And this loader runs with the title's interrupts enabled.** `go.S` hands the
game `SR = 0x60000101` — IMASK 0, deliberately, so it can take a VBlank and a
Maple completion (§4.7) — and everything in `cdda.c` executes inside a GD
syscall in that context. So the title's VBlank handler, its sound driver and
its Maple DMA are all free to land inside a push loop writing sound RAM eight
words at a time, twenty-five times a second. Verified on the shipped binary
2026-09-03: **not one `stc sr`, and not one write to `0xa05f781c`, in the whole
image.** Adding only the `irq_disable()` half (2026-09-03) changed the symptom
not at all, which is consistent — it was the DMA suspend that was missing.
`g2_lock()`/`g2_unlock()` now bracket every burst: the two push loops,
`aica_clear_rings()` (chunked, because the whole ring is 4096 stores and holding
the bus for all of them is a dropped frame at every key-on), the channel
register writes and reads, the mixer, and the position read — which must be
atomic or its own ownership test is meaningless.

**Held across a whole push, not per eight stores.** Toggling three DMA-suspend
registers 1850 times a second would disturb the title's own audio more than it
protects ours; KOS holds it across a read/write pair and isoldr across an entire
PIO transfer (`aica_transfer`, `cdda.c:190`). One slice is ~115 µs measured
(`g_cdda_push_ticks_last`), three per fetch, 25 fetches a second — 0.85 % of the
machine with interrupts masked, in slices far shorter than a frame. The
eight-store drain still runs *inside* the section, because that one is about the
AICA's FIFO rather than about who owns the bus.

**WHAT A LOST STORE COSTS DEPENDS ON THE FORMAT, WHICH IS WHY THIS SURFACED
NOW AND WHY IT ALSO EXPLAINS THE OLD CRACKLE.** In PCM one dropped word is two
samples on one channel: a click, forgiven by the ear, recorded by nothing — the
crackle three hypotheses were spent on. In ADPCM it is eight samples on one
channel, and the format carries a predictor and a step size across them, so
**that ear is wrong from then on** — quiet, distorted, or scaled by whatever the
step ratio became — until something re-keys the channel. One ear "as if the
cable was not connected properly"; the balance wandering between left and right
for a second or twenty. Nothing on the wire is lost, no part is rejected, no
ring runs dry, and the channel registers still read back what we wrote. That is
every measurement taken so far, and none of them could have seen this.

**THE RING WAS KEPT BRIM-FULL, WHICH PUT ALL THE MARGIN ON THE FAILURE THAT
ANNOUNCES ITSELF.** Two things can go wrong and only one of them is visible: the
reader catching the writer is an underrun, which `cdda_room()` counts and
`cdda_restream()` heals, while the writer lapping the reader overwrites samples
the AICA has not read yet with **no instrument that can see it** — the only
evidence would be the play position, and that register is noise while a title
owns the machine. On the shipped geometry the fetch gate let the depth reach
15795 of 16384 samples: **589 samples — 13 ms — from the silent failure,
against 358 ms from the counted one.** Twenty-seven to one, the wrong way round.

Thirteen milliseconds is 0.03 % over forty seconds, less than two crystals that
are not the same crystal will differ by. **And ADPCM changed what a crossing
costs**: a splice used to be a click the decoder shook off within a sample, and
now each channel carries a predictor and a step size across it, so the two ears
come out scaled by different factors and stay that way until a step hits its
clamp — heard as the balance wandering between left and right for anything from
a second to twenty. `CDDA_TARGET_LEAD` (half the ring) makes the two margins
equal at 119 ms of buffered audio and 199 ms of clearance; 119 ms still absorbs
two consecutive 50 ms fetch deadlines, and a drift now hits the counted,
self-healing failure first. `g_cdda_room_min` is the achieved clearance and
settles at ~8800.

**Its falsifier fired and the hypothesis is dead.** Measured 2026-09-03 with
the cap in: `g_cdda_room_min` held at 9524 — the cap works — and the balance
wander was **unchanged**. So the writer lapping the reader was not the cause.
The cap is kept anyway, on structure: 27-to-1 margins on the wrong side of a
silent failure are indefensible whatever else turns out to be true, and
`room_min` is now a live check that they stay symmetric.

This is **not** the fill cap §4.13 records as reverted: that one was tried
against the PCM crackle alongside the position servo and went out with it when
the pair changed nothing. The slice test moved with it — it read
`RING_SAMPLES / 2`, which was half of the old brim-full depth and is the whole
of the new one, so capping the fill without touching it would have switched the
push slicing off and handed the 1 ms spike back to the frame loop.

**And the play position gets one more question asked of it, with the mechanism
the attempt that closed the file did not have.** `0x280c` is a monitor SELECT
and the title's sound driver writes it, so a reading can be somebody else's
channel with no way to tell afterwards — which is the accepted explanation for
the 2026-09-02 result. `aica_play_pos()` now **reads the select back** and
discards the sample unless it is still ours, which splits "the readings are
noise" into three separable answers: `g_cdda_pos_taken` tracking
`g_cdda_pos_tries` means the register is contested and unusable, full stop;
`g_cdda_pos_ours` climbing with a large `g_cdda_pos_err_last` means the reading
is ours and the *model* is wrong; small means there is a clock here after all.
Still measured, never acted on, per the standing rule.

Measured 2026-09-03: **517 taken out of 517 tries, 100.0 %.** That is too clean
for a race, and it is exactly what an unreadable field looks like — so the
counter alone cannot tell "the title takes it every time" from "the select does
not read back", and `g_cdda_pos_wild`/`_ours` staying at 0 is consistent with
both. `g_cdda_mslc_raw` publishes the raw word so the two are distinguishable
by inspection, and the read is now inside `g2_lock()` so an interrupt between
the write and the read-back cannot be mistaken for the title. Note
`g_cdda_aica_pos` has read 0 for every session on record, which is its own
hint.

**The model's own clock is now published so the OTHER end can check it, which
is the one calibration this file has never had.** Everything here that involves
time comes out of `cdda_advance()`: TMU2 ticks in at an assumed 12.5 MHz,
played samples out at an assumed 44100 Hz. If either assumption is wrong the
ring drifts into the reader and **no counter on the console can say so**, since
`g_cdda_underruns` is computed from that same model and agrees with itself
whatever the truth is. `g_cdda_ticks` (accumulated TMU2 ticks, clamped exactly
as `played` is, so the pair describes the model as it really ran) and
`g_cdda_played` are two adds on a path that already reads the timer, and the
host divides them by **its own** elapsed time: 12 500 000/s and 44 100/s are
what a correct model looks like. Note this only works against a **measured**
sample interval — §16.

`CDDA_TARGET_LEAD` — capping the fill at half a ring so the seam sits 186 ms from
the reader in both directions rather than 53 ms in front — was tried against the
crackle in the same pass and **did not change it**, so it was reverted with the
servo. The reasoning still holds and is worth having written down if the drift
hypothesis ever comes back with evidence; what it did not do is fix anything.

`cdda_advance()`'s constant, however, is kept exact: the ratio is 3699.376 and
the constant was 3699, so the model ran 0.0102 % slow all by itself — 4.5 samples
a second. `+ (dt * 3) / 8` brings it to 3699.375, an error of 0.000027 %, for one
multiply and a shift. Q20 cannot simply be widened: `TICK_CLAMP` is 1000000 and
`dt * 3699` already reaches 3.7e9 of a 32-bit range.

**So the crackle is still open, and three hypotheses have now been killed by
their own falsifiers** — a late part spliced into the next fetch, a drifted model
correctable from the hardware, and the seam sitting too close to the reader. What
is established is only that the ring never underruns by its own accounting, that
no part is rejected, that no frame is dropped by the chip, and that the missing
packets are lost rather than late. **The next place to look is the one thing
nothing here checks: whether the two AICA channels are still ours** — see the
known gaps below, and `aica_check_cdda()`.

**THE ONE LINK NEVER CHECKED IS THE SOUND RAM ITSELF.** Everything measured so
far says the bytes leave this loader intact — nothing lost on the wire, nothing
refused, the ring never dry by its own accounting, the channel registers reading
back what was written, the G2 bus now taken properly before every burst — and it
still glitches. The rings live in the **top 16 KB of sound RAM**
(`0x1f0000..0x1fffff`), which is where isoldr puts its own and which **a title is
perfectly entitled to use for its samples**. If the game's sound driver stores
there, our audio is overwritten *after* we write it and *before* the AICA reads
it: one ear damaged more than the other depending on where its buffer lands, for
as long as it keeps using that memory, with not one counter in this file able to
see it.

The check is nearly free and it is exact, because **the AICA never writes sample
RAM**: a word we wrote and nobody else touched must read back unchanged. Each
push remembers its last store, alternating ears; the next push — ~13 ms later,
with the reader ~119 ms behind, so we are not racing ourselves — reads it back.
A mismatch is somebody else's write, full stop. `g_cdda_ram_clobber` with
`_addr` / `_got` / `_want`. A re-key disarms it, since that rewrites the ring.

**And a PCM loader set is kept for the A/B** (`make loaders CDDA_ADPCM=0`,
deployed beside the default as `loaders-pcm/`, selected with `--loader-dir`).
It splits the remaining space in half: PCM clean means the damage is specific to
the format — the shared encoder/decoder state, the running predictor, the
`AICA_SM_ADPCM_LS` loop assumption; PCM glitching identically means the format
is innocent and the cause is common to both paths. Tell the sets apart by
content, not by path: the wire command is `DC23` in a PCM image and `DC24` in an
ADPCM one, and `strings` finds exactly one of the two.

**A key-off needs a moment to take, and isoldr says so at the one place it
matters**: "Need wait a bit before setup channels again, otherwise it doesn't
stop and playback will be out of sync" — a 1 ms BIOS spin at the end of
`aica_stop_cdda()` (`cdda.c:349`). `cdda_channels_stop()` now spins 12500 TMU2
ticks for the same reason. Every caller that stops is either about to start
again (`cdda_restream()`, `cdda_release()`, `cdda_seek()`) or stopping for good,
where it costs nothing.

Geometry checked against `setup_pcm_buffer()` while reading it, and it agrees
byte for byte: ADPCM `size` = 0x4000, 8192 bytes a channel, `end_pos = size - 1`
= 16383 in **samples**, left at `AICA_MEMORY_END - size` and right at
`- (size >> 1)`, sound RAM end 0x200000. So the format, the bases, the LEA and
the ring length are all isoldr's own numbers.

isoldr does not have this problem because it does not integrate: it restarts its
timer every half-buffer and `aica_get_pseudo_pos()` measures within one, against
an `end_tm` calibrated per motherboard revision (`setup_pcm_buffer` carries the
VA0/VA1 numbers and notes which buffer sizes give "no error"). Re-anchoring 25
times a second is the same idea against a different reference.

Known gaps: **nothing checks that the channels are still ours.** isoldr re-reads
its two channels every service and counts the registers that no longer hold what
it wrote (`aica_check_cdda()`, `cdda.c:474`), setting `restore` when four or more
disagree -- because a title's own sound driver is free to walk all 64 channels.
This loader writes them once at key-on and never looks again, so music that
starts and later dies without a counter moving is that, and `aica_check_cdda` is
where to start. `CMD_GETSCD` is still force-completed rather than answering a Q
subcode, exactly as isoldr leaves it; a title that reads its playback position
from the subcode rather than from `CMD_REQ_STAT` gets nothing moving.
ADPCM tracks are not handled — a CD audio track is linear PCM by definition,
and isoldr's `adpcm_split` is for its own `.raw` conversions.
