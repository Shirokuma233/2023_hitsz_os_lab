#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[];  // trampoline.S

/*
 * create a direct-map page table for the kernel.
 */
void kvminit() {
  kernel_pagetable = (pagetable_t)kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void kvminithart() {
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// 根据虚拟地址查找页表项,实际是叶子页表项,观察level即可看出
//  Return the address of the PTE in page table pagetable
//  that corresponds to virtual address va.  If alloc!=0,
//  create any required page-table pages.
//
//  The risc-v Sv39 scheme has three levels of page-table
//  pages. A page-table page contains 512 64-bit PTEs.
//  A 64-bit virtual address is split into five fields:
//    39..63 -- must be zero.
//    30..38 -- 9 bits of level-2 index.
//    21..29 -- 9 bits of level-1 index.
//    12..20 -- 9 bits of level-0 index.
//     0..11 -- 12 bits of byte offset within the page.

pte_t *walk(pagetable_t pagetable, uint64 va, int alloc) {
  if (va >= MAXVA) panic("walk");

  for (int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0) return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// 根据虚拟地址查找最终的物理地址，只能用于查找用户页表
//  Look up a virtual address, return the physical address,
//  or 0 if not mapped.
//  Can only be used to look up user pages.
uint64 walkaddr(pagetable_t pagetable, uint64 va) {
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA) return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0) return 0;
  if ((*pte & PTE_V) == 0) return 0;
  if ((*pte & PTE_U) == 0) return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// 启动时添加映射
//  add a mapping to the kernel page table.
//  only used when booting.
//  does not flush TLB or enable paging.
void kvmmap(uint64 va, uint64 pa, uint64 sz, int perm) {
  if (mappages(kernel_pagetable, va, sz, pa, perm) != 0) panic("kvmmap");
}

// 在内核页表中查找虚拟地址对应的物理地址
//  translate a kernel virtual address to
//  a physical address. only needed for
//  addresses on the stack.
//  assumes va is page aligned.
uint64 kvmpa(uint64 va) {
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;

  pte = walk(kernel_pagetable, va, 0);
  if (pte == 0) panic("kvmpa");
  if ((*pte & PTE_V) == 0) panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa + off;
}

// 为从虚拟地址va开始的一段地址创建页表项，使其映射到从物理地址pa开始的一段地址
//  Create PTEs for virtual addresses starting at va that refer to
//  physical addresses starting at pa. va and size might not
//  be page-aligned. Returns 0 on success, -1 if walk() couldn't
//  allocate a needed page-table page.
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm) {
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0) return -1;
    if (*pte & PTE_V) panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last) break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// 该函数用于解除虚拟地址va开始的npages个页的映射。如果do_free为1，则还会释放对应的物理内存
//  Remove npages of mappings starting from va. va must be
//  page-aligned. The mappings must exist.
//  Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0) panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    if ((pte = walk(pagetable, a, 0)) == 0) panic("uvmunmap: walk");
    if ((*pte & PTE_V) == 0) panic("uvmunmap: not mapped");
    if (PTE_FLAGS(*pte) == PTE_V) panic("uvmunmap: not a leaf");
    if (do_free) {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

// 分配一页的用户表
//  create an empty user page table.
//  returns 0 if out of memory.
pagetable_t uvmcreate() {
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0) return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}
// 该函数将用户程序的初始化代码加载到页表的地址0处，用于创建第一个进程
//  Load the user initcode into address 0 of pagetable,
//  for the very first process.
//  sz must be less than a page.
void uvminit(pagetable_t pagetable, uchar *src, uint sz) {
  char *mem;

  if (sz >= PGSIZE) panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
  memmove(mem, src, sz);
}

// 该函数用于为进程动态分配虚拟内存
//  Allocate PTEs and physical memory to grow process from oldsz to
//  newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  char *mem;
  uint64 a;

  if (newsz < oldsz) return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R | PTE_U) != 0) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// 用于释放用户页以将进程大小从 oldsz 调整为 newsz
//  Deallocate user pages to bring the process size from oldsz to
//  newsz.  oldsz and newsz need not be page-aligned, nor does newsz
//  need to be less than oldsz.  oldsz can be larger than the actual
//  process size.  Returns the new process size.
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  if (newsz >= oldsz) return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// 递归释放页表
//  Recursively free page-table pages.
//  All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable) {
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

// 释放用户内存页和页表页的函数,一开始是释放映射的物理内存,然后释放页目录
//  Free user memory pages,
//  then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz) {
  if (sz > 0) uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// 将父进程的页表和内存复制到子进程的页表和内存的函数
//  Given a parent process's page table, copy
//  its memory into a child's page table.
//  Copies both the page table and the
//  physical memory.
//  returns 0 on success, -1 on failure.
//  frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0) panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0) panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0) goto err;
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

// 将页表项标记为无效的，针对用户访问
//  mark a PTE invalid for user access.
//  used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va) {
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0) panic("uvmclear");
  *pte &= ~PTE_U;
}

// 从内核复制数据到用户空间的函数
//  Copy from kernel to user.
//  Copy len bytes from src to virtual address dstva in a given page table.
//  Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) return -1;
    n = PGSIZE - (dstva - va0);
    if (n > len) n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// 从用户空间复制数据到内核的函数
//  Copy from user to kernel.
//  Copy len bytes to dst from virtual address srcva in a given page table.
//  Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
  return copyin_new(pagetable, dst, srcva, len);
}

// 从用户空间复制 null-terminated 字符串到内核的函数
//  Copy a null-terminated string from user to kernel.
//  Copy bytes to dst from virtual address srcva in a given page table,
//  until a '\0', or max.
//  Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
  return copyinstr_new(pagetable, dst, srcva, max);
}

// 它比较当前的 satp 寄存器的值与全局页表 kernel_pagetable 的 satp 值是否相等，如果不相等则返回 1，否则返回 0
//  check if use global kpgtbl or not
int test_pagetable() {
  uint64 satp = r_satp();
  uint64 gsatp = MAKE_SATP(kernel_pagetable);
  printf("test_pagetable: %d\n", satp != gsatp);
  return satp != gsatp;
}

// 打印页表
void vmprint(pagetable_t pagetable) {
  printf("page table %p\n", pagetable);
  vpprint(pagetable, 2, 0, 0);
}

// 用于递归打印用户页表的函数, 开始用了int而不是uint64而踩坑了，因为位数不够，移位会出现问题
void vpprint(pagetable_t pagetable, int rank, uint64 L2, uint64 L1) {
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];  // 获取第i条PTE
    // 首先如果不有效就continue
    if (!(pte & PTE_V)) continue;
    // 其次如果有效，先求出pa,flags位
    uint64 pa = PTE2PA(pte);
    char flags[5] = "----";
    if (pte & PTE_R) flags[0] = 'r';
    if (pte & PTE_W) flags[1] = 'w';
    if (pte & PTE_X) flags[2] = 'x';
    if (pte & PTE_U) flags[3] = 'u';
    // 那我们根据他是第几级页目录打印不同的内容
    if (rank == 2) {
      printf("||idx: %d: pa: %p, flags: %s\n", i, pa, flags);
    } else if (rank == 1) {
      printf("||   ||idx: %d: pa: %p, flags: %s\n", i, pa, flags);
    } else {
      uint64 va = (L2 << 30) + (L1 << 21) + (i << 12);
      printf("||   ||   ||idx: %d: va: %p -> pa: %p, flags: %s\n", i, va, pa, flags);
    }
    // 判断PTE的Flag位，如果还有下一级页表(即当前是根页表或次页表)，
    if ((pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      uint64 child = PTE2PA(pte);  // 将PTE转为为物理地址
      // 同理对根页表和次页表参数不同
      if (rank == 2)
        vpprint((pagetable_t)child, rank - 1, i, 0);
      else if (rank == 1)
        vpprint((pagetable_t)child, rank - 1, L2, i);  // 此时L2索引是前面传进来的
    }
  }
}

// task2,给内核进程创建一个页表，返回这个页表
pagetable_t kpcreate() {
  pagetable_t k_pagetable = (pagetable_t)kalloc();

  memset(k_pagetable, 0, PGSIZE);

  // uart registers
  my_kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W, k_pagetable);

  // virtio mmio disk interface
  my_kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W, k_pagetable);

  // PLIC
  my_kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W, k_pagetable);

  // map kernel text executable and read-only.
  my_kvmmap(KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X, k_pagetable);

  // map kernel data and the physical RAM we'll make use of.
  my_kvmmap((uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W, k_pagetable);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  my_kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X, k_pagetable);

  return k_pagetable;
}

// 用于task2的map映射
void my_kvmmap(uint64 va, uint64 pa, uint64 sz, int perm, pagetable_t k_pagetable) {
  if (mappages(k_pagetable, va, sz, pa, perm) != 0) panic("my_kvmmap");
}

// 用于task2的内核页表清除,不释放叶子页表的物理帧
void proc_free_k_pagetable(pagetable_t k_pagetable) {
  // there are 2^9 = 512 PTEs in a page table.
  // printf("当前页表:%p\n", k_pagetable);
  for (int i = 0; i < 512; i++) {
    pte_t pte = k_pagetable[i];
    // printf("当前pa:%p\n", PTE2PA(pte));
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      // printf("当前内部pa:%p\n", PTE2PA(pte));
      proc_free_k_pagetable((pagetable_t)child);
      k_pagetable[i] = 0;
    }
  }
  // cccccc放过来的时候放到for循环里去了，debug了1个小时，无语死了aaaaaaaaa
  kfree((void *)k_pagetable);
}

// task3用于设置一个内核页表的次页表项
void set_pte_U2K(pagetable_t k_pagetable, uint64 va_second, uint64 va_first, pte_t pte_first) {
  // 类似采用walk方法,若当前地址有子页表则进入，否则新建页表初始化后进入
  pte_t *pte = &k_pagetable[va_second];
  if (*pte & PTE_V) {
    k_pagetable = (pagetable_t)PTE2PA(*pte);
  } else {
    k_pagetable = (pde_t *)kalloc();
    memset(k_pagetable, 0, PGSIZE);
    *pte = PA2PTE(k_pagetable) | PTE_V;
  }
  // 至此根页表项构造完毕，且有效,下面进入次页表,后面我们不需要在分配页表空间了，直接赋值即可
  k_pagetable[va_first] = pte_first;
}

// task3用于把进程的用户表映射到内核页表里,采用共享叶子页表的办法
void sync_pagetable(pagetable_t u_pagetable, pagetable_t k_pagetable, uint64 start, uint64 end) {
  // 将当前用户页表的对应虚拟地址的页表项加入到k_pagetable
  for(uint64 address = start ; address < end ; address += PGSIZE){
    pte_t *pte = walk(u_pagetable,address,0);           // 返回进程中对应虚拟地址的页表项
    pte_t *kernel_pte = walk(k_pagetable,address,1);    // 新增内核页表在对应虚拟地址的页表项
    *kernel_pte = (*pte)&(~PTE_U);                      // 标志位修改
  }
}