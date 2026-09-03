#include "flash_advanced.h"
#include <string.h>

// �ⲿ��������Ҫ���ӵ��ײ�HC32FLASH����
extern HC32FLASH_STATUS HC32FLASH_EraseSector(uint32_t u32Addr);
extern HC32FLASH_STATUS HC32FLASH_WritedWord_Check(uint32_t u32Addr, uint32_t data);
extern uint32_t HC32FLASH_ReaddWord(uint32_t u32Addr);

// 有效扇区范围定义
#define VALID_SECTOR_START    13    // 有效起始扇区(13): APP1起始 0x0001A000
#define VALID_SECTOR_END      61    // 有效结束扇区(61): APP_RUN_SLOT之前
#define PROTECTED_SECTOR      63    // 额外保护扇区63(不可使用)

// 管理记录存储: 扇区9, 地址范围 0x00012000 - 0x00013FFF
#define MANAGEMENT_SECTOR     9                      // 管理记录扇区(在预留区)
#define MANAGEMENT_SECTOR_ADDR (0x00012000)          // 扇区9起始地址
#define MANAGEMENT_MAGIC       0x5A5A5A5A            // 魔数
#define MANAGEMENT_VERSION     0x00000001            // 版本号

// ============ �ڲ��ṹ���� ============
struct FlashAdvHandle {
    FlashAdvConfig_t config;
    FlashAdvOps_t ops;
    
    // ״̬��¼
    FlashAdvOpType_t last_op_type;
    uint32_t last_address;
    uint32_t last_data;
    FlashAdvStatus_t last_status;
    
    // ͳ����Ϣ
    FlashAdvStatistics_t stats;
    
    // ��ʷ��¼
    FlashAdvHistoryRecord_t history[FLASH_ADV_HISTORY_SIZE];
    uint32_t total_operations;
    uint8_t history_count;
    
    // ������Ϣ
    FlashAdvLifetimeInfo_t lifetime;
    
    // ��ʼ����־
    uint8_t initialized;
    uint8_t management_enabled;  // ������¼�Ƿ�����
};

// ȫ�־��
static FlashAdvHandle_t g_flash_adv_handle;
static uint8_t g_flash_adv_created = 0;

// ============ �ڲ������������� ============
static uint8_t FlashAdv_IsValid(FlashAdvHandle_t* handle, uint32_t address);
static uint32_t FlashAdv_GetSectorIndex(FlashAdvHandle_t* handle, uint32_t address);
static void FlashAdv_RecordOperation(FlashAdvHandle_t* handle, 
                                      FlashAdvOpType_t op_type,
                                      uint32_t address, 
                                      uint32_t data, 
                                      FlashAdvStatus_t status);
static void FlashAdv_UpdateLifetime(FlashAdvHandle_t* handle, uint32_t sector_idx);
static uint8_t FlashAdv_IsProtected(FlashAdvHandle_t* handle, uint32_t address);
static FlashAdvStatus_t FlashAdv_WriteWithRetry(FlashAdvHandle_t* handle, 
                                                 uint32_t address, 
                                                 uint32_t data, 
                                                 uint32_t max_retries);
static uint32_t FlashAdv_CalculateChecksum(const FlashAdvLifetimeInfo_t* info);

// ============ �ײ����亯�� ============
static FlashAdvStatus_t Adv_EraseSector(uint32_t address)
{
    HC32FLASH_STATUS st = HC32FLASH_EraseSector(address);
    return (st == HC32FLASH_OK) ? FLASH_ADV_OK : FLASH_ADV_ERROR;
}

static FlashAdvStatus_t Adv_WriteWordCheck(uint32_t address, uint32_t data)
{
    HC32FLASH_STATUS st = HC32FLASH_WritedWord_Check(address, data);
    return (st == HC32FLASH_OK) ? FLASH_ADV_OK : FLASH_ADV_ERROR;
}

static uint32_t Adv_ReadWord(uint32_t address)
{
    return HC32FLASH_ReaddWord(address);
}

static FlashAdvStatus_t Adv_WaitDone(uint32_t timeout_us)
{
    for (uint32_t i = 0; i < timeout_us; i++) {
        __NOP();
    }
    return FLASH_ADV_OK;
}

static void GetDefaultOps(FlashAdvOps_t* ops)
{
    ops->erase_sector = Adv_EraseSector;
    ops->write_word_check = Adv_WriteWordCheck;
    ops->read_word = Adv_ReadWord;
    ops->wait_done = Adv_WaitDone;
}

// ============ У��ͼ��� ============
static uint32_t FlashAdv_CalculateChecksum(const FlashAdvLifetimeInfo_t* info)
{
    uint32_t sum = 0;
    const uint32_t* data = (const uint32_t*)info;
    // �����checksum�ֶ���������ֽ�
    for (uint32_t i = 0; i < sizeof(FlashAdvLifetimeInfo_t) - 4; i += 4) {
        sum += *data++;
    }
    return sum;
}

// ============ ����������Ϣ��Flash������9�� ============
FlashAdvStatus_t FlashAdv_SaveLifetimeInfo(FlashAdvHandle_t* handle)
{
    if (!handle || !handle->management_enabled) {
        return FLASH_ADV_ERROR;
    }
    
    // ׼��Ҫ���������
    FlashAdvLifetimeInfo_t save_info = handle->lifetime;
    save_info.magic = MANAGEMENT_MAGIC;
    save_info.version = MANAGEMENT_VERSION;
    save_info.checksum = FlashAdv_CalculateChecksum(&save_info);
    
    // �Ȳ�����������
    FlashAdvStatus_t status = handle->ops.erase_sector(MANAGEMENT_SECTOR_ADDR);
    if (status != FLASH_ADV_OK) {
        return status;
    }
    
    // �����д��
    uint32_t* write_ptr = (uint32_t*)&save_info;
    uint32_t word_count = sizeof(FlashAdvLifetimeInfo_t) / 4;
    
    for (uint32_t i = 0; i < word_count; i++) {
        uint32_t addr = MANAGEMENT_SECTOR_ADDR + i * 4;
        status = handle->ops.write_word_check(addr, write_ptr[i]);
        if (status != FLASH_ADV_OK) {
            return status;
        }
    }
    
    return FLASH_ADV_OK;
}

// ============ ��Flash����������Ϣ������9�� ============
FlashAdvStatus_t FlashAdv_LoadLifetimeInfo(FlashAdvHandle_t* handle)
{
    if (!handle || !handle->management_enabled) {
        return FLASH_ADV_ERROR;
    }
    
    // ��ȡ������������
    FlashAdvLifetimeInfo_t loaded_info;
    uint32_t* read_ptr = (uint32_t*)&loaded_info;
    
    for (uint32_t i = 0; i < sizeof(FlashAdvLifetimeInfo_t) / 4; i++) {
        read_ptr[i] = handle->ops.read_word(MANAGEMENT_SECTOR_ADDR + i * 4);
    }
    
    // ���ħ����У���
    if (loaded_info.magic == MANAGEMENT_MAGIC && 
        loaded_info.version == MANAGEMENT_VERSION) {
        uint32_t calc_checksum = FlashAdv_CalculateChecksum(&loaded_info);
        if (calc_checksum == loaded_info.checksum) {
            // ������Ч���ָ�������Ϣ
            handle->lifetime = loaded_info;
            return FLASH_ADV_OK;
        }
    }
    
    return FLASH_ADV_ERROR;
}

// ============ �ڲ���������ʵ�� ============
static uint8_t FlashAdv_IsValid(FlashAdvHandle_t* handle, uint32_t address)
{
    if (!handle) return 0;
    uint32_t end = handle->config.base_address + handle->config.total_size;
    return (address >= handle->config.base_address && address < end);
}

static uint32_t FlashAdv_GetSectorIndex(FlashAdvHandle_t* handle, uint32_t address)
{
    if (!handle || handle->config.sector_size == 0) return 0;
    uint32_t offset = address - handle->config.base_address;
    return offset / handle->config.sector_size;
}

// �ҵ� FlashAdv_IsProtected �������滻Ϊ��
static uint8_t FlashAdv_IsProtected(FlashAdvHandle_t* handle, uint32_t address)
{
    if (!handle) return 1;
    
    // ʹ��ͳһ�ĺ궨����б������
    return FLASH_ADV_IS_ADDRESS_PROTECTED(address);
}

static void FlashAdv_RecordOperation(FlashAdvHandle_t* handle, 
                                      FlashAdvOpType_t op_type,
                                      uint32_t address, 
                                      uint32_t data, 
                                      FlashAdvStatus_t status)
{
    if (!handle) return;
    
    uint32_t idx = handle->total_operations % FLASH_ADV_HISTORY_SIZE;
    
    handle->history[idx].op_type = op_type;
    handle->history[idx].address = address;
    handle->history[idx].data = data;
    handle->history[idx].status = status;
    handle->history[idx].sequence_num = handle->total_operations;
    handle->history[idx].timestamp_ms = 0;
    
    handle->total_operations++;
    if (handle->history_count < FLASH_ADV_HISTORY_SIZE) {
        handle->history_count++;
    }
    
    handle->last_op_type = op_type;
    handle->last_address = address;
    handle->last_data = data;
    handle->last_status = status;
}

static void FlashAdv_UpdateLifetime(FlashAdvHandle_t* handle, uint32_t sector_idx)
{
    if (!handle || sector_idx >= handle->lifetime.sector_count || 
        sector_idx >= MAX_FLASH_SECTORS_ADV) return;
    
    handle->lifetime.sector_erase_counts[sector_idx]++;
    handle->lifetime.total_erase_count++;
    
    uint32_t new_max = 0;
    uint32_t new_min = 0xFFFFFFFF;
    
    for (uint32_t i = 0; i < handle->lifetime.sector_count && i < MAX_FLASH_SECTORS_ADV; i++) {
        uint32_t count = handle->lifetime.sector_erase_counts[i];
        if (count > new_max) {
            new_max = count;
        }
        if (count < new_min) {
            new_min = count;
        }
    }
    
    handle->lifetime.current_max_erase = new_max;
    handle->lifetime.current_min_erase = (new_min == 0xFFFFFFFF) ? 0 : new_min;
    
    if (handle->lifetime.max_erase_cycles > 0) {
        uint32_t usage_percent = (new_max * 100) / handle->lifetime.max_erase_cycles;
        handle->lifetime.health_percent = (usage_percent > 100) ? 0 : (100 - usage_percent);
    }
    
#ifdef FLASH_MANAGEMENT_RECORD_ENABLE
    // ÿ�β����󱣴�������Ϣ
    FlashAdv_SaveLifetimeInfo(handle);
#endif
}

static FlashAdvStatus_t FlashAdv_WriteWithRetry(FlashAdvHandle_t* handle, 
                                                 uint32_t address, 
                                                 uint32_t data, 
                                                 uint32_t max_retries)
{
    FlashAdvStatus_t status;
    
    for (uint32_t retry = 0; retry <= max_retries; retry++) {
        status = handle->ops.write_word_check(address, data);
        
        if (status == FLASH_ADV_OK) {
            if (retry > 0) {
                handle->stats.retry_count += retry;
            }
            return FLASH_ADV_OK;
        }
        
        if (handle->ops.wait_done) {
            handle->ops.wait_done(100);
        }
    }
    
    return status;
}

// ============ �����ӿ�ʵ�� ============

FlashAdvHandle_t* FlashAdv_Create(const FlashAdvConfig_t* config, const FlashAdvOps_t* ops)
{
    if (g_flash_adv_created) {
        return &g_flash_adv_handle;
    }
    
    FlashAdvOps_t default_ops;
    if (!ops) {
        GetDefaultOps(&default_ops);
        ops = &default_ops;
    }
    
    if (!config || !ops->erase_sector || !ops->write_word_check || !ops->read_word) {
        return NULL;
    }
    
    memset(&g_flash_adv_handle, 0, sizeof(FlashAdvHandle_t));
    
    g_flash_adv_handle.config = *config;
    g_flash_adv_handle.ops = *ops;
    
#ifdef FLASH_MANAGEMENT_RECORD_ENABLE
    g_flash_adv_handle.management_enabled = 1;
#else
    g_flash_adv_handle.management_enabled = 0;
#endif
    
    // ��ʼ��������Ϣ
    g_flash_adv_handle.lifetime.max_erase_cycles = config->max_erase_cycles;
    g_flash_adv_handle.lifetime.warning_threshold = 80;
    g_flash_adv_handle.lifetime.sector_size = config->sector_size;
    g_flash_adv_handle.lifetime.sector_count = config->total_size / config->sector_size;
    g_flash_adv_handle.lifetime.valid_start_sector = VALID_SECTOR_START;
    g_flash_adv_handle.lifetime.valid_end_sector = VALID_SECTOR_END;
    
    if (g_flash_adv_handle.lifetime.sector_count > MAX_FLASH_SECTORS_ADV) {
        g_flash_adv_handle.lifetime.sector_count = MAX_FLASH_SECTORS_ADV;
    }
    
    g_flash_adv_handle.lifetime.health_percent = 100;
    g_flash_adv_handle.lifetime.current_max_erase = 0;
    g_flash_adv_handle.lifetime.current_min_erase = 0;
    g_flash_adv_handle.lifetime.total_erase_count = 0;
    
    for (uint32_t i = 0; i < g_flash_adv_handle.lifetime.sector_count; i++) {
        g_flash_adv_handle.lifetime.sector_erase_counts[i] = 0;
    }
    
    // ���Լ����ѱ����������Ϣ
#ifdef FLASH_MANAGEMENT_RECORD_ENABLE
    FlashAdv_LoadLifetimeInfo(&g_flash_adv_handle);
#endif
    
    g_flash_adv_handle.last_op_type = OP_TYPE_NONE;
    g_flash_adv_handle.last_status = FLASH_ADV_OK;
    g_flash_adv_handle.initialized = 1;
    g_flash_adv_created = 1;
    
    return &g_flash_adv_handle;
}

void FlashAdv_Destroy(FlashAdvHandle_t* handle)
{
    if (!handle) return;
    memset(handle, 0, sizeof(FlashAdvHandle_t));
    g_flash_adv_created = 0;
}

FlashAdvStatus_t FlashAdv_EraseSector(FlashAdvHandle_t* handle, uint32_t address)
{
    if (!handle || !handle->initialized) return FLASH_ADV_ERROR;
    
    if (!FlashAdv_IsValid(handle, address)) {
        FlashAdv_RecordOperation(handle, OP_TYPE_ERASE_SECTOR, address, 0, FLASH_ADV_ADDR_INVALID);
        handle->stats.error_count++;
        return FLASH_ADV_ADDR_INVALID;
    }
    
    if (FlashAdv_IsProtected(handle, address)) {
        FlashAdv_RecordOperation(handle, OP_TYPE_ERASE_SECTOR, address, 0, FLASH_ADV_PROTECTED);
        handle->stats.error_count++;
        return FLASH_ADV_PROTECTED;
    }
    
    handle->stats.total_operations++;
    
    FlashAdvStatus_t status = handle->ops.erase_sector(address);
    
    if (status == FLASH_ADV_OK) {
        handle->stats.erase_count++;
        uint32_t sector_idx = FlashAdv_GetSectorIndex(handle, address);
        if (sector_idx < handle->lifetime.sector_count) {
            FlashAdv_UpdateLifetime(handle, sector_idx);
        }
    } else {
        handle->stats.error_count++;
        handle->stats.consecutive_errors++;
    }
    
    FlashAdv_RecordOperation(handle, OP_TYPE_ERASE_SECTOR, address, 0, status);
    
    return status;
}

FlashAdvStatus_t FlashAdv_WriteWord(FlashAdvHandle_t* handle, uint32_t address, uint32_t data)
{
    if (!handle || !handle->initialized) return FLASH_ADV_ERROR;
    
    if (!FlashAdv_IsValid(handle, address)) {
        FlashAdv_RecordOperation(handle, OP_TYPE_WRITE_WORD, address, data, FLASH_ADV_ADDR_INVALID);
        handle->stats.error_count++;
        return FLASH_ADV_ADDR_INVALID;
    }
    
    if (FlashAdv_IsProtected(handle, address)) {
        FlashAdv_RecordOperation(handle, OP_TYPE_WRITE_WORD, address, data, FLASH_ADV_PROTECTED);
        handle->stats.error_count++;
        return FLASH_ADV_PROTECTED;
    }
    
    handle->stats.total_operations++;
    
    FlashAdvStatus_t status = FlashAdv_WriteWithRetry(handle, address, data, 3);
    
    if (status == FLASH_ADV_OK) {
        handle->stats.write_count++;
        handle->stats.consecutive_errors = 0;
    } else {
        handle->stats.error_count++;
        handle->stats.consecutive_errors++;
    }
    
    FlashAdv_RecordOperation(handle, OP_TYPE_WRITE_WORD, address, data, status);
    
    return status;
}

uint32_t FlashAdv_ReadWord(FlashAdvHandle_t* handle, uint32_t address)
{
    if (!handle || !handle->initialized) return 0xFFFFFFFF;
    
    if (!FlashAdv_IsValid(handle, address)) {
        FlashAdv_RecordOperation(handle, OP_TYPE_READ_WORD, address, 0, FLASH_ADV_ADDR_INVALID);
        handle->stats.error_count++;
        return 0xFFFFFFFF;
    }
    
    handle->stats.total_operations++;
    handle->stats.read_count++;
    
    uint32_t data = handle->ops.read_word(address);
    FlashAdv_RecordOperation(handle, OP_TYPE_READ_WORD, address, data, FLASH_ADV_OK);
    
    return data;
}

FlashAdvStatus_t FlashAdv_BulkWrite(FlashAdvHandle_t* handle, const FlashAdvBulkWriteParams_t* params)
{
    if (!handle || !params || !params->data) return FLASH_ADV_ERROR;
    
    if (params->word_count == 0) return FLASH_ADV_OK;
    
    uint32_t end_address = params->start_address + (params->word_count * 4);
    
    if (!FlashAdv_IsValid(handle, params->start_address) || 
        !FlashAdv_IsValid(handle, end_address - 4)) {
        handle->stats.error_count++;
        return FLASH_ADV_ADDR_INVALID;
    }
    
    if (FlashAdv_IsProtected(handle, params->start_address)) {
        handle->stats.error_count++;
        return FLASH_ADV_PROTECTED;
    }
    
    handle->stats.total_operations++;
    
    FlashAdvStatus_t status;
    uint32_t success_count = 0;
    
    for (uint32_t i = 0; i < params->word_count; i++) {
        uint32_t addr = params->start_address + (i * 4);
        uint32_t data = params->data[i];
        
        if (params->retry_on_error) {
            status = FlashAdv_WriteWithRetry(handle, addr, data, 3);
        } else {
            status = handle->ops.write_word_check(addr, data);
        }
        
        if (status != FLASH_ADV_OK) {
            handle->stats.error_count++;
            handle->stats.consecutive_errors++;
            FlashAdv_RecordOperation(handle, OP_TYPE_WRITE_BULK, addr, data, status);
            return status;
        }
        
        success_count++;
        
        if (params->verify_enabled) {
            uint32_t verify_data = handle->ops.read_word(addr);
            if (verify_data != data) {
                handle->stats.error_count++;
                status = FLASH_ADV_WRITE_MISMATCH;
                FlashAdv_RecordOperation(handle, OP_TYPE_WRITE_BULK, addr, data, status);
                return status;
            }
        }
        
        FlashAdv_RecordOperation(handle, OP_TYPE_WRITE_BULK, addr, data, FLASH_ADV_OK);
    }
    
    handle->stats.write_count += success_count;
    handle->stats.consecutive_errors = 0;
    
    return FLASH_ADV_OK;
}

FlashAdvStatus_t FlashAdv_BulkWriteSimple(FlashAdvHandle_t* handle, 
                                           uint32_t start_address, 
                                           const uint32_t* data, 
                                           uint32_t word_count)
{
    FlashAdvBulkWriteParams_t params = {
        .start_address = start_address,
        .data = data,
        .word_count = word_count,
        .verify_enabled = 1,
        .retry_on_error = 1
    };
    return FlashAdv_BulkWrite(handle, &params);
}

FlashAdvStatus_t FlashAdv_GetStatistics(FlashAdvHandle_t* handle, FlashAdvStatistics_t* stats)
{
    if (!handle || !stats) return FLASH_ADV_ERROR;
    *stats = handle->stats;
    return FLASH_ADV_OK;
}

void FlashAdv_ResetStatistics(FlashAdvHandle_t* handle)
{
    if (!handle) return;
    memset(&handle->stats, 0, sizeof(FlashAdvStatistics_t));
}

FlashAdvStatus_t FlashAdv_GetLifetimeInfo(FlashAdvHandle_t* handle, FlashAdvLifetimeInfo_t* info)
{
    if (!handle || !info) return FLASH_ADV_ERROR;
    *info = handle->lifetime;
    return FLASH_ADV_OK;
}

FlashAdvStatus_t FlashAdv_GetHistory(FlashAdvHandle_t* handle, 
                                      FlashAdvHistoryRecord_t* out_buffer, 
                                      uint32_t buffer_size, 
                                      uint32_t* num_returned)
{
    if (!handle || !out_buffer || !num_returned) return FLASH_ADV_ERROR;
    
    uint32_t copy_count = (handle->history_count < buffer_size) ? 
                          handle->history_count : buffer_size;
    
    uint32_t start_idx = (handle->total_operations > handle->history_count) ?
                         (handle->total_operations % FLASH_ADV_HISTORY_SIZE) : 0;
    
    for (uint32_t i = 0; i < copy_count; i++) {
        uint32_t idx = (start_idx + i) % FLASH_ADV_HISTORY_SIZE;
        out_buffer[i] = handle->history[idx];
    }
    
    *num_returned = copy_count;
    return FLASH_ADV_OK;
}

FlashAdvStatus_t FlashAdv_GetLastOperation(FlashAdvHandle_t* handle, FlashAdvHistoryRecord_t* record)
{
    if (!handle || !record) return FLASH_ADV_ERROR;
    
    if (handle->total_operations == 0) {
        return FLASH_ADV_ERROR;
    }
    
    uint32_t last_idx = (handle->total_operations - 1) % FLASH_ADV_HISTORY_SIZE;
    *record = handle->history[last_idx];
    return FLASH_ADV_OK;
}

void FlashAdv_ClearHistory(FlashAdvHandle_t* handle)
{
    if (!handle) return;
    memset(handle->history, 0, sizeof(handle->history));
    handle->history_count = 0;
    handle->total_operations = 0;
}

uint8_t FlashAdv_IsAddressProtected(FlashAdvHandle_t* handle, uint32_t address)
{
    if (!handle) return 1;
    return FlashAdv_IsProtected(handle, address);
}

uint8_t FlashAdv_IsValidAddress(FlashAdvHandle_t* handle, uint32_t address)
{
    if (!handle) return 0;
    return FlashAdv_IsValid(handle, address);
}

uint32_t FlashAdv_GetSectorStart(FlashAdvHandle_t* handle, uint32_t address)
{
    if (!handle) return 0;
    
    uint32_t offset = address - handle->config.base_address;
    uint32_t sector_offset = (offset / handle->config.sector_size) * handle->config.sector_size;
    return handle->config.base_address + sector_offset;
}

uint32_t FlashAdv_GetSectorEraseCount(FlashAdvHandle_t* handle, uint32_t sector_index)
{
    if (!handle || sector_index >= handle->lifetime.sector_count ||
        sector_index >= MAX_FLASH_SECTORS_ADV) {
        return 0xFFFFFFFF;
    }
    return handle->lifetime.sector_erase_counts[sector_index];
}

uint32_t FlashAdv_GetAddressEraseCount(FlashAdvHandle_t* handle, uint32_t address)
{
    if (!handle) return 0xFFFFFFFF;
    
    if (!FlashAdv_IsValid(handle, address)) {
        return 0xFFFFFFFF;
    }
    
    uint32_t sector_idx = FlashAdv_GetSectorIndex(handle, address);
    return FlashAdv_GetSectorEraseCount(handle, sector_idx);
}

void FlashAdv_ResetLifetime(FlashAdvHandle_t* handle)
{
    if (!handle) return;
    
    handle->lifetime.current_max_erase = 0;
    handle->lifetime.current_min_erase = 0;
    handle->lifetime.total_erase_count = 0;
    handle->lifetime.health_percent = 100;
    
    for (uint32_t i = 0; i < handle->lifetime.sector_count && i < MAX_FLASH_SECTORS_ADV; i++) {
        handle->lifetime.sector_erase_counts[i] = 0;
    }
    
#ifdef FLASH_MANAGEMENT_RECORD_ENABLE
    FlashAdv_SaveLifetimeInfo(handle);
#endif
}

uint8_t FlashAdv_IsSectorWritable(FlashAdvHandle_t* handle, uint32_t sector_index)
{
    if (!handle) return 0;
    return (sector_index >= VALID_SECTOR_START && 
            sector_index <= VALID_SECTOR_END &&
            sector_index != MANAGEMENT_SECTOR &&
            sector_index != PROTECTED_SECTOR);
}

void FlashAdv_GetValidSectorRange(FlashAdvHandle_t* handle, uint32_t* start, uint32_t* end)
{
    if (!handle) {
        if (start) *start = 0;
        if (end) *end = 0;
        return;
    }
    if (start) *start = VALID_SECTOR_START;
    if (end) *end = VALID_SECTOR_END;
}

// ============ ���Ժ���ʵ�� ============
#define FLASH_ADV_TEST_ADDR     0x00050000
#define FLASH_ADV_TEST_WORD_COUNT 8

static uint32_t g_test_result = 0;

uint32_t flash_advanced_test(void)
{
    FlashAdvConfig_t config = {
        .base_address = 0x00000000,
        .sector_size = 0x2000,
        .total_size = 0x80000,
        .protected_size = 0x00000,
        .max_erase_cycles = 10000
    };
    
    FlashAdvHandle_t* flash = FlashAdv_Create(&config, NULL);
    if (!flash) {
        g_test_result = 0xFFFFFFFF;
        return g_test_result;
    }
    
    uint32_t test_data[FLASH_ADV_TEST_WORD_COUNT] = {
        0x12345678, 0x23456789, 0x3456789A, 0x456789AB,
        0x56789ABC, 0x6789ABCD, 0x789ABCDE, 0x89ABCDEF
    };
    
    uint32_t verify_data[FLASH_ADV_TEST_WORD_COUNT];
    
    uint32_t sector_start = FlashAdv_GetSectorStart(flash, FLASH_ADV_TEST_ADDR);
    FlashAdvStatus_t status = FlashAdv_EraseSector(flash, sector_start);
    if (status != FLASH_ADV_OK) {
        g_test_result = 1;
        return g_test_result;
    }
    
    status = FlashAdv_BulkWriteSimple(flash, FLASH_ADV_TEST_ADDR, test_data, FLASH_ADV_TEST_WORD_COUNT);
    if (status != FLASH_ADV_OK) {
        g_test_result = 2;
        return g_test_result;
    }
    
    for (uint32_t i = 0; i < FLASH_ADV_TEST_WORD_COUNT; i++) {
        verify_data[i] = FlashAdv_ReadWord(flash, FLASH_ADV_TEST_ADDR + (i * 4));
        if (verify_data[i] != test_data[i]) {
            g_test_result = 3 + i;
            return g_test_result;
        }
    }
    
    g_test_result = 0;
    return g_test_result;
}


// �û����������ַ����������Լ�����㣩
#define USER_REGION_START_ADDR  (VALID_SECTOR_START * 0x2000)   // ����10: 0x14000
#define USER_REGION_END_ADDR    ((VALID_SECTOR_END + 1) * 0x2000) // ����63��ʼ: 0x7E000? ʵ�ʽ�����0x7C000
// ����HC32F46x���������֣�����0=0x00000, ����10=0x14000, ����62=0x7C000

void FlashAdv_GetUserRegion(uint32_t *start_addr, uint32_t *end_addr, uint32_t *size)
{
    if (start_addr) *start_addr = VALID_SECTOR_START * 0x2000;
    if (end_addr) *end_addr = (VALID_SECTOR_END + 1) * 0x2000;  // �ų�����62? ��Ҫȷ�ϱ߽�
    if (size) *size = (VALID_SECTOR_END - VALID_SECTOR_START + 1) * 0x2000;
}
