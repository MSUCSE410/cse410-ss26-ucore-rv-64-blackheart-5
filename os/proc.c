#include "proc.h"
#include "defs.h"
#include "loader.h"
#include "trap.h"
#include "vm.h"
#include "queue.h"

struct proc pool[NPROC];
__attribute__((aligned(16))) char kstack[NPROC][PAGE_SIZE];
__attribute__((aligned(4096))) char trapframe[NPROC][TRAP_PAGE_SIZE];

extern char boot_stack_top[];
struct proc *current_proc;
struct proc idle;
struct queue task_queue;

int threadid()
{
	return curr_proc()->pid;
}

struct proc *curr_proc()
{
	return current_proc;
}

// initialize the proc table at boot time.
void proc_init()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		p->state = UNUSED;
		p->kstack = (uint64)kstack[p - pool];
		p->trapframe = (struct trapframe *)trapframe[p - pool];
	}
	idle.kstack = (uint64)boot_stack_top;
	idle.pid = IDLE_PID;
	current_proc = &idle;
	init_queue(&task_queue);
}

int allocpid()
{
	static int PID = 1;
	return PID++;
}

// Stride scheduler: pick the RUNNABLE process with the smallest stride.
struct proc *fetch_task()
{
	// If the queue is empty, nothing to run.
	if (task_queue.empty) {
		debugf("No task to fetch\n");
		return NULL;
	}

	// Walk the queue from front to tail to find the process with
	// the smallest stride. The queue is circular, so indices wrap.
	int best_qidx = -1;          // Position within the queue array
	int best_proc_idx = -1;      // Index into the pool[] array
	uint64 best_stride = 0;      // Smallest stride seen so far

	int i = task_queue.front;
	do {
		int proc_idx = task_queue.data[i];
		struct proc *p = &pool[proc_idx];

		// Only consider processes that are still RUNNABLE.
		// (A process in the queue should be RUNNABLE, but be safe.)
		if (p->state == RUNNABLE) {
			if (best_qidx == -1 || p->stride < best_stride) {
				best_qidx = i;
				best_proc_idx = proc_idx;
				best_stride = p->stride;
			}
		}
		i = (i + 1) % QUEUE_SIZE;
	} while (i != task_queue.tail);

	// No runnable process found in the queue.
	if (best_qidx == -1) {
		return NULL;
	}

	// Remove the chosen entry from the queue by shifting later entries
	// backward one slot. This preserves FIFO order among equal strides.
	int cur = best_qidx;
	while (cur != task_queue.tail) {
		int next = (cur + 1) % QUEUE_SIZE;
		if (next == task_queue.tail) break;
		task_queue.data[cur] = task_queue.data[next];
		cur = next;
	}
	// Move tail back by one to drop the now-duplicated last entry.
	task_queue.tail = (task_queue.tail - 1 + QUEUE_SIZE) % QUEUE_SIZE;
	if (task_queue.front == task_queue.tail) {
		task_queue.empty = 1;
	}

	struct proc *chosen = &pool[best_proc_idx];

	// Advance the scheduled process's stride by its pass value.
	// Recompute pass in case priority changed via sys_set_priority.
	chosen->pass = BIG_STRIDE / chosen->priority;
	chosen->stride += chosen->pass;

	debugf("stride sched: pick pid=%d stride(after)=%lld prio=%lld\n",
	       chosen->pid, chosen->stride, chosen->priority);
	return chosen;
}

void add_task(struct proc *p)
{
	push_queue(&task_queue, p - pool);
	debugf("add task %d(pid=%d) to task queue\n", p - pool, p->pid);
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel.
// If there are no free procs, or a memory allocation fails, return 0.
struct proc *allocproc()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		if (p->state == UNUSED) {
			goto found;
		}
	}
	return 0;

found:
	// init proc
	p->pid = allocpid();
	p->state = USED;
	p->ustack = 0;
	p->max_page = 0;
	p->parent = NULL;
	p->exit_code = 0;
	p->pagetable = uvmcreate((uint64)p->trapframe);
	memset(&p->context, 0, sizeof(p->context));
	memset((void *)p->kstack, 0, KSTACK_SIZE);
	memset((void *)p->trapframe, 0, TRAP_PAGE_SIZE);
	p->context.ra = (uint64)usertrapret;
	p->context.sp = p->kstack + KSTACK_SIZE;

	// ---- Stride scheduling init (Project 3) ----
	// Per the spec: initial stride = 0, initial priority = 16.
	// pass = BIG_STRIDE / priority so higher priority => smaller pass
	// => smaller increments => scheduled more often.
	p->priority = DEFAULT_PRIORITY;
	p->stride = 0;
	p->pass = BIG_STRIDE / p->priority;

	return p;
}

// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler()
{
	struct proc *p;
	for (;;) {
		/*int has_proc = 0;
		for (p = pool; p < &pool[NPROC]; p++) {
			if (p->state == RUNNABLE) {
				has_proc = 1;
				tracef("swtich to proc %d", p - pool);
				p->state = RUNNING;
				current_proc = p;
				swtch(&idle.context, &p->context);
			}
		}
		if(has_proc == 0) {
			panic("all app are over!\n");
		}*/
		p = fetch_task();
		if (p == NULL) {
			panic("all app are over!\n");
		}
		tracef("swtich to proc %d", p - pool);
		p->state = RUNNING;
		current_proc = p;
		swtch(&idle.context, &p->context);
	}
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched()
{
	struct proc *p = curr_proc();
	if (p->state == RUNNING)
		panic("sched running");
	swtch(&p->context, &idle.context);
}

// Give up the CPU for one scheduling round.
void yield()
{
	current_proc->state = RUNNABLE;
	add_task(current_proc);
	sched();
}

// Free a process's page table, and free the
// physical memory it refers to.
void freepagetable(pagetable_t pagetable, uint64 max_page)
{
	uvmunmap(pagetable, TRAMPOLINE, 1, 0);
	uvmunmap(pagetable, TRAPFRAME, 1, 0);
	uvmfree(pagetable, max_page);
}

void freeproc(struct proc *p)
{
	if (p->pagetable)
		freepagetable(p->pagetable, p->max_page);
	p->pagetable = 0;
	p->state = UNUSED;
}

int fork()
{
	struct proc *np;
	struct proc *p = curr_proc();
	// Allocate process.
	if ((np = allocproc()) == 0) {
		panic("allocproc\n");
	}
	// Copy user memory from parent to child.
	if (uvmcopy(p->pagetable, np->pagetable, p->max_page) < 0) {
		panic("uvmcopy\n");
	}
	np->max_page = p->max_page;
	// copy saved user registers.
	*(np->trapframe) = *(p->trapframe);
	// Cause fork to return 0 in the child.
	np->trapframe->a0 = 0;
	np->parent = p;
	np->state = RUNNABLE;
	add_task(np);
	return np->pid;
}

int exec(char *name)
{
	int id = get_id_by_name(name);
	if (id < 0)
		return -1;
	struct proc *p = curr_proc();
	uvmunmap(p->pagetable, 0, p->max_page, 1);
	p->max_page = 0;
	loader(id, p);
	return 0;
}

int wait(int pid, int *code)
{
	struct proc *np;
	int havekids;
	struct proc *p = curr_proc();

	for (;;) {
		// Scan through table looking for exited children.
		havekids = 0;
		for (np = pool; np < &pool[NPROC]; np++) {
			if (np->state != UNUSED && np->parent == p &&
			    (pid <= 0 || np->pid == pid)) {
				havekids = 1;
				if (np->state == ZOMBIE) {
					// Found one.
					np->state = UNUSED;
					pid = np->pid;
					*code = np->exit_code;
					return pid;
				}
			}
		}
		if (!havekids) {
			return -1;
		}
		p->state = RUNNABLE;
		add_task(p);
		sched();
	}
}

// Exit the current process.
void exit(int code)
{
	struct proc *p = curr_proc();
	p->exit_code = code;
	debugf("proc %d exit with %d\n", p->pid, code);
	freeproc(p);
	if (p->parent != NULL) {
		// Parent should `wait`
		p->state = ZOMBIE;
	}
	// Set the `parent` of all children to NULL
	struct proc *np;
	for (np = pool; np < &pool[NPROC]; np++) {
		if (np->parent == p) {
			np->parent = NULL;
		}
	}
	sched();
}