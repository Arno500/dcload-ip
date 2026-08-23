!
! Hand the machine over to the loaded program.
!
! THE SR VALUE IS NOT COSMETIC. This used to load SR = 0x500000f0, which is
! MD=1, RB=0, **BL=1** and **IMASK=15** -- exceptions blocked and every
! interrupt level masked. A title started that way can never take a VBlank, a
! Maple completion or a GD DMA completion until it clears those bits itself,
! and any exception it does take becomes a MANUAL RESET rather than a fault it
! could handle.
!
! CORRECTION, 2026-08-16, read off the isoldr sources in
! kos/ds/firmware/isoldr/loader/. This file used to claim isoldr hands a title
! SR = 0x60000101 (BL=0, IMASK=0). IT DOES NOT. startup.s' _boot_sr literal is
! **0x700000f0** -- MD=1, RB=1, BL=1, IMASK=15 -- and 0x60000101 appears in
! main.c only inside the NAOMI branch, which also moves VBR to 0x0c000000 and
! the stack to 0x0cc00000. So for a Dreamcast title the reference loader masks
! everything, and the value this file replaced (0x500000f0) differed from
! isoldr's only in RB. ISOLDR_HANDOFF below restores isoldr's actual value.
!
! It also explains a measurement that had sat unexplained for a long time:
! Sonic Adventure's interrupt entry counter read ZERO across a quarter of a
! million GD syscalls. Not because the VBR was wrong, but because nothing could
! ever be delivered through it.
!
! RB=1 SWITCHES THE REGISTER BANK, so r0-r7 change meaning the instant SR is
! written. The entry address arrives in r4 and would evaporate there. It is
! stashed in r14 first, which is not banked -- exactly what isoldr does.
!
! CACHES ARE HANDED OVER DISABLED, AND THIS IS A DELIBERATE DIVERGENCE.
! isoldr writes CCR = 0x0909 here (caches on) because its transports are DMA:
! the device writes the game's buffer directly and the loader only has to purge
! afterwards. Ours is not. dcload fills those buffers with CPU stores from
! inside a syscall, while the game reads them back uncached because it asked
! for a DMAREAD -- isoldr's own notes say exactly that, "parce que le jeu
! relira le buffer en non cache". Running the guest with caches off makes that
! coherent by construction instead of by remembering to purge every path.
!
! THE STACK AND VBR HANDED OVER DO NOT FOLLOW THE LOADER. DCLOAD_GUEST_VBR is
! 0x8c00f400 for every base, because that is the BIOS VBR and it is what isoldr
! hands every title from every value of its `memory` setting -- startup.s'
! _boot_vbr and _boot_stack are plain constants there, overridden only on the
! NAOMI path. When the loader is relinked into high RAM to get out of a title's
! way, exception.bin still goes to 0x8c00f400, and the loader's own stack
! (_stack) is the one that moves. At the stock base the two coincide, which is
! why this file carried a single literal for both until the base became
! per-title.
!
! GUEST_IRQ_MASKED -- A DIAGNOSTIC, NOT A FIX.
! The vector table this loader hands the title (exception.S, VBR + 0x600) is
! `nop; rte; nop`: it ACKNOWLEDGES NOTHING. With IMASK=0 above, any interrupt
! that is still enabled when the title starts -- Holly's SB_IML*NRM/EXT/ERR and
! the SH4's own IPRA/B/C, both left however the BIOS set them, since neither
! 1st_read nor dcload ever clears them -- is taken, returned from unacknowledged,
! and immediately taken again. That is a livelock before the title executes an
! instruction of its own: black screen, no exception (BL=0, so a real fault would
! still reach the dump), no disc read, and dcload never gets the CPU back because
! its only re-entry is a GD syscall.
!
! It cannot show up under flycast+REIOS, where no BIOS ever programmed those
! enables. And a title that installs its own handlers into our VBR early enough
! -- Sonic Adventure does -- never notices.
!
! Setting this to 1 masks every interrupt at handoff (IMASK=15) and changes
! nothing else, BL included. If a title that hung now boots, the answer is to
! quiesce Holly and the INTC before go() and leave SR permissive -- not to ship
! this, which takes back the interrupt latency the value above exists to give.
!
! Writing CCR from a cached region is illegal (SH4 requires P2 plus a settling
! window), so the whole handoff body runs from P2 -- the same shape as
! disable.s, and the same reason isoldr calls its own _boot_stub through
! NONCACHED_ADDR(). Doing it HERE rather than trusting the caller means the
! guest cannot be entered with caches on by some future path into go().
!
	.section .text
	.global _go

_go:
	mov.l	go_body_k,r0
	mov.l	p2_mask,r1
	or	r1,r0
	jmp	@r0
	mov	r4,r14			! delay slot: entry, out of the banked set

	.align 2
go_body:
	mov.l	stack_addr_k,r0
	mov.l	@r0,r15
	mov.l	entry_addr_k,r0
	mov.l	@r0,r0
	lds	r0,pr			! a program that returns lands back in dcload
	mov.l	vbr_data_k,r0
	mov.l	@r0,r0
	ldc	r0,vbr
	mov.l	fpscr_data_k,r0
	mov.l	@r0,r0
	lds	r0,fpscr
#if ISOLDR_REGS
	mov.l	gbr_data_k,r0
	mov.l	@r0,r0
	ldc	r0,gbr			! isoldr sets GBR; we never did. A KATANA
					! title reaches the BIOS syscall vectors as
					! @(0xb0..0xbc, GBR), so a wrong GBR calls
					! through garbage.
#endif
	mov.l	sr_data_k,r0
	mov.l	@r0,r0
#if ISOLDR_REGS
	ldc	r14,spc			! isoldr: SPC = the entry point ...
	ldc	r0,ssr			! ... and SSR = the SR being installed, so
					! a title may enter itself with an rte.
#endif
	ldc	r0,sr			! bank may flip here: nothing in r0-r7 is live
	mov.l	ccr_addr_k,r0
	mov.l	ccr_data_k,r1
	mov.l	@r1,r1
	mov.l	r1,@r0			! caches off + invalidate
	mov	#0,r0			! 1 -- also the SH4 settling window after CCR
	mov	r0,r1			! 2
	mov	r0,r2			! 3
	mov	r0,r3			! 4
	mov	r0,r4			! 5
	mov	r0,r5			! 6
	mov	r0,r6			! 7
	mov	r0,r7			! 8
	mov	r0,r8
	mov	r0,r9
	mov	r0,r10
	mov	r0,r11
	mov	r0,r12
	mov	r0,r13
	clrmac
	clrt
	jmp	@r14
	mov	r0,r14

	.align 2
go_body_k:
	.long	go_body
p2_mask:
	.long	0xa0000000
ccr_addr_k:
	.long	0xff00001c
ccr_data_k:
	.long	ccr_data
ccr_data:
#if GUEST_CACHES_ON
	.long	0x00000909	! isoldr's value: both caches ON, both invalidated
#else
	.long	0x00000808
#endif
stack_addr_k:
	.long	stack_addr
stack_addr:
	.long	DCLOAD_GUEST_VBR
entry_addr_k:
	.long	entry_addr
entry_addr:
	/* Where a program that RETURNS lands: the loader's own entry point, via
	 * P2 because caches are off by the time it gets here. Taken from the
	 * linker's ORIGIN rather than restated, since the loader's base is
	 * per-title now (see the Makefile header). */
	.long	_dcload_base + 0x20000000
sr_data_k:
	.long	sr_data
sr_data:
#if GUEST_IRQ_MASKED
	.long	0x600001f1	! same, with IMASK=15 -- see the header
#elif ISOLDR_SR
	.long	0x700000f0	! isoldr's own _boot_sr, verbatim
#else
	.long	0x60000101
#endif
#if ISOLDR_REGS
gbr_data_k:
	.long	gbr_data
gbr_data:
	.long	0x8c000000	! the BIOS syscall base -- isoldr's newgbr
#endif
vbr_data_k:
	.long	vbr_data
vbr_data:
	.long	DCLOAD_GUEST_VBR
fpscr_data_k:
	.long	fpscr_data
fpscr_data:
#if ISOLDR_REGS
	.long	0x00040001	! isoldr's value: RM=1, round toward zero
#else
	.long	0x40000
#endif
