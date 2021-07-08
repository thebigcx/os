#include <sched/sched.h>
#include <mem/paging.h>
#include <intr/idt.h>
#include <drivers/input/keyboard/ps2_keyboard.h>
#include <drivers/input/mouse/ps2_mouse.h>
#include <util/elf.h>
#include <drivers/fs/vfs/vfs.h>
#include <util/types.h>
#include <util/stdlib.h>
#include <mem/kheap.h>
#include <sys/system.h>
#include <sched/spinlock.h>
#include <intr/apic.h>
#include <time/time.h>
#include <cpu/cpu.h>
#include <drivers/gfx/fb/fb.h>

void ctx_switch(reg_ctx_t* regs, uint64_t pml4);

int scheduler_ready = 0;

tree_t* proctree;
list_t* procs;
list_t* proc_queue;
list_t* sleep_lst;
proc_t* currproc = NULL;

extern void kernel_proc();

int creatpid()
{
    static int pid = 0;
    return pid++;
}

void idle()
{
    while (1);
}

void sched_init()
{
    proc_queue = list_create();
    sleep_lst = list_create();
    procs = list_create();
    proctree = tree_create();
}

void sched_start()
{
    idt_set_int(IPI_SCHED, schedule); // Software interrupt for schedule (0xfd)

    //idle_proc = mk_proc(idle);

    scheduler_ready = 1;
    
    sti();
    for (;;);
}

void schedule(reg_ctx_t* r)
{
    if (proc_queue->head != NULL)
    {
        if (currproc)
        {
            currproc->regs = *r;
            // If it hasn't blocked or is sleeping, add it back to the ready list
            if (currproc->state == PROC_STATE_RUNNING)
            {
                currproc->state = PROC_STATE_READY;
                list_push_back(proc_queue, currproc);
            }
        }

        currproc = list_pop_front(proc_queue);
        currproc->state = PROC_STATE_RUNNING;

        ctx_switch(&(currproc->regs), (uint64_t)currproc->addr_space->pml4_phys);
    }
}

proc_t* mk_proc(void* entry)
{
    proc_t* proc = kmalloc(sizeof(proc_t));
    proc->state = PROC_STATE_READY;
    proc->addr_space = page_mk_map();
    proc->pid = creatpid();
    proc->sleep_exp = 0;
    proc->file_descs = list_create();
    strcpy(proc->name, "unknown");

    void* stack = page_kernel_alloc4k(16);
    for (uint32_t i = 0; i < 16; i++)
    {
        page_kernel_map_memory(stack + i * PAGE_SIZE_4K, pmm_request(), 1);
    }

    memset(&(proc->regs), 0, sizeof(reg_ctx_t));
    proc->regs.rip = (uint64_t)entry;
    proc->regs.rflags = RFLAG_INTR | 0x2; // Interrupts, reserved
    proc->regs.cs = KERNEL_CS;
    proc->regs.ss = KERNEL_SS;
    proc->regs.rbp = (uint64_t)stack + 0x16000;
    proc->regs.rsp = (uint64_t)stack + 0x16000;

    return proc;
}

void sched_tick(reg_ctx_t* r)
{
    if (!scheduler_ready) return;

    /*cli();

    proc_t* next = sleep_tsk_lst;
    proc_t* this;
    sleep_tsk_lst = NULL;

    while (next != NULL)
    {
        this = next;
        next = this->next;

        if (this->sleep_exp <= pit_uptime() * 1000)
        {
            sched_unblock(this);
        }
        else
        {
            this->next = sleep_tsk_lst;
            sleep_tsk_lst = this;
        }
    }

    sti();*/

    schedule(r);
}

void sched_spawn(proc_t* proc, proc_t* parent)
{
    cli();
    //acquire_lock(ready_lst_lock);

    list_push_back(proc_queue, proc);
    list_push_back(procs, proc);
    //if (parent)
        //proc->treenode = tree_insert(proctree, parent->treenode, proc);
    //else
        proc->treenode = tree_insert(proctree, proctree->root, proc);

    sti();
    //release_lock(ready_lst_lock);
}

void sched_terminate()
{
    /*sched_lock();

    currproc->next = kill_tsk_lst;
    kill_tsk_lst = currproc;

    sched_unlock();

    sched_block(PROC_STATE_KILLED);

    sched_unblock();*/
}

void sched_proc_destroy(proc_t* proc)
{
    uint32_t files = proc->file_descs->cnt;
    for (uint32_t i = 0; i < files; i++)
    {
        fs_fd_t* fd = list_pop_front(proc->file_descs);
        vfs_close(fd);
    }

    page_destroy_map(proc->addr_space);

    kfree(proc);
}

void sched_block(uint32_t state)
{
    cli();

    currproc->state = state;
    //lapic_send_ipi(0, ICR_ALL_EX_SELF, ICR_FIXED, IPI_SCHED);
    lapic_send_ipi(0, ICR_SELF, ICR_FIXED, IPI_SCHED);

    sti();
}

void sched_unblock(proc_t* proc)
{
    cli();

    list_push_back(proc_queue, proc);
    proc->state = PROC_STATE_READY;

    sti();
}

void sched_fork(proc_t* proc)
{
    proc_t* new = kmalloc(sizeof(proc_t));
    new->addr_space = page_clone_map(proc->addr_space);
    new->pid = creatpid();
    new->sleep_exp = 0;
    new->file_descs = list_create();
    new->state = proc->state;
    strcpy(new->name, proc->name);
    
    memcpy(&new->regs, &proc->regs, sizeof(reg_ctx_t));
    strcpy(new->working_dir, proc->working_dir);

    list_foreach(proc->file_descs, node)
    {
        fs_fd_t* fd = node->val;
        fs_fd_t* newfd; // vfs_node_t does not need to be copied
        memcpy(newfd, fd, sizeof(fs_fd_t));
        list_push_back(new->file_descs, newfd);
    }

    // Child processes get a new stack
    space_alloc_region_at(0x20000, 0x4000, proc->addr_space);
    sched_spawn(new, proc);
}

proc_t* sched_get_currproc()
{
    return currproc;
}

static void nano_sleep_until(uint64_t t)
{
    cli();

    if (t < pit_uptime() * 1000)
    {
        sti();
        return;
    }

    currproc->sleep_exp = t;
    list_push_back(sleep_lst, currproc);

    sti();

    sched_block(PROC_STATE_SLEEP);
}

void nano_sleep(uint64_t ns)
{
    nano_sleep_until(pit_uptime() * 1000 + ns);
}

void sleep(uint64_t s)
{
    nano_sleep(s * 1000000000);
}

bool check_elf_hdr(elf64_hdr_t* hdr)
{
    if (hdr->ident[0] != ELFMAG0 ||
        hdr->ident[1] != ELFMAG1 ||
        hdr->ident[2] != ELFMAG2 ||
        hdr->ident[3] != ELFMAG3)
    {
        
        return false;
    }

    if (hdr->ident[EI_CLASS] != ELFCLASS64) return false;

    return true;
}

void* loadelf(uint8_t* elf_dat, proc_t* proc)
{
    elf64_hdr_t hdr;

    memcpy(&hdr, elf_dat, sizeof(elf64_hdr_t));

    if (!check_elf_hdr(&hdr))
        return NULL;

    for (uint16_t i = 0; i < hdr.ph_num; i++)
    {
        elf64_phdr_t phdr;
        memcpy(&phdr, elf_dat + hdr.ph_off + hdr.ph_ent_size * i, sizeof(elf64_phdr_t));

        if (phdr.type == PT_LOAD)
        {
            uint64_t begin = phdr.vaddr;
            uint64_t size = phdr.mem_sz; // TODO: fix

            if (size % PAGE_SIZE_4K != 0)
                size = size - (size % PAGE_SIZE_4K) + PAGE_SIZE_4K;

            space_alloc_region_at(begin, size / PAGE_SIZE_4K, proc->addr_space);

            void* tmp = page_kernel_alloc4k(1);

            for (uint32_t i = 0; i < size; i += PAGE_SIZE_4K)
            {
                void* phys = pmm_request();
                page_kernel_map_memory(tmp, phys, 1);
                memcpy(tmp, elf_dat + phdr.offset + i, PAGE_SIZE_4K);
                page_map_memory(phdr.vaddr + i, phys, 1, proc->addr_space);
            }

            page_kernel_free4k(tmp, 1);
        }
    }

    proc->regs.rip = hdr.entry;
}

// Prepare the stack of a process, by pushing the necessary variables (argc, argv, env, envp)
static void prepstack(proc_t* proc, const char* file, int argc, char** argv, int envp, char** env)
{
    uint64_t stack = proc->regs.rsp;

    uint64_t* tmp_argv = kmalloc((argc + 1) * sizeof(uint64_t)); // First arg is exec path

    // Push 'args' onto the stack (load proc's virtual address space)
    uint64_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3));

    cli();
    asm volatile ("mov %%rax, %%cr3" :: "a"(proc->addr_space->pml4_phys));

    stack -= strlen(file) + 1;
    strcpy((char*)stack, file);
    tmp_argv[0] = stack;

    for (int i = 0; i < argc; i++)
    {
        stack -= strlen(argv[i]) + 1;
        strcpy(stack, argv[i]);
        tmp_argv[i + 1] = stack;
    }

    for (int i = argc; i >= 0; i--)
    {
        stack -= sizeof(uint64_t);
        *((uint64_t*)stack) = tmp_argv[i];
        serial_printf("d\n");
    }

    asm volatile ("mov %%rax, %%cr3" :: "a"(cr3));
    sti();

    proc->regs.rsp = stack;
    proc->regs.rsi = stack;
    
    kfree(tmp_argv);
}

proc_t* mkelfproc(const char* path, int argc, char** argv, int envp, char** env)
{
    vfs_node_t* node = vfs_resolve_path(path, NULL);
    uint8_t* buffer = kmalloc(node->size);
    vfs_read(node, buffer, 0, node->size);

    proc_t* proc = kmalloc(sizeof(proc_t));

    proc->pid = creatpid();
    proc->sleep_exp = 0;
    proc->state = PROC_STATE_READY;
    proc->addr_space = page_mk_map(); // Creates a virtual address space with kernel mapped
    proc->file_descs = list_create();
    strcpy(proc->name, "unknown");

    // TODO: These are temporary - later will be hooked up to PTYs
    vfs_node_t* stdin_node = vfs_resolve_path("/dev/stdout", NULL);
    fs_fd_t* stdin = vfs_open(stdin_node, 0, 0);
    list_push_back(proc->file_descs, stdin);

    vfs_node_t* stdout_node = vfs_resolve_path("/dev/stdout", NULL);
    fs_fd_t* stdout = vfs_open(stdout_node, 0, 0);
    list_push_back(proc->file_descs, stdout);

    space_alloc_region_at(0x20000, 0x4000, proc->addr_space);
    for (uint32_t i = 0; i < 0x4000; i += PAGE_SIZE_4K)
    {
        page_map_memory(0x20000 + i, pmm_request(), 1, proc->addr_space);
    }
    
    memset(&(proc->regs), 0, sizeof(reg_ctx_t));
    loadelf(buffer, proc);
    proc->regs.rflags = RFLAG_INTR | 0x2; // Interrupts, reserved
    proc->regs.cs = KERNEL_CS;
    proc->regs.ss = KERNEL_SS;
    proc->regs.rbp = (uint64_t)0x24000;
    proc->regs.rsp = (uint64_t)0x24000;

    // TODO: this does not work without usermode. PLEASE FIX!!!!
    //prepstack(proc, path, argc, argv, envp, env);
    
    kfree(buffer);

    return proc;
}

void sched_exec(const char* path, int argc, char** argv)
{
    proc_t* proc = mkelfproc(path, argc, argv, 0, NULL);
    sched_spawn(proc, NULL);
    sched_block(PROC_STATE_PAUSED); // TODO: terminate
}

sem_t* sem_create(uint32_t max_cnt)
{
    sem_t* sem = kmalloc(sizeof(sem_t));
    sem->curr_cnt = 0;
    sem->max_cnt = max_cnt;
    sem->waitlst = list_create();
    return sem;
}

void sem_acquire(sem_t* sem)
{
    if (sem->curr_cnt < sem->max_cnt)
    {
        sem->curr_cnt++;
    }
    else
    {
        list_push_back(sem->waitlst, currproc);
        sched_block(PROC_STATE_WAIT_LOCK);
    }
}

void sem_release(sem_t* sem)
{
    if (sem->waitlst->head != NULL)
        sched_unblock(list_pop_front(sem->waitlst));
    else
        sem->curr_cnt--;
}

mutex_t* mutex_create()
{
    mutex_t* mutex = kmalloc(sizeof(mutex_t));
    mutex->waitlst = list_create();
    mutex->proc = 0;
    return mutex;
}

void mutex_acquire(mutex_t* mutex)
{
    if (!mutex->proc)
    {
        mutex->proc = 1;
    }
    else
    {
        list_push_back(mutex->waitlst, currproc);
        sched_block(PROC_STATE_WAIT_LOCK);
    }
}

void mutex_release(mutex_t* mutex)
{
    if (mutex->waitlst->head != NULL)
        sched_unblock(list_pop_front(mutex->waitlst));
    else
        mutex->proc = 0;
}