/*******************************************
* 文件名: upgrade_tool.c
* 功能: 升级工装顶层状态机实现
* 说明: 状态流转 (含超时/重试策略见 tool_config.h):
*   LISTEN --心跳版本低--> EXT_SESSION -> UNLOCK_APP -> JUMP_BOOT
*     -> WAIT_BOOT(等71 01) -> PROG_SESSION -> UNLOCK_BOOT
*     -> DL_REQ -> TRANSFER(36xN) -> EXIT -> ECU_RESET
*     -> WAIT_5101(等51 01) -> DONE -> LISTEN
*   任意步失败: 整流程从头重试 TOOL_FLOW_RETRY_MAX 次, 仍失败进入 ERROR 停机
*   NRC 0x33: 自动重新执行 27 解锁, 成功后回到失败步骤重发
*******************************************/
#include "upgrade_tool.h"
#include "uds_tester.h"
#include "tool_config.h"
#include "Adapter_Can.h"
#include "isotp_transport.h"
#include "rtt_log.h"
#include "TickTimer.h"
#include <string.h>

/* [TOOL] 行首带 [秒.毫秒] 时间戳 (与 [OTA] 行 time= 同源: tickTimer) */
#ifndef TOOL_T
#define TOOL_T(fmt, ...) \
    do { uint64_t _tm = tickTimer_GetCount(); \
         LOG_CH(LOG_CH_MAIN, LOG_LEVEL_DEBUG, COLOR_CYAN,   "TOOL", "[%3u.%03us] " fmt, \
                (unsigned)(_tm / 1000U), (unsigned)(_tm % 1000U), ##__VA_ARGS__); } while (0)
#endif
#ifndef TOOL_I
#define TOOL_I(fmt, ...) \
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

/***************************** 内部状态 ***********************************/

typedef enum {
    TOOL_ST_LISTEN = 0,     /* 监听心跳, 比对版本 */
    TOOL_ST_EXT_SESSION,    /* 10 03 */
    TOOL_ST_UNLOCK_APP,     /* 27 01/02 (APP 上下文) */
    TOOL_ST_JUMP_BOOT,      /* 31 01 (产品静默复位, 等超时视为正常) */
    TOOL_ST_WAIT_BOOT,      /* 等 boot 补发 71 01 FF 00 */
    TOOL_ST_PROG_SESSION,   /* 10 02 */
    TOOL_ST_UNLOCK_BOOT,    /* 27 01/02 (boot 上下文) */
    TOOL_ST_DL_REQ,         /* 34 */
    TOOL_ST_TRANSFER,       /* 36 x N */
    TOOL_ST_EXIT,           /* 37 */
    TOOL_ST_ECU_RESET,      /* 11 01 (静默复位) */
    TOOL_ST_WAIT_5101,      /* 等 APP 补发 51 01 */
    TOOL_ST_RETRY_DELAY,    /* 整流程重试前延迟 */
    TOOL_ST_ERROR,          /* 错误停机 */
} tool_state_t;

static tool_state_t s_state = TOOL_ST_LISTEN;

/* 请求发送标记: 每次进入状态置 0, 发送后置 1 */
static uint8_t s_req_sent = 0;

/* 安全解锁子步骤: 0=请求种子, 1=发送密钥 */
static uint8_t s_unlock_sub = 0;

/* 安全解锁种子 (27 01 响应) */
static uint8_t s_seed[4];

/* 0x33 恢复: 解锁完成后回到的状态 */
static tool_state_t s_resume_state = TOOL_ST_LISTEN;
static uint8_t s_in_boot = 0;           /* 产品是否已进入 boot 阶段 */

/* 传输进度 */
static uint32_t s_blk_offset = 0;       /* 已发送字节数 */
static uint8_t  s_blk_seq = TOOL_DL_SEQ_FIRST;
static uint8_t  s_blk_retry = 0;
static uint16_t s_blk_len = 0;          /* 当前块长度 */

/* 流程级重试 */
static uint8_t  s_flow_retry = 0;

/* 心跳 */
static volatile uint8_t s_hb_flag = 0;
static volatile uint8_t s_hb_ver = 0;

/* 步骤上报 */
static uint8_t s_rpt_step = 0xFFU;      /* 上次上报值 (0xFF 强制首发) */
static uint8_t s_rpt_param = 0;
static uint8_t s_rpt_detail = 0;

/* 非阻塞延时器 (TickTimer 模块) */
static NonBlockingDelay_t s_flow_retry_tmr;  /* 整流程失败重试延时 */
static NonBlockingDelay_t s_stage_tmr;       /* WAIT_BOOT / WAIT_5101 阶段超时 */
static NonBlockingDelay_t s_poll_tmr;        /* 1ms 轮询门控 */

/***************************** 步骤上报 ***********************************/

/* 发送一帧步骤上报 (0x18FF1109: byte0=步骤, byte1=参数, byte2=细节) */
static void status_send(void)
{
    CanMsg_t msg;

    msg.u32ID = TOOL_CANID_STATUS;
    msg.u8IDE = 1U;
    msg.u8RTR = 0U;
    msg.u8FDF = 0U;
    msg.u8BRS = 0U;
    msg.u8DLC = 8U;
    msg.au8Data[0] = s_rpt_step;
    msg.au8Data[1] = s_rpt_param;
    msg.au8Data[2] = s_rpt_detail;
    msg.au8Data[3] = 0U;
    msg.au8Data[4] = 0U;
    msg.au8Data[5] = 0U;
    msg.au8Data[6] = 0U;
    msg.au8Data[7] = 0U;
    (void)CanIf_Send(&msg);
    OTA_I("[TX] 0x18FF1109, %02X %02X %02X 00 00 00 00 00 <-- Status step=0x%02X param=0x%02X detail=0x%02X",
          s_rpt_step, s_rpt_param, s_rpt_detail, s_rpt_step, s_rpt_param, s_rpt_detail);
}

/* 更新步骤码: 内容变化时发送一次, 不变不重发 */
static void status_report(uint8_t step, uint8_t param, uint8_t detail)
{
    if ((step == s_rpt_step) && (param == s_rpt_param) && (detail == s_rpt_detail)) {
        return;                     /* 与上次上报相同, 不重发 */
    }
    s_rpt_step = step;
    s_rpt_param = param;
    s_rpt_detail = detail;
    status_send();
}

/***************************** 心跳接收 ***********************************/

static void Hb_RxCallback(const CanMsg_t *msg)
{
    if ((msg != NULL) && (msg->u8DLC > TOOL_HB_VER_BYTE_IDX)) {
        /* 心跳帧 1s 一条, 打印量太大且会挤掉 RTT 其他日志, 不打印 */
        s_hb_ver = msg->au8Data[TOOL_HB_VER_BYTE_IDX];
        s_hb_flag = 1U;
    }
}

/***************************** 内部辅助 ***********************************/

/* 状态切换 */
static void goto_state(tool_state_t st)
{
    s_state = st;
    s_req_sent = 0;
    s_unlock_sub = 0;       /* 每次进入新状态都从 27 01 重新开始解锁流程 */

    /* 阶段超时: 按目标状态配置 */
    switch (st) {
        case TOOL_ST_WAIT_BOOT:
            nbDelay_SetTime(&s_stage_tmr, TOOL_TIMEOUT_BOOT_READY_MS);
            nbDelay_Start(&s_stage_tmr);
            break;
        case TOOL_ST_WAIT_5101:
            nbDelay_SetTime(&s_stage_tmr, TOOL_TIMEOUT_RESET_ACK_MS);
            nbDelay_Start(&s_stage_tmr);
            break;
        default:
            nbDelay_Stop(&s_stage_tmr);
            break;
    }
}

/* NRC 0x33 恢复: 回到解锁步骤, 成功后回到 resume_state 重发请求 */
static void security_resume(tool_state_t failed_state)
{
    TOOL_W("Security unlock then resume step %d", (int)failed_state);
    s_resume_state = failed_state;
    s_unlock_sub = 0;
    goto_state(s_in_boot ? TOOL_ST_UNLOCK_BOOT : TOOL_ST_UNLOCK_APP);
}

/* 整流程失败处理: 重试或停机 */
static void flow_fail(uint8_t err_code, uint8_t detail)
{
    UdsTester_ResetTxn();
    s_flow_retry++;
    if (s_flow_retry > TOOL_FLOW_RETRY_MAX) {
        TOOL_E("Flow failed, retries exhausted (err=0x%02X detail=0x%02X), stop", err_code, detail);
        s_state = TOOL_ST_ERROR;
        status_report(TOOL_STEP_ERROR, err_code, detail);
    } else {
        TOOL_W("Flow failed (err=0x%02X detail=0x%02X), retry %d/%d in %d ms",
               err_code, detail, s_flow_retry, TOOL_FLOW_RETRY_MAX, (int)TOOL_FLOW_RETRY_DELAY_MS);
        status_report(TOOL_STEP_ERROR, err_code, detail);
        nbDelay_Start(&s_flow_retry_tmr);
        s_state = TOOL_ST_RETRY_DELAY;
    }
}

/* 检查非应答报文 (71 01 FF 00 / 51 01) */
static uint8_t unsolicited_sid(void)
{
    uint8_t buf[16];
    uint16_t len = 0;

    if (UdsTester_TakeUnsolicited(buf, sizeof(buf), &len) == 0U) {
        return 0U;
    }
    if (len < 1) {
        return 0U;
    }
    return buf[0];
}

/***************************** 状态处理 ***********************************/

static void st_listen(void)
{
    if (s_hb_flag != 0U) {
        s_hb_flag = 0U;
        if (TOOL_FW_VERSION > s_hb_ver) {
            TOOL_I("Upgrade triggered: product ver=0x%02X < tool ver=0x%02X", s_hb_ver, TOOL_FW_VERSION);
            status_report(TOOL_STEP_VERSION_OK, s_hb_ver, 0U);
            s_in_boot = 0;
            /* 注意: s_flow_retry 不在此清零, 保证整流程失败 x2 后真正停机 */
            goto_state(TOOL_ST_EXT_SESSION);
        }
    }
}

/* 安全解锁 (APP / boot 共用) */
static void st_unlock(void)
{
    tool_txn_state_t txn = UdsTester_TxnState();
    const uint8_t *resp;
    uint16_t resp_len = 0;

    if (s_req_sent == 0U) {
        if (s_unlock_sub == 0U) {
            s_req_sent = (UdsTester_ReqSeed() == 0) ? 1U : 0U;
        } else {
            s_req_sent = (UdsTester_SendKey(s_seed) == 0) ? 1U : 0U;
        }
        return;
    }

    if (txn == TOOL_TXN_OK) {
        UdsTester_ResetTxn();
        resp = UdsTester_TxnResp(&resp_len);
        if (s_unlock_sub == 0U) {
            /* 67 01 负载: {01 s1 s2 s3 s4} */
            if (resp_len >= 5U) {
                memcpy(s_seed, &resp[1], 4);
                s_unlock_sub = 1U;
                s_req_sent = 0;
            } else {
                TOOL_E("Seed resp length error: %d", resp_len);
                flow_fail(TOOL_ERR_NRC, 0U);
            }
        } else {
            if (s_state == TOOL_ST_UNLOCK_APP) {
                goto_state(TOOL_ST_JUMP_BOOT);
            } else {
                /* 解锁完成: 回到 0x33 恢复点或正常下一步 */
                if (s_resume_state != TOOL_ST_LISTEN) {
                    tool_state_t resume = s_resume_state;
                    s_resume_state = TOOL_ST_LISTEN;
                    s_req_sent = 0;
                    s_state = resume;
                } else {
                    goto_state(TOOL_ST_DL_REQ);
                }
            }
        }
    } else if (txn == TOOL_TXN_ERR) {
        tool_txn_err_t err = UdsTester_TxnError();
        UdsTester_ResetTxn();
        if (err == TOOL_TXN_ERR_SECURITY) {
            security_resume(s_state);
        } else if (err == TOOL_TXN_ERR_NRC22_RETRY) {
            flow_fail(TOOL_ERR_NRC22_EXHAUSTED, UdsTester_TxnNrc());
        } else if (err == TOOL_TXN_ERR_TIMEOUT) {
            flow_fail(TOOL_ERR_P2_TIMEOUT, 0U);
        } else {
            flow_fail(TOOL_ERR_NRC, UdsTester_TxnNrc());
        }
    }
}

static void st_ext_session(void)
{
    tool_txn_state_t txn = UdsTester_TxnState();

    if (s_req_sent == 0U) {
        status_report(TOOL_STEP_EXT_SESSION, 0U, 0U);
        s_req_sent = (UdsTester_ReqSession(0x03U) == 0) ? 1U : 0U;
        return;
    }

    if (txn == TOOL_TXN_OK) {
        UdsTester_ResetTxn();
        goto_state(TOOL_ST_UNLOCK_APP);
    } else if (txn == TOOL_TXN_ERR) {
        tool_txn_err_t err = UdsTester_TxnError();
        UdsTester_ResetTxn();
        if (err == TOOL_TXN_ERR_SECURITY) {
            security_resume(TOOL_ST_EXT_SESSION);
        } else if (err == TOOL_TXN_ERR_NRC22_RETRY) {
            flow_fail(TOOL_ERR_NRC22_EXHAUSTED, UdsTester_TxnNrc());
        } else if (err == TOOL_TXN_ERR_TIMEOUT) {
            flow_fail(TOOL_ERR_P2_TIMEOUT, 0U);
        } else {
            flow_fail(TOOL_ERR_NRC, UdsTester_TxnNrc());
        }
    }
}

static void st_jump_boot(void)
{
    tool_txn_state_t txn = UdsTester_TxnState();

    if (s_req_sent == 0U) {
        status_report(TOOL_STEP_JUMP_BOOT, 0U, 0U);
        s_req_sent = (UdsTester_ReqRoutineCtrl(0xFF02U, 0x01U) == 0) ? 1U : 0U;
        return;
    }

    if (txn == TOOL_TXN_OK) {
        /* 肯定响应 (71 01 FF 00, 负载在 SID 之后) = boot 已就绪, 直接进入编程会话 */
        UdsTester_ResetTxn();
        s_in_boot = 1U;
        status_report(TOOL_STEP_PROG_SESSION, 0U, 0U);
        goto_state(TOOL_ST_PROG_SESSION);
    } else if (txn == TOOL_TXN_ERR) {
        tool_txn_err_t err = UdsTester_TxnError();
        UdsTester_ResetTxn();
        if (err == TOOL_TXN_ERR_TIMEOUT) {
            /* 预期行为: 产品静默复位 */
            TOOL_I("No resp to 31 01 (expected), product resetting...");
            s_in_boot = 1U;
            goto_state(TOOL_ST_WAIT_BOOT);
        } else if (err == TOOL_TXN_ERR_SECURITY) {
            security_resume(TOOL_ST_JUMP_BOOT);
        } else if (err == TOOL_TXN_ERR_NRC22_RETRY) {
            flow_fail(TOOL_ERR_NRC22_EXHAUSTED, UdsTester_TxnNrc());
        } else {
            flow_fail(TOOL_ERR_NRC, UdsTester_TxnNrc());
        }
    }
}

static void st_wait_boot(void)
{
    uint8_t sid = unsolicited_sid();

    if (sid == 0x71U) {
        status_report(TOOL_STEP_PROG_SESSION, 0U, 0U);
        goto_state(TOOL_ST_PROG_SESSION);
        return;
    }

    /* s_stage_tmr 在 goto_state 进入 WAIT_BOOT 时启动 */
    if (nbDelay_IsComplete(&s_stage_tmr)) {
        flow_fail(TOOL_ERR_BOOT_READY_TIMEOUT, 0U);
    }
}

static void st_prog_session(void)
{
    tool_txn_state_t txn = UdsTester_TxnState();

    if (s_req_sent == 0U) {
        s_req_sent = (UdsTester_ReqSession(0x02U) == 0) ? 1U : 0U;
        return;
    }

    if (txn == TOOL_TXN_OK) {
        UdsTester_ResetTxn();
        goto_state(TOOL_ST_UNLOCK_BOOT);
    } else if (txn == TOOL_TXN_ERR) {
        tool_txn_err_t err = UdsTester_TxnError();
        UdsTester_ResetTxn();
        if (err == TOOL_TXN_ERR_SECURITY) {
            security_resume(TOOL_ST_PROG_SESSION);
        } else if (err == TOOL_TXN_ERR_NRC22_RETRY) {
            flow_fail(TOOL_ERR_NRC22_EXHAUSTED, UdsTester_TxnNrc());
        } else if (err == TOOL_TXN_ERR_TIMEOUT) {
            flow_fail(TOOL_ERR_P2_TIMEOUT, 0U);
        } else {
            flow_fail(TOOL_ERR_NRC, UdsTester_TxnNrc());
        }
    }
}

static void st_dl_req(void)
{
    tool_txn_state_t txn = UdsTester_TxnState();
    const uint8_t *resp;
    uint16_t resp_len = 0;

    if (s_req_sent == 0U) {
        status_report(TOOL_STEP_DOWNLOAD, 0U, 0U);
        s_req_sent = (UdsTester_ReqDownload(TOOL_DL_ADDR, TOOL_FW_SIZE) == 0) ? 1U : 0U;
        return;
    }

    if (txn == TOOL_TXN_OK) {
        UdsTester_ResetTxn();
        resp = UdsTester_TxnResp(&resp_len);
        /* 74 负载: {maxBlockSizeHi, maxBlockSizeLo} */
        if (resp_len >= 2U) {
            TOOL_I("Download accepted, max block size=%d", (int)(((uint16_t)resp[0] << 8) | resp[1]));
        }
        s_blk_offset = 0;
        s_blk_seq = TOOL_DL_SEQ_FIRST;
        s_blk_retry = 0;
        goto_state(TOOL_ST_TRANSFER);
    } else if (txn == TOOL_TXN_ERR) {
        tool_txn_err_t err = UdsTester_TxnError();
        UdsTester_ResetTxn();
        if (err == TOOL_TXN_ERR_SECURITY) {
            security_resume(TOOL_ST_DL_REQ);
        } else if (err == TOOL_TXN_ERR_NRC22_RETRY) {
            flow_fail(TOOL_ERR_NRC22_EXHAUSTED, UdsTester_TxnNrc());
        } else if (err == TOOL_TXN_ERR_TIMEOUT) {
            flow_fail(TOOL_ERR_P2_TIMEOUT, 0U);
        } else {
            flow_fail(TOOL_ERR_NRC, UdsTester_TxnNrc());
        }
    }
}

static void st_transfer(void)
{
    tool_txn_state_t txn = UdsTester_TxnState();
    uint32_t remaining;
    uint8_t percent;

    if (s_req_sent == 0U) {
        remaining = TOOL_FW_SIZE - s_blk_offset;
        s_blk_len = (remaining > TOOL_DL_BLOCK_DATA_SIZE) ? TOOL_DL_BLOCK_DATA_SIZE : (uint16_t)remaining;
        s_req_sent = (UdsTester_ReqTransferData(s_blk_seq,
                                                (const uint8_t *)TOOL_FW_STORE_ADDR + s_blk_offset,
                                                s_blk_len) == 0) ? 1U : 0U;
        return;
    }

    if (txn == TOOL_TXN_OK) {
        UdsTester_ResetTxn();
        s_blk_offset += s_blk_len;
        s_blk_retry = 0;
        percent = (uint8_t)(((uint64_t)s_blk_offset * 100U) / TOOL_FW_SIZE);
        status_report(TOOL_STEP_DOWNLOAD, percent, 0U);

        if (s_blk_offset >= TOOL_FW_SIZE) {
            TOOL_I("All %d bytes transferred", (int)TOOL_FW_SIZE);
            goto_state(TOOL_ST_EXIT);
        } else {
            /* 块序号回绕: 0xFF -> 1 (跟随产品端规则) */
            s_blk_seq = (s_blk_seq >= TOOL_DL_SEQ_WRAP) ? TOOL_DL_SEQ_FIRST : (uint8_t)(s_blk_seq + 1U);
            s_req_sent = 0;
        }
    } else if (txn == TOOL_TXN_ERR) {
        tool_txn_err_t err = UdsTester_TxnError();
        UdsTester_ResetTxn();
        if (err == TOOL_TXN_ERR_SECURITY) {
            security_resume(TOOL_ST_TRANSFER);
        } else if ((err == TOOL_TXN_ERR_TIMEOUT) || (err == TOOL_TXN_ERR_NRC)) {
            s_blk_retry++;
            if (s_blk_retry >= TOOL_BLOCK_RETRY_MAX) {
                TOOL_E("Block 0x%02X failed %d times", s_blk_seq, s_blk_retry);
                flow_fail(TOOL_ERR_NRC, UdsTester_TxnNrc());
            } else {
                TOOL_W("Block 0x%02X failed (err=%d), resend %d/%d",
                       s_blk_seq, err, s_blk_retry, TOOL_BLOCK_RETRY_MAX);
                s_req_sent = 0;
            }
        } else if (err == TOOL_TXN_ERR_NRC22_RETRY) {
            flow_fail(TOOL_ERR_NRC22_EXHAUSTED, UdsTester_TxnNrc());
        } else {
            flow_fail(TOOL_ERR_ISOTP, 0U);
        }
    }
}

static void st_exit(void)
{
    tool_txn_state_t txn = UdsTester_TxnState();

    if (s_req_sent == 0U) {
        status_report(TOOL_STEP_TRANSFER_EXIT, 100U, 0U);
        s_req_sent = (UdsTester_ReqTransferExit() == 0) ? 1U : 0U;
        return;
    }

    if (txn == TOOL_TXN_OK) {
        UdsTester_ResetTxn();
        goto_state(TOOL_ST_ECU_RESET);
    } else if (txn == TOOL_TXN_ERR) {
        tool_txn_err_t err = UdsTester_TxnError();
        UdsTester_ResetTxn();
        if (err == TOOL_TXN_ERR_SECURITY) {
            security_resume(TOOL_ST_EXIT);
        } else if (err == TOOL_TXN_ERR_NRC22_RETRY) {
            flow_fail(TOOL_ERR_NRC22_EXHAUSTED, UdsTester_TxnNrc());
        } else if (err == TOOL_TXN_ERR_TIMEOUT) {
            flow_fail(TOOL_ERR_P2_TIMEOUT, 0U);
        } else {
            flow_fail(TOOL_ERR_NRC, UdsTester_TxnNrc());
        }
    }
}

static void st_ecu_reset(void)
{
    tool_txn_state_t txn = UdsTester_TxnState();

    if (s_req_sent == 0U) {
        status_report(TOOL_STEP_ECU_RESET, 0U, 0U);
        s_req_sent = (UdsTester_ReqEcuReset(0x01U) == 0) ? 1U : 0U;
        return;
    }

    if (txn == TOOL_TXN_OK) {
        /* boot 上下文正常无响应, 收到响应也继续 */
        UdsTester_ResetTxn();
        goto_state(TOOL_ST_WAIT_5101);
    } else if (txn == TOOL_TXN_ERR) {
        tool_txn_err_t err = UdsTester_TxnError();
        UdsTester_ResetTxn();
        if (err == TOOL_TXN_ERR_TIMEOUT) {
            /* 预期行为: 产品静默复位 */
            TOOL_I("No resp to 11 01 (expected), product resetting...");
            goto_state(TOOL_ST_WAIT_5101);
        } else {
            flow_fail(TOOL_ERR_NRC, UdsTester_TxnNrc());
        }
    }
}

static void st_wait_5101(void)
{
    uint8_t sid = unsolicited_sid();

    if (sid == 0x51U) {
        status_report(TOOL_STEP_DONE, 100U, 0U);
        s_flow_retry = 0;
        s_in_boot = 0;
        goto_state(TOOL_ST_LISTEN);
        return;
    }

    /* s_stage_tmr 在 goto_state 进入 WAIT_5101 时启动 */
    if (nbDelay_IsComplete(&s_stage_tmr)) {
        /* 兜底: 若心跳显示版本已更新, 同样视为成功 */
        if ((s_hb_flag != 0U) && (s_hb_ver >= TOOL_FW_VERSION)) {
            s_hb_flag = 0U;
            TOOL_I("51 01 missed but heartbeat ver=0x%02X, upgrade done", s_hb_ver);
            status_report(TOOL_STEP_DONE, 100U, 0U);
            s_flow_retry = 0;
            s_in_boot = 0;
            goto_state(TOOL_ST_LISTEN);
        } else {
            flow_fail(TOOL_ERR_RESET_ACK_TIMEOUT, 0U);
        }
    }
}

static void st_retry_delay(void)
{
    if (nbDelay_IsComplete(&s_flow_retry_tmr)) {
        TOOL_I("Back to heartbeat listening");
        status_report(TOOL_STEP_IDLE, 0U, 0U);
        goto_state(TOOL_ST_LISTEN);
    }
}

/***************************** 公开接口实现 *******************************/

void Tool_Init(void)
{
    CanIf_RxFilterEntry_t entry;

    UdsTester_Init();

    /* UDS 响应 ID -> ISO-TP 重组 + 事务解析 */
    entry.u32CanId = TOOL_CANID_UDS_RESPONSE;
    entry.u32CanMask = 0UL;
    entry.u8Format = CAN_ID_EXT;
    entry.pfnCallback = &UdsTester_OnCanRx;
    (void)CanIf_RegisterRxFilter(&entry);

    /* 产品心跳 ID -> 版本监听 */
    entry.u32CanId = TOOL_CANID_HEARTBEAT;
    entry.pfnCallback = &Hb_RxCallback;
    (void)CanIf_RegisterRxFilter(&entry);

    s_state = TOOL_ST_LISTEN;
    nbDelay_Init(&s_flow_retry_tmr, TOOL_FLOW_RETRY_DELAY_MS);
    nbDelay_Init(&s_stage_tmr, TOOL_TIMEOUT_BOOT_READY_MS);
    nbDelay_Init(&s_poll_tmr, 1U);
    nbDelay_Start(&s_poll_tmr);
    status_report(TOOL_STEP_IDLE, 0U, 0U);
    TOOL_I("=== Upgrade Tool ready, TOOL_FW_VERSION=0x%02X ===", TOOL_FW_VERSION);
}

void Tool_Poll(void)
{
    CanIf_Poll();
    UdsTester_Poll();

    /* 1ms 门控任务 */
    if (nbDelay_IsComplete_noclose(&s_poll_tmr)) {
        nbDelay_Start(&s_poll_tmr);
        isotp_ms_update();
        isotp_tx_process();
    }

    switch (s_state) {
        case TOOL_ST_LISTEN:       st_listen();       break;
        case TOOL_ST_EXT_SESSION:  st_ext_session();  break;
        case TOOL_ST_UNLOCK_APP:   st_unlock();       break;
        case TOOL_ST_JUMP_BOOT:    st_jump_boot();    break;
        case TOOL_ST_WAIT_BOOT:    st_wait_boot();    break;
        case TOOL_ST_PROG_SESSION: st_prog_session(); break;
        case TOOL_ST_UNLOCK_BOOT:  st_unlock();       break;
        case TOOL_ST_DL_REQ:       st_dl_req();       break;
        case TOOL_ST_TRANSFER:     st_transfer();     break;
        case TOOL_ST_EXIT:         st_exit();         break;
        case TOOL_ST_ECU_RESET:    st_ecu_reset();    break;
        case TOOL_ST_WAIT_5101:    st_wait_5101();    break;
        case TOOL_ST_RETRY_DELAY:  st_retry_delay();  break;
        case TOOL_ST_ERROR:        /* 停机, 保持最后上报值 */ break;
        default:                   goto_state(TOOL_ST_LISTEN); break;
    }
}
