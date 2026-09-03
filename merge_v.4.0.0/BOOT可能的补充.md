# Bootloader 跳转前中断处理 — 可能的改进点

> 分析时间：2026-07-28
> 芯片：HC32F460 (Cortex-M4)
> 涉及文件：`Bootloader_App.c`、`Bootloader_App.h`

---

## 一、问题总结

Keil Debug 观察 NVIC 时，跳转前某些中断的寄存器状态为 **E=0 / P=1 / A=0**（失能、挂起、未运行）。

当前代码只操作了 NVIC 层，没有触及三层中断链路的上面两层。

---

## 二、根因分析：三层中断链路

HC32F460 的中断链路不是标准的外设→NVIC，而是三级：

```
[外设中断标志寄存器]  →  [INTC: SELx 映射 + IER 使能]  →  [NVIC: ISER/ICPR]  →  CPU
    ↑ 第1层：没清              ↑ 第2层：没关                  ↑ 第3层：关了但清不掉
```

| 层级 | 寄存器 | 当前代码 |
|------|--------|:-------:|
| ① 外设中断标志 | 各外设自己的 SR / IR 寄存器 | ❌ 没碰 |
| ② INTC IER（中断使能） | `CM_INTC->IER` (32bit, 每 bit 对应一个中断通道) | ❌ 没碰 |
| ③ NVIC ISER / ICPR | `0xE000E100` 区域 | ✅ 做了 |

**Pending 清不掉的原因**：外设中断标志还在 → INTC IER 还是使能的 → 外设通过 INTC 持续向 NVIC 发 pending → 你清了 ICPR，它马上又被置起。

---

## 三、Pending 未清除的潜在危害

| 场景 | 后果 |
|------|------|
| APP `__enable_irq()` 在外设初始化**之前** | ISR 访问未初始化的外设寄存器 → **HardFault** |
| APP 未初始化 CAN，但 bootloader 的 CAN 中断 pending 还在 | `IRQxxx_Handler()` → `m_apfnIrqHandler[xxx]()` → 函数表在 bootloader RAM 区，APP 重置 .bss 后变成野指针 → **HardFault** |
| PendSV 未清除 | 如果用了 RTOS 或手动 PendSV，APP 启动后可能立即进入上下文切换 |

---

## 四、修改方案概述

按照中断链路的顺序 **自上而下关闭**：

```
① SysTick 停 + PendSV 清
② 禁用 INTC 层 IER（切外设→NVIC 的通路）
③ __disable_irq()
④ 禁用 NVIC 全部 IRQ + 清 NVIC 全部 pending
⑤ 设 MSP、VTOR → 跳转
```

> **注意**：不关闭 CAN 外设本身。INTC IER=0 足以阻断 CAN 中断向 NVIC 传播，
> 且 CAN 在 UDS 固件下载路径中仍需正常工作。

---

## 五、具体修改内容

### 5.1 涉及文件

| 文件 | 改动类型 |
|------|---------|
| `Bootloader_App.c` | 修改 `DisableAllNVICInterrupts()`、`Bootloader_JumpToApp()` |
| `Bootloader_App.h` | 新增函数声明 |

---

### 5.2 修改 1：新增 `Bootloader_PeripheralShutdown()` + 重写 `DisableAllNVICInterrupts()`

**文件**：`Bootloader_App.c`（替换原 `DisableAllNVICInterrupts` 函数，约第 59-70 行）

#### 原代码：

```c
void DisableAllNVICInterrupts(void)
{
    uint32_t i;
    for(i = 0; i < 128; i++)
    {
        NVIC_DisableIRQ((IRQn_Type)i);
    }
    for(i = 0; i < 4; i++)
    {
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }
}
```

#### 修改为：

```c
/*
 * 跳转 APP 前的外设/系统资源关闭。
 * 目标：清理 SysTick、PendSV 等系统级中断源，
 *       关闭 HC32F460 的 INTC 层使能，
 *       使后续 NVIC 层的 pending 能够被真正清零。
 *
 * 注意：CAN 外设不在此处关闭。
 *       如果走 UDS 路径（Bootloader_UdsMain），CAN 仍需要工作；
 *       如果走直接启动路径（Boot_StartupSequence → JumpToApp），
 *       CAN 中断 pending 由后续 NVIC_DisableAndClearAll() 配合
 *       INTC 层关闭来处理。
 */
static void Bootloader_PeripheralShutdown(void)
{
    /*
     * 1. 停止 SysTick
     *
     *    确保 SysTick 完全归零，避免 APP 启动后在配置自己 SysTick 之前
     *    收到意外的 SysTick 中断。
     */
    SysTick->CTRL  = 0;
    SysTick->LOAD  = 0;
    SysTick->VAL   = 0;
    SCB->ICSR |= SCB_ICSR_PENDSTCLR_Msk;

    /*
     * 2. 清除 PendSV pending
     *
     *    如果 bootloader 路径中从未触发 PendSV，此行无害；
     *    如果使用过 RTOS 或手动 PendSV，此行是必要的保险。
     */
    SCB->ICSR |= SCB_ICSR_PENDSVCLR_Msk;

    /*
     * 3. 关闭 HC32F460 INTC 层中断使能
     *
     *    关闭 INTC 层之后，即使外设中断标志还挂着，
     *    也不会再向 NVIC 发送新的 pending 请求。
     *
     *    注意：不在这里关闭 CAN 外设本身（CAN 可能仍需运行）。
     *    CAN 外设的中断标志会因 INTC IER=0 被阻断在 INTC 层，
     *    不会再传播到 NVIC。
     *
     *    CM_INTC->IER : 32 位，每 bit 对应一个 INTC 中断通道
     *    CM_INTC->EVTER: 32 位，每 bit 对应一个事件输出通道
     */
    CM_INTC->IER   = 0x00000000UL;
    CM_INTC->EVTER = 0x00000000UL;

    __DSB();
    __ISB();
}

/*
 * 关闭全部 NVIC 中断使能 + 清除全部 NVIC pending。
 * 必须在 Bootloader_PeripheralShutdown() 之后调用，
 * 因为外围关闭后 pending 才能真正被清零。
 */
static void NVIC_DisableAndClearAll(void)
{
    uint8_t i;

    for (i = 0; i < 8; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFF;  // 禁用 IRQ 0~255
        NVIC->ICPR[i] = 0xFFFFFFFF;  // 清除 IRQ 0~255 pending
    }

    __DSB();
    __ISB();
}
```

---

### 5.3 修改 2：重写 `Bootloader_JumpToApp()`

**文件**：`Bootloader_App.c`（替换原函数，约第 237-280 行）

#### 原代码：

```c
void Bootloader_JumpToApp(uint32_t u32AppAddr)
{
    uint32_t app_start_address;
    uint32_t app_sp = *(uint32_t *)u32AppAddr;
    app_start_address = *(uint32_t *)(u32AppAddr + 4);

    MAIN_D("=== Bootloader Jump To APP ===\r\n");
    MAIN_D("  APP Start Addr: 0x%08X\r\n", u32AppAddr);
    MAIN_D("  APP SP:         0x%08X\r\n", app_sp);
    MAIN_D("  APP Reset:      0x%08X\r\n", app_start_address);

    if (app_start_address == 0xFFFFFFFF) {
        MAIN_D("  ERROR: APP Reset vector is 0xFFFFFFFF, jump aborted!\r\n");
        return;
    }

    // 1. 清除全部 RAM（bootloader 和 APP 共用）
    // ClearAllRAM(); // APP startup handles RAM init
    
    // 2. 停止 SysTick
    SysTick->CTRL = 0;
    SCB->ICSR |= SCB_ICSR_PENDSTCLR_Msk; 

    // 3. 关闭全局中断
    __disable_irq();
    DisableAllNVICInterrupts();
    
    // 4. 清中断使能和挂起寄存器
    for (uint8_t i = 0; i < 8; i++) 
    {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    // 5. 设置栈指针和向量表
    __set_MSP(*(uint32_t *)u32AppAddr);
    SCB->VTOR = ((uint32_t)u32AppAddr & SCB_VTOR_TBLOFF_Msk);
    
    __DSB();
    __ISB();

    // 6. 跳转到 APP
    (*((void(*)(void))app_start_address))();
}
```

#### 修改为：

```c
void Bootloader_JumpToApp(uint32_t u32AppAddr)
{
    uint32_t app_start_address;
    uint32_t app_sp;

    app_sp             = *(uint32_t *)u32AppAddr;
    app_start_address  = *(uint32_t *)(u32AppAddr + 4);

    MAIN_D("=== Bootloader Jump To APP ===\r\n");
    MAIN_D("  APP Start Addr: 0x%08X\r\n", u32AppAddr);
    MAIN_D("  APP SP:         0x%08X\r\n", app_sp);
    MAIN_D("  APP Reset:      0x%08X\r\n", app_start_address);

    if (app_start_address == 0xFFFFFFFF) {
        MAIN_D("  ERROR: APP Reset vector is 0xFFFFFFFF, jump aborted!\r\n");
        return;
    }

    /*
     * 步骤 1: 停 SysTick、清 PendSV、关闭 INTC 层
     *         （必须在关全局中断之前做，确保系统中断源先停下来）
     */
    Bootloader_PeripheralShutdown();

    /*
     * 步骤 2: 关全局中断
     */
    __disable_irq();

    /*
     * 步骤 3: 禁用 NVIC 全部 IRQ + 清除全部 NVIC pending
     *         此时外设和 INTC 都已关闭，pending 可以真正清零且不会反弹
     */
    NVIC_DisableAndClearAll();

    /*
     * 步骤 4: 设置 MSP 和向量表偏移
     */
    __set_MSP(app_sp);
    SCB->VTOR = ((uint32_t)u32AppAddr & SCB_VTOR_TBLOFF_Msk);

    __DSB();
    __ISB();

    /*
     * 步骤 5: 跳转到 APP 复位向量
     */
    MAIN_D("  Jumping to APP...\r\n");
    (*((void(*)(void))app_start_address))();

    /* 不应到达这里 */
    while (1) { __nop(); }
}
```

---

### 5.4 修改 3：`Bootloader_App.h` 新增声明

**文件**：`Bootloader_App.h`

在 `void ClearAllRAM(void);` 声明附近（约第 174 行），**不需要新增对外公开的声明**（`Bootloader_PeripheralShutdown()` 和 `NVIC_DisableAndClearAll()` 都是 `static` 的，仅在 `.c` 内可见）。

原有头文件中删除不再需要的声明：

#### 原代码（约第 158 行）：

```c
void DisableAllNVICInterrupts(void);
```

#### 改为：

```c
/* 已移除 DisableAllNVICInterrupts() 声明（不再作为独立公开接口） */
```

> **说明**：`DisableAllNVICInterrupts()` 和新增的 `Bootloader_PeripheralShutdown()`、`NVIC_DisableAndClearAll()` 都是静态函数，不需要对外暴露。如果你有其他 `.c` 文件调用了 `DisableAllNVICInterrupts()`，请一并处理。

---

## 六、关键设计说明

### 6.1 为什么 INTC 关闭必须在 `__disable_irq()` 之前？

先关 INTC 层 IER（切断外设→NVIC 通路），再清 NVIC pending：

- INTC IER=0 → 外设中断标志无法再向 NVIC 产生新的 pending
- 随后清 NVIC ICPR → 已存在的 pending 被清零，且不会反弹
- 最后 `__disable_irq()` → 跳转

如果反过来（先 `__disable_irq()` 再关 INTC）：
- CPU 确实不响应中断了，但外设仍在向 NVIC 发 pending
- 先清了 NVIC pending → INTC 还没关 → 外设马上又置起 pending
- P=1 的问题就还在

### 6.2 CAN 不在此关闭的原因

CAN 外设不在 `Bootloader_PeripheralShutdown()` 中关闭，因为：
- **UDS 路径**（`Bootloader_UdsMain`）：CAN 仍需工作，关闭 CAN 会断掉 UDS 通信
- **直接启动路径**（`JumpToApp`）：`CM_INTC->IER = 0` 已阻断 CAN → NVIC 的 pending 传递

INTC 层关闭后，即使 CAN 外设中断标志还挂着，也不会到达 NVIC。APP 启动后重新初始化 CAN 时会自行清理。

### 6.3 INTC 层的处理

`CM_INTC->IER` 直接写 0 而不是逐 bit 清零，原因是：
- 这是普通读写寄存器（非 write-1-clear），写 0 就是关闭所有通道
- 省去遍历 128 个 IRQn 去调 `INTC_IntCmd()` 的开销
- 不依赖 `DDL_ASSERT` 是否编译开启

### 6.4 `NVIC_DisableAndClearAll` 的范围

`ICU_ICER[0..7]` 和 `ICPR[0..7]` 覆盖 IRQ 0~255，超出了 HC32F460 实际使用的 144 个 IRQ（INT000~INT143）。多写无害，确保覆盖完整。

---

## 七、不需要修改的部分

| 文件/函数 | 原因 |
|-----------|------|
| `main.c` / `main()` | 不涉及中断处理细节，只是调用 `Boot_StartupSequence()` |
| `Boot_StartupSequence()` | 其内部的 `Bootloader_JumpToApp()` 已被修改覆盖 |
| `ClearAllRAM()` | 当前已被注释掉，且 APP 自己处理 RAM 初始化 |
| `InitSharedCtrl()` | 功能独立，不受中断改动影响 |
| 所有 Flash/WDT/UDS 共用函数 | 功能独立，不受中断改动影响 |

---

## 八、验证方法

修改后，在 Keil Debug 中验证：

1. 在 `NVIC_DisableAndClearAll()` 之后（即 `__set_MSP()` 之前）设断点
2. 打开 NVIC 窗口，逐一检查每个 IRQ 的 E/P/A 状态
3. 期望结果：**所有 IRQ 的 E=0、P=0、A=0**
4. 如果仍然有 P=1，说明 `Bootloader_PeripheralShutdown()` 中该 IRQ 对应的外设没有彻底关闭，需要定位是哪个外设并补充关闭代码
