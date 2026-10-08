        #
        # interrupts and exceptions while in supervisor
        # mode come here.
        #
        # the current stack is a kernel stack.
        # push registers, call kerneltrap().
        # when kerneltrap() returns, restore registers, return.
        #
.globl kerneltrap
.globl kernelvec
.align 4
kernelvec:
        # make room to save registers.
        addi sp, sp, -256

        # save caller-saved registers.
        sd ra, 0(sp)
        # sd sp, 8(sp)
        sd gp, 16(sp)
        # sd tp, 24(sp)
        sd t0, 32(sp)
        sd t1, 40(sp)
        sd t2, 48(sp)
        sd a0, 72(sp)
        sd a1, 80(sp)
        sd a2, 88(sp)
        sd a3, 96(sp)
        sd a4, 104(sp)
        sd a5, 112(sp)
        sd a6, 120(sp)
        sd a7, 128(sp)
        sd t3, 216(sp)
        sd t4, 224(sp)
        sd t5, 232(sp)
        sd t6, 240(sp)

        # call the C trap handler in trap.c
        call kerneltrap

        # restore registers.
        ld ra, 0(sp)
        # ld sp, 8(sp)
        ld gp, 16(sp)
        # not tp (contains hartid), in case we moved CPUs
        ld t0, 32(sp)
        ld t1, 40(sp)
        ld t2, 48(sp)
        ld a0, 72(sp)
        ld a1, 80(sp)
        ld a2, 88(sp)
        ld a3, 96(sp)
        ld a4, 104(sp)
        ld a5, 112(sp)
        ld a6, 120(sp)
        ld a7, 128(sp)
        ld t3, 216(sp)
        ld t4, 224(sp)
        ld t5, 232(sp)
        ld t6, 240(sp)

        addi sp, sp, 256

        # return to whatever we were doing in the kernel.
        sret

.globl machinevec
machinevec:

        # mepc 4 증가
        csrr t0, mepc
        addi t0, t0, 4
        csrw mepc, t0

        li t0, 24
        beq a7, t0, handle_getpmpaddr
        li t0, 25
        beq a7, t0, handle_getpmpcfg
        li a0, -1
        mret

handle_getpmpaddr:

        li t0, 0
        beq a0, t0, read_pmpaddr0
        li t0, 1
        beq a0, t0, read_pmpaddr1
        li t0, 2
        beq a0, t0, read_pmpaddr2
        li t0, 3
        beq a0, t0, read_pmpaddr3
        li a0, -1
        mret

read_pmpaddr0:
        csrr a0, pmpaddr0
        j process_pmpaddr

read_pmpaddr1:
        csrr a0, pmpaddr1
        j process_pmpaddr

read_pmpaddr2:
        csrr a0, pmpaddr2
        j process_pmpaddr

read_pmpaddr3:
        csrr a0, pmpaddr3
        j process_pmpaddr

process_pmpaddr:
        slli a0, a0, 2
        li  t0, 0x00FFFFFFFFFFFFFF
        and a0, a0, t0
        mret

handle_getpmpcfg:
        li t0, 0
        beq a0, t0, read_pmpcfg0
        li t0, 1
        beq a0, t0, read_pmpcfg1
        li t0, 2
        beq a0, t0, read_pmpcfg2
        li t0, 3
        beq a0, t0, read_pmpcfg3
        li a0, -1
        mret

read_pmpcfg0:
        csrr a0, pmpcfg0
        li  t0, 0x00000000000000FF
        and a0, a0, t0
        mret

read_pmpcfg1:
        csrr a0, pmpcfg0
        li  t0, 0x000000000000FF00
        and a0, a0, t0
        srli a0, a0, 8
        mret

read_pmpcfg2:
        csrr a0, pmpcfg0
        li  t0, 0x0000000000FF0000
        and a0, a0, t0
        srli a0, a0, 16
        mret

read_pmpcfg3:
        csrr a0, pmpcfg0
        li  t0, 0x00000000FF000000
        and a0, a0, t0
        srli a0, a0, 24
        mret


.globl mcall_getpmpaddr
mcall_getpmpaddr:

        li a7, 24
        ecall
        ret

.globl mcall_getpmpcfg
mcall_getpmpcfg:

        li a7, 25
        ecall
        ret