/* Test kernel for the arm64 xnu loader judge. Checks the entry contract
 * GRUB promises and reports each item on the PL011 at QEMU virt's
 * 0x09000000: x0 points at a v2 boot-parameter block whose
 * efi_system_table (at i386's offset) is a real EFI system table, the MMU
 * is off, sp is set. Position-independent, no stack use; the one literal pool entry is a
 * constant, not an address. */
	.globl _start
_start:
	mov	x19, x0
	mov	x20, sp
	mov	x1, #0x09000000
	adr	x2, m_enter
	bl	puts

	cbz	x19, 1f
	ldrh	w3, [x19, #2]		/* vermajor */
	cmp	w3, #2
	b.ne	1f
	adr	x2, m_bp
	bl	puts
	ldr	w4, [x19, #1104]	/* efi_system_table, i386 offset */
	cbz	w4, 1f
	ldr	x5, [x4]		/* EFI_SYSTEM_TABLE signature */
	ldr	x6, =0x5453595320494249	/* "IBI SYST" */
	cmp	x5, x6
	b.ne	1f
	adr	x2, m_st
	bl	puts
1:	mrs	x3, CurrentEL
	cmp	x3, #(2 << 2)
	b.eq	2f
	mrs	x3, sctlr_el1
	b	3f
2:	mrs	x3, sctlr_el2
3:	tbnz	x3, #0, 4f
	adr	x2, m_mmu
	bl	puts
4:	cbz	x20, 5f
	adr	x2, m_sp
	bl	puts
5:	adr	x2, m_done
	bl	puts
6:	wfe
	b	6b

/* x1 = UART, x2 = NUL-terminated string. Clobbers w3. */
puts:	ldrb	w3, [x2], #1
	cbz	w3, 7f
	strb	w3, [x1]
	b	puts
7:	ret

m_enter: .asciz "[xnu-arm64-test] entered\n"
m_bp:	 .asciz "[xnu-arm64-test] x0 = boot params v2\n"
m_st:	 .asciz "[xnu-arm64-test] efi_system_table valid\n"
m_mmu:	 .asciz "[xnu-arm64-test] mmu off\n"
m_sp:	 .asciz "[xnu-arm64-test] sp set\n"
m_done:	 .asciz "[xnu-arm64-test] done\n"
