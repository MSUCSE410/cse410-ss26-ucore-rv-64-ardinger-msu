#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "vm.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(TimeVal *val, int _tz) // TODO: implement sys_gettimeofday in pagetable. (VA to PA) virtual -> physical?
{
	// YOUR CODE
	struct proc *p = curr_proc();

	uint64 phys = useraddr(p->pagetable, (uint64)val); // Map VA to PA
	if (phys == 0) {
		return -1; // Bad addr
	}

	// val param cannot be accessed by the kernel now that we're using virtual memory
	// Saves us from deref
	TimeVal *new_val = (TimeVal *)phys; // Cast PA as a time value ptr

	/* The code in `ch3` will leads to memory bugs*/

	uint64 cycle = get_cycle();
	new_val->sec = cycle / CPU_FREQ;
	new_val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	return 0;
}

// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)
/*
* LAB1: you may need to define sys_task_info here
*/
int sys_task_info(struct TaskInfo *info) {
	struct proc *p = curr_proc();

	uint64 phys = useraddr(p->pagetable, (uint64)info); // Map VA to PA
	if (phys == 0) {
		return -1; // Bad addr
	}

	// info param cannot be accessed by the kernel now that we're using virtual memory
	struct TaskInfo * new_info = (struct TaskInfo *)phys; // Cast PA to TaskInfo ptr
	new_info->status = p->task_info.status;

	// Copy syscall counts from current process to new task info using VA
	memmove(new_info->syscall_times, p->task_info.syscall_times, sizeof(p->task_info.syscall_times));

	uint64 curr_time = get_cycle()*1000/CPU_FREQ;
	new_info->time = curr_time - p->task_info.time; // int?
	return 0;
}

/* params:
start: starting index of the virtual mem

*/
uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd) {
	struct proc *p = curr_proc();

	// start must be page-aligned (virtual mem)
	if (start % PGSIZE != 0) {
		return -1;
	}

	if (len == 0) {
		return -1; // No length of mapped byte
	}

	if (len > 1024 * 1024 * 1024) {
		return -1; // Length is larger than 1 GB
	}

	if (port & ~0x7) {
		return -1; // Other bits of port must be 0 (like 1101)
	}

	if ((port & 0x7) == 0) {
		return -1; // Cannot R, W, or X the memory
	}

	// Convert port into PTE permission bits. I.e. 010 = PTE_W
	int perm = 0;
	if (port & 0x1) {
		perm |= PTE_R;
	}
	if (port & 0x2) {
		perm |= PTE_W;
	}
	if (port & 0x4) {
		perm |= PTE_X;
	}
	perm |= PTE_U; // user

	uint64 a = PGROUNDUP(len);
	while (a > 0) {
		void *pa = kalloc(); // pa is a ptr to physical memory page
		if (pa == 0) {
			return -1;
		}
		if (mappages(p->pagetable, start, PGSIZE, (uint64) pa, perm) != 0) {
			return -1; // walkaddr couldn't allocate a page (page possibly already exists)
		}

		a -= PGSIZE;
		start += PGSIZE;
	}

	return 0;
}

/* params:
len: length of mapped byte
*/
uint64 sys_munmap(uint64 start, uint64 len) {
	struct proc *p = curr_proc();

	// start must be page-aligned
	if (start % PGSIZE != 0) {
		return -1;
	}

	if (len == 0) {
		return -1;
	}

	int num_pages = PGROUNDUP(len) / PGSIZE;

	uint64 b = start;
	for (; b < start + num_pages * PGSIZE; b += PGSIZE) { // Loop thru to unmap virtual mem
		if (useraddr(p->pagetable, b) == 0) {
			return -1; // Can't unmap an already unmapped page
		}
		uvmunmap(p->pagetable, b, 1, 0); // Remove 1 pg from va
	}

	return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	/*
	* LAB1: you may need to update syscall counter for task info here
	*/
	curr_proc()->task_info.syscall_times[id]++;

	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday((TimeVal *)args[0], args[1]);
		break;
	case SYS_task_info:
		ret = sys_task_info((struct TaskInfo *)args[0]); // TODO: no struct
		break;
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
