#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;
// import call sigret assembly function
extern void call_sigret(void);
extern void call_sigret_end(void);
int counter;


// Default functions for kernel space signal
int
sig_kill (int signum)
{
  myproc()->killed = 1;
  return 0;
}

int
sig_stop (int signum)
{
  myproc()->stopped = 1;
  return 0;
}

int
sig_cont (int signum)
{
  myproc()->stopped = 0;
  return 0;
}

// Execute all pending signal
void
execPendings(struct trapframe *tf)
{ 
  struct proc *p = myproc();

  if(p == NULL) return;
  // If not on user mode
  if ((tf->cs & 3) != DPL_USER) return;
  // make sure it's not already handling signals
  if(!cas(&p->handling_signal, 0, 1)) return;

    counter++;
    int ibit;
    // cprintf("%s%d%s", "pid: ", myproc()->pid,"\n");
    // cprintf("%s%d%s", "pending: ", myproc()->pending,"\n");
    // Check pending signals before returning to user space 
    for(int i = 0; i < 32; i++){
      // Check if the i-th bit is on
      ibit = (p->pending & (1u << i)) >> i;
      if(ibit){
        cprintf("in Here!\n"); 
        cprintf("%s%d%s", "counter: ", counter,"\n");  
        cprintf("%s%d%s", "pid: ", p->pid,"\n");
        cprintf("%s%d%s", "i value: ", i,"\n");
        cprintf("%s%d%s", "ibit value: ", ibit,"\n");
        cprintf("%s%d%s", "handler: ", p->handlers[i],"\n");

        // if it's kill or stop - execute
        if(i == SIGKILL){
          sig_kill(i);
          // Turn off the bit
          p->pending = (p->pending ^ (1u << i));
          continue;
        }
        if(i == SIGSTOP){
          sig_stop(i);
          p->pending = (p->pending ^ (1u << i));
          continue;
        }
        // Else - check if masked
        if((p->mask & (1u << i)) >> i){
          // Don't turn off the bit
          continue;
        }

        // Check if it's kernel space signal
        if(p->handlers[i] == SIG_DFL){
          if(i == SIGCONT)
            sig_cont(i);
          else{ 
            sig_kill(i);
          }
        }
        else if(p->handlers[i] == (void *)SIG_IGN){
          // Do nothing
        }
        else if(p->handlers[i] == (void *)SIGKILL){
          sig_kill(i);
        }
        else if(p->handlers[i] == (void *)SIGSTOP){
          sig_stop(i);
        }
        else if(p->handlers[i] == (void *)SIGCONT){
          sig_cont(i);
        }
        // if it's user space program
        else{
          cprintf("%s\n", "made it inside else function!! ");
          cprintf("%s%d\n", "proc pending: ", p->pending);
          cprintf("%s%d\n", "p->handling_user_signal: ", p->handling_user_signal);
          // Prevent handling another user space signal concurrently
          if(!cas(&p->handling_user_signal, 0, 1)){
            p->handling_signal = 0;
             return;
          }
          cprintf("%s%d\n", "p->handling_user_signal: ", p->handling_user_signal);
          // Save initial state of mask
          p->mask_backup = p->mask;
          // Replace with current signal mask
          p->mask = p->masksArr[i];
          // Backup process trapframe
          // memmove(&p->backup, p->tf, sizeof(struct trapframe));
          *p->backup = *p->tf;
          // Test prints
          // cprintf("%s%d\n", "call_sigret address: ", call_sigret);
          // cprintf("%s%d\n", "&call_sigret address: ", &call_sigret);
          // cprintf("%s%d\n", "&call_sigret_end address: ", &call_sigret_end);
          // Inject sigret as return address
          uint call_size = (uint)&call_sigret_end - (uint)&call_sigret;
          p->tf->esp -= call_size;
          memmove((void*) p->tf->esp, call_sigret, call_size);
          // Push i (= signum)
          *((int*)(p->tf->esp - 4)) = i;
          // Push return address
          *((int*)(p->tf->esp - 8)) = p->tf->esp;

          cprintf("%s%d\n", "eip return from handler: ", p->tf->esp);
          cprintf("%s%d\n", "&eip return from handler: ", &p->tf->esp);
          cprintf("%s%d\n", "*eip return from handler: ", *((int*)(p->tf->esp)));

          p->tf->esp -= 8;
          // jump to the corresponding sa_handler
          cprintf("%s%d\n", "eip value before: ", p->tf->eip);
          p->tf->eip = (uint)p->handlers[i];
          // break;
          cprintf("%s%d\n", "eip value after: ", p->tf->eip);

          cprintf("%s%d\n", "proc pending before changing: ", p->pending);
        p->pending = (p->pending ^ (1u << i));
        cprintf("%s%d\n", "proc pending after handling: ", p->pending);
        // @TODO: check if needed
        break;
        }

        // Turn off the pending bit of the signal we handled
        cprintf("%s%d\n", "proc pending before changing: ", p->pending);
        p->pending = (p->pending ^ (1u << i));
        cprintf("%s%d\n", "proc pending after handling: ", p->pending);
        
      }
    }
    p->handling_signal = 0;
}

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
