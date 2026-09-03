/**
 * @file    Adapter_Can.c
 * @brief   CAN适配层实现 - 基于四驱控制器 can_module (支持TX中断)
 * @note    实现 Adapter_Can.h 接口，内部调用 can_module API
 *          使用 TX 完成中断驱动发送队列
 */

#include "Adapter_Can.h"
#include "can_module.h"
#include "TickTimer.h"
#include "rtt_log.h"
#include <string.h>
#include <stdlib.h>

/*==============================================================================
 * 调试宏
 *============================================================================*/
#define CANIF_D(fmt, ...)  LOG_CH(LOG_CH_MAIN, LOG_LEVEL_DEBUG, COLOR_CYAN,   "CANIF", fmt, ##__VA_ARGS__)
#define CANIF_I(fmt, ...)  LOG_CH(LOG_CH_MAIN, LOG_LEVEL_INFO,  COLOR_GREEN, "CANIF", fmt, ##__VA_ARGS__)
#define CANIF_W(fmt, ...)  LOG_CH(LOG_CH_MAIN, LOG_LEVEL_WARN,  COLOR_YELLOW,"CANIF", fmt, ##__VA_ARGS__)
#define CANIF_E(fmt, ...)  LOG_CH(LOG_CH_MAIN, LOG_LEVEL_ERROR, COLOR_RED,   "CANIF", fmt, ##__VA_ARGS__)

/*==============================================================================
 * 本地定义
 *============================================================================*/

/* 回显功能：1=开启（未匹配的CAN帧回发），0=关闭 */
#define CANIF_ECHO_ENABLE           (0U)

/* TX队列大小 (必须是2的幂) */
#define CANIF_TX_QUEUE_SIZE         (32U)
#define CANIF_TX_QUEUE_MASK         (CANIF_TX_QUEUE_SIZE - 1U)

/* RX过滤器最大数量 */
#define CANIF_MAX_RX_FILTERS        (16U)

/* Bus-Off恢复等待时间 (ms) */
#define CANIF_BUSOFF_RECOVERY_MS    (500U)

/* CAN硬件实例 (固定使用CAN1) */
#define CANIF_CAN_INSTANCE          (CM_CAN)

/*==============================================================================
 * 本地变量
 *============================================================================*/

/* ---------- TX队列 ---------- */
static CanMsg_t m_astcTxQueue[CANIF_TX_QUEUE_SIZE];
static volatile uint8_t m_u8TxHead = 0U;
static volatile uint8_t m_u8TxTail = 0U;

/* ---------- RX过滤器表 ---------- */
static CanIf_RxFilterEntry_t m_astcRxFilters[CANIF_MAX_RX_FILTERS];
static uint8_t m_u8RxFilterCount = 0U;

/* 默认RX回调 (未匹配帧) */
static void (*m_pfnDefaultRxCallback)(const CanMsg_t *pMsg) = NULL;

/* ---------- can_module 句柄和缓存 ---------- */
static can_handle_t m_stcCanHandle;

/* ---------- Bus-Off状态 ---------- */
static NonBlockingDelay_t m_stcBusOffTimer;
static volatile bool m_bBusOff = false;

/* ---------- 初始化标志 ---------- */
static bool m_bInitialized = false;

/*==============================================================================
 * 本地函数原型
 *============================================================================*/

static void CanIf_ConvertToCanFrame(const CanMsg_t *pSrc, can_frame_t *pDst);
static void CanIf_ConvertFromCanFrame(const stc_can_rx_frame_t *pSrc, CanMsg_t *pDst);
static bool CanIf_MatchFilter(const CanMsg_t *pMsg, const CanIf_RxFilterEntry_t *pFilter);
static void CanIf_DispatchRx(const CanMsg_t *pMsg);
static void CanIf_TxCompleteCallback(void);
static void CanIf_CheckBusOff(void);

/* 默认回显回调 */
static void CanIf_EchoCallback(const CanMsg_t *pMsg);

/*==============================================================================
 * 本地函数实现
 *============================================================================*/

/**
 * @brief TX完成回调 (由 can_module 在中断中调用)
 * @note  在中断上下文中执行，必须快速处理
 *        从SW队列取下一帧发送
 */
static void CanIf_TxCompleteCallback(void)
{
    CanMsg_t stcMsg;
    can_frame_t stcFrame;
    bool bHasFrame = false;
    int8_t send_ret;
    
    /* 从队列取一帧 */
    if (m_u8TxHead != m_u8TxTail) {
        stcMsg = m_astcTxQueue[m_u8TxTail];
        m_u8TxTail = (m_u8TxTail + 1U) & CANIF_TX_QUEUE_MASK;
        bHasFrame = true;
    }
    
    if (!bHasFrame) {
        return;
    }
    
    /* 转换为 can_module 格式并发送 */
    CanIf_ConvertToCanFrame(&stcMsg, &stcFrame);
    
    if (stcFrame.ide == IDE_EXD) {
        send_ret = can_transmit_ext(stcFrame.id, stcFrame.data, stcFrame.len);
    } else {
        send_ret = can_transmit_std(stcFrame.id, stcFrame.data, stcFrame.len);
    }
    
    if (send_ret != CAN_RET_OK) {
        /* 发送失败，放回队列头部 (理论上不应该发生) */
        m_u8TxTail = (m_u8TxTail - 1U) & CANIF_TX_QUEUE_MASK;
        CANIF_W("TX callback: send failed, frame returned to queue");
    }
}

/**
 * @brief 将 CanMsg_t 转换为 can_module 的 can_frame_t
 */
static void CanIf_ConvertToCanFrame(const CanMsg_t *pSrc, can_frame_t *pDst)
{
    if (pSrc == NULL || pDst == NULL) {
        return;
    }
    
    pDst->id = pSrc->u32ID;
    pDst->len = pSrc->u8DLC;
    pDst->rtr = pSrc->u8RTR ? RTR_REMOTE_FRAME : RTR_DATA_FRAME;
    pDst->ide = pSrc->u8IDE ? IDE_EXD : IDE_STD;
    memcpy(pDst->data, pSrc->au8Data, 8);
}

/**
 * @brief 将 can_module 的 stc_can_rx_frame_t 转换为 CanMsg_t
 */
static void CanIf_ConvertFromCanFrame(const stc_can_rx_frame_t *pSrc, CanMsg_t *pDst)
{
    if (pSrc == NULL || pDst == NULL) {
        return;
    }
    
    pDst->u32ID = pSrc->u32ID;
    pDst->u8IDE = pSrc->IDE;
    pDst->u8RTR = pSrc->RTR;
    pDst->u8FDF = 0U;
    pDst->u8BRS = 0U;
    pDst->u8DLC = pSrc->DLC;
    memcpy(pDst->au8Data, pSrc->au8Data, 8);
    pDst->u32Timestamp = (uint32_t)tickTimer_GetCount();
}

/**
 * @brief 检查报文是否匹配过滤器
 * @return true=匹配, false=不匹配
 */
static bool CanIf_MatchFilter(const CanMsg_t *pMsg, const CanIf_RxFilterEntry_t *pFilter)
{
    uint32_t u32EffectiveMask;
    
    if (pMsg == NULL || pFilter == NULL) {
        return false;
    }
    
    /* 格式检查 */
    if (pFilter->u8Format == CAN_ID_STD && pMsg->u8IDE != 0U) {
        return false;
    }
    if (pFilter->u8Format == CAN_ID_EXT && pMsg->u8IDE == 0U) {
        return false;
    }
    
    /* ID匹配: mask bit=1 表示不关心 (忽略该位) */
    u32EffectiveMask = ~(pFilter->u32CanMask);
    if ((pMsg->u32ID & u32EffectiveMask) != (pFilter->u32CanId & u32EffectiveMask)) {
        return false;
    }
    
    return true;
}

/**
 * @brief 分发接收帧到匹配的回调函数
 * @note  所有匹配的过滤器回调都会被调用 (支持多路分发)
 */
static void CanIf_DispatchRx(const CanMsg_t *pMsg)
{
    uint8_t i;
    bool bMatched = false;
    
    if (pMsg == NULL) {
        return;
    }
    
    /* 遍历过滤器表，调用所有匹配的回调 */
    for (i = 0U; i < m_u8RxFilterCount; i++) {
        if (CanIf_MatchFilter(pMsg, &m_astcRxFilters[i])) {
            if (m_astcRxFilters[i].pfnCallback != NULL) {
                m_astcRxFilters[i].pfnCallback(pMsg);
            }
            bMatched = true;
        }
    }
    
    /* 无匹配 → 默认回调 */
    if (!bMatched && m_pfnDefaultRxCallback != NULL) {
        m_pfnDefaultRxCallback(pMsg);
    }
}

/**
 * @brief Bus-Off状态检测和恢复
 */
static void CanIf_CheckBusOff(void)
{
    uint32_t u32Status;
    
    u32Status = CAN_GetStatusValue(CANIF_CAN_INSTANCE);
    
    if ((u32Status & CAN_FLAG_BUS_OFF) != 0U) {
        if (!m_bBusOff) {
            CANIF_W("Bus-Off detected! Starting recovery timer...");
            m_bBusOff = true;
            nbDelay_Start(&m_stcBusOffTimer);
        }
        
        if (nbDelay_IsComplete(&m_stcBusOffTimer)) {
            CANIF_I("Bus-Off recovery: exiting local reset");
            CAN_ExitLocalReset(CANIF_CAN_INSTANCE);
            m_bBusOff = false;
        }
    } else {
        if (m_bBusOff) {
            CANIF_I("Bus-Off recovered");
            m_bBusOff = false;
        }
    }
}

/**
 * @brief 默认回显回调
 */
static void CanIf_EchoCallback(const CanMsg_t *pMsg)
{
    (void)CanIf_Send(pMsg);
}

/*==============================================================================
 * 公共API实现
 *============================================================================*/

/**
 * @brief 初始化CAN适配层
 */
void CanIf_Init(void)
{
    CANIF_I("=== CanIf Init Start ===");
    
    /* ---------- 1. 配置 can_module 参数 ---------- */
    static can_cfg_t stcCanCfg = {
        .can_ins = CAN1,
        .CANx = CM_CAN,
        .en_can_tx = CAN_FUNC_ENABLE,
        .en_can_rx = CAN_FUNC_ENABLE,
        
        /* GPIO: PB14=RX, PB15=TX */
        .gpio_rx = {GPIO_PORT_B, GPIO_PIN_14, GPIO_FUNC_51},
        .gpio_tx = {GPIO_PORT_B, GPIO_PIN_15, GPIO_FUNC_50},
        
        /* 波特率: 250kbps */
        .can_bdr = CAN_BDR_250K,
        .work_mode = CAN_WORK_MD_NORMAL,
        
        /* TX配置 - 参考四驱控制器 can_hw.c */
        .can_tx_cfg = {
            .en_ptb_single_shot = CAN_PTB_SINGLESHOT_TX_ENABLE,
            .en_stb_single_shot = CAN_STB_SINGLESHOT_TX_DISABLE,
            .en_stb_prio_md = CAN_STB_PRIO_MD_DISABLE,
        },
        
        /* RX配置 - 参考四驱控制器 can_hw.c */
        .can_rx_cfg = {
            .rx_warn_lmt = 8U,
            .err_warn_lmt = 10U,
            .rx_all_frame = CAN_RX_ALL_FRAME_DISABLE,
            .rx_ovf_mode = CAN_RX_OVF_SAVE_NEW,
            .self_ack = CAN_SELF_ACK_ENABLE,
        },
        
        /* 中断配置 - 使用 can_module_irq_handler */
        .can_int_type = (CAN_INT_RX | CAN_INT_PTB_TX | CAN_INT_RX_OVERRUN | 
                         CAN_INT_RX_BUF_FULL | CAN_INT_RX_BUF_WARN | CAN_INT_ERR_INT),
        .can_int = {
            .can_int_irqn = INT002_IRQn,
            .can_int_pri = DDL_IRQ_PRIO_07,
            .can_int_callback = can_module_irq_handler,  /* ← 使用 can_module 的中断处理函数 */
        },
        
        .en_can_filte = FILTER_DISABLE,
        .can_filter = {
            .id = 0UL,
            .id_mask = 0UL,
            .id_type = CAN_ID_STD_EXT,
        },
    };
    
    /* ---------- 2. 初始化 can_module ---------- */
    if (can_module_init(&m_stcCanHandle, &stcCanCfg) != CAN_RET_OK) {
        CANIF_E("can_module_init failed!");
        return;
    }
    CANIF_D("can_module initialized");
    
    /* ---------- 3. 注册 TX 完成回调 ---------- */
    can_register_tx_callback(&CanIf_TxCompleteCallback);
    CANIF_D("TX callback registered");
    
    /* ---------- 4. 初始化RX缓存 ---------- */
    
    /* ---------- 5. 初始化TX队列 ---------- */
    memset(m_astcTxQueue, 0, sizeof(m_astcTxQueue));
    m_u8TxHead = 0U;
    m_u8TxTail = 0U;
    
    /* ---------- 6. 初始化过滤器表 ---------- */
    memset(m_astcRxFilters, 0, sizeof(m_astcRxFilters));
    m_u8RxFilterCount = 0U;
    m_pfnDefaultRxCallback = NULL;
    
    /* ---------- 7. 初始化Bus-Off定时器 ---------- */
    nbDelay_Init(&m_stcBusOffTimer, CANIF_BUSOFF_RECOVERY_MS);
    m_bBusOff = false;
    
    /* ---------- 8. 设置回显功能 ---------- */
#if CANIF_ECHO_ENABLE
    CanIf_SetDefaultRxCallback(&CanIf_EchoCallback);
#else
    CanIf_SetDefaultRxCallback(NULL);
#endif
    
    /* ---------- 9. 设置初始化标志 ---------- */
    m_bInitialized = true;
    
    CANIF_I("=== CanIf Init Done ===");
}

/**
 * @brief 发送CAN消息 (非阻塞)
 */
bool CanIf_Send(const CanMsg_t *pMsg)
{
    can_frame_t stcFrame;
    int8_t send_ret;
    bool bResult = false;
    
    if (!m_bInitialized || pMsg == NULL) {
        return false;
    }
    
    if (pMsg->u8DLC > 8U) {
        CANIF_E("Invalid DLC: %d", pMsg->u8DLC);
        return false;
    }
    
    /* 转换为 can_module 格式 */
    CanIf_ConvertToCanFrame(pMsg, &stcFrame);
    
    __disable_irq();
    
    /* 尝试直接硬件发送 (如果TX空闲) */
    if (!can_is_tx_busy()) {
        if (stcFrame.ide == IDE_EXD) {
            send_ret = can_transmit_ext(stcFrame.id, stcFrame.data, stcFrame.len);
        } else {
            send_ret = can_transmit_std(stcFrame.id, stcFrame.data, stcFrame.len);
        }
        
        if (send_ret == CAN_RET_OK) {
            __enable_irq();
            CANIF_D("TX direct: ID=0x%08X, len=%d", stcFrame.id, stcFrame.len);
            return true;
        }
    }
    
    /* 硬件忙 → 入队 */
    {
        uint8_t u8Next = (m_u8TxHead + 1U) & CANIF_TX_QUEUE_MASK;
        if (u8Next != m_u8TxTail) {
            m_astcTxQueue[m_u8TxHead] = *pMsg;
            m_u8TxHead = u8Next;
            bResult = true;
            CANIF_D("TX queued: ID=0x%08X, queue=%d", pMsg->u32ID, 
                     (m_u8TxHead - m_u8TxTail) & CANIF_TX_QUEUE_MASK);
        } else {
            CANIF_W("TX queue full! ID=0x%08X dropped", pMsg->u32ID);
        }
    }
    
    __enable_irq();
    return bResult;
}

/**
 * @brief 主循环轮询
 */
void CanIf_Poll(void)
{
    stc_can_rx_frame_t stcRxFrame;
    CanMsg_t stcMsg;
    
    if (!m_bInitialized) {
        return;
    }
    
    /* ========== 1. ========== */
    while (can_read(&m_stcCanHandle.can_rx, &stcRxFrame) == CAN_RET_OK) {
        CanIf_ConvertFromCanFrame(&stcRxFrame, &stcMsg);
        CanIf_DispatchRx(&stcMsg);
    }
    
    /* ========== 2. Bus-Off检测和恢复 ========== */
    CanIf_CheckBusOff();
    
    /* ========== 3. 安全网: 如果TX空闲但队列非空，手动触发发送 ========== */
    /* 防止中断回调在某些边缘情况下遗漏 */
    if (!can_is_tx_busy() && (m_u8TxHead != m_u8TxTail)) {
        CanIf_TxCompleteCallback();
    }
}

/**
 * @brief 注册RX过滤器
 */
bool CanIf_RegisterRxFilter(const CanIf_RxFilterEntry_t *pEntry)
{
    if (!m_bInitialized || pEntry == NULL) {
        return false;
    }
    
    if (pEntry->pfnCallback == NULL) {
        CANIF_W("Register filter: callback is NULL");
        return false;
    }
    
    if (m_u8RxFilterCount >= CANIF_MAX_RX_FILTERS) {
        CANIF_W("Register filter: table full (max=%d)", CANIF_MAX_RX_FILTERS);
        return false;
    }
    
    m_astcRxFilters[m_u8RxFilterCount] = *pEntry;
    m_u8RxFilterCount++;
    
    CANIF_D("Filter registered: ID=0x%08X, Mask=0x%08X, count=%d",
            pEntry->u32CanId, pEntry->u32CanMask, m_u8RxFilterCount);
    
    return true;
}

/**
 * @brief 设置默认RX回调
 */
void CanIf_SetDefaultRxCallback(void (*pfnCallback)(const CanMsg_t *pMsg))
{
    m_pfnDefaultRxCallback = pfnCallback;
    CANIF_D("Default callback set: %s", pfnCallback ? "YES" : "NULL");
}

/**
 * @brief 获取TX队列待发送帧数
 */
uint8_t CanIf_GetTxQueueCount(void)
{
    uint8_t u8Count;
    
    if (!m_bInitialized) {
        return 0U;
    }
    
    __disable_irq();
    u8Count = (m_u8TxHead - m_u8TxTail) & CANIF_TX_QUEUE_MASK;
    __enable_irq();
    
    return u8Count;
}
