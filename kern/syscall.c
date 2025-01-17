/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/syscall.h>
#include <kern/console.h>
#include <kern/sched.h>

// Print a string to the system console.
// The string is exactly 'len' characters long.
// Destroys the environment on memory errors.
static void
sys_cputs(const char *s, size_t len)
{
	// Check that the user has permission to read memory [s, s+len).
	// Destroy the environment if not.
	user_mem_assert(curenv, s, len,  PTE_U | PTE_P);

	// Print the string supplied by the user.
	cprintf("%.*s", len, s);
}

// Read a character from the system console without blocking.
// Returns the character, or 0 if there is no input waiting.
static int
sys_cgetc(void)
{
	return cons_getc();
}

// Returns the current environment's envid.
static envid_t
sys_getenvid(void)
{
	return curenv->env_id;
}

// Destroy a given environment (possibly the currently running environment).
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_destroy(envid_t envid)
{
	int r;
	struct Env *e;
	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	if (e == curenv)
		cprintf("[%08x] exiting gracefully\n", curenv->env_id);
	else
		cprintf("[%08x] destroying %08x\n", curenv->env_id, e->env_id);
	env_destroy(e);
	return 0;
}

// Deschedule current environment and pick a different one to run.
static void
sys_yield(void)
{
	sched_yield();
}

// Allocate a new environment.
// Returns envid of new environment, or < 0 on error.  Errors are:
//	-E_NO_FREE_ENV if no free environment is available.
//	-E_NO_MEM on memory exhaustion.
static envid_t
sys_exofork(void)
{
	// Create the new environment with env_alloc(), from kern/env.c.
	// It should be left as env_alloc created it, except that
	// status is set to ENV_NOT_RUNNABLE, and the register set is copied
	// from the current environment -- but tweaked so sys_exofork
	// will appear to return 0.

	struct Env * new_env;
	int alloc_result = env_alloc(&new_env, curenv->env_id); 
	if (alloc_result < 0)
		return alloc_result;
	new_env->env_status = ENV_NOT_RUNNABLE;
	new_env->env_tf = curenv->env_tf;
	new_env->env_tf.tf_regs.reg_eax = 0;
	return new_env->env_id;
}

// Set envid's env_status to status, which must be ENV_RUNNABLE
// or ENV_NOT_RUNNABLE.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if status is not a valid status for an environment.
static int
sys_env_set_status(envid_t envid, int status)
{
	// Hint: Use the 'envid2env' function from kern/env.c to translate an
	// envid to a struct Env.
	// You should set envid2env's third argument to 1, which will
	// check whether the current environment has permission to set
	// envid's status.

	struct Env * env;
	int envid_result = envid2env(envid, &env, 1);
	if (envid_result < 0)
		return envid_result;
	if (status != ENV_RUNNABLE && status != ENV_NOT_RUNNABLE)
		return -E_INVAL;
	env->env_status = status;
	return 0;

}

// Set the page fault upcall for 'envid' by modifying the corresponding struct
// Env's 'env_pgfault_upcall' field.  When 'envid' causes a page fault, the
// kernel will push a fault record onto the exception stack, then branch to
// 'func'.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_pgfault_upcall(envid_t envid, void *func)
{
	struct Env * env;
	if(ENVX(envid) >= NENV || (envid2env(envid, &env, 1) < 0))
		return -E_BAD_ENV;
	env->env_pgfault_upcall = func;
	return 0;
}

// Allocate a page of memory and map it at 'va' with permission
// 'perm' in the address space of 'envid'.
// The page's contents are set to 0.
// If a page is already mapped at 'va', that page is unmapped as a
// side effect.
//
// perm -- PTE_U | PTE_P must be set, PTE_AVAIL | PTE_W may or may not be set,
//         but no other bits may be set.  See PTE_SYSCALL in inc/mmu.h.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
//	-E_INVAL if perm is inappropriate (see above).
//	-E_NO_MEM if there's no memory to allocate the new page,
//		or to allocate any necessary page tables.
static int
sys_page_alloc(envid_t envid, void *va, int perm)
{
	struct Env * env;
	if (ENVX(envid) >= NENV || (envid2env(envid, &env, 1) < 0))
		return -E_BAD_ENV;
	if (va >= (void *)UTOP ||(int) va % PGSIZE != 0)
		return -E_INVAL;
	if (!(perm & (PTE_U | PTE_P)))
		return -E_INVAL;
	struct PageInfo * page = page_alloc(0);
	if (page_insert(env->env_pgdir, page, va, perm) != 0) {
		page_free(page);
		return -E_NO_MEM;
	}
	return 0;
}

// Map the page of memory at 'srcva' in srcenvid's address space
// at 'dstva' in dstenvid's address space with permission 'perm'.
// Perm has the same restrictions as in sys_page_alloc, except
// that it also must not grant write access to a read-only
// page.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if srcenvid and/or dstenvid doesn't currently exist,
//		or the caller doesn't have permission to change one of them.
//	-E_INVAL if srcva >= UTOP or srcva is not page-aligned,
//		or dstva >= UTOP or dstva is not page-aligned.
//	-E_INVAL is srcva is not mapped in srcenvid's address space.
//	-E_INVAL if perm is inappropriate (see sys_page_alloc).
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in srcenvid's
//		address space.
//	-E_NO_MEM if there's no memory to allocate any necessary page tables.
static int
sys_page_map(envid_t srcenvid, void *srcva,
	     envid_t dstenvid, void *dstva, int perm)
{
	// Hint: This function is a wrapper around page_lookup() and
	//   page_insert() from kern/pmap.c.
	//   Again, most of the new code you write should be to check the
	//   parameters for correctness.
	//   Use the third argument to page_lookup() to
	//   check the current permissions on the page.

	if(ENVX(srcenvid) >= NENV || ENVX(dstenvid) >= NENV) {
		return -E_BAD_ENV;
	}
	struct Env * srcenv;
	struct Env * dstenv;
	int srcenvid_result = envid2env(srcenvid, &srcenv, 1);
	int dstenvid_result = envid2env(dstenvid, &dstenv, 1);
	if (srcenvid_result < 0) {
		return srcenvid_result;
	}
	if(dstenvid_result < 0) {
		return dstenvid_result;
	}
	if (srcva >= (void *)UTOP || dstva >= (void *)UTOP ||
	    ((int)srcva % PGSIZE) != 0 || ((int)dstva % PGSIZE) != 0) {
		return -E_INVAL;
	}
	if (!(perm & (PTE_U | PTE_P))) {
		return -E_INVAL;
	}
	pte_t * pte_store;
	struct PageInfo * srcpage = page_lookup(srcenv->env_pgdir, srcva, &pte_store);
	if(!srcpage) {
		return -E_INVAL;
	}
	if ((perm&PTE_W) && !(*pte_store & PTE_W)) {
		return -E_INVAL;
	}
	int insert_result = page_insert(dstenv->env_pgdir, srcpage, dstva, perm);
	if(insert_result) {
		return -E_NO_MEM;
	}
	return 0;
}

// Unmap the page of memory at 'va' in the address space of 'envid'.
// If no page is mapped, the function silently succeeds.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
static int
sys_page_unmap(envid_t envid, void *va)
{
	//This function is a wrapper around page_remove().
	if (ENVX(envid) >= NENV)
		return -E_BAD_ENV;
	struct Env * env;
	int envid_result = envid2env(envid, &env, 1);
	if (envid_result < 0)
		return envid_result;
	if(va >= (void *)UTOP || (int)va%PGSIZE != 0)
		return -E_INVAL;
	page_remove(env->env_pgdir, va);
	return 0;

}

static int
ipc_helper(envid_t recvenvid, envid_t sendenvid, uint32_t value, void *srcva, unsigned perm)
{
	struct Env *recvenv;
	struct Env *sendenv;
	if (envid2env(recvenvid, &recvenv, 0) < 0) {
		return -E_BAD_ENV;
	}
	if (envid2env(sendenvid, &sendenv, 0) < 0) {
		return -E_BAD_ENV;
	}
	recvenv->env_ipc_perm = 0;
	if (srcva < (void *) UTOP && (recvenv->env_ipc_dstva < (void *) UTOP) ) {
		if ( ((int) srcva%PGSIZE) !=0) {
			return -E_INVAL;
		}
		if (!(perm & (PTE_U | PTE_P)) ) {
			return -E_INVAL;
		}
		pte_t * pte;	
		struct PageInfo * srcpage = page_lookup(sendenv->env_pgdir, srcva, &pte);	
		if (!(srcpage) ) {
			return -E_INVAL;
		}

		if ((perm & PTE_W) && !(*pte & PTE_W)) {
			return -E_INVAL;
		}
		int insert_result = page_insert(recvenv->env_pgdir, srcpage,
						recvenv->env_ipc_dstva, perm);
		if(insert_result < 0) {
			return -E_NO_MEM;
		}
		recvenv->env_ipc_perm = perm;
	}
	//send value
	recvenv->env_ipc_recving = 0;
	recvenv->env_ipc_from = sendenvid;
	recvenv->env_ipc_value = value;
	recvenv->env_tf.tf_regs.reg_eax = 0;
	recvenv->env_status = ENV_RUNNABLE;
	return 0;
}
// Try to send 'value' to the target env 'envid'.
// If srcva < UTOP, then also send page currently mapped at 'srcva',
// so that receiver gets a duplicate mapping of the same page.
//
// The send fails with a return value of -E_IPC_NOT_RECV if the
// target is not blocked, waiting for an IPC.
//
// The send also can fail for the other reasons listed below.
//
// Otherwise, the send succeeds, and the target's ipc fields are
// updated as follows:
//    env_ipc_recving is set to 0 to block future sends;
//    env_ipc_from is set to the sending envid;
//    env_ipc_value is set to the 'value' parameter;
//    env_ipc_perm is set to 'perm' if a page was transferred, 0 otherwise.
// The target environment is marked runnable again, returning 0
// from the paused sys_ipc_recv system call.  (Hint: does the
// sys_ipc_recv function ever actually return?)
//
// If the sender wants to send a page but the receiver isn't asking for one,
// then no page mapping is transferred, but no error occurs.
// The ipc only happens when no errors occur.
//
// Returns 0 on success, < 0 on error.
// Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist.
//		(No need to check permissions.)
//	-E_IPC_NOT_RECV if envid is not currently blocked in sys_ipc_recv,
//		or anothepr environment managed to send first.
//	-E_INVAL if srcva < UTOP but srcva is not page-aligned.
//	-E_INVAL if srcva < UTOP and perm is inappropriate
//		(see sys_page_alloc).
//	-E_INVAL if srcva < UTOP but srcva is not mapped in the caller's
//		address space.
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in the
//		current environment's address space.
//	-E_NO_MEM if there's not enough memory to map srcva in envid's
//		address space.
static int
sys_ipc_send(envid_t envid, uint32_t value, void *srcva, unsigned perm)
{
	struct Env * e;	
	if (envid2env(envid, &e, 0) < 0) {
		return -E_BAD_ENV;
	}
	// check if there is a recving env, if not, we save our parameters
	// and set ourselves NOTRUNNABLE
	if (!e->env_ipc_recving) {
		curenv->value_to_send = value;
		curenv->srcva_to_send = srcva;
		curenv->perm_for_send = perm;
		e->env_senders[e->senders_count] = curenv->env_id;
		e->senders_count++;
		curenv->env_status = ENV_NOT_RUNNABLE;
		sys_yield();
	}
	return ipc_helper(envid, curenv->env_id, value, srcva, perm);
}

// Block until a value is ready.  Record that you want to receive
// using the env_ipc_recving and env_ipc_dstva fields of struct Env,
// mark yourself not runnable, and then give up the CPU.
//
// If 'dstva' is < UTOP, then you are willing to receive a page of data.
// 'dstva' is the virtual address at which the sent page should be mapped.
//
// This function only returns on error, but the system call will eventually
// return 0 on success.
// Return < 0 on error.  Errors are:
//	-E_INVAL if dstva < UTOP but dstva is not page-aligned.
static int
sys_ipc_recv(void *dstva)
{

	if ((dstva < (void *)UTOP) && ((int)dstva % PGSIZE) != 0) {
		return -E_INVAL;
	}

	// Go to sleep if count == 0
	if (curenv->senders_count == 0) {
		curenv->env_ipc_recving = 1;
		curenv->env_ipc_dstva = dstva;
		curenv->env_status = ENV_NOT_RUNNABLE;
		sys_yield();
		return 0; // for compiler
	}
	
	// Decrease the count so that we are dealing with the env that incremented senders_count
	curenv->senders_count--;
	envid_t sendenvid = curenv->env_senders[curenv->senders_count];
	struct Env *sendenv;
	if (envid2env(sendenvid, &sendenv, 0) < 0) {
		return -E_BAD_ENV;
	}
	curenv->env_ipc_dstva = dstva;
	int helper_result = ipc_helper(curenv->env_id, sendenvid, sendenv->value_to_send,
			sendenv->srcva_to_send, sendenv->perm_for_send);
	if (helper_result < 0) {
		return helper_result;
	}
	// set the sending env to be runnable again.
	sendenv->env_tf.tf_regs.reg_eax = 0;
	sendenv->env_status = ENV_RUNNABLE;
	return 0;
}



// Dispatches to the correct kernel function, passing the arguments.
int32_t
syscall(uint32_t syscallno, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	// Call the function corresponding to the 'syscallno' parameter.
	// Return any appropriate return value.

	switch (syscallno) {
	case SYS_cputs:
		sys_cputs((char *) a1, a2);
		return 0;
	case SYS_cgetc:
		return sys_cgetc();
	case SYS_getenvid:
		return (int32_t) sys_getenvid();
	case SYS_env_destroy:
		return sys_env_destroy((envid_t) a1);
	case SYS_yield:
		sys_yield();
		return 0;
	case SYS_exofork:
		return sys_exofork();
	case SYS_env_set_status:
		return sys_env_set_status((envid_t) a1, a2);
	case SYS_page_alloc:
		return sys_page_alloc((envid_t) a1, (void *)a2, a3);
	case SYS_page_map:
		return sys_page_map((envid_t) a1, (void *)a2, (envid_t) a3, (void *)a4, a5);
	case SYS_page_unmap:
		return sys_page_unmap((envid_t) a1, (void *)a2);
	case SYS_env_set_pgfault_upcall:
		return sys_env_set_pgfault_upcall((envid_t) a1, (void *) a2);
	case SYS_ipc_recv:
		return sys_ipc_recv((void *)a1);
	case SYS_ipc_send:
		return sys_ipc_send( (envid_t) a1, a2, (void *) a3, (unsigned) a4); 
	default:
		return -E_INVAL;
	}
}

