# UDS OTA 应用封装层 — 修改记录

> 日期: 2026-07-12
> 范围: boot / app1 工程 (app2 待推广)

## 1. 背景

boot/app1/app2 三个 main.c 中各有约 50 行 UDS 相关代码完全重复：

| 重复内容 | 行数 | 说明 |
|----------|------|------|
| `#include` (4 个 UDS 头文件) | 4 | Adapter_Can.h / isotp_transport.h / uds_diagnostic.h / flash_download.h |
| `extern void uds_dl_init_fw(void)` | 1 | 前向声明（uds_dl_bridge.c 没有 .h 文件） |
| `volatile uint32_t g_delayed_reset_ms` | 2 | 延迟复位倒计时 |
| `s_uds_rx_buffer[4100]` | 1 | ISOTP 重组缓冲区 |
| `ISOTP_RxCallback()` | 8 | CAN RX → ISOTP → UDS 分发 |
| `ISOTP_RegisterRxFilters()` | 22 | 注册 4 个 CAN ID 过滤器 |
| 初始化序列（5 行） | 5 | isotp_init → RegisterRxFilters → FlashDownload_Init → uds_dl_init_fw → uds_init |
| 主循环轮询（~12 行） | 12 | 1ms 门控 + 延迟复位倒计时 + isotp_ms_update + uds_ms_update + isotp_tx_process + FlashDownload_Task + CanIf_Poll |

此外 `Bootloader_UdsMain()` 内部的 while(1) 循环中也有几乎相同的轮询代码（延迟复位 + ISOTP/UDS 超时 + FlashDownload_Task + CanIf_Poll）。

## 2. 设计思路

将这些重复代码封装为 `uds_ota.h` + `uds_ota.c`，放在 UDS 目录下，三个工程共享。

### 2.1 对外 API

```c
/* 初始化 UDS/CAN/ISOTP 栈（所有固件通用） */
void UdsOta_Init(void);

/* 主循环轮询：1ms 门控 + 延迟复位 + ISOTP/UDS 超时 + FlashDownload + CAN 接收 */
void UdsOta_Poll(void);

/* APP 启动时检查并补发挂起的 UDS 响应 (Phase 3: 51 01) */
void UdsOta_App_CheckPendingAck(void);

/* Bootloader 进入 UDS 编程模式 (Phase 2: 31 ACK + 下载循环)，不返回 */
void UdsOta_Bootloader_Enter(void);
```

### 2.2 调用关系与 Phase 对应

```
                    boot                          app1/app2
                    ────                          ─────────
main():
  Hardware_Init()              Hardware_Init()
  UdsOta_Init()                UdsOta_App_CheckPendingAck()  ← Phase 3
  Boot_StartupSequence()       UdsOta_Init()
    │                          while(1):
    ├─ 读共享Flash                   UdsOta_Poll()
    │   phase==1? (ENTER_BOOTLOADER)
    │   YES → UdsOta_Bootloader_Enter()  ← Phase 2
    │         └─ Bootloader_UdsMain()
    │            ├─ 补发 31 ACK
    │            ├─ Init FlashDownload(APP2)
    │            └─ while(1): UdsOta_Poll() + 检测完成
    │                        (不返回)
    │
    └─ phase!=1 → 正常启动 → JumpToApp

  while(1):
    UdsOta_Poll()               ← bootloader 非 UDS 模式的后备
```

### 2.3 Phase 1 去哪了？

Phase 1（APP 收到 0x31 → 写共享 Flash → 延迟复位）完全在 UDS 层内部完成：
- `uds_handle_routine_control()` 检测到 `SCB->VTOR` 是 APP → 调用 `UdsShared_SetPhase(ENTER_BOOTLOADER)` → 设置 `g_delayed_reset_ms` → `UdsOta_Poll()` 中倒计时结束后复位
- main.c 不需要额外代码

## 3. 修改文件清单

### 3.1 新建

| 文件 | 路径 | 说明 |
|------|------|------|
| `uds_ota.h` | `boot/.../UDS/` + `app1/.../UDS/` | 4 个 API 声明 + `extern g_delayed_reset_ms` |
| `uds_ota.c` | `boot/.../UDS/` + `app1/.../UDS/` | 内部持有 `s_uds_rx_buffer`、`ISOTP_RxCallback`、`ISOTP_RegisterRxFilters`、`g_delayed_reset_ms`、4 个公开函数实现 |

> `uds_ota.h` / `uds_ota.c` 是两个工程完全共享的，内容一致。

### 3.2 修改

| 工程 | 文件 | 变化 |
|------|------|------|
| boot | `template/source/main.c` | 删除 ~50 行 UDS 样板：4 个 include → `uds_ota.h`；删除 `extern uds_dl_init_fw`、`g_delayed_reset_ms`、`s_uds_rx_buffer`、`ISOTP_RxCallback`、`ISOTP_RegisterRxFilters`；init → `UdsOta_Init()`；主循环 → `UdsOta_Poll()` |
| boot | `Bootloader_App/Bootloader_App.c` | 新增 `#include "uds_ota.h"`；`Bootloader_UdsMain()` 删除 `last_ms_tick` 和内联 poll，替换为 `UdsOta_Poll()` |
| app1 | `template/source/main.c` | 同 boot；额外将 `App_CheckPendingUdsAck()` 替换为 `UdsOta_App_CheckPendingAck()` |
| app1 | `Bootloader_App/Bootloader_App.c` | 同 boot |

### 3.3 main.c 精简前后对比

```
精简前 (~50 行 UDS 相关)               精简后 (~3-5 行 UDS 相关)
═══════════════════════════════        ═══════════════════════════════
#include "Adapter_Can.h"               #include "uds_ota.h"
#include "isotp_transport.h"
#include "uds_diagnostic.h"
#include "flash_download.h"

extern void uds_dl_init_fw(void);      (移入 uds_ota.c)

volatile uint32_t g_delayed_reset_ms;  (移入 uds_ota.c)

static uint8_t s_uds_rx_buffer[4100];  (移入 uds_ota.c)
static void ISOTP_RxCallback(...){}    (移入 uds_ota.c)
static void ISOTP_RegisterRxFilters()  (移入 uds_ota.c)
{...}

// main() 中:                           // main() 中:
isotp_init(0);                         UdsOta_Init();
ISOTP_RegisterRxFilters();
FlashDownload_Init(NULL);
uds_dl_init_fw();
uds_init();

// while(1) 中:                         // while(1) 中:
1ms 门控 + 延迟复位 +                   UdsOta_Poll();
isotp_ms_update/uds_ms_update +
isotp_tx_process +
FlashDownload_Task + CanIf_Poll
```

### 3.4 boot 与 app1 的 main() 差异

```
boot main():                         app1 main():
  Hardware_Init()                      SCB->VTOR = APP1_START_ADDR
  UdsOta_Init()                        __enable_irq()
  Boot_StartupSequence()               Hardware_Init()
  while(1): UdsOta_Poll()              UdsOta_App_CheckPendingAck()  ← Phase 3
                                       UdsOta_Init()
                                       while(1): UdsOta_Poll()
```

仅启动流程不同，UDS 栈部分完全统一为 3 个 API 调用。

## 4. UdsOta_Poll 的双重调用

`UdsOta_Poll()` 在两处被调用：

1. **main() 的 while(1)** — bootloader 正常启动但未进入 UDS 模式时（phase ≠ 1），作为后备轮询
2. **Bootloader_UdsMain() 的 while(1)** — 进入 Phase 2 下载模式后

两者的区别仅在于 Phase 2 额外有 WDT 喂狗和 `FW_UPDATE_COMPLETE` 检测。`UdsOta_Poll()` 内部使用静态变量 `s_last_ms_tick` 管理 1ms 门控，两处调用互不干扰。

## 5. 推广情况（app2 已完成，2026-08-02）

- **app2 已按 app1 同样方式完成 UDS 封装层对齐**（`uds_ota.h/c`、main.c 精简、Bootloader_App.c 同模式修改）；
- 2026-08-02 起 boot/app1/app2 三个工程的分区与下载上限统一调整为 **80KB**（`FW_APP_MAX_SIZE=0x14000`、`max_firmware_size=80KB`、`TBOX_ADDR_END=0x08018000`）；
- app2 不需要 `UdsOta_Bootloader_Enter()`（APP 不会进入 UDS 编程模式），但保留该函数也不会造成问题（不调用就不会链接）；
- 四驱控制盒工程（D:\260706_NL）已按 app1 模式移植 OTA，并直接复用本工程的 boot，**2026-08-02 实测 Phase 1→2→3 全链路通过**（详见四驱 `app/can/uds/uds移植.md`）。

---

## 6. 修改记录（2026-08-02）

| 修改项 | 涉及文件 | 说明 |
|--------|---------|------|
| 分区统一 80KB | `Bootloader_App.c`, `flash_download.h/c` (boot/app1/app2) | `max_firmware_size` 48→80KB、`user_end_addr` +0xC000→+0x14000、`FW_APP_MAX_SIZE` 0xC000→0x14000、`TBOX_ADDR_END` 0x08010000→0x08018000 |
| PB6 相位指示删除 | `uds_diagnostic.c`, `Bootloader_App.c` (boot/app1/app2) | Phase 1/2/3 闪烁及 `ShowBootStatus()` 全部移除（四驱工程 PB6 为电机引脚，闪烁会导致过流故障） |
| app1 31 ACK 发送方式统一 | `Bootloader_App.c` (app1) | raw CAN 改回 `isotp_send_message`，与 boot 一致 |
| 四驱工程移植实测通过 | 四驱工程 (D:\260706_NL) | 复用本工程 boot，Phase 1→2→3 全链路验证通过 |
