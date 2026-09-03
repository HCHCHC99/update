/**************************************************************************************************
    Filename:     security_access.h
    Description: Security access algorithm for UDS 0x27 service
**************************************************************************************************/
#ifndef SECURITY_ACCESS_H_
#define SECURITY_ACCESS_H_

#include "stdint.h"
#include "stdbool.h"

#define CN_LEN (6)
#define LV1_CODE (0xe738)

// 函数声明 - 使用标准bool类型
bool seedkey_calc_lv1_key(uint8_t *seed, uint8_t *key);

#endif /* SECURITY_ACCESS_H_ */
