/* Services and the native compatibility domain have separate locks. Native
 * address-space changes use an acknowledged rendezvous before reusing pages.
 * No lock is held while applications execute in parallel in ring 3. */
#define CPU_MAX 8
typedef struct {
    u64 task,stack,user_rsp,index,idle_stack;
    volatile u32 online,in_user;u32 apic_id,compat_owned,vm_barriers;
    Tss tss;u64 gdt[7];
} Cpu;
static Cpu cpus[CPU_MAX];
volatile u32 cpu_count=1,cpu_online=1;
volatile u64 cpu_user_returns[CPU_MAX],cpu_rendezvous[CPU_MAX],cpu_fast_calls[CPU_MAX],cpu_parallel_service_calls;
static volatile u32 compatibility_lock,ipc_locks[TASK_COUNT];
static int task_cpu[TASK_COUNT];
static u32 task_affinity[TASK_COUNT];
static Cpu *cpu_local(void){u64 index;__asm__ volatile("mov %%gs:24,%0":"=r"(index));return &cpus[index];}
#define current_task (cpu_local()->task)
#define kernel_stack_top (cpu_local()->stack)
static void spin_lock(volatile u32 *lock){while(__atomic_exchange_n(lock,1,__ATOMIC_ACQUIRE))while(__atomic_load_n(lock,__ATOMIC_RELAXED))__asm__ volatile("pause");}
static void spin_unlock(volatile u32 *lock){__atomic_store_n(lock,0,__ATOMIC_RELEASE);}
static void cpu_ipi(u32 destination,u32 value);
static void compatibility_enter(void){
    Cpu *self=cpu_local();if(self->compat_owned)return;
    __atomic_store_n(&self->in_user,0,__ATOMIC_RELEASE);
    spin_lock(&compatibility_lock);self->compat_owned=1;self->vm_barriers=0;
}
void cpu_release(void){
    Cpu *self=cpu_local();
    if(self->task<TASK_COUNT&&native_active[self->task]){
        cpu_user_returns[self->index]++;__atomic_store_n(&self->in_user,1,__ATOMIC_RELEASE);
    }
    if(self->compat_owned){self->compat_owned=0;spin_unlock(&compatibility_lock);}
}
