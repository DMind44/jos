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
	cprintf("Inside pgfault.\n");
	void *addr = (void *) utf->utf_fault_va;
	uint32_t err = utf->utf_err;
	int r;

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at uvpt
	//   (see <inc/memlayout.h>).

	// LAB 5: Your code here.
	cprintf("pte for addr: %x. \n", uvpt[PGNUM(addr)]);
	cprintf("pgaddr for addr: %x. \n", PGADDR(PDX(addr), PTX(addr), PGOFF(addr)));
	cprintf("addr and PTE_COW: %x. \n", ((pte_t)addr&PTE_COW));
	cprintf("err and error write: %x. \n", (err & FEC_WR));
	cprintf("eip of trapframe: %x. \n", utf->utf_eip);
	if ( ((err & FEC_WR) == 0) || (uvpt[PGNUM(addr)] & PTE_COW) == 0) {
		panic("faulting access is not a write to a copy-on-write page");
	}
	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.

	// LAB 5: Your code here.
	envid_t envid = thisenv->env_id;
	if (sys_page_alloc(envid, PFTEMP, PTE_P | PTE_U | PTE_W) < 0) {
		panic("sys_page_alloc failed");
	}
//	cprintf("Right before memcpy.");
	memcpy(PFTEMP, addr, PGSIZE); 
	int pgmap_result = sys_page_map(envid, PFTEMP, envid, addr, PTE_P | PTE_U | PTE_W);
	if (pgmap_result < 0) {
		panic("sys_page_map failed");
	}
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
	// how do I check the perms of a page?
	int perm = PTE_P | PTE_U;
//	envid_t cur_envid = sys_getenvid(); //thisenv OR 
	if (uvpt[pn] & PTE_W || uvpt[pn] & PTE_COW) {
	       	r = sys_page_map(thisenv->env_id, (void *)(pn*PGSIZE), envid, (void *)(pn*PGSIZE), perm | PTE_COW); //find macro for pn*PGSIZE
		if (r < 0) {
			panic("failed to map page in child.\n");
		}
		// how do I mark the mapping copy-on-write?
	       	int map_result = sys_page_map(thisenv->env_id, (void *)(pn*PGSIZE), thisenv->env_id, (void *)(pn*PGSIZE), perm | PTE_COW);
		if (map_result < 0) {
		        panic("failed to map page.");
		}
	}
	else {
		// Handling pages that are present but not copy-on-write or writable
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
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever bpe marked copy-on-write,
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
	if (envid == 0) {
		//set page fualt handler
		set_pgfault_handler(pgfault);
		thisenv = &envs[ENVX(sys_getenvid())];
		return 0;
	}

	if (envid < 0) {
		panic("sys_exofork failed: %e", envid);
	}
	// copy address space and page fault handler to the child
	cprintf("PDX(UTOP): %d \n", PDX(UTOP));
	cprintf("uvpt at UTOP: %x \n", uvpt[PGNUM(UTOP)]);
//	cprintf("uvpt[1022]: %x \n", uvpt[1025]);		
	size_t pgnum;
	for (pgnum = 0; pgnum < PGNUM(UTOP); pgnum++) {
//	for (pgnum = 0; pgnum < NPTENTRIES; pgnum++) {
		// Question: here PGNUM(UTOP) = 977920 but when I run the code, the loop ends at pgnum = 1023. What seems to be going wrong here?  Solution? use the address &uvpt[pgnum] instead.
		// Page fault is caused everytime a child tries to print. I may not be duplicating pages properly.
		if ( (uvpd[pgnum] & PTE_U) && (uvpd[pgnum] & PTE_P) ) {
			if ( (uvpt[pgnum] & PTE_U) && (uvpt[pgnum] & PTE_P) ) { // before this, check that uvpd is present.
				duppage(envid, pgnum);
			}
		}
//		cprintf("end of iteration reached. pgnum: %08x. pgnum UTOP: %08x\n", pgnum, PGNUM(UTOP));
	}
	cprintf("pgnum: %d\n", pgnum);
	cprintf("alloc result reached.\n");	
	int alloc_result = sys_page_alloc(envid, (void *) UXSTACKTOP - PGSIZE, PTE_W | PTE_P | PTE_U);
	if (alloc_result < 0) {
		panic("cannot allocate page for exception stack");
	}
	// mark the child as runnable and return child envid if in parent, 0 if in child and <0 on error
	cprintf("set status reached.\n");	
	int set_status_result = sys_env_set_status(envid, ENV_RUNNABLE);
	if (set_status_result < 0) {
		return set_status_result;
	}
	cprintf("end of fork reached.\n");
	return envid;
	
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
