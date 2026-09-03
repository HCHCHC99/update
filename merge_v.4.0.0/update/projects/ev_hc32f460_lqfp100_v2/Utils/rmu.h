#ifndef __RMU_H__
#define __RMU_H__

#include "hc32_ll.h"
#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * RMU 重启记录模块（Utils/rmu.c）
 * ----------------------------------------------------------------------------
 * 职责：boot 上电第一件事 —— 读取全部 RMU 复位状态（RSTF0 所有位）、
 *       分类（故障/正常）、清除粘性标志、更新并写回 FLASH 记录。
 * 记录位置：每个 APP 的状态扇区（APP1=0x16000 / APP2=0x18000）。
 *           +0x000 / +0x008 偏移与原有 WDT_FEED_CONTROL / WDT_COUNT 保持一致。
 * 故障类（SWDT/WDT/MPU_ERR）进 APP 故障计数并持久化；
 * 正常原因（POR/掉电/软件复位等）仅在 RAM 视图显示本次原因，
 * 不再累计/写 FLASH（保护 FLASH 擦写寿命）。
 * ========================================================================== */

#define RMU_RECORD_MAGIC        0x524D5532UL   /* "RMU2" */
#define RMU_RECORD_VERSION      1UL
#define RMU_FAULT_MASK          (RMU_FLAG_SWDT | RMU_FLAG_WDT | RMU_FLAG_MPU_ERR)

typedef enum {
    RMU_SLOT_APP1 = 0,
    RMU_SLOT_APP2 = 1
} en_rmu_slot_t;

/* 上次复位原因（可读性视图）：每个字段 0=否，1=是（按 RSTF0 各位填充） */
typedef struct {
    uint32_t bPor;          /* 上电复位 PORF */
    uint32_t bPin;          /* 复位脚复位 PINRF */
    uint32_t bBor;          /* 欠压复位 BORF */
    uint32_t bPvd1;         /* PVD1 复位 */
    uint32_t bPvd2;         /* PVD2 复位 */
    uint32_t bWdt;          /* WDT 复位（故障） */
    uint32_t bSwdt;         /* SWDT 复位（故障） */
    uint32_t bPowerDown;    /* 掉电复位 PDRF */
    uint32_t bSw;           /* 软件复位 SWRF */
    uint32_t bMpu;          /* MPU 错误复位（故障） */
    uint32_t bRamParity;    /* RAM 奇偶校验错误复位 */
    uint32_t bRamEcc;       /* RAM ECC 错误复位 */
    uint32_t bClkErr;       /* 时钟频率错误复位 */
    uint32_t bXtalErr;      /* 晶振错误复位 */
    uint32_t bMulti;        /* 多重复位原因 MULTIRF */
} stc_rmu_last_cause_t;

/* 复位原因累计计数（仅故障类 WDT/SWDT/MPU 持久化；正常项恒 0，Keil Watch 可直接展开） */
typedef struct {
    uint32_t u32Por;        /* 上电复位 */
    uint32_t u32Pin;        /* 复位脚复位 */
    uint32_t u32Bor;        /* 欠压复位 */
    uint32_t u32Pvd1;       /* PVD1 复位 */
    uint32_t u32Pvd2;       /* PVD2 复位 */
    uint32_t u32Wdt;        /* WDT 复位（故障） */
    uint32_t u32Swdt;       /* SWDT 复位（故障） */
    uint32_t u32PowerDown;  /* 掉电复位 */
    uint32_t u32Sw;         /* 软件复位 */
    uint32_t u32Mpu;        /* MPU 错误复位（故障） */
    uint32_t u32RamParity;  /* RAM 奇偶校验错误 */
    uint32_t u32RamEcc;     /* RAM ECC 错误 */
    uint32_t u32ClkErr;     /* 时钟频率错误 */
    uint32_t u32XtalErr;    /* 晶振错误 */
    uint32_t u32Multi;      /* 多重复位原因 */
} stc_rmu_reason_count_t;

typedef struct {
    uint32_t u32FeedCtrl;         /* +0x000 兼容原 WDT_FEED_CONTROL_APPx_ADDR */
    uint32_t u32Magic;            /* +0x004 记录有效标志 */
    uint32_t u32FaultCount;       /* +0x008 兼容原 WDT_COUNT_APPx_ADDR */
    uint32_t u32LastResetCause;   /* +0x00C 最近一次故障复位原始 RSTF0（正常上电不更新/不写） */
    uint32_t u32LastFaultCause;   /* +0x010 最近一次故障原因，0=无 */
    uint32_t u32NonFaultCount;    /* +0x014 兼容字段，不再累计（正常上电不写 FLASH） */
    stc_rmu_reason_count_t stcReasonCount;  /* +0x018 各复位原因累计计数 */
} stc_rmu_slot_record_t;

/* 调试用 RAM 镜像：Keil Watch 直接加下面四个全局变量即可分别查看 APP1/APP2 */
extern volatile stc_rmu_last_cause_t   g_stcRmuLastCauseApp1;   /* APP1 上次复位原因视图（0/1） */
extern volatile stc_rmu_reason_count_t g_stcRmuReasonCountApp1; /* APP1 各原因累计计数镜像 */
extern volatile stc_rmu_last_cause_t   g_stcRmuLastCauseApp2;   /* APP2 上次复位原因视图（0/1） */
extern volatile stc_rmu_reason_count_t g_stcRmuReasonCountApp2; /* APP2 各原因累计计数镜像 */

void        Rmu_ProcessPowerUp(en_rmu_slot_t eCurrentSlot); /* 启动序列第一件事 */
uint32_t    Rmu_ReadRawStatus(void);                        /* 读 RSTF0 全部状态（不清标志） */
bool        Rmu_IsFaultCause(uint32_t u32RawCause);         /* 按 RMU_FAULT_MASK 分类 */
const char *Rmu_CauseName(uint32_t u32RawCause);            /* 诊断打印用 */
int32_t     Rmu_LoadSlotRecord(en_rmu_slot_t eSlot, stc_rmu_slot_record_t *pstcRec);
int32_t     Rmu_SaveSlotRecord(en_rmu_slot_t eSlot, const stc_rmu_slot_record_t *pstcRec);
int32_t     Rmu_ClearSlotFault(en_rmu_slot_t eSlot);        /* 只清故障计数（调试用） */
int32_t     Rmu_ResetSlotRecord(en_rmu_slot_t eSlot);       /* 整槽清零（UDS 刷写该 APP 后调用）：擦除状态扇区 + 清 RAM 镜像 */
uint32_t    Rmu_GetFaultCount(en_rmu_slot_t eSlot);
uint32_t    Rmu_GetLastResetCause(en_rmu_slot_t eSlot);

#endif /* __RMU_H__ */
