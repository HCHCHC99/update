/*******************************************
* 文件名: uds_ota.c
* 作者: AI Assistant
* 版本: V1.0.0
* 功能: UDS OTA 应用封装层实现
* 说明: 从 main.c 搬运 ISOTP_RxCallback / ISOTP_RegisterRxFilters /
*       g_delayed_reset_ms / s_uds_rx_buffer，统一管理
*******************************************/
#include "uds_ota.h"
#include "Adapter_Can.h"
#include "isotp_transport.h"
#include "uds_diagnostic.h"
#include "flash_download.h"
#include "rtt_log.h"
#include "TickTimer.h"
#include "main.h"
#include "Bootloader_App.h"

/***************************** 前向声明 ***********************************/
extern void uds_dl_init_fw(void);

/***************************** 全局变量 ***********************************/

/* 延迟复位倒计时：UDS handler 设为 DELAYED_RESET_MS，Poll 中逐 ms 递减 */
volatile uint32_t g_delayed_reset_ms = 0;

/***************************** 静态变量 ***********************************/

/* ISOTP 重组后的 UDS 消息输出缓冲区（最大 4096 字节） */
static uint8_t s_uds_rx_buffer[4100];

/***************************** 内部函数 ***********************************/

/*
 * ISOTP CAN RX 回调 — 由 CanIf 分发层在主循环上下文调用。
 * 将 CAN 帧送入 ISOTP 重组，完成后分发到 UDS 诊断层。
 */
static void ISOTP_RxCallback(const CanMsg_t *pMsg)
{
    uint16_t out_len = 0;
    int8_t result = isotp_receive_frame(0, pMsg->u32ID,
                                        (uint8_t*)pMsg->au8Data, pMsg->u8DLC,
                                        s_uds_rx_buffer, &out_len);
    if (result == ISOTP_OK) {
        uds_receive_handler(0, pMsg->u32ID, s_uds_rx_buffer, out_len);
    }
}

/*
 * 注册 ISOTP 所需的 4 个 CAN ID 到 CanIf 过滤器。
 * ISOTP 过滤列表: 0x18DA03F1, 0x18DAF103, 0x18FF8118, 0x18DBFFF0
 */
static void ISOTP_RegisterRxFilters(void)
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
    stcEntry.pfnCallback = &ISOTP_RxCallback;

    for (uint8_t i = 0U; i < 4U; i++) {
        stcEntry.u32CanId = s_isotp_can_ids[i];
        if (!CanIf_RegisterRxFilter(&stcEntry)) {
            MAIN_D("ISOTP: failed to register RX filter for CAN ID 0x%08X\r\n",
                   s_isotp_can_ids[i]);
        }
    }
    MAIN_D("ISOTP: 4 CAN ID RX filters registered\r\n");
}

/***************************** 公开接口实现 *******************************/

/* 初始化 UDS/CAN/ISOTP 栈 */
void UdsOta_Init(void)
{
    MAIN_D("=== UDS Stack Init Start ===\r\n");

    isotp_init(0);
    ISOTP_RegisterRxFilters();
    FlashDownload_Init(NULL);
    uds_dl_init_fw();
    uds_init();

    MAIN_D("=== UDS Stack Init Done ===\r\n");
}

/* 主循环轮询 */
void UdsOta_Poll(void)
{
    static uint64_t s_last_ms_tick = 0;
    uint64_t current_tick = tickTimer_GetCount();

    /* 1ms 门控 */
    if (current_tick != s_last_ms_tick) {
        s_last_ms_tick = current_tick;

        /* 延迟复位倒计时 */
        if (g_delayed_reset_ms > 0) {
            g_delayed_reset_ms--;
            if (g_delayed_reset_ms == 0) {
                MAIN_D("Delayed reset done, resetting...\r\n");
                NVIC_SystemReset();
                while(1);
            }
        }

        isotp_ms_update();
        uds_ms_update();
        isotp_tx_process();
    }

    FlashDownload_Task();
    CanIf_Poll();
}

/* APP 启动时检查并补发挂起的 UDS 响应 */
void UdsOta_App_CheckPendingAck(void)
{
    App_CheckPendingUdsAck();
}

/* Bootloader 进入 UDS 编程模式 */
void UdsOta_Bootloader_Enter(void)
{
    Bootloader_UdsMain();
}
