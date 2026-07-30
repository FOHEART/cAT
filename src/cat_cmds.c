/**
  ******************************************************************************
  * @file     cat_cmds.c
  * @brief    cAT built-in AT command implementations — hardware-independent
  *
  *           Commands: INFO, UPTIME, VER, HELP, RESET
  *
  *           Platform dependencies (SysTick, FW version, system reset) are
  *           abstracted via __attribute__((weak)) callbacks. Override them
  *           in your project to provide real implementations.
  *
  *           Output goes through cat_write_char(), which must be provided
  *           by the portable layer.
  ******************************************************************************
  */

#include "cat_cmds.h"
#include "cat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*============================================================================*/
/*                      Platform Abstraction Callbacks (Weak Defaults)        */
/*============================================================================*/

__attribute__((weak)) uint32_t cat_get_sys_tick(void)
{
    return 0;
}

__attribute__((weak)) const char* cat_get_fw_version(void)
{
    return "unknown";
}

__attribute__((weak)) const char* cat_get_build_time(void)
{
    return "unknown";
}

__attribute__((weak)) void cat_system_reset(void)
{
    /* Default: spin forever — override to call NVIC_SystemReset() etc. */
    while (1);
}

__attribute__((weak)) void cat_system_restore(void)
{
    /* Default: no-op — override to implement factory reset logic */
}

__attribute__((weak)) uint32_t cat_get_baudrate(void)
{
    /* Default: return 0 — override to return actual UART baudrate */
    return 0;
}

__attribute__((weak)) void cat_set_baudrate(uint32_t baudrate)
{
    /* Default: no-op — override to reconfigure UART hardware */
    (void)baudrate;
}

/*============================================================================*/
/*                      Helper: print string via cat_write_char               */
/*============================================================================*/

static void cat_print(const char *str)
{
    while (*str)
        cat_write_char(*str++);
}

/*============================================================================*/
/*                      AT+INFO                                               */
/*============================================================================*/

cat_return_state cmd_info_run(const struct cat_command *cmd)
{
    (void)cmd;

    cat_print("System Information\r\n");
    cat_print("SCLK: 48000000 Hz\r\n");
    cat_print("AHB:  48000000 Hz\r\n");
    cat_print("APB1: 48000000 Hz\r\n");
    cat_print("APB2: 48000000 Hz\r\n");
    cat_print("APB3: 12000000 Hz\r\n");

    return CAT_RETURN_STATE_OK;
}

/*============================================================================*/
/*                      AT+UPTIME                                             */
/*============================================================================*/

cat_return_state cmd_uptime_run(const struct cat_command *cmd)
{
    (void)cmd;

    char buf[32];
    unsigned int ms = cat_get_sys_tick();
    unsigned int sec  = ms / 1000;
    unsigned int min  = sec / 60;
    unsigned int hr   = min / 60;

    sec  %= 60;
    min  %= 60;

    int n = snprintf(buf, sizeof(buf), "Uptime: %u:%02u:%02u\r\n", hr, min, sec);
    for (int i = 0; i < n && i < (int)sizeof(buf); i++)
        cat_write_char(buf[i]);

    return CAT_RETURN_STATE_OK;
}

/*============================================================================*/
/*                      AT+VER                                                */
/*============================================================================*/

cat_return_state cmd_ver_run(const struct cat_command *cmd)
{
    (void)cmd;

    cat_print("FW Version: ");
    cat_print(cat_get_fw_version());
    cat_write_char('\r');
    cat_write_char('\n');

    cat_print("Build Time: ");
    cat_print(cat_get_build_time());
    cat_write_char('\r');
    cat_write_char('\n');

    return CAT_RETURN_STATE_OK;
}

/*============================================================================*/
/*                      AT+HELP                                               */
/*============================================================================*/

cat_return_state cmd_help_run(const struct cat_command *cmd)
{
    (void)cmd;

    /* cAT built-in: prints all registered commands + descriptions */
    return CAT_RETURN_STATE_PRINT_CMD_LIST_OK;
}

/*============================================================================*/
/*                      AT+RESET                                              */
/*============================================================================*/

cat_return_state cmd_reset_run(const struct cat_command *cmd)
{
    (void)cmd;

    cat_print("OK\r\n");

    /* Flush TX buffer before reset */
    /* (cat_write_char flushes on '\n', so "OK\r\n" should already be sent) */

    cat_system_reset();

    /* Should never reach here */
    return CAT_RETURN_STATE_OK;
}

/*============================================================================*/
/*                      AT+RESTORE                                            */
/*============================================================================*/

cat_return_state cmd_restore_run(const struct cat_command *cmd)
{
    (void)cmd;

    cat_print("OK\r\n");

    cat_system_restore();

    return CAT_RETURN_STATE_OK;
}

/*============================================================================*/
/*                      AT+UARTCFG? (read baudrate)                           */
/*============================================================================*/

cat_return_state cmd_uartcfg_read(const struct cat_command *cmd, uint8_t *data, size_t *data_size, const size_t max_data_size)
{
    (void)cmd;

    uint32_t baud = cat_get_baudrate();

    int n = snprintf((char *)data, max_data_size, "+UARTCFG:%lu\r\n", (unsigned long)baud);
    if (n > 0 && (size_t)n < max_data_size)
        *data_size = (size_t)n;

    return CAT_RETURN_STATE_DATA_OK;
}

cat_return_state cmd_uartcfg_write(const struct cat_command *cmd, const uint8_t *data, const size_t data_size, const size_t args_num)
{
    (void)cmd;
    (void)args_num;

    /* Parse baudrate from argument string */
    char buf[16];
    size_t len = data_size < sizeof(buf) - 1 ? data_size : sizeof(buf) - 1;
    memcpy(buf, (const char *)data, len);
    buf[len] = '\0';

    unsigned long baud = strtoul(buf, NULL, 10);
    if (baud == 0)
        return CAT_RETURN_STATE_ERROR;

    cat_set_baudrate((uint32_t)baud);

    return CAT_RETURN_STATE_OK;
}

/*============================================================================*/
/*                      Command Table & Group Definition                      */
/*============================================================================*/

static const struct cat_command s_cmds[] = {
    {
        .name        = "+INFO",
        .description = "Print system information",
        .run         = cmd_info_run,
    },
    {
        .name        = "+UPTIME",
        .description = "Print system uptime",
        .run         = cmd_uptime_run,
    },
    {
        .name        = "+VER",
        .description = "Print firmware version",
        .run         = cmd_ver_run,
    },
    {
        .name        = "+HELP",
        .description = "List all AT commands",
        .run         = cmd_help_run,
    },
    {
        .name        = "+RESET",
        .description = "Reset MCU",
        .run         = cmd_reset_run,
    },
    {
        .name        = "+RESTORE",
        .description = "Restore factory defaults",
        .run         = cmd_restore_run,
    },
    {
        .name        = "+UARTCFG",
        .description = "Configure UART baudrate",
        .read        = cmd_uartcfg_read,
        .write       = cmd_uartcfg_write,
    },
};

struct cat_command_group cat_builtin_cmd_group = {
    .name     = "builtin",
    .cmd      = s_cmds,
    .cmd_num  = sizeof(s_cmds) / sizeof(s_cmds[0]),
};

static struct cat_command_group *s_cmd_groups[] = {
    &cat_builtin_cmd_group,
};
