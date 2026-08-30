/*
 * cdda.h -- CD-DA (Redbook audio) playback, on isoldr's model.
 *
 * WHAT THIS IS FOR
 *
 * A GD-ROM carries its music as ordinary audio tracks, and a title plays them
 * by asking the GD driver for CMD_PLAY_TRACKS / CMD_PLAY_SECTORS and then
 * getting on with its frame loop -- the drive does the rest. There is no drive
 * here, so nothing plays: dcload used to answer those commands with
 * "COMPLETED, and the drive is spinning", which is honest about the contract
 * and silent about the music. isoldr's answer is to BE the drive: read the
 * audio track off the storage device, and feed the samples to two AICA
 * channels itself (loader/cdda.c, 1589 lines). This is that, for this
 * transport.
 *
 * WHAT IS DIFFERENT HERE, AND WHY
 *
 *  - THE SOURCE IS THE NETWORK. isoldr reads its track from an IDE/SD device
 *    by DMA or PIO, which is what its `cdda` preset field's SRC_DMA/SRC_PIO
 *    bits select. We ask the host for raw 2352-byte sectors over UDP
 *    (CMD_CDDAREAD), so those two bits have no meaning for us and the transfer
 *    is synchronous like every other read dcload makes.
 *
 *  - THE DESTINATION IS PIO. isoldr's DST_DMA/DST_SQ/DST_PIO pick between AICA
 *    DMA, store queues and CPU writes. dcload writes with the CPU, 32 bits at
 *    a time with a G2 FIFO wait every eight -- the one method that needs no
 *    setup, cannot collide with the BBA's own G2 traffic through a DMA channel
 *    we do not own, and is fast enough by two orders of magnitude: the stream
 *    needs 176 KB/s and G2 PIO does tens of MB/s.
 *
 *  - THE POSITION COMES FROM THE HARDWARE, not from a timer. isoldr's
 *    POS_TMU1/POS_TMU2 bits pick an SH4 timer and it derives a PSEUDO position
 *    from the elapsed count, because its own playback is driven from that
 *    timer's interrupt. We read the channel's real play position out of the
 *    AICA (registers 0x280c/0x2814), which needs no timer, cannot drift, and
 *    costs one G2 round trip. dcload has no timer to spare in any case --
 *    perfctr.c owns counter 1 for the DHCP lease and the adapter timeouts.
 *
 *  - THE CHANNELS ARE FIXED at 62 and 63, isoldr's CH_FIXED. CH_ADAPT hunts
 *    for a channel the game has left alone; it needs the game to have started
 *    its own sound driver first, and it is the option DreamShell's own preset
 *    for the title this was written for does not use.
 *
 * WHEN IT RUNS
 *
 * cdda_service() is the whole engine, and it is called from exactly where
 * isoldr calls CDDA_MainLoop(): the GD server's dispatch loop and
 * gdGdcGetDrvStat (loader/syscalls.c:866 and :1067). Those are the two places
 * a title touches every frame, and they are TOP LEVEL of a syscall -- which
 * matters, because the fetch transmits, and AGENTS.md 4.5 forbids transmitting
 * nested inside bb->loop().
 *
 * That is also the limit worth stating plainly: a title that stops calling the
 * GD driver stops feeding the music, and after the ring drains it goes quiet.
 * isoldr covers that case from the ASIC interrupt (its `irq` preset field);
 * see cdda_service_irq() and vbr_hook.c for this loader's version.
 */

#ifndef __CDDA_H__
#define __CDDA_H__

#ifndef WITH_CDDA
#define WITH_CDDA 1
#endif

/* Drive status as CMD_PLAY/PAUSE would leave it, for gdGdcGetDrvStat. */
#define CDDA_STOPPED  0
#define CDDA_PLAYING  1
#define CDDA_PAUSED   2

#if WITH_CDDA

/* isoldr's CDDA_PlayTracks / CDDA_PlaySectors: the arguments arrive from the
 * title in _GDS.param[0..2] and mean first, last, loop. `loop` is 0x0f for
 * "forever" in the BIOS convention; anything else is a repeat count. */
int cdda_play_tracks(unsigned int first, unsigned int last, unsigned int loop);
int cdda_play_sectors(unsigned int first, unsigned int last, unsigned int loop);
int cdda_pause(void);
int cdda_release(void);
int cdda_stop(void);
int cdda_seek(unsigned int lba);

/* Keep the stream fed. Cheap when there is nothing to do: one G2 register read
 * to find the play position, and a return. */
void cdda_service(void);

/* The same, from an interrupt: pushes samples already in the staging buffer
 * but never transmits, because pkt_buf belongs to whatever the interrupt
 * suspended. See vbr_hook.c. */
void cdda_service_irq(void);

int cdda_state(void);
/* The sector the stream is playing, for CMD_REQ_STAT and CMD_GETSCD. */
unsigned int cdda_current_lba(void);

#else

#define cdda_play_tracks(a, b, c)   (-1)
#define cdda_play_sectors(a, b, c)  (-1)
#define cdda_pause()                (-1)
#define cdda_release()              (-1)
#define cdda_stop()                 (-1)
#define cdda_seek(a)                (-1)
#define cdda_service()              do { } while (0)
#define cdda_service_irq()          do { } while (0)
#define cdda_state()                (CDDA_STOPPED)
#define cdda_current_lba()          (0u)

#endif /* WITH_CDDA */

#endif /* __CDDA_H__ */
