# OTA-Flash 补充

> 本文档补充 [flash分区.md](flash分区.md)、[flash操作.md](flash操作.md)、[收发流程.md](收发流程.md) 中未覆盖的内容：
> - OTA 跳转全部参数 (Flash + RAM/CPU) 及其存放位置
> - Flash 全部 64 扇区的分配状态与空闲扇区

---

## 一、OTA 跳转全部参数及存放位置

### 1.1 Flash 中的参数

```
参数                    扇区  地址        大小   谁写              谁读              作用
───────────────────    ────  ──────────  ────  ────────────────  ────────────────  ───────────────────
stc_uds_shared_t        8     0x10000     56B   APP(收到0x31)     Bootloader启动    跨复位传递OTA状态
                                                 BL(下载完成)      APP启动(发ACK)
                                                 BL(收到0x11)

  ├─ magic               +0              4B    —                双方               固定0x55445300
  ├─ phase               +4              4B    APP→1             BL启动时决定      0=IDLE
                                                 BL→2             是否进UDS模式      1=进Bootloader
                                                                                    2=下载完成
  ├─ target_slot         +8              4B    BL(下载完成)       —                记录新固件槽位
  ├─ fw_size            +12              4B    BL(下载完成)       —                新固件大小
  ├─ fw_crc             +16              4B    BL(下载完成)       —                新固件CRC32
  ├─ result             +20              4B    BL(下载完成)       —                1=成功
  └─ pending_sid        +24              4B    APP→0x31          APP启动检查       需补发的CAN ACK
                                                 BL→0x11                            0x31/0x11/0

APP_RUN_SLOT magic      62     0x7C000     4B    BL(0x31/启动)    BL启动时          决定启动哪个APP
                                                 Boot_SetRunSlot   GetCurrentSlot    0x5A5A5A5A→APP1
                                                 Boot_SwitchAndRun                   0xA5A5A5A5→APP2

APP1 WDT State          11     0x16000     8B    BL(启动/WDT超时) BL启动时          WDT喂狗+复位计数
  ├─ feed_ctrl (偏移+0)                                                                0=喂狗
  └─ reset_count(偏移+8)                                                               >3→槽位禁用

APP2 WDT State          12     0x18000     8B    同上             同上              同上(APP2用)

APP2 固件 (OTA目标)     38-43  0x4C000    48KB   FlashDownload    —                 OTA下载的新固件
                                                 (经FlashAdvanced)

UDS_POST_FLASH_BOOT_ADDR —     (编译宏)   —     代码写死          BL(0x31处理)      OTA后强制启动APP1
  = APP1_START_ADDR                              Bootloader_App.h  写APP_RUN_SLOT    (=写0x5A5A5A5A到0x7C000)
  = 0x1A000                                      第31行
```

### 1.2 RAM / CPU 中的参数

```
参数                    位置              大小   作用
───────────────────    ────────────────  ────   ──────────────────
SCB->VTOR               CPU 寄存器         —     上下文检测(唯一依据):
                                                   =0         → Bootloader
                                                   =0x1A000   → APP1
                                                   =0x4C000   → APP2

stc_shared_ctrl_t       0x1FFFAF00        32B    WDT喂狗控制(跨复位保持于RAM顶)
  ├─ app1_feed_ctrl        +0                      0=喂狗 / 0xDEADBEEF=不喂
  ├─ app2_feed_ctrl        +4                      同上
  ├─ debug_flag            +8                      调试器复位保持标志
  └─ reserved[5]          +12                      保留

stc_boot_context_t      栈/BSS (BL)      ~100B   Boot_StartupSequence 启动上下文
  ├─ eCurrentSlot                                 当前APP_RUN_SLOT读出的槽位
  ├─ eTargetSlot                                  决定启动哪个APP
  ├─ stcApp1 / stcApp2                            两个槽的健康状态+WDT计数
  └─ u8NeedUpdateSlotFlag                         是否需回写slot标志到Flash

FlashDownloadContext_t   BSS (BL)        ~100B   OTA下载状态机上下文
  ├─ target_address                               =0x4C000 (APP2)
  ├─ total_size / received_size                   下载进度
  ├─ buffer_offset                                RAM缓冲区位置
  ├─ expected_sequence                            块序号校验(1-255)
  ├─ flash_handle                                 FlashAdvanced句柄
  └─ rx_crc                                      运行CRC32

g_fw_ram_buffer          BSS (BL)         60KB   OTA数据对齐工作区
  (FW_RAM_BUFFER_SIZE)
```

### 1.3 三阶段中参数的读写时序

```
Phase 1: APP收到0x31 ──→ 复位
─────────────────────────────
写 [扇8] phase=1, pending_sid=0x31   (UdsShared_SetPhase)
不写 [扇62]                          (APP_RUN_SLOT 不动)
不写 [扇38-43]                       (APP2 不动)
     │
     ↓ NVIC_SystemReset()
     
Phase 2: Bootloader启动 ──→ 下载 ──→ 收到0x11 ──→ 复位  
───────────────────────────────────────────────────────
读 [扇8] phase=1 → 决定进入 UDS 模式
写 [扇62] APP1_MAGIC                 (Boot_SetRunSlotToAddr(UDS_POST_FLASH_BOOT_ADDR))
擦+写 [扇38-43] 新固件              (FlashDownload → FlashAdvanced)
写 [扇8] phase=2, result=1,
         target_slot=APP2            (Bootloader_UdsMain 主循环检测到 COMPLETE)
读 [扇8] 验证 magic                  (uds_handle_ecu_reset)
写 [扇8] pending_sid=0x11           (uds_handle_ecu_reset)
     │
     ↓ NVIC_SystemReset()

Phase 3: Bootloader再次启动 ──→ 跳转APP1 ──→ 发延迟ACK
─────────────────────────────────────────────────────
读 [扇8] phase=2 (≠1) → 跳过 UDS 模式
读 [扇62] APP1_MAGIC → 决定启动 APP1
     ↓ Bootloader_JumpToApp(0x1A000)
读 [扇8] pending_sid=0x11 → 需发 ACK
发 CAN 51 01 (RAW CAN, 不经过ISOTP)
擦 [扇8] UdsShared_Clear() → 回到 IDLE
```

---

## 二、Flash 全部 64 扇区分配状态

### 2.1 完整分配表

```
扇区    地址        大小      用途                   状态        FlashAdvanced视角
────    ────        ────      ──────────────────    ──────      ────────────────
 0-7    0x00000    128KB     Bootloader             已用        保护区(0-12)
 8      0x10000      8KB     UDS Shared State       已用        保护区(0-12)
 9      0x12000      8KB     FlashAdvanced管理记录   已用        保护区(0-12)
10      0x14000      8KB     预留                    空闲 ⬜      保护区(0-12)
11      0x16000      8KB     APP1 WDT State         已用        保护区(0-12)
12      0x18000      8KB     APP2 WDT State         已用        保护区(0-12)
────    ────        ────      ──────────────────    ──────      ────────────────
13-37   0x1A000    200KB     APP1 固件              已用        有效用户区(13-61)
38-43   0x4C000     48KB     APP2 OTA 固件(实际)    已用        有效用户区(13-61)
44-55   0x58000     96KB     ─                      空闲 ⬜      有效用户区(13-61)
56-61   0x70000     48KB     param_manager          已用        有效用户区(13-61)
────    ────        ────      ──────────────────    ──────      ────────────────
62      0x7C000      8KB     APP_RUN_SLOT           已用        灰色(非有效非保护)
63      0x7E000      8KB     硬保护                  不可用 ⛔   单独保护
════    ════        ════      ══════════════════    ══════      ════════════════
```

### 2.2 空闲扇区详情

| 扇区 | 地址 | 大小 | 在哪个区 | 能直接用? | 说明 |
|------|------|------|----------|:---:|------|
| **10** | 0x14000 | 8KB | 保护区 | ⚠️ | 物理空闲。但被 FlashAdvanced 保护 (扇区0-12), 只有 FlashAdvanced 自己能写。外部模块需绕过高阶 API 才能操作 |
| **44-55** | 0x58000 | 96KB | 有效用户区 | ✅ | **最佳扩展区**。无任何模块占用, FlashAdvanced 允许写入, 紧邻 APP2 (扇43), 连续 96KB |
| APP2 未用 | 0x58000-0x7AFFF | 148KB | 有效用户区 | ⚠️ | 链接脚本声明 196KB (扇38-61), OTA 实际只写 48KB (扇38-43)。扇44-55 完全空闲, 扇56-61 已被 param_manager 占用。如需使用扇44-55, 需确保 APP2 链接脚本不覆盖, 且不与 param_manager 冲突 |

### 2.3 空闲空间汇总

```
┌─────────────────────────────────────────┐
│              空闲空间汇总                 │
├──────────┬─────────┬────────┬───────────┤
│ 位置      │ 大小     │ 可用性  │ 备注       │
├──────────┼─────────┼────────┼───────────┤
│ 扇区 10   │   8 KB  │ 受限   │ 保护区     │
│ 扇区 44-55│  96 KB  │ 完全   │ 最佳选择   │
├──────────┼─────────┼────────┼───────────┤
│ 合计      │ 104 KB  │        │           │
│ (实际上)  │  96 KB  │        │ 排除扇区10 │
└──────────┴─────────┴────────┴───────────┘
```

### 2.4 空闲空间可扩展用途

| 用途 | 建议位置 | 需要大小 | 注意事项 |
|------|---------|---------|---------|
| 第二个 OTA 槽 (APP3) | 扇区 44-55 | ~48KB | 需新增 OTA 槽位逻辑 |
| 数据日志分区 | 扇区 44-55 | 可变 | 需实现写入管理 |
| 扩展 param_manager 扇区 | 扇区 44-55 | 可变 | 改 SEC_START/SEC_END |
| 固件备份区 | 扇区 44-55 | 48KB | 需双槽切换逻辑 |
| FlashAdvanced 管理备份 | 扇区 44 | 8KB | 如扇区 9 磨损过度 |

---

## 三、相关文档索引

| 文档 | 内容 |
|------|------|
| [flash分区.md](flash分区.md) | 扇区布局图、FlashAdvanced 分区定义、关键宏、扇区冲突修复 |
| [flash操作.md](flash操作.md) | 全部 Flash 函数 (可操作区域、保护状态、调用链) |
| [收发流程.md](收发流程.md) | UDS OTA 三阶段协议流程、延迟 ACK 机制、0x11/0x31 处理 |
| **OTA-flash补充.md** (本文档) | OTA 跳转参数全集、空闲扇区分析 |
