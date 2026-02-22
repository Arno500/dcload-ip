	.extern _gdGdcReqCmd
	.extern _gdGdcGetCmdStat
	.extern _gdGdcExecServer
	.extern _gdGdcInitSystem
	.extern _gdGdcGetDrvStat
	.extern _gdGdcG1DmaEnd
	.extern _gdGdcReqDmaTrans
	.extern _gdGdcCheckDmaTrans
	.extern _gdGdcReadAbort
	.extern _gdGdcChangeDataType
	.extern _gdGdcReset

	.section .text
	.global _cdfs_redir_enable
	.global _cdfs_redir_disable
	.global _cdfs_redir_save
.align 2

_cdfs_redir_save:
	mov.l cdfs_saved_k, r0
	mov.l @r0, r0
	tst r0,r0
	bf already_saved
	mov.l cdfs_entry_k, r0
	mov.l @r0,r0
	mov.l cdfs_saved_k, r1
	mov.l r0, @r1
already_saved:
	rts
	nop

_cdfs_redir_disable:
	mov.l cdfs_saved_k, r0
	mov.l @r0, r0
	mov.l cdfs_entry_k, r1
	mov.l r0, @r1
	rts
	nop

_cdfs_redir_enable:
	mov.l cdfs_entry_k, r0
	mov.l cdfs_redir_k, r1
	mov.l r1, @r0
	rts
	nop

.align 2
cdfs_entry_k:
	.long 0xac0000bc
cdfs_saved_k:
	.long cdfs_saved
cdfs_saved:
	.long 0
cdfs_redir_k:
	.long cdfs_redir

cdfs_redir:
	mov r7,r0
	! allow syscall IDs 0..10 (11 entries in gd_first_k table)
	mov #11,r1
	cmp/hs r0,r1
	bf badsyscall
	mov.l gd_first_k,r1
	shll2 r0
	mov.l @(r0,r1),r0
	jmp @r0
	nop
badsyscall:
	mov #-1,r0
	rts
	nop

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
