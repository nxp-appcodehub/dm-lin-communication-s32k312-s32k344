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
#include "Mcl.h"
#include "Port.h"
#include "CDD_Uart.h"
#include "Lpuart_Uart_Ip_Irq.h"
#include "uart_comm.h"
#include "Gpt.h"
#include "Lin_43_LPUART_FLEXIO.h"
#include "Platform.h"
#include "IntCtrl_Ip_Cfg.h"
#include "Lpuart_Uart_Ip.h"
#include "stdbool.h"
#include "lin_stack_cfg.h"
#include "lin_common_api.h"
#include "lin_commontl_api.h"
#include "lin_diagnostic_service.h"
#include "OsIf.h"

/*==================================================================================================
 *                                      DEFINES AND MACROS
 ==================================================================================================*/

/* LIN channel index */
#define T_LinChannel_0  ((uint8)0)

/* GPT timeout period - equivalent to 500 us at the configured clock */
#define PIT_PERIOD      15000U

/*==================================================================================================
 *                                      GLOBAL VARIABLES
 ==================================================================================================*/

volatile int exit_code = 0;

/* Slave node address (configured NAD = 0x02) */
l_u8 NAD = 0x02U;

/*==================================================================================================
 *                                   LOCAL FUNCTION PROTOTYPES
 ==================================================================================================*/

static void MCU_Setup(void);
static void TMR_Init(void);
static void Lin_PHY_Init(void);
static void DelayMs(uint32 ms);
static void print_char_msg(const char *prefix, l_u8 ch, const char *suffix);
void        TMR_ISR(void);
void        lin_master_task(void);

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
    lin_master_task();

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
    Mcu_Init(&Mcu_Config);
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
    Gpt_Init(&Gpt_Config);
    Gpt_StartTimer(GptConf_GptChannelConfiguration_GptChannelConfiguration_0, PIT_PERIOD);
    Gpt_EnableNotification(GptConf_GptChannelConfiguration_GptChannelConfiguration_0);
}

/**
 * @brief Initialize the LIN physical interface and issue a wakeup pulse on the bus.
 */
static void Lin_PHY_Init(void)
{
    Lin_43_LPUART_FLEXIO_Init(&Lin_43_LPUART_FLEXIO_xConfig);
    Lin_43_LPUART_FLEXIO_WakeupInternal(T_LinChannel_0);
    Lin_43_LPUART_FLEXIO_Wakeup(T_LinChannel_0);
}

/**
 * @brief GPT notification callback - invoked every 500 us.
 * @details Services the LIN timeout counter. Every 5 ms (10 ticks) the master
 *          schedule tick l_sch_tick() is called to advance the LIN schedule table.
 */
void TMR_ISR(void)
{
    static uint32_t interruptCount = 0UL;

    lin_dal_timeout_service(LI0);

    if (++interruptCount > 9UL)
    {
        l_sch_tick(LI0);
        interruptCount = 0UL;
    }
}

/**
 * @brief Build and transmit a UART message of the form: prefix + raw ASCII byte + suffix.
 * @param prefix  Null-terminated string printed before the character.
 * @param ch      Raw ASCII byte to embed in the message.
 * @param suffix  Null-terminated string printed after the character.
 */
static void print_char_msg(const char *prefix, l_u8 ch, const char *suffix)
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
 * @brief LIN master application loop.
 * @details Initializes the LIN interface, sets the NormalTable schedule, then loops
 *          forever sending a cycling ASCII character (0x20..0x7F, wrapping) to the
 *          slave via the LIN transport layer (MasterReq 0x3C, SID 0x23). Waits for
 *          LD_COMPLETED, receives the slave reply (SlaveResp 0x3D, RSID 0x63 + next
 *          char), and prints both results over UART.
 */
void lin_master_task(void)
{
    l_u8 tx_char = 32U;
    /* tx_buf[0] = SID 0x23, tx_buf[1] = payload byte */
    l_u8 tx_buf[2U] = {0x23U, 32U};
    l_u8 rx_buf[8U] = {0U};
    l_u16 rx_len;
    l_u8 rx_nad;
    l_u8 tx_status;
    l_u8 rx_status;

    l_sys_init();
    ld_init(LI0);
    l_ifc_init(LI0);
    l_sch_set(LI0, LI0_NormalTable, 0u);

    for (;;)
    {
        tx_buf[0] = 0x23U;
        tx_buf[1] = tx_char;

        ld_send_message(LI0, 2U, NAD, tx_buf);

        do
        {
            tx_status = ld_tx_status(LI0);
        } while (tx_status == LD_IN_PROGRESS);

        if (tx_status == LD_COMPLETED)
        {
            print_char_msg("FRDM-A-S32K312 sent [", tx_char, "] [OK]\r\n");
        }
        else
        {
            print_char_msg("FRDM-A-S32K312 sent [", tx_char, "] [FAIL]\r\n");
        }

        rx_len = 8U;
        rx_nad = NAD;
        ld_receive_message(LI0, &rx_len, &rx_nad, rx_buf);

        do
        {
            rx_status = ld_rx_status(LI0);
        } while (rx_status == LD_IN_PROGRESS);

        /* rx_buf[0] = RSID (0x63), rx_buf[1] = echoed character + 1 */
        if (rx_status == LD_COMPLETED)
        {
            print_char_msg("FRDM-A-S32K312 received [", rx_buf[1], "] from FRDM-A-S32K344 \r\n");
        }
        else
        {
            print_char_msg("FRDM-A-S32K312 received [", rx_buf[1], "] from FRDM-A-S32K344 [FAIL]\r\n");
        }

        tx_char++;
        if (tx_char > 127U)
        {
            tx_char = 32U;
        }

        DelayMs(200);
    }
}

/**
 * @brief Millisecond delay function using OsIf timer
 *
 * @param ui32TimeoutMs  Delay time in milliseconds
 */
static void DelayMs(uint32 ui32TimeoutMs) {
	uint32 ui32CurTime = OsIf_GetCounter(OSIF_COUNTER_SYSTEM);
	uint32 ui32ElapsedTicks = 0U;
	uint32 ui32TimeoutTicks = OsIf_MicrosToTicks(ui32TimeoutMs * 1000U,
			OSIF_COUNTER_SYSTEM);

	while (ui32ElapsedTicks < ui32TimeoutTicks) {
		ui32ElapsedTicks += OsIf_GetElapsed(&ui32CurTime, OSIF_COUNTER_SYSTEM);
	}
}

#ifdef __cplusplus
}
#endif

/** @} */
