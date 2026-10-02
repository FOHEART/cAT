/**
 * @file    user_cat_port.c
 * @brief   cAT 硬件适配层模板实现 —— STM32 (HAL)，样例平台 STM32H563ZI + USART3
 * @date    2026-10-02
 *
 * 移植方式：把本文件与 user_cat_port.h 复制到你的工程（例如 Drivers/BSP/），
 * 按下面「移植修改清单」逐项改，再把源文件加进构建系统即可。
 *
 * 组成：
 *   - I/O 回调：read 从接收环形缓冲弹字节；write 走行缓冲、遇 '\n' 整行阻塞发送；
 *   - RX：串口中断逐字节搬运（ISR 内不打印、不解析），缓冲满时丢弃新字节；
 *   - 平台弱回调：时间 / 主频 / 波特率 / 固件信息 / 复位 / 模式 / UARTCFG 写许可。
 *
 * 移植修改清单：
 *   [1] 串口句柄与实例：usart.h 里 CubeMX 生成的句柄名（样例 huart3）与 USART3 实例
 *   [2] CAT_UART_BAUDRATE：与 .ioc（或你的时钟配置）一致；系统主频不写死，用 HAL
 *       的 HAL_RCC_GetSysClockFreq() 实时读取（时钟可动态变化）
 *   [3] CAT_FW_VERSION_STR：改成你的固件版本
 *   [4] 缓冲大小：CAT_RX_BUF_SIZE 必须 2 的幂；CAT_TX_BUF_SIZE 要放得下最长响应行
 *   [5] 日志：样例用 EasyLogger；不想引入日志库时删掉 <elog.h> 与所有 elog_* 调用
 *   [6] 命令组：s_cmd_groups[] 里注册你要用的组（内置组 + 自定义组）
 *   [7] 复位 / 模式 / UARTCFG：按目标平台需求实现或保留默认行为
 *
 * 与日志共存：样例中同一串口同时是日志口，两者都在主循环阻塞发送，HAL 的锁保证
 * 单次发送不被另一调用者打断；行缓冲使响应与日志按整行交错而非字节级交错。
 *
 * 构建系统集成（CMake 示例）：
 *   target_sources(${CMAKE_PROJECT_NAME} PRIVATE
 *       cAT/src/cat.c
 *       cAT/src/cat_cmds.c
 *       Drivers/BSP/user_cat_port.c
 *       Drivers/BSP/user_cat_cmds.c
 *   )
 *   target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE cAT/src)
 */

#include "user_cat_port.h"

#include <string.h>

#include "cat.h"
#include "cat_cmds.h"
#include "usart.h" /* [1] huart3：CubeMX 生成的串口头文件 */
#include <elog.h>  /* [5] 可选：不使用 EasyLogger 时删除 */
#include "user_cat_cmds.h"

/* ================================================================== */
/*  编译期常量（按目标平台修改）                                        */
/* ================================================================== */

/** 接收环形缓冲大小（必须是 2 的幂）。缓冲满时丢弃新到达的字节。 */
#define CAT_RX_BUF_SIZE     256U
#define CAT_RX_BUF_MASK     (CAT_RX_BUF_SIZE - 1U)

/** 发送行缓冲大小（需容纳一条完整的 AT 响应行）。 */
#define CAT_TX_BUF_SIZE     256U

/** cAT 解析器工作缓冲（存放待解析的命令参数字符串）。 */
#define CAT_WORK_BUF_SIZE   256U

/** 单次阻塞发送超时（ms）。 */
#define CAT_TX_TIMEOUT_MS   1000U

/** [3] AT+VER / AT+INFO 报告的信息。 */
#define CAT_FW_VERSION_STR  "DHCap"
#define CAT_FW_BUILD_TIME_STR __DATE__ " " __TIME__

/** [2] 串口波特率（与 .ioc / usart.c 一致）。 */
#define CAT_UART_BAUDRATE   921600U

/* 系统主频不写死：运行时可能因时钟切换 / 低功耗模式而变化，由 HAL_RCC_GetSysClockFreq()
 * 从 RCC 寄存器实时计算（见 cat_get_sys_clk）。 */

/* ================================================================== */
/*  静态状态（全部静态分配，无动态内存）                                 */
/* ================================================================== */

static uint8_t s_cat_rx_buf[CAT_RX_BUF_SIZE];
static volatile uint16_t s_cat_rx_head; /**< 中断侧写入位置 */
static volatile uint16_t s_cat_rx_tail; /**< 主循环侧读取位置 */

static char s_cat_tx_buf[CAT_TX_BUF_SIZE];
static uint16_t s_cat_tx_len;

static uint8_t s_cat_work_buf[CAT_WORK_BUF_SIZE];

static uint8_t s_rx_byte; /**< HAL_UART_Receive_IT 的单字节目标 */

static struct cat_object s_cat;

static cat_mode_t s_cat_mode = CAT_MODE_CONFIG;

/* ================================================================== */
/*  发送：行缓冲 + 整行阻塞发送                                         */
/* ================================================================== */

static void cat_tx_flush(void)
{
  if (s_cat_tx_len == 0U)
  {
    return;
  }

  (void)HAL_UART_Transmit(&huart3, (const uint8_t *)s_cat_tx_buf, s_cat_tx_len, CAT_TX_TIMEOUT_MS);
  s_cat_tx_len = 0U;
}

int cat_write_char(char ch)
{
  s_cat_tx_buf[s_cat_tx_len++] = ch;

  /* 整行发送：减少与日志输出的字节级交错，也避免每条响应多次 HAL 调用 */
  if ((ch == '\n') || (s_cat_tx_len >= CAT_TX_BUF_SIZE))
  {
    cat_tx_flush();
  }

  return 1;
}

/* ================================================================== */
/*  接收：中断逐字节搬运 + 环形缓冲                                     */
/* ================================================================== */

static int cat_io_read(char *ch)
{
  if (s_cat_rx_tail == s_cat_rx_head)
  {
    return 0; /* 无数据 */
  }

  *ch = (char)s_cat_rx_buf[s_cat_rx_tail];
  s_cat_rx_tail = (uint16_t)((s_cat_rx_tail + 1U) & CAT_RX_BUF_MASK);
  return 1;
}

static int cat_io_write(char ch)
{
  return cat_write_char(ch);
}

/**
 * @brief 串口接收完成回调：把字节搬进环形缓冲并重新挂接收。
 * @note  中断上下文：只做搬运，不打印、不解析。
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART3) /* [1] 目标串口实例 */
  {
    uint16_t next = (uint16_t)((s_cat_rx_head + 1U) & CAT_RX_BUF_MASK);

    if (next != s_cat_rx_tail)
    {
      s_cat_rx_buf[s_cat_rx_head] = s_rx_byte;
      s_cat_rx_head = next;
    }
    /* else：缓冲已满，丢弃新字节，保留尚未处理的数据 */

    (void)HAL_UART_Receive_IT(&huart3, &s_rx_byte, 1U);
  }
}

/**
 * @brief UART 错误回调：清错误标志并重新挂接收。
 * @note  若不处理，一次溢出/帧错误会让 HAL 终止接收，导致后续命令全部丢失。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART3) /* [1] 目标串口实例 */
  {
    (void)HAL_UART_Receive_IT(&huart3, &s_rx_byte, 1U);
  }
}

/* ================================================================== */
/*  cAT 描述符与命令组                                                 */
/* ================================================================== */

/* [6] 命令组注册顺序即 AT+HELP 的列出顺序 */
static struct cat_command_group *s_cmd_groups[] = {
    &cat_builtin_cmd_group,
    &user_cat_custom_cmd_group,
};

static const struct cat_io_interface s_cat_io = {
    .write = cat_io_write,
    .read = cat_io_read,
};

static const struct cat_descriptor s_cat_desc = {
    .cmd_group = s_cmd_groups,
    .cmd_group_num = sizeof(s_cmd_groups) / sizeof(s_cmd_groups[0]),
    .buf = s_cat_work_buf,
    .buf_size = sizeof(s_cat_work_buf),
    .unsolicited_buf = NULL,
    .unsolicited_buf_size = 0,
};

/* ================================================================== */
/*  平台弱回调实现（覆盖 cAT 库内的默认桩，原型见 cat_cmds.h）           */
/* ================================================================== */

uint32_t cat_get_sys_tick(void)
{
  return HAL_GetTick();
}

uint32_t cat_get_sys_clk(void)
{
  /* 实时读取 RCC 寄存器计算 SYSCLK，随时钟切换 / 低功耗模式动态变化（AT+INFO 用） */
  return HAL_RCC_GetSysClockFreq();
}

uint32_t cat_get_baudrate(void)
{
  return CAT_UART_BAUDRATE;
}

const char *cat_get_fw_version(void)
{
  return CAT_FW_VERSION_STR;
}

const char *cat_get_build_time(void)
{
  return CAT_FW_BUILD_TIME_STR;
}

void cat_system_reset(void)
{
  /* AT+RESET：库已先打印 OK，且行缓冲在该 '\n' 处已发送完毕 */
  NVIC_SystemReset();
}

void cat_system_restore(void)
{
  /* 样例暂无持久化配置，AT+RESTORE 返回 OK 但无副作用（有意为之）。
   * 有 Flash/EEPROM 配置时在这里实现「恢复出厂设置」。 */
  elog_w("cat", "AT+RESTORE: no persistent config to restore");
}

void cat_set_baudrate(uint32_t baudrate)
{
  /* 不会被走到：cat_uartcfg_write_allowed() 返回 false 时库直接返回 ERROR。
   * 若允许改波特率，在这里调用 HAL_UART_Init() 重配串口。 */
  (void)baudrate;
}

bool cat_uartcfg_write_allowed(void)
{
  /* 样例中同一串口同时承载日志输出，不允许 AT 命令改波特率（否则日志与会话同时失效）。
   * AT 口独占串口时可返回 true 恢复默认行为。 */
  return false;
}

cat_mode_t cat_get_mode(void)
{
  return s_cat_mode;
}

void cat_set_mode(cat_mode_t mode)
{
  s_cat_mode = mode;
  elog_i("cat", "AT+MODE set to %d", (int)mode);
}

/* ================================================================== */
/*  对外接口                                                           */
/* ================================================================== */

void user_cat_port_init(void)
{
  s_cat_tx_len = 0U;
  s_cat_rx_head = 0U;
  s_cat_rx_tail = 0U;

  cat_init(&s_cat, &s_cat_desc, &s_cat_io, NULL);

  /* 挂上接收：此后每个字节由串口 IRQHandler -> HAL_UART_IRQHandler 回调推进 */
  (void)HAL_UART_Receive_IT(&huart3, &s_rx_byte, 1U);
}

void user_cat_port_service(void)
{
  (void)cat_service(&s_cat);
}
