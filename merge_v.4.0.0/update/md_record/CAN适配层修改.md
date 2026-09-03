# CAN 适配层架构说明

## 1. 整体架构

基于 CAN 总线的 UDS 诊断/OTA 升级通信栈采用分层设计，自底向上共 5 层：

```
┌─────────────────────────────────────────────────────────┐
│                   main.c 主循环                          │
│  isotp_ms_update() / uds_ms_update() / isotp_tx_process()│
│  FlashDownload_Task() / CanIf_Poll()                    │
└─────────────────────────────────────────────────────────┘
                            │
┌─────────────────────────────────────────────────────────┐
│  Layer 5: Flash 下载层 (flash_download.c/.h)             │
│  - Flash 擦除/写入/校验/CRC 计算                          │
│  - 下载进度跟踪和状态管理                                  │
│  - FlashDownload_Task() 后台任务                          │
└─────────────────────────────────────────────────────────┘
                            │
┌─────────────────────────────────────────────────────────┐
│  Layer 4: 下载桥接层 (uds_dl_bridge.c + uds_dl_if.h)      │
│  - 将 Flash 操作封装为 uds_dl_if_t 接口                   │
│  - 通过函数指针表实现 UDS 层与 Flash 层的解耦              │
│  - uds_dl_init_fw() 注册固件下载接口实例                   │
└─────────────────────────────────────────────────────────┘
                            │
┌─────────────────────────────────────────────────────────┐
│  Layer 3: UDS 诊断层 (uds_diagnostic.c/.h)               │
│  - ISO 14229 UDS 服务处理                                │
│  - 支持服务: 0x10(会话), 0x22(读DID), 0x2E(写DID),       │
│             0x27(安全访问), 0x31(例程控制),               │
│             0x34/36/37(请求/传输/退出下载)                 │
│  - uds_receive_handler() 处理重组后的 UDS 消息             │
│  - isotp_tx_process() 驱动 UDS 响应发送                   │
└─────────────────────────────────────────────────────────┘
                            │
┌─────────────────────────────────────────────────────────┐
│  Layer 2: ISO-TP 传输层 (isotp_transport.c/.h)           │
│  - ISO 15765-2 多帧传输协议                               │
│  - 单帧(SF)/首帧(FF)/连续帧(CF)/流控帧(FC) 重组与分段      │
│  - isotp_receive_frame() 将 CAN 帧重组为完整 UDS 消息      │
│  - isotp_tx_process()    将 UDS 消息分段为 CAN 帧发送      │
│  - isotp_ms_update()     1ms 定时器（超时管理）            │
└─────────────────────────────────────────────────────────┘
                            │
┌─────────────────────────────────────────────────────────┐
│  Layer 1: CAN 适配层 (Adapter_Can.c/.h)                  │
│  - 封装 can_module，提供统一的上层接口                     │
│  - CanIf_Init()    初始化 CAN 硬件                        │
│  - CanIf_Send()    发送 (直接 + TX队列)                   │
│  - CanIf_Poll()    从硬件缓冲区取帧 → 分发到注册的回调      │
│  - CanIf_RegisterRxFilter()  注册 CAN ID 过滤器+回调       │
│  - Bus-Off 检测与自动恢复                                  │
└─────────────────────────────────────────────────────────┘
                            │
┌─────────────────────────────────────────────────────────┐
│  Layer 0: CAN 硬件驱动层 (can_module.c/.h)                │
│  - HC32F460 CAN 外设 HAL 封装                             │
│  - GPIO 初始化 (PB14=RX, PB15=TX)                        │
│  - 波特率 / 工作模式 / 滤波器 / 中断 配置                  │
│  - can_transmit_std() / can_transmit_ext() 发送           │
│  - can_module_irq_handler() ISR (RX 接收 + TX 完成)       │
│  - can_rx_cache_put() / can_read() 环形缓冲区 FIFO         │
└─────────────────────────────────────────────────────────┘
```

---

## 2. 数据传递流程

### 2.1 接收路径 (RX) — CAN 总线 → Flash

```
CAN 总线
  │
  ▼
[HC32F460 CAN 外设] ──RX中断──▶ can_module_irq_handler()
  │                                │
  │                    CAN_GetRxFrame(CM_CAN, &frame)
  │                                │
  │                    can_rx_cache_put(m_pRxCache, &frame)
  │                                │
  │                    m_stcCanHandle.can_rx (环形缓冲区 FIFO)
  │                                │
  │              ┌─────────────────┘
  │              ▼
  │    CanIf_Poll() ── while (can_read(&m_stcCanHandle.can_rx, ...))
  │              │
  │              ▼
  │    CanIf_DispatchRx(&stcMsg)
  │       │                                      │
  │       ├─ 匹配 Filter 0: 0x18DA03F1 ──▶ ISOTP_RxCallback()
  │       ├─ 匹配 Filter 1: 0x18DAF103 ──▶ ISOTP_RxCallback()
  │       ├─ 匹配 Filter 2: 0x18FF8118 ──▶ ISOTP_RxCallback()
  │       └─ 匹配 Filter 3: 0x18DBFFF0 ──▶ ISOTP_RxCallback()
  │                                                │
  │                          isotp_receive_frame(0, id, data, len, buffer, &out_len)
  │                                                │
  │                          单帧/多帧重组 → ISOTP_OK 时
  │                                                │
  │                          uds_receive_handler(0, id, buffer, out_len)
  │                                                │
  │                          UDS 服务处理 (0x34/36/37/31...)
  │                                                │
  │                          → FlashDownload_OnRequestDownload()
  │                          → FlashDownload_OnTransferData()
  │                          → FlashDownload_OnTransferExit()
  │                                                │
  ▼                                                ▼
[Flash 存储器]  ←──────────────────── FlashDownload_Task()
```

### 2.2 发送路径 (TX) — Flash → CAN 总线

```
UDS 响应消息
  │
  ▼
isotp_tx_process()  ← main 循环中调用
  │
  ▼
CanIf_Send(&stcMsg)
  │
  ├─ 硬件空闲? ──Yes──▶ can_transmit_ext(id, data, len)
  │                           │
  │                           ▼
  │                    CAN_FillTxFrame(CM_CAN, PTB, &tx)
  │                    CAN_StartTx(CM_CAN, PTB)
  │                           │
  │              TX 完成后 ISR ─▶ m_bTxBusy = false ─▶ CanIf_TxCompleteCallback()
  │                           │                              │
  │                           │                 从 TX 队列取下一帧发送
  │                           │
  └─ 硬件忙 ──▶ 入队 m_astcTxQueue[] ─▶ 等待回调取出发送
```

### 2.3 关键：RX 缓冲区的一致性

```
can_module_irq_handler()   写入  →  m_stcCanHandle.can_rx   (ISR 上下文)
CanIf_Poll()               读取  →  m_stcCanHandle.can_rx   (主循环上下文)

         ┌──────────────────────────────────┐
         │    m_stcCanHandle.can_rx          │
         │    (can_rx_cache_t 环形缓冲区)     │
         │    read_idx / write_idx / cnt     │
         │    rx_frame[CAN_RX_BUF_SIZE=5]    │
         └──────────────────────────────────┘
                ↑ 写入(ISR)        ↓ 读取(Poll)

⚠ 两者必须指向同一个缓冲区实例，否则所有接收帧将丢失。
这是本次移植修正的核心问题。
```

---

## 3. CAN 驱动配置要点

### 3.1 硬件引脚

| 信号 | GPIO | 复用功能 | 说明 |
|------|------|----------|------|
| CAN RX | PB14 | FUNC_51 | CAN 接收引脚 |
| CAN TX | PB15 | FUNC_50 | CAN 发送引脚 |

### 3.2 波特率

| 配置项 | 值 | 说明 |
|--------|-----|------|
| 预设波特率 | `CAN_BDR_250K` (250 kbps) | 可选 250K/500K/1M/CUSTOM |
| Prescaler | 4 | 时钟分频 |
| TimeSeg1 | 6 | 传播段 + 相位缓冲段1 |
| TimeSeg2 | 2 | 相位缓冲段2 |
| SJW | 2 | 同步跳转宽度 |

> 系统时钟: HCLK=200MHz, PCLK1=100MHz (CAN 外设挂载于 APB1)

### 3.3 工作模式

| 配置 | 值 |
|------|-----|
| 工作模式 | `CAN_WORK_MD_NORMAL` (正常模式) |
| 自应答 | `CAN_SELF_ACK_ENABLE` (使能) |

### 3.4 TX 发送配置

| 配置项 | 值 | 说明 |
|--------|-----|------|
| PTB 单次发送 | `ENABLE` | 发送失败不自动重试 |
| STB 单次发送 | `DISABLE` | 次发送缓冲区不启用 |
| STB 优先级模式 | `DISABLE` | |
| TX 队列大小 | 32 帧 | 环形队列 |
| TX 发送方式 | 直接发送 + 队列 | 空闲时直接发，忙时入队 |

### 3.5 RX 接收配置

| 配置项 | 值 | 说明 |
|--------|-----|------|
| 接收告警阈值 | 8 | |
| 错误告警阈值 | 10 | 实际阈值 = (10+1)×8 = 88 |
| 接收所有帧 | `DISABLE` | 使用硬件滤波 |
| 溢出模式 | `SAVE_NEW` | 溢出时保存新帧 |
| 硬件滤波器 | 通过全部帧 | ID=0, Mask=0x18FFFFFF, STD_EXT |
| 软件过滤 | CanIf_RxFilterEntry[] | 最多 16 个，按 CAN ID + Mask 精确匹配 |
| RX 缓冲区 | 5 帧 | 环形 FIFO，ISR 写入，Poll 读取 |

### 3.6 中断配置

| 配置项 | 值 |
|--------|-----|
| 中断向量 | INT002_IRQn |
| 中断优先级 | DDL_IRQ_PRIO_07 |
| ISR 回调 | `can_module_irq_handler` |
| 使能的中断源 | RX \| PTB_TX \| RX_OVERRUN \| RX_BUF_FULL \| RX_BUF_WARN \| ERR_INT |

### 3.7 Bus-Off 恢复

- 检测方式: `CanIf_Poll()` 中轮询 `CAN_GetStatusValue()` 的 `CAN_FLAG_BUS_OFF` 标志
- 恢复策略: 检测到 Bus-Off 后等待 **500ms**，然后调用 `CAN_ExitLocalReset()` 恢复
- 不依赖中断处理，在主循环中完成

### 3.8 CAN ID 过滤列表 (ISOTP)

| CAN ID | 用途 |
|--------|------|
| `0x18DA03F1` | 物理寻址请求 (TBOX → 控制器) |
| `0x18DAF103` | 物理寻址响应 (控制器 → TBOX) |
| `0x18FF8118` | OTA 专用 ID |
| `0x18DBFFF0` | 功能寻址请求 (广播) |

---

## 4. 架构统一与修正记录

### 4.1 修正背景

三个固件工程（app1 / app2 / boot）的 CAN 适配层原本架构不统一：

| 工程 | 原有 CAN 硬件层 | 问题 |
|------|---------------|------|
| app1 | `can_module.c` | RX 缓冲区错位（`m_stcRxCache` vs `m_stcCanHandle.can_rx`） |
| boot | `can_module.c` | 同上 |
| app2 | `Can_LLD.c` | 完全不同的驱动接口，与 app1/boot 不一致 |

### 4.2 修正内容 (2026-07-10)

#### app1 / boot：修复 RX 缓冲区错位

**问题**：移植时在 `Adapter_Can.c` 中创建了一个独立的 `m_stcRxCache` 变量，`CanIf_Poll()` 从这个空缓冲区读取帧；而 ISR (`can_module_irq_handler`) 通过全局指针 `m_pRxCache` 将帧写入了 `m_stcCanHandle.can_rx`。两者不是同一个缓冲区，所有接收帧被丢弃。

**修正方案**（仅修改 `Adapter_Can.c`）：
1. 删除多余的 `static can_rx_cache_t m_stcRxCache;` 变量声明
2. 删除 `CanIf_Init()` 中对 `m_stcRxCache` 的 `memset` 无用操作
3. `CanIf_Poll()` 中 `can_read()` 的缓冲区由 `&m_stcRxCache` 改为 `&m_stcCanHandle.can_rx`

#### app2：整体架构迁移

**问题**：app2 使用独立的 `Can_LLD.c/.h` 作为 CAN 硬件驱动层，接口与 app1/boot 的 `can_module.c/.h` 完全不同，代码维护困难。

**修正方案**：
1. 从 app1 复制 `can_module.c`、`can_module.h` 到 app2
2. 从 app1 复制已修复的 `Adapter_Can.c`、`Adapter_Can.h` 到 app2（覆盖旧的 Can_LLD 版本）
3. 更新 `Adapter.h`：`#include "Can_LLD.h"` → `#include "can_module.h"`

### 4.3 修改文件清单

```
merge/
├── app1/projects/ev_hc32f460_lqfp100_v2/Adp/
│   └── Adapter_Can.c              ← 修复 RX 缓冲区 (m_stcRxCache → m_stcCanHandle.can_rx)
│
├── boot/projects/ev_hc32f460_lqfp100_v2/Adp/
│   └── Adapter_Can.c              ← 同上修复
│
├── app2/projects/ev_hc32f460_lqfp100_v2/Adp/
│   ├── can_module.c               ← 新增 (从 app1 复制)
│   ├── can_module.h               ← 新增 (从 app1 复制)
│   ├── Adapter_Can.c              ← 覆盖 (从 app1 复制，含修复)
│   ├── Adapter_Can.h              ← 覆盖 (从 app1 复制)
│   └── Adapter.h                  ← 修改 (#include "Can_LLD.h" → "can_module.h")
│
└── CAN适配层修改.md                ← 本文档
```

### 4.4 移植来源

原始代码位于 `D:\260706_NL\app\can\`，原始架构中 ISR 回调 `app_can_int_callback()` 和接收函数 `app_can_receive()` 在同一个文件 (`app_can.c`) 中，直接操作同一个 `can_handle.can_rx` 缓冲区。移植时拆分为 `can_module` + `Adapter_Can` 两层后引入了多余的中间变量。

### 4.5 统一后的架构

三个工程现在使用完全一致的 CAN 适配层架构：

```
UDS / ISOTP / FlashDownload  (Layer 3-5)
        │
Adapter_Can.c/.h             (Layer 1 — 从 app1 复制)
        │
can_module.c/.h              (Layer 0 — 从 app1 复制)
        │
HC32F460 CAN 外设 (DDL)      (Hardware)
```
