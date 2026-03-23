#include "proc.h"
#include "timer.h" 
#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
//proj 2
#include "vm.h"      
#include "kalloc.h" 


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

uint64 sys_gettimeofday(TimeVal *val, int _tz) // TODO: implement sys_gettimeofday in pagetable. (VA to PA)
{
	// YOUR CODE
	    struct proc *p = curr_proc();
    uint64 cycle = get_cycle();
    
    // Translate virtual address to physical address
    uint64 pa = useraddr(p->pagetable, (uint64)val);
    if (pa == 0) {
        return -1;  // Invalid address
    }
    
    TimeVal *pval = (TimeVal *)pa;
    pval->sec = cycle / CPU_FREQ;
    pval->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
    return 0;


	// val->sec = 0;
	// val->usec = 0;

	// /* The code in `ch3` will leads to memory bugs*/

	// // uint64 cycle = get_cycle();
	// // val->sec = cycle / CPU_FREQ;
	// // val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	// return 0;
}

// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)
/*
* LAB1: you may need to define sys_task_info here
*/
int sys_task_info(struct TaskInfo *ti) {
    struct proc *p = curr_proc();
    //proj 2: Translate virtual address to physical
    uint64 pa = useraddr(p->pagetable, (uint64)ti);
    if (pa == 0) {
        return -1;
    }
    // struct TaskInfo *ti = (struct TaskInfo *)pa;
	
	
	ti-status = Running;   // currently executing → always Running
    // copy syscall counts
    for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
        ti->syscall_times[i] = p->syscall_times[i];
    }
    // compute elapsed ms since first scheduled
    uint64 elapsed_cycles = get_cycle() - p->start_time;
    ti->time = (int)(elapsed_cycles * 1000 / CPU_FREQ);
    return 0;
}


extern char trap_page[];


//add proj 2
int sys_mmap(uint64 start, uint64 len, int port, int flag, int fd) {
    struct proc *p = curr_proc();
    
    // len = 0, just return
    if (len == 0) {
        return 0;
    }
    
    // Check max size (1 GiB)
    if (len > 1024 * 1024 * 1024) {
        return -1;
    }
    
    // start must be page-aligned
    if (start % PGSIZE != 0) {
        return -1;
    }
    
    // Check port: other bits must be 0
    if ((port & ~0x7) != 0) {
        return -1;
    }
    
    // Check port: at least one of R/W/X must be set
    if ((port & 0x7) == 0) {
        return -1;
    }
    
    // Round up to page size
    len = PGROUNDUP(len);
    uint64 npages = len / PGSIZE;
    
    // Convert port to PTE permissions
    int perm = PTE_U | PTE_V;
    if (port & 0x1) perm |= PTE_R;
    if (port & 0x2) perm |= PTE_W;
    if (port & 0x4) perm |= PTE_X;
    
    // Map each page
    for (uint64 i = 0; i < npages; i++) {
        uint64 va = start + i * PGSIZE;
        
        // Allocate physical page
        void *pa = kalloc();
        if (pa == 0) {
            return -1;  // Out of memory
        }
        
        memset(pa, 0, PGSIZE);
        
        // Map it
        if (mappages(p->pagetable, va, PGSIZE, (uint64)pa, perm) != 0) {
            kfree(pa);
            return -1;  // Already mapped
        }
    }
    
    return 0;
}

int sys_munmap(uint64 start, uint64 len) {
    struct proc *p = curr_proc();
    
    // Round up to page size
    len = PGROUNDUP(len);
    uint64 npages = len / PGSIZE;
    
    // Check if all pages are mapped
    for (uint64 i = 0; i < npages; i++) {
        uint64 va = start + i * PGSIZE;
        if (walkaddr(p->pagetable, va) == 0) {
            return -1;  // Unmapped page found
        }
    }
    
    // Unmap and free
    uvmunmap(p->pagetable, start, npages, 1);
    return 0;
}
//

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
	if (id >= 0 && id < MAX_SYSCALL_NUM) {
        curr_proc()->syscall_times[id]++;
    }

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
	
	//proj 2
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break;
    case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
	/*
	* LAB1: you may need to add SYS_taskinfo case here
	*/
	case SYS_task_info:
        ret = sys_task_info((struct TaskInfo *)args[0]);
        break;
	

	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
