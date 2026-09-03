# OTA 可靠性分析

> 基于对 `Bootloader_App.c`、`uds_diagnostic.c`、`flash_download.c`、`main.c`、`Timer0_Unit2.c` 等源码的完整阅读。
> 分析日期：2026-07-06 | 更新日期：2026-07-12（双APP故障救援、pending_sid修复、相位指示器）
>
> **相关文档:** [CAN适配层修改.md](CAN适配层修改.md) — CAN 驱动架构与缓冲区修复

---

## 一、跳转流程全景图

```
上电 → Boot_StartupSequence()
         │
         ├─ [1] InitSharedCtrl()          // 共享RAM初始化 (debug_flag恢复WDT控制)
         ├─ [2] UDS Shared检查              // 读扇区8 magic/phase
         │    ├─ phase==ENTER_BOOTLOADER → Bootloader_UdsMain() [永不返回的while(1)]
         │    └─ 其他 → 继续正常启动
         ├─ [3] GetWdtResetType()          // 读RMU寄存器，判断是否WDT复位
         ├─ [4] GetCurrentSlot()           // 读 0x7C000 → SLOT_A_MAGIC/SLOT_B_MAGIC
         ├─ [5] ValidateSlotFlag()         // SLOT_NONE → 默认SLOT_APP1
         ├─ [6] InitAppInfo(APP1+APP2)     // 读WDT计数，默认标记AVAILABLE
         ├─ [7] UpdateAppState()           // WDT>=MAX(3) → DISABLED
         ├─ [8] HandleWatchdogReset()      // WDT复位→计数+1→可能触发DISABLED
         ├─ [9] SelectTargetSlot()         // 选择目标APP (见决策矩阵)
         ├─ [10] UpdateSlotFlagToFlash()   // slot变化时回写到扇区62
         │
         └─ [11] Bootloader_JumpToApp()
              ├─ 验证 Reset Vector ≠ 0xFFFFFFFF
              ├─ 关 SysTick、关全局中断、清NVIC
              ├─ 设 MSP、VTOR → 跳转到APP Reset Handler
              └─ 或 RunBootloaderForever() → UdsOta_Bootloader_Enter() [进入UDS救援模式, 等待TBOX OTA]
```

### Bootloader_JumpToApp 跳转前检查清单

| 步骤 | 操作 | 代码位置 |
|------|------|---------|
| 1 | 读 APP 起始地址的 SP 和 Reset Vector | `:243-244` |
| 2 | 检查 Reset Vector ≠ `0xFFFFFFFF`（全擦除状态） | `:251-254` |
| 3 | 停 SysTick (`SysTick->CTRL=0`, `SCB_ICSR_PENDSTCLR`) | `:260-261` |
| 4 | 关全局中断 + 禁用所有 NVIC (128个) | `:264-265` |
| 5 | 清 NVIC 使能和挂起寄存器 (8×32bit) | `:268-272` |
| 6 | 设 MSP = APP 向量表[0] | `:275` |
| 7 | 设 VTOR = APP 起始地址 | `:276` |
| 8 | `__DSB()` + `__ISB()` 屏障 | `:278-279` |
| 9 | 跳转: `(*((void(*)(void))app_start_address))()` | `:282` |

---

## 二、按 APP 故障数 × 故障时间分类排查

### 维度说明

| 维度 | 分类 |
|------|------|
| **APP故障数** | ① 均正常 ② 单故障(APP1坏/APP2坏) ③ 均故障 |
| **故障时间** | 刷写前 / 刷写中 / 刷写后 |

---

### 🔵 场景 1：APP均正常 + 刷写前（OTA触发阶段）

**流程：**
```
TBOX → 0x31 → APP1收到 
  → 直接写 UDS Shared Flash:
      magic=UDS_SHARED_MAGIC, phase=ENTER_BOOTLOADER, pending_sid=0x31
  → g_delayed_reset_ms = DELAYED_RESET_MS(100)
  → *resp_len = 0 (不发CAN响应)
  → NVIC_SystemReset()
  
Boot_StartupSequence:
  → 读 UDS Shared → phase=ENTER_BOOTLOADER 
  → Bootloader_UdsMain()
    → 读 pending_sid == 0x31 → 发送 71 01 FF 00 → 清零 pending_sid
    → 初始化 FlashDownload + UDS → 主循环等下载
```

**现有保护：**
- ✅ UDS Shared 有 magic 校验（`0x55445300`）和 pending_sid 校验（仅 `==0x31` 才发 ACK）
- ✅ 需要安全解锁流程（`10 03` → `27 01/02` → 才能 `31 01`）
- ✅ 延迟 ACK 设计：APP 不响应 0x31，由 Bootloader 启动后补发 `71 01 FF 00`

**潜在问题：**

| # | 问题 | 严重度 | 代码位置 | 说明 |
|---|------|:---:|------|------|
| B1 | **UDS Shared 无 phase 冗余校验** | 🟡 P1 | `Boot_StartupSequence:374-384` | 仅靠一个 magic 判断。Phase 2 增加了 `pending_sid` 校验（仅 `==0x31` 才发 ACK），部分缓解但未根治 |
| B2 | **进入 UDS 模式后无超时退出** | 🟡 P1 | `Bootloader_UdsMain:677` | `while(1)` 死循环。双APP故障恢复模式下可重新 OTA 修复，但单 APP 正常时仍存在超时风险 |
| B3 | **Boot_SetRunSlotToAddr(APP1) 过早执行** | 🟢 P2 | `uds_handle_routine_control` Bootloader分支 | 刷写还没开始，`APP_RUN_SLOT` 就被设成 APP1。当前行为实际安全(作为回退)，但语义不清晰 |

---

### 🔵 场景 2：APP均正常 + 刷写中（固件传输+烧录阶段）

**流程：**
```
Bootloader_UdsMain() 主循环 (while(1)):
  0x34 RequestDownload → 擦除 APP2 扇区(38-43, 6×8KB=48KB)
  0x36 TransferData × N → RAM buffer(60KB) → FlashAdv_BulkWriteSimple → APP2
  0x37 TransferExit → 冲刷buffer + CRC32校验
  FlashDownload_GetState()==FW_UPDATE_COMPLETE 
    → 写 UDS shared: phase=PROGRAMMING_DONE, result=1, target_slot=APP2
  0x11 ECU Reset → 写 pending_sid=0x11 → NVIC_SystemReset()
```

**现有保护：**
- ✅ FlashAdvanced 保护：擦写前检查扇区 ∈ [0-12] 或 =63 → 拒绝
- ✅ CRC32 运行校验（逐块更新 `rx_crc`，0x37 完成时取反验证）
- ✅ 块序号顺序校验（`expected_sequence` 1→255，跳号拒绝）
- ✅ 写入后硬件回读 + 软件二次验证（`HC32FLASH_WritedWord_Check`）
- ✅ 500ms 间隔喂 SWDT（`SWDT_FeedDog()`）
- ✅ TBOX 地址映射校验（`0x08004xxx` → `0x0004Cxxx`，范围检查）

**潜在问题：**

| # | 问题 | 严重度 | 代码位置 | 说明 |
|---|------|:---:|------|------|
| B4 | **擦除 APP2 后、写入完成前断电/复位** | 🔴 P0 | `flash_download.c:fw_erase_range` | APP2 扇区已被擦除（全 `0xFF`），固件损坏。复位后 `InitAppInfo` **不检查向量表有效性**，标记 APP2 为 `AVAILABLE`。跳转时 `Bootloader_JumpToApp` 检测到 `0xFFFFFFFF` 会拒绝跳转并 `return`——但 `return` 后并没有 fallback 到另一个 APP |
| B5 | **刷写中 WDT 复位后重新进入 UDS 模式** | 🔴 P0 | `Boot_StartupSequence:374-384` | WDT 复位→RAM 清空→FlashDownload 状态机丢失。但 UDS phase 可能仍是 `ENTER_BOOTLOADER`（下载完成前不变）→ 再次进入 `Bootloader_UdsMain`。此时 APP2 半损坏，且 `FlashDownload_Init` 重新初始化后不知道之前的状态 |
| B6 | **擦除 APP2 前未写"刷写进行中"标记** | 🟡 P1 | UDS Shared 结构 | 无法区分"从未刷过"（APP2 为空）和"刷写中断"（APP2 半损坏）。这两种情况的恢复策略应不同 |
| B7 | **60KB RAM buffer 无 ECC/校验** | 🟢 P2 | `flash_download.c:g_fw_ram_buffer` | 数据在 RAM 中等待凑满时发生 bit-flip，CRC 能发现但已浪费一次 Flash 擦写寿命 |
| B8 | **擦除范围依赖 TBOX 传入的 size** | 🟡 P1 | `uds_handle_request_download` | 如果 TBOX 传错 size，可能擦除超出 APP2 区域的范围。好在有 `flash_download.c` 的地址检查（`fw_is_address_valid`）拦截——前提是检查逻辑正确 |

---

### 🔵 场景 3：APP均正常 + 刷写后（跳转+ACK阶段）

**流程：**
```
0x11 Reset → Bootloader重启 
  → 读 UDS Shared: phase=PROGRAMMING_DONE (≠ENTER_BOOTLOADER)
  → 跳过UDS模式 → 正常启动流程
  → SelectTargetSlot: 读 APP_RUN_SLOT=APP1(0x5A5A5A5A)
  → Bootloader_JumpToApp(APP1_START_ADDR=0x1A000)

APP1 main():
  SCB->VTOR = APP1_START_ADDR
  Hardware_Init()
  App_CheckPendingUdsAck():
    读UDS Shared → pending_sid=0x11
    → 发 RAW CAN 51 01 (手动构造ISOTP单帧 PCI=0x04)
    → UdsShared_Clear() 擦除扇区8
```

**现有保护：**
- ✅ `Bootloader_JumpToApp` 检查 Reset Vector ≠ `0xFFFFFFFF`
- ✅ 跳转前完整清理中断环境（SysTick + NVIC + 全局中断）
- ✅ 延迟 ACK 跨复位传递（pending_sid 机制）

**潜在问题：**

| # | 问题 | 严重度 | 代码位置 | 说明 |
|---|------|:---:|------|------|
| B9 | **OTA 后始终启动 APP1（硬编码）** | 🟡 P1 | `Bootloader_App.h:31` | `UDS_POST_FLASH_BOOT_ADDR = APP1_START_ADDR`。新固件在 APP2，但系统启动旧固件 APP1。代码注释已承认这是开发期硬编码 |
| B10 | **51 01 ACK 用 RAW CAN 无重发机制** | 🟡 P1 | `App_CheckPendingUdsAck:743-757` | 手动构造 ISOTP 单帧，不经过 ISOTP 层。若 CAN 总线繁忙导致仲裁丢失，TBOX 永远收不到 ACK。注：依赖的 `CanIf_Send` 在 `Hardware_Init` → `CanIf_Init` 之后已就绪（CAN 硬件层正常工作，参见 [CAN适配层修改.md](CAN适配层修改.md)） |
| B11 | **UdsShared_Clear 在 ACK 发出后立即执行** | 🟢 P2 | `App_CheckPendingUdsAck:753` | CAN 帧可能还在 TX mailbox 中，此时复位会导致 ACK 丢失。实际风险低（数据已拷贝到 CAN 控制器），但建议加延时 |

---

### 🟡 场景 4：单 APP 故障 + 刷写前

**子场景 4A：APP1 故障(WDT>=3)，APP2 正常**

| # | 问题 | 严重度 | 说明 |
|---|------|:---:|------|
| B12 | **OTA 目标永远是 APP2——自毁风险** | 🔴 P0 | APP1 已故障，APP2 是唯一正常 APP。OTA 擦除 APP2 是自杀行为。一旦刷写失败→双 APP 均故障→变砖 |
| — | Slot 选择逻辑 | ✅ | `SelectTargetSlot` 正确 fallback 到 APP2 |

**子场景 4B：APP2 故障，APP1 正常**

| # | 问题 | 严重度 | 说明 |
|---|------|:---:|------|
| — | OTA 擦除 APP2 写入新固件 | ✅ | 合理：用 OTA 修复已损坏的 APP2 |
| B13 | **APP2 故障原因可能是 Flash 物理损坏** | 🟡 P1 | 如果 APP2 区域的 Flash 物理损坏（擦除寿命耗尽），OTA 擦写会失败且无法恢复。应检查 FlashAdvanced 的扇区寿命统计 |

---

### 🟡 场景 5：单 APP 故障 + 刷写中

| # | 问题 | 严重度 | 说明 |
|---|------|:---:|------|
| — | 与场景2相同 | — | 刷写中断问题同样存在 |
| B14 | **APP1 在刷写期间不可能因 WDT 变故障** | ✅ | 刷写期间运行的是 Bootloader，APP 根本没运行。但需注意：**APP1 在刷写前已有的 WDT 计数不会被清除** |

---

### 🟡 场景 6：单 APP 故障 + 刷写后

| # | 问题 | 严重度 | 说明 |
|---|------|:---:|------|
| — | 刷写成功+APP1故障 | ✅ | `SelectTargetSlot` fallback 到 APP2（新固件），系统恢复 |
| B15 | **刷写失败+APP1故障 → 双APP故障** | 🔴 P0 | APP2 损坏(刷写失败) + APP1 故障(已有) → `RunBootloaderForever` → 变砖 |

---

### 🟢 场景 7：双 APP 均故障 ✅ 已修复

**触发路径：**
1. APP1 WDT 复位 ≥3 次 → `APP_STATE_DISABLED`
2. APP2 WDT 复位 ≥3 次（或 OTA 擦除后未成功写入、或固件本身崩溃）
3. `SelectTargetSlot` → `eTargetSlot = SLOT_NONE`
4. `RunBootloaderForever()` → `UdsOta_Bootloader_Enter()` → `Bootloader_UdsMain()` → **UDS 编程模式等待救援**

**修复后代码 (`Bootloader_App.c:519-524`)：**
```c
static void RunBootloaderForever(void) {
    MAIN_D("  Both APPs disabled, entering UDS programming mode for recovery\r\n");
    UdsOta_Bootloader_Enter();
    while(1) { __nop(); }
}
```

**救援流程：**
```
双APP故障 → RunBootloaderForever → Bootloader_UdsMain
  → pending_sid=0 → 跳过31 ACK
  → UDS初始化 + while(1): UdsOta_Poll()
  → TBOX: 10 02 → 27 → 34/36/37 → 下载新固件 → FW_UPDATE_COMPLETE
  → ClearAppStateBySlot(SLOT_APP2) ← 清零APP2的WDT计数
  → TBOX: 11 01 → 复位
  → Boot_StartupSequence: APP2 WDT=0 → AVAILABLE → JumpToApp(APP2)
```

| # | 问题 | 严重度 | 状态 |
|---|------|:---:|:--:|
| B16 | ~~双 APP 故障时无救援通道~~ | 🔴→🟢 | ✅ 已修复 |
| B17 | ~~双 APP 故障时无法进入 Bootloader_UdsMain~~ | 🔴→🟢 | ✅ 已修复 |

---

## 三、SelectTargetSlot 决策矩阵

```
当前Slot │ APP1状态 │ APP2状态 │ → 目标Slot │ 说明
────────┼─────────┼─────────┼───────────┼────────────────
 APP1   │  AVAIL  │  AVAIL  │   APP1    │ 正常，保持不变
 APP1   │  AVAIL  │ DISABLE │   APP1    │ 正常
 APP1   │ DISABLE │  AVAIL  │   APP2    │ 自动fallback ✅
 APP1   │ DISABLE │ DISABLE │   NONE    │ ☠️ RunBootloaderForever
 APP2   │  AVAIL  │  AVAIL  │   APP2    │ 正常
 APP2   │  AVAIL  │ DISABLE │   APP1    │ 自动fallback ✅
 APP2   │ DISABLE │  AVAIL  │   APP2    │ 正常
 APP2   │ DISABLE │ DISABLE │   NONE    │ ☠️ RunBootloaderForever
 NONE   │  AVAIL  │  AVAIL  │   APP1    │ ValidateSlotFlag默认
 NONE   │  AVAIL  │ DISABLE │   APP1    │
 NONE   │ DISABLE │  AVAIL  │   APP2    │
 NONE   │ DISABLE │ DISABLE │   NONE    │ ☠️ RunBootloaderForever
```

---

## 四、APP 状态判定逻辑（当前实现）

### InitAppInfo (`:439-445`)
```c
static void InitAppInfo(stc_app_info_t *pstcApp, en_slot_type_t eSlot, uint32_t u32Addr) {
    pstcApp->eSlot = eSlot;
    pstcApp->u32StartAddr = u32Addr;
    uint32_t cnt = (eSlot == SLOT_APP1) ? READ_FLASH_DIRECT(WDT_COUNT_APP1_ADDR)
                                        : READ_FLASH_DIRECT(WDT_COUNT_APP2_ADDR);
    pstcApp->u32WdtCount = (cnt == 0xFFFFFFFF) ? 0 : cnt;
    pstcApp->eState = APP_STATE_AVAILABLE;  // ← 默认就是 AVAILABLE!
}
```

### UpdateAppState (`:447-449`)
```c
static void UpdateAppState(stc_app_info_t *pstcApp) {
    pstcApp->eState = (pstcApp->u32WdtCount >= MAX_WDT_RESET_COUNT)
                      ? APP_STATE_DISABLED
                      : APP_STATE_AVAILABLE;
}
```

### 🔴 核心缺失：无固件完整性检查

`InitAppInfo` 将 APP 初始状态设为 `AVAILABLE`，后续仅依据 WDT 计数判断。**完全没有检查 APP 固件是否真的存在且有效**：

- 不检查 APP 向量表的 SP 值（是否在 RAM 范围内）
- 不检查 APP 向量表的 Reset Vector（是否为有效 Flash 地址）
- 不检查 APP 固件的 CRC 或签名
- 一个被擦除的 APP（全 `0xFF`）也会被标记为 `AVAILABLE`

`Bootloader_JumpToApp` 中虽然有 `0xFFFFFFFF` 检查（`:251`），但返回后并没有 fallback 逻辑——因为该函数的调用者已经在 `Boot_StartupSequence` 末尾，只有一个线性的 if-else 判断。

---

## 五、全部已知 Bug 和风险汇总

### 🔴 P0 — 会导致设备变砖

| # | 标题 | 代码位置 | 触发条件 | 状态 |
|---|------|------|------|:--:|
| **B16/B17** | ~~双 APP 故障时无救援通道~~ | `Bootloader_App.c:519-524` | 双WDT≥3 | ✅ 已修复 |
| **B4** | **擦除后 APP2 向量表无效但被标记 AVAILABLE**：`InitAppInfo` 不检查固件完整性 | `Bootloader_App.c:439-445` | OTA擦除后断电 | — |
| **B5** | **刷写中 WDT 复位后重新进入 UDS 模式**：FlashDownload 状态丢失，APP2 半损坏 | `Boot_StartupSequence:374-384` | OTA中途WDT复位 | — |
| **B12** | **OTA 刷写唯一正常 APP 无自保**：APP1 故障时不应擦除 APP2 | `flash_download.c`, `uds_diagnostic.c 0x31 handler` | APP1故障+OTA触发 | — |

### 🟡 P1 — 会导致临时不可用或恢复困难

| # | 标题 | 代码位置 | 触发条件 |
|---|------|------|------|
| **B2** | **`Bootloader_UdsMain` 无超时退出**：CAN 丢失后永远卡住 | `Bootloader_App.c:677` | CAN通信中断 |
| **B6** | **无"刷写进行中"标记**：无法区分空 APP2 和半损坏 APP2 | UDS Shared 结构(扇区8) | OTA中断后重启 |
| **B8** | **擦除范围依赖 TBOX 传入参数**：虽然有地址检查，但检查边界值得审视 | `flash_download.c:fw_is_address_valid` | TBOX发错误参数 |
| **B9** | **OTA 后始终启动 APP1(硬编码)**：新固件在 APP2 但不运行 | `Bootloader_App.h:31` | OTA完成后 |
| **B10** | **51 01 ACK 用 RAW CAN 无重发**：CAN 仲裁丢失则 TBOX 永远收不到 | `App_CheckPendingUdsAck:736-749` | CAN总线繁忙 |
| **B13** | **APP2 Flash 物理损坏时 OTA 无感知**：应检查 FlashAdvanced 寿命 | `flash_download.c` | Flash寿命耗尽 |
| **B1** | **UDS Shared phase 无冗余校验**：脏数据可能导致误入编程模式 | `Boot_StartupSequence:374-384` | Flash数据翻转 |

### 🟢 P2 — 改进建议

| # | 标题 | 代码位置 |
|---|------|------|
| **B7** | 60KB RAM buffer 无校验保护 | `flash_download.c:g_fw_ram_buffer` |
| **B3** | `Boot_SetRunSlotToAddr(APP1)` 在刷写前执行，语义不清晰 | `uds_diagnostic.c 0x31 handler (BL分支)` |
| **B11** | `UdsShared_Clear` 在 ACK 发出后立即执行无延时 | `App_CheckPendingUdsAck:753` |
| B18 | WDT 计数读-擦-写协议：擦除中断电可能丢失 feed_ctrl | `UpdateWdtResetCount:114-143` |
| B19 | `Bootloader_JumpToApp` 中 `return` 后无 fallback 处理 | `:252-253` |
| B20 | `ShowBootStatus` 的 LED 闪烁仅在调试时有价值，量产无意义 | `Bootloader_App.c:500-519` |

---

## 六、建议修复优先级

### 第一优先（防变砖）

1. **`RunBootloaderForever` 改为进入 UDS 救援模式**
   - 在死循环之前启动 CAN/UDS，等待外部 OTA 救援
   - 或直接在 `Boot_StartupSequence` 中双 APP 故障时进入 `Bootloader_UdsMain`

2. **`InitAppInfo` 增加固件有效性检查**
   ```c
   // 建议增加的检查：
   uint32_t sp = READ_FLASH_DIRECT(u32Addr);
   uint32_t reset_vec = READ_FLASH_DIRECT(u32Addr + 4);
   if (sp < RAM_START_ADDR || sp > RAM_END_ADDR || 
       reset_vec == 0xFFFFFFFF || reset_vec < APP1_START_ADDR) {
       pstcApp->eState = APP_STATE_DISABLED;  // 固件无效
   }
   ```

3. **OTA 前检查另一个 APP 的健康状态**
   - 在 0x31 handler 或 `FlashDownload_OnRequestDownload` 中：如果非目标 APP 已故障，拒绝擦除

### 第二优先（提高可靠性）

4. **`Bootloader_UdsMain` 增加超时退出**
   - 增加一个倒计时（如 30 秒无 UDS 通信→退出到正常启动）

5. **增加"刷写进行中"标记**
   - 在 UDS Shared 中增加 `flash_in_progress` 字段
   - 擦除 APP2 前写 1，完成或失败后写 0
   - Bootloader 启动时检测到 `flash_in_progress=1`，重新初始化下载状态

6. **51 01 ACK 增加重发或改用 ISOTP 发送**

### 第三优先（长期改进）

7. OTA 后启动目标改为动态判断（当前硬编码 APP1）
8. UDS Shared 双份冗余存储
9. 增加固件整体 CRC 校验（非仅 OTA 过程的传输 CRC）
10. RAM buffer 增加分块软件 CRC

---

---

## 八、2026-07-12 修改记录

### 已修复问题

| 修复项 | 涉及文件 | 说明 |
|--------|---------|------|
| **双 APP 故障救援 (B16/B17)** | `Bootloader_App.c` (boot/app1/app2) | `RunBootloaderForever()` 改为调用 `UdsOta_Bootloader_Enter()`，进入 UDS 编程模式等待 TBOX 救援 |
| **救援后 WDT 清零** | `Bootloader_App.c` (boot/app1/app2) | `FW_UPDATE_COMPLETE` 后自动调用 `ClearAppStateBySlot(SLOT_APP2)`，确保复位后新固件可启动 |
| **WDT 计数持久化** | `Bootloader_App.c` (boot/app1/app2) | `g_u32Debug_ClearAppState` 从 3 改为 0，WDT 计数跨复位保留 |
| **Phase 1 pending_sid** | `uds_diagnostic.c` (boot/app1/app2) | 写 Flash 时同时设置 `pending_sid=0x31`（不再使用 `UdsShared_SetPhase` 清零该字段） |
| **Phase 2 条件 31 ACK** | `Bootloader_App.c` (boot/app1/app2) | 读取 `pending_sid`，仅 `==0x31` 时才发送 71 01 FF 00，发送后清零 |
| **PB6 相位指示器** | `uds_diagnostic.c`, `Bootloader_App.c`, `main.c` (boot/app1/app2) | Phase 1/2/3 各用 PB6 闪烁 3 次（100/200/300ms），替代 main.c 阻塞调试闪烁 |
| **app2 UDS 对齐** | `uds_ota.h/c` (新建), `main.c`, `Bootloader_App.c` (app2) | app2 获得与 app1 一致的 UDS 封装层 |

### 仍待修复（选中但未实施）

| # | 问题 | 原因 |
|---|------|------|
| B4 | InitAppInfo 无固件完整性检查 | 需增加 SP/ResetVector 范围校验 |
| B5 | 刷写中 WDT 复位的状态恢复 | 需在 UDS Shared 增加"刷写进行中"标记 |
| B12 | OTA 擦除唯一正常 APP 无自保 | 需在 0x31 handler 检查非目标 APP 状态 |

## 九、CAN 驱动修复补充说明 (2026-07-10)

本可靠性分析文档撰写时（2026-07-06），三个固件工程（app1/boot/app2）的 CAN 适配层存在以下问题：

| 工程 | 问题 | 影响 |
|------|------|------|
| app1, boot | RX 缓冲区错位：ISR 写入 `m_stcCanHandle.can_rx`，Poll 读取空的 `m_stcRxCache`，所有接收帧被丢弃 | CAN 通信完全中断，UDS/OTA 流程无法测试 |
| app2 | 使用独立的 `Can_LLD` 驱动层，架构与 app1/boot 不一致 | 维护困难，代码无法复用 |

**修复后（2026-07-10）：**
- 三个工程的 CAN 驱动统一为 `can_module` + `Adapter_Can` 两层架构
- RX 缓冲区统一为 `m_stcCanHandle.can_rx`（ISR 和 Poll 操作同一缓冲区）
- CAN 硬件通信已验证可用

> 详见 [CAN适配层修改.md](CAN适配层修改.md)

**对本分析的影响：** 本文档中的所有 UDS/OTA 流程（会话切换、安全访问、0x34/36/37 下载、延迟 ACK 等）依赖 CAN 通信正常。修复后的 CAN 驱动使这些流程能够在硬件层面正常运行，不影响本文档中分析的所有可靠性问题——这些问题属于协议/逻辑层面，与 CAN 硬件驱动无关。
