/**
  ******************************************************************************
  * @file     cat_cmds.h
  * @brief    cAT built-in AT command declarations — hardware-independent
  *
  *           Provides a reusable set of built-in AT commands (INFO, UPTIME,
  *           VER, HELP, RESET) that are part of the cAT library itself.
  *
  *           Platform-specific functions (SysTick, FW version, system reset)
  *           are abstracted via __weak callbacks — override them in your
  *           project to provide real implementations.
  ******************************************************************************
  */
#ifndef CAT_CMDS_H
#define CAT_CMDS_H

#include "cat.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================*/
/*                      Built-in Command Group                                */
/*============================================================================*/

/** The built-in command group containing all standard AT commands */
extern struct cat_command_group cat_builtin_cmd_group;

/*============================================================================*/
/*                      Command Handler Declarations                          */
/*============================================================================*/

cat_return_state cmd_info_run(const struct cat_command *cmd);
cat_return_state cmd_uptime_run(const struct cat_command *cmd);
cat_return_state cmd_ver_run(const struct cat_command *cmd);
cat_return_state cmd_help_run(const struct cat_command *cmd);
cat_return_state cmd_reset_run(const struct cat_command *cmd);
cat_return_state cmd_restore_run(const struct cat_command *cmd);
cat_return_state cmd_uartcfg_read(const struct cat_command *cmd, uint8_t *data, size_t *data_size, const size_t max_data_size);
cat_return_state cmd_uartcfg_write(const struct cat_command *cmd, const uint8_t *data, const size_t data_size, const size_t args_num);

/*============================================================================*/
/*                      Platform Abstraction Callbacks (__weak)               */
/*============================================================================*/
/*
 * Override these in your project to provide platform-specific behavior.
 * Default weak stubs are defined in cat_cmds.c.
 */

/**
 * @brief Get system tick count in milliseconds.
 *        Override to return a real SysTick counter for AT+UPTIME.
 * @return Milliseconds since boot (default: 0)
 */
uint32_t cat_get_sys_tick(void);

/**
 * @brief Get firmware version string.
 *        Override to return a project-specific version for AT+VER.
 * @return Pointer to null-terminated version string (default: "unknown")
 */
const char* cat_get_fw_version(void);

/**
 * @brief Get build time string.
 *        Override to return a project-specific build timestamp for AT+VER.
 * @return Pointer to null-terminated time string (default: "unknown")
 */
const char* cat_get_build_time(void);

/**
 * @brief System reset.
 *        Override to call the platform's reset function (e.g., NVIC_SystemReset).
 *        Default: infinite empty loop.
 */
void cat_system_reset(void);

/**
 * @brief Restore factory defaults.
 *        Override to implement project-specific factory reset logic.
 *        Default: no-op (prints OK only).
 */
void cat_system_restore(void);

/**
 * @brief Get UART baudrate.
 *        Override to return the actual UART baudrate for AT+UARTCFG?.
 * @return Baudrate value (default: 0)
 */
uint32_t cat_get_baudrate(void);

/**
 * @brief Set UART baudrate.
 *        Override to reconfigure the UART with the given baudrate for AT+UARTCFG=<baud>.
 * @param baudrate Baudrate value to set
 */
void cat_set_baudrate(uint32_t baudrate);

#ifdef __cplusplus
}
#endif

#endif /* CAT_CMDS_H */
