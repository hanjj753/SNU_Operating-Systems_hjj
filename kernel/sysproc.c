#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if (t == SBRK_EAGER || n < 0) {
    if (growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if (addr + n < addr)
      return -1;
    if (addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if (n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (killed(myproc())) {
      release(&tickslock);
      return -1;
    }
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
    acquire(&tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

#ifdef SNU
uint64
sys_kcall(void)
{
  int n;

  argint(0, &n);
  switch (n)
  {
    case KC_FREEMEM:    return freemem;
    case KC_USED4K:     return used4k;
    case KC_USED2M:     return used2m;
    case KC_PF:         return pagefaults;
    default:            return -1;
  }
}

static struct { // system 전체의 mmap state 관리
    struct spinlock lock;
    int count;
    struct mmap_object objects[64];
} mmap_state;

void
mmapinit(void)
{
    initlock(&mmap_state.lock, "mmap");
    for (int i = 0; i < 64; i++) initlock(&mmap_state.objects[i].lock, "mmapobj");
}

static struct mmap_object *
mmap_new(uint64 length)
{
  acquire(&mmap_state.lock);
  for(int i = 0; i<64; i++) { //최대 64개의 mmap
    struct mmap_object *obj = &mmap_state.objects[i];
    if (obj->refs == 0) { // 자리가 있으면
      obj->pages = 0;
      obj->length = length;
      
      obj->refs = 1;
      mmap_state.count++;
      release(&mmap_state.lock);
      return obj;
    }
  }
  release(&mmap_state.lock);
  return 0; //w자리 없으면 0
}

void
mmap_hold(struct mmap_object *obj) // obj ref 늘리는 함수
{
  acquire(&mmap_state.lock);
  if (obj == 0 || obj->refs <= 0)
    panic("mmap_hold");

  obj->refs++;
  release(&mmap_state.lock);
}

void
mmap_release(struct mmap_object *obj)
{
    acquire(&mmap_state.lock);
    if (obj == 0 || obj->refs <= 0)
      panic("mmap_release");

    if (--obj->refs == 0) {// ref이 0이 되어야 release
      acquire(&obj->lock);

      if (obj->pages != 0)
        uvmfree(obj->pages, obj->length);

      obj->pages = 0;
      obj->length = 0;
      release(&obj->lock);

      mmap_state.count--;
    }
    release(&mmap_state.lock);
}

void
mmap_cleanup(struct proc *p)
{
  for (int i = 0; i < 4; i++) {
    struct mmap_region *r = &p->regions[i];
    if (!r->valid)
      continue;

    uvmunmap(p->pagetable, r->addr, r->length / PGSIZE, 1);
    r->valid = 0;
    mmap_release(r->obj);
  }
}

uint64
sys_mmap(void)
{
  // PA4: FILL HERE
  struct proc *p = myproc(); //현재 실행 중인 프로세스

  //mmap에 전달된 인자 가져오기
  uint64 addr;
  int length, prot, flags;
  argaddr(0, &addr);
  argint(1, &length);
  argint(2, &prot);
  argint(3, &flags);

  //오류 검사
  if (length <= 0 || length > 64*1024*1024) return 0;    // length 오류
  if (prot != PROT_READ && prot != PROT_WRITE) return 0; // prot이 이상하면 오류
  
  int mode = flags & (MAP_PRIVATE | MAP_SHARED);
  if (mode != MAP_PRIVATE && mode != MAP_SHARED) return 0; //flag이 private나 shared가 아니면 오류

  int allowed = MAP_PRIVATE | MAP_SHARED | MAP_HUGEPAGE;
  if ((flags & ~allowed) != 0) return 0;                   //flag에서 다른 bit이 켜져 있으면 오류

  // 페이지 할당
  uint64 page_size = (flags & MAP_HUGEPAGE) ? HGPGSIZE : PGSIZE;
  if (addr % page_size != 0) return 0; // addr이 page_size의 배수인지 확인
  uint64 rounded = ((uint64)length + page_size - 1) / page_size * page_size;
  
  int empty = 4;
  // addr ~ addr + rounded vs old
  for (int i = 0;i<4;i++) {
    struct mmap_region *r = &p->regions[i];
    if (!r->valid) {empty = i; continue;} // valid가 0이면
    uint64 old_addr = r->addr; 
    uint64 old_length = r->length; 

    if ((addr < old_addr + old_length) && (old_addr < addr + rounded)) return 0;
  }

  if (empty ==4) return 0;

  struct mmap_object *obj = mmap_new(rounded); // 자리가 있어야 return
  if (obj == 0) return 0;

  struct mmap_region *r = &p->regions[empty];
  r->addr = addr;
  r->length = rounded;
  r->prot = prot;
  r->flags = flags;
  r->obj = obj;
  r->valid = 1;

  return addr;
}

uint64
sys_munmap(void)
{
  // PA4: FILL HERE
  struct proc *p = myproc(); //현재 실행 중인 프로세스
  
  uint64 addr;
  argaddr(0, &addr);

  for (int i = 0;i<4;i++) {
    struct mmap_region *r = &p->regions[i];
    if (!r->valid) continue;
    if (addr != r->addr) continue;

    uvmunmap(p->pagetable, r->addr, r->length / PGSIZE, 1);
    r->valid = 0;

    mmap_release(r->obj);

    return 0;
  }
  return -1;
}
#endif
