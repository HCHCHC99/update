#ifndef CAN_ADAPTER_H_
#define CAN_ADAPTER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "Adapter_Can.h"

/* Old CAN TX frame type -> new CanMsg_t */
typedef CanMsg_t CAN_TSMT_FRAME_t;

/*
 * Map old API to new API.
 * These are inline wrappers so UDS/ISO-TP code compiles unchanged.
 */

static inline int8_t can_adapter_LoadExtFrame(CAN_TSMT_FRAME_t *pFrame, uint32_t CAN_ID,
                                              uint8_t *pData, uint8_t Len)
{
    if (pFrame == NULL || pData == NULL) {
        return -1;
    }
    pFrame->u32ID = CAN_ID;
    pFrame->u8IDE = 1U;
    pFrame->u8RTR = 0U;
    pFrame->u8FDF = 0U;
    pFrame->u8BRS = 0U;
    pFrame->u8DLC = (Len > 8U) ? 8U : Len;
    for (uint8_t i = 0U; i < pFrame->u8DLC; i++) {
        pFrame->au8Data[i] = pData[i];
    }
    pFrame->u32Timestamp = 0UL;
    return 0;
}

/*
 * Non-blocking send via new CanIf layer.
 * The old API was polling (blocking), but since UDS uses ISO-TP with its own
 * flow-control / timeout state machine, fire-and-forget via CanIf_Send is safe.
 * The TX queue is drained by CanIf_Poll() from the main loop.
 */
static inline int8_t can_adapter_Transmit_Polling(uint8_t Channel, const CAN_TSMT_FRAME_t *pFrame,
                                                  uint8_t Len)
{
    (void)Channel;
    (void)Len;
    if (pFrame == NULL) {
        return -1;
    }
    CanIf_Send(pFrame);
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* CAN_ADAPTER_H_ */
