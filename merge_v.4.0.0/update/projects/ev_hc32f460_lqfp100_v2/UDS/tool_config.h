/*******************************************
* 文件名: tool_config.h
* 功能: 升级工装配置宏（版本/超时/重试/CAN ID/步骤码）
* 说明: 工装作为 UDS Tester(客户端) 模拟 TBOX 角色，
*       监听产品心跳 → 比对版本 → 自动刷写 168KB 固件。
*       所有可调参数集中在本文件。
*******************************************/
#ifndef TOOL_CONFIG_H_
#define TOOL_CONFIG_H_

#include "stdint.h"

/***************************** CAN ID 定义 *********************************/

/* UDS 物理寻址 (工装 <-> 产品) */
#define TOOL_CANID_UDS_REQUEST      0x18DA03F1UL    /* 工装 -> 产品 请求 */
#define TOOL_CANID_UDS_RESPONSE     0x18DAF103UL    /* 产品 -> 工装 响应 */

/* 工装/产品 私有帧 */
#define TOOL_CANID_HEARTBEAT        0x18FF1108UL    /* 产品心跳 (工装接收) */
#define TOOL_CANID_STATUS           0x18FF1109UL    /* 工装步骤上报 (工装发送) */

/* 心跳帧版本号所在字节索引 */
#define TOOL_HB_VER_BYTE_IDX        3U

/***************************** 工装固件版本 *********************************/

/* 工装自身固件版本号 (与心跳 byte[TOOL_HB_VER_BYTE_IDX] 比较用) */
#define TOOL_FW_VERSION             3U

/***************************** 本地固件镜像 *********************************/

/* 固件镜像由烧录器预烧在工装内部 Flash，升级时全量发出 */
#define TOOL_FW_STORE_ADDR          0x00044000UL    /* 镜像存储起始地址 */
#define TOOL_FW_SIZE                0x0002A000UL    /* 168KB */

/***************************** 下载参数 *********************************/

/* 0x34 请求下载的目标地址标签 (产品端映射: 0x08018000 -> APP1 0x1A000) */
#define TOOL_DL_ADDR                0x08018000UL

/* 0x36 每块数据长度 (沿用现有 TBOX: 258 字节/块) */
#define TOOL_DL_BLOCK_DATA_SIZE     258U

/* 0x36 块序号规则: 从 1 开始递增, 0xFF 后回绕到 1 (跟随产品端) */
#define TOOL_DL_SEQ_FIRST           1U
#define TOOL_DL_SEQ_WRAP            0xFFU

/***************************** 超时策略 (ms) *********************************/

#define TOOL_TIMEOUT_P2_MS          5000UL   /* 单次 UDS 请求-响应超时 (0x78 除外) */
#define TOOL_TIMEOUT_FC_WAIT_MS     10000UL  /* ISO-TP 流控帧等待超时 N_Bs (isotp 层) */
#define TOOL_TIMEOUT_BOOT_READY_MS  15000UL  /* 31 01 后等待 71 01 FF 00 超时 */
#define TOOL_TIMEOUT_RESET_ACK_MS   15000UL  /* 11 01 后等待 51 01 超时 */

/***************************** 重试策略 *********************************/

#define TOOL_NRC22_RETRY_DELAY_MS   5000UL   /* NRC 0x22 延迟重试间隔 */
#define TOOL_NRC22_RETRY_MAX        3U       /* NRC 0x22 最大重试次数 */

#define TOOL_BLOCK_RETRY_MAX        3U       /* 0x36 单块失败整块重发次数 */

#define TOOL_FLOW_RETRY_MAX         2U       /* 整流程失败后从头重试次数 (总尝试 = 1+2) */
#define TOOL_FLOW_RETRY_DELAY_MS    2000UL   /* 整流程重试前等待时间 */

/***************************** 工装步骤码 (状态上报 byte[0]) *********************/

typedef enum {
    TOOL_STEP_IDLE          = 0x00U,    /* 空闲监听心跳 */
    TOOL_STEP_VERSION_OK    = 0x01U,    /* 版本比对通过, 触发升级 */
    TOOL_STEP_EXT_SESSION   = 0x02U,    /* 10 03 扩展会话 */
    TOOL_STEP_UNLOCK        = 0x03U,    /* 27 安全解锁 */
    TOOL_STEP_JUMP_BOOT     = 0x04U,    /* 31 01 请求进 boot */
    TOOL_STEP_PROG_SESSION  = 0x05U,    /* 10 02 编程会话 */
    TOOL_STEP_DOWNLOAD      = 0x06U,    /* 34/36 下载中 (byte[1]=进度%) */
    TOOL_STEP_TRANSFER_EXIT = 0x07U,    /* 37 退出传输 */
    TOOL_STEP_ECU_RESET     = 0x08U,    /* 11 01 复位等待 */
    TOOL_STEP_DONE          = 0x09U,    /* 升级完成 */
    TOOL_STEP_ERROR         = 0x0FU,    /* 错误停机 (byte[1]=错误码) */
} tool_step_t;

/***************************** 工装错误码 (状态上报 byte[1], step=ERROR) **********/

#define TOOL_ERR_NONE               0x00U   /* 无错误 */
#define TOOL_ERR_P2_TIMEOUT         0x01U   /* UDS 响应超时 */
#define TOOL_ERR_NRC                0x02U   /* 否定响应 (byte[2]=NRC) */
#define TOOL_ERR_ISOTP              0x03U   /* ISO-TP 发送失败 */
#define TOOL_ERR_NRC22_EXHAUSTED    0x04U   /* 0x22 重试耗尽 */
#define TOOL_ERR_SECURITY_DENIED    0x05U   /* 0x33 解锁流程失败 */
#define TOOL_ERR_FLOW_RETRY_EXHAUST 0x06U   /* 整流程重试耗尽 */
#define TOOL_ERR_BOOT_READY_TIMEOUT 0x07U   /* 等 71 01 超时 */
#define TOOL_ERR_RESET_ACK_TIMEOUT  0x08U   /* 等 51 01 超时 */

#endif /* TOOL_CONFIG_H_ */
