#ifndef __IRQ_H__
#define __IRQ_H__

/* The interrupt hook (irq.c, irq_hook.S). Built when WITH_IRQ_HOOK=1. */

#ifndef WITH_IRQ_HOOK
#define WITH_IRQ_HOOK 1
#endif

#if WITH_IRQ_HOOK
/* From each GD syscall of a title running with the MMU on (Windows CE): make
 * sure the live vector table carries the hook, installing it on a table that
 * shows the expected words and on nothing else. Cheap when already there. */
void irq_hook_check(void);

/* Called by irq_entry with every register saved, SR.BL = 1, on the loader's
 * stack. Must not fault and must stay bounded in time. */
void irq_tick(void);

/* Route the BBA's RX interrupt to us while a disc read is on the wire
 * (Katana titles, on a Holly level they leave unused). */
void irq_rx_arm(unsigned int on);

extern volatile unsigned int g_irq_hooked;
#else
#define irq_hook_check() do { } while (0)
#define irq_rx_arm(on)   do { } while (0)
#endif

#endif
