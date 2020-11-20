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
	cprintf("faulting addr: %x \n", addr);
	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	if ( ((err & FEC_WR) == 0) || (uvpt[PGNUM(ROUNDDOWN(addr, PGSIZE))] & PTE_COW) == 0) {
		panic("faulting access is not a write to a copy-on-write page");
	}
	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	envid_t envid = 0; // set envid to 0 (current environment)
	if ((sys_page_alloc(envid, PFTEMP, PTE_P | PTE_U | PTE_W)) < 0) {
		panic("sys_page_alloc failed");
	}
	memcpy(PFTEMP, ROUNDDOWN(addr, PGSIZE), PGSIZE);
	int pgmap_result = sys_page_map(envid, PFTEMP, envid, ROUNDDOWN(addr,PGSIZE ), PTE_P | PTE_U | PTE_W);
	if (pgmap_result < 0) {
		panic("sys_page_map failed");
	}
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn)
{
	int r;
	int perm = PTE_P | PTE_U;
	// Handles shared pages
	if ((uvpt[(size_t)pn] & PTE_SHARE) == PTE_SHARE) {
		r = sys_page_map(thisenv->env_id, (void *)(pn*PGSIZE), envid, (void *)(pn*PGSIZE), uvpt[(size_t)pn]&PTE_SYSCALL);
		if (r < 0) {
			panic("failed to map page in child.\n");
		}
	}
	else if ((uvpt[(size_t)pn] & PTE_W) || (uvpt[(size_t)pn] & PTE_COW)) {
		r = sys_page_map(thisenv->env_id, (void *)(pn*PGSIZE), envid, (void *)(pn*PGSIZE), perm | PTE_COW);
		if (r < 0) {
			panic("failed to map page in child.\n");
		}
		int map_result = sys_page_map(thisenv->env_id, (void *)(pn*PGSIZE), thisenv->env_id, (void *)(pn*PGSIZE), perm | PTE_COW);
		if (map_result < 0) {
			panic("failed to map page.");
		}
	}
	else { // Handling pages that are present but not copy-on-write or writable
	       	r = sys_page_map(thisenv->env_id, (void *)(pn*PGSIZE), envid, (void *)(pn*PGSIZE), perm);
		if (r < 0) {
			panic("failed to map present page at child.");
		}
	}
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

envid_t
fork(void)
{
	set_pgfault_handler(pgfault);
	envid_t envid;
	envid = sys_exofork();
	if (envid < 0) {
		panic("sys_exofork failed: %e", envid);
	}
	if (envid == 0) {
		thisenv = &envs[ENVX(sys_getenvid())];
		return 0;
	}
	// exception stack for child.
	int alloc_result = sys_page_alloc(envid, (void *) UXSTACKTOP - PGSIZE, PTE_P | PTE_U | PTE_W);
	if (alloc_result < 0) {
		panic("cannot allocate page for exception stack");
	}
	size_t pgnum;
	for (pgnum = 0; pgnum < PGNUM(UTOP)-1; pgnum++) {
		if ((uvpd[(pgnum >> 10)] & PTE_U) && (uvpd[(pgnum >> 10)] & PTE_P)) {
			if ( (uvpt[pgnum] & PTE_U) && (uvpt[pgnum] & PTE_P) ) {
				duppage(envid, pgnum);
			}
		}
	}
	int set_upcall_result = sys_env_set_pgfault_upcall(envid, thisenv->env_pgfault_upcall);
	if (set_upcall_result < 0 ) {
		panic("setting upcall for child failed.");
	}
	int set_status_result = sys_env_set_status(envid, ENV_RUNNABLE);
	if (set_status_result < 0) {
		return set_status_result;
	}
	return envid;
	
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
