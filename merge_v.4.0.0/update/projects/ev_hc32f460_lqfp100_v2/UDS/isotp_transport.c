/*******************************************
* �ļ���: isotp_transport.c
* ����: AI Assistant
* �汾: V1.0.0
* ����: ISO 15765-2 �����Э��ʵ��
* ��ע: ֧�ֳ����ĵķְ����ͺͽ���
*******************************************/
#include "isotp_transport.h"
#include "can_adapter.h"
#include "rtt_log.h"
#include <string.h>
#include "hz_timer.h"

/***************************** ��̬���� ***********************************/

/* ��̬���ջ����� (8KB) */
static uint8_t s_rx_buffer[ISOTP_BUFFER_SIZE];

/* ISO-TP ����ʵ�� (������) */
static isotp_connection_t s_isotp_conn;

/* ��ʼ����־ */
static bool s_isotp_initialized = false;

/* ���͵ȴ���־ (������ѭ������) */
static bool s_tx_pending = false;

/***************************** ��̬������CAN ID ���˼�¼��****************************/
#if (ISOTP_ENABLE_FILTER_RECORD == 1)

/* ��ע�� CAN ID �б� */
static const uint32_t s_filter_can_ids[ISOTP_FILTER_CAN_ID_COUNT] = ISOTP_FILTER_CAN_ID_LIST;

/* ��¼�ṹ�壨�򻯰棬��ʱ�����*/
typedef struct {
    uint32_t can_id;
    uint8_t  data[8];
    uint8_t  len;
} filter_record_t;

/* ���μ�¼������ */
static filter_record_t s_filter_records[ISOTP_FILTER_BUFFER_SIZE];
static uint16_t s_filter_record_index = 0;      /* ��ǰд��λ�� */
static uint32_t s_filter_record_count = 0;      /* �ܼ�¼���� */

/* ����¼������ Keil Watch ���ٲ鿴��*/
static uint32_t s_filter_last_can_id = 0;
static uint8_t  s_filter_last_data[8] = {0};
static uint8_t  s_filter_last_len = 0;

/* OTA ������ż����� */
static uint32_t s_ota_seq = 0;

#endif /* ISOTP_ENABLE_FILTER_RECORD */

/***************************** ��̬�������� ***********************************/

static void isotp_send_flow_control(uint8_t channel, uint32_t dst_id, uint8_t flow_status, 
                                     uint8_t block_size, uint8_t st_min);
static int8_t isotp_send_single_frame(uint8_t channel, uint32_t dst_id, uint8_t* data, uint8_t len);
static int8_t isotp_send_first_frame(uint8_t channel, uint32_t dst_id, uint8_t* data, uint16_t len);
static int8_t isotp_send_consecutive_frame(uint8_t channel, uint32_t dst_id);
static void isotp_reset_connection(void);
static void isotp_handle_flow_control_internal(uint8_t flow_status, uint8_t block_size, uint8_t st_min);

/***************************** ������������ӡ ****************************/
static void isotp_handle_flow_control_internal(uint8_t flow_status, uint8_t block_size, uint8_t st_min);
static void isotp_print_ota_frame(uint32_t can_id, uint8_t* data, uint8_t len);
static void isotp_print_filtered_frame(uint32_t can_id, uint8_t* data, uint8_t len);

/* ��ӡ���յ���CAN֡��ϸ��Ϣ */
static void isotp_print_rx_frame(uint32_t can_id, uint8_t* frame_data, uint8_t frame_len)
{
    uint8_t frame_type = frame_data[0] & 0xF0;
    uint8_t frame_info = frame_data[0] & 0x0F;
    char type_str[20];
    uint8_t i;
    char data_str[64] = {0};
    char temp[8];
    
    /* ���������ַ��� */
    for (i = 0; i < frame_len && i < 8; i++) {
        sprintf(temp, "%02X ", frame_data[i]);
        strcat(data_str, temp);
    }
    
    /* ʶ��֡���� */
    switch(frame_type) {
        case ISOTP_FRAME_SINGLE:
            sprintf(type_str, "SINGLE");
            break;
        case ISOTP_FRAME_FIRST:
            sprintf(type_str, "FIRST");
            break;
        case ISOTP_FRAME_CONSECUTIVE:
            sprintf(type_str, "CONSECUTIVE");
            break;
        case ISOTP_FRAME_FLOW_CONTROL:
            sprintf(type_str, "FLOW_CTRL");
            break;
        default:
            sprintf(type_str, "UNKNOWN");
            break;
    }
    
    ISOTP_I("[RX] CAN_ID=0x%08X, Type=%s, Len=%d, Data=%s", can_id, type_str, frame_len, data_str);
    
    /* ��ӡ������Ϣ */
    if (frame_type == ISOTP_FRAME_SINGLE) {
        ISOTP_D("[RX] Single Frame: DataLen=%d bytes", frame_info);
        if (frame_len > 1) {
            char payload_str[32] = {0};
            for (i = 1; i < frame_len && i < 8; i++) {
                sprintf(temp, "%02X ", frame_data[i]);
                strcat(payload_str, temp);
            }
            ISOTP_D("[RX] Payload: %s", payload_str);
        }
    } 
    else if (frame_type == ISOTP_FRAME_FIRST) {
        uint16_t total_len = (frame_info << 8) | frame_data[1];
        uint8_t first_data_len = (total_len > 6) ? 6 : total_len;
        ISOTP_D("[RX] First Frame: TotalLen=%d bytes, FirstDataLen=%d", total_len, first_data_len);
        if (first_data_len > 0) {
            char first_data_str[32] = {0};
            for (i = 2; i < 2 + first_data_len && i < 8; i++) {
                sprintf(temp, "%02X ", frame_data[i]);
                strcat(first_data_str, temp);
            }
            ISOTP_D("[RX] First data: %s", first_data_str);
        }
    }
    else if (frame_type == ISOTP_FRAME_CONSECUTIVE) {
        ISOTP_D("[RX] Consecutive Frame: Sequence=%d", frame_info);
        if (frame_len > 1) {
            char cf_data_str[32] = {0};
            for (i = 1; i < frame_len && i < 8; i++) {
                sprintf(temp, "%02X ", frame_data[i]);
                strcat(cf_data_str, temp);
            }
            ISOTP_D("[RX] Data: %s", cf_data_str);
        }
    }
    else if (frame_type == ISOTP_FRAME_FLOW_CONTROL) {
        uint8_t flow_status = frame_info;
        uint8_t block_size = frame_data[1];
        uint8_t st_min = frame_data[2];
        const char* status_str;
        
        switch(flow_status) {
            case ISOTP_FC_CTS: status_str = "CTS"; break;
            case ISOTP_FC_WAIT: status_str = "WAIT"; break;
            case ISOTP_FC_OVERFLOW: status_str = "OVERFLOW"; break;
            default: status_str = "UNKNOWN"; break;
        }
        ISOTP_D("[RX] Flow Control: Status=%s(%d), BlockSize=%d, STmin=%d", 
                status_str, flow_status, block_size, st_min);
    }
}

/* ��ӡ���͵�CAN֡��ϸ��Ϣ */
static void isotp_print_tx_frame(uint32_t can_id, uint8_t* frame_data, uint8_t frame_len)
{
    uint8_t frame_type = frame_data[0] & 0xF0;
    uint8_t frame_info = frame_data[0] & 0x0F;
    char type_str[20];
    uint8_t i;
    char data_str[64] = {0};
    char temp[8];
    
    /* ���������ַ��� */
    for (i = 0; i < frame_len && i < 8; i++) {
        sprintf(temp, "%02X ", frame_data[i]);
        strcat(data_str, temp);
    }
    
    /* ʶ��֡���� */
    switch(frame_type) {
        case ISOTP_FRAME_SINGLE:
            sprintf(type_str, "SINGLE");
            break;
        case ISOTP_FRAME_FIRST:
            sprintf(type_str, "FIRST");
            break;
        case ISOTP_FRAME_CONSECUTIVE:
            sprintf(type_str, "CONSECUTIVE");
            break;
        case ISOTP_FRAME_FLOW_CONTROL:
            sprintf(type_str, "FLOW_CTRL");
            break;
        default:
            sprintf(type_str, "UNKNOWN");
            break;
    }
    
    ISOTP_I("[TX] CAN_ID=0x%08X, Type=%s, Len=%d, Data=%s", can_id, type_str, frame_len, data_str);
    isotp_print_ota_frame(can_id, frame_data, frame_len);
    
    /* ��ӡ������Ϣ */
    if (frame_type == ISOTP_FRAME_SINGLE) {
        ISOTP_D("[TX] Single Frame: DataLen=%d bytes", frame_info);
        if (frame_len > 1) {
            char payload_str[32] = {0};
            for (i = 1; i < frame_len && i < 8; i++) {
                sprintf(temp, "%02X ", frame_data[i]);
                strcat(payload_str, temp);
            }
            ISOTP_D("[TX] Payload: %s", payload_str);
        }
    } 
    else if (frame_type == ISOTP_FRAME_FIRST) {
        uint16_t total_len = (frame_info << 8) | frame_data[1];
        uint8_t first_data_len = (total_len > 6) ? 6 : total_len;
        ISOTP_D("[TX] First Frame: TotalLen=%d bytes, FirstDataLen=%d", total_len, first_data_len);
        if (first_data_len > 0) {
            char first_data_str[32] = {0};
            for (i = 2; i < 2 + first_data_len && i < 8; i++) {
                sprintf(temp, "%02X ", frame_data[i]);
                strcat(first_data_str, temp);
            }
            ISOTP_D("[TX] First data: %s", first_data_str);
        }
    }
    else if (frame_type == ISOTP_FRAME_CONSECUTIVE) {
        ISOTP_D("[TX] Consecutive Frame: Sequence=%d", frame_info);
        if (frame_len > 1) {
            char cf_data_str[32] = {0};
            for (i = 1; i < frame_len && i < 8; i++) {
                sprintf(temp, "%02X ", frame_data[i]);
                strcat(cf_data_str, temp);
            }
            ISOTP_D("[TX] Data: %s", cf_data_str);
        }
    }
    else if (frame_type == ISOTP_FRAME_FLOW_CONTROL) {
        uint8_t flow_status = frame_info;
        uint8_t block_size = frame_data[1];
        uint8_t st_min = frame_data[2];
        const char* status_str;
        
        switch(flow_status) {
            case ISOTP_FC_CTS: status_str = "CTS"; break;
            case ISOTP_FC_WAIT: status_str = "WAIT"; break;
            case ISOTP_FC_OVERFLOW: status_str = "OVERFLOW"; break;
            default: status_str = "UNKNOWN"; break;
        }
        ISOTP_D("[TX] Flow Control: Status=%s(%d), BlockSize=%d, STmin=%d", 
                status_str, flow_status, block_size, st_min);
    }
}

/***************************** CAN ID ���˼�¼�������� ****************************/
#if (ISOTP_ENABLE_FILTER_RECORD == 1)

/* ��� CAN ID �Ƿ��ڹ�ע�б��� */
static bool isotp_is_can_id_filtered(uint32_t can_id)
{
    for (uint8_t i = 0; i < ISOTP_FILTER_CAN_ID_COUNT; i++) {
        if (s_filter_can_ids[i] == can_id) {
            return true;
        }
    }
    return false;
}

/* ��¼ CAN ֡�����λ��������򻯰棬��ʱ�����*/
static void isotp_record_frame(uint32_t can_id, uint8_t* data, uint8_t len)
{
    filter_record_t* record = &s_filter_records[s_filter_record_index];
    record->can_id = can_id;
    record->len = (len > 8) ? 8 : len;
    memcpy(record->data, data, record->len);
    
    /* ��������¼���������� Keil Watch �鿴��*/
    s_filter_last_can_id = can_id;
    s_filter_last_len = (len > 8) ? 8 : len;
    memcpy(s_filter_last_data, data, s_filter_last_len);
    
    s_filter_record_count++;
    s_filter_record_index++;
    if (s_filter_record_index >= ISOTP_FILTER_BUFFER_SIZE) {
        s_filter_record_index = 0;
    }
    
    /* OTA ���Դ�ӡ������ţ� */
    isotp_print_ota_frame(can_id, data, len);
}

/* OTA ���Դ�ӡ������ŵĹ�ע CAN ID ֡ */
/* OTA RX ע�͸������� */
static const char* isotp_nrc_name(uint8_t nrc)
{
    switch (nrc) {
        case 0x11: return "servNotSup";
        case 0x12: return "subNotSup";
        case 0x13: return "lenInvalid";
        case 0x22: return "condNotOK";
        case 0x24: return "reqSeqErr";
        case 0x31: return "rangeErr";
        case 0x33: return "secDenied";
        case 0x35: return "keyInvalid";
        case 0x36: return "exceedTry";
        case 0x72: return "progFail";
        case 0x78: return "pending";
        default:   return "";
    }
}

static const char* isotp_ota_annotate(uint32_t can_id, uint8_t* data)
{
    uint8_t frame_type = data[0] & 0xF0;
    uint8_t frame_info = data[0] & 0x0F;
    const uint8_t* p = &data[1];    /* SF 的 UDS payload 起始 (FF 为 data[2]) */
    static char s_buf[28];          /* 含变量字段的注解 (块号/NRC 名) */

    if (can_id == 0x18DA03F1) {
        /* ---- 工装 -> 产品 请求 ---- */
        if (frame_type == ISOTP_FRAME_CONSECUTIVE)  return "36 cont";
        if (frame_type == ISOTP_FRAME_FLOW_CONTROL) return "";
        if (frame_type == ISOTP_FRAME_FIRST) {
            p = &data[2];   /* FF: byte1 是长度低字节, UDS payload 从 byte2 起 */
        }
        switch (p[0]) {
            case 0x10: return (p[1] == 0x03U) ? "10 03 extSess" : "10 02 progSess";
            case 0x27: return (p[1] == 0x01U) ? "27 01 reqSeed" : "27 02 sendKey";
            case 0x31: return "31 FF02 enterBL";
            case 0x34: return "34 dlReq";
            case 0x36:
                sprintf(s_buf, "36 blk seq=%02X", p[1]);
                return s_buf;
            case 0x37: return "37 txExit";
            case 0x11: return "11 01 ecuReset";
            default:   return "";
        }
    }

    /* ---- 产品 -> 工装 响应 ---- */
    if (frame_type == ISOTP_FRAME_FLOW_CONTROL) {
        if (frame_info == ISOTP_FC_CTS)      return "<-- FC CTS";
        if (frame_info == ISOTP_FC_WAIT)     return "<-- FC WAIT";
        if (frame_info == ISOTP_FC_OVERFLOW) return "<-- FC OVR";
        return "<-- FC";
    }
    if (frame_type == ISOTP_FRAME_CONSECUTIVE) return "<-- 76 cont";
    if (frame_type == ISOTP_FRAME_FIRST)       return "<-- FF Resp";

    if (frame_type == ISOTP_FRAME_SINGLE) {
        uint8_t sid = data[1];
        switch (sid) {
            case 0x50: return (data[2] == 0x03U) ? "<-- 50 extSessOK" : "<-- 50 progSessOK";
            case 0x51: return "<-- 51 ResetACK";
            case 0x67: return (data[2] == 0x01U) ? "<-- 67 Seed" : "<-- 67 KeyOK";
            case 0x71: return (data[3] == 0xFFU) ? "<-- 71 bootReady" : "<-- 71 RoutineACK";
            case 0x74: return "<-- 74 DlOK";
            case 0x76: return "<-- 76 BlockACK";
            case 0x77: return "<-- 77 ExitOK";
            case 0x7F:
                sprintf(s_buf, "<-- NRC %02X %s", data[3], isotp_nrc_name(data[3]));
                return s_buf;
            default:   return "";
        }
    }

    return "";
}

static void isotp_print_ota_frame(uint32_t can_id, uint8_t* data, uint8_t len)
{
    s_ota_seq++;
    
    /* ��ȡ��ǰʱ��������룩 */
    uint64_t tick_ms = tickTimer_GetCount();
    uint32_t seconds = (uint32_t)(tick_ms / 1000);
    uint32_t milliseconds = (uint32_t)(tick_ms % 1000);
    
    /* 判断方向 (升级工装视角: 请求ID为发送, 响应ID为接收) */
    const char* direction;
    if (can_id == 0x18DA03F1) {
        direction = "[TX]";         /* 工装 -> 产品 UDS请求 */
    } else if (can_id == 0x18DAF103) {
        direction = "[RX]";         /* 产品 -> 工装 UDS响应 */
    } else if (can_id == 0x18FF8118) {
        direction = "[RX]";
    } else if (can_id == 0x18FF5858) {
        direction = "[RX]";
    } else if (can_id == 0x18DBFFF0) {
        direction = "[RX]";
    } else {
        direction = "[--]";
    }
    
    const char* ann = isotp_ota_annotate(can_id, data);
    OTA_I("seq=%-4d, time=%3u.%03us, %s 0x%08X, %02X %02X %02X %02X %02X %02X %02X %02X %s",
          s_ota_seq, seconds, milliseconds, direction, can_id,
          data[0], data[1], data[2], data[3],
          data[4], data[5], data[6], data[7], ann);
}

/* ��ӡ���˼�¼�� CAN ֡��ͳһ OTA ��ʽ��*/
static void isotp_print_filtered_frame(uint32_t can_id, uint8_t* data, uint8_t len)
{
    isotp_print_ota_frame(can_id, data, len);
}

#endif /* ISOTP_ENABLE_FILTER_RECORD */

/***************************** ����ʵ�� ***********************************/

/* ������������ */
static void isotp_reset_connection(void)
{
    ISOTP_D("Reset connection");

    s_isotp_conn.rx_state = ISOTP_RX_IDLE;
    s_isotp_conn.rx_src_id = 0;
    s_isotp_conn.rx_total_len = 0;
    s_isotp_conn.rx_received_len = 0;
    s_isotp_conn.rx_expected_seq = 1;
    s_isotp_conn.rx_cf_count_in_block = 0;
    s_isotp_conn.rx_timeout_counter = 0;
    
    s_isotp_conn.tx_state = ISOTP_TX_IDLE;
    s_isotp_conn.tx_dst_id = 0;
    s_isotp_conn.tx_buffer = NULL;
    s_isotp_conn.tx_total_len = 0;
    s_isotp_conn.tx_sent_len = 0;
    s_isotp_conn.tx_seq = 1;
    s_isotp_conn.tx_cf_count_in_block = 0;
    s_isotp_conn.tx_timeout_counter = 0;
    s_isotp_conn.tx_bs = 0;
    s_isotp_conn.tx_st_min = 0;
    s_isotp_conn.tx_st_min_counter = 0;
    
    s_tx_pending = false;
}

/* ��ʼ�� ISO-TP �� */
void isotp_init(uint8_t channel)
{
    ISOTP_I("=== ISO-TP Init Start ===");
    
    memset(&s_isotp_conn, 0, sizeof(s_isotp_conn));
    memset(s_rx_buffer, 0, sizeof(s_rx_buffer));
    
    s_isotp_conn.rx_buffer = s_rx_buffer;
    s_isotp_conn.timeout_ms = ISOTP_RX_TIMEOUT_MS;
    s_isotp_conn.channel = channel;

    isotp_reset_connection();

    OTA_D("INIT: response_id=0x%08X stmin=%d bs=%d", (unsigned long)ISOTP_DEFAULT_RESPONSE_ID, ISOTP_DEFAULT_ST_MIN_MS, ISOTP_DEFAULT_BLOCK_SIZE);
    s_isotp_initialized = true;

    ISOTP_I("BS=%d, STmin=%dms, timeout=%dms",
            ISOTP_DEFAULT_BLOCK_SIZE, ISOTP_DEFAULT_ST_MIN_MS, s_isotp_conn.timeout_ms);
    ISOTP_I("=== ISO-TP Init Done ===");
}

/* 1ms ��ʱ������ */
void isotp_ms_update(void)
{
    if (!s_isotp_initialized) {
        return;
    }
    
    /* ���ճ�ʱ��� */
    if (s_isotp_conn.rx_state == ISOTP_RX_ACTIVE) {
        if (s_isotp_conn.rx_timeout_counter > 0) {
            s_isotp_conn.rx_timeout_counter--;
            if (s_isotp_conn.rx_timeout_counter == 0) {
                ISOTP_W("RX timeout! state=%d, received=%d/%d", 
                        s_isotp_conn.rx_state, 
                        s_isotp_conn.rx_received_len, 
                        s_isotp_conn.rx_total_len);
                s_isotp_conn.rx_state = ISOTP_RX_TIMEOUT;
                isotp_reset_connection();
            }
        }
    }
    
    /* ���� STmin �ӳٴ��� */
    if (s_isotp_conn.tx_st_min_counter > 0) {
        s_isotp_conn.tx_st_min_counter--;
        if (s_isotp_conn.tx_st_min_counter == 0 && s_isotp_conn.tx_state == ISOTP_TX_SENDING_CF) {
            s_tx_pending = true;
        }
    }
    
    /* ���ͳ�ʱ��� */
    if (s_isotp_conn.tx_state != ISOTP_TX_IDLE && s_isotp_conn.tx_state != ISOTP_TX_COMPLETE) {
        if (s_isotp_conn.tx_timeout_counter > 0) {
            s_isotp_conn.tx_timeout_counter--;
            if (s_isotp_conn.tx_timeout_counter == 0) {
                ISOTP_W("TX timeout! state=%d", s_isotp_conn.tx_state);
                s_isotp_conn.tx_state = ISOTP_TX_TIMEOUT;
                isotp_reset_tx();
            }
        }
    }
}

/* ��������֡ */
static void isotp_send_flow_control(uint8_t channel, uint32_t dst_id, uint8_t flow_status,
                                     uint8_t block_size, uint8_t st_min)
{
    CAN_TSMT_FRAME_t tx_frame = {0};
    uint8_t tx_data[8];
    uint8_t i;
    
    tx_data[0] = ISOTP_FRAME_FLOW_CONTROL | flow_status;
    tx_data[1] = block_size;
    tx_data[2] = st_min;
    for (i = 3; i < 8; i++) {
        tx_data[i] = 0xAA;
    }
    
    OTA_D("FC: dst_id=0x%08X bs=%d stmin=%d", (unsigned long)dst_id, block_size, st_min);
    isotp_print_tx_frame(dst_id, tx_data, 8);
    
    can_adapter_LoadExtFrame(&tx_frame, dst_id, tx_data, 8);
    can_adapter_Transmit_Polling(channel, &tx_frame, 1);
    
    ISOTP_D("Send FC: status=%d, BS=%d, STmin=%d", flow_status, block_size, st_min);
}

/* ���͵�֡ */
static int8_t isotp_send_single_frame(uint8_t channel, uint32_t dst_id, uint8_t* data, uint8_t len)
{
    CAN_TSMT_FRAME_t tx_frame = {0};
    uint8_t tx_data[8];
    uint8_t i;
    
    ISOTP_I(">>> isotp_send_single_frame: dst_id=0x%08X, len=%d", dst_id, len);
    
    if (len > 7) {
        ISOTP_E("Single frame too long: %d > 7", len);
        return ISOTP_ERROR;
    }
    
    for (i = 0; i < 8; i++) {
        tx_data[i] = 0xAA;
    }
    
    tx_data[0] = (uint8_t)(ISOTP_FRAME_SINGLE | len);
    for (i = 0; i < len; i++) {
        tx_data[1 + i] = data[i];
    }
    
    isotp_print_tx_frame(dst_id, tx_data, 8);
    
    can_adapter_LoadExtFrame(&tx_frame, dst_id, tx_data, 8);
    can_adapter_Transmit_Polling(channel, &tx_frame, 1);
    
    ISOTP_I("Single frame sent successfully");
    return ISOTP_OK;
}

/* ������֡ */
static int8_t isotp_send_first_frame(uint8_t channel, uint32_t dst_id, uint8_t* data, uint16_t len)
{
    CAN_TSMT_FRAME_t tx_frame = {0};
    uint8_t tx_data[8];
    uint8_t i;
    
    if (len > ISOTP_MAX_MESSAGE_LEN) {
        ISOTP_E("Message too long: %d > %d", len, ISOTP_MAX_MESSAGE_LEN);
        return ISOTP_ERROR;
    }
    
    for (i = 0; i < 8; i++) {
        tx_data[i] = 0xAA;
    }
    
    tx_data[0] = (uint8_t)(ISOTP_FRAME_FIRST | ((len >> 8) & 0x0F));
    tx_data[1] = (uint8_t)(len & 0xFF);
    
    uint8_t first_data_len = (len > 6) ? 6 : len;
    for (i = 0; i < first_data_len; i++) {
        tx_data[2 + i] = data[i];
    }
    
    isotp_print_tx_frame(dst_id, tx_data, 8);
    
    can_adapter_LoadExtFrame(&tx_frame, dst_id, tx_data, 8);
    can_adapter_Transmit_Polling(channel, &tx_frame, 1);
    
    ISOTP_D("Send FF: total_len=%d, first_data=%d", len, first_data_len);
    return ISOTP_OK;
}

/* ��������֡ */
static int8_t isotp_send_consecutive_frame(uint8_t channel, uint32_t dst_id)
{
    CAN_TSMT_FRAME_t tx_frame = {0};
    uint8_t tx_data[8];
    uint8_t i;
    uint16_t remaining = s_isotp_conn.tx_total_len - s_isotp_conn.tx_sent_len;
    uint8_t cf_data_len = (remaining > 7) ? 7 : remaining;
    
    for (i = 0; i < 8; i++) {
        tx_data[i] = 0xAA;
    }
    
    tx_data[0] = (uint8_t)(ISOTP_FRAME_CONSECUTIVE | (s_isotp_conn.tx_seq & 0x0F));
    for (i = 0; i < cf_data_len; i++) {
        tx_data[1 + i] = s_isotp_conn.tx_buffer[s_isotp_conn.tx_sent_len + i];
    }
    
    isotp_print_tx_frame(dst_id, tx_data, 8);
    
    can_adapter_LoadExtFrame(&tx_frame, dst_id, tx_data, 8);
    can_adapter_Transmit_Polling(channel, &tx_frame, 1);
    
    s_isotp_conn.tx_sent_len += cf_data_len;
    s_isotp_conn.tx_seq = (s_isotp_conn.tx_seq + 1) & 0x0F;
    s_isotp_conn.tx_cf_count_in_block++;
    
    ISOTP_D("Send CF: seq=%d, data_len=%d, sent=%d/%d", 
            s_isotp_conn.tx_seq - 1, cf_data_len, 
            s_isotp_conn.tx_sent_len, s_isotp_conn.tx_total_len);
    
    return ISOTP_OK;
}

/* �ڲ����ش������� */
static void isotp_handle_flow_control_internal(uint8_t flow_status, uint8_t block_size, uint8_t st_min)
{
    ISOTP_D("Handle FC internal: tx_state=%d, flow_status=%d", s_isotp_conn.tx_state, flow_status);
    
    if (s_isotp_conn.tx_state != ISOTP_TX_SENDING_FF) {
        ISOTP_W("Unexpected FC, tx_state=%d", s_isotp_conn.tx_state);
        return;
    }
    
    if (flow_status == ISOTP_FC_CTS) {
        s_isotp_conn.tx_bs = block_size;
        s_isotp_conn.tx_st_min = st_min;
        s_isotp_conn.tx_cf_count_in_block = 0;
        s_isotp_conn.tx_state = ISOTP_TX_SENDING_CF;
        s_tx_pending = true;
        ISOTP_D("RX FC: CTS, BS=%d, STmin=%d", block_size, st_min);
    } else if (flow_status == ISOTP_FC_WAIT) {
        ISOTP_D("RX FC: WAIT");
    } else if (flow_status == ISOTP_FC_OVERFLOW) {
        ISOTP_W("RX FC: OVERFLOW");
        isotp_reset_tx();
    }
}

/* ���� CAN ֡���� */
int8_t isotp_receive_frame(uint8_t channel, uint32_t can_id, uint8_t* frame_data,
                            uint8_t frame_len, uint8_t* out_data, uint16_t* out_len)
{
    if (!s_isotp_initialized || frame_len < 1) {
        return ISOTP_ERROR;
    }

    /* ==================== CAN ID ���˼�¼ ==================== */
#if (ISOTP_ENABLE_FILTER_RECORD == 1)
    if (isotp_is_can_id_filtered(can_id)) {
        isotp_record_frame(can_id, frame_data, frame_len);
    } else {
        return ISOTP_ERROR;
    }
#endif

    /* ==================== ����֡��� ==================== */
    if (can_id == 0x18DA03F1 && frame_len >= 8)
    {
        if (frame_data[0] == 0x24 && 
            frame_data[1] == 0xD2 && 
            frame_data[2] == 0x04 && 
            frame_data[3] == 0x00 && 
            frame_data[4] == 0xB5 && 
            frame_data[5] == 0xD2 && 
            frame_data[6] == 0x04 && 
            frame_data[7] == 0x00)
        {
            __nop();
        }
    }

    /* ��ӡ���յ���֡��Ϣ */
    isotp_print_rx_frame(can_id, frame_data, frame_len);
    
    uint8_t frame_type = frame_data[0] & 0xF0;
    uint8_t frame_info = frame_data[0] & 0x0F;
    
    /* ����֡���� - ������ǰ�棬����������״̬ */
    if (frame_type == ISOTP_FRAME_FLOW_CONTROL) {
        uint8_t flow_status = frame_info;
        uint8_t block_size = frame_data[1];
        uint8_t st_min = frame_data[2];
        
        isotp_handle_flow_control_internal(flow_status, block_size, st_min);
        return ISOTP_BUSY;
    }
    
    if (s_isotp_conn.rx_state == ISOTP_RX_IDLE) {
        
        /* ��֡���� */
        if (frame_type == ISOTP_FRAME_SINGLE) {
            uint8_t sf_len = frame_info;
            if (sf_len > frame_len - 1) {
                sf_len = frame_len - 1;
            }
            if (sf_len > 0 && out_data && out_len) {
                memcpy(out_data, &frame_data[1], sf_len);
                *out_len = sf_len;
            }
            ISOTP_I("[RX] Single Frame completed: received %d bytes", sf_len);
            return ISOTP_OK;
        }
        
        /* ��֡���� */
        if (frame_type == ISOTP_FRAME_FIRST) {
            s_isotp_conn.rx_total_len = (frame_info << 8) | frame_data[1];
            
            ISOTP_I("[RX] First Frame: total_len=%d bytes", s_isotp_conn.rx_total_len);
            
            if (s_isotp_conn.rx_total_len > ISOTP_BUFFER_SIZE) {
                ISOTP_E("Message too large: %d > %d", s_isotp_conn.rx_total_len, ISOTP_BUFFER_SIZE);
                isotp_send_flow_control(channel, ISOTP_DEFAULT_RESPONSE_ID, ISOTP_FC_OVERFLOW, 0, 0);
                isotp_reset_connection();
                return ISOTP_ERROR;
            }
            
            s_isotp_conn.rx_state = ISOTP_RX_ACTIVE;
            s_isotp_conn.rx_src_id = can_id;
            s_isotp_conn.rx_received_len = 0;
            s_isotp_conn.rx_expected_seq = 1;   /* ��һ������֡�������к�Ϊ 1 */
            s_isotp_conn.rx_cf_count_in_block = 0;
            s_isotp_conn.rx_timeout_counter = s_isotp_conn.timeout_ms;
            
            uint8_t first_data_len = (s_isotp_conn.rx_total_len > 6) ? 6 : s_isotp_conn.rx_total_len;
            memcpy(s_isotp_conn.rx_buffer, &frame_data[2], first_data_len);
            s_isotp_conn.rx_received_len = first_data_len;
            
            ISOTP_I("[RX] First Frame data copied: %d bytes", first_data_len);
            
            if (s_isotp_conn.rx_received_len >= s_isotp_conn.rx_total_len) {
                s_isotp_conn.rx_state = ISOTP_RX_COMPLETE;
                
                if (out_data && out_len) {
                    memcpy(out_data, s_isotp_conn.rx_buffer, s_isotp_conn.rx_total_len);
                    *out_len = s_isotp_conn.rx_total_len;
                }
                
                ISOTP_I("[RX] Complete (all data in FF): total_len=%d", s_isotp_conn.rx_total_len);
                isotp_reset_connection();
                return ISOTP_OK;
            }
            
            OTA_D("FF: response_id=0x%08X", (unsigned long)ISOTP_DEFAULT_RESPONSE_ID);
            isotp_send_flow_control(channel, ISOTP_DEFAULT_RESPONSE_ID, ISOTP_FC_CTS,
                                     ISOTP_DEFAULT_BLOCK_SIZE, ISOTP_DEFAULT_ST_MIN_MS);
            ISOTP_I("[RX] Sent FC after FF: BS=%d, STmin=%d", 
                    ISOTP_DEFAULT_BLOCK_SIZE, ISOTP_DEFAULT_ST_MIN_MS);
            
            return ISOTP_BUSY;
        }
        
        ISOTP_W("Unexpected frame type 0x%02X in IDLE state", frame_type);
        return ISOTP_ERROR;
    }
    
    /* ������״̬����������֡ */
    if (s_isotp_conn.rx_state == ISOTP_RX_ACTIVE) {
        
        if (can_id != s_isotp_conn.rx_src_id) {
            ISOTP_W("CAN ID mismatch: expected 0x%08X, got 0x%08X", 
                    s_isotp_conn.rx_src_id, can_id);
            isotp_reset_connection();
            return ISOTP_ERROR;
        }
        
        if (frame_type != ISOTP_FRAME_CONSECUTIVE) {
            ISOTP_W("Expected CF, got frame type 0x%02X", frame_type);
            isotp_reset_connection();
            return ISOTP_ERROR;
        }
        
        uint8_t seq = frame_info;
        
        if (seq != s_isotp_conn.rx_expected_seq) {
            ISOTP_W("Sequence mismatch: expected=%d, got=%d", 
                    s_isotp_conn.rx_expected_seq, seq);
            isotp_reset_connection();
            return ISOTP_ERROR;
        }
        
        uint8_t cf_data_len = frame_len - 1;
        uint16_t remaining = s_isotp_conn.rx_total_len - s_isotp_conn.rx_received_len;
        if (cf_data_len > remaining) {
            cf_data_len = remaining;
        }
        
        memcpy(&s_isotp_conn.rx_buffer[s_isotp_conn.rx_received_len], &frame_data[1], cf_data_len);
        s_isotp_conn.rx_received_len += cf_data_len;
        
        s_isotp_conn.rx_expected_seq = (s_isotp_conn.rx_expected_seq + 1) & 0x0F;
        
        s_isotp_conn.rx_cf_count_in_block++;
        s_isotp_conn.rx_timeout_counter = s_isotp_conn.timeout_ms;
        
        ISOTP_D("[RX] CF: seq=%d, data_len=%d, received=%d/%d, cf_in_block=%d", 
                seq, cf_data_len, s_isotp_conn.rx_received_len, 
                s_isotp_conn.rx_total_len, s_isotp_conn.rx_cf_count_in_block);
        
        if (s_isotp_conn.rx_received_len >= s_isotp_conn.rx_total_len) {
            s_isotp_conn.rx_state = ISOTP_RX_COMPLETE;
            
            if (out_data && out_len) {
                memcpy(out_data, s_isotp_conn.rx_buffer, s_isotp_conn.rx_total_len);
                *out_len = s_isotp_conn.rx_total_len;
                ISOTP_I("[RX] COMPLETE: copied %d bytes to out_data", *out_len);
            }
            
            ISOTP_I("[RX] Complete: total_len=%d, received_len=%d", 
                    s_isotp_conn.rx_total_len, s_isotp_conn.rx_received_len);
            isotp_reset_connection();
            return ISOTP_OK;
        }
        
        /* ��û�������ݣ����� BUSY */
        return ISOTP_BUSY;
    }
    
    return ISOTP_ERROR;
}

/* ����������Ϣ */
int8_t isotp_send_message(uint8_t channel, uint32_t dst_id, uint8_t* data, uint16_t len)
{
    if (!s_isotp_initialized) {
        return ISOTP_ERROR;
    }
    
    if (s_isotp_conn.tx_state != ISOTP_TX_IDLE) {
        ISOTP_W("TX busy, state=%d", s_isotp_conn.tx_state);
        return ISOTP_BUSY;
    }
    
    if (data == NULL || len == 0) {
        return ISOTP_ERROR;
    }
    
    ISOTP_I("[TX] Send message: dst_id=0x%08X, len=%d", dst_id, len);
    
    /* ��ӡҪ���͵��������� */
    ISOTP_D("[TX] Data to send: ");
    for (uint16_t i = 0; i < len && i < 16; i++) {
        ISOTP_D("[TX]   data[%d]=0x%02X", i, data[i]);
    }
    if (len > 16) {
        ISOTP_D("[TX]   ... (%d more bytes)", len - 16);
    }
    
    if (len <= 7) {
        return isotp_send_single_frame(channel, dst_id, data, (uint8_t)len);
    }
    
    ISOTP_I("[TX] Using multi-frame");
    
    s_isotp_conn.tx_buffer = data;
    s_isotp_conn.tx_total_len = len;
    s_isotp_conn.tx_dst_id = dst_id;
    s_isotp_conn.tx_state = ISOTP_TX_SENDING_FF;
    s_isotp_conn.tx_timeout_counter = s_isotp_conn.timeout_ms;
    
    int8_t result = isotp_send_first_frame(channel, dst_id, data, len);
    if (result != ISOTP_OK) {
        isotp_reset_tx();
        return result;
    }
    
    s_isotp_conn.tx_sent_len = (len > 6) ? 6 : len;
    s_isotp_conn.tx_seq = 1;
    s_isotp_conn.tx_cf_count_in_block = 0;
    
    ISOTP_I("[TX] FF sent, waiting for FC, sent=%d/%d", s_isotp_conn.tx_sent_len, len);
    
    return ISOTP_BUSY;
}

/* ���ʹ������� */
void isotp_tx_process(void)
{
    if (!s_isotp_initialized) {
        return;
    }
    
    if (s_tx_pending && s_isotp_conn.tx_state == ISOTP_TX_SENDING_CF) {
        s_tx_pending = false;
        
        if (s_isotp_conn.tx_sent_len >= s_isotp_conn.tx_total_len) {
            s_isotp_conn.tx_state = ISOTP_TX_COMPLETE;
            ISOTP_I("[TX] Complete");
            isotp_reset_tx();
            return;
        }
        
        int8_t result = isotp_send_consecutive_frame(s_isotp_conn.channel, s_isotp_conn.tx_dst_id);
        if (result != ISOTP_OK) {
            ISOTP_E("[TX] CF failed");
            isotp_reset_tx();
            return;
        }
        
        if (s_isotp_conn.tx_sent_len >= s_isotp_conn.tx_total_len) {
            s_isotp_conn.tx_state = ISOTP_TX_COMPLETE;
            ISOTP_I("[TX] Complete");
            isotp_reset_tx();
            return;
        }
        
        if (s_isotp_conn.tx_bs > 0 && 
            s_isotp_conn.tx_cf_count_in_block >= s_isotp_conn.tx_bs) {
            ISOTP_D("[TX] Waiting for FC, block complete, cf_count=%d, bs=%d",
                    s_isotp_conn.tx_cf_count_in_block, s_isotp_conn.tx_bs);
            s_isotp_conn.tx_state = ISOTP_TX_SENDING_FF;
            s_isotp_conn.tx_cf_count_in_block = 0;
            return;
        }
        
        if (s_isotp_conn.tx_st_min > 0) {
            s_isotp_conn.tx_st_min_counter = s_isotp_conn.tx_st_min;
            ISOTP_D("[TX] STmin delay: %d ms", s_isotp_conn.tx_st_min);
        } else {
            s_tx_pending = true;
        }
    }
}

/* ���ý���״̬ */
void isotp_reset_rx(void)
{
    ISOTP_D("Reset RX");
    s_isotp_conn.rx_state = ISOTP_RX_IDLE;
    s_isotp_conn.rx_timeout_counter = 0;
    s_isotp_conn.rx_received_len = 0;
    s_isotp_conn.rx_expected_seq = 1;
    s_isotp_conn.rx_cf_count_in_block = 0;
}

/* ���÷���״̬ */
void isotp_reset_tx(void)
{
    ISOTP_D("Reset TX");
    s_isotp_conn.tx_state = ISOTP_TX_IDLE;
    s_isotp_conn.tx_timeout_counter = 0;
    s_isotp_conn.tx_buffer = NULL;
    s_isotp_conn.tx_sent_len = 0;
    s_isotp_conn.tx_seq = 1;
    s_isotp_conn.tx_cf_count_in_block = 0;
    s_isotp_conn.tx_st_min_counter = 0;
    s_tx_pending = false;
}

/* ��ȡ��ǰ����״̬ */
isotp_rx_state_t isotp_get_rx_state(void)
{
    return s_isotp_conn.rx_state;
}

/* ��ȡ��ǰ����״̬ */
isotp_tx_state_t isotp_get_tx_state(void)
{
    return s_isotp_conn.tx_state;
}

/* �ⲿ�ӿڣ��������յ�������֡ */
void isotp_handle_flow_control(uint8_t flow_status, uint8_t block_size, uint8_t st_min)
{
    isotp_handle_flow_control_internal(flow_status, block_size, st_min);
}

/***************************** CAN ID ���˼�¼���Խӿ�ʵ�� ****************************/
#if (ISOTP_ENABLE_FILTER_RECORD == 1)

uint32_t isotp_get_filter_record_count(void)
{
    return s_filter_record_count;
}

uint32_t isotp_get_last_filtered_can_id(void)
{
    return s_filter_last_can_id;
}

void isotp_get_last_filtered_data(uint8_t* out_data, uint8_t* out_len)
{
    if (out_data && out_len) {
        memcpy(out_data, s_filter_last_data, s_filter_last_len);
        *out_len = s_filter_last_len;
    }
}

bool isotp_get_filter_record(uint16_t index, uint32_t* can_id, uint8_t* data, uint8_t* len)
{
    if (index >= ISOTP_FILTER_BUFFER_SIZE) {
        return false;
    }
    
    /* ����ʵ��λ�ã����µ��� s_filter_record_index - 1����ǰ�� index �� */
    uint16_t record_idx;
    if (s_filter_record_index >= index + 1) {
        record_idx = s_filter_record_index - 1 - index;
    } else {
        record_idx = ISOTP_FILTER_BUFFER_SIZE + s_filter_record_index - 1 - index;
    }
    
    filter_record_t* record = &s_filter_records[record_idx];
    
    if (can_id) *can_id = record->can_id;
    if (len) *len = record->len;
    if (data) memcpy(data, record->data, record->len);
    
    return true;
}

#endif /* ISOTP_ENABLE_FILTER_RECORD */
