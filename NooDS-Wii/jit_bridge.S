/*
    Copyright (C) 2026 radicalten

    This file is part of NooDS-Wii.

    NooDS-Wii is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS-Wii is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS-Wii. If not, see <https://www.gnu.org/licenses/>.
*/

/*
 * jit_bridge.S — Broadway / PowerPC 750CL EABI assembly bridge for NooDS-Wii JIT
 *
 * Signature:
 *   extern "C" int jit_enter(void *code, Interpreter *cpu);
 *   extern "C" void jit_exit(void);
 *
 * Calling Convention & Register Contract (PowerPC EABI / SysV):
 *   Entry arguments:
 *     r3 = void *code         (Target compiled PPC stub/block in MEM2 or MEM1)
 *     r4 = Interpreter *cpu   (Pointer to active guest CPU: ARM9 or ARM7)
 *
 *   Stack frame layout (32 bytes, 16-byte aligned):
 *     0(r1)  : Backchain pointer to caller's r1
 *     4(r1)  : Reserved for callee LR save
 *     8(r1)  : Padding / alignment
 *     12(r1) : Padding / alignment
 *     16(r1) : Saved caller r28 (callee-saved scratch)
 *     20(r1) : Saved caller r29 (callee-saved scratch)
 *     24(r1) : Saved caller r30 (callee-saved accumulated block cycle counter)
 *     28(r1) : Saved caller r31 (callee-saved dedicated Interpreter* pointer)
 *     36(r1) : Saved caller LR (at +4 of caller's stack frame per PowerPC EABI)
 *
 *   Register usage inside generated PPC code:
 *     r1      : Stack pointer (preserved)
 *     r2, r13 : System/SDA anchors (strictly reserved, never touched)
 *     r0      : Scratch for SPR moves; NEVER used as RA base in D-form instructions
 *               unless literal 0 is intended (li/lis)
 *     r3..r10 : Volatile scratch / C helper arguments & return value
 *     r11..r12: Volatile call target / address materialization scratch
 *     r28..r29: Callee-saved JIT scratch registers
 *     r30     : Callee-saved cycle accumulator across multi-instruction blocks
 *     r31     : Callee-saved Interpreter* pointer
 *
 *   Return value:
 *     r3 = int (exact guest cycles executed by the stub or basic block)
 */

    .section .text
    .align 4
    .globl jit_enter
    .type jit_enter, @function
jit_enter:
    stwu    1, -32(1)
    mflr    0
    stw     0, 36(1)
    stmw    28, 16(1)
    mr      31, 4
    mr      30, 5
    mtctr   3
    bctr
    .size jit_enter, .-jit_enter

    .align 4
    .globl jit_exit
    .type jit_exit, @function
jit_exit:
    lmw     28, 16(1)
    lwz     0, 36(1)
    mtlr    0
    addi    1, 1, 32
    blr
    .size jit_exit, .-jit_exit
