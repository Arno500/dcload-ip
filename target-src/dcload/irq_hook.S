!
! Interrupt hook: a way into the loader that does not wait for the title to
! call the GD driver (AGENTS.md 4.15, docs/wince-investigation.md 8).
!
! Every SH4 interrupt -- Windows CE's TMU0 tick, VBlank, anything -- enters at
! VBR+0x600. irq.c copies the template below over the TITLE's live vector
! table, so that the vector's first three instructions become
! `nop; bra tramp; nop` and the trampoline sits in the free words just under
! it. The trampoline switches to a stack of the loader's own, calls
! irq_entry, and irq_entry comes back to VBR+0x606 having rebuilt what the
! three replaced instructions would have left in the registers.
!
! Layout copied into the title's table (DreamShell isoldr's, exception-lowlevel.S,
! with the trampoline moved up to end exactly at the vector):
!
!   VBR+0x5e8  mov.l stack,r15 ; mov.l r0,@-r15 ; mov.l entry,r0 ; jmp @r0 ; nop ; nop
!   VBR+0x5f4  .long stack     (written by irq.c: a linker symbol, so it relocates)
!   VBR+0x5f8  .long entry     (idem)
!   VBR+0x5fc  .long 0
!   VBR+0x600  nop ; bra VBR+0x5e8 ; nop
!   VBR+0x606  the title's own handler continues
!
! Katana titles: their entry starts with nops (irq.c), so irq_out only gives
! r0, r1 and r15 back and resumes at VBR+0x606 (see there).
!
! What is only right for Windows CE, and why irq.c patches nothing else:
!   - The three instructions replaced are CE's `mov.l @(40,r7),r6 ;
!     mov.l @(VBR+0x68c),r0 ; mov r6,r1` (Sega Rally 2's 0WINCEOS.BIN,
!     VBR 0x8c012110, checked 2026-09-27). irq.c patches only a table that
!     shows exactly those words, and irq_out rebuilds exactly them.
!   - r15 is replaced before anything is pushed: CE's thread stacks are
!     virtual, and a TLB miss with SR.BL set resets the CPU. SGR holds the
!     interrupted r15 and gives it back on the way out.
!
! State on entry to irq_entry: SR.MD = SR.RB = SR.BL = 1, r15 = our stack with
! the title's bank-1 r0 pushed, r0 free. Nothing can interrupt this code, and
! nothing in it may fault: it touches P1/P2 only.
!
! THE FPU. GCC uses the FP registers as spill space (-m4-single-only), and
! SH4_aligned_memcpy uses fschg/fmov.d. CE switches the FPU lazily, so the
! interrupted thread may run with SR.FD = 1, where the first FPU instruction
! would raise an exception -- with BL = 1, a reset. So the slow path clears
! FD and saves FPSCR, FPUL and both register banks before any C runs.
!
! Bank-1 r0-r7 are CE's registers (r7 is its kernel data pointer, which the
! rebuilt prologue reads). C code runs in bank 1 here, so all of them are
! saved. Bank 0 is the interrupted thread's, and C does not reach it.
!

	.section .text

	.global _irq_tramp
	.global _irq_tramp_stack
	.global _irq_tramp_entry
	.global _irq_tramp_vec
	.global _irq_tramp_end
	.global _irq_entry
	.global _irq_icache_flush

!
! The template. Never executed here: irq.c copies [_irq_tramp, _irq_tramp_end)
! to VBR+0x5e8, so _irq_tramp_vec lands on VBR+0x600. It must start 4-aligned
! (the literals are PC-relative and the destination is 4-aligned).
!
.align 2
_irq_tramp:
	mov.l	_irq_tramp_stack,r15
	mov.l	r0,@-r15
	mov.l	_irq_tramp_entry,r0
	jmp	@r0
	nop
	nop
_irq_tramp_stack:
	.long	0
_irq_tramp_entry:
	.long	0
	.long	0
_irq_tramp_vec:
	nop			! never a branch as a vector's first instruction
	bra	_irq_tramp
	nop
_irq_tramp_end:

!
! void irq_entry(void) -- see the header for the state it is entered in.
!
.align 2
_irq_entry:
	mov.l	r1,@-r15
	mov.l	ie_entries_k,r0
	mov.l	@r0,r1
	add	#1,r1
	mov.l	r1,@r0
	mov.l	ie_work_k,r0
	mov.l	@r0,r0
	tst	r0,r0		! T is dead: the title's handler sets it at +0x608
	bt	irq_out

	mov.l	r2,@-r15
	mov.l	r3,@-r15
	mov.l	r4,@-r15
	mov.l	r5,@-r15
	mov.l	r6,@-r15
	mov.l	r7,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	sts.l	mach,@-r15
	sts.l	macl,@-r15
	sts.l	pr,@-r15
	stc.l	gbr,@-r15		! 19 longs: r15 = 4 mod 8, as fpu_push wants
	mov.l	ie_push_k,r0
	jsr	@r0		! SR, FD cleared, FPUL, FPSCR, both FP banks
	nop

	mov.l	ie_tick_k,r0
	jsr	@r0
	nop

	mov.l	ie_pop_k,r0
	jsr	@r0		! SR (and so FD) back as the title had it
	nop
	ldc.l	@r15+,gbr
	lds.l	@r15+,pr
	lds.l	@r15+,macl
	lds.l	@r15+,mach
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	mov.l	@r15+,r7
	mov.l	@r15+,r6
	mov.l	@r15+,r5
	mov.l	@r15+,r4
	mov.l	@r15+,r3
	mov.l	@r15+,r2
	mov.l	ie_swallow_k,r0
	mov.l	@r0,r1
	tst	r1,r1
	bt	irq_out
	! Ours alone (the BBA's RX interrupt, irq.c): return from it here. The
	! title's handler would find nothing of its own pending.
	mov	#0,r1
	mov.l	r1,@r0
	mov.l	@r15+,r1
	mov.l	@r15,r0
	stc	sgr,r15
	rte
	nop

irq_out:
	mov.l	ie_nop_k,r0
	mov.l	@r0,r0
	tst	r0,r0
	bt	ce_out
	! The three replaced words were nops (Katana): give back r0, r1 and r15
	! untouched and resume at VBR+0x606. Only r0 and r1 are free, so the
	! title's r0 is parked just under its own SP -- where the entry's next
	! instruction, mov.l r0,@-r15, puts it again -- and popped in the delay
	! slot. With the MMU off that address needs no TLB.
	stc	sgr,r1
	mov.l	@(4,r15),r0	! the title's r0 (the trampoline pushed it)
	mov.l	r0,@-r1
	mov.l	r1,@(4,r15)	! its SP - 4, in that slot
	stc	vbr,r0
	mov.w	ie_606,r1
	add	r1,r0
	mov.l	@r15+,r1	! the title's r1
	mov.l	@r15,r15
	jmp	@r0
	mov.l	@r15+,r0

ce_out:
	! r0, r1 and r6 are about to receive what CE's three replaced
	! instructions put there, so their saved copies are dropped; nothing
	! else is touched. The jump target is read before the delay slot runs.
	stc	vbr,r1
	mov.w	ie_68c,r0
	mov.l	@(r0,r1),r0	! r0 = *(VBR+0x68c)
	mov.w	ie_606,r6
	add	r6,r1		! r1 = VBR+0x606
	mov.l	@(40,r7),r6	! r6 = @(40,r7)
	stc	sgr,r15		! the interrupted r15
	jmp	@r1
	mov	r6,r1		! r1 = r6

.align 1
ie_68c:
	.word	0x068c
ie_606:
	.word	0x0606
.align 2
ie_entries_k:
	.long	_g_irq_entries
ie_nop_k:
	.long	_g_irq_nop_entry
ie_swallow_k:
	.long	_g_irq_swallow
ie_work_k:
	.long	_g_irq_work
ie_push_k:
	.long	_fpu_push
ie_pop_k:
	.long	_fpu_pop
ie_tick_k:
	.long	_irq_tick

!
! void irq_icache_flush(void) -- invalidate the whole instruction cache
! (CCR.ICI). CCR may only be written from P2, and eight instructions must
! pass before a cached fetch. The caller has written the new code back to
! memory already (ocbwb).
!
.align 2
_irq_icache_flush:
	mov.l	ic_body_k,r0
	mov.l	ic_p2_k,r1
	or	r1,r0
	jmp	@r0
	nop
ic_body:
	mov.l	ic_ccr_k,r1
	mov.l	@r1,r0
	mov.w	ic_ici,r2
	or	r2,r0
	mov.l	r0,@r1
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	rts
	nop
.align 1
ic_ici:
	.word	0x0800
.align 2
ic_body_k:
	.long	ic_body
ic_p2_k:
	.long	0xa0000000
ic_ccr_k:
	.long	0xff00001c
