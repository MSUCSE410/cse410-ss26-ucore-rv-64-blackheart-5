#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "types.h"
#include "proc.h"
#include "vm.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, va, len);
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

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	debugf("sys_read fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDIN)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
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
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}


uint64 sys_task_info(struct TaskInfo *ti)
{
    struct proc *p = curr_proc();
    if (!p) return -1;
 
    struct TaskInfo info;
    info.status = Running;
 
    for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
        info.syscall_times[i] = p->syscall_times[i];
    }
 
    uint64 elapsed = get_cycle() - p->start_time;
    info.time = (int)(elapsed * 1000 / CPU_FREQ);
 
    if (copyout(p->pagetable, (uint64)ti, (char *)&info, sizeof(info)) < 0) {
        return -1;
    }
 
    return 0;
}
 

int sys_mmap(uint64 start, uint64 len, int port, int flag, int fd)
{
    struct proc *p = curr_proc();
    
    if (len == 0) return 0;
    if (len > 1024ULL * 1024 * 1024) return -1;
    if (start % PGSIZE != 0) return -1;
    if ((port & ~0x7) != 0) return -1;
    if ((port & 0x7) == 0) return -1;
    
    uint64 rounded_len = PGROUNDUP(len);
    uint64 npages = rounded_len / PGSIZE;
    
    for (uint64 i = 0; i < npages; i++) {
        uint64 va = start + i * PGSIZE;
        if (walkaddr(p->pagetable, va) != 0) {
            return -1;
        }
    }
    
    int perm = PTE_U;
    if (port & 0x1) perm |= PTE_R;
    if (port & 0x2) perm |= PTE_W;
    if (port & 0x4) perm |= PTE_X;
    
    for (uint64 i = 0; i < npages; i++) {
        uint64 va = start + i * PGSIZE;
        void *pa = kalloc();
        if (pa == 0) return -1;
        
        memset(pa, 0, PGSIZE);
        
        if (mappages(p->pagetable, va, PGSIZE, (uint64)pa, perm) != 0) {
            kfree(pa);
            return -1;
        }
    }
    
    return 0;
}
 
 
int sys_munmap(uint64 start, uint64 len)
{
    struct proc *p = curr_proc();
 
    if (start % PGSIZE != 0) {
        return -1;
    }
    
    len = PGROUNDUP(len);
    uint64 npages = len / PGSIZE;
    
    for (uint64 i = 0; i < npages; i++) {
        uint64 va = start + i * PGSIZE;
        if (walkaddr(p->pagetable, va) == 0) {
            return -1;
        }
    }
    
    uvmunmap(p->pagetable, start, npages, 1);
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
	debugf("fork!\n");
	return fork();
}

uint64 sys_exec(uint64 va)
{
	struct proc *p = curr_proc();
	char name[200];
	copyinstr(p->pagetable, name, va, 200);
	debugf("sys_exec %s\n", name);
	return exec(name);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

// spawn: create a new child process that runs the program named `va`.
// Equivalent to fork+exec BUT without ever copying the parent's memory.
//
// Why not just call fork() then exec() in the kernel?
// Because fork() sets up the child's context so it returns to usertrapret
// with ra/sp pointing into the child's kernel stack. If we then called
// exec() here, it would run on the *current* process (the parent), replacing
// the parent's memory. That's the opposite of what we want.
//
// So we build the child directly:
//   1) allocproc  -> new PCB with fresh page table
//   2) bin_loader -> map the target program into that page table
//   3) parent link + add to ready queue
//
// Returns the child's pid on success, -1 on any error.
uint64 sys_spawn(uint64 va)
{
	// TODO: your job is to complete the sys call
	struct proc *p = curr_proc();

	// ---- 1. Copy the filename from user space into the kernel ----
	char name[MAX_STR_LEN];
	if (copyinstr(p->pagetable, name, va, MAX_STR_LEN) < 0) {
		return -1;
	}

	// ---- 2. Look up the app id (invalid filename -> error) ----
	int id = get_id_by_name(name);
	if (id < 0) {
		return -1;
	}

	// ---- 3. Allocate a new PCB (full process pool -> error) ----
	struct proc *np = allocproc();
	if (np == NULL) {
		return -1;
	}

	// ---- 4. Load the target program into the child's page table ----
	// `loader` calls `bin_loader` which: allocates pages, copies the
	// program's .text/.data into them, maps them at BASE_ADDRESS,
	// allocates a user stack, sets up trapframe (sp, epc), and sets
	// state = RUNNABLE. No parent memory is copied anywhere.
	// Note: loader() currently panics internally on memory errors,
	// so it won't return < 0 in practice. We keep the check for safety,
	// but can't call freeproc here because it isn't declared in proc.h.
	// On loader failure the kernel would panic anyway.
	if (loader(id, np) < 0) {
		return -1;
	}
	// ---- 5. Establish parent-child relationship ----
	// Needed so the parent can wait() on this child and collect exit code.
	np->parent = p;

	// ---- 6. Make the child eligible for scheduling ----
	// bin_loader already set state = RUNNABLE; just put it in the queue.
	add_task(np);

	// ---- 7. Return child's PID (like fork does for the parent) ----
	return np->pid;
}

// Set the priority of the calling process.
// Per spec: prio must be in [2, isize_max]. We accept any prio >= 2.
// Returns prio on success, -1 on failure.
uint64 sys_set_priority(long long prio){
    // TODO: your job is to complete the sys call
    // Priority must be >= 2 (priority of 1 would make pass = BIG_STRIDE,
	// which starves every other process; priority of 0 is division-by-zero).
	if (prio < 2) {
		return -1;
	}

	struct proc *p = curr_proc();
	p->priority = (uint64)prio;

	// Recompute pass immediately so the next scheduling reflects the new priority.
	p->pass = BIG_STRIDE / p->priority;

	return prio;
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
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
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
 
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break;
	
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
	
	case SYS_task_info:
		ret = sys_task_info((struct TaskInfo *)args[0]);
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
		ret = sys_exec(args[0]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	///spawn case
	case SYS_spawn:
		ret = sys_spawn(args[0]);
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

