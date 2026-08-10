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

/* The server task itself; entered from the assembly, never returns. */
void gdcServerMain(void);

/* Claim/refresh the post-mortem block in high RAM. Call once per dcload boot,
 * before anything else can write to it. */
void cdfs_pm_boot(void);

#endif
