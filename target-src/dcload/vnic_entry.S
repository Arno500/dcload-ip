!
! The way into vnic.c from a title's ETD driver (AGENTS.md 4.21).
!
! The host writes a stub over the first instructions of each ETD entry point
! it recognises (dcload-ip-rs src/vnic.rs):
!
!     mov.l  @(lit,PC),r1	! _vnic_etd, relocated with the loader
!     jmp    @r1
!     mov    #index,r0		! in the delay slot: which function
!     [nop]			! when the literal needs aligning
!     .long  _vnic_etd
!
! r1 and r0 are free at a call (both compilers), r4-r7 hold the title's
! arguments. We are on the title's stack, with its SR. The Hitachi compiler's
! callers may keep MACH/MACL live across a call, so they are kept here; GCC's
! code does not otherwise reach what that compiler preserves (r8-r15, PR).
! The index goes to vnic_call() as a fourth argument, in r7: no ETD function
! takes four.
!

	.section .text
	.global _vnic_etd
	.global _vnic_pso

! Phantasy Star Online's driver is called through a table of function
! pointers its network layer fills from a literal pool: the host points
! those words (and the one naming its link test) here, at _vnic_pso + 4 * n,
! and each entry gives _vnic_etd its index (vnic_call(), 8 and up).
	.align 2
_vnic_pso:
	bra	_vnic_etd
	mov	#8,r0		! init
	bra	_vnic_etd
	mov	#9,r0		! send
	bra	_vnic_etd
	mov	#10,r0		! terminate
	bra	_vnic_etd
	mov	#11,r0		! getmac
	bra	_vnic_etd
	mov	#12,r0		! link

	.align 2
_vnic_etd:
	sts.l	pr,@-r15
	sts.l	mach,@-r15
	sts.l	macl,@-r15
	mov.l	1f,r1
	jsr	@r1
	mov	r0,r7
	lds.l	@r15+,macl
	lds.l	@r15+,mach
	lds.l	@r15+,pr
	rts
	nop
	.align 2
1:	.long	_vnic_call

! The BIOS flashrom vector 0x8c0000b8, taken by vnic_flash_arm() (vnic.c,
! "THE NETWORK SETTINGS IN FLASH"): r7 = 0 info, 1 read (r4 offset, r5
! buffer, r6 bytes), 2 write, 3 delete. A read or a write goes to
! vnic_flash_rw(), which lays the overlay over a read and lets only the kept
! block of a write reach the flash; a delete is answered as done (not -1)
! and goes nowhere; info passes through. The Hitachi compiler's callers may
! keep MACH/MACL live, so they are kept.
	.global _vnic_flash
	.align 2
_vnic_flash:
	mov	r7,r0
	cmp/eq	#3,r0
	bt/s	vf_delete
	tst	r0,r0
	bt	vf_pass
	sts.l	pr,@-r15
	sts.l	mach,@-r15
	sts.l	macl,@-r15
	mov.l	vf_rw_k,r0
	jsr	@r0
	nop
	lds.l	@r15+,macl
	lds.l	@r15+,mach
	lds.l	@r15+,pr
	rts
	nop
vf_pass:
	mov.l	vf_saved_k,r0
	mov.l	@r0,r0
	jmp	@r0
	nop
vf_delete:
	rts
	mov	#0,r0
	.align 2
vf_saved_k:	.long	_vnic_flash_saved
vf_rw_k:	.long	_vnic_flash_rw

! Entered instead of a decoded program (vnic.c, "LATE PATCHES"): KATSUO.SEA
! jumps here with r2 = r3 = the program's entry and PR as its caller left it.
! Every register goes back as it was, r2 and r3 hold the entry again, and the
! jump is KATSUO's own.
	.global _vnic_late
	.align 2
_vnic_late:
	sts.l	pr,@-r15
	sts.l	mach,@-r15
	sts.l	macl,@-r15
	mov.l	r0,@-r15
	mov.l	r1,@-r15
	mov.l	r4,@-r15
	mov.l	r5,@-r15
	mov.l	r6,@-r15
	mov.l	r7,@-r15
	mov.l	2f,r1
	jsr	@r1
	nop
	mov	r0,r3
	mov	r0,r2
	mov.l	@r15+,r7
	mov.l	@r15+,r6
	mov.l	@r15+,r5
	mov.l	@r15+,r4
	mov.l	@r15+,r1
	mov.l	@r15+,r0
	lds.l	@r15+,macl
	lds.l	@r15+,mach
	lds.l	@r15+,pr
	jmp	@r3
	nop
	.align 2
2:	.long	_vnic_late_apply
