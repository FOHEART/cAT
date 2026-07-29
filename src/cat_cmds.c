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
};

struct cat_command_group cat_builtin_cmd_group = {
    .name     = "builtin",
    .cmd      = s_cmds,
    .cmd_num  = sizeof(s_cmds) / sizeof(s_cmds[0]),
};

static struct cat_command_group *s_cmd_groups[] = {
    &cat_builtin_cmd_group,
};
