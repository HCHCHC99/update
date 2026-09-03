/*******************************************
* 文件名: uds_tester.h
* 功能: UDS 客户端 (Tester) 层 — 请求构造 + 响应解析 + NRC 策略
* 说明: 工装作为 UDS 客户端，单事务模式 (同一时刻只有一条未完成请求)。
*       - 响应超时: TOOL_TIMEOUT_P2_MS
*       - NRC 0x78: 重置 P2 继续等待 (无上限)
*       - NRC 0x22: 延迟 TOOL_NRC22_RETRY_DELAY_MS 重试, 最多 TOOL_NRC22_RETRY_MAX 次
*       - NRC 0x33: 事务失败并返回 SECURITY 错误, 由状态机执行解锁后重发
*******************************************/
#ifndef UDS_TESTER_H_
#define UDS_TESTER_H_

#include "stdint.h"
#include "Adapter_Can.h"
#include "tool_config.h"

/***************************** 类型定义 ***********************************/

/* 事务状态 */
typedef enum {
    TOOL_TXN_IDLE = 0,      /* 空闲, 可发送新请求 */
    TOOL_TXN_WAIT_RESP,     /* 已发送, 等待响应 */
    TOOL_TXN_WAIT_RETRY,    /* NRC 0x22 延迟重试等待中 */
    TOOL_TXN_OK,            /* 收到肯定响应 (结果待读取) */
    TOOL_TXN_ERR,           /* 事务失败 (结果待读取) */
} tool_txn_state_t;

/* 事务错误码 */
typedef enum {
    TOOL_TXN_ERR_NONE = 0,
    TOOL_TXN_ERR_TIMEOUT,       /* P2 超时 */
    TOOL_TXN_ERR_NRC,           /* 其他 NRC (详见 UdsTester_TxnNrc) */
    TOOL_TXN_ERR_NRC22_RETRY,   /* 0x22 重试耗尽 */
    TOOL_TXN_ERR_SECURITY,      /* NRC 0x33 需先解锁 */
    TOOL_TXN_ERR_ISOTP,         /* ISO-TP 发送失败 */
    TOOL_TXN_ERR_BUSY,          /* 事务未完成时又发新请求 */
} tool_txn_err_t;

/***************************** 公开接口 ***********************************/

/* 初始化 (内部调用 isotp_init, 不注册 CAN 过滤器, 由 upgrade_tool 统一注册) */
void UdsTester_Init(void);

/* 主循环轮询: 1ms 门控, 处理 P2 超时与 0x22 延迟重试 */
void UdsTester_Poll(void);

/* CAN 接收入口: 由 CanIf 过滤器回调调用 (响应 ID 帧) */
void UdsTester_OnCanRx(const CanMsg_t *msg);

/* 发送请求 (拷贝到内部缓冲), 非阻塞; 返回 0=已发出, -1=失败/忙 */
int8_t UdsTester_Request(const uint8_t *req, uint16_t len);

/* ---- 请求构造便捷函数 (内部即调用 UdsTester_Request) ---- */
int8_t UdsTester_ReqSession(uint8_t type);                       /* 10 xx */
int8_t UdsTester_ReqSeed(void);                                  /* 27 01 */
int8_t UdsTester_SendKey(const uint8_t seed[4]);                 /* 27 02 + key */
int8_t UdsTester_ReqRoutineCtrl(uint16_t rid, uint8_t type);     /* 31 [rid] [type] */
int8_t UdsTester_ReqDownload(uint32_t addr, uint32_t size);      /* 34 00 44 addr size */
int8_t UdsTester_ReqTransferData(uint8_t seq, const uint8_t *data, uint16_t len); /* 36 seq data */
int8_t UdsTester_ReqTransferExit(void);                          /* 37 */
int8_t UdsTester_ReqEcuReset(uint8_t type);                      /* 11 xx */

/* ---- 事务结果查询 ---- */
tool_txn_state_t UdsTester_TxnState(void);
tool_txn_err_t   UdsTester_TxnError(void);
uint8_t          UdsTester_TxnNrc(void);                        /* 最近一次 NRC */
/* 肯定响应负载 (不含响应 SID, 如 67 01 [seed] -> {01 s1 s2 s3 s4}) */
const uint8_t*   UdsTester_TxnResp(uint16_t *len);
/* 清空事务回到 IDLE (复位 ISO-TP 发送状态) */
void UdsTester_ResetTxn(void);

/* 取走一条非请求应答的 UDS 报文 (如 boot 补发的 71 01 FF 00 / APP 补发的 51 01)
 * 返回 1=取到, 0=无 */
uint8_t UdsTester_TakeUnsolicited(uint8_t *buf, uint16_t buf_size, uint16_t *len);

#endif /* UDS_TESTER_H_ */
