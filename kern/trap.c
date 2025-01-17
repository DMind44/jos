#include <inc/mmu.h>
#include <inc/x86.h>
#include <inc/assert.h>

#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/env.h>
#include <kern/syscall.h>
#include <kern/sched.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/cpu.h>
#include <kern/spinlock.h>

static struct Taskstate ts;

/* For debugging, so print_trapframe can distinguish between printing
 * a saved trapframe and printing the current trapframe and print some
 * additional information in the latter case.
 */
static struct Trapframe *last_tf;

/* Interrupt descriptor table.  (Must be built at run time because
 * shifted function addresses can't be represented in relocation records.)
 */
struct Gatedesc idt[256] = { { 0 } };
struct Pseudodesc idt_pd = {
	sizeof(idt) - 1, (uint32_t) idt
};


static const char *trapname(int trapno)
{
	static const char * const excnames[] = {
		"Divide error",
		"Debug",
		"Non-Maskable Interrupt",
		"Breakpoint",
		"Overflow",
		"BOUND Range Exceeded",
		"Invalid Opcode",
		"Device Not Available",
		"Double Fault",
		"Coprocessor Segment Overrun",
		"Invalid TSS",
		"Segment Not Present",
		"Stack Fault",
		"General Protection",
		"Page Fault",
		"(unknown trap)",
		"x87 FPU Floating-Point Error",
		"Alignment Check",
		"Machine-Check",
		"SIMD Floating-Point Exception"
	};

	if (trapno < ARRAY_SIZE(excnames))
		return excnames[trapno];
	if (trapno == T_SYSCALL)
		return "System call";
	if (trapno >= IRQ_OFFSET && trapno < IRQ_OFFSET + 16)
		return "Hardware Interrupt";
	return "(unknown trap)";
}


void
trap_init(void)
{
	extern struct Segdesc gdt[];
	
	// initiate IRQs
	void TIMER();
	void KBD();
	void SERIAL();
	void SPURIOUS();
	void IDE();

	void DIVIDE_ERROR();
	void DEBUG();
	void NON_MASKABLE_INTERRUPT();
	void BREAKPOINT ();
	void OVERFLOW ();
	void BOUND_RANGE_EXCEEDED();
	void INVALID_OPCODE ();
	void DEVICE_NOT_AVAILABLE ();
	void DOUBLE_FAULT();
	void INVALID_TSS();
	void SEGMENT_NOT_PRESENT();
	void STACK_FAULT();
	void GENERAL_PROTECTION();
	void PAGE_FAULT();
	void X87_FPU_FLOATINGPOINT_ERROR();
	void ALIGNMENT_CHECK();
	void MACHINE_CHECK();
	void SIMD_FLOATING_POINT_EXCEPTION();
	void SYSTEM_CALL();
	
	SETGATE(idt[0], 0, GD_KT, &DIVIDE_ERROR, 0);
	SETGATE(idt[1], 0, GD_KT, &DEBUG, 3);
	SETGATE(idt[2], 0, GD_KT, &NON_MASKABLE_INTERRUPT, 0);
	SETGATE(idt[3], 0, GD_KT, &BREAKPOINT, 3);
	SETGATE(idt[4], 0, GD_KT, &OVERFLOW, 0);
	SETGATE(idt[5], 0, GD_KT, &BOUND_RANGE_EXCEEDED, 0);
	SETGATE(idt[6], 0, GD_KT, &INVALID_OPCODE, 0);
	SETGATE(idt[7], 0, GD_KT, &DEVICE_NOT_AVAILABLE, 0);
	SETGATE(idt[8], 0, GD_KT, &DOUBLE_FAULT, 0);
	SETGATE(idt[10], 0, GD_KT, &INVALID_TSS, 0);
	SETGATE(idt[11], 0, GD_KT, &SEGMENT_NOT_PRESENT, 0);
	SETGATE(idt[12], 0, GD_KT, &STACK_FAULT, 0);
	SETGATE(idt[13], 0, GD_KT, &GENERAL_PROTECTION, 0);
	SETGATE(idt[14], 0, GD_KT, &PAGE_FAULT, 0); // changed this is_trap from one to zero because using an interrupt gate ensures that interrupts are disabled in kernel mode. Do I need to do this for all the other gates?
	SETGATE(idt[16], 0, GD_KT, &X87_FPU_FLOATINGPOINT_ERROR, 0);
	SETGATE(idt[17], 0, GD_KT, &ALIGNMENT_CHECK, 0);
	SETGATE(idt[18], 0, GD_KT, &MACHINE_CHECK, 0);
	SETGATE(idt[19], 0, GD_KT, &SIMD_FLOATING_POINT_EXCEPTION, 0);
	SETGATE(idt[T_SYSCALL], 0, GD_KT, &SYSTEM_CALL, 3);

	SETGATE(idt[IRQ_OFFSET+IRQ_TIMER], 0, GD_KT, &TIMER, 0);
	SETGATE(idt[IRQ_OFFSET+IRQ_KBD], 0, GD_KT, &KBD, 0);
	SETGATE(idt[IRQ_OFFSET+IRQ_SERIAL], 0, GD_KT, &SERIAL, 0);
	SETGATE(idt[IRQ_OFFSET+IRQ_SPURIOUS], 0, GD_KT, &SPURIOUS, 0);
	SETGATE(idt[IRQ_OFFSET+IRQ_IDE], 0, GD_KT, &IDE, 0);
	// Per-CPU setup 
	trap_init_percpu();
}

// Initialize and load the per-CPU TSS and IDT
void
trap_init_percpu(void)
{
	// The example code here sets up the Task State Segment (TSS) and
	// the TSS descriptor for CPU 0. But it is incorrect if we are
	// running on other CPUs because each CPU has its own kernel stack.
	// Fix the code so that it works for all CPUs.
	//
	// Hints:
	//   - The macro "thiscpu" always refers to the current CPU's
	//     struct CpuInfo;
	//   - The ID of the current CPU is given by cpunum() or
	//     thiscpu->cpu_id;
	//   - Use "thiscpu->cpu_ts" as the TSS for the current CPU,
	//     rather than the global "ts" variable;
	//   - Use gdt[(GD_TSS0 >> 3) + i] for CPU i's TSS descriptor;
	//   - You mapped the per-CPU kernel stacks in mem_init_mp()
	//   - You won't want to load GD_TSS0: that's the GD_TSS for cpu 0.
	//     The definition of gdt[] shows where the GD_TSSes are stored for
	//     other cpus.
	//
	// ltr sets a 'busy' flag in the TSS selector, so if you
	// accidentally load the same TSS on more than one CPU, you'll
	// get a triple fault.  If you set up an individual CPU's TSS
	// wrong, you may not get a fault until you try to return from
	// user space on that CPU.
	//
	// LAB 5: Your code here:
	uint8_t thiscpu_num = thiscpu->cpu_id;
	struct Taskstate * thiscpu_ts = &thiscpu->cpu_ts;
	// Setup a TSS so that we get the right stack
	// when we trap to the kernel.
	thiscpu_ts->ts_esp0 = KSTACKTOP - (thiscpu_num)*(KSTKSIZE+KSTKGAP);
	thiscpu_ts->ts_ss0 = GD_KD;
	thiscpu_ts->ts_iomb = sizeof(struct Taskstate);
	// Initialize the TSS slot of the gdt.
	gdt[(GD_TSS0 >> 3) + thiscpu_num] = SEG16(STS_T32A, (uint32_t) (thiscpu_ts),
					sizeof(struct Taskstate) - 1, 0);
	gdt[(GD_TSS0 >> 3) + thiscpu_num].sd_s = 0;

	// Load the TSS selector (like other segment selectors, the
	// bottom three bits are special; we leave them 0)
	ltr(GD_TSS0 + sizeof(struct Segdesc) * thiscpu_num);

	// Load the IDT
	lidt(&idt_pd);
}

void
print_trapframe(struct Trapframe *tf)
{
	cprintf("TRAP frame at %p from CPU %d\n", tf, cpunum());
	print_regs(&tf->tf_regs);
	cprintf("  es   0x----%04x\n", tf->tf_es);
	cprintf("  ds   0x----%04x\n", tf->tf_ds);
	cprintf("  trap 0x%08x %s\n", tf->tf_trapno, trapname(tf->tf_trapno));
	// If this trap was a page fault that just happened
	// (so %cr2 is meaningful), print the faulting linear address.
	if (tf == last_tf && tf->tf_trapno == T_PGFLT)
		cprintf("  cr2  0x%08x\n", rcr2());
	cprintf("  err  0x%08x", tf->tf_err);
	// For page faults, print decoded fault error code:
	// U/K=fault occurred in user/kernel mode
	// W/R=a write/read caused the fault
	// PR=a protection violation caused the fault (NP=page not present).
	if (tf->tf_trapno == T_PGFLT)
		cprintf(" [%s, %s, %s]\n",
			tf->tf_err & 4 ? "user" : "kernel",
			tf->tf_err & 2 ? "write" : "read",
			tf->tf_err & 1 ? "protection" : "not-present");
	else
		cprintf("\n");
	cprintf("  eip  0x%08x\n", tf->tf_eip);
	cprintf("  cs   0x----%04x\n", tf->tf_cs);
	cprintf("  flag 0x%08x\n", tf->tf_eflags);
	if ((tf->tf_cs & 3) != 0) {
		cprintf("  esp  0x%08x\n", tf->tf_esp);
		cprintf("  ss   0x----%04x\n", tf->tf_ss);
	}
}

void
print_regs(struct PushRegs *regs)
{
	cprintf("  edi  0x%08x\n", regs->reg_edi);
	cprintf("  esi  0x%08x\n", regs->reg_esi);
	cprintf("  ebp  0x%08x\n", regs->reg_ebp);
	cprintf("  oesp 0x%08x\n", regs->reg_oesp);
	cprintf("  ebx  0x%08x\n", regs->reg_ebx);
	cprintf("  edx  0x%08x\n", regs->reg_edx);
	cprintf("  ecx  0x%08x\n", regs->reg_ecx);
	cprintf("  eax  0x%08x\n", regs->reg_eax);
}

static void
trap_dispatch(struct Trapframe *tf)
{
	// Handle processor exceptions.
	switch(tf->tf_trapno) {
	case T_PGFLT:
		page_fault_handler(tf);
		break;

	case T_BRKPT:
		monitor(tf);
		break;

	case T_SYSCALL: {
		struct PushRegs * regs = &tf->tf_regs;
		regs->reg_eax = syscall(regs->reg_eax, regs->reg_edx, regs->reg_ecx, 
							regs->reg_ebx, regs->reg_edi, regs->reg_esi);
		break;
	}
	case IRQ_OFFSET + IRQ_SPURIOUS:
		// Handle spurious interrupts
		// The hardware sometimes raises these because of noise on the
		// IRQ line or other reasons. We don't care.
		cprintf("Spurious interrupt on irq 7\n");
		print_trapframe(tf);
		return;
	// Handle clock interrupts. Don't forget to acknowledge the
	// interrupt using lapic_eoi() before calling the scheduler!
	// LAB 7: Your code here.
	case IRQ_OFFSET + IRQ_TIMER:
		lapic_eoi();
		sched_yield(); 
	default:
		// Unexpected trap: The user process or the kernel has a bug.
		print_trapframe(tf);
		if (tf->tf_cs == GD_KT)
			panic("unhandled trap in kernel");
		else {
			env_destroy(curenv);
			return;
		}
	}

}

void
trap(struct Trapframe *tf)
{
	// The environment may have set DF and some versions
	// of GCC rely on DF being clear
	asm volatile("cld" ::: "cc");

	// Halt the CPU if some other CPU has called panic()
	extern char *panicstr;
	if (panicstr)
		asm volatile("hlt");

	// Re-acqurie the big kernel lock if we were halted in
	// sched_yield()
	if (xchg(&thiscpu->cpu_status, CPU_STARTED) == CPU_HALTED)
		lock_kernel();
	// Check that interrupts are disabled.  If this assertion
	// fails, DO NOT be tempted to fix it by inserting a "cli" in
	// the interrupt path.
	assert(!(read_eflags() & FL_IF));

	if ((tf->tf_cs & 3) == 3) {
		lock_kernel();
		// Trapped from user mode.
		// Acquire the big kernel lock before doing any
		// serious kernel work.
		// LAB 5: Your code here.
		assert(curenv);

		// Garbage collect if current enviroment is a zombie
		if (curenv->env_status == ENV_DYING) {
			env_free(curenv);
			curenv = NULL;
			sched_yield();
		}

		// Copy trap frame (which is currently on the stack)
		// into 'curenv->env_tf', so that running the environment
		// will restart at the trap point.
		curenv->env_tf = *tf;
		// The trapframe on the stack should be ignored from here on.
		tf = &curenv->env_tf;
	}

	// Record that tf is the last real trapframe so
	// print_trapframe can print some additional information.
	last_tf = tf;

	// Dispatch based on what type of trap occurred
	trap_dispatch(tf);

	// If we made it to this point, then no other environment was
	// scheduled, so we should return to the current environment
	// if doing so makes sense.
	if (curenv && curenv->env_status == ENV_RUNNING)
		env_run(curenv);
	else
		sched_yield();
}


void
page_fault_handler(struct Trapframe *tf)
{
	uint32_t fault_va;

	// Read processor's CR2 register to find the faulting address
	fault_va = rcr2();

	// Handle kernel-mode page faults.

	// check low bits of tf_cs
	if ((tf->tf_cs & 3) == 0) {
		panic("Kernel Mode Page Fault");
	}

	// We've already handled kernel-mode exceptions, so if we get here,
	// the page fault happened in user mode.

	// Call the environment's page fault upcall, if one exists.  Set up a
	// page fault stack frame on the user exception stack (below
	// UXSTACKTOP), then branch to curenv->env_pgfault_upcall.
	//
	// The page fault upcall might cause another page fault, in which case
	// we branch to the page fault upcall recursively, pushing another
	// page fault stack frame on top of the user exception stack.
	//
	// It is convenient for our code which returns from a page fault
	// (lib/pfentry.S) to have one word of scratch space at the top of the
	// trap-time stack; it allows us to more easily restore the eip/esp. In
	// the non-recursive case, we don't have to worry about this because
	// the top of the regular user stack is free.  In the recursive case,
	// this means we have to leave an extra word between the current top of
	// the exception stack and the new stack frame because the exception
	// stack _is_ the trap-time stack.
	//
	// If there's no page fault upcall, the environment didn't allocate a
	// page for its exception stack or can't write to it, or the exception
	// stack overflows, then destroy the environment that caused the fault.
	// Note that the grade script assumes you will first check for the page
	// fault upcall and print the "user fault va" message below if there is
	// none.  The remaining three checks can be combined into a single test.
	//
	// Hints:
	//   user_mem_assert() and env_run() are useful here.
	//   To change what the user environment runs, modify 'curenv->env_tf'
	//   (the 'tf' variable points at 'curenv->env_tf').
	struct Env *env = curenv;
	if (env->env_pgfault_upcall) {
		// set up a page fault stack frame on user exception stack
		struct UTrapframe exception_stack;
		exception_stack.utf_fault_va = fault_va;
		exception_stack.utf_err = tf->tf_err;
		exception_stack.utf_regs = tf->tf_regs;
		exception_stack.utf_eip = tf->tf_eip;
		exception_stack.utf_eflags = tf->tf_eflags;
		exception_stack.utf_esp = tf->tf_esp;
		
		if  (tf->tf_esp >= (UXSTACKTOP - PGSIZE) && tf->tf_esp < UXSTACKTOP) {
			// push empty 32-bit word
			tf->tf_esp -= 0x4;
		}
		else {
			tf->tf_esp = UXSTACKTOP;	
		}
		// push UTrapframe
		tf->tf_esp -= sizeof(struct UTrapframe);
		user_mem_assert(curenv, curenv->env_pgfault_upcall, sizeof(curenv->env_pgfault_upcall), PTE_U|PTE_P);
		user_mem_assert(curenv, (void *) tf->tf_esp, sizeof(struct UTrapframe), PTE_U|PTE_P|PTE_W);
		*(struct UTrapframe *) tf->tf_esp = exception_stack;
			
		curenv->env_tf.tf_eip = (uint32_t) curenv->env_pgfault_upcall;
		env_run(curenv);
	}

	// Destroy the environment that caused the fault.
	cprintf("[%08x] user fault va %08x ip %08x\n",
		curenv->env_id, fault_va, tf->tf_eip);
	print_trapframe(tf);
	env_destroy(curenv);
}

