#include <fs.h>
#include <disk.h>
#include <memory.h>
#include <printk.h>
#include <global.h>
#include <dir.h>


extern ide_channel channels[CHANNEL_DEVICE_CNT];

static void init_super_block(disk *d, partition *part, super_block *sb) {
    uint32_t total_secs = part->count;

    uint32_t inode_bmap_secs = DIV_ROUND_UP(
        MAX_FILES_PER_PART, 
        BITS_PER_SECTOR);
    uint32_t inode_array_secs = DIV_ROUND_UP(
        sizeof(inode_t) * MAX_FILES_PER_PART, 
        BITS_PER_SECTOR);

    uint32_t fb_secs = total_secs - 2 - inode_bmap_secs - inode_array_secs;

    uint32_t fb_bmap_secs = DIV_ROUND_UP(fb_secs, BITS_PER_SECTOR);
    fb_bmap_secs = DIV_ROUND_UP(fb_secs - fb_bmap_secs, BITS_PER_SECTOR);

    sb->magic_num = 0xcafebebe;

    sb->block_cnt = total_secs;

    sb->inode_cnt = MAX_FILES_PER_PART;

    sb->start_lba = part->start;

    sb->fb_bmap_addr = sb->start_lba + 2;
    sb->fb_bmap_size = fb_bmap_secs;
    
    sb->in_bmap_addr = sb->fb_bmap_addr + sb->fb_bmap_size;
    sb->in_bmap_size = inode_bmap_secs;
    
    sb->in_array_addr = sb->in_bmap_addr + sb->in_bmap_size;
    sb->in_array_size = inode_array_secs;

    sb->data_start = sb->in_array_addr + sb->in_array_addr;
    sb->root_inode_no = 0;
    sb->root_dir_size = sizeof(dir_entry);

    printk("\n%s superblock build ok", part->name);
}
 
//初始化根目录，root dir位于数据块的第一个块，inode序号为0
static void set_root_dir(void *buf, 
                         uint32_t buf_size, 
                         disk *d, 
                         super_block *sb) {
    //root directory
    memset(buf, 0, buf_size);
    dir_entry *root = (dir_entry*)buf;

    memset(root->name, '.', 1);
    root->inode_no = 0;
    root->type = ET_DIRECTORY;
    ++root;

    memset(root->name, '.', 2);
    root->inode_no = 0;
    root->type = ET_DIRECTORY;
    
    ide_write(d, sb->data_start, buf, 1);
}

//初始化inode数组
static void set_inode_array(void *buf, 
                            uint32_t buf_size, 
                            disk *d, 
                            super_block *sb) {
    //inode array
    memset(buf, 0, buf_size);
    inode_t *i = (inode_t*)buf;
    i->i_size = sb->root_dir_size * 2;
    i->i_no = 0;
    i->i_sectors[0] = sb->data_start;
    ide_write(d, sb->in_array_addr, buf, sb->in_array_size);
}

static void set_inode_bitmap(void *buf,
                             uint32_t buf_size,
                             disk *d,
                             super_block *sb) {
    //inode bitmap
    memset(buf, 0, buf_size);
    ((uint8_t*)buf)[0] |= 0x1;
    ide_write(d, sb->in_bmap_addr, buf, sb->in_array_size);
}

static void set_free_block_bitmap(uint8_t *buf,
                                  uint32_t buf_size,
                                  disk *d,
                                  super_block *sb,
                                  partition *part) {
    uint32_t total_secs = part->count;
    uint32_t inode_bmap_secs = DIV_ROUND_UP(
        MAX_FILES_PER_PART, 
        BITS_PER_SECTOR);
    uint32_t inode_array_secs = DIV_ROUND_UP(
        sizeof(inode_t) * MAX_FILES_PER_PART, 
        BITS_PER_SECTOR);
    uint32_t fb_secs = total_secs - 2 - inode_bmap_secs - inode_array_secs;
    uint32_t fb_bmap_secs = DIV_ROUND_UP(fb_secs, BITS_PER_SECTOR);
    uint32_t fb_bmap_len = fb_secs - fb_bmap_secs;

    //block bitmap 第0块分配给根目录
    buf[0] |= 0x1;

    uint32_t last_byte = fb_bmap_len / 8;
    uint8_t last_bit = fb_bmap_len % 8;
    uint32_t last_size = SECTOR_SIZE - (last_byte % SECTOR_SIZE);

    //设置位图的最后字节
    memset(&buf[last_byte], 0xff, last_size);
    for (int i = 0; i <= last_bit; ++i) {
        buf[last_byte] &= ~(1 << i);
    }

    ide_write(d, sb->fb_bmap_addr, buf, sb->fb_bmap_size);
}

//格式化分区，创建文件系统
// ----------------------------------------------------------------
// | boot  | super | free block | inode  | inode | root |  free   |
// | block | block |   bitmap   | bitmap | array |  dir |  blocks |
// ----------------------------------------------------------------
static void build_fs(disk *d, partition *part) {
    super_block sb;
    init_super_block(d, part, &sb);

    //将初始化了的superblock写入disk
    ide_write(d, part->start + 1, &sb, 1);

    uint32_t buf_size = max(sb.fb_bmap_size, sb.in_bmap_size);
    uint8_t *buf = (uint8_t*)sys_malloc(buf_size);

    set_free_block_bitmap(buf, buf_size, d, &sb, part);
    
    set_inode_bitmap(buf, buf_size, d, &sb);
    
    set_inode_array(buf, buf_size, d, &sb);
    
    set_root_dir(buf, buf_size, d, &sb);

    sys_free(buf);
}

//文件系统初始化
void fs_init() {
    super_block *sb = (super_block*)sys_malloc(BLOCK_SIZE);

    ide_channel *channel = &channels[0];
    
    //hd80m.img
    disk *d = &channel->devices[1];

    partition *part = d->prim_parts;
    for (int i = 0; i < PRIM_PARTS_CNT; ++i) {
        if (part->count != 0) {
            memset(sb, 0, SECTOR_SIZE);
            ide_read(d, part->start + 1, sb, 1);

            //如果存在文件系统，就不需要build_fs
            if (sb->magic_num == 0xcafebebe) {
                printk("\n%s file system ok!", part->name);
            } else {
                build_fs(d, part);
                printk("\n%s build file system.", part->name);
            }
        }
    }

    part = d->logic_parts;
    for (int i = 0; i < LOGIC_PARTS_CNT; ++i) {
        if (part->count != 0) {
            memset(sb, 0, SECTOR_SIZE);
            ide_read(d, part->start + 1, sb, 1);

            //如果存在文件系统，就不需要build_fs
            if (sb->magic_num == 0xcafebebe) {
                printk("%s file system ok!\n", part->name);
            } else {
                build_fs(d, part);
                printk("%s build file system.\n", part->name);
            }
        }
    }

    sys_free(sb);
}