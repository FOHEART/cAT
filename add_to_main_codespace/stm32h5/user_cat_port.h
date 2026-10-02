/**
 * @file    user_cat_port.h
 * @brief   cAT 硬件适配层模板 —— STM32 (HAL)，样例平台 STM32H563ZI + USART3
 * @date    2026-10-02
 *
 * 本文件是「复制到你的工程后按目标 MCU / 板卡修改」的参考代码，配合 doc/AT32_PortGuide.md
 * 与 README.md「Porting to a New Platform」一节使用。
 *
 * 本层职责（库源码不直接调用任何外设，硬件相关实现全部集中在这里）：
 *   - I/O 回调：read（非阻塞取字节）/ write（行缓冲，遇 '\n' 整行发送）
 *   - 接收：串口中断逐字节搬运进环形缓冲（样例：USART3 + HAL_UART_Receive_IT）
 *   - 平台弱回调：时间 / 主频 / 波特率 / 固件信息 / 复位 / 模式 / UARTCFG 写许可
 *
 * 常用修改点：
 *   1. 串口句柄与实例名（样例用 CubeMX 默认的 huart3 / USART3，见 user_cat_port.c）
 *   2. 波特率 CAT_UART_BAUDRATE（务必与时钟/IOC 配置一致）；系统主频用 HAL 的
 *      HAL_RCC_GetSysClockFreq() 实时读取，不要写死（时钟可能动态变化）
 *   3. 缓冲大小（CAT_RX_BUF_SIZE 必须是 2 的幂；CAT_TX_BUF_SIZE 要能放下一条完整响应行）
 *   4. 日志接口：样例用 EasyLogger（elog_*），不用日志库时删掉 include 与 elog_* 调用
 *   5. 是否允许 AT+UARTCFG=<baud> 改波特率：样例返回 false（AT 口同时是日志口）
 */

#ifndef __USER_CAT_PORT_H__
#define __USER_CAT_PORT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/** @defgroup cAT_STM32_Port cAT STM32 (HAL) 硬件适配层 @{ */

/**
 * @brief 初始化 cAT 解析器并挂上串口接收中断。
 * @note  需在串口初始化（如 MX_USART3_UART_Init）之后调用；
 *        若移植层内要打日志，还需在日志库初始化之后调用。
 */
void user_cat_port_init(void);

/**
 * @brief 驱动 cAT 状态机：从接收环形缓冲取字节解析并回送响应。
 * @note  在主循环中周期调用（非阻塞，无数据时立即返回）。
 *        主循环里的长阻塞（如 HAL_Delay）会推迟 AT 响应。
 */
void user_cat_port_service(void);

/**
 * @brief 向 AT 串口写一个字节（行缓冲，遇 '\n' 或缓冲满时整行阻塞发送）。
 * @param ch 待发送字节
 * @return 恒为 1（cAT 的 I/O 约定：1 = 写入成功）
 * @note  函数名与签名由库约定 —— cat_cmds.c 以 `extern int cat_write_char(char);`
 *        引用它，内置命令（AT+HELP / AT+INFO 等）的输出同样走这里。
 */
int cat_write_char(char ch);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* __USER_CAT_PORT_H__ */
