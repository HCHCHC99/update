#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H

/*
 * ============================================================
 * 内存 / Flash 分区唯一宏定义源
 * ------------------------------------------------------------
 * 所�? APP1/APP2 起始地址、分区大小、TBOX 槽位标签、RAM 布局�?
 * OTA 固件大小限制都集中在本文件�?
 * 修改分区只需改本文件，并同步 app1/app2 �? Keil Target IROM1
 * （Start/Size）链接设置�?
 * ============================================================
 */

/* ========== 芯片 / 扇区 ========== */
#define FLASH_SECTOR_SIZE           0x2000UL        /* 8KB/扇区 */
#define FLASH_TOTAL_SIZE            0x80000UL       /* 512KB (HC32F460xE) */
#define RAM_START_ADDR              0x1FFF8000UL
#define RAM_SIZE                    0x2F000UL       /* 188KB */
#define RAM_END_ADDR                (RAM_START_ADDR + RAM_SIZE)

/* ========== 系统保留�? ========== */
#define BOOT_START_ADDR             0x00000000UL
#define BOOT_LINK_SIZE              0x00014000UL    /* boot 链接�? 80KB（镜像约 47KB�? */
#define UDS_SHARED_SECTOR_BASE      0x00010000UL    /* 扇区 8 */
#define FLASHADV_MGMT_SECTOR_BASE   0x00012000UL    /* 扇区 9 */
#define APP1_STATE_SECTOR_BASE      0x00016000UL    /* 扇区 11 */
#define APP2_STATE_SECTOR_BASE      0x00018000UL    /* 扇区 12 */
#define WDT_FEED_CONTROL_APP1_ADDR  (APP1_STATE_SECTOR_BASE + 0x000)
#define WDT_COUNT_APP1_ADDR         (APP1_STATE_SECTOR_BASE + 0x008)
#define WDT_FEED_CONTROL_APP2_ADDR  (APP2_STATE_SECTOR_BASE + 0x000)
#define WDT_COUNT_APP2_ADDR         (APP2_STATE_SECTOR_BASE + 0x008)
#define SHARED_RAM_BASE_ADDR        0x1FFF8000UL
#define SHARED_CTRL_OFFSET          0x2F000UL
#define SHARED_CTRL_ADDR            (SHARED_RAM_BASE_ADDR + SHARED_CTRL_OFFSET - 0x100)

/* ========== APP 分区�?168KB / 168KB，扇�? 13~54 平分�? ========== */
#define APP1_START_ADDR             0x0001A000UL    /* 扇区 13 */
#define APP2_START_ADDR             0x00044000UL    /* 扇区 34 */
#define APP_MAX_SIZE                0x0002A000UL    /* 168KB */
#define APP1_END_ADDR               (APP1_START_ADDR + APP_MAX_SIZE)  /* 0x00044000 */
#define APP2_END_ADDR               (APP2_START_ADDR + APP_MAX_SIZE)  /* 0x0006E000 */

/* 兼容旧名（原 flash_download.h 中的定义�? */
#define FW_APP1_START_ADDR          APP1_START_ADDR
#define FW_APP_START_ADDR           APP2_START_ADDR
#define FW_APP_MAX_SIZE             APP_MAX_SIZE
#define FW_BOOTLOADER_START_ADDR    BOOT_START_ADDR

/* ========== 参数�? / 跳转�? ========== */
#define PARAM_MANAGER_START_ADDR    0x00070000UL    /* 扇区 56 */
#define APP_RUN_SLOT_ADDR           0x0007C000UL    /* 扇区 62 */

/* ========== TBOX 槽位标签（客户协议固定，仅表示烧录到哪个槽，不校验区间） ========== */
#define TBOX_ADDR_APP1              0x08018000UL    /* 标签：烧录到 APP1�?0x1A000�? */
#define TBOX_ADDR_APP2              0x08004000UL    /* 标签：烧录到 APP2�?0x44000�? */

#define MAP_TBOX_ADDR_TO_FLASH(addr) \
    (((addr) == TBOX_ADDR_APP1) ? APP1_START_ADDR : \
     ((addr) == TBOX_ADDR_APP2) ? APP2_START_ADDR : (addr))

/* ========== OTA 限制 ========== */
#define FW_MAX_FIRMWARE_SIZE        APP_MAX_SIZE    /* 固件最�? = APP 分区大小 = 168KB */
#define FW_RAM_BUFFER_SIZE          (8 * 1024)      /* 0x36 块对齐暂存（仅对�?/调试用） */

#endif /* MEMORY_MAP_H */