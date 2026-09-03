/********************************文件说明*************************************
*文件名: uds_dl_if.h
*作者: AI Assistant
*版本: V1.0.0
*功能简介: UDS 下载抽象接口层
*说明: 隔离 UDS 协议层与具体下载实现（固件升级、配置数据等）
*      通过函数指针表实现多态，UDS层不直接依赖具体实现
*****************************************************************************/
#ifndef UDS_DL_IF_H_
#define UDS_DL_IF_H_

#include <stdint.h>
#include <stdbool.h>

/****************************** 结果码定义 ***********************************/
typedef enum
{
    UDS_DL_OK = 0,              /* 成功 */
    UDS_DL_ADDR_INVALID,        /* 地址无效 */
    UDS_DL_SIZE_TOO_LARGE,      /* 数据过大 */
    UDS_DL_ERASE_FAILED,        /* 擦除失败 */
    UDS_DL_WRITE_FAILED,        /* 写入失败 */
    UDS_DL_VERIFY_FAILED,       /* 校验失败 */
    UDS_DL_SEQUENCE_ERROR,      /* 序列号错误 */
    UDS_DL_BUSY,                /* 忙 */
    UDS_DL_NOT_READY,           /* 未就绪 */
    UDS_DL_CRC_MISMATCH,        /* CRC不匹配 */
    UDS_DL_GENERAL_ERROR        /* 通用错误 */
} uds_dl_result_t;

/****************************** 进度信息 *************************************/
typedef struct
{
    uint32_t total_size;            /* 总大小（字节） */
    uint32_t received_size;         /* 已接收大小（字节） */
    uint32_t target_address;        /* 目标地址 */
    uint8_t  progress_percent;      /* 进度百分比 0-100 */
} uds_dl_progress_t;

/****************************** 下载状态 *************************************/
typedef enum
{
    UDS_DL_STATE_IDLE = 0,          /* 空闲 */
    UDS_DL_STATE_READY,             /* 已就绪，等待数据 */
    UDS_DL_STATE_TRANSFERRING,      /* 数据传输中 */
    UDS_DL_STATE_VERIFYING,         /* 校验中（后台处理） */
    UDS_DL_STATE_COMPLETE,          /* 完成 */
    UDS_DL_STATE_ERROR              /* 错误 */
} uds_dl_state_t;

/****************************** 下载接口函数指针表 ****************************/
typedef struct
{
    /**
     * @brief 初始化下载模块
     * @param config_data 配置数据指针（可为NULL）
     * @param config_len 配置数据长度
     * @return 处理结果
     */
    uds_dl_result_t (*init)(void* config_data, uint16_t config_len);

    /**
     * @brief 请求下载（对应 UDS 0x34）
     * @param address 目标地址
     * @param size 数据总大小
     * @return 处理结果
     */
    uds_dl_result_t (*on_request_download)(uint32_t address, uint32_t size);

    /**
     * @brief 传输数据（对应 UDS 0x36）
     * @param block_sequence_number 块序列号（1-255）
     * @param data 数据指针
     * @param len 数据长度（字节）
     * @return 处理结果
     */
    uds_dl_result_t (*on_transfer_data)(uint8_t block_sequence_number,
	                                         const uint8_t* data,
	                                         uint16_t len);

    /**
     * @brief 请求传输退出（对应 UDS 0x37）
     * @return 处理结果
     */
    uds_dl_result_t (*on_transfer_exit)(void);

    /**
     * @brief 擦除操作（对应 UDS 0x31 RID）
     * @param address 起始地址
     * @param size 擦除大小
     * @return 处理结果
     */
    uds_dl_result_t (*erase)(uint32_t address, uint32_t size);

    /**
     * @brief 计算CRC（对应 UDS 0x31 RID）
     * @param address 起始地址
     * @param size 计算大小
     * @param crc_result 输出CRC结果
     * @return 处理结果
     */
    uds_dl_result_t (*calculate_crc)(uint32_t address, uint32_t size,
                                      uint32_t* crc_result);

    /**
     * @brief 获取当前状态
     * @return 当前状态
     */
    uds_dl_state_t (*get_state)(void);

    /**
     * @brief 获取进度信息
     * @param progress 输出进度信息
     */
    void (*get_progress)(uds_dl_progress_t* progress);

    /**
     * @brief 获取最后一次错误
     * @return 错误码
     */
    uds_dl_result_t (*get_last_error)(void);

    /**
     * @brief 取消当前下载任务
     */
    void (*cancel)(void);

    /**
     * @brief 重置下载模块
     */
    void (*reset)(void);

    /**
     * @brief 后台任务处理（处理耗时操作如Flash擦写）
     * @note 需要在主循环中周期性调用
     */
    void (*task)(void);

    /**
     * @brief 检查是否有待处理的响应（NRC 0x78）
     * @return true=需要返回响应等待
     */
    bool (*is_pending)(void);

    /**
     * @brief 读取 DID 数据
     * @param did 数据标识符
     * @param value 输出值
     * @return true=读取成功
     */
    bool (*read_did)(uint16_t did, uint32_t* value);

} uds_dl_if_t;

/****************************** 全局接口 *************************************/

/**
 * @brief 注册下载接口实现
 * @param iface 接口函数指针表
 * @note 在系统初始化时调用，注册具体的下载实现（如固件升级）
 */
void uds_dl_register(const uds_dl_if_t* iface);

/**
 * @brief 获取当前注册的下载接口
 * @return 接口指针，未注册返回NULL
 */
const uds_dl_if_t* uds_dl_get_if(void);

/**
 * @brief 检查下载接口是否已注册
 * @return true=已注册
 */
bool uds_dl_is_registered(void);

#endif /* UDS_DL_IF_H_ */
