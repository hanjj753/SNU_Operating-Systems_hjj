#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S
 
#ifdef SNU
int pagefaults;     // only for page faults that occur in a memory-mapped region
#endif

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext,
         PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

static pte_t * // PTE와 함꼐 무슨 레벨에서 찾았는지 알려줌
walklevel(pagetable_t pagetable, uint64 va, int alloc, int *out_level)
{
  if (va >= MAXVA)
    panic("walk");

  for (int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) { // valid
      if (*pte & (PTE_R | PTE_W | PTE_X)) { // huge frame을 가리키는 leaf라면 바로 멈춤
        *out_level = level;
        return pte;
      }
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  *out_level = 0;
  return &pagetable[PX(0, va)];
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  int level;
  return walklevel(pagetable, va, alloc, &level);
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  int level;
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;

  pte = walklevel(pagetable, va, 0, &level);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;

  uint64 mask = (1UL << PXSHIFT(level)) -1; // 2MB(hugetable)이면 0x1FFFFF, 즉 아래 21bit
  pa = PTE2PA(*pte) + (PGROUNDDOWN(va) & mask); // va의 아래 21bit offset만 가지고 온다음에 합친다
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if ((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if (size == 0)
    panic("mappages: size");

  a = va;
  last = va + size - PGSIZE;
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

#ifdef SNU
int
maphugepages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{

  if (size != HGPGSIZE || va >= MAXVA || va % HGPGSIZE != 0 || pa % HGPGSIZE != 0) return -1;

  pte_t *pte2 = &pagetable[PX(2, va)]; //PX = va의 해당하는 level의 9bit 꺼내는 함수
  pagetable_t l1;

  if (*pte2 & PTE_V) { // valid한지
    if (*pte2 & (PTE_R | PTE_W | PTE_X)) return -1; 
    l1 = (pagetable_t)PTE2PA(*pte2); // 1 level page table 주소는 10 bit 이후를 뒤에 12bit 0 붙인 거
  } else {
    l1 = (pagetable_t)kalloc(); // page table 하나는 512 entry * 64bit(8byte) = 4096B=4KB
    if (l1 == 0) return -1;

    memset(l1, 0, PGSIZE);
    *pte2 = PA2PTE(l1) | PTE_V;
  }

  // L1의 PTE가 2MiB 데이터 프레임을 직접 가리킨다.
  pte_t *pte1 = &l1[PX(1, va)]; // L1 table의 PTE 꺼냄

  pagetable_t old_l0 = 0;

  if (*pte1 & PTE_V) {
    if (*pte1 & (PTE_R | PTE_W | PTE_X))
      return -1;

    old_l0 = (pagetable_t)PTE2PA(*pte1);
    for (int i = 0; i < 512; i++)
      if (old_l0[i] & PTE_V)
        return -1;
  }

  *pte1 = PA2PTE(pa) | perm | PTE_V;
  sfence_vma();

  if (old_l0 != 0)
    kfree((void *)old_l0);

  return 0;
}
#endif

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;
  int level;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  a = va;
  while (a < va + npages * PGSIZE) {
    pte = walklevel(pagetable, a, 0, &level);
    if (pte == 0) {a += PGSIZE; continue;} // leaf page table entry allocated?
    if ((*pte & PTE_V) == 0) {a += PGSIZE; continue;}// has physical page been allocated?
    if (do_free) {
      if (level == 0) {
        uint64 pa = PTE2PA(*pte);
        kfree((void *)pa);
      } else {
        uint64 pa = PTE2PA(*pte);
        kfree_huge((void *)pa);
      }
    }
    *pte = 0;
    a += (level == 1) ? HGPGSIZE : PGSIZE;
  }

  sfence_vma();
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) !=
        0) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0)
      continue; // page table entry hasn't been allocated
    if ((*pte & PTE_V) == 0)
      continue; // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char *)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

#ifdef SNU
static uint64
mmap_cow_fault(pagetable_t pagetable, uint64 va)
{
  if (va >= MAXVA)
    return 0;

  int level;
  pte_t *pte = walklevel(pagetable, va, 0, &level); // va의 pte 갖고 온다

  if (pte == 0 || !(*pte & PTE_V) || !(*pte & PTE_U) ||
      !(*pte & PTE_COW) || (level != 0 && level != 1))
    return 0;

  int huge = level == 1;
  uint64 size = huge ? HGPGSIZE : PGSIZE;
  uint64 va0 = va & ~(size - 1);
  uint64 oldpa = PTE2PA(*pte); // 기존 pa
  uint64 perm = (PTE_FLAGS(*pte) | PTE_W) & ~PTE_COW;

  // 혼자 사용하는 프레임이면 복사할 필요 없음
  if (kref_count(oldpa, huge) == 1) {
    *pte = PA2PTE(oldpa) | perm;
    sfence_vma();
    return oldpa + (PGROUNDDOWN(va) - va0);
  }

  // 새로운 프레임 만든다
  uint64 newpa = (uint64)(huge ? kalloc_huge() : kalloc());
  if (newpa == 0)
    return 0;

  memmove((void *)newpa, (void *)oldpa, size);

  // 기존 PTE를 새 프레임으로 교체
  *pte = PA2PTE(newpa) | perm;
  sfence_vma();

  // 이전 프레임에 대한 현재 프로세스의 참조 반납
  if (huge)
    kfree_huge((void *)oldpa);
  else
    kfree((void *)oldpa);

  return newpa + (PGROUNDDOWN(va) - va0);
}
#endif

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 psz, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);

    #ifdef SNU
    if (pte != 0 && (*pte & PTE_COW)) {
      pa0 = mmap_cow_fault(pagetable, va0);
      if (pa0 == 0) return -1;

      pte = walk(pagetable, va0, 0); //새 주소로 바꿔준다
    }
    #endif
    // forbid copyout over read-only user text pages.
    if (pte == 0 ||  (*pte & PTE_W) == 0)
      return -1;

    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 1)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva,
          uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 1)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0) {
      if (*p == '\0') {
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null) {
    return 0;
  } else {
    return -1;
  }
}

#ifdef SNU
static uint64
mmap_shared_faults(pagetable_t pagetable, struct mmap_region *r, uint64 va)
{

  struct mmap_object *obj = r->obj;
  int huge = (r->flags & MAP_HUGEPAGE) != 0;
  uint64 size = huge ? HGPGSIZE : PGSIZE;
  uint64 va0 = va & ~(size - 1);
  uint64 pa;

  if (obj == 0 || va < r->addr || va - r->addr >= r->length)
    return 0;
  if (ismapped(pagetable, va0))
    return 0;

  uint64 offset = va0 - r->addr; // 해당 페이지에서 addr 시작 주소까지 offset
  int perm = PTE_U | PTE_R;
  if (r->prot == PROT_WRITE) perm |= PTE_W;

  acquire(&obj->lock);

  if(offset >= obj->length) {
    release(&obj->lock);
    return 0;
  }

  if (obj->pages == 0) {
    obj->pages = uvmcreate(); // 없으면 pagetable create
    if (obj->pages == 0) {
      release(&obj->lock);
      return 0;
    }
  }

  int level;
  pte_t *pte = walklevel(obj->pages, offset, 0, &level);

  if (pte != 0 && (*pte & PTE_V)) { // 공유 표에 프레임 있으면, 매핑이 존재하는가 -> 그거 사용
    if (level != (huge ? 1 : 0)) {
      release(&obj->lock);
      return 0;
    }
    pa = PTE2PA(*pte);
  } else { // 없으면 새로 할당
    pa = (uint64)(huge ? kalloc_huge() : kalloc());

    if (pa == 0) {
      release(&obj->lock);
      return 0;
    }
    memset((void *)pa, 0, size);

    int rc = huge ? maphugepages(obj->pages, offset, size, pa, PTE_R | PTE_W) : mappages(obj->pages, offset, size, pa, PTE_R | PTE_W);

    if (rc != 0) { // 오류 나면
      if (huge) kfree_huge((void *)pa);
      else kfree((void *)pa);

      release(&obj->lock);
      return 0;
    }
  }

  kref_inc(pa, huge);
  release(&obj->lock);

  int rc = huge ? maphugepages(pagetable, va0, size, pa, perm) : mappages(pagetable, va0, size, pa, perm);

  if (rc != 0) {
    if (huge)
      kfree_huge((void *)pa);
    else
      kfree((void *)pa);
    return 0;
  }

  sfence_vma();
  return pa + (PGROUNDDOWN(va) - va0);
} 

#endif

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 psz, uint64 va, int read) // read = 1이면 읽기 접근, read = 0이면 write 접근
{ // 지금 lazy heap만 처리하는데 mmap도 처리하면 됨.
  uint64 mem;

  #ifdef SNU
  struct proc *p = myproc();

  // mmap된 영역은 psz보다 클 수 있음 -> 먼저 검사
  for (int i = 0;i<4;i++) {
    struct mmap_region *r = &p->regions[i];

    if (pagetable != p->pagetable || !r->valid) continue;

    if (r->addr <= va && va < (r->addr + r->length)) {
      if (read == 0 && r->prot == PROT_READ) return 0; // write 요청인데 read인 경우

      if (ismapped(pagetable, va)) {
        if (read == 0 && (r->flags & MAP_PRIVATE)) return mmap_cow_fault(pagetable, va); //write 요청이고 private
        return 0;
      } //이미 va가 pagetable에 있는지 확인

      if (read == 0 && r->prot == PROT_READ) return 0;
      int perm = PTE_U | PTE_R;
      if (r->prot == PROT_WRITE) perm |= PTE_W;

      if ((r->flags & MAP_SHARED) != 0) return mmap_shared_faults(pagetable, r, va); // mmap shared 폴트(부모가 접근하지 않은 상태로 fork 후 자식 접근)
      
      if (r->flags & MAP_HUGEPAGE) { // frame 할당
        uint64 va0 = va & ~(HGPGSIZE - 1); // va가 속한 페이지의 시작

        mem = (uint64)kalloc_huge();
        if (mem == 0) return 0; // 오류 났다
        memset((void *)mem, 0, HGPGSIZE);
        if (maphugepages(pagetable, va0, HGPGSIZE, mem, perm) != 0) {
          kfree_huge((void *)mem);
          return 0;
        }
        return mem + (PGROUNDDOWN(va) - va0); // va의 4KB offset 만큼 더해줘야 함
      } else {
        uint64 va0 = va & ~(PGSIZE - 1); // va가 속한 페이지의 시작

        mem = (uint64)kalloc();
        if (mem == 0) return 0; // 오류 났다
        memset((void *)mem, 0, PGSIZE);
        if (mappages(pagetable, va0, PGSIZE, mem, perm) != 0) {
          kfree((void *)mem);
          return 0;
        }
        return mem;
      }

    }
  }
  #endif

  if (va >= psz) // 접근하려던 가상 주소가 psz = 프로세스 메모리 크기보다 작은지
    return 0;
  va = PGROUNDDOWN(va);
  if (ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64)kalloc();
  if (mem == 0) // 오류 났다
    return 0;
  memset((void *)mem, 0, PGSIZE);
  if (mappages(pagetable, va, PGSIZE, mem, PTE_W | PTE_U | PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V) {
    return 1;
  }
  return 0;
}

#ifdef SNU
int
mmapfork(struct proc *parent, struct proc *child)
{
  for (int i = 0; i<4; i++) {
    struct mmap_region *r = &parent->regions[i];
    if(!r->valid) continue;

    child->regions[i] = *r;
    mmap_hold(r->obj); //obj + 1 (child 땜에)

    int huge = (r->flags & MAP_HUGEPAGE);
    uint64 step = huge ? HGPGSIZE : PGSIZE;

    for (uint64 va = r->addr; va < r->addr + r->length; va += step) {
      int level;
      pte_t *pte = walklevel(parent->pagetable, va, 0, &level);

      if (pte == 0 || !(*pte & PTE_V)) continue;
      if (level != (huge ? 1 : 0)) panic("mmapfork level"); //level이 안 맞는다

      uint64 pa = PTE2PA(*pte);
      int perm = PTE_FLAGS(*pte);

      if ((r->flags & MAP_PRIVATE) && r->prot == PROT_WRITE)
        perm = (perm & ~PTE_W) | PTE_COW;

      kref_inc(pa, huge);

      int rc = huge ? maphugepages(child->pagetable, va, step, pa, perm) : mappages(child->pagetable, va, step, pa, perm);
      if (rc != 0) {
        // 이번에 증가시킨 참조 되돌리기
        if (huge) kfree_huge((void *)pa); // 어차피 parent의 ref 남아있으니 ref만 -1됨
        else kfree((void *)pa);

        mmap_cleanup(child); // 앞에서 성공했던 child의 매핑과 영역 참조도 반납
        return -1;
      }
    }
  }

  // 자식 매핑이 모두 성공한 뒤 부모도 쓰기 금지로 변경
  for (int i = 0; i < 4; i++) {
    struct mmap_region *r = &parent->regions[i];
    if (!r->valid || !(r->flags & MAP_PRIVATE) || r->prot != PROT_WRITE) //valid = 0, private 이면서 write 가능
      continue;

    uint64 step = (r->flags & MAP_HUGEPAGE) ? HGPGSIZE : PGSIZE;

    for (uint64 va = r->addr; va < r->addr + r->length; va += step) {
      pte_t *pte = walk(parent->pagetable, va, 0);
      if (pte != 0 && (*pte & PTE_V))
        *pte = (*pte & ~PTE_W) | PTE_COW; //부모도 COW
    }
  }

  sfence_vma();
  return 0;
}
#endif
