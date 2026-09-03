# Flash 操作函数全集

> **相关文档:** [flash分区.md](flash分区.md) — 扇区布局 | [收发流程.md](收发流程.md) — OTA 协议流程 | [OTA-flash补充.md](OTA-flash补充.md) — OTA 跳转参数 & 空闲扇区

本文档汇总项目中所有涉及 Flash 读/写/擦除的函数，按模块分类，标注可操作区域、保护检查状态、用途及关键参数。

---

## 总览：Flash 写入保护架构

```
                         ┌──────────────────┐
                         │  hc32f46x_flash.c │  ← 底层 EFM 驱动, 无任何保护
                         │  (5 个基本操作)    │
                         └────────┬─────────┘
                                  │
           ┌──────────────────────┼──────────────────────┐
           │                      │                      │
    直接调用 (绕过保护)      直接调用 (绕过保护)      FlashAdv_Create()
           │                      │                      │
  ┌────────┴────────┐  ┌─────────┴────────┐  ┌─────────┴─────────┐
  │ Bootloader_App.c │  │ param_manager.c  │  │ flash_advanced.c  │ ← 保护层
  │ UDS Shared (扇8) │  │ 参数存储 (扇56-61)│  │ 扇区0-12,63 拒绝  │
  │ WDT State(扇11-12)│  │                  │  │ 扇区13-61 允许    │
  │ APP_RUN_SLOT(扇62)│  └─────────────────┘  └────────┬─────────┘
  └──────────────────┘                                  │
                                                        │ FlashAdv_BulkWriteSimple
                                                        │ FlashAdv_EraseSector
                                                        ▼
                                              ┌──────────────────┐
                                              │ flash_download.c │ ← OTA 固件下载
                                              │ APP2 (扇38-43)   │
                                              └──────────────────┘
```

**结论：** 只有 `flash_download.c` (OTA 刷写) 经过 `flash_advanced` 保护层。`Bootloader_App.c` 和 `param_manager.c` 直接调用底层 `hc32f46x_flash.c`，无保护检查。

---

## 扇区归属速查

```
扇区    地址        归属                    FlashAdvanced 视角   保护?
────    ────        ────                    ────────────────    ────
 0-5    0x00000     Bootloader (48KB 预留)     保护区 (0-12)       ✅ 拒绝
 6-7    0x0C000     预留 (16KB)               保护区 (0-12)       ✅ 拒绝
 8      0x10000     UDS Shared State        保护区 (0-12)       ✅ 拒绝
 9      0x12000     FlashAdvanced 管理记录   保护区 (0-12)       ✅ 拒绝
10      0x14000     预留                     保护区 (0-12)       ✅ 拒绝
11      0x16000     APP1 WDT State          保护区 (0-12)       ✅ 拒绝
12      0x18000     APP2 WDT State          保护区 (0-12)       ✅ 拒绝
────    ────        ────                    ────────────────    ────
13-22   0x1A000     APP1 固件 (80KB)          有效用户区 (13-61)  ✅ 允许-37   0x2E000     空闲 (120KB)              有效用户区 (13-61)  ✅ 允许
38-47   0x4C000     APP2 OTA 固件 (80KB)     有效用户区 (13-61)  ✅ 允许
48-55   0x60000     空闲 (64KB)              有效用户区 (13-61)  ✅ 允许
56-61   0x70000     param_manager (48KB)    有效用户区 (13-61)  ✅ 允许
────    ────        ────                    ────────────────    ────
62      0x7C000     APP_RUN_SLOT            灰色地带            不保护也不操作
63      0x7E000     硬保护                   单独保护             ✅ 拒绝
```

---

## 一、hc32f46x_flash.c/.h — 底层 Flash 适配器

**位置:** `boot/Adp/`, `app1/Adp/`, `app2/Adp/` (三个项目内容一致)

**保护:** ❌ 无 — 传入任何地址都直接操作

| 函数 | 操作 | .ramfunc | 用途及关键参数 |
|------|:---:|:---:|------|
| `HC32FLASH_EraseSector(u32Addr)` | 擦除 | ✅ | 擦除 `u32Addr` 所在 8KB 扇区。流程: Unlock→ClearFlags→EFM_SectorErase→Lock |
| `HC32FLASH_WritedWord_NoCheck(u32Addr, data)` | 写入 | ❌ | 写 1 字, 不回读校验。`EFM_ProgramWord()` 单次编程 |
| `HC32FLASH_WritedWord_Check(u32Addr, data)` | 写入 | ✅ | 写 1 字 + 回读校验。用 `EFM_ProgramWordReadBack()` 硬件回读, 再软件 `HC32FLASH_ReaddWord` 二次验证, 不匹配返回 `HC32FLASH_PGMISMTCH` |
| `HC32FLASH_ReaddWord(u32Addr)` | 读取 | ❌ | 直接指针读取 `*(volatile uint32_t*)u32Addr`, 绕过 Flash Cache |
| `HC32FLASH_GetStatus()` | 状态 | ❌ | 读 EFM FSR 寄存器, 返回: OK/BUSY/WPRERR/PGAERR/PEWERR/COLERR/PGMISMTCH |

**限制:**
- 擦除和校验写入必须在 RAM 执行 (`.ramfunc`), 因为 HC32F460 不能同时从 Flash 执行代码和擦写 Flash
- 擦除粒度固定 8KB (硬件扇区大小)
- 擦除后所有 bit 变 1, 编程只能将 1 变 0 (Flash 物理特性)

---

## 二、flash_advanced.c/.h — Flash 保护层与统计

**位置:** `boot/Adp/`, `app1/Adp/`, `app2/Adp/` (三个项目内容一致)

**保护宏:**
```c
#define FLASH_ADV_PROTECTED_SECTOR_START  0    // 保护区: 扇区 0-12
#define FLASH_ADV_PROTECTED_SECTOR_END   12
#define FLASH_ADV_PROTECTED_SECTOR       63    // 单独保护扇区 63

#define FLASH_ADV_VALID_SECTOR_START     13    // 有效用户区: 扇区 13-61
#define FLASH_ADV_VALID_SECTOR_END       61
```

**保护逻辑:** `FlashAdv_EraseSector` / `FlashAdv_WriteWord` 内部先调用 `FlashAdv_IsProtected(addr)`, 若扇区 ∈ [0-12] 或 =63, 返回 `FLASH_ADV_PROTECTED` 拒绝操作。

### 生命周期管理

| 函数 | 操作 | 用途及关键参数 |
|------|:---:|------|
| `FlashAdv_Create(config, ops)` | 创建 | 创建全局句柄 `g_flash_adv_handle`。`config`: base=0, sector_size=0x2000, total_size=0x80000, max_erase_cycles=10000。`ops` 为 NULL 则用默认回调。初始化寿命信息, 尝试从扇区 9 加载历史记录 |
| `FlashAdv_Destroy(handle)` | 销毁 | 清零句柄 |

### 基本操作 (有保护检查)

| 函数 | 操作 | .ramfunc | 用途及关键参数 |
|------|:---:|:---:|------|
| `FlashAdv_EraseSector(handle, address)` | 擦除 | 间接 ✅ | 先检查 `IsValid` → `IsProtected`, 通过后调用 `HC32FLASH_EraseSector()`, 更新寿命计数 |
| `FlashAdv_WriteWord(handle, address, data)` | 写入 | 间接 ✅ | 先检查 `IsValid` → `IsProtected`, 通过后调用 `HC32FLASH_WritedWord_Check()`, 含 3 次重试 |
| `FlashAdv_ReadWord(handle, address)` | 读取 | ❌ | 先检查 `IsValid`, 通过后调用 `HC32FLASH_ReaddWord()` |
| `FlashAdv_BulkWrite(handle, params)` | 批量写 | 间接 ✅ | `params` 含 start_address, data, word_count, verify_enabled, retry_on_error。逐字写入, verify_enabled 时每字回读校验 |
| `FlashAdv_BulkWriteSimple(handle, start_addr, data, word_count)` | 批量写 | 间接 ✅ | `BulkWrite` 的简化版, 默认开启 verify + retry。**flash_download 使用的接口** |

### 地址检查

| 函数 | 用途及关键参数 |
|------|------|
| `FlashAdv_IsAddressProtected(handle, address)` | 扇区 ∈ [0-12] 或 =63 → 受保护 |
| `FlashAdv_IsValidAddress(handle, address)` | 地址在 [base, base+total_size) 内 |
| `FlashAdv_IsSectorWritable(handle, sector_index)` | 扇区 ∈ [13-61] 且 ≠9(管理记录) 且 ≠63 |
| `FlashAdv_IsAddressProtectedByMacro(address)` | 静态宏版本, 无需 handle |
| `FlashAdv_IsUserAddress(address)` | 地址在有效用户区 [0x1A000, 0x7C000) |
| `FlashAdv_GetSectorStart(handle, address)` | 返回地址所在 8KB 扇区的起始地址 |
| `FlashAdv_GetSectorSize()` | 返回 `0x2000` (8KB) |
| `FlashAdv_GetUserRegion(start, end, size)` | 返回用户区: start=0x1A000 (扇区13), end=0x7C000 (扇区62起始) |
| `FlashAdv_GetValidSectorRange(handle, start, end)` | 返回有效扇区号: 13–61 |
| `FlashAdv_GetValidSectorRangeNum(start, end)` | 同上, 无需 handle |
| `FlashAdv_GetProtectedSectorRange(start, end)` | 返回保护区扇区号: 0–12 |

### 统计与寿命 (管理记录在扇区 9, 0x12000)

| 函数 | 操作 | 用途及关键参数 |
|------|:---:|------|
| `FlashAdv_GetStatistics(handle, stats)` | 读取 | 获取 erase_count, write_count, read_count, error_count, total_operations 等 |
| `FlashAdv_ResetStatistics(handle)` | 写入 | 清零统计 |
| `FlashAdv_GetLifetimeInfo(handle, info)` | 读取 | 获取每扇区擦除计数数组 `sector_erase_counts[64]`, 健康度百分比, 最大/最小擦除次数 |
| `FlashAdv_SaveLifetimeInfo(handle)` | 擦除+写 | 擦除扇区 9, 写入 `FlashAdvLifetimeInfo_t` (含 magic 0x5A5A5A5A + checksum) |
| `FlashAdv_LoadLifetimeInfo(handle)` | 读取 | 从扇区 9 读回寿命信息, 验证 magic + checksum |
| `FlashAdv_ResetLifetime(handle)` | 写入 | 清零寿命计数并保存到扇区 9 |
| `FlashAdv_GetSectorEraseCount(handle, sector)` | 读取 | 返回指定扇区的累计擦除次数 |
| `FlashAdv_GetAddressEraseCount(handle, address)` | 读取 | 返回指定地址所在扇区的累计擦除次数 |

### 历史记录

| 函数 | 用途及关键参数 |
|------|------|
| `FlashAdv_GetHistory(handle, out, buf_size, num)` | 读取最近 16 条操作历史 (op_type, address, data, status) |
| `FlashAdv_GetLastOperation(handle, record)` | 读取最后一条操作记录 |
| `FlashAdv_ClearHistory(handle)` | 清除历史 |

---

## 三、Bootloader_App.c/.h — Bootloader Flash 操作 (绕过 FlashAdvanced)

**位置:** 三个项目共享相同代码

**保护:** ❌ 全部绕过 FlashAdvanced, 直接调 `hc32f46x_flash.c` 或 EFM 寄存器

### UDS Shared State — 扇区 8 (0x10000)

| 函数 | 操作 | 用途及关键参数 |
|------|:---:|------|
| `UdsShared_Read(pState)` | 读取 | 从 `0x10000` 读 56 字节到 `stc_uds_shared_t`。通过 `READ_FLASH_DIRECT` 逐字读取 |
| `UdsShared_Write(pState)` | 擦除+写 | **擦除整个扇区 8**, 写入 `pState` (56 字节, 14 个 word)。流程: Unlock→Erase→ProgramWord×14→Lock |
| `UdsShared_Clear()` | 擦除 | **擦除整个扇区 8**, 清空所有 UDS 共享状态 |
| `UdsShared_SetPhase(phase, target_slot)` | 读-擦-写 | 先读取现有状态, 修改 `phase` 和 `target_slot`, 设置 `pending_sid`, 擦除扇区 8 后写回 |
| `App_CheckPendingUdsAck()` | 读取 | APP 启动时调用。读 UDS Shared, 若 `pending_sid≠0` 则发送延迟 CAN ACK, 然后 `UdsShared_Clear()` |

**`stc_uds_shared_t` 结构 (56 字节):**

| 字段 | 偏移 | 大小 | 用途 |
|------|------|------|------|
| `magic` | +0 | 4B | `0x55445300` ("UDS\0") |
| `phase` | +4 | 4B | OTA 阶段: 0=IDLE, 1=ENTER_BOOTLOADER, 2=PROGRAMMING_DONE |
| `target_slot` | +8 | 4B | 目标槽位: SLOT_APP1 / SLOT_APP2 |
| `fw_size` | +12 | 4B | 固件大小 (bytes) |
| `fw_crc` | +16 | 4B | 固件 CRC32 |
| `result` | +20 | 4B | OTA 结果: 0=进行中, 1=成功, 0xFF=失败 |
| `pending_sid` | +24 | 4B | 延迟 ACK 的 SID: 0x31 / 0x11 / 0=none |
| `reserved[7]` | +28 | 28B | 保留 |

### WDT State — 扇区 11 (0x16000) 或 扇区 12 (0x18000)

| 函数 | 操作 | 用途及关键参数 |
|------|:---:|------|
| `GetWdtFeedControl(addr)` | 读取 | 从 `addr` (0x16000 或 0x18000) 偏移 +0 读 WDT 喂狗控制字 |
| `SetWdtFeedControl(addr, value)` | 读-擦-写 | 读取扇区中两个值 (偏移+0和+8), **擦除整个扇区**, 修改 WDT feed control, 两个值一起写回 |
| `GetWdtResetCount(addr)` | 读取 | 从 `addr` 偏移 +8 读 WDT 复位计数 |
| `UpdateWdtResetCount(addr, cnt)` | 读-擦-写 | 读取两个值, **擦除整个扇区**, 更新 WDT count, 写回两个值 |
| `ClearWdtResetCount(addr)` | 读-擦-写 | 同上, count 清零 |
| `ClearAppStateBySlot(slot)` | 读-擦-写 | 根据 slot 选扇区 11 或 12, 擦除并重置 WDT 状态 |

**关键协议:** WDT 扇区存两个独立值 (偏移 +0 和 +8), 修改任何一个时都需要: 读出两个值 → 擦除扇区 → 两个值一起写回。避免数据丢失。

### APP_RUN_SLOT — 扇区 62 (0x7C000)

| 函数 | 操作 | 用途及关键参数 |
|------|:---:|------|
| `GetCurrentSlot()` | 读取 | 读 `0x7C000` 偏移 +0, 返回 `SLOT_APP1` / `SLOT_APP2` / `SLOT_NONE` |
| `Boot_SetRunSlotToAddr(addr)` | 擦除+写 | **擦除扇区 62**, 根据 `addr` 写入 `SLOT_A_MAGIC` (0x5A5A5A5A) 或 `SLOT_B_MAGIC` (0xA5A5A5A5) |
| `Boot_SwitchAndRunOther()` | 读-擦-写 | 读取当前 slot, **擦除扇区 62**, 写入另一个 slot 的 magic, 然后 `NVIC_SystemReset()` |
| `UpdateSlotFlagToFlash(ctx)` | 擦除+写 | 根据 `ctx.eTargetSlot` **擦除扇区 62**, 写入对应 magic |

### 通用

| 函数 | 操作 | 用途及关键参数 |
|------|:---:|------|
| `Bootloader_FlashEraseSector(addr)` | 擦除 | 通用擦除包装, 直接调用 `HC32FLASH_EraseSector()`, 无保护 |
| `READ_FLASH_DIRECT(addr)` | 读取 | 禁用 Flash Cache → 读取 → 恢复 Cache。用于确保读到的不是缓存中的旧数据 |
| `Bootloader_JumpToApp(appAddr)` | 跳转 | 设置 MSP (从 APP 向量表), 设置 SCB->VTOR, 跳转到 APP Reset Vector |

---

## 四、flash_download.c/.h — OTA 固件下载 (经 FlashAdvanced)

**位置:** `boot/UDS/` (仅 bootloader)

**保护:** ✅ 经 FlashAdvanced — 擦除和写入都通过 `FlashAdv_EraseSector` / `FlashAdv_BulkWriteSimple`, 受保护扇区 (0-12, 63) 会被拒绝

**可操作区域:** APP2 固件区, 扇区 38-43 (0x4C000–0x57FFF), 最大 48KB

**关键宏:**
```c
#define FW_APP_START_ADDR           0x0004C000  // OTA 目标 = APP2
#define FW_APP_MAX_SIZE             0x00014000  // 最大 80KB (10 扇区)
#define FW_RAM_BUFFER_SIZE          (60*1024)   // RAM 对齐工作缓冲区
#define FW_FLASH_WRITE_ENABLED      1           // 0=干运行, 1=真实写入
#define TBOX_ADDR_START             0x08004000  // TBOX 发送的逻辑地址
#define MAP_TBOX_ADDR_TO_FLASH(a)   (a - 0x08004000 + 0x0004C000)
```

### 生命周期

| 函数 | 用途及关键参数 |
|------|------|
| `FlashDownload_Init(config)` | 初始化状态机。创建 `FlashAdv_Create()` 句柄, 获取用户区域范围。`config` 为 NULL 时使用默认值 (max=256KB, sector=8KB, verify=on, auto_reset=off) |
| `FlashDownload_Reset()` | 重置所有内部状态 (不清除 FlashAdv 句柄) |
| `FlashDownload_Cancel()` | 取消当前下载, 回到 IDLE |

### UDS 服务回调 (由 uds_dl_bridge 调用)

| 函数 | UDS | 操作 | 用途及关键参数 |
|------|:---:|:---:|------|
| `FlashDownload_OnRequestDownload(address, size)` | 0x34 | 擦除 | **映射 TBOX 地址**: `0x08004xxx → 0x0004Cxxx`。校验地址在 APP2 范围、size≤max。**擦除目标扇区** (通过 `fw_erase_range → FlashAdv_EraseSector`)。状态: IDLE→READY |
| `FlashDownload_OnTransferData(seq, data, len)` | 0x36 | 写入 | 校验序号 (1-255)、数据不越界。**立即写入 Flash** (通过 `fw_write_buffer_to_flash → FlashAdv_BulkWriteSimple`)。更新运行 CRC32。状态: READY→TRANSFERRING |
| `FlashDownload_OnTransferExit()` | 0x37 | 校验 | 完成 CRC32 取反 (`~rx_crc`)。检查接收完整性。状态: TRANSFERRING→COMPLETE |
| `FlashDownload_Erase(address, size)` | 0x31 | 擦除 | RID_ERASE_FIRMWARE。实际擦除已在 0x34 完成, 此处仅日志记录 |
| `FlashDownload_CalculateCRC(address, size, crc)` | 0x31 | 读取 | RID_CALCULATE_CRC。从 Flash 读取数据并计算 CRC32 |

### 状态查询

| 函数 | 用途及关键参数 |
|------|------|
| `FlashDownload_GetState()` | 返回当前状态: IDLE/PREPARING/READY/TRANSFERRING/VERIFYING/COMPLETE/ERROR |
| `FlashDownload_GetProgress(progress)` | 填充 `FlashDownloadProgress_t`: total_size, received_size, target_address, progress_percent |
| `FlashDownload_GetLastError()` | 返回最后错误码: OK/ADDR_INVALID/SIZE_TOO_LARGE/ERASE_FAILED/WRITE_FAILED/VERIFY_FAILED/SEQUENCE_ERROR |
| `FlashDownload_IsPending()` | 是否需要发 NRC 0x78 (Response Pending) |
| `FlashDownload_Task()` | 主循环任务。当前为空壳 (状态机在 UDS 回调中推进) |

### 版本/CRC 读取 (DID 0x22 回调)

| 函数 | 用途及关键参数 |
|------|------|
| `FlashDownload_GetFirmwareVersion()` | 读 APP2 Flash 偏移 +0x10 处的 uint16 (固件版本字段) |
| `FlashDownload_GetBootloaderVersion()` | 读 0x00000010 处的 uint16 (Bootloader 版本字段) |
| `FlashDownload_GetFirmwareCRC()` | 读 APP2 Flash 末尾 -4 处 (`0x4C000 + 0xC000 - 4`) 的 uint32 |

### 内部写路径详解

```
FlashDownload_OnTransferData(data, len)
  → fw_write_buffer_to_flash(flash_addr, data, len)
      1. fw_is_address_valid()         // 映射后地址在 0x4C000–0x58FFF?
      2. fw_is_address_protected()     // FlashAdv_IsAddressProtected() 检查
      3. memcpy(g_fw_ram_buffer, data, len)  // 拷到 60KB RAM 对齐缓冲区
      4. 不足 4 字节对齐的补 0xFF
      5. FlashAdv_BulkWriteSimple(handle, addr, g_fw_ram_buffer, word_count)
         → FlashAdv_BulkWrite()
           → for each word: FlashAdv_WriteWord() → HC32FLASH_WritedWord_Check()
           → verify_enabled: 回读校验
```

---

## 五、param_manager.c/.h — 磨损均衡参数存储 (绕过 FlashAdvanced)

**位置:** `boot/Utils/`, `app1/Utils/`, `app2/Utils/` (三个项目内容一致)

**保护:** ❌ 绕过 FlashAdvanced, 直接调 `hc32f46x_flash.c`

**可操作区域:** 扇区 56–61 (0x70000–0x7BFFF), 6×8KB=48KB

```c
#define SEC_START  61   // 最高优先级, 从扇区 61 开始扫描
#define SEC_END    56   // 最低扇区 56
#define SECTOR_SIZE 0x2000
```

### 公开接口

| 函数 | 操作 | 用途及关键参数 |
|------|:---:|------|
| `Param_Init(pConfig, pSetDefaults)` | 读取 | 扫描扇区 61→56, 找 valid block (magic head+tail + CRC 校验), 取最高 SeqID 的 block 加载到 `pConfig->pParamBuf`。若无有效 block, 调用 `pSetDefaults()` 初始化默认值并 `Param_Save` 写入 |
| `Param_Save(pConfig)` | 擦除+写 | 将 `pConfig->pParamBuf` 写入下一可用位置。若当前扇区空间不足 → 擦除当前扇区后重写 (若擦除次数 <10000) 或移到下一扇区 (磨损均衡回绕 56→61)。写入含: MagicHead, SeqID+1, 数据, CRC32, MagicTail。写入后回读校验。最多 7 次重试 |
| `Param_Debug_EraseAll(pConfig, pDefaults)` | 擦除+写 | 擦除全部 6 个扇区 (56-61), 然后 `Param_Init` 重建 |
| `Param_EraseSector(address)` | 擦除 | 公开的擦除接口, 直接调 `Internal_Erase` |

### 内部函数

| 函数 | 操作 | 用途 |
|------|:---:|------|
| `Internal_Erase(address)` | 擦除 | 直接调 `HC32FLASH_EraseSector(address)`, 关中断保护 |
| `Internal_ReadWord(addr)` | 读取 | 直接调 `HC32FLASH_ReaddWord(addr)`, 关中断保护 |
| `Internal_WriteWord(addr, data)` | 写入 | 直接调 `HC32FLASH_WritedWord_Check(addr, data)`, 关中断保护 |
| `Internal_WriteBuffer(addr, buf, words)` | 写入 | 逐字调用 `Internal_WriteWord`, 失败立即返回 |
| `Internal_VerifyBuffer(addr, buf, words)` | 读取 | 逐字回读并比较, 不匹配返回 false |
| `CalcParamCRC(buf, size, checksumOffset)` | — | 软件 CRC32 校验, 排除 checksum 字段自身 |
| `GetField32(buf, offset)` / `SetField32(buf, offset, val)` | — | 从字节缓冲区偏移处读写 uint32 |

### 每扇区存储格式

```
+0x000  MagicHead    0x55AA55AA
+0x004  Sequence ID  递增序号 (磨损均衡依据)
+0x008  Erase Count  本扇区累计擦除次数
+0x00C  Param Data   用户参数结构体 (大小由 Param_Config_t.paramSize 定义)
+...    ...
+N-8     CRC32       软件 CRC 校验 (排除自身字段)
+N-4     MagicTail   0xAA44AA44
```

---

## 六、全部 Flash 写操作 — 按扇区汇总

| 扇区 | 地址 | 写入者 | 函数 | 经 FlashAdv? |
|------|------|--------|------|:---:|
| 8 | 0x10000 | Bootloader_App | `UdsShared_Write`, `UdsShared_SetPhase`, `UdsShared_Clear` | ❌ |
| 9 | 0x12000 | flash_advanced | `FlashAdv_SaveLifetimeInfo` | ✅ (自身) |
| 11 | 0x16000 | Bootloader_App | `SetWdtFeedControl`, `UpdateWdtResetCount`, `ClearWdtResetCount` | ❌ |
| 12 | 0x18000 | Bootloader_App | `SetWdtFeedControl`, `UpdateWdtResetCount`, `ClearWdtResetCount` | ❌ |
| 38-47 | 0x4C000 | flash_download | `FlashDownload_OnRequestDownload` (擦除), `FlashDownload_OnTransferData` (写入) | ✅ |
| 56-61 | 0x70000 | param_manager | `Param_Save`, `Param_Debug_EraseAll` | ❌ |
| 62 | 0x7C000 | Bootloader_App | `Boot_SetRunSlotToAddr`, `Boot_SwitchAndRunOther`, `UpdateSlotFlagToFlash` | ❌ |

---

## 七、读写路径速查

| 操作 | 调用链 | 保护? |
|------|--------|:---:|
| **OTA 下载固件** | `FlashDownload_OnTransferData → fw_write_buffer_to_flash → FlashAdv_BulkWriteSimple → FlashAdv_WriteWord → HC32FLASH_WritedWord_Check` | ✅ |
| **OTA 擦除目标区** | `FlashDownload_OnRequestDownload → fw_erase_range → FlashAdv_EraseSector → HC32FLASH_EraseSector` | ✅ |
| **写 UDS Shared** | `UdsShared_Write → EFM_REG_Unlock → EFM_SectorErase → EFM_ProgramWord×14 → EFM_REG_Lock` | ❌ |
| **写 APP_RUN_SLOT** | `Boot_SetRunSlotToAddr → EFM_REG_Unlock → EFM_SectorErase → EFM_ProgramWord → EFM_REG_Lock` | ❌ |
| **写 WDT State** | `UpdateWdtResetCount → READ_FLASH_DIRECT×2 → EFM_Erase → EFM_ProgramWord×2` | ❌ |
| **写参数** | `Param_Save → Internal_Erase(HC32FLASH_EraseSector) → Internal_WriteBuffer(HC32FLASH_WritedWord_Check)` | ❌ |
| **FlashAdv 保存寿命** | `FlashAdv_SaveLifetimeInfo → HC32FLASH_EraseSector → HC32FLASH_WritedWord_Check×N` | ✅ (自身) |

---

## 八、编译安全开关

| 宏 | 文件 | 作用 |
|----|------|------|
| `FW_FLASH_WRITE_ENABLED` | flash_download.h | 0 = OTA 干运行 (不真写 Flash), 1 = 真实写入 |
| `FLASH_DEBUG_ENABLE` | hc32f46x_flash.h | 打印每次 Flash 操作状态 |
| `FLASH_MANAGEMENT_RECORD_ENABLE` | flash_advanced.h | 开启寿命记录持久化到扇区 9 |
| `PARAM_DEBUG` | param_manager.h | 打印参数扫描/磨损统计 |

---

## 修改记录（2026-08-02）

| 修改项 | 说明 |
|--------|------|
| APP1/APP2 分区 80KB | 速查表、汇总表同步：APP1 扇区13-22（0x1A000~0x2DFFF），APP2 扇区38-47（0x4C000~0x5FFFF）；`FW_APP_MAX_SIZE=0x14000`（80KB，10 扇区） |
| Bootloader 预留 48KB | 扇区 0-5（0x0~0xC000），boot 镜像实测 46.55KB；扇区 6-7 预留 |
| PB6 相位指示删除 | OTA 工程 Phase 1/2/3 闪烁与 `ShowBootStatus()` 全部移除（涉及 `uds_diagnostic.c`、`Bootloader_App.c`，不含本文件的 Flash 函数） |
| 四驱工程移植实测通过 | 复用本工程 boot，Phase 1→2→3 全链路验证通过（详见四驱 `app/can/uds/uds移植.md`） |

## 四驱控制盒工程变体（D:\260706_NL）

四驱工程没有 FlashAdvanced 层，`flash_download.c` 直接使用 `hc32_ll_efm.h` 的 EFM 接口：

| 项目 | OTA 工程 | 四驱工程 |
|------|---------|---------|
| OTA 下载写入 | `flash_download.c → FlashAdv_BulkWriteSimple → hc32f46x_flash.c` | `flash_download.c → EFM_SectorErase / EFM_ProgramWordReadBack`（直写） |
| 参数存储 | 扇区 56-61（0x70000），`param_manager` | 扇区 55（0x6E000），`drv_mcu_flash.c`（`STORAGE_SECTOR_ADDR`） |
| UDS 共享区 | 扇区 8（0x10000），EFM_REG_Unlock + EFM_SectorErase + EFM_ProgramWord | 扇区 8（0x10000），LL_PERIPH_WE + EFM_SectorErase + EFM_ProgramWordReadBack |
| 防越界检查 | 仅地址窗口检查 | 额外增加 `m + size` 越界检查（防止擦到参数区/跳转槽） |
