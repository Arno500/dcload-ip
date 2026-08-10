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
! That is not what a Dreamcast hands a game, and it is not what DreamShell's
! isoldr hands one either: its _boot_stub uses boot_sr = 0x60000101, i.e. MD=1,
! RB=1, BL=0, IMASK=0. isoldr's notes are blunt about this -- games are
! extremely sensitive to the initial register state -- and it is the reference
! implementation that runs retail titles end to end.
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
	mov.l	sr_data_k,r0
	mov.l	@r0,r0
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
	.long	0x00000808
stack_addr_k:
	.long	stack_addr
stack_addr:
	.long	0x8c00f400
entry_addr_k:
	.long	entry_addr
entry_addr:
	.long	0xac004000
sr_data_k:
	.long	sr_data
sr_data:
	.long	0x60000101
vbr_data_k:
	.long	vbr_data
vbr_data:
	.long	0x8c00f400
fpscr_data_k:
	.long	fpscr_data
fpscr_data:
	.long	0x40000
