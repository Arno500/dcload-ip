!
! GD-ROM syscall redirection for dcload-ip.
!
! WHAT CHANGED AND WHY (read this before touching anything below).
!
! The Dreamcast BIOS GD driver is not a library of functions, it is a SERVER
! TASK. A title calls gdGdcInitSystem() exactly once; that call never returns
! in the ordinary sense. The driver parks the caller's context and then runs
! its own endless dispatch loop. Two other syscalls move between the two
! contexts:
!
!   gdGdcExecServer()  - the game says "run for a while". Resumes the server
!                        exactly where it last yielded.
!   gdcExitToGame()    - the server says "I have done what I can for now".
!                        Returns to whoever last called ExecServer.
!
! That is a coroutine, and it is the whole reason a real Dreamcast can service
! a multi-megabyte read while the title keeps rendering: the driver makes
! progress in slices, and between slices the game owns the CPU again.
!
! dcload used to have none of this. gdGdcExecServer returned 0 and did
! nothing, and the entire read happened inside gdGdcReqCmd while the title was
! blocked. Sonic Adventure calls ExecServer about 45000 times per session
! against 119 ReqCmd, so it is built around the server model; handing it a
! driver that only works inside ReqCmd is a different machine from the one it
! was written for.
!
! ONE STACK, NOT TWO. The inactive side's frame is parked in saved_regs[]
! rather than living on a second stack. That is deliberate: it keeps the
! network path (bb->loop and every packet handler it reaches) running on the
! game's stack, exactly where it runs today and where it is known to fit. A
! private loader stack would have had to be sized for that whole excursion.
! The copy therefore only ever covers the depth AT THE YIELD POINT -- server
! main loop plus data_transfer, a handful of longs -- never the deep network
! excursions, which always complete before a yield.
!
! Architecture follows DreamShell's ISO loader (loader/gdc_syscall.s by SWAT);
! the code here is dcload's own.
!
! HAZARDS, each one paid for once:
!   - The park buffer is written DOWNWARD from saved_regs_end and read back
!     UPWARD. Get the direction wrong and the stack image comes back mirrored.
!   - Every PC-relative pool (mov.l @(disp,PC)) reaches at most 1020 bytes
!     FORWARD and must be 4-byte aligned. Each routine below carries its own
!     pool immediately after it. Do not hoist them into one block at the end.
!   - T is live across the register pushes in both swap routines: it is set
!     before them and consumed after. mov.l / sts.l / lds.l do not touch T.
!     Do not insert anything that does.
!   - The park buffer lives in .text, which is RAM here, and is initialised by
!     the linker. It must NOT be moved to .bss without adding a runtime init.
!   - THE PARKED FRAME IS RELOCATABLE. It is restored onto whatever r15 the
!     next ExecServer arrives with, which need not be the address it was
!     copied from. So no function live across a yield may take the address of
!     one of its own locals, or keep a frame pointer: such a pointer would
!     still aim at the old location. Nothing on the server path does this
!     today (gdcServerMain / data_transfer / data_transfer_emu_async have no
!     address-taken locals) and nothing added to it may. Functions that run to
!     completion between yields -- bb->loop() and the whole network path --
!     are unaffected, because their frames are never parked.
!

	.extern _gdGdcReqCmd
	.extern _gdGdcGetCmdStat
	.extern _gdGdcGetDrvStat
	.extern _gdGdcG1DmaEnd
	.extern _gdGdcReqDmaTrans
	.extern _gdGdcCheckDmaTrans
	.extern _gdGdcReadAbort
	.extern _gdGdcReset
	.extern _gdGdcChangeDataType
	.extern _gdGdcSetPioCallback
	.extern _gdGdcReqPioTrans
	.extern _gdGdcCheckPioTrans
	.extern _gdGdcChangeDisc
	.extern _gdGdcCartRead
	.extern _gdGdcDummy
	.extern _gdcServerMain

	.section .text
	.global _cdfs_redir_enable
	.global _cdfs_redir_disable
	.global _cdfs_redir_save
	.global _gdcExitToGame
	.global _gd_lock
	.global _gd_unlock
	.global _gdGdcExecServer
	.global _gdGdcInitSystem

! Three words the C side needs to READ, and nothing else. gd_lock_byte says
! whether the GD path is held; saved_regs_ptr against saved_regs_end says
! whether the server context is PARKED (ptr below end, a frame is stored) or
! RUNNING (ptr == end, the buffer was emptied by es_enter). Held while parked
! is the one combination that cannot persist: gdcExitToGame parks and then
! releases, so a parked server never leaves the lock behind. See
! gd_lock_watchdog() in cdfs_syscalls.c.
	.global _gd_lock_state
	.set _gd_lock_state, gd_lock_byte
	.global _gd_park_ptr
	.set _gd_park_ptr, saved_regs_ptr
	.global _gd_park_end
	.set _gd_park_end, saved_regs_end
.align 2

! TWO VECTORS, not one. The BIOS publishes the GD driver at 0xac0000bc (with
! the misc group in front, r6 == -1) and at 0xac0000c0 (the driver alone), and
! isoldr takes both (gdc_syscall.s). Every Katana title measured calls only
! 0xbc, so this changes nothing for them; Windows CE's COREDLL names 0xc0.
! The pair is saved, restored and armed together: the 0xc0 word and its saved
! copy sit four bytes after their 0xbc counterparts, hence the @(4,rN).
!
! There is a THIRD door that no vector covers: the driver body itself, which
! the vectors point at (0x8c0010f0). Windows CE's GD driver calls it directly,
! 23 times in Sega Rally 2's 0WINCEOS.BIN, with r6 = 0 and r7 = the index.
! The host rewrites those literals to _gd_bios_entry (dispatch.rs,
! gd_body_patches), as isoldr's gdc_syscall_patch() does -- from the host,
! because overwriting the BIOS's own copy of the driver here would take away
! the real driver gd_spin_down_drive() needs at the next boot.
_cdfs_redir_save:
	mov.l cdfs_saved_k, r1
	mov.l @r1, r0
	tst r0,r0
	bf already_saved
	mov.l cdfs_entry_k, r2
	mov.l @r2,r0
	mov.l r0, @r1
	mov.l @(4,r2),r0
	mov.l r0, @(4,r1)
already_saved:
	rts
	nop

_cdfs_redir_disable:
	mov.l cdfs_saved_k, r1
	mov.l cdfs_entry_k, r2
	mov.l @r1, r0
	mov.l r0, @r2
	mov.l @(4,r1), r0
	rts
	mov.l r0, @(4,r2)

_cdfs_redir_enable:
	mov.l cdfs_entry_k, r0
	mov.l cdfs_redir_k, r1
	mov.l r1, @r0
	mov.l cdfs_redir_c0_k, r1
	rts
	mov.l r1, @(4,r0)

.align 2
cdfs_entry_k:
	.long 0xac0000bc
cdfs_saved_k:
	.long cdfs_saved
cdfs_saved:
	.long 0		! 0xac0000bc as the BIOS left it
	.long 0		! 0xac0000c0 as the BIOS left it
cdfs_redir_k:
	.long cdfs_redir
cdfs_redir_c0_k:
	.long cdfs_redir_c0

! int gd_lock(void) -- returns 0 when acquired, 1 when already held.
! TAS.B sets T when the byte WAS zero, i.e. when we just took it.
_gd_lock:
	mov.l gd_lock_k, r0
	tas.b @r0
	bt gd_lock_taken
	rts
	mov #1, r0
gd_lock_taken:
	rts
	mov #0, r0

! void gd_unlock(void)
_gd_unlock:
	mov.l gd_lock_k, r0
	mov #0, r1
	rts
	mov.b r1, @r0

.align 2
gd_lock_k:
	.long gd_lock_byte

!
! int gdGdcExecServer(void)
!
! Resume the parked server context. If the server is already running (the
! game re-entered us from an interrupt, or called ExecServer from inside a
! callback) we decline and let it poll again -- exactly what the BIOS does.
!
_gdGdcExecServer:
	mov.l es_cnt_k, r0
	mov.l @(8,r0), r1
	add #1, r1
	mov.l r1, @(8,r0)
	mov.l es_live_k, r0
	mov.l @r0, r0
	tst r0, r0
	bf es_resume
	mov.l es_init_k, r0		! never initialised -> become the server now
	jmp @r0
	nop
es_resume:
	mov.l es_lock_k, r0
	tas.b @r0
	bt es_enter
	mov.l es_reent_k, r0		! server already running: decline, game polls
	mov.l @r0, r1
	add #1, r1
	mov.l r1, @r0
	rts
	mov #0, r0
es_enter:
	sts.l pr, @-r15
	sts.l mach, @-r15
	sts.l macl, @-r15
	mov.l r14, @-r15
	mov.l r13, @-r15
	mov.l r12, @-r15
	mov.l r11, @-r15
	mov.l r10, @-r15
	mov.l r9, @-r15
	mov.l r8, @-r15
	mov.l es_stack_k, r0
	mov.l r15, @r0			! park the game side here
	mov.l es_regs_k, r0
	mov.l @r0, r0			! r0 = saved_regs_ptr
	mov.l @r0+, r2			! r2 = parked stack depth, in longs
	tst r2, r2
	bt es_pop_regs
es_copy_stack:
	mov.l @r0+, r3
	dt r2
	mov.l r3, @-r15
	bf es_copy_stack
es_pop_regs:
	mov.l @r0+, r8
	mov.l @r0+, r9
	mov.l @r0+, r10
	mov.l @r0+, r11
	mov.l @r0+, r12
	mov.l @r0+, r13
	mov.l @r0+, r14
	lds.l @r0+, macl
	lds.l @r0+, mach
	lds.l @r0+, pr
	mov r0, r2
	mov.l es_regs_k, r0
	rts
	mov.l r2, @r0			! buffer is empty again

.align 2
es_lock_k:
	.long gd_lock_byte
es_stack_k:
	.long saved_stack
es_regs_k:
	.long saved_regs_ptr
es_cnt_k:
	.long _g_gd_idx_counts
es_reent_k:
	.long _g_cdfs_sync_reentered
es_live_k:
	.long gd_server_live
es_init_k:
	.long _gdGdcInitSystem

!
! void gdcExitToGame(void)
!
! Park the server context and return to the last caller of ExecServer (or of
! InitSystem, the first time round).
!
_gdcExitToGame:
	mov.l eg_stack_k, r0
	mov.l @r0, r2
	sub r15, r2			! r2 = saved_stack - r15, in bytes
	shlr2 r2			! -> longs
	mov r2, r1			! r1 keeps the count for the trailer
	mov.l eg_regs_k, r0
	mov.l @r0, r0			! r0 = saved_regs_ptr (top of buffer)
	sts.l pr, @-r0
	sts.l mach, @-r0
	sts.l macl, @-r0
	mov.l r14, @-r0
	mov.l r13, @-r0
	mov.l r12, @-r0
	mov.l r11, @-r0
	mov.l r10, @-r0
	mov.l r9, @-r0
	mov.l r8, @-r0
	tst r2, r2
	bt eg_park_count
eg_copy_stack:
	mov.l @r15+, r3
	dt r2
	mov.l r3, @-r0
	bf eg_copy_stack
eg_park_count:
	mov.l r1, @-r0
	mov.l eg_depth_k, r3		! publish the depth: a silent overrun of
	mov.l r1, @r3			! saved_regs[] would corrupt whatever follows
	mov r0, r2
	mov.l eg_regs_k, r0
	mov.l r2, @r0
	mov.l @r15+, r8			! r15 is back at saved_stack here
	mov.l @r15+, r9
	mov.l @r15+, r10
	mov.l @r15+, r11
	mov.l @r15+, r12
	mov.l @r15+, r13
	mov.l @r15+, r14
	lds.l @r15+, macl
	lds.l @r15+, mach
	lds.l @r15+, pr
	mov.l eg_lock_k, r0
	mov #0, r2
	mov.b r2, @r0			! release, and we are the game again
	rts
	mov #0, r0			! ExecServer/InitSystem return 0 to the title

.align 2
eg_stack_k:
	.long saved_stack
eg_regs_k:
	.long saved_regs_ptr
eg_lock_k:
	.long gd_lock_byte
eg_depth_k:
	.long _g_gd_park_longs

!
! int gdGdcInitSystem(void)
!
! Become the server. This does not return to the caller from here -- the
! first gdcExitToGame() inside gdcServerMain() is what returns.
!
_gdGdcInitSystem:
	mov.l is_cnt_k, r0
	mov.l @(12,r0), r1
	add #1, r1
	mov.l r1, @(12,r0)
	mov.l is_live_k, r0
	mov #1, r2
	mov.l r2, @r0			! the server exists from now on
	mov.l is_lock_k, r0
	mov #1, r2
	mov.b r2, @r0			! and it is running right now
	sts.l pr, @-r15
	sts.l mach, @-r15
	sts.l macl, @-r15
	mov.l r14, @-r15
	mov.l r13, @-r15
	mov.l r12, @-r15
	mov.l r11, @-r15
	mov.l r10, @-r15
	mov.l r9, @-r15
	mov.l r8, @-r15
	mov.l is_stack_k, r0
	mov.l r15, @r0
	mov.l is_regs_end_k, r2		! re-arm the park buffer
	mov.l is_regs_k, r0
	mov.l r2, @r0
	mov.l is_main_k, r1
	jmp @r1
	nop

.align 2
is_lock_k:
	.long gd_lock_byte
is_stack_k:
	.long saved_stack
is_regs_k:
	.long saved_regs_ptr
is_regs_end_k:
	.long saved_regs_end
is_main_k:
	.long _gdcServerMain
is_cnt_k:
	.long _g_gd_idx_counts
is_live_k:
	.long gd_server_live

!
! Syscall entry. r7 selects the function, r6 == -1 marks the non-GD ("misc")
! calls that share this vector and that we must not claim.
!
	.global _gd_bios_entry
_gd_bios_entry:
cdfs_redir:
	mov #-1, r1
	cmp/eq r6, r1
	bt/s misc_syscall
	mov #0, r6
cdfs_redir_c0:
	mov r7,r0
	mov #18,r1			! 18 entries in gd_first_k below
	cmp/hi r0,r1			! T = (18 > r7); cmp/hs here would admit 18
	bf badsyscall
	mov.l gd_first_k,r1
	shll2 r0
	mov.l @(r0,r1),r0
	jmp @r0
	nop
badsyscall:
	mov #-1,r6
misc_syscall:
	rts
	mov r6,r0

.align 2
gd_first_k:
	.long gdGdcReqCmd
gdGdcReqCmd:
	.long _gdGdcReqCmd
gdGdcGetCmdStat:
	.long _gdGdcGetCmdStat
gdGdcExecServer:
	.long _gdGdcExecServer
gdGdcInitSystem:
	.long _gdGdcInitSystem
gdGdcGetDrvStat:
	.long _gdGdcGetDrvStat
gdGdcG1DmaEnd:
	.long _gdGdcG1DmaEnd
gdGdcReqDmaTrans:
	.long _gdGdcReqDmaTrans
gdGdcCheckDmaTrans:
	.long _gdGdcCheckDmaTrans
gdGdcReadAbort:
	.long _gdGdcReadAbort
gdGdcReset:
	.long _gdGdcReset
gdGdcChangeDataType:
	.long _gdGdcChangeDataType
gdGdcSetPioCallback:
	.long _gdGdcSetPioCallback
gdGdcReqPioTrans:
	.long _gdGdcReqPioTrans
gdGdcCheckPioTrans:
	.long _gdGdcCheckPioTrans
gdGdcUnk1:
	.long _gdGdcDummy
gdGdcChangeDisc:
	.long _gdGdcChangeDisc
gdGdcCartRead:
	.long _gdGdcCartRead
gdGdcUnk4:
	.long _gdGdcDummy

!
! void gd_on_loader_stack(void (*fn)(void))
!
! Call fn on a stack of the loader's own (the Maple DMA page, P1), with the FPU
! usable, and come back to the caller's stack.
! A Windows CE title calls the GD driver on a thread stack at a VIRTUAL address:
! every push there goes through the TLB, and a miss or an uncommitted stack
! page enters CE's kernel -- which re-enables interrupts and may switch threads
! -- whatever IMASK the loader set. Caught under flycast (2026-09-27): the GD
! thread left in the middle of a network exchange with IMASK 15, and the ring
! overflowed while it was away (docs/wince-investigation.md 7q). P1 has no TLB.
!
! THE FPU IS THE OTHER WAY INTO CE's KERNEL (2026-09-27, 9b). CE switches the
! FPU lazily: its threads run with SR.FD = 1 until they use it, and the first
! FPU instruction raises an exception its general handler takes -- on the
! CURRENT stack, this one, with IMASK lowered to 0. The exchange has FPU
! instructions (SH4_aligned_memcpy's fmov.d, and GCC spills to FP registers),
! so every exchange under CE entered the kernel and could be preempted there.
! Found when the interrupt hook, which used the same stack top, overwrote such
! a preempted exchange. So fn runs with FD clear and the FP registers -- the
! hardware state of whichever thread owns them -- saved and put back
! (fpu_push/fpu_pop below), and nothing in it enters CE.
!
! The CALLER masks interrupts first: this is one stack for every thread, so
! nothing may be switched in while it is in use. fn keeps r8-r15 (C ABI).
!
! Why the Maple page and not _stack: the loader's own stack region, above
! _end, holds the big GD stage while a CE title runs (dcload.x.in, .gdstage).
! The page is 4 KB in both layout families and only a MAPL command uses its
! first 2 KB -- never while a title runs. The interrupt hook's stack is the
! page's first KB (irq.c), below this one.
!
.globl _gd_on_loader_stack
.align 2
_gd_on_loader_stack:
	mov.l	r14,@-r15
	mov.l	r8,@-r15
	sts.l	pr,@-r15
	mov	r15,r14
	mov	r4,r8
	mov.l	ols_stack_k,r15
	add	#-4,r15		! fpu_push wants r15 = 4 mod 8
	mov.l	ols_push_k,r0
	jsr	@r0
	nop
	jsr	@r8
	nop
	mov.l	ols_pop_k,r0
	jsr	@r0
	nop
	mov	r14,r15
	lds.l	@r15+,pr
	mov.l	@r15+,r8
	rts
	mov.l	@r15+,r14
.align 2
ols_stack_k:
	.long	_maple_dma_buffer + 0x1000
ols_push_k:
	.long	_fpu_push
ols_pop_k:
	.long	_fpu_pop

!
! fpu_push / fpu_pop -- from assembly only (irq_hook.S, gd_on_loader_stack).
!
! fpu_push saves SR, clears SR.FD, then pushes FPUL, FPSCR and both FP banks
! (0x8c bytes in all) and leaves FPSCR as the loader's C expects it. fpu_pop
! undoes it, SR last, so FD -- and everything else in SR -- is back as it was.
! Both clobber r0, r1 and PR. The pair stores need r15 8-aligned after the
! three longs, i.e. r15 = 4 mod 8 on entry to fpu_push: an address error there
! runs under whatever the caller masked, and in the interrupt hook that is a
! reset. The register values saved are not ours: under CE they are the last
! FPU user's, whoever that is, and they go back untouched.
!
.globl _fpu_push
.globl _fpu_pop
.align 2
_fpu_push:
	stc	sr,r0
	mov.l	r0,@-r15
	mov.l	fp_fd_clear_k,r1
	and	r1,r0
	ldc	r0,sr
	sts.l	fpul,@-r15
	sts.l	fpscr,@-r15
	mov.l	fp_pair_k,r0
	lds	r0,fpscr	! SZ = 1: fmov moves register pairs
	fmov	dr0,@-r15
	fmov	dr2,@-r15
	fmov	dr4,@-r15
	fmov	dr6,@-r15
	fmov	dr8,@-r15
	fmov	dr10,@-r15
	fmov	dr12,@-r15
	fmov	dr14,@-r15
	fmov	xd0,@-r15
	fmov	xd2,@-r15
	fmov	xd4,@-r15
	fmov	xd6,@-r15
	fmov	xd8,@-r15
	fmov	xd10,@-r15
	fmov	xd12,@-r15
	fmov	xd14,@-r15
	mov.l	fp_c_k,r0
	lds	r0,fpscr	! what go.S hands a program, and what C here assumes
	rts
	nop

_fpu_pop:
	mov.l	fp_pair_k,r0
	lds	r0,fpscr
	fmov	@r15+,xd14
	fmov	@r15+,xd12
	fmov	@r15+,xd10
	fmov	@r15+,xd8
	fmov	@r15+,xd6
	fmov	@r15+,xd4
	fmov	@r15+,xd2
	fmov	@r15+,xd0
	fmov	@r15+,dr14
	fmov	@r15+,dr12
	fmov	@r15+,dr10
	fmov	@r15+,dr8
	fmov	@r15+,dr6
	fmov	@r15+,dr4
	fmov	@r15+,dr2
	fmov	@r15+,dr0
	lds.l	@r15+,fpscr
	lds.l	@r15+,fpul
	mov.l	@r15+,r0
	ldc	r0,sr
	rts
	nop
.align 2
fp_fd_clear_k:
	.long	0xffff7fff
fp_pair_k:
	.long	0x00100000
fp_c_k:
	.long	0x00040000

!
! Coroutine state. Lives in .text (RAM) so the linker initialises it; moving
! it to .bss would need an explicit runtime init of saved_regs_ptr.
!
.align 2
saved_stack:
	.long 0
saved_regs_ptr:
	.long saved_regs_end
saved_regs:
	.long 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	.long 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	.long 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	.long 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	.long 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	.long 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
saved_regs_end:
	.long 0
gd_lock_byte:
	.long 0
gd_server_live:
	.long 0
