/**
 * @file    user_cat_cmds.h
 * @brief   cAT 自定义命令组模板 —— STM32（样例：AT+LED / AT+LOG）
 * @date    2026-10-02
 *
 * 与移植层分开：这里放业务相关命令，移植层（user_cat_port.*）只负责串口收发与平台弱回调。
 *
 * 移植时改这里：
 *   - 把 `user_cat_custom_cmd_group` 改名成与你的工程对应的名字（可选）
 *   - 该命令组必须在 user_cat_port.c 的 `s_cmd_groups[]` 里注册，否则 AT 里看不到
 *   - 不需要自定义命令时可以整个文件删掉，并从 s_cmd_groups[] 移除对应项
 */

#ifndef __USER_CAT_CMDS_H__
#define __USER_CAT_CMDS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "cat.h"

/** 本样例的自定义命令组：`+LED`（读写板载 LED）、`+LOG`（读写日志过滤级别） */
extern struct cat_command_group user_cat_custom_cmd_group;

#ifdef __cplusplus
}
#endif

#endif /* __USER_CAT_CMDS_H__ */
