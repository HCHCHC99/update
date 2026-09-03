#ifndef FLASH_ADVANCED_H_
#define FLASH_ADVANCED_H_

#include <stdint.h>
#include <stddef.h>
#include "hc32f46x_flash.h"

// ============ Flash������¼ʹ�ܺ� ============
// ����˺�������Flash������¼����д��¼�־û���
#define FLASH_MANAGEMENT_RECORD_ENABLE

// ============ Flash�������ã�ͳһ������ ============
// ������С��8KB = 8192�ֽڣ�
#define FLASH_ADV_SECTOR_SIZE            0x2000

// ����������512KB / 8KB = 64��
#define FLASH_ADV_TOTAL_SECTORS          64

// ============ �������򻮷� ============
// �����������壨���ɱ��û���д��
#define FLASH_ADV_PROTECTED_SECTOR_START 0     // 保护起始扇区(扇区0)
#define FLASH_ADV_PROTECTED_SECTOR_END   12    // 保护结束扇区(扇区12): Bootloader(0-7)+UDS共享(8)+预留(9-10)+WDT(11-12)
#define FLASH_ADV_PROTECTED_SECTOR       63    // 额外保护扇区63(不可使用)

// 管理记录存储: 扇区9, 地址范围 0x00012000 - 0x00013FFF
#define FLASH_ADV_MANAGEMENT_SECTOR      9     // 管理记录扇区(在预留区)
#define FLASH_ADV_MANAGEMENT_SECTOR_ADDR (FLASH_ADV_MANAGEMENT_SECTOR * FLASH_ADV_SECTOR_SIZE)  // 0x00012000
#define FLASH_ADV_MANAGEMENT_MAGIC       0x5A5A5A5A
#define FLASH_ADV_MANAGEMENT_VERSION     0x00000001

// 有效扇区范围(用户APP区域)
#define FLASH_ADV_VALID_SECTOR_START     13    // 有效起始扇区(13): APP1起始 0x0001A000
#define FLASH_ADV_VALID_SECTOR_END       61    // 有效结束扇区(61): APP_RUN_SLOT之前

// �û����������ַ����
#define FLASH_ADV_USER_START_ADDR        (FLASH_ADV_VALID_SECTOR_START * FLASH_ADV_SECTOR_SIZE)   // 0x0000C000
#define FLASH_ADV_USER_END_ADDR          ((FLASH_ADV_VALID_SECTOR_END + 1) * FLASH_ADV_SECTOR_SIZE) // 0x0007E000
#define FLASH_ADV_USER_SIZE              ((FLASH_ADV_VALID_SECTOR_END - FLASH_ADV_VALID_SECTOR_START + 1) * FLASH_ADV_SECTOR_SIZE)

// ============ �������꣨ͳһ�ӿڣ� ============
// ��������Ƿ��ڱ���������
#define FLASH_ADV_IS_SECTOR_PROTECTED(sector) \
    (((sector) >= FLASH_ADV_PROTECTED_SECTOR_START && (sector) <= FLASH_ADV_PROTECTED_SECTOR_END) || \
     (sector) == FLASH_ADV_PROTECTED_SECTOR)

// ����ַ�Ƿ��ڱ��������ڣ�ͨ���������жϣ�
#define FLASH_ADV_IS_ADDRESS_PROTECTED(addr) \
    FLASH_ADV_IS_SECTOR_PROTECTED((addr) / FLASH_ADV_SECTOR_SIZE)

// ��������Ƿ����û�����������
#define FLASH_ADV_IS_SECTOR_VALID(sector) \
    ((sector) >= FLASH_ADV_VALID_SECTOR_START && (sector) <= FLASH_ADV_VALID_SECTOR_END)

// ����ַ�Ƿ����û�����������
#define FLASH_ADV_IS_USER_ADDRESS(addr) \
    ((addr) >= FLASH_ADV_USER_START_ADDR && (addr) < FLASH_ADV_USER_END_ADDR)

// ��������Ƿ��д������Ч�������Ҳ��ڱ��������ڣ�
#define FLASH_ADV_IS_SECTOR_WRITABLE(sector) \
    (FLASH_ADV_IS_SECTOR_VALID(sector) && !FLASH_ADV_IS_SECTOR_PROTECTED(sector))

// ����ַ�Ƿ��д
#define FLASH_ADV_IS_ADDRESS_WRITABLE(addr) \
    (FLASH_ADV_IS_USER_ADDRESS(addr) && !FLASH_ADV_IS_ADDRESS_PROTECTED(addr))

// ��ȡ������ʼ��ַ
#define FLASH_ADV_GET_SECTOR_START(addr) \
    ((addr) & ~(FLASH_ADV_SECTOR_SIZE - 1))

// ��ȡ��ַ���ڵ�������
#define FLASH_ADV_GET_SECTOR_INDEX(addr) \
    ((addr) / FLASH_ADV_SECTOR_SIZE)

// ============ 1. ����״̬���� ============
typedef enum {
    FLASH_ADV_OK = 0,
    FLASH_ADV_ERROR = 1,
    FLASH_ADV_BUSY = 2,
    FLASH_ADV_TIMEOUT = 3,
    FLASH_ADV_ADDR_INVALID = 4,
    FLASH_ADV_PROTECTED = 5,
    FLASH_ADV_WRITE_MISMATCH = 6
} FlashAdvStatus_t;

// ============ 2. ���ò��� ============
typedef struct {
    uint32_t base_address;      // Flash����ַ
    uint32_t sector_size;       // ������С
    uint32_t total_size;        // �ܴ�С
    uint32_t protected_size;    // ���������С���ӻ���ַ��ʼ��
    uint32_t max_erase_cycles;  // �����������������������㣩
} FlashAdvConfig_t;

// ============ 3. �������� ============
typedef enum {
    OP_TYPE_NONE = 0,
    OP_TYPE_ERASE_SECTOR,
    OP_TYPE_WRITE_WORD,
    OP_TYPE_WRITE_BULK,
    OP_TYPE_READ_WORD
} FlashAdvOpType_t;

// ============ 4. ������ʷ��¼ ============
#define FLASH_ADV_HISTORY_SIZE 16

typedef struct {
    FlashAdvOpType_t op_type;
    uint32_t address;
    uint32_t data;
    FlashAdvStatus_t status;
    uint32_t sequence_num;
    uint32_t timestamp_ms;
} FlashAdvHistoryRecord_t;

// ============ 5. ͳ����Ϣ ============
typedef struct {
    uint32_t erase_count;       // ��������
    uint32_t write_count;       // д�������������
    uint32_t read_count;        // ��ȡ����
    uint32_t error_count;       // �������
    uint32_t total_operations;  // �ܲ�������
    uint32_t consecutive_errors;// �����������
    uint32_t retry_count;       // ���Դ���
} FlashAdvStatistics_t;

// ============ 6. ������Ϣ ============
#define MAX_FLASH_SECTORS_ADV   64   // 64��������ÿ��8KB

typedef struct {
    uint32_t magic;                     // ħ����������֤������Ч��
    uint32_t version;                   // �汾��
    uint32_t max_erase_cycles;          // ����������
    uint32_t warning_threshold;         // ������ֵ���ٷֱȣ�
    uint32_t current_max_erase;         // ��ǰ����������
    uint32_t current_min_erase;         // ��ǰ��С��������
    uint32_t total_erase_count;         // �ܲ�����������������֮�ͣ�
    uint32_t sector_size;               // ������С���ֽڣ�= 8192
    uint32_t sector_erase_counts[MAX_FLASH_SECTORS_ADV];  // ��������������
    uint16_t sector_count;              // �������� = 64
    uint8_t health_percent;             // ���彡���Ȱٷֱ�
    uint32_t valid_start_sector;        // ��Ч��ʼ����
    uint32_t valid_end_sector;          // ��Ч��������
    uint32_t checksum;                  // У���
} FlashAdvLifetimeInfo_t;

// ============ 7. ����д����� ============
typedef struct {
    uint32_t start_address;     // ��ʼ��ַ
    const uint32_t* data;       // ����ָ��
    uint32_t word_count;        // ������4�ֽ�Ϊ��λ��
    uint8_t verify_enabled;     // �Ƿ�������֤
    uint8_t retry_on_error;     // ����ʱ�Ƿ�����
} FlashAdvBulkWriteParams_t;

// ============ 8. �߼�Flash��� ============
typedef struct FlashAdvHandle FlashAdvHandle_t;

// ============ 9. �ײ��������ָ�� ============
typedef struct {
    FlashAdvStatus_t (*erase_sector)(uint32_t address);
    FlashAdvStatus_t (*write_word_check)(uint32_t address, uint32_t data);
    uint32_t (*read_word)(uint32_t address);
    FlashAdvStatus_t (*wait_done)(uint32_t timeout_us);
} FlashAdvOps_t;

// ============ 10. �����ӿں��� ============

// ����/����ʵ��
FlashAdvHandle_t* FlashAdv_Create(const FlashAdvConfig_t* config, const FlashAdvOps_t* ops);
void FlashAdv_Destroy(FlashAdvHandle_t* handle);

// ��������
FlashAdvStatus_t FlashAdv_EraseSector(FlashAdvHandle_t* handle, uint32_t address);
FlashAdvStatus_t FlashAdv_WriteWord(FlashAdvHandle_t* handle, uint32_t address, uint32_t data);
uint32_t FlashAdv_ReadWord(FlashAdvHandle_t* handle, uint32_t address);

// �߼����ܣ�����д��
FlashAdvStatus_t FlashAdv_BulkWrite(FlashAdvHandle_t* handle, const FlashAdvBulkWriteParams_t* params);
FlashAdvStatus_t FlashAdv_BulkWriteSimple(FlashAdvHandle_t* handle, 
                                           uint32_t start_address, 
                                           const uint32_t* data, 
                                           uint32_t word_count);

// ��ȡͳ����Ϣ
FlashAdvStatus_t FlashAdv_GetStatistics(FlashAdvHandle_t* handle, FlashAdvStatistics_t* stats);
void FlashAdv_ResetStatistics(FlashAdvHandle_t* handle);

// ��ȡ������Ϣ
FlashAdvStatus_t FlashAdv_GetLifetimeInfo(FlashAdvHandle_t* handle, FlashAdvLifetimeInfo_t* info);

// ����/����������Ϣ��Flash
FlashAdvStatus_t FlashAdv_SaveLifetimeInfo(FlashAdvHandle_t* handle);
FlashAdvStatus_t FlashAdv_LoadLifetimeInfo(FlashAdvHandle_t* handle);

// ��ȡ������ʷ
FlashAdvStatus_t FlashAdv_GetHistory(FlashAdvHandle_t* handle, 
                                      FlashAdvHistoryRecord_t* out_buffer, 
                                      uint32_t buffer_size, 
                                      uint32_t* num_returned);

// ��ȡ����һ�β�����¼
FlashAdvStatus_t FlashAdv_GetLastOperation(FlashAdvHandle_t* handle, FlashAdvHistoryRecord_t* record);

// �����ʷ��¼
void FlashAdv_ClearHistory(FlashAdvHandle_t* handle);

// ����������
uint8_t FlashAdv_IsAddressProtected(FlashAdvHandle_t* handle, uint32_t address);

// ��ַ��Ч�Լ��
uint8_t FlashAdv_IsValidAddress(FlashAdvHandle_t* handle, uint32_t address);

// ������ַ����
uint32_t FlashAdv_GetSectorStart(FlashAdvHandle_t* handle, uint32_t address);

// ��ȡָ�������Ĳ�������
uint32_t FlashAdv_GetSectorEraseCount(FlashAdvHandle_t* handle, uint32_t sector_index);

// ��ȡָ����ַ���������Ĳ�������
uint32_t FlashAdv_GetAddressEraseCount(FlashAdvHandle_t* handle, uint32_t address);

// ��������ͳ��
void FlashAdv_ResetLifetime(FlashAdvHandle_t* handle);

// ��������Ƿ��д
uint8_t FlashAdv_IsSectorWritable(FlashAdvHandle_t* handle, uint32_t sector_index);

// ��ȡ��Ч������Χ
void FlashAdv_GetValidSectorRange(FlashAdvHandle_t* handle, uint32_t* start, uint32_t* end);

// ============ 11. ���Ժ��� ============
uint32_t flash_advanced_test(void);

// ============ 12. �û�����ӿڣ����ϲ�ģ��ʹ�ã� ============

// ��ȡ�û�����Flash����
void FlashAdv_GetUserRegion(uint32_t *start_addr, uint32_t *end_addr, uint32_t *size);

// ����ַ�Ƿ����û����������ڣ�ʹ�ú궨�壩
static inline uint8_t FlashAdv_IsUserAddress(uint32_t address)
{
    return FLASH_ADV_IS_USER_ADDRESS(address);
}

// ��ȡ������С
static inline uint32_t FlashAdv_GetSectorSize(void)
{
    return FLASH_ADV_SECTOR_SIZE;
}

// ��ȡ��Ч������Χ�������ţ�
void FlashAdv_GetValidSectorRangeNum(uint32_t *start_sector, uint32_t *end_sector);

// ��ȡ����������Χ
static inline void FlashAdv_GetProtectedSectorRange(uint32_t *start_sector, uint32_t *end_sector)
{
    if (start_sector) *start_sector = FLASH_ADV_PROTECTED_SECTOR_START;
    if (end_sector) *end_sector = FLASH_ADV_PROTECTED_SECTOR_END;
}

// ����ַ�Ƿ��ڱ���������
static inline uint8_t FlashAdv_IsAddressProtectedByMacro(uint32_t address)
{
    return FLASH_ADV_IS_ADDRESS_PROTECTED(address);
}

#endif /* FLASH_ADVANCED_H_ */
