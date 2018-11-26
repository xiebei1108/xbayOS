#include <disk.h>
#include <global.h>
#include <io.h>
#include <debug.h>
#include <timer.h>

//获取channel对应的IO port
#define RW_DATA(channel)             (channel->start_port + 0x0)
#define R_ERROR_W_FEATURES(channel)  (channel->start_port + 0x1)
#define RW_SECTOR_CNT(channel)       (channel->start_port + 0x2)
#define RW_LBA_LOW(channel)          (channel->start_port + 0x3)
#define RW_LBA_MID(channel)          (channel->start_port + 0x4)
#define RW_LBA_HIGH(channel)         (channel->start_port + 0x5)
#define RW_DEVICE(channel)           (channel->start_port + 0x6)
#define R_STATUS_W_CMD(channel)      (channel->start_port + 0x7)
#define R_ALT_W_CTL(channel)         (channel->start_port + 0x206)

//alternate status寄存器
#define ALT_STATUS_BUSY         0x80
#define ALT_STATUS_DRIVER_READY 0x40
#define ALT_STATUS_DATA_READY   0x08

//device寄存器
#define DEV_MBS 0xa0
#define DEV_LBA 0x40
#define DEV_DEV 0x10

//hard disk operations
#define CMD_IDENTIFY        0xec
#define CMD_READ_SECTOR     0x20
#define CMD_WRITE_SECTOR    0x30
#define BYTES_PER_SECTOR    512


#define MAX_LBA  ((80 * 1024 * 1024 / 512) - 1)


uint8_t channel_cnt;
ide_channel channels[CHANNEL_DEVICE_CNT];

void ide_channel_init() {
    uint8_t disk_cnt = *((uint8_t*)DISK_CNT_POINTER);
    channel_cnt = DIV_ROUND_UP(disk_cnt, 2);

    ide_channel *channel;
    for (int i = 0; i < channel_cnt; ++i) {
        channel = &channels[i];

        if (i == 0) {
            //channel0，对应从片IRQ14
            channel->start_port = 0x1f0;
            channel->irq_no = 0x20 + 14;
        } else if (i == 1) {
            //channel1，对应从片IRQ15
            channel->start_port = 0x170;
            channel->irq_no = 0x20 + 15;
        }
        channel->waiting_intr = false;
        mutex_init(&channel->mutex);
        sem_init(&channel->sem, 0);    
    }
}

//根据传入的disk，通过其对应的channel，给响应IO端口写入数据，选择读写的硬盘
static void select_disk(disk *d) {
    uint8_t device = DEV_MBS | DEV_LBA | (d->no == 0 ? 0 : DEV_DEV);
    outb(RW_DEVICE(d->channel), device);
}

//在disk中，指定待读写的扇区数cnt
static void count_out(disk *d, uint8_t cnt) {
    ide_channel *channel = d->channel;
    outb(RW_SECTOR_CNT(channel), cnt);
}

//在disk中，选择起始地址为lba的扇区
//通过向disk d对应的channel写入对应寄存器的值
static void select_sector(disk *d, uint32_t lba) {
    ASSERT(lba <= MAX_LBA);
    ide_channel *channel = d->channel;
    outb(RW_LBA_LOW(channel), lba & 0x000000ff);
    outb(RW_LBA_MID(channel), (lba & 0x0000ff00) >> 8);
    outb(RW_LBA_HIGH(channel), (lba & 0x00ff0000) >> 16);
    
    //lba的24-27位存储在device寄存器0-3位
    uint8_t device = DEV_MBS | 
                     DEV_LBA | 
                     (d->no == 0 ? 0 : DEV_DEV) | 
                     ((lba & 0x0f000000) >> 24);
    outb(RW_DEVICE(channel), device);
}

//给channel发送命令cmd
static void send_cmd(ide_channel *channel, uint8_t cmd) {
    //写入命令，等待硬盘控制器发来的中断
    channel->waiting_intr = true;
    outb(R_STATUS_W_CMD(channel), cmd);
}

//从disk d中读取数据到buf中，待读取的数据扇区数为cnt
static void disk_read(disk *d, void *buf, uint8_t cnt) {
    uint32_t cnt_ = cnt == 0 ? 256 : cnt;
    uint32_t words = cnt_ * BYTES_PER_SECTOR / 2;
    insw(RW_DATA(d->channel), buf, words);
}

//向disk d写入数据，数据保存在buf中，待写入数据扇区数为cnt
static void disk_write(disk *d, void *buf, uint8_t cnt) {
    uint32_t cnt_ = cnt == 0 ? 256 : cnt;
    uint32_t words = cnt_ * BYTES_PER_SECTOR / 2;
    outsw(RW_DATA(d->channel), buf, words);
}

//等待30s，如果硬盘没有响应，那么返回false。否则，返回硬盘是否正常执行
static bool spin_wait(disk *d) {
    uint32_t ms = 30 * 1000; //max seconds to wait
    while (ms > 0) {
        uint8_t status = inb(R_STATUS_W_CMD(d->channel));
        if (!(status & ALT_STATUS_BUSY)) {
            return !!(status & ALT_STATUS_DATA_READY);
        }
        sleep_by_msecond(10);
        ms -= 10;
    }
    return false;
}