# OTA工程新APP分区：memory_map.h 修改说明

> 日期：2026-08-04
> 适用范围：`D:\ota_ddl3.3_v.3.1\merge_v.2.0.0`（boot / app1 / app2 三个工程各有一份 memory_map.h）
> 目标：扇区 13~54 平分给 APP1/APP2（168KB / 168KB），APP2 首地址从 `0x4C000` 改为 `0x44000`

---

## 一、新布局

扇区 13~54 共 42 个扇区（8KB/扇区），平分 = 每边 21 个扇区 = **168KB（0x2A000）**

| 槽位 | 扇区 | 地址范围 | 大小 |
|---|---|---|---|
| **APP1** | 13–33 | `0x1A000 ~ 0x43FFF` | 168KB |
| **APP2** | 34–54 | `0x44000 ~ 0x6DFFF` | 168KB |

- APP2 结束 `0x6E000` 正好是 55 号扇区起点，**不占用扇区55**（四驱参数存储）✅
- 每个工程的文件位置：`<工程>\projects\ev_hc32f460_lqfp100_v2\Bootloader_App\memory_map.h`

---

## 二、memory_map.h 需要改的宏（boot / app1 / app2 三份同步改）

| 宏 | 旧值 | 新值 |
|---|---|---|
| `APP2_START_ADDR` | `0x0004C000UL` | `0x00044000UL` |
| `APP_MAX_SIZE` | `0x00014000UL`（80KB） | `0x0002A000UL`（168KB） |
| `APP1_END_ADDR` | （自动） | `APP1_START_ADDR + APP_MAX_SIZE` = `0x44000` |
| `APP2_END_ADDR` | （自动） | `APP2_START_ADDR + APP_MAX_SIZE` = `0x6E000` |
| `TBOX_ADDR_APP1_END` | （自动） | `TBOX_ADDR_APP1_START + APP_MAX_SIZE` = `0x08042000` |
| `TBOX_ADDR_APP2_START` | `0x08004000UL` | `(TBOX_ADDR_APP1_END)` = `0x08042000UL` |
| `TBOX_ADDR_APP2_END` | （自动） | `TBOX_ADDR_APP2_START + APP_MAX_SIZE` = `0x0806C000` |

不用改：`APP1_START_ADDR`（0x1A000）、`FW_APP1_START_ADDR/FW_APP_START_ADDR/FW_APP_MAX_SIZE`（别名自动跟随）、`FW_MAX_FIRMWARE_SIZE`（=APP_MAX_SIZE 自动变 168KB）、`FW_RAM_BUFFER_SIZE`（8KB，够用）、跳转逻辑（`APP2_START_ADDR` 宏驱动）。

---

## 三、完整新版 memory_map.h（可直接覆盖三份）

```c
#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H

/*
 * ============================================================
 * 内存 / Flash 分区唯一宏定义源
 * 新分区：扇区 13~54 平分（168KB / 168KB）
 *   APP1 0x1A000 / APP2 0x44000 / 结束 0x6E000（不碰扇区55）
 * 修改分区只需改本文件，并同步 app1/app2 的 Keil Target IROM1
 * （Start/Size）链接设置，以及 TBOX 侧 0x34 地址窗口。
 * ============================================================
 */

/* ========== 芯片 / 扇区 ========== */
#define FLASH_SECTOR_SIZE           0x2000UL        /* 8KB/扇区 */
#define FLASH_TOTAL_SIZE            0x80000UL       /* 512KB (HC32F460xE) */
#define RAM_START_ADDR              0x1FFF8000UL
#define RAM_SIZE                    0x2F000UL       /* 188KB */
#define RAM_END_ADDR                (RAM_START_ADDR + RAM_SIZE)

/* ========== 系统保留区 ========== */
#define BOOT_START_ADDR             0x00000000UL
#define BOOT_LINK_SIZE              0x00014000UL    /* boot 链接区 80KB（镜像约 47KB） */
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

/* ========== APP 分区（新方案：扇区 13~54 平分 = 168KB / 168KB） ========== */
#define APP1_START_ADDR             0x0001A000UL    /* 扇区 13 */
#define APP2_START_ADDR             0x00044000UL    /* 扇区 34 */
#define APP_MAX_SIZE                0x0002A000UL    /* 168KB */
#define APP1_END_ADDR               (APP1_START_ADDR + APP_MAX_SIZE)  /* 0x44000 */
#define APP2_END_ADDR               (APP2_START_ADDR + APP_MAX_SIZE)  /* 0x6E000 */

/* 兼容旧名（原 flash_download.h 中的定义） */
#define FW_APP1_START_ADDR          APP1_START_ADDR
#define FW_APP_START_ADDR           APP2_START_ADDR
#define FW_APP_MAX_SIZE             APP_MAX_SIZE
#define FW_BOOTLOADER_START_ADDR    BOOT_START_ADDR

/* ========== 参数区 / 跳转槽 ========== */
#define PARAM_MANAGER_START_ADDR    0x00070000UL    /* 扇区 56 */
#define APP_RUN_SLOT_ADDR           0x0007C000UL    /* 扇区 62 */

/* ========== TBOX 地址窗口（新方案 168KB，APP1 窗口起点保持兼容） ========== */
#define TBOX_ADDR_APP1_START        0x08018000UL
#define TBOX_ADDR_APP1_END          (TBOX_ADDR_APP1_START + APP_MAX_SIZE)  /* 0x08042000 */
#define TBOX_ADDR_APP2_START        (TBOX_ADDR_APP1_END)                   /* 0x08042000 */
#define TBOX_ADDR_APP2_END          (TBOX_ADDR_APP2_START + APP_MAX_SIZE)  /* 0x0806C000 */

#define MAP_TBOX_ADDR_TO_FLASH(addr) \
    (((addr) >= TBOX_ADDR_APP1_START && (addr) < TBOX_ADDR_APP1_END) ? \
     ((addr) - TBOX_ADDR_APP1_START + APP1_START_ADDR) : \
    (((addr) >= TBOX_ADDR_APP2_START && (addr) < TBOX_ADDR_APP2_END) ? \
     ((addr) - TBOX_ADDR_APP2_START + APP2_START_ADDR) : (addr)))

/* ========== OTA 限制 ========== */
#define FW_MAX_FIRMWARE_SIZE        APP_MAX_SIZE    /* 固件最大 = APP 分区大小 = 168KB */
#define FW_RAM_BUFFER_SIZE          (8 * 1024)      /* 0x36 块对齐暂存（仅对齐/调试用） */

#endif /* MEMORY_MAP_H */
```

---

## 四、其它必须同步修改的地方

| 位置 | 修改 |
|---|---|
| **Keil Target IROM1** | app1：Start `0x1A000`（不变），Size `0x14000` → **`0x2A000`**；app2：Start `0x4C000` → **`0x44000`**，Size `0x14000` → **`0x2A000`**；boot 不变 |
| **uvprojx**（`template - 副本.uvprojx`） | app1/app2 的 IROM1 Start/Size 对应修改（改前先关 Keil） |
| **TBOX 协议侧** | 0x34 地址窗口：烧 APP1 发 `0x08018000`（不变），烧 APP2 发 `0x08042000` |
| **四驱工程** | `D:\260706_NL` 的 `memory_map.h` 按 `四驱新APP分区.md` 同步（同一套布局） |

---

## 五、验证清单

- [ ] boot/app1/app2 三份 memory_map.h 内容一致
- [ ] Keil 编译通过：boot 无溢出；app1/app2 按新 IROM1 链接
- [ ] boot RTT 显示 `Max firmware size: 168 KB`、`User flash range` 正确
- [ ] TBOX 0x34 发 `0x08018000`（168KB）→ 映射 `0x1A000`，返回 `74 00 40 00`
- [ ] TBOX 0x34 发 `0x08042000`（168KB）→ 映射 `0x44000`，返回 `74 00 40 00`
- [ ] 0x36 传满 168KB（672 块，序号回绕已修复）→ 0x37 CRC 通过
- [ ] 擦除范围不越过 `0x6E000`（不碰扇区55）
- [ ] 复位后跳转槽 = 实际烧录槽位（烧 APP1 跳 APP1 / 烧 APP2 跳 APP2）