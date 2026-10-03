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

/* Run fn on the loader's own stack (cdfs_redir.s). Interrupts must already be
 * masked: see gd_exchange() in cdfs_syscalls.c. */
void gd_on_loader_stack(void (*fn)(void));

/* Run fn without a thread switch and, under Windows CE, off the title's
 * virtual stack (cdfs_syscalls.c). */
void gd_exchange(void (*fn)(void));

/* Put the real GD-ROM drive in <STANDBY> through the BIOS driver, once, at
 * boot. Only valid after cdfs_redir_save()/cdfs_redir_disable() have pointed
 * the syscall vector back at the BIOS. Built only when WITH_GD_SPINDOWN=1;
 * the result is latched in g_gd_spindown (cdfs_syscalls.c). */
void gd_spin_down_drive(void);
extern unsigned int g_gd_spindown;

/* Advance a disc read in flight from the interrupt hook's tick, GD lock free.
 * Non-zero if one was in flight (cdfs_syscalls.c, data_transfer_async). */
int gd_async_tick(void);
int gd_async_busy(void);
/* From the tick: listen to the network for up to 1 ms if nothing else owns
 * it. Non-zero if it did (cdfs_syscalls.c, IRQ_IDLE_LISTEN). */
int gd_idle_listen(void);

#endif
