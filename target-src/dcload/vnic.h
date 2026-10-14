#ifndef __VNIC_H__
#define __VNIC_H__

/*
 * The title's own Ethernet driver, served by the loader (vnic.c, AGENTS.md
 * 4.21). Built when WITH_VNIC=1: resident image, HIGH layout, with the hook.
 */

#ifndef WITH_VNIC
#define WITH_VNIC 0
#endif

#if WITH_VNIC
/* Non-zero while the title's driver is switched on (its ETD_CtlXmit(1)): the
 * adapter keeps receiving between the loader's own exchanges. */
extern volatile unsigned int g_vnic_on;

/* A frame just received (`len` bytes, CRC excluded): hand it to the title if
 * it is not the loader's. Returns 1 when the loader must not process it. */
int vnic_rx(unsigned char *frame, unsigned int len);

/* From the tick, GD lock free and no CD-DA fetch on the network: send what
 * the title queued, keep reception on, drain the ring for it. Non-zero if it
 * used the network. */
int vnic_tick(unsigned int t0);

/* At EXEC, for a disc image's title: the flash overlay with the adapter's IP
 * settings, if the host gave them (g_vnic_isp). Takes the flashrom vector. */
void vnic_flash_arm(void);

#define vnic_keeps_rx() (g_vnic_on)
#else
#define vnic_flash_arm()    do { } while (0)
#define vnic_rx(frame, len) 0
#define vnic_keeps_rx()     0
#endif

#endif
