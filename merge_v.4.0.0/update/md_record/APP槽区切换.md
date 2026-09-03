# APP 槽区切换说明

> 主题：双 APP 均坏 → UDS 只刷写 APP1 → 验证 APP2 故障记录应保留；以及 Keil Watch 观察 stcCtx 的正确时机。
> 涉及工程：`D:\ota_ddl3.3_v.3.1\merge_v.2.0.0\boot`（bootloader）。

---

## 1. 背景：双 APP 均坏的切换流程

1. APP1 连续 3 次 SWDT 复位 → boot 记录 APP1 `fault_count=3`（坏块）→ 自动切换跳转目标到 APP2；
2. APP2 连续 3 次 SWDT 复位 → boot 记录 APP2 `fault_count=3`（坏块）→ 双坏 → `SelectTargetSlot()` 返回 `SLOT_NONE` → `RunBootloaderForever()` 进入 UDS 编程模式等待刷写；
3. TBOX 只刷写 APP1 → 烧录完成 → `Rmu_ResetSlotRecord(RMU_SLOT_APP1)` **只擦 APP1 状态扇区（0x16000）**，APP2 状态扇区（0x18000）保持不动；
4. 刷写完成后 `Boot_SetRunSlotToAddr(APP1_START_ADDR)`，复位后跳 APP1 运行。

关键点：**两个 APP 的状态扇区物理独立（0x16000 / 0x18000，8KB/扇区）**，`Rmu_ResetSlotRecord` 只擦传入的那一个，烧 APP1 不会清 APP2。

---

## 2. 现象：Keil Watch 里 stcCtx 显示“APP1、APP2 均可用”

- 烧完 APP1 后，在 bootloader 里打断点看 `stcCtx`，发现 `stcApp1.eState` 和 `stcApp2.eState` 都是 `APP_STATE_AVAILABLE`，看起来两个都可用；
- 但直接看 flash：`*(unsigned long*)0x18008` = `3` → **APP2 的故障计数并没有被清除**。

### 结论

**代码行为是对的（烧 APP1 只清 APP1），之前“两个都可用”是观察时机问题，不是 APP2 被清。**

---

## 3. 原因：stcCtx 是“逐步填充”的局部变量

`Boot_StartupSequence()` 中：

```c
stc_boot_context_t stcCtx;
memset(&stcCtx, 0, sizeof(stcCtx));   // ← 此时全 0：eState=0=AVAILABLE，u32FaultCount=0
...
InitAppInfo(&stcCtx.stcApp1, ...);    // 填 eSlot/u32StartAddr/u32FaultCount，并先把 eState 设成 AVAILABLE
InitAppInfo(&stcCtx.stcApp2, ...);
UpdateAppState(&stcCtx.stcApp1);      // ← 到这里才根据 flash 计数把坏块改成 DISABLED
UpdateAppState(&stcCtx.stcApp2);
SelectTargetSlot(&stcCtx);            // ← 到这里才决定最终跳转槽
UpdateSlotFlagToFlash(&stcCtx);
```

- 断点在 `InitAppInfo` **之前** → 整个 stcCtx 是 memset 的全 0 → APP1/APP2 都显示 `AVAILABLE`、fault=0；
- 断点在 `InitAppInfo` 和 `UpdateAppState` **之间** → eState 还是 `InitAppInfo` 无条件设的 `AVAILABLE`（此时 u32FaultCount 已是真值：APP1=0、APP2=3）。

所以看 stcCtx 必须等到 `UpdateAppState()` 执行完之后。

---

## 4. 正确观察方法

1. **断点打在** `SelectTargetSlot()` 之后、`Bootloader_JumpToApp()` 之前的这一行：
   ```c
   MAIN_D("  CurSlot: %s, Target: %s\r\n", ...);
   ```
2. 此时 Watch 应看到：
   - `stcCtx.stcApp1.eState` = `APP_STATE_AVAILABLE`，`u32FaultCount` = 0（刚刷的新固件）
   - `stcCtx.stcApp2.eState` = `APP_STATE_DISABLED`，`u32FaultCount` = 3（坏块保留）
   - `stcCtx.eTargetSlot` = `SLOT_APP1`（只跳 APP1）

3. 或不看 stcCtx，直接看真值：
   - `*(unsigned long*)0x18008` = 3（APP2 fault_count，坏块保留）
   - `*(unsigned long*)0x16008` = 0 / 0xFFFFFFFF（APP1 已被整槽清零）
   - `*(unsigned long*)0x18004` = 0x524D5532（APP2 magic，记录有效）
   - `g_stcRmuReasonCountApp2.u32Swdt` = 3（APP2 的 SWDT 统计保留）
   - `g_stcRmuReasonCountApp1.u32Swdt` = 0（APP1 已整槽清零）

---

## 5. 相关代码位置

| 功能 | 位置 |
| --- | --- |
| 双坏进 UDS 等待 | `Bootloader_App.c` `SelectTargetSlot()` → `SLOT_NONE` → `RunBootloaderForever()` |
| 烧录完成整槽清零 | `Bootloader_App.c` `Bootloader_UdsMain()` → `Rmu_ResetSlotRecord(eSlot)` |
| 整槽清零实现 | `rmu.c` `Rmu_ResetSlotRecord()`：`EFM_SectorErase(0x16000/0x18000)` + 清 RAM 镜像 |
| stcCtx 填充顺序 | `Bootloader_App.c` `Boot_StartupSequence()`：`memset` → `InitAppInfo` → `UpdateAppState` → `SelectTargetSlot` |
| 从 flash 读故障计数 | `Bootloader_App.c` `InitAppInfo()` → `Rmu_GetFaultCount()` |

---

## 6. 相关提交

- `ca8509d` feat(boot): per-slot RMU RAM mirrors (App1/App2) and full slot reset on OTA flash
- `715645b` fix(boot): force UDS_SHARED_MAGIC on OTA completion so APP can re-send 0x51/0x71 after reset