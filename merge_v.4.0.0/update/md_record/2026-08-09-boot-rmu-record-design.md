# Boot RMU 重启记录与 APP 故障计数设计

> 日期: 2026-08-09
> 适用范围: OTA 工程 boot（`D:\ota_ddl3.3_v.3.1\merge_v.2.0.0\boot`）
> 说明: 仅改 boot 目录；app1/app2 的 Bootloader_App 副本（独立编译）不动；不修改任何 uvprojx/uvoptx（工程文件由用户自行添加）
> 状态: 待用户评审（评审通过后再进入实现）

---

## 1. 目标

1. boot 上电后**第一件事**处理 RMU：先读 FLASH 中“上次运行的 APP 槽”，再读全部复位状态 → 分类（故障/正常）→ 清标志 → 记录 → 写 FLASH，必须在 50ms 强制指令窗口和 UDS 分支之前完成。
2. 新增 RMU 状态读取函数：读取 RSTF0 **全部状态位**；故障类（SWDT/WDT/MPU）进入 APP 故障计数，其他正常原因（POR/掉电/复位脚/软件复位等）只记录、不计故障。
3. `Utils` 模块新增 `rmu.c/.h`：用结构体管理重启记录；包含 APP1/APP2 故障次数、最近复位原因、普通复位累计次数；提供“读取全状态”和“清除故障记录”函数。
4. 记录写入现有错误计数相关扇区：APP1 = `0x16000`（扇区11）、APP2 = `0x18000`（扇区12）。
5. 覆盖边界场景：**第一次上电（全新/擦除态）**、**第一次上电无 APP（空片）**。
6. 修正已知隐患：RMU 标志在进入 UDS/强制指令分支前就被读取并清除，避免“SWDT 复位 → 先进 UDS → 标志残留 → 下次多计 1 次”。

## 2. 已确认的设计决策

| 决策点 | 选择 |
|---|---|
| 记录位置 | 方案 A：每个 APP 状态扇区各放一份自己的记录结构 |
| 正常原因记录形式 | 方案 A：`last_reset_cause` + `last_fault_cause` + `non_fault_count` |
| 写入策略 | 方案 A：每次上电都写（后续再优化为“仅故障/原因变化时写”） |
| 故障类掩码 | 维持现状：`SWDRF | WDRF | MPUERF` |
| 工程文件 | 由用户自行在 Keil 中添加 rmu.c，实现不修改 uvprojx/uvoptx |

## 3. 新增文件

- `boot/projects/ev_hc32f460_lqfp100_v2/Utils/rmu.h`
- `boot/projects/ev_hc32f460_lqfp100_v2/Utils/rmu.c`
- 用户自行把 `rmu.c` 加进 `boot/projects/ev_hc32f460_lqfp100_v2/template/MDK/template - 副本.uvprojx`

## 4. 上电启动顺序（先读槽，再处理 RMU）

```
Boot_StartupSequence():
  ① GetCurrentSlot()            读 FLASH 0x7C000，得到“上次运行的槽”
      - 0x5A5A5A5A → APP1；0xA5A5A5A5 → APP2
      - 0xFFFFFFFF / 其他无效值 → 第一次上电/槽未初始化 → 默认 APP1（沿用 ValidateSlotFlag）
  ② Rmu_ProcessPowerUp(eSlot)   第一件事处理 RMU（本模块核心，见 5.3）
  ③ 50ms 强制指令窗口（坏块判定用 Rmu_GetFaultCount）
  ④ UDS 共享区检查（phase==1 → UDS 编程模式）
  ⑤ 固件有效性检查 + 槽状态评估（含首次/空片，见 6）
  ⑥ SelectTargetSlot / UpdateSlotFlagToFlash / Bootloader_JumpToApp
      - 无可用槽 → RunBootloaderForever → UDS 编程模式等待刷写
```

> 顺序说明：RMU 计数要记在“上次运行的 APP”上，所以必须先读 0x7C000 确定槽，再做 RMU 检测/计数/写记录。二者都只需要读 FLASH/寄存器，不需要 CAN 等外设初始化。

## 5. rmu.h 设计

### 5.1 结构体（每槽 32 字节，8KB 扇区足够）

```c
#define RMU_RECORD_MAGIC        0x524D5531UL   /* "RMU1" */
#define RMU_RECORD_VERSION      1UL
#define RMU_FAULT_MASK          (RMU_FLAG_SWDT | RMU_FLAG_WDT | RMU_FLAG_MPU_ERR)

typedef enum {
    RMU_SLOT_APP1 = 0,
    RMU_SLOT_APP2 = 1
} en_rmu_slot_t;

typedef struct {
    uint32_t u32FeedCtrl;        /* +0x000 兼容原 WDT_FEED_CONTROL_APPx_ADDR */
    uint32_t u32Magic;           /* +0x004 记录有效标志 */
    uint32_t u32FaultCount;      /* +0x008 兼容原 WDT_COUNT_APPx_ADDR */
    uint32_t u32LastResetCause;  /* +0x00C 最近一次上电原始 RSTF0（含正常原因） */
    uint32_t u32LastFaultCause;  /* +0x010 最近一次故障原因，0=无 */
    uint32_t u32NonFaultCount;   /* +0x014 普通复位/掉电累计次数 */
    uint32_t au32Reserved[2];    /* +0x018 / +0x01C */
} stc_rmu_slot_record_t;
```

> 关键：`+0x000`、`+0x008` 偏移与现有 memory_map.h 宏一致，旧数据可无缝沿用。

### 5.2 对外 API

```c
void        Rmu_ProcessPowerUp(en_rmu_slot_t eCurrentSlot); /* 启动序列第②步调用 */
uint32_t    Rmu_ReadRawStatus(void);                        /* 读 RSTF0 全部状态（不清标志） */
bool        Rmu_IsFaultCause(uint32_t u32RawCause);         /* 按 RMU_FAULT_MASK 分类 */
const char *Rmu_CauseName(uint32_t u32RawCause);            /* 诊断打印用 */
int32_t     Rmu_LoadSlotRecord(en_rmu_slot_t eSlot, stc_rmu_slot_record_t *pstcRec);
int32_t     Rmu_SaveSlotRecord(en_rmu_slot_t eSlot, const stc_rmu_slot_record_t *pstcRec);
int32_t     Rmu_ClearSlotFault(en_rmu_slot_t eSlot);        /* 清除故障记录 */
uint32_t    Rmu_GetFaultCount(en_rmu_slot_t eSlot);
uint32_t    Rmu_GetLastResetCause(en_rmu_slot_t eSlot);
```

### 5.3 rmu.c 内部要点

- `Rmu_ReadRawStatus()`：直接读 `CM_RMU->RSTF0` 原始值（uint16，转 uint32）。
- `Rmu_IsFaultCause()`：`(raw & RMU_FAULT_MASK) != 0` 即故障；否则为正常原因。
- `Rmu_ProcessPowerUp()` 流程：
  1. `raw = Rmu_ReadRawStatus()`；
  2. `isFault = Rmu_IsFaultCause(raw)`；
  3. `PWC_REG_Unlock(PWC_UNLOCK_CODE1)` → `RMU_ClearStatus()` → `PWC_REG_Lock(...)`（**立即清标志**）；
  4. 载入当前槽记录（magic 不符按“旧数据迁移/首次初始化”处理，见 7）；
  5. 更新 `u32LastResetCause = raw`；
  6. 若 `isFault` 且该槽 `u32FaultCount < MAX_WDT_RESET_COUNT(3)`：`u32FaultCount+1`，`u32LastFaultCause = raw & RMU_FAULT_MASK`；否则 `u32NonFaultCount+1`；
  7. `Rmu_SaveSlotRecord()` 整结构读-改-写回该槽扇区（每次上电都写，见决策）；
  8. RTT 打印：`[RMU] raw=0x%04X cause=%s slot=%d fault=%d normal=%d`。
- FLASH 操作复用现有 EFM 方式（`EFM_REG_Unlock` → `EFM_SectorErase` → `EFM_ProgramWord` → `EFM_REG_Lock`），擦除后按结构偏移逐字写回。
- `Rmu_SaveSlotRecord()` 先读旧记录、合并后整写，避免破坏 feed_ctrl/计数（读-改-写）。
- 槽位→扇区映射：`RMU_SLOT_APP1 → APP1_STATE_SECTOR_BASE(0x16000)`、`RMU_SLOT_APP2 → APP2_STATE_SECTOR_BASE(0x18000)`，来自 memory_map.h。

## 6. 边界场景：第一次上电 / 无 APP

### 6.1 第一次上电（全新板/整片擦除态）

- **判定**：`0x7C000` 读回 `0xFFFFFFFF`（或非两个有效 magic）→ 槽未初始化，默认 APP1。
- **RMU**：本次原因通常是 `PORF`（也可能 `BORF/PINRF` 等），归为正常原因 → 不计故障，`non_fault_count+1`。
- **记录初始化**：状态扇区 `magic` 无效 → 按首次初始化写入：`fault_count=0`、`non_fault_count=1`、`last_reset_cause=本次原因`、`last_fault_cause=0`，并写回 magic；同时沿用现有逻辑把 `0x7C000` 回写为 APP1 magic。
- **跳转**：若 APP1 有固件 → 正常跳 APP1；若 APP1 无固件 → 按 6.2 处理。

### 6.2 第一次上电无 APP（空片，两个槽都无固件）

- **固件有效性检查（新增）**：`IsAppFirmwareValid(u32AppAddr)`：
  - `SP = *(uint32_t*)u32AppAddr` 必须落在 RAM 范围（`RAM_START_ADDR ~ RAM_END_ADDR`）；
  - `ResetVector = *(uint32_t*)(u32AppAddr+4)` 必须非 `0xFFFFFFFF` 且在合法 APP 地址范围。
- **槽状态评估**：`UpdateAppState()` 改为 `AVAILABLE = (fault_count < 3) && IsAppFirmwareValid(addr)`，即“无固件”视为不可用。
- **两个槽都无固件** → `SelectTargetSlot` 返回 `SLOT_NONE` → `RunBootloaderForever()` → `UdsOta_Bootloader_Enter()` 进入 UDS 编程模式等待刷写（不再出现“跳空片失败后在主循环空转”的情况）。
- **一个槽有固件、一个槽无固件**：有固件且计数未满的槽正常启动；无固件槽不参与跳转。
- **OTA 中途/擦除期间**：boot 在 UDS 编程模式内完成擦写，不依赖本检查；下载完成后目标槽固件有效，复位后正常跳转。
- 行为变化说明：现有代码只按“计数<3”判定可用，可能跳到空片后 `Bootloader_JumpToApp` 校验失败返回、主循环空转；本设计补上固件有效性检查并统一进入 UDS 等待刷写。

## 7. 兼容性与迁移

1. `+0x000`（feed_ctrl）、`+0x008`（fault_count）偏移不变 → 旧板数据直接沿用。
2. 首次升级：`u32Magic != RMU_RECORD_MAGIC` 时，保留旧 feed_ctrl 和 fault_count，其余新字段初始化后整写回，并写入 magic。
3. `0xFFFFFFFF` 擦除态统一视为默认值（count=0、cause=0、non_fault=0）。
4. 软件复位（`NVIC_SystemReset`，OTA/强制指令触发）只置 `SWRF` → 归为正常原因，不进故障计数。
5. 正常上电（POR）→ `last_reset_cause=PORF`，`non_fault_count+1`，故障计数不变。

## 8. 诊断打印（RTT）

- 每次上电：`[RMU] raw=0x%04X cause=%s slot=%d fault=%d normal=%d`
- 故障时额外：`[RMU] fault_count++ -> %d (limit 3)`
- 首次/空片：`[RMU] first boot / no valid APP, enter UDS programming mode`
- 双槽都 ≥3 时沿用现有 `RunBootloaderForever` 提示。

## 9. 验证方案（实现后手动验证）

1. **首次上电（空片）**：上电应进入 UDS 编程模式（RTT 提示无有效 APP），可正常刷入 APP1。
2. **正常上电**：RTT `cause=PORF`、`fault=0`、`normal` 递增；读 `0x16008/0x18008` 计数仍为 0。
3. **APP1 卡死 3 次**：每次 RTT `cause=SWDT`、APP1 fault 0→1→2→3；第 3 次后 APP1 DISABLED，boot 自动改跳 APP2（0x7C000 改写）。
4. **APP2 再卡死 3 次**：双槽 DISABLED → 停在 UDS 编程模式等待刷写。
5. **OTA 刷 APP1 成功后**：`Rmu_ClearSlotFault(APP1)` 生效，`0x16008=0`。
6. **回归：软件复位**（0x31 OTA 触发、强制指令）后上电，`cause=SWRF`，故障计数不变。
7. **回归：SWDT 复位后立刻用 `0x18FF5858/0xFF` 进 UDS**，完成后正常上电不再“多计 1 次”。

## 10. 已知取舍 / 风险

- **FLASH 磨损**：方案 A 每次上电擦写一次当前槽 8KB 扇区；长期频繁上下电有磨损，后续按决策切方案 B（仅故障/原因变化时写）。
- **正常原因只记在“当前运行槽”扇区**：跨槽分析需分别读两个扇区。
- **boot 自身 SWDT 超时**：若 boot 初始化耗时接近 SWDT 默认超时，理论上会自我复位；当前初始化耗时远小于超时，实测确认即可。
- **固件有效性检查为轻量校验**（SP/ResetVector），不做整包 CRC；完整校验仍由 OTA 下载阶段的 0x37 CRC 负责。
