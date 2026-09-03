
  #include "main.h"
  #include "Hardware.h"
  #include "rtt_log.h"
  #include "timer6_timebase.h"
  #include "Motor_hall.h"
  #include "TickTimer.h"
  #include "device_manager.h"
  #include "App_Motor_Project.h"
  #include "param_manager.h"
  #include "Gpio_io.h"
  #include "App_Comm.h"
  #include "Params.h"
  #include "App_FaultHandler.h"
  #include "rtt_manager.h"
  #include "Pwm.h"
  #include "hc32_ll_utility.h"
  #include "upgrade_tool.h"
#include "Bootloader_App.h"


  /*=============================================================================
   * ȫ��PWMʵ�������������ʹ�ã�????
   *=============================================================================*/
  pwm_t g_motor_pwm_ch1;  // PB6
  pwm_t g_motor_pwm_ch2;  // PB7
  pwm_t g_motor_pwm_ch3;  // PB8
  pwm_t g_motor_pwm_ch4;  // PB9

  /*=============================================================================
   * ��������
   *=============================================================================*/
  static void Motor_Pwm_Init(void);

  /*=============================================================================
   * ��ʼ����������õ�PWM��4��ͨ����ȫ������Ч��
   *=============================================================================*/
  static void Motor_Pwm_Init(void)
  {
      // ����������?4��ͨ��ȫ������Ч������תͨ��ռ�ձȷ���ʵ�֣�
      // Ƶ�ʣ�20kHz����ʼռ�ձȣ�0%

      // ����GPIO���裨�����޸�GPIO�������ã�
      LL_PERIPH_WE(LL_PERIPH_GPIO);

      // CH1: PB6 - ����Ч
      g_motor_pwm_ch1 = PWM_Init(CM_TMRA_4, FCG2_PERIPH_TMRA_4, TMRA_CH1,
                                  GPIO_PORT_B, GPIO_PIN_06, GPIO_FUNC_4,
                                  TMRA_MD_SAWTOOTH, TMRA_DIR_UP,
                                  6000, 0, PWM_ACTIVE_LOW);

      // CH2: PB7 - ����Ч
      g_motor_pwm_ch2 = PWM_Init(CM_TMRA_4, FCG2_PERIPH_TMRA_4, TMRA_CH2,
                                  GPIO_PORT_B, GPIO_PIN_07, GPIO_FUNC_4,
                                  TMRA_MD_SAWTOOTH, TMRA_DIR_UP,
                                  6000, 0, PWM_ACTIVE_LOW);

      // CH3: PB8 - ����Ч
      g_motor_pwm_ch3 = PWM_Init(CM_TMRA_4, FCG2_PERIPH_TMRA_4, TMRA_CH3,
                                  GPIO_PORT_B, GPIO_PIN_08, GPIO_FUNC_4,
                                  TMRA_MD_SAWTOOTH, TMRA_DIR_UP,
                                  6000, 0, PWM_ACTIVE_LOW);

      // CH4: PB9 - ����Ч
      g_motor_pwm_ch4 = PWM_Init(CM_TMRA_4, FCG2_PERIPH_TMRA_4, TMRA_CH4,
                                  GPIO_PORT_B, GPIO_PIN_09, GPIO_FUNC_4,
                                  TMRA_MD_SAWTOOTH, TMRA_DIR_UP,
                                  6000, 0, PWM_ACTIVE_LOW);

      // ����GPIO���裨������ú�������????
      LL_PERIPH_WP(LL_PERIPH_GPIO);

      // ����FCG���裨ʹ�ܶ�ʱ��ʱ�ӣ�
      LL_PERIPH_WE(LL_PERIPH_FCG);

      // ��������PWM��ʱ��
      PWM_Start(&g_motor_pwm_ch1);
      PWM_Start(&g_motor_pwm_ch2);
      PWM_Start(&g_motor_pwm_ch3);
      PWM_Start(&g_motor_pwm_ch4);

      // ʹ�����????
      PWM_OutputCmd(&g_motor_pwm_ch1, PWM_OUTPUT_ENABLE);
      PWM_OutputCmd(&g_motor_pwm_ch2, PWM_OUTPUT_ENABLE);
      PWM_OutputCmd(&g_motor_pwm_ch3, PWM_OUTPUT_ENABLE);
      PWM_OutputCmd(&g_motor_pwm_ch4, PWM_OUTPUT_ENABLE);

      // ����FCG����
      LL_PERIPH_WP(LL_PERIPH_FCG);

      MAIN_D("Motor PWM initialized: 4 channels, 20kHz, low active\r\n");
  }

  /*=============================================================================
   * 调试功能开�?
   *=============================================================================*/

  /* CAN 心跳包：1=开启（每秒发�? 0x12345678），0=关闭 */
  #define CAN_HEARTBEAT_ENABLE       (0U)

/* UDS/CAN 功能总开关：1=启用 UDS 诊断+CAN 通信�?0=仅保�? Bootloader/APP */
/* UDS_CAN_ENABLE moved to main.h */


  /*=============================================================================
   * ������
   *=============================================================================*/

/*=============================================================================
 * 升级工装固件 (单一固件直接运行, 不跳转 APP)
 * 固件镜像存放在本片 0x44000 起始 168KB, 由 Keil/JFlash 单独烧录
 *=============================================================================*/
int main(void)
{
    Hardware_Init();
    MAIN_D("===== main(): UPGRADE TOOL PATH =====\r\n");

    /* 上电自动启动心跳监听与升级流程 */
    Tool_Init();

    {
        while (1)
        {
            Tool_Poll();
        }
    }
}
