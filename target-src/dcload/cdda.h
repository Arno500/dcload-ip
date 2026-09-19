/*
 * cdda.h -- CD-DA (Redbook audio) playback: the loader is the drive.
 *
 * A title plays its disc's audio tracks with CMD_PLAY_TRACKS /
 * CMD_PLAY_SECTORS. cdda.c answers those itself: it fetches the audio sectors
 * from the host over UDP (4-bit ADPCM by default) and loops them on AICA
 * channels 62/63 from a ring in sound RAM, whose write head it keeps a fixed
 * lead of audio ahead of the play position. The design is described at the top
 * of cdda.c.
 *
 * cdda_service() must be called regularly, and only at the top level of a GD
 * syscall: the GD server loop and gdGdcGetDrvStat call it, and the disc read
 * loop calls cdda_service_between_chunks(). A title that stops calling the GD
 * driver stops feeding the music.
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

/* CDDA_PlayTracks / CDDA_PlaySectors: the arguments arrive from the title in
 * _GDS.param[0..2] and mean first, last, loop. `loop` is 0x0f for "forever" in
 * the BIOS convention; anything else is a repeat count. */
int cdda_play_tracks(unsigned int first, unsigned int last, unsigned int loop);
int cdda_play_sectors(unsigned int first, unsigned int last, unsigned int loop);
int cdda_pause(void);
int cdda_release(void);
int cdda_stop(void);
int cdda_seek(unsigned int lba);

/* Keep the stream fed. Cheap when there is nothing to refill. */
void cdda_service(void);
/* The same, without the 1 ms listening window: for the disc read loop. */
void cdda_service_between_chunks(void);

int cdda_state(void);

/* What the title is told about the audio, for CMD_REQ_STAT and CMD_GETSCD. */
unsigned int cdda_current_lba(void);     /* FAD the listener is hearing */
unsigned int cdda_track(void);           /* current track number, or 0 */
unsigned int cdda_repeat(void);          /* remaining repeats (0x0f = forever) */
unsigned int cdda_audio_status(void);    /* SCD audio status (0x11 playing ...) */

#else

#define cdda_play_tracks(a, b, c)   (-1)
#define cdda_play_sectors(a, b, c)  (-1)
#define cdda_pause()                (-1)
#define cdda_release()              (-1)
#define cdda_stop()                 (-1)
#define cdda_seek(a)                (-1)
#define cdda_service()              do { } while (0)
#define cdda_service_between_chunks() do { } while (0)
#define cdda_state()                (CDDA_STOPPED)
#define cdda_current_lba()          (0u)
#define cdda_track()                (0u)
#define cdda_repeat()               (0u)
#define cdda_audio_status()         (0x15u)

#endif /* WITH_CDDA */

#endif /* __CDDA_H__ */
