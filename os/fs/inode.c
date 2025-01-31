#include <file/file.h>
#include <fs/fs.h>
#include <fs/buf.h>
#include <proc/proc.h>

struct {
    struct mutex lock;
    struct inode inode[NINODE];
} itable;

struct {
    struct mutex lock;
    struct page_cache cache[NCACHE];
    struct page_cache* lru[NCACHE];
} ctable;

static int cache_writeback(struct page_cache* cache);

static int ctable_lru_evict() {
//    infof("ctable_lru_evict");
    for (int i = NCACHE - 1; i>=0;i--) {
        struct page_cache* cache = ctable.lru[i];
        if (cache &&                                        // cache entry exists
            get_physical_page_ref(cache->page) == 1) {  // cache page is not shared

            // if dirty, write back to disk
            if (cache->dirty && cache_writeback(cache) != 0) {
                panic("cache_writeback error");
            }
            recycle_physical_page(cache->page);

            // dereference inode
            iput(cache->host);

            // clear the cache entry
            memset(cache, 0, sizeof(struct page_cache));

            // move cache entry from back to front
            for (int j = i; j < NCACHE - 1; j++) {
                ctable.lru[j] = ctable.lru[j+1];
            }
            ctable.lru[NCACHE - 1] = NULL;

            return 0;
        }
    }
    infof("ctable_lru_evict: no cache entry to evict");
    return -1;
}

static int ctable_lru_add(struct page_cache* cache) {
//    infof("ctable_lru_add");
    int i;
    if (ctable.lru[NCACHE-1] != NULL && ctable_lru_evict() < 0) {
        infof("ctable_lru_add: ctable_lru_evict error");
        return -1;
    }
    for (i = NCACHE - 1; i > 0; i--) {
        ctable.lru[i] = ctable.lru[i-1];
    }
    ctable.lru[0] = cache;
    return 0;
}

static int ctable_lru_remove(struct page_cache* cache) {
//    infof("ctable_lru_remove");
    for (int i = 0; i < NCACHE; i++) {
        if (ctable.lru[i] == cache) {
            for (int j = i; j < NCACHE - 1; j++) {
                ctable.lru[j] = ctable.lru[j+1];
            }
            ctable.lru[NCACHE - 1] = NULL;
            return 0;
        }
    }
    infof("ctable_lru_remove: cache not found");
    return -1;
}

static int ctable_lru_adjust(struct page_cache* cache) {
//    infof("ctable_lru_adjust");
    for (int i = 0; i < NCACHE; i++) {
        if (ctable.lru[i] == cache) {
            for (int j = i; j > 0; j--) {
                ctable.lru[j] = ctable.lru[j-1];
            }
            ctable.lru[0] = cache;
            return 0;
        }
    }
    infof("ctable_lru_adjust: cache not found");
    return -1;
}

struct page_cache* ctable_acquire(struct inode* ip, uint offset) {
//    infof("ctable_acquire, ip: %p, offset: %d", ip, offset);
    KERNEL_ASSERT(ip != NULL, "inode is NULL");
    KERNEL_ASSERT((offset & (PGSIZE - 1)) == 0, "offset is not aligned");

    struct page_cache* cache;

    acquire_mutex_sleep(&ctable.lock);
    // reuse cache if it is already in the cache
    for (cache = ctable.cache; cache < ctable.cache + NCACHE; cache++) {
        acquire_mutex_sleep(&cache->lock);
        if (cache->valid && cache->host == ip && cache->offset == offset) {
//            infof("reuse cache");
            ctable_lru_adjust(cache);
            release_mutex_sleep(&ctable.lock);
            return cache;
        }
        release_mutex_sleep(&cache->lock);
    }
    // if not, find an empty cache
    int first_chance = 1;
find_again:
    for (cache = ctable.cache; cache < ctable.cache + NCACHE; cache++) {
        acquire_mutex_sleep(&cache->lock);
        if (!cache->valid) {
//            infof("create cache");
            cache->host = ip;
            cache->offset = offset;
            cache->valid = TRUE;
            cache->dirty = FALSE;
            cache->page = alloc_physical_page();
            memset(cache->page, 0, PGSIZE);
            release_mutex_sleep(&ctable.lock);
            goto read_page;
        }
        release_mutex_sleep(&cache->lock);
    }
    if (first_chance) {
        first_chance = 0;
        ctable_lru_evict();
        goto find_again;
    } else {
        release_mutex_sleep(&ctable.lock);
        infof("ctable_acquire: no free space");
        return NULL;
    }

read_page:
    if (f_lseek(&ip->file, offset) != FR_OK) {
        infof("ctable_acquire: invalid offset");
        goto read_page_err;
    }

    UINT size;
    if (f_read(&ip->file, cache->page, PAGE_SIZE, &size) != FR_OK) {
        infof("ctable_acquire: read error");
        goto read_page_err;
    }

    idup(ip);
    ctable_lru_add(cache);
    return cache;

read_page_err:
    cache->host = NULL;
    cache->offset = 0;
    cache->valid = FALSE;
    cache->dirty = FALSE;
    recycle_physical_page(cache->page);
    cache->page = NULL;
    release_mutex_sleep(&cache->lock);
    return NULL;
}

static int cache_writeback(struct page_cache* cache) {
    infof("cache_writeback, cache: %p", cache);
    KERNEL_ASSERT(cache != NULL, "cache is NULL");
    KERNEL_ASSERT(cache->host != NULL, "cache->host is NULL");
    KERNEL_ASSERT(cache->page != NULL, "cache->page is NULL");

    if (f_lseek(&cache->host->file, cache->offset) != FR_OK) {
        infof("cache_writeback: invalid offset");
        return -1;
    }
    uint filesize = f_size(&cache->host->file);
    uint n = MIN(filesize - cache->offset, PGSIZE);
    uint writesize;
    if (f_write(&cache->host->file, cache->page, n, &writesize) != FR_OK || writesize != n) {
        infof("cache_writeback: write error");
        return -1;
    }
    return 0;
}

//static int ctable_release(struct inode* ip, uint offset) {
//    infof("ctable_release, ip: %p, offset: %d", ip, offset);
//    KERNEL_ASSERT(ip != NULL, "inode is NULL");
//    KERNEL_ASSERT((offset & (PGSIZE - 1)) == 0, "offset is not aligned");
//
//    struct page_cache* cache;
//    acquire_mutex_sleep(&ctable.lock);
//    for (cache = ctable.cache; cache < ctable.cache + NCACHE; cache++) {
//        acquire_mutex_sleep(&cache->lock);
//        if (cache->valid && cache->host == ip && cache->offset == offset) {
//            infof("release cache");
//            if (cache->dirty && cache_writeback(cache) != 0) {
//                panic("cache_writeback error");
//            }
//            cache->valid = FALSE;
//            cache->host = NULL;
//            cache->offset = 0;
//            cache->dirty = FALSE;
//            recycle_physical_page(cache->page);
//            cache->page = NULL;
//            release_mutex_sleep(&cache->lock);
//            return 0;
//        }
//        release_mutex_sleep(&cache->lock);
//    }
//    infof("ctable_release: no cache matched");
//    release_mutex_sleep(&ctable.lock);
//    return -1;
//}

// when all processes are about to terminate, call this function to free all caches
// so the disk can get all changes back to it
void ctable_release(struct inode *ip) {
    infof("ctable_release");
    struct page_cache* cache;
    acquire_mutex_sleep(&ctable.lock);
    for (cache = ctable.cache; cache < ctable.cache + NCACHE; cache++) {
        acquire_mutex_sleep(&cache->lock);
        if (cache->valid && (cache->host == ip || ip == NULL)) {
            // if dirty, write back to disk
            if (cache->dirty && cache_writeback(cache) != 0) {
                panic("cache_writeback error");
            }

            // release physical page
            KERNEL_ASSERT(get_physical_page_ref(cache->page) == 1, "page ref is not 1");
            recycle_physical_page(cache->page);

            // dereference inode
            iput(cache->host);

            // clear the cache entry
            memset(cache, 0, sizeof(struct page_cache));

            // remove it from the lru list
            ctable_lru_remove(cache);

        }
        release_mutex_sleep(&cache->lock);
    }
    release_mutex_sleep(&ctable.lock);
}

static void cache_table_init() {
    init_mutex(&ctable.lock);
    for (int i = 0; i < NCACHE; i++) {
        init_mutex(&ctable.cache[i].lock);
    }
}


//static uint
//bmap(struct inode *ip, uint bn);
static struct inode *
inode_or_parent_by_name(char *path, int nameiparent, char *name);

// Free a disk block.
//static void free_disk_block(int dev, uint block_id) {
//    struct buf *bitmap_block;
//    int bit_offset; // in bitmap block
//    int mask;
//
//    bitmap_block = acquire_buf_and_read(dev, BITMAP_BLOCK_CONTAINING(block_id, sb));
//    bit_offset = block_id % BITS_PER_BITMAP_BLOCK;
//    mask = 1 << (bit_offset % 8);
//    if ((bitmap_block->data[bit_offset / 8] & mask) == 0)
//        panic("freeing free block");
//    bitmap_block->data[bit_offset / 8] &= ~mask;
//    write_buf_to_disk(bitmap_block);
//    release_buf(bitmap_block);
//}

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char *
skipelem(char *path, char *name) {
    char *s;
    int len;

    while (*path == '/')
        path++;
    if (*path == 0)
        return 0;
    s = path;
    while (*path != '/' && *path != 0)
        path++;
    len = path - s;
    if (len >= DIRSIZ)
        memmove(name, s, DIRSIZ);
    else {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

// Zero a block.
//static void
//set_block_to_zero(int dev, int bno) {
//    struct buf *bp;
//    bp = acquire_buf_and_read(dev, bno);
//    memset(bp->data, 0, BSIZE);
//    write_buf_to_disk(bp);
//    release_buf(bp);
//}

// Blocks.

// Allocate a zeroed disk block.
//static uint
//alloc_zeroed_block(uint dev) {
//    int base_block_id;
//    int local_block_id;
//    int bit_mask;
//    struct buf *bitmap_block;
//
//    bitmap_block = NULL;
//    for (base_block_id = 0; base_block_id < sb.total_blocks; base_block_id += BITS_PER_BITMAP_BLOCK) {
//        // the corresponding bitmap block
//        bitmap_block = acquire_buf_and_read(dev, BITMAP_BLOCK_CONTAINING(base_block_id, sb));
//
//        // iterate all bits in this bitmap block
//        for (local_block_id = 0; local_block_id < BITS_PER_BITMAP_BLOCK && base_block_id + local_block_id < sb.total_blocks; local_block_id++) {
//            bit_mask = 1 << (local_block_id % 8);
//            if ((bitmap_block->data[local_block_id / 8] & bit_mask) == 0) {
//                // the block free
//                bitmap_block->data[local_block_id / 8] |= bit_mask; // Mark block in use.
//                write_buf_to_disk(bitmap_block);
//                release_buf(bitmap_block);
//                set_block_to_zero(dev, base_block_id + local_block_id);
//                return base_block_id + local_block_id;
//            }
//        }
//        release_buf(bitmap_block);
//    }
//    panic("alloc_zeroed_block: out of blocks");
//    return 0;
//}


// Allocate an inode on device dev.
// Mark it as allocated by  giving it type `type`.
// Returns an allocated and referenced inode.
//struct inode *
//alloc_disk_inode(uint dev, short type) {
//    int inum;
//    struct buf *bp;
//    struct dinode *disk_inode;
//
//    for (inum = 1; inum < sb.ninodes; inum++) {
//        bp = acquire_buf_and_read(dev, BLOCK_CONTAINING_INODE(inum, sb));
//        disk_inode = (struct dinode *)bp->data + inum % INODES_PER_BLOCK;
//        if (disk_inode->type == 0) {
//            // a free inode
//            memset(disk_inode, 0, sizeof(*disk_inode));
//            disk_inode->type = type;
//            write_buf_to_disk(bp);
//            release_buf(bp);
//            return iget(dev, inum);
//        }
//        release_buf(bp);
//    }
//    panic("ialloc: no inodes");
//    return 0;
//}

void inode_table_init() {
//    init_spin_lock_with_name(&itable.lock, "itable");
    init_mutex(&itable.lock);
    for (int i = 0; i < NINODE; i++) {
        init_mutex(&itable.inode[i].lock);
    }
    cache_table_init();
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk.
//void iupdate(struct inode *ip) {
//    struct buf *bp;
//    struct dinode *dip;
//
//    bp = acquire_buf_and_read(ip->dev, BLOCK_CONTAINING_INODE(ip->inum, sb));
//    dip = (struct dinode *)bp->data + ip->inum % INODES_PER_BLOCK;
//    dip->type = ip->type;
//    dip->major = ip->major;
//    dip->minor = ip->minor;
//    dip->num_link = ip->num_link;
//    dip->size = ip->size;
//    memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
//    write_buf_to_disk(bp);
//    release_buf(bp);
//}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not read
// it from disk.
//struct inode *
//iget(uint dev, uint inum) {
//    debugcore("iget");
//
//    struct inode *inode_ptr, *empty;
//    acquire(&itable.lock);
//    // Is the inode already in the table?
//    empty = NULL;
//    for (inode_ptr = &itable.inode[0]; inode_ptr < &itable.inode[NINODE]; inode_ptr++) {
//        if (inode_ptr->ref > 0 && inode_ptr->dev == dev && inode_ptr->inum == inum) {
//            inode_ptr->ref++;
//            release(&itable.lock);
//            return inode_ptr;
//        }
//        if (empty == 0 && inode_ptr->ref == 0) // Remember empty slot.
//            empty = inode_ptr;
//    }
//
//    // Recycle an inode entry.
//    if (empty == NULL)
//        panic("iget: no inodes");
//
//    inode_ptr = empty;
//    inode_ptr->dev = dev;
//    inode_ptr->inum = inum;
//    inode_ptr->ref = 1;
//    inode_ptr->valid = 0;
//    release(&itable.lock);
//    return inode_ptr;
//}

struct inode *iget_root() {
    debugcore("iget_root");

    struct inode *inode_ptr, *empty;
    //    acquire(&itable.lock);
    acquire_mutex_sleep(&itable.lock);
    // Is the inode already in the table?
    empty = NULL;
    for (inode_ptr = &itable.inode[0]; inode_ptr < &itable.inode[NINODE]; inode_ptr++) {
        if (inode_ptr->ref > 0 && inode_ptr->dev == ROOTDEV && strcmp(inode_ptr->path, "/") == 0) {
            inode_ptr->ref++;
            //    release(&itable.lock);
            release_mutex_sleep(&itable.lock);
            return inode_ptr;
        }
        if (inode_ptr->ref == 0) { // Remember empty slot.
            empty = inode_ptr; // still holding lock
            break;
        }
    }

    // Recycle an inode entry.
    if (empty == NULL)
        panic("iget: no inodes");

    inode_ptr = empty;

    // open root directory via fatfs interface
    FRESULT result = f_opendir(&inode_ptr->dir, "/");
    if (result != FR_OK) {
        printf("f_opendir failed: %d\n", result);
        panic("iget_root: f_opendir failed");
    }

    // fill other fields in inode
    inode_ptr->dev = ROOTDEV;
    inode_ptr->ref = 1;
    inode_ptr->type = T_DIR;
    strcpy(inode_ptr->path, "/");
    inode_ptr->unlinked = 0;
    inode_ptr->new_path[0] = '\0';

//    acquire(&itable.lock);
    release_mutex_sleep(&itable.lock);
    return inode_ptr;
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode table entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void iput(struct inode *ip) {
//    tracecore("iput");
    KERNEL_ASSERT(ip != NULL, "inode can not be NULL");
//    acquire(&itable.lock);
    acquire_mutex_sleep(&itable.lock);
    KERNEL_ASSERT(ip->ref > 0, "inode ref can not be 0");

    if (ip->ref == 1) {
        // close file/direcroty via fatfs interface
        if (ip->type == T_DIR) {
            // close directory via fatfs interface
            infof("iput: close directory %s", ip->path);
            FRESULT result = f_closedir(&ip->dir);
            if (result != FR_OK) {
                printf("iput: f_closedir failed, result = %d\n", result);
                panic("iput: f_closedir failed");
            }
        } else {
            // close file via fatfs interface
            infof("iput: closing file %s\n", ip->path);
            FRESULT result = f_close(&ip->file);
            if (result != FR_OK) {
                printf("iput: f_close failed, result = %d\n", result);
                panic("iput: f_close failed");
            }
        }
        // delete file/directory if it is unlinked
        // or rename to a new path
        if (ip->unlinked) {
            infof("iput: deleting file %s\n", ip->path);
            FRESULT result = f_unlink(ip->path);
            if (result != FR_OK) {
                infof("iput: f_unlink failed, result = %d\n", result);
//                panic("iput: f_unlink failed");
            }
        } else if (strlen(ip->new_path) != 0) {
            infof("iput: renaming file %s to %s\n", ip->path, ip->new_path);
            FRESULT result = f_rename(ip->path, ip->new_path);
            if (result != FR_OK) {
                infof("iput: f_rename failed, result = %d\n", result);
                panic("iput: f_rename failed");
            }
        }
    }
    ip->ref--;
//    release(&itable.lock);
    release_mutex_sleep(&itable.lock);
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode *
idup(struct inode *ip) {
//    tracecore("idup");
    KERNEL_ASSERT(ip != NULL, "inode can not be NULL");
//    acquire(&itable.lock);
    acquire_mutex_sleep(&itable.lock);
    ip->ref++;
//    release(&itable.lock);
    release_mutex_sleep(&itable.lock);
    return ip;
}

// Common idiom: unlock, then put.
void iunlockput(struct inode *ip) {
    iunlock(ip);
    iput(ip);
}

// Reads the inode from disk if necessary.
//void ivalid(struct inode *ip) {
//    debugcore("ivalid");
//
//    struct buf *bp;
//    struct dinode *dip;
//    if (ip->valid == 0) {
//        bp = acquire_buf_and_read(ip->dev, BLOCK_CONTAINING_INODE(ip->inum, sb));
//        dip = (struct dinode *)bp->data + ip->inum % INODES_PER_BLOCK;
//        ip->type = dip->type;
//        ip->size = dip->size;
//        memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
//        release_buf(bp);
//        ip->valid = 1;
//        if (ip->type == 0)
//            panic("ivalid: no type");
//    }
//}




// Read data from inode.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
//int readi(struct inode *ip, int user_dst, void *dst, uint off, uint n) {
//    // debugcore("readi");
//    uint tot, m;
//    struct buf *bp;
//
//    if (off > ip->size || off + n < off)
//        return 0;
//    if (off + n > ip->size)
//        n = ip->size - off;
//
//    for (tot = 0; tot < n; tot += m, off += m, dst += m) {
//        bp = acquire_buf_and_read(ip->dev, bmap(ip, off / BSIZE));
//        m = MIN(n - tot, BSIZE - off % BSIZE);
//        if (either_copyout((char *)dst, (char *)bp->data + (off % BSIZE), m, user_dst) == -1) {
//            release_buf(bp);
//            tot = -1;
//            break;
//        }
//        release_buf(bp);
//    }
//    return tot;
//}

int readi(struct inode *ip, int user_dst, void *dst, uint off, uint n) {
    KERNEL_ASSERT(ip != NULL, "inode can not be NULL");

    // make sure the offset is valid
    // file size == offset is not allowed in read operation
    uint filesize = f_size(&ip->file);
    if (filesize <= off) {
        return 0;
    }
    // fix n by the file size
    if (filesize < off + n) {
        n = filesize - off;
    }

    uint off_align;
    uint64 data_align;
    uint64 n_dup = n;
    uint64 len = n;
    while (len > 0) {
        off_align = PGROUNDDOWN(off);
        struct page_cache *cache = ctable_acquire(ip, off_align);
        if (cache == NULL) {
            return 0;
        }
        data_align = (uint64)cache->page;
        n = PGSIZE - (off - off_align);
        if (n > len) {
            n = len;
        }
        if (either_copyout((char *)dst, (char *)data_align + (off - off_align), n, user_dst) == -1) {
            release_mutex_sleep(&cache->lock);
            return 0;
        }
        len -= n;
        dst += n;
        off = off_align + PGSIZE;
        release_mutex_sleep(&cache->lock);
    }
    return n_dup;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
//int writei(struct inode *ip, int user_src, void *src, uint off, uint n) {
//    uint tot, m;
//    struct buf *bp;
//
//    if (off > ip->size || off + n < off)
//        return -1;
//    if (off + n > MAXFILE * BSIZE)
//        return -1;
//
//    for (tot = 0; tot < n; tot += m, off += m, src += m) {
//        bp = acquire_buf_and_read(ip->dev, bmap(ip, off / BSIZE));
//        m = MIN(n - tot, BSIZE - off % BSIZE);
//        if (either_copyin((char *)bp->data + (off % BSIZE), (char *)src, m, user_src) == -1) {
//            release_buf(bp);
//            break;
//        }
//        write_buf_to_disk(bp);
//        release_buf(bp);
//    }
//
//    if (off > ip->size)
//        ip->size = off;
//
//    // write the i-node back to disk even if the size didn't change
//    // because the loop above might have called bmap() and added a new
//    // block to ip->addrs[].
//    iupdate(ip);
//
//    return tot;
//}

int writei(struct inode *ip, int user_src, void *src, uint off, uint n) {
    KERNEL_ASSERT(ip != NULL, "inode can not be NULL");

    // expand file if necessary
    if (off + n > f_size(&ip->file) && f_lseek(&ip->file, off + n) != FR_OK) {
        return 0;
    }
    infof("writei: off: %d, n: %d, file size: %d", off, n, f_size(&ip->file));

    uint off_align;
    uint64 data_align;
    uint64 n_dup = n;
    uint64 len = n;
    while (len > 0) {
        off_align = PGROUNDDOWN(off);
        struct page_cache *cache = ctable_acquire(ip, off_align);
        if (cache == NULL) {
            infof("writei: acquire page_cache failed");
            return 0;
        }
        data_align = (uint64)cache->page;
        n = PGSIZE - (off - off_align);
        if (n > len) {
            n = len;
        }
        if (either_copyin((char *)data_align + (off - off_align), (char *)src, n, user_src) == -1) {
            release_mutex_sleep(&cache->lock);
            infof("writei: copyin failed");
            return 0;
        }
        len -= n;
        src += n;
        off = off_align + PGSIZE;
        cache->dirty = TRUE;
        release_mutex_sleep(&cache->lock);
    }
    infof("writei: write %d bytes to disk", n_dup);
    return n_dup;
//    // seek to the right position
//    if (f_lseek(&ip->file, off) != FR_OK) {
//        return 0;
//    }
//
//    uint total = 0;
//    // read data
//    if (!user_src) {
//        // kernel address, write directly
//        if (f_write(&ip->file, src, n, &total) != FR_OK) {
//            return 0;
//        }
//        return total;
//    } else {
//        // user address, copyin
//        char buf[n];
//        if (copyin(curr_proc()->pagetable, buf, src, n) < 0) {
//            return 0;
//        }
//        if (f_write(&ip->file, buf, n, &total) != FR_OK) {
//            return 0;
//        }
//        return total;
//    }
}


// Lock the given inode.
// Reads the inode from disk if necessary.
void ilock(struct inode *ip) {

    if (ip == 0 || ip->ref < 1)
        panic("ilock");

    acquire_mutex_sleep(&ip->lock);
}

// Unlock the given inode.
void iunlock(struct inode *ip) {
//    print_inode(ip);
    KERNEL_ASSERT(ip != NULL, "inode can not be NULL");
    KERNEL_ASSERT(holdingsleep(&ip->lock), "inode is not locked");
    KERNEL_ASSERT(ip->ref >= 1, "inode ref can not be 0");
//    if (ip == NULL || !holdingsleep(&ip->lock) || ip->ref < 1)
//        panic("iunlock");

    release_mutex_sleep(&ip->lock);
}


struct inode *
inode_by_name(char *path) {
    char name[DIRSIZ] = {};
    return inode_or_parent_by_name(path, 0, name);
}

struct inode *
inode_parent_by_name(char *path, char *name) {
    return inode_or_parent_by_name(path, 1, name);
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode *
inode_or_parent_by_name(char *path, int nameiparent, char *name) {
    struct inode *ip, *next;
    debugcore("inode_or_parent_by_name, path: %s, nameiparent: %d, name: %s\n", path, nameiparent, name);

    // only if the current process is the shell, cwd can be NULL
    // because fs may sleep, so we can't initialize it in the kernel init code
    if (curr_proc()->cwd == NULL) {
        curr_proc()->cwd = iget_root();
    }

    if (*path == '/') {
        // absolute path
        ip = iget_root();
    } else {
        // relative path
        ip = idup(curr_proc()->cwd);
    }

    while ((path = skipelem(path, name)) != 0) {
        ilock(ip);
        if (ip->type != T_DIR) {
            iunlockput(ip);
            return 0;
        }
        if (nameiparent && *path == '\0') {
            // Stop one level early.
            iunlock(ip);
            return ip;
        }
        if ((next = dirlookup(ip, name)) == 0) {
            iunlockput(ip);
            return 0;
        }
        iunlockput(ip);
        ip = next;
    }
    if (nameiparent) {
        iput(ip);
        return 0;
    }
    return ip;
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
//static uint
//bmap(struct inode *ip, uint bn) {
//    uint addr, *a;
//    struct buf *bp;
//
//    if (bn < NDIRECT) {
//        if ((addr = ip->addrs[bn]) == 0)
//            ip->addrs[bn] = addr = alloc_zeroed_block(ip->dev);
//        return addr;
//    }
//    bn -= NDIRECT;
//
//    if (bn < NINDIRECT) {
//        // Load indirect block, allocating if necessary.
//        if ((addr = ip->addrs[NDIRECT]) == 0)
//            ip->addrs[NDIRECT] = addr = alloc_zeroed_block(ip->dev);
//        bp = acquire_buf_and_read(ip->dev, addr);
//        a = (uint *)bp->data;
//        if ((addr = a[bn]) == 0) {
//            a[bn] = addr = alloc_zeroed_block(ip->dev);
//            write_buf_to_disk(bp);
//        }
//        release_buf(bp);
//        return addr;
//    }
//
//    panic("bmap: out of range");
//    return 0;
//}

// Truncate inode (discard contents).
// Caller must hold ip->lock.
//void itrunc(struct inode *ip) {
//    tracecore("itrunc");
//    int i, j;
//    struct buf *bp;
//    uint *a;
//
//    for (i = 0; i < NDIRECT; i++) {
//        if (ip->addrs[i]) {
//            free_disk_block(ip->dev, ip->addrs[i]);
//            ip->addrs[i] = 0;
//        }
//    }
//
//    if (ip->addrs[NDIRECT]) {
//        bp = acquire_buf_and_read(ip->dev, ip->addrs[NDIRECT]);
//        a = (uint *)bp->data;
//        for (j = 0; j < NINDIRECT; j++) {
//            if (a[j])
//                free_disk_block(ip->dev, a[j]);
//        }
//        release_buf(bp);
//        free_disk_block(ip->dev, ip->addrs[NDIRECT]);
//        ip->addrs[NDIRECT] = 0;
//    }
//
//    ip->size = 0;
//    iupdate(ip);
//}

// do unlock and put, because we should close the file at first, iput is not compatible
//void itrunc(struct inode *ip) {
//
//    KERNEL_ASSERT(ip != NULL, "itrunc: inode is NULL");
//
//    ctable_release(ip);
//    iunlockput(ip);
//
//    KERNEL_ASSERT(ip->ref == 0, "itrunc: ref is not 0");
//    FRESULT result;
//    result = f_unlink(ip->path);
//    KERNEL_ASSERT(result == FR_OK, "itrunc: f_unlink failed");
//
//}

static void ienable_fastseek(struct inode *ip) {
//    KERNEL_ASSERT(ip != NULL, "ienable_fastseek: inode is NULL");
//    KERNEL_ASSERT(ip->type == T_FILE, "ienable_fastseek: inode is not a file");
//    ip->clmt[0] = NFASTSEEK;
//    ip->file.cltbl = ip->clmt;
//    FRESULT res = f_lseek(&ip->file, CREATE_LINKMAP);
//    KERNEL_ASSERT(res == FR_OK, "ienable_fastseek: enable_fastseek failed");
}

struct inode *
dirlookup(struct inode *dp, char *name) {
    KERNEL_ASSERT(dp->type == T_DIR, "dirlookup: not a directory");

    // get absolute path of the queried entity
    char path[MAXPATH];
    memmove(path, dp->path, MAXPATH);
    if (path[strlen(path) - 1] != '/') {
        strcat(path, "/");
    }
    strcat(path, name);
    infof("dirlookup: path: %s\n", path);

    // get the inode of the queried entity
    struct inode *inode_ptr, *empty;
//    acquire(&itable.lock);
    acquire_mutex_sleep(&itable.lock);
    // Is the inode already in the table?
    empty = NULL;
    for (inode_ptr = &itable.inode[0]; inode_ptr < &itable.inode[NINODE]; inode_ptr++) {
        if (inode_ptr->ref > 0 && inode_ptr->dev == ROOTDEV && strcmp(inode_ptr->path, path) == 0) {
            inode_ptr->ref++;
//            release(&itable.lock);
            release_mutex_sleep(&itable.lock);
            return inode_ptr;
        }
        if (empty == 0 && inode_ptr->ref == 0) // Remember empty slot.
            empty = inode_ptr;
    }

    // Recycle an inode entry.
    if (empty == NULL)
        panic("iget: no inodes");

    inode_ptr = empty;

    // try to open root directory via fatfs interface
    if (f_opendir(&inode_ptr->dir, path) == FR_OK) {
        inode_ptr->dev = ROOTDEV;
        inode_ptr->ref = 1;
        inode_ptr->type = T_DIR;
        strcpy(inode_ptr->path, path);
        inode_ptr->unlinked = 0;
        inode_ptr->new_path[0] = '\0';
//        release(&itable.lock);
        release_mutex_sleep(&itable.lock);
        return inode_ptr;

        // try to open file via fatfs interface
    } else if (f_open(&inode_ptr->file, path, FA_READ | FA_WRITE) == FR_OK) {

        // check special file type (device)
        struct device devinfo = {};
        struct symlink symlink_info = {};
        FIL symlink_file = {};
        uint br = 0;
        if (f_rewind(&inode_ptr->file) == FR_OK &&
            f_read(&inode_ptr->file, &devinfo, sizeof(devinfo), &br) == FR_OK &&
            br == sizeof(devinfo) &&
            devinfo.magic == DEVICE_MAGIC) {
            infof("dirlookup: open device: %s", path);
            inode_ptr->dev = ROOTDEV;
            inode_ptr->ref = 1;
            inode_ptr->type = T_DEVICE;
            inode_ptr->device.major = devinfo.major;
            inode_ptr->device.minor = devinfo.minor;
            strcpy(inode_ptr->path, path);
            inode_ptr->unlinked = 0;
            inode_ptr->new_path[0] = '\0';
//            release(&itable.lock);
            release_mutex_sleep(&itable.lock);
            return inode_ptr;

        } else if (f_rewind(&inode_ptr->file) == FR_OK &&
                   f_read(&inode_ptr->file, &symlink_info, sizeof(symlink_info), &br) == FR_OK &&
                   br > sizeof(int) &&
                   symlink_info.magic == SYMLINK_MAGIC &&
                   symlink_info.path[0] == '/') {
            infof("dirlookup: open symlink: %s, linkto %s", path, symlink_info.path);
            // close old file
            f_close(&inode_ptr->file);
            // record new file
            if (f_open(&inode_ptr->file, symlink_info.path, FA_READ | FA_WRITE) != FR_OK) {
                infof("dirlookup: symlink destination is invalid: %s", symlink_info.path);
                return NULL;
            }
            inode_ptr->dev = ROOTDEV;
            inode_ptr->ref = 1;
            inode_ptr->type = T_FILE;
            strcpy(inode_ptr->path, symlink_info.path);
            inode_ptr->unlinked = 0;
            inode_ptr->new_path[0] = '\0';
//            release(&itable.lock);
            release_mutex_sleep(&itable.lock);
            ienable_fastseek(inode_ptr);
            return inode_ptr;

        } else {
            infof("dirlookup: open file: %s", path);
            inode_ptr->dev = ROOTDEV;
            inode_ptr->ref = 1;
            inode_ptr->type = T_FILE;
            strcpy(inode_ptr->path, path);
            inode_ptr->unlinked = 0;
            inode_ptr->new_path[0] = '\0';
//            release(&itable.lock);
            release_mutex_sleep(&itable.lock);
            ienable_fastseek(inode_ptr);
            return inode_ptr;
        }

    } else {
//        release(&itable.lock);
        release_mutex_sleep(&itable.lock);
        return NULL;
    }
}

struct inode *
icreate(struct inode *dp, char *name, int type, int major, int minor) {
    KERNEL_ASSERT(dp->type == T_DIR, "icreate_file: not a directory");

    infof("icreate: %s\n", name);
    infof("current directory: %s\n", dp->path);
    // get absolute path of the queried entity
    char path[MAXPATH];
    memmove(path, dp->path, MAXPATH);
    if (path[strlen(path) - 1] != '/') {
        strcat(path, "/");
    }
    strcat(path, name);
    infof("icreate: path: %s\n", path);

    // create the inode of the queried entity
    // get the inode of the queried entity
    struct inode *inode_ptr, *empty;
//    acquire(&itable.lock);
    acquire_mutex_sleep(&itable.lock);
    // Is the inode already in the table?
    empty = NULL;
    for (inode_ptr = &itable.inode[0]; inode_ptr < &itable.inode[NINODE]; inode_ptr++) {
        if (inode_ptr->ref > 0 && inode_ptr->dev == ROOTDEV && strcmp(inode_ptr->path, path) == 0) {
            inode_ptr->ref++;
//            release(&itable.lock);
            release_mutex_sleep(&itable.lock);
            return inode_ptr;
        }
        if (empty == 0 && inode_ptr->ref == 0) // Remember empty slot.
            empty = inode_ptr;
    }

    // Recycle an inode entry.
    if (empty == NULL)
        panic("iget: no inodes");

    inode_ptr = empty;
    FRESULT result;

    infof("inode_ptr: %p\n", inode_ptr);

    if (type == T_DIR) {
        infof("icreate::dir: %s\n", path);
        // create directory via fatfs interface
        if ((result = f_mkdir(path)) != FR_OK) {
            infof("icreate::dir: f_mkdir failed: %d\n", result);
//            release(&itable.lock);
            release_mutex_sleep(&itable.lock);
            return NULL;
        }
        if ((result = f_opendir(&inode_ptr->dir, path)) != FR_OK) {
            infof("icreate::dir: f_opendir failed: %d\n", result);
//            release(&itable.lock);
            release_mutex_sleep(&itable.lock);
            return NULL;
        }
        inode_ptr->dev = ROOTDEV;
        inode_ptr->ref = 1;
        inode_ptr->type = T_DIR;
        strcpy(inode_ptr->path, path);
        inode_ptr->unlinked = 0;
        inode_ptr->new_path[0] = '\0';
//        release(&itable.lock);
        release_mutex_sleep(&itable.lock);
        return inode_ptr;

    } else if (type == T_FILE) {
        infof("icreate::file: %s\n", path);
        // create file via fatfs interface
        if ((result = f_open(&inode_ptr->file, path, FA_CREATE_ALWAYS | FA_WRITE | FA_READ)) != FR_OK) {
            infof("icreate::file: f_open failed: %d\n", result);
//            release(&itable.lock);
            release_mutex_sleep(&itable.lock);
            return NULL;
        }
        inode_ptr->dev = ROOTDEV;
        inode_ptr->ref = 1;
        inode_ptr->type = T_FILE;
        strcpy(inode_ptr->path, path);
        inode_ptr->unlinked = 0;
        inode_ptr->new_path[0] = '\0';
//        release(&itable.lock);
        release_mutex_sleep(&itable.lock);
        ienable_fastseek(inode_ptr);
        return inode_ptr;

    } else if (type == T_DEVICE) {
        infof("icreate::device: %s\n", path);
        // create device via fatfs interface
        struct device devinfo = {
                .magic = DEVICE_MAGIC,
                .major = major,
                .minor = minor
        };
        uint bw = 0;
        if ((result = f_open(&inode_ptr->file, path, FA_CREATE_ALWAYS | FA_WRITE | FA_READ)) != FR_OK ||
            (result = f_write(&inode_ptr->file, &devinfo, sizeof(devinfo), &bw)) != FR_OK ||
            bw != sizeof(devinfo)) {
            infof("icreate::device: f_open/f_write failed: %d\n", result);
//            release(&itable.lock);
            release_mutex_sleep(&itable.lock);
            return NULL;
        }
        inode_ptr->dev = ROOTDEV;
        inode_ptr->ref = 1;
        inode_ptr->type = T_DEVICE;
        inode_ptr->device.major = major;
        inode_ptr->device.minor = minor;
        strcpy(inode_ptr->path, path);
        inode_ptr->unlinked = 0;
        inode_ptr->new_path[0] = '\0';
//        release(&itable.lock);
        release_mutex_sleep(&itable.lock);
        return inode_ptr;

    } else {
        infof("icreate: unknown type: %d\n", type);
//        release(&itable.lock);
        release_mutex_sleep(&itable.lock);
        return NULL;
    }
}

void print_inode(struct inode *ip) {
    printf("inode: %p\n", ip);
    printf("  dev: %d\n", ip->dev);
    printf("  ref: %d\n", ip->ref);
    switch (ip->type) {
        case T_DIR:
            printf("  type: directory\n");
            break;
        case T_FILE:
            printf("  type: file\n");
            break;
        case T_DEVICE:
            printf("  type: device\n");
            break;
        default:
            printf("  type: unknown\n");
            break;
    }
    printf("  path: %s\n", ip->path);
}

void inode_test() {
    struct inode *root = iget_root();
    print_inode(root);
    struct inode *file = icreate(root, "test.txt", T_FILE, 0, 0);
    print_inode(file);



    panic("inode_test complete");
}

int igetdents(struct inode* dp, char *buf, unsigned long len) {
    infof("igetdents: %s\n", dp->path);

    if (dp->type != T_DIR) {
        infof("igetdents: not a dir (2)");
        return -1;
    }

    // ref: http://elm-chan.org/fsw/ff/doc/readdir.html

    FRESULT res;
    static FILINFO fno;
    int namelen;
    struct linux_dirent64* curr;

//    if ((res = f_rewinddir(&dp->dir)) != FR_OK) {
//        infof("igetdents: f_rewinddir failed: %d", res);
//        return -1;
//    }

    curr = (struct linux_dirent64*)buf;

    for (;;) {
        res = f_readdir(&dp->dir, &fno);       /* Read a directory item */
        if (res != FR_OK) {
            infof("igetdents: f_readdir failed: %d", res);
            return -1;
        }
        if (fno.fname[0] == 0) {
            break;                             /* Break on end of directory */
        }

        namelen = strlen(fno.fname);
        if ((uint64)curr + sizeof(struct linux_dirent64) + namelen + 1 > (uint64)buf + len) {
            infof("igetdents: buffer overflow");
            return (uint64)curr - (uint64)buf;
        }
        // fat32 not depends on ino
        curr->d_ino = 0;
        curr->d_off = ((uint64)curr - (uint64)buf) + sizeof(struct linux_dirent64) + namelen + 1;
        curr->d_reclen = sizeof(struct linux_dirent64) + namelen + 1;
        // only support distinguish regular file and directory now
        curr->d_type = fno.fattrib & AM_DIR ? DT_DIR : DT_REG;
        strcpy(curr->d_name, fno.fname);

        curr = (struct linux_dirent64*)((uint64)curr + curr->d_reclen);
    }

    return (uint64)curr - (uint64)buf;
}

int stati(struct inode *ip, struct kstat *st) {
    infof("stati: %s type: %d", ip->path, ip->type);
    memset(st, 0, sizeof(struct kstat));
    st->st_dev = ip->dev;
    // fat32 doesn't support hardlink, so nlink must be 1
    st->st_nlink = 1;
    // fill st_blksize
    st->st_blksize = BSIZE;
    // fill st_mode and st_size according to inode type
    switch (ip->type) {
        case T_DIR:
            st->st_mode = S_IFDIR;
            st->st_size = 4;
            break;
        case T_FILE:
            st->st_mode = S_IFREG;
            st->st_size = f_size(&ip->file);
            break;
        case T_DEVICE:
            st->st_mode = S_IFCHR;
            st->st_size = f_size(&ip->file);
            break;
    }
    st->st_blocks = ROUNDUP(st->st_size, BSIZE) / BSIZE;
    return 0;
}

int ilink(struct inode *oldip, struct inode *newip) {
    int len = strlen(oldip->path);
    // contains delimiter
    int magic = SYMLINK_MAGIC;
    if (writei(newip, FALSE, &magic, 0, sizeof(magic)) != sizeof(magic)) {
        infof("ilink: writei failed");
        return -1;
    }
    if (writei(newip, FALSE, oldip->path, sizeof(magic), len + 1) != len + 1) {
        infof("ilink: writei failed");
        return -1;
    }
    // remove the new inode from the page cache
    // so we can read the symlink again to open the real file
    ctable_release(newip);
    return 0;
}

int iunlink(struct inode *ip) {
    // just record the inode as deleted
    // when there's no reference to the inode, the file will be deleted
    ip->unlinked = 1;
    return 0;
}

void itrunc(struct inode *ip) {
    infof("itrunc: %s", ip->path);
    KERNEL_ASSERT(ip->type == T_FILE, "itrunc: not a file");
    KERNEL_ASSERT(f_rewind(&ip->file) == FR_OK, "itrunc: f_rewind failed");
    KERNEL_ASSERT(f_truncate(&ip->file) == FR_OK, "itrunc: f_truncate failed");
}

int ipath(struct inode *ip, char *path) {
    strcpy(path, ip->path);
    return 0;
}

int irename(struct inode *ip, const char *new_path) {
    infof("irename: %s to %s", ip->path, new_path);
    strcpy(ip->new_path, new_path);
    return 0;
}