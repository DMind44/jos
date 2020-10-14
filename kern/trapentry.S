/* See COPYRIGHT for copyright information. */

#include <inc/mmu.h>
#include <inc/memlayout.h>
#include <inc/trap.h>



###################################################################
# exceptions/interrupts
###################################################################

/* TRAPHANDLER defines a globally-visible function for handling a trap.
 * It pushes a trap number onto the stack, then jumps to _alltraps.
 * Use TRAPHANDLER for traps where the CPU automatically pushes an error code.
 *
 * You shouldn't call a TRAPHANDLER function from C, but you may
 * need to _declare_ one in C (for instance, to get a function pointer
 * during IDT setup).  You can declare the function with
 *   void NAME();
 * where NAME is the argument passed to TRAPHANDLER.
 */
#define TRAPHANDLER(name, num)						\
	.globl name;		/* define global symbol for 'name' */	\
	.type name, @function;	/* symbol type is function */		\
	.align 2;		/* align function definition */		\
	name:			/* function starts here */		\
	pushl $(num);							\
	jmp _alltraps

/* Use TRAPHANDLER_NOEC for traps where the CPU doesn't push an error code.
 * It pushes a 0 in place of the error code, so the trap frame has the same
 * format in either case.
 */
#define TRAPHANDLER_NOEC(name, num)					\
	.globl name;							\
	.type name, @function;						\
	.align 2;							\
	name:								\
	pushl $0;							\
	pushl $(num);							\
	jmp _alltraps

.text
	TRAPHANDLER_NOEC(DIVIDE_ERROR, T_DIVIDE)
	TRAPHANDLER_NOEC(DEBUG, T_DEBUG)
	TRAPHANDLER_NOEC(NON_MASKABLE_INTERRUPT, T_NMI)
	TRAPHANDLER_NOEC(BREAKPOINT, T_BRKPT)
	TRAPHANDLER_NOEC(OVERFLOW, T_OFLOW)
	TRAPHANDLER_NOEC(BOUND_RANGE_EXCEEDED, T_BOUND)
	TRAPHANDLER_NOEC(INVALID_OPCODE, T_ILLOP)
	TRAPHANDLER_NOEC(DEVICE_NOT_AVAILABLE, T_DEVICE)
	TRAPHANDLER(DOUBLE_FAULT, T_DBLFLT)
	TRAPHANDLER(INVALID_TSS, T_TSS)
	TRAPHANDLER(SEGMENT_NOT_PRESENT, T_SEGNP)
	TRAPHANDLER(STACK_FAULT, T_STACK)
	TRAPHANDLER(GENERAL_PROTECTION, T_GPFLT)
	TRAPHANDLER(PAGE_FAULT, T_PGFLT)
	TRAPHANDLER_NOEC(X87_FPU_FLOATINGPOINT_ERROR, T_FPERR)
	TRAPHANDLER(ALIGNMENT_CHECK, T_ALIGN)
	TRAPHANDLER_NOEC(MACHINE_CHECK, T_MCHK)
	TRAPHANDLER_NOEC(SIMD_FLOATING_POINT_EXCEPTION, T_SIMDERR)

	//push values to make the stack look like a struct Trapframe
_alltraps:
	# Build trap frame.
	pushl %ds		
	pushl %es		
	pushal
	# Set up data segment
	movw $(GD_KD), %ecx	
	movw %ecx, %ds		
	movw %ecx, %es		
	# Call trap(tf), where tf=%esp
	pushl %esp		
	call trap		
