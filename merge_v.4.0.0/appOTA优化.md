# APP OTA 优化（阶段 2/3）

> 日期: 2026-08-02
> 适用范围: OTA 工程 app1 / app2（D:\ota_ddl3.3_v.3.1\merge_v.1.0.0\app1、app2）
> 配套: boot 侧说明见 `bootOTA优化.md`；四驱 APP 的差异见四驱工程 `四驱APPOTA优化.md`

---

## 一、背景与目标

OTA 工程包含 boot / app1 / app2 三份固件。阶段 2/3 的目标：

- **阶段 2**：APP（app1/app2）运行中收到强制指令 `0x18FF5858 / 0xFF` → 软件复位进 boot → boot 窗口直接进入编程模式（不依赖 0x31 会话）；
- **阶段 3**：APP 运行中收到 `0x18FF5858 / 0x01|0x02` → 软件复位进 boot → boot 窗口做坏块检查、设置下次启动槽位、回帧确认。

**APP 侧只做“检测 → 软件复位”**，窗口检测、坏块判定、写槽、回帧、跳转全部由 boot 完成。

---

## 二、指令协议（APP 侧只接收，不回复）

| CAN ID | data[0] | 含义 | APP 动作 |
|---|---|---|---|
| `0x18FF5858` | `0xFF` | 强制进入 bootloader 编程模式 | 软件复位 |
| `0x18FF5858` | `0x01` | 强制下次启动 APP1 | 软件复位 |
| `0x18FF5858` | `0x02` | 强制下次启动 APP2 | 软件复位 |

> 回帧 `0x18EF5858`（0x01/0x02/0x03/0x04）由 boot 发出，APP 不参与。

---

## 三、设计

### 3.1 检测点：CanIf 过滤器回调（`uds_ota.c`）

OTA 工程的 app1/app2 与 boot 使用同一套 CAN 适配层，**`CanIf_Poll()` 有 RX 分发**（与四驱不同，四驱移除了分发）。因此可以直接注册 0x18FF5858 的精确匹配过滤器，回调在 `UdsOta_Poll → CanIf_Poll` 的主循环上下文被调用：

```c
/* uds_ota.c */
static volatile uint8_t s_force_cmd = 0;      /* 静态标志，仅本文件使用 */

static void ForceCmd_RxCallback(const CanMsg_t *pMsg)
{
    if ((pMsg != NULL) && (pMsg->u8DLC >= 1U)) {
        s_force_cmd = pMsg->au8Data[0];
    }
}

static void ForceCmd_RegisterRxFilter(void)
{
    CanIf_RxFilterEntry_t stcEntry;
    stcEntry.u32CanId    = BOOT_FORCE_CMD_CAN_ID;   /* 0x18FF5858 */
    stcEntry.u32CanMask  = 0UL;                     /* 精确匹配 */
    stcEntry.u8Format    = (uint8_t)CAN_ID_EXT;
    stcEntry.pfnCallback = ForceCmd_RxCallback;
    CanIf_RegisterRxFilter(&stcEntry);
}
```

注册时机：`UdsOta_Init()` 中，与 `ISOTP_RegisterRxFilters()` 并列（CAN 初始化完成后、主循环开始前）。

### 3.2 动作：`UdsOta_Poll()` 检查并软复位

```c
/* UdsOta_Poll() 开头 */
if (s_force_cmd != 0U) {
    uint8_t u8ForceCmd = s_force_cmd;
    s_force_cmd = 0U;
    MAIN_D("Force OTA cmd 0x18FF5858 = 0x%02X, resetting to bootloader...\r\n", ...);
    NVIC_SystemReset();
    while (1) { }
}
```

任意非 0 指令（0xFF/0x01/0x02）一律复位，指令语义由 boot 窗口裁决。

---

## 四、程序调用链路

```
CAN 总线: 0x18FF5858 / 0xFF|0x01|0x02
  │
  ▼
CAN 接收中断 → can_rx 环形缓存
  │
  ▼ (主循环)
UdsOta_Poll()
  └─ CanIf_Poll()
       └─ can_read() → CanIf_DispatchRx()
            └─ 匹配 0x18FF5858 → ForceCmd_RxCallback → s_force_cmd = data[0]
  │
  ├─ (下一轮) UdsOta_Poll() 检查 s_force_cmd != 0
  │     ├─ 打印 "Force OTA cmd 0x18FF5858 = 0xXX"
  │     ├─ NVIC_SystemReset()
  │     └─ while(1)（不返回）
  ▼
OTA boot 启动 → 50ms 窗口 → 按指令语义处理（见 bootOTA优化.md）
```

注：0x18FF5858 不经过 ISOTP（裸指令帧）；同帧也会被 UDS 层的 CAN ID 过滤丢弃，不会产生多余 UDS 响应。

---

## 五、为什么这样设计

| 设计点 | 原因 |
|---|---|
| **用 CanIf 过滤器回调** | app1/app2 的 `CanIf_Poll` 有 RX 分发，过滤器精确匹配天然可用，与 boot 的实现方式一致，代码模式统一 |
| **放在 `uds_ota.c` 内（静态标志）** | 标志只在本文件消费，无需跨文件暴露；`uds_ota.c` 本就是 UDS 栈封装层，职责匹配 |
| **`UdsOta_Init` 时注册** | CAN/过滤器框架已就绪后注册一次即可；主循环开始后回调即生效 |
| **`UdsOta_Poll` 检查动作** | 主循环统一轮询点；回调只置位、Poll 消费，避免在接收路径做复位这种重操作 |
| **任意非 0 都复位** | 指令语义（0xFF/0x01/0x02）由 boot 窗口裁决，APP 不感知具体含义；未来扩展指令码无需改 APP |
| **不写共享区** | 方案 A（boot 再检测）：复位后 boot 50ms 窗口需再次收到指令（TBOX 持续发送）；无 flash 写入、无残留状态 |

---

## 六、与四驱 APP 的差异（为什么分开写）

| | OTA app1/app2 | 四驱 APP |
|---|---|---|
| RX 分发 | `CanIf_Poll` **有**分发 | `CanIf_Poll` **无**分发（全走 `app_can_receive`） |
| 检测落点 | `uds_ota.c` 的 CanIf 过滤器回调 | `uds_rx_entry.c` 的独立分支 |
| 标志 | `s_force_cmd`（文件内 static） | `g_force_ota_cmd`（extern，跨文件） |
| 行为 | 任意指令 → 软复位 | 任意指令 → 软复位（一致） |

两者行为一致、实现因 RX 架构不同而落点不同。

---

## 七、涉及文件（app1、app2 各一份，内容一致）

| 文件 | 改动 |
|---|---|
| `Bootloader_App/Bootloader_App.h` | 新增 `BOOT_FORCE_CMD_*` 宏（0x18FF5858 / 0xFF / 0x01 / 0x02） |
| `UDS/uds_ota.c` | `s_force_cmd` 标志、`ForceCmd_RxCallback`、`ForceCmd_RegisterRxFilter`、`UdsOta_Init` 注册、`UdsOta_Poll` 检查复位 |

> 附：`stcEntry.u8Format = (uint8_t)CAN_ID_EXT` 的强转——`CAN_ID_EXT` 是 32 位掩码（0x60000000），截断后等效“接受任意帧格式”，原代码即如此工作；强转仅为消除编译告警，行为不变。

---

## 八、测试要点

1. 烧录 boot + app1（或 app2），运行 APP；
2. TBOX 持续发送 `0x18FF5858 / FF|01|02`；
3. RTT 应看到 `Force OTA cmd 0x18FF5858 = 0xXX, resetting to bootloader...` → 复位 → boot 窗口打印 → 按指令处理（进编程模式 / 写槽+回帧+复位 / 拒绝）；
4. 验证回帧 `0x18EF5858` 状态值。