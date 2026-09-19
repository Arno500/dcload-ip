#ifndef __CDFS_H__
#define __CDFS_H__

void cdfs_redir_enable(void);
void cdfs_redir_save(void);
void cdfs_redir_disable(void);

/* GD server coroutine, implemented in cdfs_redir.s. gdcExitToGame() may only
 * be called from inside gdcServerMain() or something it calls -- from any
 * other context there is no parked game frame to return to. */
void gdcExitToGame(void);
int gd_lock(void);
void gd_unlock(void);

/* Read-only views of cdfs_redir.s' own state, for the stuck-lock watchdog.
 * gd_park_end is a LOCATION -- its ADDRESS is the empty-buffer value that
 * gd_park_ptr is compared against. */
/* Non-zero while a disc read or TOC transfer owns bin_info and pkt_buf. Nothing
 * else may start a transfer then -- see the note at its definition. */
extern unsigned int g_gd_in_transfer;

extern volatile unsigned int gd_lock_state;
extern volatile unsigned int gd_park_ptr;
extern unsigned int gd_park_end;

/* The server task itself; entered from the assembly, never returns. */
void gdcServerMain(void);

/* Claim/refresh the post-mortem block in high RAM. Call once per dcload boot,
 * before anything else can write to it. */
void cdfs_pm_boot(void);

#endif
