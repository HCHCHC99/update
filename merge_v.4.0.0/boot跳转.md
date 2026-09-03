# Boot 跳转机制与 OTA 模式说明

> 日期: 2026-08-02
> 适用范围: OTA boot（D:\ota_ddl3.3_v.3.1\merge_v.1.0.0\boot）
> 相关: [收发流程.md](收发流程.md) ｜ [flash分区.md](flash分区.md) ｜ [bootOTA优化.md](bootOTA优化.md)

---

## 一、boot 上电跳转流程

boot 每次上电在 `Boot_StartupSequence()` 中完成“读槽 → 选槽 → 跳转”：

```
Boot_StartupSequence()
  → UDS 共享区检查（phase==1 → 进编程模式；否则正常启动）
  → GetCurrentSlot()          读 0x7C000 (APP_RUN_SLOT)
       0x5A5A5A5A → SLOT_APP1（0x1A000）
       0xA5A5A5A5 → SLOT_APP2（0x4C000）
       其他       → SLOT_NONE（默认 APP1 并回写）
  → InitAppInfo / UpdateAppState     读 WDT 故障计数，≥3 判 DISABLED
  → HandleWatchdogReset / ValidateSlotFlag
  → SelectTargetSlot()        当前槽健康则选当前槽，否则切健康槽，双故障 → SLOT_NONE
  → UpdateSlotFlagToFlash()   需要时回写 0x7C000
  → Bootloader_JumpToApp(目标地址)
```

**关键点**：跳转目标完全由 **`0x7C000` 的 magic** 决定（健康前提下）。因此“设置跳转”= 写 `0x7C000`。

---

## 二、OTA 模式管理（三种模式）

`Bootloader_App/Bootloader_App.h` 中定义：

```c
/* ===== OTA 模式选择 =====
 * 调试模式        (BOOT_OTA_MODE_DEBUG=1, BOOT_OTA_MODE_PLUS=0):
 *                  烧写窗口: 仅 APP2（TBOX 0x08004000~0x08018000 → 0x4C000）
 *                  OTA 完成后始终跳转 APP1（便于反复调试）
 * 调试模式Plus版  (BOOT_OTA_MODE_DEBUG=1, BOOT_OTA_MODE_PLUS=1):
 *                  烧写窗口: APP1 + APP2 双窗口（新增 TBOX 0x08018000~0x0802C000 → 0x1A000）
 *                  OTA 完成后始终跳转 APP1
 * 正式模式        (BOOT_OTA_MODE_DEBUG=0, BOOT_OTA_MODE_PLUS=任意):
 *                  烧写窗口: APP1 + APP2 双窗口
 *                  OTA 完成后跳转实际烧录的槽位（烧到哪里就跳到哪里）
 */
#ifndef BOOT_OTA_MODE_DEBUG
#define BOOT_OTA_MODE_DEBUG                 1U
#endif
#ifndef BOOT_OTA_MODE_PLUS
#define BOOT_OTA_MODE_PLUS                  0U
#endif

/* 双烧写窗口使能: 调试Plus 或 正式模式 */
#if ((BOOT_OTA_MODE_DEBUG == 0U) || (BOOT_OTA_MODE_PLUS == 1U))
#define BOOT_OTA_DUAL_WINDOW_EN             1U
#else
#define BOOT_OTA_DUAL_WINDOW_EN             0U
#endif

#if (BOOT_OTA_MODE_DEBUG == 1U)
  #define UDS_TARGET_FLASH_ADDR            APP2_START_ADDR
  #define UDS_POST_FLASH_BOOT_ADDR         APP1_START_ADDR   /* 调试/调试Plus: 烧完跳APP1 */
#else
  #define UDS_TARGET_FLASH_ADDR            APP2_START_ADDR
  #define UDS_POST_FLASH_BOOT_ADDR         APP2_START_ADDR   /* 正式: 烧哪里跳哪里 */
#endif
```

| 模式 | 配置 | 烧写窗口 | OTA 完成后跳转 |
|---|---|---|---|
| 调试模式（默认） | DEBUG=1, PLUS=0 | 仅 APP2 | **APP1**（烧错也能回 APP1） |
| 调试模式Plus版 | DEBUG=1, PLUS=1 | **APP1 + APP2** 双窗口 | **APP1** |
| 正式模式 | DEBUG=0, PLUS=任意 | **APP1 + APP2** 双窗口 | **实际烧录槽位**（烧到哪里就跳到哪里） |

正式模式还做了“按实际下载地址写跳转槽”的兜底（`Bootloader_UdsMain` 的 `FW_UPDATE_COMPLETE`，见第四节），可直接支持动态选槽。
## 三、0x34 地址映射 → 烧写区域

### 3.1 0x34 请求格式（RequestDownload）

```
34 [DFI数据格式] [ALFI地址/长度格式] [地址字节...] [大小字节...]
例: 34 00 44 08 00 40 00 00 08 00 00 00 ...
    ALFI=0x44 → 地址4字节 + 大小4字节
```

### 3.2 地址映射（flash_download.h）

```c
#define FW_APP1_START_ADDR          0x0001A000   /* APP1 烧写基址 */
#define FW_APP_START_ADDR           0x0004C000   /* APP2 烧写基址 */
#define FW_APP_MAX_SIZE             0x00014000   /* 80KB */

/* TBOX 地址窗: 窗口A → APP1, 窗口B → APP2 */
#define TBOX_ADDR_APP1_START        0x08018000UL
#define TBOX_ADDR_APP1_END          0x0802C000UL   /* 80KB */
#define TBOX_ADDR_APP2_START        0x08004000UL
#define TBOX_ADDR_APP2_END          0x08018000UL   /* 80KB */

#define MAP_TBOX_ADDR_TO_FLASH(addr) \
    (((addr) >= TBOX_ADDR_APP1_START && (addr) < TBOX_ADDR_APP1_END) ? \
     ((addr) - TBOX_ADDR_APP1_START + FW_APP1_START_ADDR) : \
    (((addr) >= TBOX_ADDR_APP2_START && (addr) < TBOX_ADDR_APP2_END) ? \
     ((addr) - TBOX_ADDR_APP2_START + FW_APP_START_ADDR) : (addr)))
```

### 3.3 映射关系（按模式）

```
TBOX 地址                实际 Flash 地址           区域
0x08018000 ~ 0x0802BFFF  →  0x0001A000 ~ 0x0002DFFF  APP1（80KB）
0x08004000 ~ 0x08017FFF  →  0x0004C000 ~ 0x0005FFFF  APP2（80KB）
窗口外                    →  原样返回 → 校验失败（NRC 0x31）
```

校验 `fw_is_address_valid(mapped)`：

| 模式 | 允许的窗口 |
|---|---|
| 调试模式 | 仅 APP2（`0x4C000~0x60000`） |
| 调试模式Plus版 / 正式模式 | **APP1（0x1A000~0x2E000）+ APP2** 双窗口 |

（由 `BOOT_OTA_DUAL_WINDOW_EN` 编译控制，`flash_download.c` 中按模式放行对应窗口。）

### 3.4 结论（回答“0x34 是否决定 APP1/APP2”）

**是的，0x34 的地址字段决定映射到的烧写区域**，机制是“地址 → `MAP_TBOX_ADDR_TO_FLASH` → `target_address` → 擦写”。当前实现：

- **调试模式**：仅 APP2 窗口有效（`0x08004000~0x08018000` → APP2）；
- **调试模式Plus版 / 正式模式**：**APP1 + APP2 双窗口**有效，TBOX 发 `0x08018000~0x0802BFFF` 即烧 APP1，发 `0x08004000~0x08017FFF` 即烧 APP2。

相关服务同样走这套映射：0x31 `RID_ERASE_FIRMWARE`、`RID_CALCULATE_CRC`。
## 四、烧录完成后的跳转目标（两个写入点）

| 时机 | 写什么 | 谁写 | 说明 |
|---|---|---|---|
| 0x31 例程控制（boot 上下文） | `0x7C000 = UDS_POST_FLASH_BOOT_ADDR` 对应 magic | `uds_diagnostic.c` | 调试/调试Plus=APP1；正式=APP2（宏决定，仅作初始设置） |
| `FW_UPDATE_COMPLETE` | `0x7C000 = 实际下载目标` magic | `Bootloader_App.c` | 按 `FlashDownload_GetProgress().target_address` 判定实际烧写地址再写槽；**正式模式**下生效 |
| `FW_UPDATE_COMPLETE` | 共享区 `target_slot` + 清 WDT 计数 | `Bootloader_App.c` | **所有模式**都按实际下载目标槽记录并清除该槽 WDT 故障计数 |

```c
/* Bootloader_UdsMain: FW_UPDATE_COMPLETE 处理 */
if (!s_uds_shared_written && FlashDownload_GetState() == FW_UPDATE_COMPLETE) {
    stc_uds_shared_t state;
    FlashDownloadProgress_t stcProg;
    UdsShared_Read(&state);
    FlashDownload_GetProgress(&stcProg);

    state.phase = UDS_PHASE_PROGRAMMING_DONE;
    state.result = 1;
    /* 实际下载目标槽（由 0x34 地址映射决定: APP1/APP2） */
    if (stcProg.target_address == APP2_START_ADDR) {
        state.target_slot = SLOT_APP2;
    } else {
        state.target_slot = SLOT_APP1;
    }
    UdsShared_Write(&state);
    s_uds_shared_written = 1;
    ClearAppStateBySlot(state.target_slot);      /* 清实际下载槽的 WDT 计数 */
#if (BOOT_OTA_MODE_DEBUG == 0U)
    /* 正式模式: 烧到哪里，就设置跳转到哪里 */
    if (stcProg.target_address == APP2_START_ADDR) {
        Boot_SetRunSlotToAddr(APP2_START_ADDR);
    } else if (stcProg.target_address == APP1_START_ADDR) {
        Boot_SetRunSlotToAddr(APP1_START_ADDR);
    }
#endif
}
```

复位后 boot 再次上电 → `GetCurrentSlot()` 读到的就是刚写好的槽位 → 跳转到“烧录的目标”。
## 五、跳转决策汇总

| 场景 | 0x7C000 结果 | 复位后跳转 |
|---|---|---|
| 正常上电（无 OTA） | 保持原值 | 原槽（健康前提下） |
| OTA 下载完成（调试模式） | APP1 magic | APP1 |
| OTA 下载完成（正式模式） | 实际下载槽 magic（当前=APP2） | APP2 |
| 阶段2 强制进编程模式（0xFF） | 不写槽 | 原槽（下载完成后按上述规则） |
| 阶段3 强制设槽（0x01/0x02） | 目标槽 magic | 目标槽（坏块≥3 拒绝，不写） |
| 双 APP 均故障 | 不写 | 留在编程模式等待刷写 |

---

## 六、双窗口映射说明（2026-08-02 已实现）

- **APP1 烧写映射已实现**：TBOX 地址 `0x08018000~0x0802C000` → APP1（0x1A000），与 APP2 窗口（`0x08004000~0x08018000` → 0x4C000）并存；
- **窗口放行由模式控制**：调试模式仅 APP2；调试Plus/正式模式双窗口（`BOOT_OTA_DUAL_WINDOW_EN`）；
- **下载完成后的槽位记录**已按实际下载目标（`target_slot`、WDT 计数清除、正式模式跳转槽写入均跟随实际地址）；
- 上位机/TBOX 如需烧 APP1：0x34 请求地址发 `0x08018000`（对应实际 0x1A000）；烧 APP2 保持 `0x08004000`；
- 注意：APP1/APP2 的链接区均为 80KB（`0x14000`），烧写固件大小不得超过窗口。
