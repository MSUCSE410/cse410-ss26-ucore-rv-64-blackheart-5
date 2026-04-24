#include "console.h"
#include "defs.h"
#include "loader.h"
#include "sync.h"
#include "syscall.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
// In os/syscall.h or similar
#define SYS_enable_deadlock_detect 469

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
	case FD_PIPE:
		return pipewrite(f->pipe, va, len);
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
	case FD_PIPE:
		return piperead(f->pipe, va, len);
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
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
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

uint64 sys_pipe(uint64 fdarray)
{
	struct proc *p = curr_proc();
	uint64 fd0, fd1;
	struct file *f0, *f1;
	if (f0 < 0 || f1 < 0) {
		return -1;
	}
	f0 = filealloc();
	f1 = filealloc();
	if (pipealloc(f0, f1) < 0)
		goto err0;
	fd0 = fdalloc(f0);
	fd1 = fdalloc(f1);
	if (fd0 < 0 || fd1 < 0)
		goto err0;
	if (copyout(p->pagetable, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
	    copyout(p->pagetable, fdarray + sizeof(uint64), (char *)&fd1,
		    sizeof(fd1)) < 0) {
		goto err1;
	}
	return 0;

err1:
	p->files[fd0] = 0;
	p->files[fd1] = 0;
err0:
	fileclose(f0);
	fileclose(f1);
	return -1;
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

int sys_thread_create(uint64 entry, uint64 arg)
{
	struct proc *p = curr_proc();
	int tid = allocthread(p, entry, 1);
	if (tid < 0) {
		errorf("fail to create thread");
		return -1;
	}
	struct thread *t = &p->threads[tid];
	t->trapframe->a0 = arg;
	t->state = RUNNABLE;
	add_task(t);
	return tid;
}

int sys_gettid()
{
	return curr_thread()->tid;
}

int sys_waittid(int tid)
{
	if (tid < 0 || tid >= NTHREAD) {
		errorf("unexpected tid %d", tid);
		return -1;
	}
	struct thread *t = &curr_proc()->threads[tid];
	if (t->state == T_UNUSED || tid == curr_thread()->tid) {
		return -1;
	}
	if (t->state != EXITED) {
		return -2;
	}
	memset((void *)t->kstack, 7, KSTACK_SIZE);
	t->tid = -1;
	t->state = T_UNUSED;
	return t->exit_code;
}

/*
*	LAB5: (3) In the TA's reference implementation, here defines funtion
*					int deadlock_detect(const int available[LOCK_POOL_SIZE],
*						const int allocation[NTHREAD][LOCK_POOL_SIZE],
*						const int request[NTHREAD][LOCK_POOL_SIZE])
*				for both mutex and semaphore detect, you can also
*				use this idea or just ignore it.
*/

// Banker's algorithm
// 1. work[] = available[]; finish all threads with no requests
// 2. find thread i where request[i] <= work[]; simulate it finishing: work += allocation[i]
// 3. if any thread still unfinished → deadlock
static int deadlock_detect(const int available[LOCK_POOL_SIZE],
                            const int allocation[NTHREAD][LOCK_POOL_SIZE],
                            const int request[NTHREAD][LOCK_POOL_SIZE])
{
    int work[LOCK_POOL_SIZE]; // Copy of available resources (we simulate on this)
    int finish[NTHREAD]; // Tracks if each thread can finish (1 = done, 0 = not done)

	 // Initialize work[] with currently available resources
    for (int i = 0; i < LOCK_POOL_SIZE; i++)
        work[i] = available[i];
    
	// Initially mark all threads as not finished
	for (int i = 0; i < NTHREAD; i++)
        finish[i] = 0;


	// Check which threads are not waiting for any resources
    // If a thread has no requests, it can finish immediately
    for (int i = 0; i < NTHREAD; i++) {
        int waiting = 0; // Flag to check if thread is waiting for something
        
		//loop to check if this thread is requesting any resource
		for (int j = 0; j < LOCK_POOL_SIZE; j++)
            if (request[i][j] > 0) { waiting = 1; break; }
        if (!waiting) finish[i] = 1;
    }

    // Try to find a safe sequence where threads can finish one by one
    int changed = 1;  // keeps track if we made progress in an iteration
    while (changed) {
        changed = 0; // reset iteration tracker each loop

		// loop through each thread
        for (int i = 0; i < NTHREAD; i++) {
            if (finish[i]) continue; // skip if finished 
            // Can thread’s requests can be satisfied with current work[], assume 1 initially-->it can
            int can = 1;
            
			
			for (int j = 0; j < LOCK_POOL_SIZE; j++)
                if (request[i][j] > work[j]) { can = 0; break; }
            
			// If the thread can run
			if (can) {
                // Simulate this thread finishing and releasing resources
                for (int j = 0; j < LOCK_POOL_SIZE; j++)
                    work[j] += allocation[i][j];
                finish[i] = 1; // Mark thread as finished
                changed = 1;  // We made progress with this thread
            }
        }
    }

    // If any thread is not finished, we have a deadlock
	// check if any thread is still unfinished
    for (int i = 0; i < NTHREAD; i++)
        if (!finish[i]) return -1; // deadlock detected
    return 0; // no deasdlock
}


// sys_mutex_create: initialize mutex_available[id] = 1
// sys_mutex_lock: if detect on AND mutex unavailable → run Banker's → return -0xDEAD if deadlock
//   skip current thread's own allocation when building adj_available (prevents self-deadlock false negative)
//   after acquiring: allocation[tid][id]++, available[id]--
// sys_mutex_unlock: allocation[tid][id]--, available[id]++, then unlock
int sys_mutex_create(int blocking)
{
	struct mutex *m = mutex_create(blocking);
	if (m == NULL) {
		errorf("fail to create mutex: out of resource");
		return -1;
	}
	
	// LAB5: (4-1) You may want to maintain some variables for detect here
	int mutex_id = m - curr_proc()->mutex_pool;
    // LAB5: (4-1)
    curr_proc()->mutex_available[mutex_id] = 1; // this mutex is available (1 == free, not locked)
    debugf("create mutex %d", mutex_id);
    return mutex_id;
}



int sys_mutex_lock(int mutex_id)
{
	if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
		errorf("Unexpected mutex id %d", mutex_id);
		return -1;
	}
	// LAB5: (4-1) You may want to maintain some variables for detect
	//       or call your detect algorithm here
	struct proc *p = curr_proc();

	// If deadlock detection is enabled AND mutex is currently unavailable (locked)
    if (p->deadlock_detect && p->mutex_available[mutex_id] == 0) {
        int tid = curr_thread()->tid;  // Get current thread ID
        int adj_available[LOCK_POOL_SIZE]; // a temp "available" array for simulation
        
		// Copy current available resources into adj_available
		for (int i = 0; i < LOCK_POOL_SIZE; i++)
            adj_available[i] = p->mutex_available[i];

		// Adjust available resources based on other running threads
        for (int i = 0; i < NTHREAD; i++) {
            if (i == tid) continue; // skip current thread, self-deadlock detection
            struct thread *t = &p->threads[i]; // Get thread i
            if (t->state == T_UNUSED || t->state == EXITED)
                continue;
            if (t->state != SLEEPING) {
				// If thread is running, assume it will release its resources
                for (int j = 0; j < LOCK_POOL_SIZE; j++)
                    adj_available[j] += p->mutex_allocation[i][j];
            }
        }

        p->mutex_request[tid][mutex_id] = 1; // Mark that current thread is requesting this mutex
        
		// check deadlock detection algorithm
		if (deadlock_detect(adj_available,
                            p->mutex_allocation,
                            p->mutex_request) < 0) {
            p->mutex_request[tid][mutex_id] = 0;
            errorf("detect deadlock on locking mutex %d!", mutex_id);
            return -0xDEAD;
        }
        p->mutex_request[tid][mutex_id] = 0;
    }
	// lock the mutex (may block if unavailable)
    mutex_lock(&p->mutex_pool[mutex_id]);
    if (p->deadlock_detect) {
        int tid = curr_thread()->tid;
        p->mutex_allocation[tid][mutex_id]++; // Record that this thread now holds the mutex
        p->mutex_available[mutex_id]--;  // Decrease available count since mutex is now taken
    }
    return 0;
}

int sys_mutex_unlock(int mutex_id)
{
	if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
		errorf("Unexpected mutex id %d", mutex_id);
		return -1;
	}
	// LAB5: (4-1) You may want to maintain some variables for detect here
	struct proc *p = curr_proc();
    // LAB5: (4-1)
    if (p->deadlock_detect) {
        int tid = curr_thread()->tid;
        p->mutex_allocation[tid][mutex_id]--; // decrease allocation since we releasing this mutex
        p->mutex_available[mutex_id]++; // ++ available count since mutex is now free
    }
    mutex_unlock(&p->mutex_pool[mutex_id]);
    return 0;
}

int sys_semaphore_create(int res_count)
{
	struct semaphore *s = semaphore_create(res_count);
	if (s == NULL) {
		errorf("fail to create semaphore: out of resource");
		return -1;
	}
	// LAB5: (4-2) You may want to maintain some variables for detect here
	int sem_id = s - curr_proc()->semaphore_pool;  // calc semaphore ID by finding its index in the semaphore pool
	curr_proc()->sem_available[sem_id] = res_count; // set to available resources
    debugf("create semaphore %d", sem_id);
    return sem_id;
}

int sys_semaphore_up(int semaphore_id)
{
	if (semaphore_id < 0 ||
	    semaphore_id >= curr_proc()->next_semaphore_id) {
		errorf("Unexpected semaphore id %d", semaphore_id);
		return -1;
	}
	// LAB5: (4-2) You may want to maintain some variables for detect here
	struct proc *p = curr_proc();
    if (p->deadlock_detect) {
        int tid = curr_thread()->tid;
        if (p->sem_allocation[tid][semaphore_id] > 0)//thread has resources
            p->sem_allocation[tid][semaphore_id]--;//dec alloc cause resource is being released
    }
    semaphore_up(&p->semaphore_pool[semaphore_id]); // call to do the actual semaphore "up" (signal/release)
    return 0;
}

int sys_semaphore_down(int semaphore_id)
{
	if (semaphore_id < 0 ||
	    semaphore_id >= curr_proc()->next_semaphore_id) {
		errorf("Unexpected semaphore id %d", semaphore_id);
		return -1;
	}
	// LAB5: (4-2) You may want to maintain some variables for detect
	//       or call your detect algorithm here
	struct proc *p = curr_proc();
    struct semaphore *s = &p->semaphore_pool[semaphore_id];
    if (p->deadlock_detect && s->count <= 0) {
        int tid = curr_thread()->tid;
        int available[LOCK_POOL_SIZE]; // current available resources array
        
		// loop fill available[] with semaphore counts (only positive values)
		for (int i = 0; i < LOCK_POOL_SIZE; i++) {
            available[i] = p->semaphore_pool[i].count > 0 ?
                           p->semaphore_pool[i].count : 0;
        }
        int adj_available[LOCK_POOL_SIZE]; //temp arr for simulation
        for (int i = 0; i < LOCK_POOL_SIZE; i++)//copy from available to temp arr
            adj_available[i] = available[i];
        
		//loop other threads
		for (int i = 0; i < NTHREAD; i++) {
            if (i == tid) continue; // skip current thread to avoid self-deadlock
            struct thread *t = &p->threads[i];
            if (t->state == T_UNUSED || t->state == EXITED)
                continue;
            if (t->state != SLEEPING) {
                for (int j = 0; j < LOCK_POOL_SIZE; j++)
                    adj_available[j] += p->sem_allocation[i][j];
            }
        }


        p->sem_request[tid][semaphore_id] = 1; // thread is requesting this semaphore
        if (deadlock_detect(adj_available, p->sem_allocation, p->sem_request) < 0) {
            p->sem_request[tid][semaphore_id] = 0;
            errorf("detect deadlock on down semaphore %d!", semaphore_id);
            return -0xDEAD;
        }
        p->sem_request[tid][semaphore_id] = 0;
    }
	// Check if this operation will block (no resources available)
    int will_block = (s->count <= 0);
    semaphore_down(s); // call to semaphore actual down (may block if count <= 0)
    if (p->deadlock_detect && !will_block) {
        int tid = curr_thread()->tid;
        p->sem_allocation[tid][semaphore_id]++; //threads has 1 resource from semaphore
    }
    return 0;
}

int sys_condvar_create()
{
	struct condvar *c = condvar_create();
	if (c == NULL) {
		errorf("fail to create condvar: out of resource");
		return -1;
	}
	int cond_id = c - curr_proc()->condvar_pool;
	debugf("create condvar %d", cond_id);
	return cond_id;
}

int sys_condvar_signal(int cond_id)
{
	if (cond_id < 0 || cond_id >= curr_proc()->next_condvar_id) {
		errorf("Unexpected condvar id %d", cond_id);
		return -1;
	}
	cond_signal(&curr_proc()->condvar_pool[cond_id]);
	return 0;
}

int sys_condvar_wait(int cond_id, int mutex_id)
{
	if (cond_id < 0 || cond_id >= curr_proc()->next_condvar_id) {
        errorf("Unexpected condvar id %d", cond_id);
        return -1;
    }
    if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
        errorf("Unexpected mutex id %d", mutex_id);
        return -1;
    }
    // Update tracking before unlock
    struct proc *p = curr_proc();
    if (p->deadlock_detect) {
        int tid = curr_thread()->tid;
        p->mutex_allocation[tid][mutex_id]--;
        p->mutex_available[mutex_id]++;
    }
    cond_wait(&curr_proc()->condvar_pool[cond_id],
              &curr_proc()->mutex_pool[mutex_id]);
    // Update tracking after re-lock
    if (p->deadlock_detect) {
        int tid = curr_thread()->tid;
        p->mutex_allocation[tid][mutex_id]++;
        p->mutex_available[mutex_id]--;
    }
    return 0;
}

// LAB5: (2) you may need to define function enable_deadlock_detect here

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_thread()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	if (id != SYS_write && id != SYS_read && id != SYS_sched_yield) {
		debugf("syscall %d args = [%x, %x, %x, %x, %x, %x]", id,
		       args[0], args[1], args[2], args[3], args[4], args[5]);
	}
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
	// case SYS_nanosleep:
	// 	ret = sys_nanosleep(args[0]);
	// 	break;
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
	case SYS_pipe2:
		ret = sys_pipe(args[0]);
		break;
	case SYS_thread_create:
		ret = sys_thread_create(args[0], args[1]);
		break;
	case SYS_gettid:
		ret = sys_gettid();
		break;
	case SYS_waittid:
		ret = sys_waittid(args[0]);
		break;
	case SYS_mutex_create:
		ret = sys_mutex_create(args[0]);
		break;
	case SYS_mutex_lock:
		ret = sys_mutex_lock(args[0]);
		break;
	case SYS_mutex_unlock:
		ret = sys_mutex_unlock(args[0]);
		break;
	case SYS_semaphore_create:
		ret = sys_semaphore_create(args[0]);
		break;
	case SYS_semaphore_up:
		ret = sys_semaphore_up(args[0]);
		break;
	case SYS_semaphore_down:
		ret = sys_semaphore_down(args[0]);
		break;
	case SYS_condvar_create:
		ret = sys_condvar_create();
		break;
	case SYS_condvar_signal:
		ret = sys_condvar_signal(args[0]);
		break;
	case SYS_condvar_wait:
		ret = sys_condvar_wait(args[0], args[1]);
		break;
	// LAB5: (2) you may need to add case SYS_enable_deadlock_detect here
	case SYS_enable_deadlock_detect:
		ret = sys_enable_deadlock_detect(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	curr_thread()->trapframe->a0 = ret;
	if (id != SYS_write && id != SYS_read && id != SYS_sched_yield) {
		debugf("syscall %d ret %d", id, ret);
	}
}
