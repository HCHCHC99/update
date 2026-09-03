/*******************************************
* 文件名: upgrade_tool.h
* 功能: 升级工装顶层 — 状态机 + 心跳监听 + 版本比对 + 步骤上报
* 流程: 上电自动监听 0x18FF1108 心跳, 产品版本(byte[3]) < TOOL_FW_VERSION
*       时自动触发 UDS 刷写: 10 03 -> 27 -> 31 01 -> 等 71 -> 10 02 -> 27
*       -> 34 -> 36 x N(258B/块) -> 37 -> 11 01 -> 等 51 01 -> 回到监听
*******************************************/
#ifndef UPGRADE_TOOL_H_
#define UPGRADE_TOOL_H_

/* 初始化工装 (注册 CAN 过滤器, 内部调用 UdsTester_Init) */
void Tool_Init(void);

/* 主循环轮询 (需在 while(1) 中持续调用) */
void Tool_Poll(void);

#endif /* UPGRADE_TOOL_H_ */
