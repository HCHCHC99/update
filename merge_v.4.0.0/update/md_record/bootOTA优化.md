# Boot OTA 优化（阶段 2/3）

> 日期: 2026-08-02
> 适用范围: OTA boot（D:\ota_ddl3.3_v.3.1\merge_v.1.0.0\boot）
> 配套: APP 侧说明见 `appOTA优化.md` / 四驱 `四驱APPOTA优化.md`

---

## 一、背景与目标

### 1.1 阶段 1（既有链路）

boot 的既有职责：上电 `Boot_StartupSequence` 读共享区（phase==1 进 UDS 编程模式）→ 读 APP_RUN_SLOT → 选槽 → 跳 APP；Phase 2 负责把固件下载到 APP2（0x4C000），完成后跳 APP1。

局限：
- 进入编程模式依赖 APP 先写 `phase=1`（0x31 流程）或“双 APP 均故障”的恢复路径；
- 无法在 APP 不可用/未参与的情况下强制进入编程模式；
- 无法在运行时强制切换下次启动槽位。

### 1.2 阶段 2/3 的目标

- **阶段 2**：boot 上电后 50ms 窗口检测强制指令 `0x18FF5858 / 0xFF` → 不依赖 APP 直接进入 UDS 编程模式（防变砖的独立入口）；
- **阶段 3**：窗口内检测 `0x18FF5858 / 0x01|0x02` → 坏块检查 → 设置下次自动启动槽位（APP1/APP2）→ 回帧确认 → 软复位 → 按新槽正常启动。

---

## 二、指令 / 回帧协议

### 2.1 指令（CAN ID `0x18FF5858`，扩展帧，只看 data[0]）

| data[0] | 宏 | 含义 |
|---|---|---|
| `0xFF` | `BOOT_FORCE_CMD_ENTER_BL` | 强制进入 bootloader 编程模式（阶段2） |
| `0x01` | `BOOT_FORCE_CMD_BOOT_APP1` | 强制下次启动 APP1（阶段3） |
| `0x02` | `BOOT_FORCE_CMD_BOOT_APP2` | 强制下次启动 APP2（阶段3） |

### 2.2 回帧（CAN ID `0x18EF5858`，扩展帧，DLC=8，data[0]=状态）

| data[0] | 宏 | 含义 |
|---|---|---|
| `0x01` | `BOOT_FORCE_RESP_APP1_OK` | 已设置下次启动 APP1 |
| `0x02` | `BOOT_FORCE_RESP_APP2_OK` | 已设置下次启动 APP2 |
| `0x03` | `BOOT_FORCE_RESP_BOTH_FAULTY` | 双 APP 均故障，进入编程模式等待刷写 |
| `0x04` | `BOOT_FORCE_RESP_REJECTED` | 目标坏块标记≥3，拒绝强制跳转（槽位未修改） |

---

## 三、设计

### 3.1 检测窗口：`Boot_StartupSequence()` 最开头的 50ms

boot 的 CAN 在 main 中已初始化（`Hardware_Init → UdsOta_Init`），因此启动序列一开始就具备接收能力：

```c
MAIN_D("===== Bootloader Start =====");

/* 阶段2/3: 上电 50ms 强制指令检测窗口 */
{
    static bool s_force_filter_registered = false;
    uint64_t u64WinStart;

    if (!s_force_filter_registered) {          /* 注册一次 0x18FF5858 过滤器 */
        CanIf_RxFilterEntry_t stcForceEntry;
        MEM_ZERO_STRUCT(stcForceEntry);
        stcForceEntry.u32CanId    = BOOT_FORCE_CMD_CAN_ID;
        stcForceEntry.u32CanMask  = 0UL;       /* 精确匹配 */
        stcForceEntry.u8Format    = CAN_ID_EXT;
        stcForceEntry.pfnCallback = Boot_ForceCmdRxCallback;
        s_force_filter_registered = CanIf_RegisterRxFilter(&stcForceEntry);
    }

    s_force_cmd = 0U;
    s_force_window_active = 1U;
    u64WinStart = tickTimer_GetCount();
    while ((tickTimer_GetCount() - u64WinStart) < BOOT_FORCE_CMD_WINDOW_MS) {
        UdsOta_Poll();                          /* 驱动 CanIf_Poll → RX 分发 → 回调置标志 */
        if (s_force_cmd != 0U) {
            ... /* 0xFF / 0x01 / 0x02 分支处理 */
        }
    }
    s_force_window_active = 0U;
}
```

### 3.2 分支处理

| 指令 | 处理 |
|---|---|
| `0xFF` | 打印 → `UdsOta_Bootloader_Enter()`（= `Bootloader_UdsMain()`，复用“双 APP 故障恢复”入口，不返回） |
| `0x01/0x02` | 读 WDT 故障计数（`0x16008`/`0x18008`）→ 擦除态 `0xFFFFFFFF` 归一化为 0 → 判定健康 |
| 双故障 | 回帧 `0x03` → 进编程模式等待刷写（不返回） |
| 目标健康且槽位未变 | 写槽 → 回帧 `0x01/0x02` → 等 TX 空闲（20ms 上限）→ `NVIC_SystemReset()` |
| 目标健康但槽位已是目标值 | 回帧确认 → break（幂等，不重复复位） |
| 目标坏块≥3 | 回帧 `0x04` → break（不写槽，槽位保持原样） |

### 3.3 坏块判定与正常启动序列一致

正常启动的 `InitAppInfo()` 会把擦除态 `0xFFFFFFFF` 视为 0（健康）；窗口检查同样归一化，避免“板子从没写过 WDT 计数 → 全判故障”的误判。

### 3.4 幂等保护（防复位循环）

写槽成功后软复位，第二次进窗口时如果 `APP_RUN_SLOT` 已是目标值 → 只回帧、不写、不复位 → break → 正常启动。这样 TBOX 持续发送指令也不会造成“写槽→复位→写槽→复位”死循环。

### 3.5 回帧可靠性

`Boot_SendForceResp()` 用 `CanIf_Send` 发 `0x18EF5858`；复位前等待 `can_is_tx_busy()==false`（最长 20ms），确保回帧真正上线，而不是被复位丢在 TX 队列里。

---

## 四、程序调用链路

```
上电 → main() → Hardware_Init() → UdsOta_Init()（注册 4 个 ISOTP 过滤器）
  → Boot_StartupSequence()
      │
      ├─ 注册 0x18FF5858 过滤器（Boot_ForceCmdRxCallback）
      ├─ 50ms 窗口循环:
      │     UdsOta_Poll() → CanIf_Poll() → can_read() → CanIf_DispatchRx()
      │           └─ 匹配 0x18FF5858 → Boot_ForceCmdRxCallback → s_force_cmd = data[0]
      │
      ├─ s_force_cmd 分支:
      │     ├─ 0xFF → UdsOta_Bootloader_Enter() → Bootloader_UdsMain()（不返回）
      │     └─ 0x01/0x02:
      │           读 WDT 计数(0x16008/0x18008) → 归一化 → 健康判定
      │             ├─ 双故障 → 回帧0x03 → 进编程模式（不返回）
      │             ├─ 目标健康、槽位≠目标:
      │             │     Boot_SetRunSlotToAddr(APP1/APP2)   ← 写 0x7C000 magic
      │             │     Boot_SendForceResp(0x01/0x02)
      │             │     等 TX 空闲 → NVIC_SystemReset()
      │             ├─ 目标健康、槽位==目标: 回帧 → break（幂等）
      │             └─ 目标坏块≥3: 回帧 0x04 → break（不写槽）
      │
      └─ 窗口结束（无指令/幂等跳过）→ 正常启动序列:
            UDS 共享区检查（phase==1 → 编程模式）
            → GetCurrentSlot() 读 0x7C000
            → InitAppInfo / UpdateAppState / SelectTargetSlot
            → Bootloader_JumpToApp(目标 APP)
```

---

## 五、为什么这样设计

| 设计点 | 原因 |
|---|---|
| **窗口放 `Boot_StartupSequence` 最前** | main 已把 CAN/UDS 初始化好，此时收帧条件齐备；窗口结束后按原启动流程走，改动最小 |
| **CanIf 过滤器回调 + 标志** | boot 的 `CanIf_Poll` 有 RX 分发，过滤器精确匹配 0x18FF5858；回调在主循环上下文（非中断），直接置标志无并发问题 |
| **窗口内调 `UdsOta_Poll()` 驱动接收** | 复用现有轮询链路（ISOTP 超时、CanIf 分发、TX 排空），不用另写收帧逻辑 |
| **坏块判定与 `InitAppInfo` 一致** | 避免窗口与正常启动对同一 APP 判定不一致（0xFFFFFFFF 擦除态必须视为 0） |
| **写槽后软复位，由正常序列跳转** | 统一“跳转决策”路径：槽位是持久化配置，复位后 `GetCurrentSlot→SelectTargetSlot→JumpToApp` 自然生效；也保证 WDT 状态、槽位回写等正常处理不缺失 |
| **幂等（槽位已匹配不重复复位）** | 防 TBOX 持续发送导致“写槽→复位”死循环；同时第二次进窗口天然完成“按槽启动” |
| **回帧后等 TX 空闲再复位** | 保证 0x18EF5858 回帧可靠发出 |
| **`0xFF` 复用 `UdsOta_Bootloader_Enter`** | 与“双 APP 故障恢复”同一入口，无新增状态机；pending_sid==0 时不补发 31 ACK，TBOX 直接走 `10 02→27→34/36/37→11` |
| **拒绝时不写槽** | 用户语义：目标坏块≥3 时既不能强制跳转，也不修改自动跳转配置；槽位保持原样，正常启动按原槽/健康槽走 |

---

## 六、涉及文件

| 文件 | 改动 |
|---|---|
| `Bootloader_App/Bootloader_App.h` | `BOOT_FORCE_CMD_*`（0x18FF5858/0xFF/0x01/0x02/窗口50ms）、`BOOT_FORCE_RESP_*`（0x18EF5858/0x01~0x04） |
| `Bootloader_App/Bootloader_App.c` | `s_force_cmd`/`s_force_window_active`、`Boot_ForceCmdRxCallback`（含 [FRC] 诊断打印）、`Boot_SendForceResp`、50ms 窗口与分支处理 |
| `UDS/isotp_transport.h` | `ISOTP_FILTER_CAN_ID_LIST` 增加 0x18FF5858（OTA 帧打印过滤） |
| `UDS/isotp_transport.c` | 方向/注释支持 0x18FF5858（`<-- ForceBL / ForceBootAPP1 / ForceBootAPP2`） |

---

## 七、测试要点

- 正常: 发 `0x01` → 回帧 `0x01` → RTT `Force boot slot = APP1, resetting...` → 复位 → 跳 APP1；
- 幂等: TBOX 持续发送时第二次窗口应打印 `Slot already APP1, no reset`；
- 坏块: 把 `0x16008`（或 `0x18008`）写成 `0x03` → 回帧 `0x04`，槽位不变；
- 双故障: 两个计数都 ≥3 → 回帧 `0x03` → 进入编程模式等待刷写；
- 擦除态: 计数为 `0xFFFFFFFF` 时必须视为健康（0）。