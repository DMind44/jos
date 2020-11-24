// program to cause a breakpoint trap

#include <inc/lib.h>

void
umain(int argc, char **argv)
{
	asm volatile("int $3");
	cprintf("1st line after 1st breakpoint. \n");
	cprintf("2nd line after 1st breakpoint. \n");
	asm volatile("int $3");
	cprintf("1st line after 2nd breakpoint. \n");
	cprintf("2nd line after 2nd breakpoint. \n");
}

