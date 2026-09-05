/*******************************************
* 文件名: uds_tester.c
* 功能: UDS 客户端 (Tester) 层实现
* 说明: 请求经 ISO-TP 发送到产品 (0x18DA03F1),
*       响应经 ISO-TP 重组后解析 (0x18DAF103)。
*       NRC 策略见 uds_tester.h 头注释。
*******************************************/
#include "uds_tester.h"
#include "isotp_transport.h"
#include "security_access.h"
#include "rtt_log.h"
#include "TickTimer.h"
#include <string.h>

/* 调试打印: 复用 OTA 通道宏 (isotp_transport.h 中定义), 行首带 [秒.毫秒] 时间戳 */
#ifndef TOOL_T
#define TOOL_T(fmt, ...) \
    do { uint64_t _tm = tickTimer_GetCount(); \
         LOG_CH(LOG_CH_MAIN, LOG_LEVEL_INFO, COLOR_GREEN, "TOOL", "[%3u.%03us] " fmt, \
                (unsigned)(_tm / 1000U), (unsigned)(_tm % 1000U), ##__VA_ARGS__); } while (0)
#endif
#ifndef TOOL_W
#define TOOL_W(fmt, ...) \
    do { uint64_t _tm = tickTimer_GetCount(); \
         LOG_CH(LOG_CH_MAIN, LOG_LEVEL_WARN, COLOR_YELLOW,"TOOL", "[%3u.%03us] " fmt, \
                (unsigned)(_tm / 1000U), (unsigned)(_tm % 1000U), ##__VA_ARGS__); } while (0)
#endif
#ifndef TOOL_E
#define TOOL_E(fmt, ...) \
    do { uint64_t _tm = tickTimer_GetCount(); \
         LOG_CH(LOG_CH_MAIN, LOG_LEVEL_ERROR, COLOR_RED,  "TOOL", "[%3u.%03us] " fmt, \
                (unsigned)(_tm / 1000U), (unsigned)(_tm % 1000U), ##__VA_ARGS__); } while (0)
#endif

/***************************** 静态变量 ***********************************/

/* 请求缓冲 (0x36 块: 2 + 258 = 260 字节, isotp 发送期间直接引用本缓冲) */
static uint8_t  s_req_buf[268];
static uint16_t s_req_len = 0;

/* 肯定响应负载 (不含响应 SID) */
static uint8_t  s_resp_data[268];
static uint16_t s_resp_data_len = 0;

/* 非请求应答报文槽 (boot 71 01 FF 00 / APP 51 01) */
static uint8_t  s_unsol_buf[16];
static uint16_t s_unsol_len = 0;
static uint8_t  s_unsol_flag = 0;

/* ISO-TP 重组缓冲 */
static uint8_t  s_rx_assemble_buf[4100];

/* 事务状态 */
static tool_txn_state_t s_state = TOOL_TXN_IDLE;
static tool_txn_err_t   s_err = TOOL_TXN_ERR_NONE;
static uint8_t          s_nrc = 0;

/* 定时/重试 (TickTimer 非阻塞延时模块) */
static NonBlockingDelay_t s_p2_tmr;         /* P2 响应超时 */
static NonBlockingDelay_t s_retry_tmr;      /* 0x22 重试倒计时 */
static uint8_t  s_nrc22_retries = 0;        /* 0x22 已重试次数 */

/***************************** 内部函数 ***********************************/

static void txn_finish(tool_txn_state_t st, tool_txn_err_t err)
{
    s_state = st;
    s_err = err;
}

/*
 * 解析一条重组完成的 UDS 报文 (来自产品响应 ID)
 */
static void handle_uds_message(const uint8_t *buf, uint16_t len)
{
    if (len < 1) {
        return;
    }

    /* ---- 否定响应 ---- */
    if (buf[0] == 0x7F) {
        if (s_state != TOOL_TXN_WAIT_RESP || len < 3 || buf[1] != s_req_buf[0]) {
            /* 非当前事务的否定响应, 按非应答报文丢弃 */
            TOOL_W("NRC resp mismatch (sid=0x%02X), drop", buf[1]);
            return;
        }
        s_nrc = buf[2];
        if (s_nrc == 0x78U) {
            /* responsePending: 重启 P2 继续等待, 无上限 */
            nbDelay_Start(&s_p2_tmr);
            return;
        }
        if (s_nrc == 0x22U) {
            /* conditionsNotCorrect: 延迟重试 */
            s_nrc22_retries++;
            if (s_nrc22_retries <= TOOL_NRC22_RETRY_MAX) {
                TOOL_W("NRC 0x22, retry %d/%d after %d ms",
                       s_nrc22_retries, TOOL_NRC22_RETRY_MAX, (int)TOOL_NRC22_RETRY_DELAY_MS);
                s_state = TOOL_TXN_WAIT_RETRY;
                nbDelay_Start(&s_retry_tmr);
            } else {
                TOOL_E("NRC 0x22 retries exhausted");
                txn_finish(TOOL_TXN_ERR, TOOL_TXN_ERR_NRC22_RETRY);
            }
            return;
        }
        if (s_nrc == 0x33U) {
            /* securityAccessDenied: 交由状态机解锁后重发 */
            TOOL_W("NRC 0x33, security access required");
            txn_finish(TOOL_TXN_ERR, TOOL_TXN_ERR_SECURITY);
            return;
        }
        txn_finish(TOOL_TXN_ERR, TOOL_TXN_ERR_NRC);
        return;
    }

    /* ---- 肯定响应 (SID = 请求SID + 0x40) ---- */
    if ((s_state == TOOL_TXN_WAIT_RESP) &&
        ((buf[0] & 0x7FU) == (uint8_t)(s_req_buf[0] + 0x40U)) && ((buf[0] & 0x40U) != 0U)) {
        uint16_t payload = (uint16_t)(len - 1U);
        if (payload > sizeof(s_resp_data)) {
            payload = sizeof(s_resp_data);
        }
        memcpy(s_resp_data, &buf[1], payload);
        s_resp_data_len = payload;
        txn_finish(TOOL_TXN_OK, TOOL_TXN_ERR_NONE);
        return;
    }

    /* ---- 非当前事务应答 (71 01 FF 00 / 51 01 等) ---- */
    if (s_unsol_flag == 0U) {
        uint16_t n = (len <= sizeof(s_unsol_buf)) ? len : sizeof(s_unsol_buf);
        memcpy(s_unsol_buf, buf, n);
        s_unsol_len = n;
        s_unsol_flag = 1U;
    }
}

/***************************** 公开接口实现 *******************************/

void UdsTester_Init(void)
{
    isotp_init(0);
    s_state = TOOL_TXN_IDLE;
    s_err = TOOL_TXN_ERR_NONE;
    s_req_len = 0;
    s_resp_data_len = 0;
    s_unsol_flag = 0;
    s_nrc22_retries = 0;
    nbDelay_Init(&s_p2_tmr, TOOL_TIMEOUT_P2_MS);
    nbDelay_Init(&s_retry_tmr, TOOL_NRC22_RETRY_DELAY_MS);
    TOOL_T("UDS Tester init done");
}

void UdsTester_Poll(void)
{
    if (s_state == TOOL_TXN_WAIT_RESP) {
        if (isotp_get_tx_state() != ISOTP_TX_IDLE) {
            /* 多帧请求仍在发送中 (FF/FC/CF 未发完): P2 从发完才起算 */
            nbDelay_Start(&s_p2_tmr);
        } else if (nbDelay_IsComplete(&s_p2_tmr)) {
            TOOL_E("P2 timeout (%d ms), SID 0x%02X", (int)TOOL_TIMEOUT_P2_MS, s_req_buf[0]);
            txn_finish(TOOL_TXN_ERR, TOOL_TXN_ERR_TIMEOUT);
        }
    } else if (s_state == TOOL_TXN_WAIT_RETRY) {
        if (nbDelay_IsComplete(&s_retry_tmr)) {
            /* 重发原请求 */
            nbDelay_Start(&s_p2_tmr);
            s_state = TOOL_TXN_WAIT_RESP;
            if (isotp_send_message(0, TOOL_CANID_UDS_REQUEST, s_req_buf, s_req_len) == ISOTP_ERROR) {
                TOOL_E("Retry send failed");
                txn_finish(TOOL_TXN_ERR, TOOL_TXN_ERR_ISOTP);
            }
        }
    }
}

void UdsTester_OnCanRx(const CanMsg_t *msg)
{
    uint16_t out_len = 0;
    int8_t result;

    if (msg == NULL) {
        return;
    }

    result = isotp_receive_frame(0, msg->u32ID, (uint8_t *)msg->au8Data, msg->u8DLC,
                                 s_rx_assemble_buf, &out_len);
    if (result == ISOTP_OK) {
        handle_uds_message(s_rx_assemble_buf, out_len);
    }
}

int8_t UdsTester_Request(const uint8_t *req, uint16_t len)
{
    int8_t ret;

    if (req == NULL || len == 0 || len > sizeof(s_req_buf)) {
        return -1;
    }
    if (s_state == TOOL_TXN_WAIT_RESP || s_state == TOOL_TXN_WAIT_RETRY) {
        TOOL_W("Request busy (state=%d)", s_state);
        return -1;
    }

    memcpy(s_req_buf, req, len);
    s_req_len = len;
    s_nrc22_retries = 0;
    nbDelay_Start(&s_p2_tmr);
    s_resp_data_len = 0;
    s_state = TOOL_TXN_WAIT_RESP;

    ret = isotp_send_message(0, TOOL_CANID_UDS_REQUEST, s_req_buf, s_req_len);
    if (ret == ISOTP_ERROR) {
        TOOL_E("isotp send failed, SID 0x%02X", req[0]);
        s_state = TOOL_TXN_IDLE;
        return -1;
    }
    /* ISOTP_OK(单帧已发) / ISOTP_BUSY(多帧发送中) 均视为已发出 */
    return 0;
}

int8_t UdsTester_ReqSession(uint8_t type)
{
    uint8_t req[2];
    req[0] = 0x10U;
    req[1] = type;
    return UdsTester_Request(req, 2);
}

int8_t UdsTester_ReqSeed(void)
{
    uint8_t req[2];
    req[0] = 0x27U;
    req[1] = 0x01U;
    return UdsTester_Request(req, 2);
}

int8_t UdsTester_SendKey(const uint8_t seed[4])
{
    uint8_t req[6];
    uint8_t key[4];

    if (seed == NULL) {
        return -1;
    }
    if (!seedkey_calc_lv1_key((uint8_t *)seed, key)) {
        return -1;
    }
    req[0] = 0x27U;
    req[1] = 0x02U;
    req[2] = key[0];
    req[3] = key[1];
    req[4] = key[2];
    req[5] = key[3];
    return UdsTester_Request(req, 6);
}

int8_t UdsTester_ReqRoutineCtrl(uint16_t rid, uint8_t type)
{
    uint8_t req[4];
    req[0] = 0x31U;
    req[1] = (uint8_t)(rid >> 8);
    req[2] = (uint8_t)(rid & 0xFFU);
    req[3] = type;
    return UdsTester_Request(req, 4);
}

int8_t UdsTester_ReqDownload(uint32_t addr, uint32_t size)
{
    uint8_t req[11];
    req[0] = 0x34U;
    req[1] = 0x00U;     /* dataFormatIdentifier */
    req[2] = 0x44U;     /* addrLen=4, sizeLen=4 */
    req[3] = (uint8_t)(addr >> 24);
    req[4] = (uint8_t)(addr >> 16);
    req[5] = (uint8_t)(addr >> 8);
    req[6] = (uint8_t)(addr);
    req[7] = (uint8_t)(size >> 24);
    req[8] = (uint8_t)(size >> 16);
    req[9] = (uint8_t)(size >> 8);
    req[10] = (uint8_t)(size);
    return UdsTester_Request(req, 11);
}

int8_t UdsTester_ReqTransferData(uint8_t seq, const uint8_t *data, uint16_t len)
{
    uint8_t req[262];

    if (data == NULL || len > 260U) {
        return -1;
    }
    req[0] = 0x36U;
    req[1] = seq;
    memcpy(&req[2], data, len);
    return UdsTester_Request(req, (uint16_t)(len + 2U));
}

int8_t UdsTester_ReqTransferExit(void)
{
    uint8_t req[1];
    req[0] = 0x37U;
    return UdsTester_Request(req, 1);
}

int8_t UdsTester_ReqEcuReset(uint8_t type)
{
    uint8_t req[2];
    req[0] = 0x11U;
    req[1] = type;
    return UdsTester_Request(req, 2);
}

tool_txn_state_t UdsTester_TxnState(void)
{
    return s_state;
}

tool_txn_err_t UdsTester_TxnError(void)
{
    return s_err;
}

uint8_t UdsTester_TxnNrc(void)
{
    return s_nrc;
}

const uint8_t* UdsTester_TxnResp(uint16_t *len)
{
    if (len != NULL) {
        *len = s_resp_data_len;
    }
    return s_resp_data;
}

void UdsTester_ResetTxn(void)
{
    s_state = TOOL_TXN_IDLE;
    s_err = TOOL_TXN_ERR_NONE;
    isotp_reset_tx();
    isotp_reset_rx();
}

uint8_t UdsTester_TakeUnsolicited(uint8_t *buf, uint16_t buf_size, uint16_t *len)
{
    if (s_unsol_flag == 0U) {
        return 0;
    }
    if (buf != NULL && len != NULL) {
        uint16_t n = (s_unsol_len <= buf_size) ? s_unsol_len : buf_size;
        memcpy(buf, s_unsol_buf, n);
        *len = n;
    }
    s_unsol_flag = 0U;
    return 1;
}
