/*******************************************
* 文件名: uds_ota.h
* 作者: AI Assistant
* 版本: V1.0.0
* 功能: UDS OTA 应用封装层 — 统一 boot/app 的 UDS 初始化、轮询、阶段调度
* 说明: 封装 ISOTP / UDS / FlashDownload / CAN 滤波器的 init 和 poll，
*       消除 main.c 中的重复代码
*******************************************/
#ifndef UDS_OTA_H_
#define UDS_OTA_H_

#include "stdint.h"

/***************************** 全局变量 ***********************************/

/* 延迟复位倒计时 (ms)，UDS handler 设置为 DELAYED_RESET_MS，UdsOta_Poll 中倒计时 */
extern volatile uint32_t g_delayed_reset_ms;

/***************************** 公开接口 ***********************************/

/* 初始化 UDS/CAN/ISOTP 栈（所有固件通用） */
void UdsOta_Init(void);

/* 主循环轮询：1ms 门控 + 延迟复位 + ISOTP/UDS 超时 + FlashDownload + CAN 接收 */
void UdsOta_Poll(void);

/* APP 启动时检查并补发挂起的 UDS 响应 (Phase 3: 51 01) */
void UdsOta_App_CheckPendingAck(void);

/* Bootloader 进入 UDS 编程模式 (Phase 2: 31 ACK + 下载循环)，不返回 */
void UdsOta_Bootloader_Enter(void);

#endif /* UDS_OTA_H_ */
