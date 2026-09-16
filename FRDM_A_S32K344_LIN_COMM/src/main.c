/*==================================================================================================
* Project : RTD AUTOSAR 4.9
* Platform : CORTEXM
* Peripheral : S32K3XX
* Dependencies : none
*
* Autosar Version : 4.9.0
* Autosar Revision : ASR_REL_4_9_REV_0000
* Autosar Conf.Variant :
* SW Version : 7.0.1
* Build Version : S32K3_RTD_7_0_1_D2602_ASR_REL_4_9_REV_0000_20260206
*
* Copyright 2026 NXP
*
*   NXP Proprietary. This software is owned or controlled by NXP and may only be
*   used strictly in accordance with the applicable license terms. By expressly
*   accepting such terms or by downloading, installing, activating and/or otherwise
*   using the software, you are agreeing that you have read, and that you agree to
*   comply with and are bound by, such license terms. If you do not agree to be
*   bound by the applicable license terms, then you may not retain, install,
*   activate or otherwise use the software.
==================================================================================================*/

#ifdef __cplusplus
extern "C" {
#endif

/*==================================================================================================
 *                                        INCLUDE FILES
 ==================================================================================================*/
#include "Mcal.h"
#include "Mcu.h"
#include "Port.h"
#include "Gpt.h"
#include "Lin_43_LPUART_FLEXIO.h"
#include "Platform.h"
#include "CDD_Uart.h"
#include "uart_comm.h"
#include "Lpuart_Uart_Ip.h"
#include "IntCtrl_Ip_Cfg.h"
#include "OsIf.h"
#include "stdbool.h"
#include "lin_stack_cfg.h"
#include "lin_common_api.h"
#include "lin_commontl_api.h"
#include "lin_diagnostic_service.h"

/*==================================================================================================
 *                                      DEFINES AND MACROS
 ==================================================================================================*/

/* LIN channel index */
#define T_LinChannel_0              ((uint8)0)

/* GPT timeout period - equivalent to 500 us at the configured clock */
#define PIT_PERIOD                  20000

/*==================================================================================================
 *                                      GLOBAL VARIABLES
 ==================================================================================================*/

volatile int exit_code = 0;

uint16_t length          = 8U;
uint8_t  nad             = 0U;
uint8_t  req_data[8U]    = { 0U };

/*==================================================================================================
 *                                   LOCAL FUNCTION PROTOTYPES
 ==================================================================================================*/

static void MCU_Setup(void);
static void TMR_Init(void);
static void Lin_PHY_Init(void);
static void print_char_msg(const char *prefix, uint8_t ch, const char *suffix);
void        TMR_ISR(void);
void        lin_slave_task(void);

/*==================================================================================================
 *                                       MAIN FUNCTION
 ==================================================================================================*/

/*!
 * \brief The main function for the project.
 */
int main(void)
{
    MCU_Setup();
    Uart_Init(NULL_PTR);
    TMR_Init();
    Lin_PHY_Init();
    lin_slave_task();

    for (;;)
    {
        if (exit_code != 0)
        {
            break;
        }
    }
    return exit_code;
}

/*==================================================================================================
 *                                       LOCAL FUNCTIONS
 ==================================================================================================*/

/**
 * @brief Initialize the system clock and install platform interrupt handlers.
 */
static void MCU_Setup(void)
{
    Mcu_Init(NULL_PTR);
    Mcu_InitClock(McuClockSettingConfig_0);

    while (Mcu_GetPllStatus() != MCU_PLL_LOCKED) {};

    Mcu_DistributePllClock();
    Mcu_SetMode(McuModeSettingConf_0);
    Port_Init(NULL_PTR);
    Platform_Init(NULL_PTR);
    Platform_InstallIrqHandler(LPUART5_IRQn, &LPUART5_LIN_IP_RxTx_IRQHandler, NULL_PTR);
    Platform_InstallIrqHandler(LPUART6_IRQn, &LPUART_UART_IP_6_IRQHandler, NULL_PTR);
    Platform_SetIrqPriority(LPUART6_IRQn, 12U);
    Platform_SetIrq(LPUART6_IRQn, TRUE);
    Platform_InstallIrqHandler(PIT0_IRQn, &PIT_0_ISR, NULL_PTR);
    OsIf_Init(NULL_PTR);
}

/**
 * @brief Initialize the GPT timer and enable the periodic LIN timebase notification.
 */
static void TMR_Init(void)
{
    Gpt_Init(&Gpt_Config_VS_0);
    Gpt_StartTimer(GptConf_GptChannelConfiguration_GptChannelConfiguration_0, PIT_PERIOD);
    Gpt_EnableNotification(GptConf_GptChannelConfiguration_GptChannelConfiguration_0);
}

/**
 * @brief Initialize the LIN physical interface and issue a wakeup pulse on the bus.
 */
static void Lin_PHY_Init(void)
{
    Lin_43_LPUART_FLEXIO_Init(NULL_PTR);
    Lin_43_LPUART_FLEXIO_WakeupInternal(T_LinChannel_0);
    Lin_43_LPUART_FLEXIO_Wakeup(T_LinChannel_0);
}

/**
 * @brief GPT notification callback - invoked every 500 us.
 * @details Services the LIN timeout counter.
 */
void TMR_ISR(void)
{
    lin_dal_timeout_service(LI0);
}

/**
 * @brief Build and transmit a UART message of the form: prefix + raw ASCII byte + suffix.
 * @param prefix  Null-terminated string printed before the character.
 * @param ch      Raw ASCII byte to embed in the message.
 * @param suffix  Null-terminated string printed after the character.
 */
static void print_char_msg(const char *prefix, uint8_t ch, const char *suffix)
{
    char buf[64];
    uint32_t i = 0U;
    const char *p;

    for (p = prefix; *p != '\0'; p++)
    {
        buf[i++] = *p;
    }
    buf[i++] = (char)ch;
    for (p = suffix; *p != '\0'; p++)
    {
        buf[i++] = *p;
    }
    buf[i] = '\0';

    SendString(buf);
}

/**
 * @brief LIN slave application loop.
 * @details Initializes the LIN interface, then loops forever waiting for an ASCII
 *          character from the master via the transport layer (MasterReq 0x3C, SID 0x23).
 *          Replies with the next ASCII character (received + 1, wrapping at 127 -> 32)
 *          via SlaveResp 0x3D (RSID 0x63). Status is printed over UART.
 */
void lin_slave_task(void)
{
    uint8_t rx_char;
    uint8_t tx_char;

    l_sys_init();
    ld_init(LI0);
    l_ifc_init(LI0);

    for (;;)
    {
        length = 8U;
        ld_receive_message(LI0, &length, &nad, req_data);

        if (diag_get_flag(LI0, LI0_DIAGSRV_USER_TL_ORDER))
        {
            diag_clear_flag(LI0, LI0_DIAGSRV_USER_TL_ORDER);

            /* req_data[0] = SID (0x23), req_data[1] = payload byte */
            rx_char = req_data[1];
            print_char_msg("FRDM-A-S32K344 received [", rx_char, "] from FRDM-A-S32K312 \r\n");

            /* Reply: RSID = SID + 0x40 = 0x63, payload = received + 1 */
            tx_char = (uint8_t)(rx_char + 1U);
            if (tx_char > 127U)
            {
                tx_char = 32U;
            }
            req_data[0] = 0x63U;
            req_data[1] = tx_char;

            ld_send_message(LI0, 2U, nad, req_data);
            print_char_msg("FRDM-A-S32K344 sent [", tx_char, "] to FRDM-A-S32K312 \r\n");
        }
    }
}

#ifdef __cplusplus
}
#endif

/** @} */
