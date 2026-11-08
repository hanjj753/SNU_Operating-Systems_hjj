// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct frame_group { // physical memory 관리용 그룹
  uchar used[PGNUM]; // 각 group의 frame 512개마다 사용되었는지
  int used_count; // 얼마나 used인지, 0~512, huge면 512
  int huge_allocated; // huge로 할당지
  int ref[PGNUM];  // 일반 프레임별 참조 수
  int huge_ref;    // huge 프레임의 참조 수
};

struct {
  struct spinlock lock;
  struct frame_group groups[NGROUP];
} kmem;

#ifdef SNU
int freemem, used4k, used2m;
#endif

void
kinit()
{
  initlock(&kmem.lock, "kmem");

  acquire(&kmem.lock);
  for (int i = 0; i< NGROUP; i++) {
    kmem.groups[i].huge_allocated = 0;
    kmem.groups[i].used_count = PGNUM;
    kmem.groups[i].huge_ref = 0;
    for (int j=0;j<PGNUM;j++) {
      kmem.groups[i].used[j] = 1;
      kmem.groups[i].ref[j] = 1;
    }
  }
  #ifdef SNU
  freemem = 0; used4k=32768; used2m=0;
  #endif
  release(&kmem.lock);

  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{ // ref 줄이기, used 줄이기, used count 줄이기
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  uint64 offset = (uint64) pa - KERNBASE;
  uint64 group = offset / HGPGSIZE;
  uint64 index = (offset % HGPGSIZE) / PGSIZE;

  acquire(&kmem.lock);
  struct frame_group *g = &kmem.groups[group];

  if (g->used[index] == 1) { 
    if (g->huge_allocated || g->ref[index] <= 0) panic("kfree ref");

    if (--g->ref[index] > 0) { // 다른 process가 아직 사용 중이다.
      release(&kmem.lock);
      return;
    }

    g->used[index] = 0;
    g->used_count--;
    #ifdef SNU
    freemem++; used4k--;
    #endif
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);
  }  else panic("kfree");
  release(&kmem.lock);
}

// 전체 메모리: 128MiB
// 그룹 하나:    2MiB -> HG page 관리용
// 프레임 하나:  4KiB
// 전체 그룹 수 = 128MiB / 2MiB = 64개
// 그룹당 프레임 수 = 2MiB / 4KiB = 512개

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void) // PPN을 할당해주는 함수
{ 
  uint group_num = NGROUP; // group은 최대 64개
  uint16 frame_num = 0; //frame은 최대 512개
  acquire(&kmem.lock);
  for (int i = 0; i< NGROUP; i++) {
    if (kmem.groups[i].used_count > 0 && kmem.groups[i].used_count < PGNUM) {
      group_num = i; 
      for (int j=0;j<PGNUM;j++) if(kmem.groups[i].used[j] == 0) {frame_num = j; break;}
      break;
    }
  }

  if (group_num == NGROUP) { // 못 찾은 경우 = 다 0 또는 512
    for (int i = 0; i< NGROUP; i++) {
      if (kmem.groups[i].used_count == 0) {
        kmem.groups[i].used[0] = 1;
        kmem.groups[i].ref[0] = 1;
        kmem.groups[i].used_count++;
        #ifdef SNU
        freemem--; used4k++;
        #endif
        release(&kmem.lock);

        uint64 r = KERNBASE + HGPGSIZE * i;
        memset((char *)r, 5, PGSIZE);
        return (void *) r;
      }
    }
  } else { // 찾은 경우
    kmem.groups[group_num].used[frame_num] = 1;
    kmem.groups[group_num].ref[frame_num] = 1;
    kmem.groups[group_num].used_count++;
    #ifdef SNU
    freemem--; used4k++;
    #endif
    release(&kmem.lock);

    uint64 r = KERNBASE + HGPGSIZE * group_num + PGSIZE * frame_num;
    memset((char *)r, 5, PGSIZE);
    return (void *) r;
  }
  release(&kmem.lock);
  // 할당할 공간 없다
  return (void *) 0;
}

#ifdef SNU
void *
kalloc_huge(void)
{
  // PA4: FILL HERE

  uint group_num = NGROUP; // group은 최대 64개
  acquire(&kmem.lock);
  for (int i = 0; i< NGROUP; i++) {
    if (kmem.groups[i].used_count == 0) {
      group_num = i;
      break;
    }
  }

  if (group_num != NGROUP) { // 못 찾은 경우 = 다 0보다 크다
    for (int i = 0; i< PGNUM; i++) { kmem.groups[group_num].used[i] = 1; }
    kmem.groups[group_num].used_count = PGNUM;
    kmem.groups[group_num].huge_allocated = 1;
    kmem.groups[group_num].huge_ref = 1;
    freemem -= 512; used2m++;
    release(&kmem.lock);

    uint64 r = KERNBASE + HGPGSIZE * group_num;
    memset((char *)r, 5, HGPGSIZE);
    return (void *) r;
  }
  release(&kmem.lock);
  return (void *)0;
}

void
kfree_huge(void *pa)
{
  // PA4: FILL HERE
  if (((uint64)pa % HGPGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree_huge");

  uint64 offset = (uint64) pa - KERNBASE;
  uint64 group = offset / HGPGSIZE;

  acquire(&kmem.lock);
  struct frame_group *g = &kmem.groups[group];
  if (g->huge_allocated == 1) { 
    if (g->huge_ref <= 0) panic("kfree_huge ref");

    if (--g->huge_ref > 0) {
      release(&kmem.lock);
      return;
    }

    for (int i = 0; i< PGNUM; i++) { g->used[i] = 0; }
    g->used_count = 0;
    g->huge_allocated = 0;
    #ifdef SNU
    freemem += 512; used2m--;
    #endif

    memset(pa, 1, HGPGSIZE);
  } else panic("kfree_huge");
  release(&kmem.lock);

}

void 
kref_inc(uint64 pa, int huge) //physical memory의 ref 올리기
{
  uint64 unit = huge ? HGPGSIZE : PGSIZE;
  if (pa % unit || pa < (uint64)end || pa >= PHYSTOP) // end는 first address after kernel
    panic("krefinc");

  uint64 offset = pa - KERNBASE;
  struct frame_group *g = &kmem.groups[offset / HGPGSIZE];
  uint64 index = (offset % HGPGSIZE) / PGSIZE;

  acquire(&kmem.lock);
  int *ref;
  if (huge) {
    if(!g->huge_allocated) 
      panic("krefinc huge");
    ref = &g->huge_ref;
  } else {
    if (g->huge_allocated || !g->used[index])
      panic("krefinc base");
    ref = &g->ref[index];
  }

  if (*ref <= 0) panic("krefinc ref");

  (*ref)++;
  release(&kmem.lock);
}

int
kref_count(uint64 pa, int huge) 
{
  uint64 unit = huge ? HGPGSIZE : PGSIZE;

  if (pa % unit || pa < (uint64)end || pa >= PHYSTOP)
    panic("kref_count");

  uint64 offset = pa - KERNBASE;
  struct frame_group *g = &kmem.groups[offset / HGPGSIZE];
  uint64 index = (offset % HGPGSIZE) / PGSIZE;

  acquire(&kmem.lock);

  if ((huge && !g->huge_allocated) ||
      (!huge && (g->huge_allocated || !g->used[index])))
    panic("kref_count type");

  int n = huge ? g->huge_ref : g->ref[index];
  if (n <= 0)
    panic("kref_count ref");

  release(&kmem.lock);
  return n;
}
#endif
