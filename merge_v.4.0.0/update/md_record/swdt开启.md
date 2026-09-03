# SWDT 开启说明

> 主题：为什么之前 SWDT 失效、改了什么、为什么现在好了。
> 涉及工程：`D:\ota_ddl3.3_v.3.1\merge_v.2.0.0\boot`（bootloader）。

---

## 1. 现象

### 1.1 之前（SWDT 失效）
- 通过 CAN 发送 `0x18FF5555 / 01 00 00 00 00 00 00 00` 停止喂狗。
- APP1 的 RTT 正常打印：

```
[MAIN] [TEST] SWDT feed DISABLED by 0x18FF5555
```

- 但等待远超 7s 甚至更久，程序依然不复位回 bootloader；
- 在 boot 的 `main()` 打断点也没有触发，确认 SWDT 完全没有产生复位。

### 1.2 现在（SWDT 正常）
- 同样发停喂狗指令后，约 6.5s 芯片被 SWDT 复位，重新进入 bootloader；
- boot 上电第一件事（RMU 处理）打印出 SWDT 复位原因，并在 flash 里累计故障计数：

```
[RMU] raw=0x0040 cause=SWDT fault=1 ...
[RMU] cnt : POR=0 PIN=0 BOR=0 PVD1=0 PVD2=0 WDT=0 SWDT=1 PWRDN=0 SW=20 MPU=0 ...
```

---

## 2. 为什么之前 SWDT 失效

### 2.1 SWDT 是“纯硬件启动”的看门狗
HC32F460 有**两个看门狗**，行为完全不同：

| 项目 | WDT（通用看门狗） | SWDT（专用看门狗） |
| --- | --- | --- |
| 时钟源 | PCLK4 | 内部专用低速 RC（SWDTLRC，10kHz） |
| 启动方式 | 硬件启动 / 软件启动 | **只支持硬件启动** |
| 软件使能 | 有控制寄存器 CR，可 `WDT_Init()` | 无控制寄存器，驱动只有 Feed/Get/Clear |
| 启动/停止决定权 | 运行时可软件控制 | **完全由 ICG 预装载决定，运行时只读** |

- `hc32_ll_swdt.c/.h` 里只有：`SWDT_FeedDog()`、`SWDT_GetStatus()`、`SWDT_ClearStatus()`、`SWDT_GetCountValue()`，**没有 Init/Enable 接口**（对比 WDT 有 `WDT_Init()`）；
- `CM_SWDT` 寄存器只有 `SR`（状态）和 `RR`（刷新），没有任何使能/控制寄存器；
- 官方手册明确：**SWDT 不支持软件启动**。`SWDT_FeedDog()` 注释里那句 “software startup mode” 是从 WDT 驱动复制来的，对 SWDT 无效。

### 2.2 根因：ICG0.SWDTAUTS=1，SWDT 复位后停止且永远不启动
- SWDT 的启动方式由 ICG0 寄存器 **bit0（SWDTAUTS）** 决定：

```
SWDTAUTS = 0：复位后 SWDT 自动启动（硬件启动）
SWDTAUTS = 1：复位后 SWDT 停止
```

- **库默认值** `ICG_REG_CFG0_CONST = 0xFFDFFFBF` → bit0 = 1 → 每次复位后 SWDT 处于停止状态；
- 之前测试用值 `0xFFDFFF0F` → 只是把 SWDTCKS 从 `DIV2048` 改为 `DIV1`（把超时从约 3.7h 缩到 6.5s 级别），**bit0 仍然是 1**，SWDT 依旧不启动；
- 结果就是：计数器根本没走，喂不喂狗都无所谓，**永远不会复位**。这就是“停止喂狗 7s 也不复位”的真正原因。

### 2.3 另一个坑：改 ICG 必须断电重新上电
- ICG0 是只读寄存器，初值由硬件在**上电（POR）时自动从 flash 0x400 的 ICG 区加载**，早于用户程序运行；
- Keil 的 Reset / 调试复位**不会重新加载 ICG**；
- 所以即使烧写了新 hex，如果不真正断电再上电，ICG 仍可能是旧值。

---

## 3. 改了什么

### 3.1 唯一代码改动
文件：`boot\projects\ev_hc32f460_lqfp100_v2\template\source\hc32f4xx_conf.h`

```c
/* 覆盖 hc32_ll_icg.h 中的默认 ICG_REG_CFG0_CONST：
 * SWDTAUTS=0：复位后 SWDT 自动启动（硬件启动）
 * SWDTCKS=DIV1 x 65536，SWDTLRC 10kHz => 约 6.5s */
#define ICG_REG_CFG0_CONST    (0xFFDFFF0EUL)
```

- 覆盖机制：`hc32_ll_icg.h` 里 `ICG_REG_CFG0_CONST` 是 `#ifndef`，且该头文件先包含 `hc32f4xx_conf.h`，所以在 conf 里先定义即可生效（`hc32_ll_icg.c` 的 `u32ICGValue[]` 会被链接到 flash 0x400）。
- 与上一版 `0xFFDFFF0F` 相比只差 1 个 bit：**bit0 由 1 改为 0**。

### 3.2 0xFFDFFF0E 位域解码

| 位 | 字段 | 值 | 含义 |
| --- | --- | --- | --- |
| bit0 | SWDTAUTS | 0 | **复位后 SWDT 自动启动（硬件启动）← 关键修复** |
| bit1 | SWDTITS | 1 | 下溢/刷新错误触发复位（不是中断） |
| bits[3:2] | SWDTPERI | 11 | 计数周期 65536 |
| bits[7:4] | SWDTCKS | 0 | 时钟 DIV1 |
| bits[11:8] | SWDTWDPT | 0xF | 刷新窗口 0~100%（任意时刻喂狗都合法） |
| bit12 | SWDTSLPOFF | 1 | sleep/stop 模式下停止计数（正常运行不受影响） |
| 其它位 | WDT 相关 | 保持库默认 | 本工程 `LL_WDT_ENABLE=DDL_OFF`，WDT 未使用 |

### 3.3 超时计算

```
SWDT 时钟源：SWDTLRC ≈ 10kHz（system_hc32f460.h 中 SWDTLRC_VALUE = 10000）
超时 = 计数周期 / 时钟 = 65536 / 10000 ≈ 6.55s
```

---

## 4. 为什么现在好了

1. `0xFFDFFF0E` 使 `SWDTAUTS=0`：**每次复位后硬件自动启动 SWDT**，16 位递减计数器从 65535 开始递减；
2. 刷新窗口为 0~100%，任何时刻写 `SWDT->RR` 刷新键都会重装计数器，正常喂狗不会误触发；
3. 正常运行时的喂狗链路（原本就存在）：
   - boot：Timer0 中断每 500ms 喂一次 + UDS 主循环每 500ms 喂一次；
   - app1/app2：主循环每圈喂，受 `g_swdt_feed_disable` 控制（测试指令专用）；
   - app 工程 `LL_ICG_ENABLE=DDL_OFF`，不会覆盖 boot 烧在 0x400 的 ICG；
4. 测试时：`0x18FF5555 / data[0]=0x01` → APP 置 `g_swdt_feed_disable=1` 停止喂狗 → 约 6.5s 后计数下溢 → SWDT 复位（RMU RSTF0.SWDRF=0x40）→ boot 上电第一件事读 RMU → 属于故障类 → 当前槽 `fault_count + 1` 写回 flash → RTT 打印原因与计数。

---

## 5. 验证记录

- 编译产物 `boot.hex` 解析 `@0x400`：
  - 改前：`0xFFDFFF0F`（SWDTAUTS=1，SWDT 不启动）
  - 改后：`0xFFDFFF0E`（SWDTAUTS=0，SWDT 自动启动）
- gcc 预处理验证：`ICG_REG_CFG0_CONST` 经真实包含链解析为 `0xFFDFFF0E`；
- 实测 RTT（SWDT 复位后 boot 打印）：

```
[RMU] raw=0x0040 cause=SWDT fault=1 ...
[RMU] cnt : POR=0 PIN=0 BOR=0 PVD1=0 PVD2=0 WDT=0 SWDT=1 PWRDN=0 SW=20 MPU=0 ...
```

- 相关提交（本地 master，未推送）：
  - `639ddee` test(boot): shorten SWDT timeout to ~6.5s via ICG override（0xFFDFFF0F，当时 AUTS 未改，SWDT 仍不工作）
  - `f133f79` config(boot): keep 6.5s SWDT timeout as permanent setting
  - `a0cd58d` fix(boot): enable SWDT auto-start (SWDTAUTS=0) with 6.5s timeout（0xFFDFFF0E，本次真正修复）

---

## 6. 注意事项 / 验证方法

1. **改 ICG 后必须重新编译烧录 `boot.hex`，并断电几秒再上电**（Keil Reset 不重新加载 ICG）；
2. 上电后可用 Keil Watch 确认：
   - `*(unsigned long*)0x400` 应等于 `0xFFDFFF0E`；
   - `*(unsigned long*)0x40049404 & 0xFFFF` 在递减 → 说明 SWDT 正在计数；
3. 不要在断点上停着等复位（会停住主循环/喂狗节奏，用 RTT 全速运行观察）；
4. 故障计数逻辑：SWDT/WDT/MPU 复位属于故障类，故障计数 <3 才累加；累计达到阈值后按 RMU 设计切换/停留 boot（详见 `2026-08-09-boot-rmu-record-design.md`）；
5. 当前 6.5s 超时是本工程固定设置；若量产需要恢复默认超时（CLK/2048 × 65536 ≈ 3.7h）或调整，只需修改 `ICG_REG_CFG0_CONST` 并重新烧录 + 断电上电。