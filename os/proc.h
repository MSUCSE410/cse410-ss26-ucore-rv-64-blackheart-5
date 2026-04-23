#ifndef PROC_H
#define PROC_H
#define MAX_SYSCALL_NUM 512

#include "riscv.h"
#include "types.h"
#include "queue.h"

#define NPROC (512)
#define FD_BUFFER_SIZE (16)
// Stride scheduling constants (Project 3).
// Must be large enough that BIG_STRIDE / priority keeps good resolution,
// but small enough not to overflow quickly. 65536 recommended by the guide.
#define BIG_STRIDE (65536)
#define DEFAULT_PRIORITY (16)

struct file;

// Saved registers for kernel context switches.
struct context {
	uint64 ra;
	uint64 sp;

	// callee-saved
	uint64 s0;
	uint64 s1;
	uint64 s2;
	uint64 s3;
	uint64 s4;
	uint64 s5;
	uint64 s6;
	uint64 s7;
	uint64 s8;
	uint64 s9;
	uint64 s10;
	uint64 s11;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

typedef enum {
    Unused,
    Used,
    Sleeping,
    Runnable,
    Running,
    Zombie,
} TaskStatus;
 
struct TaskInfo {
    TaskStatus status;
    uint32 syscall_times[MAX_SYSCALL_NUM];
    uint32 time;
};

// Per-process state
struct proc {
	enum procstate state;        // Process state
	int pid;                     // Process ID
	pagetable_t pagetable;       // User page table
	uint64 ustack;               // Virtual address of user stack
	uint64 kstack;               // Virtual address of kernel stack
	struct trapframe *trapframe; // data page for trampoline.S
	struct context context;      // swtch() here to run process
	uint64 max_page;
	struct proc *parent;         // Parent process
	uint64 exit_code;
	struct file *files[FD_BUFFER_SIZE];

	// ---- Stride scheduling fields (Project 3) ----
	uint64 priority;             // Process priority (>= 2). Default 16.
	uint64 stride;               // Accumulated "distance" run so far.
	uint64 pass;                 // Amount added to stride per schedule = BIG_STRIDE / priority.

	uint64 start_time;                      // ADD THIS
    uint32 syscall_times[MAX_SYSCALL_NUM];  // ADD THIS
    int status;                             // ADD THIS if not there
};


int cpuid();
struct proc *curr_proc();
void exit(int);
void proc_init();
void scheduler() __attribute__((noreturn));
void sched();
void yield();
int fork();
int exec(char *);
int wait(int, int *);
void add_task(struct proc *);
struct proc *pop_task();
struct proc *allocproc();
int fdalloc(struct file *);
// swtch.S
void swtch(struct context *, struct context *);

#endif // PROC_H