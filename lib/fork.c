// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	uint32_t err = utf->utf_err;
	int r;

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at uvpt
	//   (see <inc/memlayout.h>).

	// LAB 5: Your code here.

	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.

	// LAB 5: Your code here.

	panic("pgfault not implemented");
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn)
{
	int r;

	// LAB 5: Your code here.
	// map virtual page pn into target envid at the same virtual address
	// if page is writable or copy-on-write, the new mapping must be created copy-on-write
	// then our mapping must be marked copy-on-write as well
       	sys_page_map(curenv, (void *)(pn*PGSIZE), envid, (void *)(pn*PGSIZZE), perm);
	panic("duppage not implemented");
	return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void)
{
	// LAB 5: Your code here.
	// uvpt is the va of virtual page table
	// uvpd is the va of current page directory
	// In parent:
	// set pgfault handler
	set_pgfault_handler(pgfault);
	// create child
	envid_t envid;
	envid = sys_exofork();
	if (envid < 0) {
		panic("sys_exofork failed: %e", envid);
	}
	// copy address space and page fault handler to the child
	size_t i;
	for (i = 0; i < (UVPT + PTSIZE)/PGSIZE; i++) {
		if (uvpt[i] < UTOP) {
			duppage(envid, uvpt[i]);
		}
/*		if (i == (UVPT + PTSIZE)/PGSIZE - 1) { // Takes care of copying page fault handler?
			duppage(envid, uvpd[i]);
			} */
	}
	int alloc_result = sys_page_alloc(envid, (void *) UXSTACKTOP - PGSIZE, PTE_W | PTE_P | PTE_U);
	if (alloc_result < 0) {
		panic("cannot allocate page for exception stack");
	}
	// mark the child as runnable and return child envid if in parent, 0 if in child and <0 on error
	int set_status_result = sys_env_set_status(envid, ENV_RUNNABLE);
	if (set_status_result < 0) {
		return set_status_result;
	}
	if (envid == 0) {
		//set page fualt handler
		set_pgfault_handler(pgfault);
		thisenv = &envs[ENVX(sys_getenvid())];
		return 0;
	}
	else {
		return envid;
	}
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
