/*******************************************
* 文件名: uds_dl_bridge.c
* 作者: AI Assistant
* 版本: V1.0.0
* 功能简介: UDS 下载抽象接口层实现（桥接层）
* 说明: 桥接 UDS 协议层与具体下载实现（当前为固件升级）
*      通过注册机制实现多态，UDS层不直接依赖具体实现
*****************************************************************************/
#include "uds_dl_if.h"
#include "flash_download.h"
#include "rtt_log.h"
#include <string.h>

/*****************************调试宏定义***************************************/
#define DL_D(fmt, ...)  LOG_CH(LOG_CH_MAIN, LOG_LEVEL_DEBUG, COLOR_CYAN,   "DL", fmt, ##__VA_ARGS__)
#define DL_I(fmt, ...)  LOG_CH(LOG_CH_MAIN, LOG_LEVEL_INFO,  COLOR_GREEN, "DL", fmt, ##__VA_ARGS__)
#define DL_W(fmt, ...)  LOG_CH(LOG_CH_MAIN, LOG_LEVEL_WARN,  COLOR_YELLOW,"DL", fmt, ##__VA_ARGS__)
#define DL_E(fmt, ...)  LOG_CH(LOG_CH_MAIN, LOG_LEVEL_ERROR, COLOR_RED,   "DL", fmt, ##__VA_ARGS__)

/*****************************私有变量***************************************/
static const uds_dl_if_t* g_dl_iface = NULL;

/*****************************结果码转换表************************************/
/* 将 FlashDownloadResult_t 转换为 uds_dl_result_t */
static uds_dl_result_t dl_convert_fw_result(FlashDownloadResult_t fw_result)
{
    switch (fw_result)
    {
        case FW_RESULT_OK:              return UDS_DL_OK;
        case FW_RESULT_ADDR_INVALID:    return UDS_DL_ADDR_INVALID;
        case FW_RESULT_SIZE_TOO_LARGE:  return UDS_DL_SIZE_TOO_LARGE;
        case FW_RESULT_ERASE_FAILED:    return UDS_DL_ERASE_FAILED;
        case FW_RESULT_WRITE_FAILED:    return UDS_DL_WRITE_FAILED;
        case FW_RESULT_VERIFY_FAILED:   return UDS_DL_VERIFY_FAILED;
        case FW_RESULT_SEQUENCE_ERROR:  return UDS_DL_SEQUENCE_ERROR;
        case FW_RESULT_BUSY:            return UDS_DL_BUSY;
        case FW_RESULT_NOT_READY:       return UDS_DL_NOT_READY;
        default:                        return UDS_DL_GENERAL_ERROR;
    }
}

/* 将 FlashDownloadState_t 转换为 uds_dl_state_t */
static uds_dl_state_t dl_convert_fw_state(FlashDownloadState_t fw_state)
{
    switch (fw_state)
    {
        case FW_UPDATE_IDLE:            return UDS_DL_STATE_IDLE;
        case FW_UPDATE_READY:           return UDS_DL_STATE_READY;
        case FW_UPDATE_TRANSFERRING:    return UDS_DL_STATE_TRANSFERRING;
        case FW_UPDATE_VERIFYING:       return UDS_DL_STATE_VERIFYING;
        case FW_UPDATE_COMPLETE:        return UDS_DL_STATE_COMPLETE;
        case FW_UPDATE_ERROR:           return UDS_DL_STATE_ERROR;
        default:                        return UDS_DL_STATE_IDLE;
    }
}

/*****************************固件升级接口实现********************************/

static uds_dl_result_t dl_fw_init(void* config_data, uint16_t config_len)
{
    (void)config_data;
    (void)config_len;
    
    FlashDownload_Init(NULL);
    return UDS_DL_OK;
}

static uds_dl_result_t dl_fw_on_request_download(uint32_t address, uint32_t size)
{
    return dl_convert_fw_result(FlashDownload_OnRequestDownload(address, size));
}

static uds_dl_result_t dl_fw_on_transfer_data(uint8_t block_sequence_number,
                                                const uint8_t* data,
                                                uint16_t len)
{
    /* flash_download.h 接口需要非 const 指针，做强制转换 */
    return dl_convert_fw_result(
        FlashDownload_OnTransferData(block_sequence_number,
                                      (uint8_t*)data,
                                      len));
}

static uds_dl_result_t dl_fw_on_transfer_exit(void)
{
    return dl_convert_fw_result(FlashDownload_OnTransferExit());
}

static uds_dl_result_t dl_fw_erase(uint32_t address, uint32_t size)
{
    return dl_convert_fw_result(FlashDownload_Erase(address, size));
}

static uds_dl_result_t dl_fw_calculate_crc(uint32_t address, uint32_t size,
                                             uint32_t* crc_result)
{
    return dl_convert_fw_result(
        FlashDownload_CalculateCRC(address, size, crc_result));
}

static uds_dl_state_t dl_fw_get_state(void)
{
    return dl_convert_fw_state(FlashDownload_GetState());
}

static void dl_fw_get_progress(uds_dl_progress_t* progress)
{
    FlashDownloadProgress_t fw_progress;
    FlashDownload_GetProgress(&fw_progress);
    
    if (progress != NULL)
    {
        progress->total_size = fw_progress.total_size;
        progress->received_size = fw_progress.received_size;
        progress->target_address = fw_progress.target_address;
        progress->progress_percent = fw_progress.progress_percent;
    }
}

static uds_dl_result_t dl_fw_get_last_error(void)
{
    return dl_convert_fw_result(FlashDownload_GetLastError());
}

static void dl_fw_cancel(void)
{
    FlashDownload_Cancel();
}

static void dl_fw_reset(void)
{
    FlashDownload_Reset();
}

static void dl_fw_task(void)
{
    FlashDownload_Task();
}

static bool dl_fw_is_pending(void)
{
    return FlashDownload_IsPending();
}

static bool dl_fw_read_did(uint16_t did, uint32_t* value)
{
    if (value == NULL) return false;
    
    switch (did)
    {
        case 0xF000: /* DID_FIRMWARE_VERSION */
            *value = FlashDownload_GetFirmwareVersion();
            return true;
            
        case 0xF001: /* DID_BOOTLOADER_VERSION */
            *value = FlashDownload_GetBootloaderVersion();
            return true;
            
        case 0xF002: /* DID_FIRMWARE_CRC */
            *value = FlashDownload_GetFirmwareCRC();
            return true;
            
        default:
            return false;
    }
}

/*****************************固件升级接口表实例******************************/
static const uds_dl_if_t g_firmware_download_iface = {
    .init                = dl_fw_init,
    .on_request_download = dl_fw_on_request_download,
    .on_transfer_data    = dl_fw_on_transfer_data,
    .on_transfer_exit    = dl_fw_on_transfer_exit,
    .erase               = dl_fw_erase,
    .calculate_crc       = dl_fw_calculate_crc,
    .get_state           = dl_fw_get_state,
    .get_progress        = dl_fw_get_progress,
    .get_last_error      = dl_fw_get_last_error,
    .cancel              = dl_fw_cancel,
    .reset               = dl_fw_reset,
    .task                = dl_fw_task,
    .is_pending          = dl_fw_is_pending,
    .read_did            = dl_fw_read_did,
};

/*****************************全局接口实现************************************/

void uds_dl_register(const uds_dl_if_t* iface)
{
    if (iface != NULL)
    {
        g_dl_iface = iface;
        DL_I("Download interface registered");
    }
}

const uds_dl_if_t* uds_dl_get_if(void)
{
    return g_dl_iface;
}

bool uds_dl_is_registered(void)
{
    return (g_dl_iface != NULL);
}

/**
 * @brief 初始化固件升级下载接口（供系统初始化时调用）
 * @note 注册固件升级实现到抽象接口层
 */
void uds_dl_init_fw(void)
{
    DL_I("=== Firmware Download Interface Init ===");
    uds_dl_register(&g_firmware_download_iface);
    DL_I("=== Firmware Download Interface Init Done ===");
}
