#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "vm.h"

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
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

uint64 sys_gettimeofday(uint64 val, int _tz)
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

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

/* params:
va: va space of a program name
*/
uint64 sys_spawn(uint64 va)
{
	struct proc *p = curr_proc();
    struct proc *np = NULL; // np = new proc
    char name[200];

    if (copyinstr(p->pagetable, name, va, 200) < 0) { // Copy filename from user va because kernel can't directly deref user ptrs
        return -1;
    }

    // Find app id by name. App registry is list of programs in kernel
    int id = get_id_by_name(name);
    if (id < 0)
        return -1;

    // Allocate new process directly
    np = allocproc();
    if (np == 0)
        return -1;

    np->parent = p;

    // Load program directly into new process
    if (loader(id, np) < 0) {
        np->state = UNUSED;
        return -1;
    }

    np->state = RUNNABLE;
    // add_task(np);

    return np->pid;
}

uint64 sys_set_priority(long long prio)
{
	// TODO: your job is to complete the sys call
	if (prio < 2 || prio > ISIZE_MAX) {
		return -1;
	}
	struct proc *p = curr_proc();
    p->prio = prio;
    p->pass = BIG_STRIDE / prio;
    return prio;
}

/*
* Proj 1 define sys_task_info here
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

	// start must be page-aligned
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
	for (; b < start + num_pages * PGSIZE; b += PGSIZE) {
		if (useraddr(p->pagetable, b) == 0) {
			return -1; // Can't unmap an already unmapped page
		}
		uvmunmap(p->pagetable, b, 1, 0); // Remove 1 pg from va
	}

	return 0;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

/**
 * fd: file descriptor
 * stat: userspace va of a Stat
 */
int sys_fstat(int fd,uint64 stat){
	//TODO: your job is to complete the syscall
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1; // fd is invalid

	struct proc *p = curr_proc();
	struct file *f = p->files[fd];

	if (f == NULL) {
		return -1; // fd is not open
	}

	ivalid(f->ip); // Ensure inode is valid from disk

	Stat st;
	st.dev = 0;
	st.ino = f->ip->inum;
	st.mode = f->ip->type == T_DIR ? DIR : FILE;
	st.nlink = f->ip->nlink;
	
	if (copyout(p->pagetable, stat, (char *)&st, sizeof(st)) < 0) {
		return -1;
	}

	return 0;
}

/**
 * oldpath: userspace va
 */
int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint64 flags){
	//TODO: your job is to complete the syscall
	char old_name[MAX_STR_LEN], new_name[MAX_STR_LEN];

	if (copyinstr(curr_proc()->pagetable, old_name, oldpath, MAX_STR_LEN) < 0 || copyinstr(curr_proc()->pagetable, new_name, newpath, MAX_STR_LEN) < 0 ) { // copy strings from user to kernel
		return -1;
	}

	if (strncmp(old_name, new_name, MAX_STR_LEN) == 0) {
		return -1; // old and new path cannot be the same
	}

	struct inode *ip = namei(old_name);
	if (ip == NULL) {
		return -1;
	}

	if (ip->type == T_DIR) {
		iput(ip);
		return -1;
	}

	ip->nlink++;
	iupdate(ip);

	struct inode *dp = root_dir();
	ivalid(dp);

	// Prevent cross-device links becase inums only unique to a device
	if (dp->dev != ip->dev || dirlink(dp, new_name, ip->inum) < 0) { // Write new dir entry into the dp
		iput(dp);
		
		// Update links on failure
		ip->nlink--;
		iupdate(ip);
		iput(ip);
		return -1;
	}

	iput(dp);
	iput(ip);

	return 0;
}

/**
 * name: file path in userspace va
 */
int sys_unlinkat(int dirfd, uint64 name, uint64 flags){
	//TODO: your job is to complete the syscall
	char path_name[MAX_STR_LEN];

	if (copyinstr(curr_proc()->pagetable, path_name, name, MAX_STR_LEN) < 0) { // copy strings from user to kernel
		return -1;
	}

	struct inode *ip = namei(path_name);
	if (ip == NULL) {
		return -1;
	}

	if (ip->type == T_DIR) {
		// iunlockput(ip);
		iput(ip);
		return -1;
	}

	struct inode *dp = root_dir();
	ivalid(dp);
	struct dirent de;
	int off;

	// Find matching dirent like in dirlink (where we find empty dirent)
	for (off = 0; off < dp->size; off += sizeof(de)) {
		if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
			panic("unlinkat readi");
		if (de.inum == ip->inum && strncmp(de.name, path_name, DIRSIZ) == 0) {
			memset(&de, 0, sizeof(de)); // Clear dirent
			if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
				panic("unlinkat writei");
			break;
		}
	}

	iput(dp);

	ivalid(ip);
	printf("unlinkat: inum=%d nlink=%d ref=%d valid=%d", ip->inum, ip->nlink, ip->ref, ip->valid);
	ip->nlink--;
	iupdate(ip);
	iput(ip);
	
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

	curr_proc()->task_info.syscall_times[id]++;

	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
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
	case SYS_setpriority:
		ret = sys_set_priority((long long)args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
