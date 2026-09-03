# Flash Partition Layout

> **相关文档:** [flash操作.md](flash操作.md) — Flash 函数全集 | [OTA-flash补充.md](OTA-flash补充.md) — OTA 跳转参数 & 空闲扇区

## Overview

HC32F460xE: 512KB Flash (`0x00000000` – `0x0007FFFF`), 64 sectors, sector size `0x2000` (8KB).

---

## 各模块扇区占用一览

```
扇区    地址        归属                     FlashAdvanced视角      实际写入方式
────     ────        ────                     ────────────────      ──────────
 0-5    0x00000     Bootloader (48KB 预留)     保护区 (0-12)         直接操作 EFM (镜像 46.55KB)
 6-7    0x0C000     预留 (16KB)               保护区 (0-12)         无
 8      0x10000     UDS Shared State          保护区 (0-12)         Bootloader_App.c 直接写
 9      0x12000     FlashAdvanced 管理记录     保护区 (0-12)         FlashAdvanced 自己维护
10      0x14000     预留                       保护区 (0-12)         无
11      0x16000     APP1 WDT State            保护区 (0-12)         Bootloader_App.c 直接写
12      0x18000     APP2 WDT State            保护区 (0-12)         Bootloader_App.c 直接写
────     ────        ────                     ────────────────      ──────────
13-22   0x1A000     APP1 固件 (80KB)          有效用户区 (13-61)    烧录 / 直接运行-37   0x2E000     空闲 (120KB)              有效用户区 (13-61)    无
38-47   0x4C000     APP2 OTA 固件 (80KB)      有效用户区 (13-61)    FlashDownload (经 FlashAdvanced)
48-54   0x60000     空闲 (56KB)               有效用户区 (13-61)    无
55      0x6E000     已被占用（四驱参数存储）    有效用户区 (13-61)    四驱 drv_mcu_flash 占用 ⚠️
56-61   0x70000     param_manager (48KB)      有效用户区 (13-61)    param_manager (绕过 FlashAdvanced)
────     ────        ────                     ────────────────      ──────────
62      0x7C000     APP_RUN_SLOT              灰色地带 (非有效非保护) Bootloader_App.c 直接写
63      0x7E000     硬保护 (不可操作)          单独保护              无
```

> ⚠️ **扇区 55（0x6E000）占用约束**：四驱工程（D:\260706_NL）的 `drv_mcu_flash.c` 使用 `STORAGE_SECTOR_ADDR=0x6E000` 存储参数。OTA 工程当前虽未使用该扇区，但做分区规划时必须按“不可用”处理（APP2 结束地址必须 ≤ 0x6E000，最多用到 54 号扇区）。

### FlashAdvanced 分区定义 (`flash_advanced.h`)

```c
#define FLASH_ADV_PROTECTED_SECTOR_START  0    // 保护区: 扇区 0-12
#define FLASH_ADV_PROTECTED_SECTOR_END   12
#define FLASH_ADV_PROTECTED_SECTOR       63    // 单独保护扇区 63

#define FLASH_ADV_VALID_SECTOR_START     13    // 有效用户区: 扇区 13-61
#define FLASH_ADV_VALID_SECTOR_END       61
```

**扇区 62 归属分析：** 不在保护区 (0-12, 63)、也不在有效用户区 (13-61)，属于灰色地带。FlashAdvanced 不会操作它（`FLASH_ADV_IS_SECTOR_WRITABLE(62)` 返回 false），由 `Bootloader_App.c` 独占写入 `APP_RUN_SLOT` 标记。

**param_manager 穿透问题：** `param_manager` 绕过 FlashAdvanced，直接调用 `hc32f46x_flash.c` 底层函数。修改前 `SEC_START=62` 越界写入扇区 62，与 `APP_RUN_SLOT` 冲突；修改后 `SEC_START=61`，与 FlashAdvanced 的有效用户区边界一致。

---

## Flash 分区详图（2026-08-02 更新：APP1/APP2 80KB）

```
0x00000000 +===========================================================+
           |                    BOOTLOADER (48KB 预留, sectors 0-5)      |
           |  Boot_StartupSequence, WDT fallback, slot selection,       |
           |  UDS OTA programming mode (Bootloader_UdsMain)             |
           |  Source: boot/projects/.../Bootloader_App/                 |
           |  实测镜像 46.55KB（链接区 0x0-0x13FFF）                     |
0x0000C000 +-----------------------------------------------------------+
           |                    UNUSED (16KB, sectors 6-7)              |
0x00010000 +-- UDS Shared State (8KB, sector 8) -----------------------+
           |  stc_uds_shared_t (56 bytes): magic, phase, target_slot,   |
           |  fw_size, fw_crc, result, pending_sid                      |
           |  Written by: UdsShared_Write() in Bootloader_App.c         |
           |  Read by: App_CheckPendingUdsAck() / Boot_StartupSequence  |
0x00012000 +-- FlashAdvanced Management Record (8KB, sector 9) --------+
           |  Erase counts per-sector, lifetime statistics              |
           |  Magic: 0x5A5A5A5A, stored by FlashAdv_SaveLifetimeInfo()  |
0x00014000 +-----------------------------------------------------------+
           |                    UNUSED / RESERVED (16KB)                 |
0x00016000 +-- APP1 State Sector (8KB, sector 11) ---------------------+
           |  Offset +0x000: WDT feed control (WDT_FEED_ENABLE/DISABLE) |
           |  Offset +0x008: WDT reset count (max 3 => slot disabled)   |
           |  Written by: SetWdtFeedControl(), UpdateWdtResetCount()    |
           |  Read by: InitAppInfo() in Boot_StartupSequence()          |
0x00018000 +-- APP2 State Sector (8KB, sector 12) ---------------------+
           |  Same structure as APP1 state sector                       |
0x0001A000 +===========================================================+
           |                    APP1 FIRMWARE (80KB, sectors 13-22)      |
           |  Slot A: motor control application                         |
           |  VTOR = 0x0001A000 (APP1_START_ADDR)                       |
0x0002E000 +-----------------------------------------------------------+
           |                    UNUSED (120KB, sectors 23-37)            |
0x0004C000 +===========================================================+
           |                    APP2 FIRMWARE (80KB, sectors 38-47)      |
           |  Slot B: backup / OTA target                               |
           |  VTOR = 0x0004C000 (APP2_START_ADDR)                       |
0x00060000 +-----------------------------------------------------------+
           |                    UNUSED (56KB, sectors 48-54)            |
           |   sector 55 (0x6E000) 已被占用（四驱参数存储）              |
0x00070000 +===========================================================+
           |                    param_manager (48KB, sectors 56-61)      |
           |  磨损均衡参数存储（绕过 FlashAdvanced）                     |
0x0007C000 +-- APP_RUN_SLOT Sector (8KB, sector 62) -------------------+
           |  Offset 0x000: SLOT_A_MAGIC(0x5A5A5A5A) or                 |
           |                SLOT_B_MAGIC(0xA5A5A5A5)                    |
           |  Written by: Boot_SetRunSlotToAddr(), Boot_SwitchAndRun()  |
           |  Read by: GetCurrentSlot() in Boot_StartupSequence()       |
0x0007E000 +-----------------------------------------------------------+
           |                    PROTECTED (8KB, sector 63, 不可操作)     |
0x00080000 +===========================================================+
```
---

## 关键宏定义 (`Bootloader_App.h`)

```
+---------------------------+----------------+------------------------------------------+
| Macro                     | Value          | Purpose                                  |
+---------------------------+----------------+------------------------------------------+
| APP1_START_ADDR           | 0x0001A000     | APP1 (Slot A) base address               |
| APP2_START_ADDR           | 0x0004C000     | APP2 (Slot B) base address               |
| APP_RUN_SLOT_ADDR         | 0x0007C000     | Current active slot magic storage        |
| APP1_STATE_SECTOR_BASE    | 0x00016000     | APP1 WDT state / feed control            |
| APP2_STATE_SECTOR_BASE    | 0x00018000     | APP2 WDT state / feed control            |
| UDS_SHARED_SECTOR_BASE    | 0x00010000     | UDS OTA shared state (Bootloader <-> APP) |
| UDS_TARGET_FLASH_ADDR     | APP2_START_ADDR| OTA firmware download target              |
| UDS_POST_FLASH_BOOT_ADDR  | APP1_START_ADDR| Post-OTA forced boot slot (hardcoded)     |
| UDS_SHARED_MAGIC          | 0x55445300     | "UDS\0" validation magic                  |
+---------------------------+----------------+------------------------------------------+
```

---

## OTA 相关关键变量及存放位置

### Flash 中的 OTA 变量

| 变量 / 结构 | 扇区 | 地址 | 大小 | 读写者 | 说明 |
|------------|------|------|------|--------|------|
| `stc_uds_shared_t` | 8 | `0x10000` | 56B | Bootloader_App.c | OTA 共享状态：magic, phase, target_slot, fw_size, fw_crc, result, pending_sid |
| `APP_RUN_SLOT` magic | 62 | `0x7C000` | 4B | Bootloader_App.c | `0x5A5A5A5A`→启 APP1, `0xA5A5A5A5A`→启 APP2 |
| APP2 固件 | 38-47 | `0x4C000` | 80KB | FlashDownload | OTA 下载目标 (`UDS_TARGET_FLASH_ADDR`) |
| FlashAdvanced 管理记录 | 9 | `0x12000` | ~540B | FlashAdvanced | 每扇区擦除计数、寿命统计 |

### UDS Shared State 结构 (`stc_uds_shared_t`) 详细字段

| 字段 | 偏移 | 说明 |
|------|------|------|
| `magic` | +0 | `0x55445300` ("UDS\0")，验证数据有效性 |
| `phase` | +4 | OTA 阶段：IDLE / DOWNLOADING / VERIFYING / COMPLETE / ERROR |
| `target_slot` | +8 | 目标槽位 (APP1 或 APP2) |
| `fw_size` | +12 | 固件总大小 (bytes) |
| `fw_crc` | +16 | 固件 CRC32 校验值 |
| `result` | +20 | OTA 结果：SUCCESS / CRC_ERROR / SIZE_ERROR / FLASH_ERROR |
| `pending_sid` | +24 | 待响应的 UDS Service ID (0x11 / 0x31) |

### OTA 流程中涉及的 Flash 操作

```
OTA 开始
  → 0x34 RequestDownload: FlashDownload 擦除 APP2 区域 (扇区 38-47, 80KB)
  → 0x36 TransferData:    数据缓冲到 60KB RAM, 满后 FlashAdv_BulkWriteSimple → APP2 Flash
  → 0x37 TransferExit:    最终冲刷缓冲区, CRC32 校验
  → UdsShared_SetPhase(COMPLETE): 写 UDS Shared State (扇区 8)
  → 0x11 ECU Reset:
       App_CheckPendingUdsAck() 读到 COMPLETE
       → UdsShared_SetPhase(IDLE) 清除
       → Boot_SwitchAndRunOther() 写 APP_RUN_SLOT (扇区 62) → NVIC_SystemReset()
```

### RAM 中的 OTA 变量

| 变量 | 位置 | 大小 | 说明 |
|------|------|------|------|
| `FlashDownloadContext_t` | BSS (boot) | ~100B | OTA 状态机上下文 |
| `FW_RAM_BUFFER` | BSS (boot) | 60KB | OTA 数据缓冲 (地址 `0x1FFF0000` 附近) |
| `stc_shared_ctrl_t` | `0x1FFFAF00` | 32B | WDT 喂狗控制 (app1/app2 feed_ctrl + debug_flag) |
| `g_ParamDebug` | BSS | ~20B | param_manager 调试信息 |

---

## Flash 写入数据流 (所有 Flash 修改路径)

> 详细函数级说明见 [flash操作.md](flash操作.md)

```
UDS OTA 固件下载 (经 FlashAdvanced):
  CAN RX → ISOTP → uds_receive_handler → uds_dl_if_t.on_transfer_data
    → FlashDownload_OnTransferData → RAM buffer (60KB)
    → FlashDownload_Task() → FlashDownload_FlushBuffer
    → FlashAdv_BulkWriteSimple → FlashAdv_IsProtected() 检查
    → hc32f46x_flash.c (.ramfunc) → EFM 硬件

UDS Shared State (绕过 FlashAdvanced):
  0x11/0x31 handler → UdsShared_SetPhase/Write
    → EFM_REG_Unlock → EFM_SectorErase(0x10000) → EFM_ProgramWord×14 → EFM_REG_Lock

WDT State (绕过 FlashAdvanced):
  TMR0_Unit2_IRQHandler → UpdateWdtResetCount
    → Read both values → EFM_Erase → Write both values back

Slot Selection (绕过 FlashAdvanced):
  Boot_StartupSequence → Boot_SetRunSlotToAddr / UpdateSlotFlagToFlash
    → EFM_Erase(0x7C000) → EFM_ProgramWord(magic)

Parameter Save (绕过 FlashAdvanced):
  APP code → Param_Save → Internal_Erase → hc32f46x_flash.c (.ramfunc) → EFM
```

**关键点：** FlashAdvanced 只保护通过它的调用路径 (FlashDownload OTA 写入)。`Bootloader_App.c` 和 `param_manager.c` 直接调用底层 `hc32f46x_flash.c` / EFM，不受 FlashAdvanced 保护。

---

## Flash 管理层级 (4 层架构)

```
+------------------+  uds_diagnostic.c   (UDS protocol dispatch)
|   UDS Layer      |  uds_dl_bridge.c    (interface bridge)
+------------------+
|   Flash Logic    |  flash_download.c   (OTA state machine, buffer, CRC)
|                  |  param_manager.c    (wear-leveled param storage)
|                  |  Bootloader_App.c   (UDS shared, WDT state, slot)
+------------------+
|   Flash Advanced |  flash_advanced.c   (protection, statistics, lifetime)
+------------------+
|   Flash Low-level|  hc32f46x_flash.c   (DDL EFM wrapper, .ramfunc)
+------------------+
```

---

### 1. `hc32f46x_flash.c` / `.h` — 底层 Flash 适配器 (无分区逻辑)

**位置:** `boot/Adp/`, `app1/Adp/`, `app2/Adp/` (三个项目内容一致)

**API:**
```
HC32FLASH_EraseSector(addr)            // 擦除 8KB 扇区 (.ramfunc)
HC32FLASH_WritedWord_NoCheck(addr,data)// 写 1 字, 不回读校验
HC32FLASH_WritedWord_Check(addr,data)  // 写 1 字 + 回读校验 (.ramfunc)
HC32FLASH_ReaddWord(addr)             // 直接读 (绕过 cache)
HC32FLASH_GetStatus()                 // 错误标志: WPRERR/PGAERR/PEWERR/COLERR/PGMISMTCH
```

**无任何地址保护** — 传入什么地址就操作什么地址。

---

### 2. `flash_advanced.c` / `.h` — Flash 保护与统计层 (分区逻辑所在)

**位置:** `boot/Adp/`, `app1/Adp/`, `app2/Adp/` (三个项目内容一致)

**分区定义 (`flash_advanced.h`):**

```c
// 保护区
#define FLASH_ADV_PROTECTED_SECTOR_START  0    // 扇区 0-12: Bootloader+UDS+WDT+预留
#define FLASH_ADV_PROTECTED_SECTOR_END   12
#define FLASH_ADV_PROTECTED_SECTOR       63    // 扇区 63: 硬保护

// 有效用户区
#define FLASH_ADV_VALID_SECTOR_START     13    // 扇区 13-61: APP1+APP2+param_manager
#define FLASH_ADV_VALID_SECTOR_END       61

// 管理记录: 扇区 9 (0x12000)
#define FLASH_ADV_MANAGEMENT_SECTOR       9
```

**保护宏:**
```c
// 扇区是否受保护 (0-12 或 63)
#define FLASH_ADV_IS_SECTOR_PROTECTED(sector) \
    (((sector) >= 0 && (sector) <= 12) || (sector) == 63)

// 扇区是否可写 (在有效区 13-61 且不受保护)
#define FLASH_ADV_IS_SECTOR_WRITABLE(sector) \
    ((sector) >= 13 && (sector) <= 61 && !FLASH_ADV_IS_SECTOR_PROTECTED(sector))
```

**操作流程 (EraseSector 为例):**
```
FlashAdv_EraseSector(handle, addr)
  → FlashAdv_IsValid(addr)       // 地址在总范围内?
  → FlashAdv_IsProtected(addr)   // 扇区 0-12 或 63? → 拒绝, 返回 FLASH_ADV_PROTECTED
  → hc32f46x_flash.c 执行实际擦除
  → FlashAdv_UpdateLifetime()    // 更新擦除计数, 持久化到扇区 9
```

---

### 3. `Bootloader_App.c` / `.h` — Bootloader Flash 操作 (绕过 FlashAdvanced)

**位置:** 三个项目共享相同代码

**职责:** UDS 共享状态、WDT 状态、槽位选择、启动跳转。

| 函数 | 扇区 | 操作 |
|------|------|------|
| `UdsShared_Read/Write/Clear/SetPhase` | 8 (0x10000) | 读写 56 字节 UDS 状态 |
| `GetWdtFeedControl/SetWdtFeedControl` | 11 或 12 | 读写 WDT 喂狗控制 |
| `UpdateWdtResetCount/ClearWdtResetCount` | 11 或 12 | 读写 WDT 复位计数 |
| `Boot_SetRunSlotToAddr/Boot_SwitchAndRunOther` | 62 (0x7C000) | 写 1 字 Slot Magic |

**WDT 扇区读-擦-写协议:** 扇区中存两个值 (偏移 +0 和 +8)，擦除前先读出两个值，擦除后两个值一起写回，避免数据丢失。

---

### 4. `flash_download.c` / `.h` — OTA 固件下载模块 (经 FlashAdvanced)

**位置:** `boot/UDS/` (仅 bootloader)

**状态机:** `IDLE → PREPARING → READY → TRANSFERRING → VERIFYING → COMPLETE`

**关键配置:**
| 宏 | 默认值 | 含义 |
|----|--------|------|
| `FW_APP_START_ADDR` | `0x0004C000` | OTA 目标 = APP2 |
| `FW_APP_MAX_SIZE` | `0x00014000` | 最大固件 = 80KB (10 扇区) |
| `FW_RAM_BUFFER_SIZE` | 60KB | RAM 缓冲区 |
| `FW_FLASH_WRITE_ENABLED` | 1 | 0 = 干运行 (不真写 Flash) |

**API (由 UDS 层通过 uds_dl_bridge 调用):**
```
FlashDownload_OnRequestDownload(addr, size)  // 0x34: 校验 + 擦除目标区域
FlashDownload_OnTransferData(seq, data, len) // 0x36: 缓冲 + 满时写 Flash
FlashDownload_OnTransferExit()               // 0x37: 最终冲刷 + CRC 校验
```

---

### 5. `uds_dl_bridge.c` — UDS 下载接口桥

**架构:** `uds_diagnostic.c → uds_dl_get_if() → uds_dl_bridge.c → flash_download.c`

使用函数指针表 (`uds_dl_if_t`) 解耦 UDS 协议层和 Flash 下载实现。

**DID 读回 (0x22 服务):**
| DID | 内容 |
|-----|------|
| 0xF000 | 固件版本 |
| 0xF001 | Bootloader 版本 |
| 0xF002 | 固件 CRC32 |

---

### 6. `param_manager.c` / `.h` — 磨损均衡参数存储 (绕过 FlashAdvanced)

**位置:** `boot/Utils/`, `app1/Utils/`, `app2/Utils/` (三个项目内容一致)

**扇区配置 (已修复):**
```c
#define SEC_START  61   // 修改前: 62 → 修改后: 61 ✅
#define SEC_END    56
// 使用扇区 56-61, 6 × 8KB = 48KB, 地址范围 0x70000-0x7BFFF
```

**磨损均衡算法:** 扫描 61→56 找最高 SeqID → 写入下一扇区 (递减, 56 回绕到 61) → 扇区满时擦除当前扇区重用。

**注意:** `Internal_Erase()` 直接调用 `hc32f46x_flash.c`，不经 FlashAdvanced，无保护检查。

---

## 扇区冲突与修复

### 问题

修改前 `param_manager` 和 `Bootloader_App` 共享扇区 62：

| 模块 | 扇区 | 写入内容 |
|------|------|---------|
| `param_manager` | 56-**62** | 参数结构体 (含 magic/CRC) |
| `Bootloader_App` | **62** | APP_RUN_SLOT Magic (1 word) |

两者都整扇区擦除后写入，无互斥协调 → 后写者覆盖先写者的数据。

### 修复: Plan A ✅ (已实施)

```c
// param_manager.c 第 5 行, 三个项目各改 1 处:
// 修改前: #define SEC_START  62
// 修改后: #define SEC_START  61
```

**修复后布局:**
```
扇区    地址        归属                  大小
────     ────        ────                  ────
56-61   0x70000     param_manager 独占     6×8KB = 48KB
62      0x7C000     APP_RUN_SLOT 独占      8KB
63      0x7E000     硬保护 (不可操作)       8KB
```

**一致性验证:**

| 模块 | 定义 | 扇区范围 | 一致? |
|------|------|---------|-------|
| `flash_advanced.h` | `VALID_SECTOR_END = 61` | 13-61 | — |
| `param_manager.c` | `SEC_START = 61, SEC_END = 56` | 56-61 | ✅ |
| `Bootloader_App.h` | `APP_RUN_SLOT_ADDR = 0x7C000` | 62 | ✅ |

---

## 编译安全开关

| 宏 | 文件 | 0 | 1 |
|----|------|---|---|
| `FW_FLASH_WRITE_ENABLED` | flash_download.h | 干运行 (不真写) | 真实 Flash 写入 |
| `FLASH_DEBUG_ENABLE` | hc32f46x_flash.h | 无调试输出 | 打印每次操作状态 |
| `FLASH_MANAGEMENT_RECORD_ENABLE` | flash_advanced.h | 无寿命追踪 | 持久化擦除计数 |
| `PARAM_DEBUG` | param_manager.h | 无调试输出 | 打印扫描/磨损信息 |

---

## Shared RAM Layout

```
0x1FFF8000 +-- RAM_START_ADDR -----------------------------------------+
           |                    RAM (188KB)                              |
           |                    Main stack + data + BSS                 |
0x1FFFAF00 +-- SHARED_CTRL_ADDR (top of RAM - 0x100) ------------------+
           |  stc_shared_ctrl_t (32 bytes):                             |
           |    app1_feed_ctrl: WDT_FEED_ENABLE(0) or DISABLE(0xDEADBEEF)|
           |    app2_feed_ctrl: WDT_FEED_ENABLE(0) or DISABLE(0xDEADBEEF)|
           |    debug_flag: WDT feed persistence across debugger resets  |
           |  Accessed via GetSharedCtrl() inline function               |
           |  Used by: TMR0_Unit2_IRQHandler (WDT feeding decision)     |
0x1FFFAF20 +-- RAM_END_ADDR -------------------------------------------+
```

---

---

## APP1/APP2 未来规划：128KB + 128KB（2026-08-04，未实施）

> 换算：128KB = `0x20000`，96KB = `0x18000`；每扇区 8KB。完整规划另见本次会话输出 `OTA写入.md`。

### 原设想（不可行）

```
APP1：0x1A000 ~ 0x3A000   (128KB，扇区 13–28)
APP2：0x32000 ~ 0x52000   (128KB，扇区 25–40，首地址 = 0x1A000 + 96KB)
重叠：0x32000 ~ 0x3A000   = 32KB（扇区 25–28，4 个扇区）
```

APP1 为 128KB 时，APP2 至少要从 0x3A000 开始，不能从 +96KB 开始。

### 方案 A（推荐）

```
APP1：0x1A000 ~ 0x3A000   (128KB，扇区 13–28)
APP2：0x3A000 ~ 0x5A000   (128KB，扇区 29–44)
结束 0x5A000 < 0x6E000 ✓（不碰 55 号扇区）
剩余空闲：0x5A000 ~ 0x6DFFF（扇区 45–54，80KB）
```

### 方案 B（若 APP2 首地址必须 = 0x32000）

```
APP1：0x1A000 ~ 0x32000   (96KB，扇区 13–24)
APP2：0x32000 ~ 0x52000   (128KB，扇区 25–40)
结束 0x52000 < 0x6E000 ✓
剩余空闲：0x52000 ~ 0x6DFFF（扇区 41–54，112KB）
```

### 实施需同步修改的位置（方案 A 为例）

| 位置 | 修改 |
|---|---|
| `flash_download.h` | `FW_APP_MAX_SIZE` `0x14000`→`0x20000`；`FW_APP_START_ADDR` `0x4C000`→`0x3A000`；TBOX 窗口扩为 128KB |
| `Bootloader_App.h` | `APP2_START_ADDR` `0x4C000`→`0x3A000` |
| `Bootloader_App.c` | `max_firmware_size` 80KB→128KB；`user_end_addr` `+0x14000`→`+0x20000` |
| `flash_download.c` | `FW_DEFAULT_MAX_SIZE` 80KB→128KB |
| app1/app2 链接脚本（Keil Target） | app1 `0x1A000`/`0x20000`；app2 `0x3A000`/`0x20000` |
| TBOX 协议侧 | 0x34 地址窗口同步（APP1 `0x08018000~0x08038000`；APP2 `0x08038000~0x08058000`） |
## 修改记录（2026-08-02）

| 修改项 | 说明 |
|--------|------|
| APP1 / APP2 调整为 80KB | APP1: 0x1A000~0x2DFFF（扇区13-22）；APP2: 0x4C000~0x5FFFF（扇区38-47）；`FW_APP_MAX_SIZE=0x14000`、`TBOX_ADDR_END=0x08018000`、boot 的 `max_firmware_size=80KB` |
| Bootloader 预留区 | 0~0xC000（48KB，镜像实测 46.55KB）；0xC000~0xFFFF（扇区6-7）为空闲 |
| PB6 相位指示删除 | 四驱工程 Phase 1 闪烁及 OTA 工程 Phase 1/2/3 闪烁、`ShowBootStatus()` 全部移除 |
| 四驱工程移植实测通过 | 复用本工程 boot，Phase 1→2→3 全链路验证通过（详见四驱 `app/can/uds/uds移植.md`） |

### 四驱控制盒工程变体（D:\260706_NL）

与 OTA 工程的分区差异：

| 区域 | OTA 工程 | 四驱工程 |
|------|---------|---------|
| 参数存储 | 扇区 56-61 (0x70000)，`param_manager` | **扇区 55 (0x6E000)**，`drv_mcu_flash.c` 的 `STORAGE_SECTOR_ADDR`（APP 自有系统） |
| Bootloader | 链接区 0x0-0x13FFF | 预留 0~0xC000（同样使用 OTA 工程的 boot 镜像） |
| APP1/APP2 | 80KB | 80KB（一致） |
| Flash 写入 | 下载经 FlashAdvanced | 四驱 `flash_download` 直接 EFM 写入（无 FlashAdvanced） |
| 跳转槽 | 0x7C000 | 0x7C000（一致，勿占用） |
---

## 修改记录（2026-08-04）

| 修改项 | 说明 |
|--------|------|
| OTA 正式模式 | `BOOT_OTA_MODE_DEBUG=0`：0x34 按实际烧录目标（APP1/APP2）写跳转槽；0x31（boot 上下文）不再抢写跳转槽 |
| 0x36 块序号回绕 | `expected_sequence` 255→1 回绕，支持 >255 块（>63.75KB）固件 |
| RAM 缓冲缩减 | `FW_RAM_BUFFER_SIZE` 88KB→8KB（仅作 0x36 块 4 字节对齐暂存）；0x34/0x36 的 RAM 大小检查与 BIN 累积打印已注释 |
| 扇区 55 占用约束 | 四驱参数存储位于 0x6E000；规划时 APP2 结束地址必须 ≤ 0x6E000 |
| 128KB 分区规划 | 见上文“APP1/APP2 未来规划”（尚未实施） |
