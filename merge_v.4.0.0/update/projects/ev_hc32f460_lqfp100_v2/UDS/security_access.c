/**************************************************************************************************
    Filename:     security_access.c
    Description: This file contains the interface to the seck Service.
**************************************************************************************************/
#include "security_access.h"
#include <stdbool.h>

#define CN_LEN (6)
#define LV1_CODE (0xe738)

static uint8_t crc8(uint8_t *data, int32_t length)
{
    uint8_t crc = 0xff;
    int32_t f, b;

    for (f = 0; f < length; f++)
    {
        crc ^= data[f];
        for (b = 0; b < 8; b++)
        {
            if ((crc & 0x80) != 0)
            {
                crc <<= 1;
                crc ^= 0x1d;
            }
            else
            {
                crc <<= 1;
            }
        }
    }
    return ~crc;
}

static bool compute_key_from_seed(uint8_t *seed, uint8_t *key)
{
    uint8_t buf_byte[6];
    uint8_t crc_byte[7];
    int32_t i;

    // 复制种子数据
    for (i = 0; i < CN_LEN; i++)
    {
        buf_byte[i] = seed[i];
    }

    // 第1次CRC8计算
    crc_byte[0] = crc8(buf_byte, CN_LEN);
    buf_byte[0] = crc_byte[0];
    
    // 第2次CRC8计算
    crc_byte[1] = crc8(buf_byte, CN_LEN);

    // 重置缓冲区
    for (i = 0; i < CN_LEN; i++)
    {
        buf_byte[i] = seed[i];
    }
    buf_byte[0] = seed[0];
    buf_byte[1] = crc_byte[1];
    crc_byte[2] = crc8(buf_byte, CN_LEN);

    buf_byte[1] = seed[1];
    buf_byte[2] = crc_byte[2];
    crc_byte[3] = crc8(buf_byte, CN_LEN);

    buf_byte[2] = seed[2];
    buf_byte[3] = crc_byte[3];
    crc_byte[4] = crc8(buf_byte, CN_LEN);

    buf_byte[3] = seed[3];
    buf_byte[4] = crc_byte[4];
    crc_byte[5] = crc8(buf_byte, CN_LEN);

    buf_byte[4] = seed[4];
    buf_byte[5] = crc_byte[5];
    crc_byte[6] = crc8(buf_byte, CN_LEN);

    // 根据条件选择密钥
    if (crc_byte[3] == 0 && crc_byte[4] == 0 && crc_byte[5] == 0 && crc_byte[6] == 0)
    {
        key[0] = crc_byte[1];
        key[1] = crc_byte[2];
        key[2] = crc_byte[3];
        key[3] = crc_byte[4];
    }
    else
    {
        key[0] = crc_byte[3];
        key[1] = crc_byte[4];
        key[2] = crc_byte[5];
        key[3] = crc_byte[6];
    }

    return true;
}

bool seedkey_calc_lv1_key(uint8_t *seed, uint8_t *key)
{
    uint8_t new_seed[CN_LEN];
    int32_t index;

    // 组合种子和固定值0xE738
    for (index = 0; index < 4; index++)
    {
        new_seed[index] = seed[index];
    }
    new_seed[4] = (LV1_CODE & 0xff00) >> 8;  // 0xE7
    new_seed[5] = LV1_CODE & 0xff;            // 0x38
    
    return compute_key_from_seed(new_seed, key);
}
