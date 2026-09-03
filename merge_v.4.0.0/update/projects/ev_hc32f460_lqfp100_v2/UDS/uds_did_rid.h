/********************************文件说明*************************************
*文件名: uds_did_rid.h
*作者: AI Assistant
*版本: V1.0.0
*功能简介: UDS DID/RID 宏定义
*说明: 集中管理所有 DID（数据标识符）和 RID（例程标识符）定义
*      供 UDS 协议层和底层下载实现共同引用
*****************************************************************************/
#ifndef UDS_DID_RID_H_
#define UDS_DID_RID_H_

/***************************** DID 定义 **************************************/
/* 数据标识符（2字节，高字节在前） */
#define DID_FIRMWARE_VERSION        0xF000  /* 固件版本号 */
#define DID_BOOTLOADER_VERSION      0xF001  /* Bootloader版本号 */
#define DID_FIRMWARE_CRC            0xF002  /* 固件CRC校验值 */

/***************************** RID 定义 **************************************/
/* 例程控制标识符（2字节，高字节在前） */
#define RID_ERASE_FIRMWARE          0xFF00  /* 擦除固件 */
#define RID_CALCULATE_CRC           0xFE00  /* 计算CRC (TBOX约定) */
#define RID_JUMP_TO_BOOTLOADER      0xFF02  /* 跳转到Bootloader */
#define RID_JUMP_TO_APPLICATION     0xFF03  /* 跳转到应用程序 */

#endif /* UDS_DID_RID_H_ */
