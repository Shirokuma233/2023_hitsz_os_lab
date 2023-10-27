// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 13
struct {
  struct spinlock global_lock;
  struct spinlock lock[NBUCKETS];
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  //每个hash桶有一个头
  struct buf head[NBUCKETS];
} bcache;

void
binit(void)
{
  struct buf *b;
  //初始化全局锁
  initlock(&bcache.global_lock, "bcache_global");
  //初始化每个桶的锁，初始化每个桶锁的链表
  for(int i=0; i<NBUCKETS; i++)
  {
    initlock(&bcache.lock[i], "bcache");
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
  }
  
  // Create linked list of buffers
  //采用枚举hash桶然后添加对应的NBUF来加入数据缓存块
  for(int i=0; i<NBUCKETS; i++)
  {
    //然后把bcache.buf[i] + NBUCKETS*x 全都加入这一个hash桶
    for(b = bcache.buf+i; b < bcache.buf + NBUF; b += NBUCKETS)
    {
      b->next = bcache.head[i].next;
      b->prev = &bcache.head[i];
      initsleeplock(&b->lock, "buffer");
      bcache.head[i].next->prev = b;
      bcache.head[i].next = b;
    }

  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  //当前块的桶编号
  int id = blockno % NBUCKETS;
  acquire(&bcache.lock[id]);

  // Is the block already cached?
  for(b = bcache.head[id].next; b != &bcache.head[id]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock[id]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.lock[id]);
  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  //根据实验指导书，允许按照顺序查找引用计数为0的块，我们直接遍历，且由于原来桶未命中，说明原来桶里所有数据一定被引用
  //根据指导书的防止死锁策略加锁解锁
  acquire(&bcache.global_lock);
  acquire(&bcache.lock[id]);
  for(b = bcache.buf; b< bcache.buf + NBUF; b++)
  {
    //但查找到引用计数0的时候
    //这里加锁应该放在判断引用次数为0之前，否则会出现前一刻应用计数为0，后一刻其他cpu加锁使用了该缓存块。


    //这里有问题


    int new_id = (b->blockno) % NBUCKETS;
    acquire(&bcache.lock[new_id]);
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.global_lock);
      release(&bcache.lock[id]);
      release(&bcache.lock[new_id]);
      acquiresleep(&b->lock);
      return b;
    }
    else  //没找到也要记得释放新桶锁
    {
      release(&bcache.lock[new_id]);
    }
  }
  //如果没有找到,要记得释放全局锁和当前桶锁
  release(&bcache.global_lock);
  release(&bcache.lock[id]);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  int id = (b->blockno) % NBUCKETS;
  acquire(&bcache.lock[id]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head[id].next;
    b->prev = &bcache.head[id];
    bcache.head[id].next->prev = b;
    bcache.head[id].next = b;
  }
  
  release(&bcache.lock[id]);
}

void
bpin(struct buf *b) {
  int id = (b->blockno) % NBUCKETS;
  acquire(&bcache.lock[id]);
  b->refcnt++;
  release(&bcache.lock[id]);
}

void
bunpin(struct buf *b) {
  int id = (b->blockno) % NBUCKETS;
  acquire(&bcache.lock[id]);
  b->refcnt--;
  release(&bcache.lock[id]);
}


