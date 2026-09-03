#include "Bootloader_App.h"
#include "Adapter_Can.h"
#include "isotp_transport.h"
#include "uds_diagnostic.h"
#include "flash_download.h"
#include "TickTimer.h"
#include "rtt_log.h"
#include "main.h"
#include "uds_ota.h"
#include "rmu.h"
/* ===== 阶段2/3: 上电强制指令检测 (CAN ID 0x18FF5858) ===== */
static volatile uint8_t s_force_cmd = 0U;
static volatile uint8_t s_force_window_active = 0U;

/* 强制指令 RX 回调：仅记录 data[0]，由 Boot_StartupSequence 的 50ms 窗口消费 */
/* 阶段3: 强制指令结果回帧（CAN ID 0x18EF5858，data[0]=状态） */
static void Boot_SendForceResp(uint8_t u8Status)
{
    CanMsg_t stcMsg;
    MEM_ZERO_STRUCT(stcMsg);
    stcMsg.u32ID = BOOT_FORCE_RESP_CAN_ID;
    stcMsg.u8IDE = 1U;
    stcMsg.u8DLC = 8U;
    stcMsg.au8Data[0] = u8Status;
    CanIf_Send(&stcMsg);
    MAIN_D("  Force resp 0x18EF5858: status=0x%02X\r\n", (unsigned int)u8Status);
}

static void Boot_ForceCmdRxCallback(const CanMsg_t *pMsg)
{
    if ((pMsg != NULL) && (pMsg->u8DLC >= 1U)) {
        s_force_cmd = pMsg->au8Data[0];
        if (s_force_window_active) {
            MAIN_D("  [FRC] RX 0x18FF5858 DLC=%d: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                   (int)pMsg->u8DLC,
                   (unsigned int)pMsg->au8Data[0], (unsigned int)pMsg->au8Data[1],
                   (unsigned int)pMsg->au8Data[2], (unsigned int)pMsg->au8Data[3],
                   (unsigned int)pMsg->au8Data[4], (unsigned int)pMsg->au8Data[5],
                   (unsigned int)pMsg->au8Data[6], (unsigned int)pMsg->au8Data[7]);
        }
    }
}

// ###########################################################################
//
//                          ##############################
//                          #                            #
//                          #    ����ʽ�㡿��ʽ���ܴ���    #
//                          #                            #
//                          ##############################
//
// ������Bootloader���ġ�Flash��WDT����ת������RAM
//
// ###########################################################################

// ==================== ����ȫ�ֱ����������ã� ====================
volatile en_dbg_clear_app_state_t g_eDebugClearAppState = DBG_CLEAR_NONE;

// ==================== �ڲ���̬�������� ====================
static en_slot_type_t GetCurrentSlot(void);
static void ValidateSlotFlag(stc_boot_context_t *pstcCtx);
static void InitAppInfo(stc_app_info_t *pstcApp, en_slot_type_t eSlot, uint32_t u32Addr);
static void UpdateAppState(stc_app_info_t *pstcApp);
static bool IsAppFirmwareValid(uint32_t u32AppAddr);
static void SelectTargetSlot(stc_boot_context_t *pstcCtx);
static void UpdateSlotFlagToFlash(stc_boot_context_t *pstcCtx);
static void RunBootloaderForever(void);
static void CheckAndClearAppState(void);

/* ===== Watch/RTT 可读性辅助：槽位 / APP 状态转字符串 ===== */
static const char *SlotToStr(en_slot_type_t eSlot)
{
    if (eSlot == SLOT_APP1) return "APP1";
    if (eSlot == SLOT_APP2) return "APP2";
    return "NONE";
}

static const char *AppStateToStr(en_app_state_t eState)
{
    return (eState == APP_STATE_AVAILABLE) ? "AVAILABLE" : "DISABLED";
}

uint32_t READ_FLASH_DIRECT(uint32_t addr)
{
    uint32_t value;
    uint32_t u32FrmcState;

    u32FrmcState = CM_EFM->FRMC;
    CM_EFM->FRMC = (u32FrmcState & ~EFM_FRMC_CACHE);
    __DSB();
    value = *((volatile uint32_t *)addr);
    __DMB();
    CM_EFM->FRMC = u32FrmcState;

    return value;
}
void Bootloader_Delay(uint32_t u32Count)
{
    while (u32Count-- > 0) { __nop(); }
}

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

// ###########################################################################
//                          ����RAM ��ʼ��
// ###########################################################################
void InitSharedCtrl(void)
{
    stc_shared_ctrl_t *pCtrl = GetSharedCtrl();
    if (pCtrl->eApp1FeedCtrl != WDT_FEED_ENABLE &&
        pCtrl->eApp1FeedCtrl != WDT_FEED_DISABLE)
    {
        pCtrl->eApp1FeedCtrl = WDT_FEED_ENABLE;
        pCtrl->eApp2FeedCtrl = WDT_FEED_ENABLE;
        pCtrl->debug_flag = 0;
        for (int i = 0; i < 5; i++) pCtrl->reserved[i] = 0;
    }
}

// ###########################################################################
//                          Flash ����
// ###########################################################################
int32_t Bootloader_FlashEraseSector(uint32_t u32Addr)
{
    int32_t res;
    EFM_REG_Unlock();
    EFM_FWMC_Cmd(ENABLE);
    while(SET != EFM_GetStatus(EFM_FLAG_RDY));
    res = EFM_SectorErase(u32Addr);
    EFM_REG_Lock();
    return res;
}

// ###########################################################################
//                          ���Ź�״̬����
// ###########################################################################
uint32_t GetWdtResetCount(uint32_t u32Addr)
{
    if (u32Addr == WDT_COUNT_APP1_ADDR) return Rmu_GetFaultCount(RMU_SLOT_APP1);
    if (u32Addr == WDT_COUNT_APP2_ADDR) return Rmu_GetFaultCount(RMU_SLOT_APP2);
    return 0U;
}

void UpdateWdtResetCount(uint32_t u32Addr, uint32_t u32CurrentCount)
{
    stc_rmu_slot_record_t stcRec;
    en_rmu_slot_t eSlot;
    uint32_t u32Count;

    (void)u32CurrentCount;
    if (u32Addr == WDT_COUNT_APP1_ADDR)      eSlot = RMU_SLOT_APP1;
    else if (u32Addr == WDT_COUNT_APP2_ADDR) eSlot = RMU_SLOT_APP2;
    else return;

    if (Rmu_LoadSlotRecord(eSlot, &stcRec) != 0) return;
    if (stcRec.u32Magic != RMU_RECORD_MAGIC) {
        stcRec.u32Magic = RMU_RECORD_MAGIC;
        stcRec.u32LastResetCause = 0U;
        stcRec.u32LastFaultCause = 0U;
        stcRec.u32NonFaultCount = 0U;
    }
    u32Count = (stcRec.u32FaultCount == 0xFFFFFFFFUL) ? 0U : stcRec.u32FaultCount;
    if (u32Count < MAX_APP_FAULT_COUNT) {
        stcRec.u32FaultCount = u32Count + 1U;
        Rmu_SaveSlotRecord(eSlot, &stcRec);
    }
}

void ClearWdtResetCount(uint32_t u32Addr)
{
    if (u32Addr == WDT_COUNT_APP1_ADDR)      Rmu_ClearSlotFault(RMU_SLOT_APP1);
    else if (u32Addr == WDT_COUNT_APP2_ADDR) Rmu_ClearSlotFault(RMU_SLOT_APP2);
}

void SetWdtFeedControl(uint32_t u32Addr, uint32_t u32Value)
{
    stc_rmu_slot_record_t stcRec;
    en_rmu_slot_t eSlot;

    if (u32Value != WDT_FEED_ENABLE && u32Value != WDT_FEED_DISABLE) return;
    if (u32Addr == WDT_FEED_CONTROL_APP1_ADDR)      eSlot = RMU_SLOT_APP1;
    else if (u32Addr == WDT_FEED_CONTROL_APP2_ADDR) eSlot = RMU_SLOT_APP2;
    else return;

    if (Rmu_LoadSlotRecord(eSlot, &stcRec) != 0) return;
    if (stcRec.u32Magic != RMU_RECORD_MAGIC) {
        stcRec.u32Magic = RMU_RECORD_MAGIC;
        stcRec.u32LastResetCause = 0U;
        stcRec.u32LastFaultCause = 0U;
        stcRec.u32NonFaultCount = 0U;
    }
    if (stcRec.u32FaultCount == 0xFFFFFFFFUL)    stcRec.u32FaultCount = 0U;
    if (stcRec.u32NonFaultCount == 0xFFFFFFFFUL) stcRec.u32NonFaultCount = 0U;
    stcRec.u32FeedCtrl = u32Value;
    Rmu_SaveSlotRecord(eSlot, &stcRec);
}

uint32_t GetWdtFeedControl(uint32_t u32Addr)
{
    stc_rmu_slot_record_t stcRec;
    en_rmu_slot_t eSlot;

    if (u32Addr == WDT_FEED_CONTROL_APP1_ADDR)      eSlot = RMU_SLOT_APP1;
    else if (u32Addr == WDT_FEED_CONTROL_APP2_ADDR) eSlot = RMU_SLOT_APP2;
    else return WDT_FEED_ENABLE;

    if (Rmu_LoadSlotRecord(eSlot, &stcRec) != 0) return WDT_FEED_ENABLE;
    return (stcRec.u32FeedCtrl == 0xFFFFFFFFUL) ? WDT_FEED_ENABLE : stcRec.u32FeedCtrl;
}

void ClearAppStateBySlot(en_slot_type_t eSlot)
{
    if (eSlot == SLOT_APP1)      Rmu_ClearSlotFault(RMU_SLOT_APP1);
    else if (eSlot == SLOT_APP2) Rmu_ClearSlotFault(RMU_SLOT_APP2);
}

// ###########################################################################
//                          Ӧ����ת & ��λ�л�
// ###########################################################################
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

    // 1. �������RAM��bootloader��APP��������
    // ClearAllRAM(); // APP startup handles RAM init
    
    // 2. ֹͣSysTick
    SysTick->CTRL = 0;
    SCB->ICSR |= SCB_ICSR_PENDSTCLR_Msk; 

    // 3. ���������ж�
    __disable_irq();
    DisableAllNVICInterrupts();
    
    // 4. ����ж�ʹ�ܺ͹���Ĵ���
    for (uint8_t i = 0; i < 8; i++) 
    {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    // 5. ����ջָ���������
    __set_MSP(*(uint32_t *)u32AppAddr);
    SCB->VTOR = ((uint32_t)u32AppAddr & SCB_VTOR_TBLOFF_Msk);
    
    __DSB();
    __ISB();

    // 6. ��ת��APP
    (*((void(*)(void))app_start_address))();
}

// RAM��պ������ؼ�ʵ�֣�
void ClearAllRAM(void)
{
    volatile uint32_t *ram_addr = (volatile uint32_t *)RAM_START_ADDR;
    register uint32_t current_msp __asm("msp");
    uint32_t clear_end;
    uint32_t ram_word_count;

    /* 保留当前栈及以下区域：Stack(0x9000) + Heap(0x4000) + 余量(0x2000) = 0xF000 */
    if (current_msp > RAM_START_ADDR + 0xF000U) {
        clear_end = current_msp - 0xF000U;
    } else {
        clear_end = RAM_START_ADDR;
    }
    if (clear_end > RAM_START_ADDR + RAM_SIZE) {
        clear_end = RAM_START_ADDR + RAM_SIZE;
    }

    ram_word_count = (clear_end - RAM_START_ADDR) / 4U;

    for (uint32_t i = 0; i < ram_word_count; i++)
    {
        ram_addr[i] = 0;
    }

    __DSB();
    __ISB();
}

void Boot_SwitchAndRunOther(void)
{
    uint32_t curr = READ_FLASH_DIRECT(APP_RUN_SLOT_ADDR);
    uint32_t target = (curr == SLOT_A_MAGIC) ? SLOT_B_MAGIC : SLOT_A_MAGIC;

    EFM_REG_Unlock();
    EFM_FWMC_Cmd(ENABLE);
    while(SET != EFM_GetStatus(EFM_FLAG_RDY));
    EFM_SectorErase(APP_RUN_SLOT_ADDR);
    EFM_ProgramWord(APP_RUN_SLOT_ADDR, target);
    EFM_REG_Lock();

    NVIC_SystemReset();
}

void Boot_SetRunSlotToAddr(uint32_t u32Addr)
{
    uint32_t u32Magic;
    if (u32Addr == APP1_START_ADDR)
        u32Magic = SLOT_A_MAGIC;
    else if (u32Addr == APP2_START_ADDR)
        u32Magic = SLOT_B_MAGIC;
    else
        return;

    EFM_REG_Unlock();
    EFM_FWMC_Cmd(ENABLE);
    while(SET != EFM_GetStatus(EFM_FLAG_RDY));
    EFM_SectorErase(APP_RUN_SLOT_ADDR);
    EFM_ProgramWord(APP_RUN_SLOT_ADDR, u32Magic);
    EFM_REG_Lock();
}

// ###########################################################################
//                          Bootloader ������
// ###########################################################################
void Bootloader_Init(void) {}

void Boot_StartupSequence(void)
{
    stc_boot_context_t stcCtx;
    memset(&stcCtx, 0, sizeof(stc_boot_context_t));

    MAIN_D("===== Bootloader Start =====\r\n");
    /* ==== 1. 第一件事：先读 FLASH 记录的上次运行槽，再处理 RMU ====
     * 读取全部 RMU 复位状态（RSTF0 所有位）→ 分类（故障/正常）→ 清除标志 →
     * 更新记录并写回 FLASH（故障进故障计数，正常原因只记录不计故障）。
     * 必须早于 50ms 强制指令窗口和 UDS 分支，避免 RMU 标志残留导致延迟计数。 */
    {
        en_slot_type_t ePowerUpSlot = GetCurrentSlot();
        if (ePowerUpSlot == SLOT_NONE) {
            ePowerUpSlot = SLOT_APP1;   /* 第一次上电/槽未初始化：默认 APP1 */
        }
        Rmu_ProcessPowerUp((ePowerUpSlot == SLOT_APP1) ? RMU_SLOT_APP1 : RMU_SLOT_APP2);
    }
    /* ==== 阶段2/3: 上电 50ms 强制指令检测窗口 ==== */
    {
        static bool s_force_filter_registered = false;
        uint64_t u64WinStart;

        if (!s_force_filter_registered) {
            CanIf_RxFilterEntry_t stcForceEntry;
            MEM_ZERO_STRUCT(stcForceEntry);
            stcForceEntry.u32CanId    = BOOT_FORCE_CMD_CAN_ID;
            stcForceEntry.u32CanMask  = 0UL;
            stcForceEntry.u8Format    = CAN_ID_EXT;
            stcForceEntry.pfnCallback = Boot_ForceCmdRxCallback;
            s_force_filter_registered = CanIf_RegisterRxFilter(&stcForceEntry);
        }

        s_force_cmd = 0U;
        s_force_window_active = 1U;
        u64WinStart = tickTimer_GetCount();
        while ((tickTimer_GetCount() - u64WinStart) < BOOT_FORCE_CMD_WINDOW_MS) {
            UdsOta_Poll();
            if (s_force_cmd != 0U) {
                MAIN_D("  Force cmd on 0x18FF5858 = 0x%02X\r\n", (unsigned int)s_force_cmd);
                if (s_force_cmd == BOOT_FORCE_CMD_ENTER_BL) {
                    MAIN_D("  -> Force enter UDS programming mode (stage2, no APP)\r\n");
                    UdsOta_Bootloader_Enter();   /* 不返回 */
                }
                else if ((s_force_cmd == BOOT_FORCE_CMD_BOOT_APP2) ||
                         (s_force_cmd == BOOT_FORCE_CMD_BOOT_APP1)) {
                    /* 阶段3: 强制下次启动槽位
                     * - 受坏块标记限制：目标>=3 拒绝，不写自动跳转槽
                     * - 设置成功后软件复位，重新进 boot 按新槽位正常启动
                     * - 幂等保护：槽位已是目标值时不重复复位（防 TBOX 持续发送导致复位循环） */
                    uint32_t u32Fault1 = Rmu_GetFaultCount(RMU_SLOT_APP1);
                    uint32_t u32Fault2 = Rmu_GetFaultCount(RMU_SLOT_APP2);
                    uint32_t u32T0;
                    uint8_t u8App1Ok;
                    uint8_t u8App2Ok;
                    /* 与 InitAppInfo 保持一致：擦除态(0xFFFFFFFF)视为 0（未初始化，不算坏块） */
                    u8App1Ok = (u32Fault1 < MAX_APP_FAULT_COUNT) ? 1U : 0U;
                    u8App2Ok = (u32Fault2 < MAX_APP_FAULT_COUNT) ? 1U : 0U;

                    if ((u8App1Ok == 0U) && (u8App2Ok == 0U)) {
                        /* 双 APP 均故障：回帧并进入编程模式等待刷写 */
                        Boot_SendForceResp(BOOT_FORCE_RESP_BOTH_FAULTY);
                        MAIN_D("  -> Both APPs faulty, entering UDS programming mode\r\n");
                        UdsOta_Bootloader_Enter();   /* 不返回 */
                    }
                    else if (s_force_cmd == BOOT_FORCE_CMD_BOOT_APP2) {
                        if (u8App2Ok != 0U) {
                            if (READ_FLASH_DIRECT(APP_RUN_SLOT_ADDR) != SLOT_B_MAGIC) {
                                Boot_SetRunSlotToAddr(APP2_START_ADDR);
                                Boot_SendForceResp(BOOT_FORCE_RESP_APP2_OK);
                                MAIN_D("  -> Force boot slot = APP2, resetting...\r\n");
                                /* 确保回帧已发出后再复位 */
                                u32T0 = tickTimer_GetCount();
                                while (can_is_tx_busy() && ((tickTimer_GetCount() - u32T0) < 20U)) { }
                                NVIC_SystemReset();
                                while (1) { }
                            } else {
                                Boot_SendForceResp(BOOT_FORCE_RESP_APP2_OK);
                                MAIN_D("  -> Slot already APP2, no reset\r\n");
                            }
                        } else {
                            /* APP2 坏块标记>=3：拒绝，不写自动跳转槽 */
                            Boot_SendForceResp(BOOT_FORCE_RESP_REJECTED);
                            MAIN_D("  -> APP2 bad-blocked (>=3), rejected, slot unchanged\r\n");
                        }
                        break;
                    }
                    else {
                        if (u8App1Ok != 0U) {
                            if (READ_FLASH_DIRECT(APP_RUN_SLOT_ADDR) != SLOT_A_MAGIC) {
                                Boot_SetRunSlotToAddr(APP1_START_ADDR);
                                Boot_SendForceResp(BOOT_FORCE_RESP_APP1_OK);
                                MAIN_D("  -> Force boot slot = APP1, resetting...\r\n");
                                /* 确保回帧已发出后再复位 */
                                u32T0 = tickTimer_GetCount();
                                while (can_is_tx_busy() && ((tickTimer_GetCount() - u32T0) < 20U)) { }
                                NVIC_SystemReset();
                                while (1) { }
                            } else {
                                Boot_SendForceResp(BOOT_FORCE_RESP_APP1_OK);
                                MAIN_D("  -> Slot already APP1, no reset\r\n");
                            }
                        } else {
                            /* APP1 坏块标记>=3：拒绝，不写自动跳转槽 */
                            Boot_SendForceResp(BOOT_FORCE_RESP_REJECTED);
                            MAIN_D("  -> APP1 bad-blocked (>=3), rejected, slot unchanged\r\n");
                        }
                        break;
                    }
                }
            }
        }
    }
    s_force_window_active = 0U;

    stc_shared_ctrl_t *pSharedCtrl = GetSharedCtrl();
    if (pSharedCtrl->debug_flag == 0x5A5A5A5A) {
        SetWdtFeedControl(WDT_FEED_CONTROL_APP1_ADDR, pSharedCtrl->eApp1FeedCtrl);
        SetWdtFeedControl(WDT_FEED_CONTROL_APP2_ADDR, pSharedCtrl->eApp2FeedCtrl);
        pSharedCtrl->debug_flag = 0;
    }

    /* ==== UDS 共享状态检查 ==== */
    {
        stc_uds_shared_t stcUdsState;
        UdsShared_Read(&stcUdsState);

        MAIN_D("  UDS Shared: magic=0x%08X, phase=%d, pending_sid=0x%02X\r\n",
               (unsigned int)stcUdsState.magic, (int)stcUdsState.phase, (unsigned int)stcUdsState.pending_sid);

        if (stcUdsState.magic == UDS_SHARED_MAGIC) {
            if (stcUdsState.phase == UDS_PHASE_ENTER_BOOTLOADER) {
                MAIN_D("  -> Enter UDS Programming Mode\r\n");
                /* APP 请求进入编程模式 → 进入 Bootloader UDS 模式 */
                Bootloader_UdsMain();
                /* Bootloader_UdsMain 不返回，内部处理所有 UDS 通信 */
                while(1) { __nop(); }
            }
            /* 其他 phase (IDLE/PROGRAMMING_DONE): 正常启动 APP，
             * APP 启动后会通过 App_CheckPendingUdsAck() 检查 pending_sid */
        }
    }

    CheckAndClearAppState();
    stcCtx.eCurrentSlot = GetCurrentSlot();
    ValidateSlotFlag(&stcCtx);

    InitAppInfo(&stcCtx.stcApp1, SLOT_APP1, APP1_START_ADDR);
    InitAppInfo(&stcCtx.stcApp2, SLOT_APP2, APP2_START_ADDR);
    UpdateAppState(&stcCtx.stcApp1);
    UpdateAppState(&stcCtx.stcApp2);
    SelectTargetSlot(&stcCtx);
    UpdateSlotFlagToFlash(&stcCtx);

    MAIN_D("  CurSlot: %s, Target: %s\r\n",
           SlotToStr(stcCtx.eCurrentSlot), SlotToStr(stcCtx.eTargetSlot));
    MAIN_D("  APP1 state=%s, fault=%d | APP2 state=%s, fault=%d\r\n",
           AppStateToStr(stcCtx.stcApp1.eState), (unsigned int)stcCtx.stcApp1.u32FaultCount,
           AppStateToStr(stcCtx.stcApp2.eState), (unsigned int)stcCtx.stcApp2.u32FaultCount);

    if (stcCtx.eTargetSlot == SLOT_APP1)      Bootloader_JumpToApp(APP1_START_ADDR);
    else if (stcCtx.eTargetSlot == SLOT_APP2) Bootloader_JumpToApp(APP2_START_ADDR);
    else {
        MAIN_D("  ERROR: No valid APP slot, running forever!\r\n");
        RunBootloaderForever();
    }
}

// ###########################################################################
//                          �ڲ���̬����
// ###########################################################################
static en_slot_type_t GetCurrentSlot(void) {
    uint32_t s = READ_FLASH_DIRECT(APP_RUN_SLOT_ADDR);
    if (s == SLOT_A_MAGIC) return SLOT_APP1;
    if (s == SLOT_B_MAGIC) return SLOT_APP2;
    return SLOT_NONE;
}

static void ValidateSlotFlag(stc_boot_context_t *pstcCtx) {
    if (pstcCtx->eCurrentSlot == SLOT_NONE) {
        pstcCtx->eCurrentSlot = SLOT_APP1;
        pstcCtx->u8NeedUpdateSlotFlag = 1;
    }
}

static void InitAppInfo(stc_app_info_t *pstcApp, en_slot_type_t eSlot, uint32_t u32Addr) {
    pstcApp->eSlot = eSlot;
    pstcApp->u32StartAddr = u32Addr;
    pstcApp->u32FaultCount = Rmu_GetFaultCount((eSlot == SLOT_APP1) ? RMU_SLOT_APP1 : RMU_SLOT_APP2);
    pstcApp->eState = APP_STATE_AVAILABLE;
}

static void UpdateAppState(stc_app_info_t *pstcApp) {
    pstcApp->eState = ((pstcApp->u32FaultCount < MAX_APP_FAULT_COUNT) &&
                       IsAppFirmwareValid(pstcApp->u32StartAddr))
                      ? APP_STATE_AVAILABLE : APP_STATE_DISABLED;
}

/* 轻量固件有效性检查：SP 在 RAM 范围、ResetVector 非擦除态且在 APP 分区内。
 * 用于“第一次上电无 APP/空片”场景：无固件视为不可用，双槽无效时留在 bootloader。
 * 完整校验由 OTA 下载阶段的 0x37 CRC 负责。 */
static bool IsAppFirmwareValid(uint32_t u32AppAddr) {
    uint32_t u32Sp = READ_FLASH_DIRECT(u32AppAddr);
    uint32_t u32ResetVec = READ_FLASH_DIRECT(u32AppAddr + 4U);

    if (u32Sp < RAM_START_ADDR || u32Sp > RAM_END_ADDR) return false;
    if (u32ResetVec == 0xFFFFFFFFUL) return false;
    if (u32ResetVec < APP1_START_ADDR || u32ResetVec > APP2_END_ADDR) return false;
    return true;
}

static void SelectTargetSlot(stc_boot_context_t *pstcCtx) {
    uint8_t aok = (pstcCtx->stcApp1.eState == APP_STATE_AVAILABLE);
    uint8_t bok = (pstcCtx->stcApp2.eState == APP_STATE_AVAILABLE);

    if (pstcCtx->eCurrentSlot == SLOT_APP1 && aok) pstcCtx->eTargetSlot = SLOT_APP1;
    else if (pstcCtx->eCurrentSlot == SLOT_APP2 && bok) pstcCtx->eTargetSlot = SLOT_APP2;
    else if (bok) { pstcCtx->eTargetSlot = SLOT_APP2; pstcCtx->u8NeedUpdateSlotFlag = 1; }
    else if (aok) { pstcCtx->eTargetSlot = SLOT_APP1; pstcCtx->u8NeedUpdateSlotFlag = 1; }
    else pstcCtx->eTargetSlot = SLOT_NONE;
}

static void UpdateSlotFlagToFlash(stc_boot_context_t *pstcCtx) {
    if (!pstcCtx->u8NeedUpdateSlotFlag || pstcCtx->eTargetSlot == SLOT_NONE) return;
    uint32_t val = (pstcCtx->eTargetSlot == SLOT_APP1) ? SLOT_A_MAGIC : SLOT_B_MAGIC;

    EFM_REG_Unlock(); EFM_FWMC_Cmd(ENABLE); while(!EFM_GetStatus(EFM_FLAG_RDY));
    EFM_SectorErase(APP_RUN_SLOT_ADDR);
    EFM_ProgramWord(APP_RUN_SLOT_ADDR, val);
    EFM_REG_Lock();
    pstcCtx->u8NeedUpdateSlotFlag = 0;
}

static void RunBootloaderForever(void) {
    MAIN_D("  Both APPs disabled, entering UDS programming mode for recovery\r\n");
    UdsOta_Bootloader_Enter();
    /* UdsOta_Bootloader_Enter never returns */
    while(1) { __nop(); }
}
static void CheckAndClearAppState(void) {
    if (g_eDebugClearAppState == DBG_CLEAR_APP1) ClearAppStateBySlot(SLOT_APP1);
    else if (g_eDebugClearAppState == DBG_CLEAR_APP2) ClearAppStateBySlot(SLOT_APP2);
    else if (g_eDebugClearAppState == DBG_CLEAR_BOTH) { ClearAppStateBySlot(SLOT_APP1); ClearAppStateBySlot(SLOT_APP2); }
}

// ====================================================================
// UDS 共享区 Flash 读写函数
// ====================================================================

void UdsShared_Read(stc_uds_shared_t *pState)
{
    uint32_t *pSrc = (uint32_t *)UDS_SHARED_SECTOR_BASE;
    uint32_t *pDst = (uint32_t *)pState;
    uint32_t count = sizeof(stc_uds_shared_t) / 4;
    uint32_t i;
    for (i = 0; i < count; i++) {
        pDst[i] = READ_FLASH_DIRECT((uint32_t)(uintptr_t)(pSrc + i));
    }
}

void UdsShared_Write(const stc_uds_shared_t *pState)
{
    uint32_t u32SectorBase = UDS_SHARED_SECTOR_BASE;
    const uint32_t *pSrc = (const uint32_t *)pState;
    uint32_t count = sizeof(stc_uds_shared_t) / 4;
    uint32_t i;

    EFM_REG_Unlock();
    EFM_FWMC_Cmd(ENABLE);
    while(SET != EFM_GetStatus(EFM_FLAG_RDY));
    EFM_SectorErase(u32SectorBase);

    for (i = 0; i < count; i++) {
        EFM_ProgramWord(u32SectorBase + i * 4, pSrc[i]);
    }
    EFM_REG_Lock();
}

void UdsShared_Clear(void)
{
    Bootloader_FlashEraseSector(UDS_SHARED_SECTOR_BASE);
}

void UdsShared_SetPhase(uint32_t phase, uint32_t target_slot)
{
    stc_uds_shared_t state;
    MEM_ZERO_STRUCT(state);
    state.magic       = UDS_SHARED_MAGIC;
    state.phase       = phase;
    state.target_slot = target_slot;
    state.result      = 0;
    UdsShared_Write(&state);
}

// ====================================================================
// Bootloader UDS 编程模式主循环
// ====================================================================


// ====================================================================
// Bootloader UDS 编程模式: ISOTP RX 回调 + 过滤器注册
// ====================================================================

/* ISOTP 重组后的 UDS 消息输出缓冲区 */
static uint8_t s_bl_uds_rx_buffer[4100];

static void BL_ISOTP_RxCallback(const CanMsg_t *pMsg)
{
    uint16_t out_len = 0;
    int8_t result = isotp_receive_frame(0, pMsg->u32ID,
                                        (uint8_t*)pMsg->au8Data, pMsg->u8DLC,
                                        s_bl_uds_rx_buffer, &out_len);
    if (result == ISOTP_OK) {
        uds_receive_handler(0, pMsg->u32ID, s_bl_uds_rx_buffer, out_len);
    }
}

static void BL_ISOTP_RegisterRxFilters(void)
{
    static const uint32_t s_isotp_can_ids[4] = {
        0x18DA03F1UL,  /* 物理寻址请求 ID (TBOX → 控制器) */
        0x18DAF103UL,  /* 物理寻址响应 ID (控制器 → TBOX) */
        0x18FF8118UL,  /* OTA 专用 ID */
        0x18DBFFF0UL   /* 功能寻址请求 ID (广播) */
    };

    CanIf_RxFilterEntry_t stcEntry;
    stcEntry.u32CanId   = 0UL;
    stcEntry.u32CanMask = 0UL;  /* 精确匹配 */
    stcEntry.u8Format   = CAN_ID_EXT;
    stcEntry.pfnCallback = &BL_ISOTP_RxCallback;

    for (uint8_t i = 0U; i < 4U; i++) {
        stcEntry.u32CanId = s_isotp_can_ids[i];
        CanIf_RegisterRxFilter(&stcEntry);
    }
}

// ====================================================================
// Bootloader UDS 编程模式主循环
// ====================================================================

void Bootloader_UdsMain(void)
{
    uint64_t last_wdt_feed;
    FlashDownloadConfig_t stcFwConfig;
    uint8_t i;

    MAIN_D("===== Bootloader UDS Main Start =====\r\n");

    

    /* ==== 1. 检查是否需要发送 31 服务的肯定响应 ==== */
    {
        stc_uds_shared_t _st;
        UdsShared_Read(&_st);
        if (_st.magic == UDS_SHARED_MAGIC && _st.pending_sid == 0x31) {
            uint8_t au8Data[4] = {0x71, 0x01, 0xFF, 0x00};
            isotp_send_message(0, 0x18DAF103UL, au8Data, 4);
            MAIN_D("  Sent deferred 31 ACK (71 01 FF 00) via ISOTP\r\n");
            _st.pending_sid = 0;
            UdsShared_Write(&_st);
        } else {
            MAIN_D("  No pending 31 ACK (pending_sid=0x%02X), skip\r\n", (unsigned int)_st.pending_sid);
        }
    }

    /* ==== 2. 初始化固件下载模块 ==== */
    MEM_ZERO_STRUCT(stcFwConfig);
    stcFwConfig.max_firmware_size     = FW_MAX_FIRMWARE_SIZE;
    stcFwConfig.flash_sector_size    = FLASH_SECTOR_SIZE;
    stcFwConfig.user_start_addr      = UDS_TARGET_FLASH_ADDR;
    stcFwConfig.user_end_addr        = UDS_TARGET_FLASH_ADDR + APP_MAX_SIZE;
    stcFwConfig.verify_enabled       = 1U;
    stcFwConfig.auto_reset_on_complete = 0U;
    FlashDownload_Init(&stcFwConfig);
    MAIN_D("  FlashDownload init done (APP2: 0x%08X-0x%08X)\r\n",
           UDS_TARGET_FLASH_ADDR, UDS_TARGET_FLASH_ADDR + APP_MAX_SIZE);

    /* ==== 3. 注册固件下载接口到 UDS ==== */
    uds_dl_init_fw();

    /* ==== 4. 初始化 UDS 诊断服务 ==== */
    uds_init();
    MAIN_D("  UDS init done\r\n");

    /* ==== 5. 主循环 ==== */
    MAIN_D("  Entering UDS main loop (CAN poll + ISOTP/UDS + FlashDownload + WDT)\r\n");
    last_wdt_feed = tickTimer_GetCount();

    /* 本次 UDS 会话中已完成处理的槽位记录（bit0=APP1, bit1=APP2）：
     * FW_UPDATE_COMPLETE 是粘性状态，用位记录避免主循环重复擦写 flash；
     * 每个槽只处理一次，但 APP1/APP2 相互独立：一次会话里刷两个也能分别清除对应槽。 */
    static uint8_t s_uds_cleared_slots = 0U;

    while (1) {
        uint64_t tick = tickTimer_GetCount();

        /* 500ms WDT 喂狗 */
        if ((tick - last_wdt_feed) >= 500) {
            SWDT_FeedDog();
            last_wdt_feed = tick;
        }

        UdsOta_Poll();

        if (FlashDownload_GetState() == FW_UPDATE_COMPLETE) {
            stc_uds_shared_t state;
            FlashDownloadProgress_t stcProg;
            en_slot_type_t eSlot;
            uint32_t u32SlotBit;

            UdsShared_Read(&state);
            FlashDownload_GetProgress(&stcProg);

            /* 实际下载目标槽（由 0x34 地址映射决定: APP1/APP2） */
            if (stcProg.target_address == APP2_START_ADDR) {
                eSlot = SLOT_APP2;
                u32SlotBit = 2U;
            } else if (stcProg.target_address == APP1_START_ADDR) {
                eSlot = SLOT_APP1;
                u32SlotBit = 1U;
            } else {
                eSlot = SLOT_NONE;
                u32SlotBit = 0U;
            }

            if (u32SlotBit != 0U && ((s_uds_cleared_slots & u32SlotBit) == 0U)) {
                /* 必须显式写 magic：旧记录可能是擦除态(0xFFFFFFFF)，
                 * 若 magic 无效，复位后 APP 的 App_CheckPendingUdsAck
                 * 会因 magic 不匹配直接返回，导致 0x51/0x71 补发失败。 */
                state.magic = UDS_SHARED_MAGIC;
                state.phase = UDS_PHASE_PROGRAMMING_DONE;
                state.result = 1;
                state.target_slot = eSlot;
                UdsShared_Write(&state);
                /* 每次烧录完成，整槽清零实际下载槽的 RMU 记录：
                 * 错误计数 + 原因统计全部擦除为“从未初始化”（flash 状态扇区整扇区擦除），
                 * 其它 APP 的状态扇区不受影响 */
                Rmu_ResetSlotRecord((eSlot == SLOT_APP1) ? RMU_SLOT_APP1 : RMU_SLOT_APP2);
#if (BOOT_OTA_MODE_DEBUG == 0U)
                /* 正式模式: 烧到哪里，就设置跳转到哪里（按实际下载目标地址设置跳转槽） */
                if (eSlot == SLOT_APP2) {
                    Boot_SetRunSlotToAddr(APP2_START_ADDR);
                } else if (eSlot == SLOT_APP1) {
                    Boot_SetRunSlotToAddr(APP1_START_ADDR);
                }
#endif
                s_uds_cleared_slots |= u32SlotBit;
                MAIN_D("  UDS shared updated: phase=PROGRAMMING_DONE, target=0x%08X WDT cleared\r\n",
                       (unsigned int)stcProg.target_address);
            }
        }
    }
}

// ====================================================================
// APP 启动时检查并补发 UDS 挂起响应
// ====================================================================
void App_CheckPendingUdsAck(void)
{
    stc_uds_shared_t state;
    UdsShared_Read(&state);

    MAIN_D("App_CheckPendingUdsAck: magic=0x%08X, pending_sid=0x%02X\r\n",
           (unsigned int)state.magic, (unsigned int)state.pending_sid);

    if (state.magic != UDS_SHARED_MAGIC) {
        MAIN_D("  No pending UDS state, skip\r\n");
        return;
    }

    if (state.pending_sid == 0x11) {
        MAIN_D("  Sending pending 11 01 ACK (51 01)\r\n");
        /* 补发 11 01 的肯定响应 (51 01)
         * ISOTP 单帧格式: PCI=0x04 (4字节数据) + 51 01 00 00, DLC=8 */
        CanMsg_t stcMsg;
        uint8_t au8Data[8] = {0x04, 0x51, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
        uint8_t i;
        stcMsg.u32ID = 0x18DAF103UL;
        stcMsg.u8IDE  = 1U;
        stcMsg.u8RTR  = 0U;
        stcMsg.u8FDF  = 0U;
        stcMsg.u8BRS  = 0U;
        stcMsg.u8DLC  = 8U;
        for (i = 0; i < 8; i++) stcMsg.au8Data[i] = au8Data[i];
        CanIf_Send(&stcMsg);
    }

    

    /* 清除共享区 */
    UdsShared_Clear();
    MAIN_D("  UDS shared state cleared\r\n");
}
