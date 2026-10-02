/**
 * @file    user_cat_cmds.c
 * @brief   cAT 自定义命令组模板实现 —— STM32（样例：AT+LED / AT+LOG）
 * @date    2026-10-02
 *
 * 演示两种常见写法，移植时照着改成你的业务：
 *   `+LED`：多个 UINT8 变量（GREEN / YELLOW / RED，值 0/1，可读可写）
 *           读回时从 BSP 刷新真实状态；写入合法值立即生效，非法值返回 ERROR 且不改硬件。
 *   `+LOG`：单个 UINT8 变量（LEVEL，0=ASSERT … 5=VERBOSE，可读可写）
 *           写入时调用日志库的运行期过滤接口。
 *
 * 移植修改清单：
 *   [1] 板级外设：样例用 STM32Cube BSP（BSP_LED_* / Led_TypeDef）——换成你的驱动 API
 *   [2] 日志库：样例用 EasyLogger（elog_*）；不用日志库时删掉 <elog.h> 与 +LOG 命令组
 *   [3] 变量回调：cat_variable.read / .write 是「读写前的钩子」，返回 0 成功、-1 让 cAT
 *       返回 ERROR；纯内存变量把 read/write 置 NULL 即可
 *
 * 说明：本命令组必须被 user_cat_port.c 的 s_cmd_groups[] 引用才会生效。
 */

#include "user_cat_cmds.h"

#include "main.h"
#include "stm32h5xx_nucleo.h" /* [1] BSP_LED_* / Led_TypeDef */
#include <elog.h>             /* [2] 可选 */

/* ================================================================== */
/*  AT+LED                                                            */
/* ================================================================== */

/** 变量顺序即 AT+LED=<GREEN>,<YELLOW>,<RED> 的参数顺序 */
static const Led_TypeDef s_led_map[] = {LED_GREEN, LED_YELLOW, LED_RED};
#define LED_NUM (sizeof(s_led_map) / sizeof(s_led_map[0]))

/** 变量存储（cAT 直接读写这里；真实状态以 BSP 为准） */
static uint8_t s_led_state[LED_NUM];

/** 由变量指针反查 LED 下标 */
static int led_index_from_var(const struct cat_variable *var)
{
  for (size_t i = 0U; i < LED_NUM; i++)
  {
    if (var->data == (void *)&s_led_state[i])
    {
      return (int)i;
    }
  }
  return -1;
}

/** 读之前从 BSP 刷新真实状态（AT+LED? 因此总是反映硬件） */
static int led_var_read(const struct cat_variable *var)
{
  int idx = led_index_from_var(var);

  if (idx < 0)
  {
    return -1;
  }

  s_led_state[idx] = (BSP_LED_GetState(s_led_map[idx]) == GPIO_PIN_SET) ? 1U : 0U;
  return 0;
}

/** 写入后立即作用到硬件；非法值（>1）恢复真实状态并让 cAT 返回 ERROR */
static int led_var_write(const struct cat_variable *var, const size_t write_size)
{
  (void)write_size;

  int idx = led_index_from_var(var);

  if (idx < 0)
  {
    return -1;
  }

  if (s_led_state[idx] > 1U)
  {
    (void)led_var_read(var); /* 把非法值改回真实状态 */
    return -1;
  }

  if (s_led_state[idx] != 0U)
  {
    (void)BSP_LED_On(s_led_map[idx]);
  }
  else
  {
    (void)BSP_LED_Off(s_led_map[idx]);
  }

  return 0;
}

static struct cat_variable led_vars[] = {
    {
        .name = "GREEN",
        .type = CAT_VAR_UINT_DEC,
        .data = &s_led_state[0],
        .data_size = sizeof(s_led_state[0]),
        .access = CAT_VAR_ACCESS_READ_WRITE,
        .write = led_var_write,
        .read = led_var_read,
    },
    {
        .name = "YELLOW",
        .type = CAT_VAR_UINT_DEC,
        .data = &s_led_state[1],
        .data_size = sizeof(s_led_state[1]),
        .access = CAT_VAR_ACCESS_READ_WRITE,
        .write = led_var_write,
        .read = led_var_read,
    },
    {
        .name = "RED",
        .type = CAT_VAR_UINT_DEC,
        .data = &s_led_state[2],
        .data_size = sizeof(s_led_state[2]),
        .access = CAT_VAR_ACCESS_READ_WRITE,
        .write = led_var_write,
        .read = led_var_read,
    },
};

/* ================================================================== */
/*  AT+LOG                                                            */
/* ================================================================== */

/* elog 只提供 elog_set_filter_lvl()，没有读取接口，因此用「已生效值」影子变量维护读回 */
static uint8_t s_log_level_applied = ELOG_LVL_VERBOSE;

/** 变量存储（AT 侧读写的值） */
static uint8_t s_log_level = ELOG_LVL_VERBOSE;

/** 读之前刷新为「已生效值」 */
static int log_var_read(const struct cat_variable *var)
{
  s_log_level = s_log_level_applied;
  (void)var;
  return 0;
}

/** 写入合法级别（0..5）→ 立即生效；越界 → 恢复旧值并返回 ERROR */
static int log_var_write(const struct cat_variable *var, const size_t write_size)
{
  (void)write_size;
  (void)var;

  if (s_log_level > ELOG_LVL_VERBOSE)
  {
    s_log_level = s_log_level_applied;
    return -1;
  }

  elog_set_filter_lvl(s_log_level);
  s_log_level_applied = s_log_level;
  return 0;
}

static struct cat_variable log_vars[] = {
    {
        .name = "LEVEL",
        .type = CAT_VAR_UINT_DEC,
        .data = &s_log_level,
        .data_size = sizeof(s_log_level),
        .access = CAT_VAR_ACCESS_READ_WRITE,
        .write = log_var_write,
        .read = log_var_read,
    },
};

/* ================================================================== */
/*  命令组                                                             */
/* ================================================================== */

static struct cat_command s_custom_cmds[] = {
    {
        .name = "+LED",
        .description = "On-board LEDs (GREEN,YELLOW,RED): 0=off 1=on",
        .var = led_vars,
        .var_num = sizeof(led_vars) / sizeof(led_vars[0]),
        .need_all_vars = false,
    },
    {
        .name = "+LOG",
        .description = "Runtime log level: 0=ASSERT 1=ERROR 2=WARN 3=INFO 4=DEBUG 5=VERBOSE",
        .var = log_vars,
        .var_num = sizeof(log_vars) / sizeof(log_vars[0]),
        .need_all_vars = false,
    },
};

struct cat_command_group user_cat_custom_cmd_group = {
    .name = "custom",
    .cmd = s_custom_cmds,
    .cmd_num = sizeof(s_custom_cmds) / sizeof(s_custom_cmds[0]),
    .disable = false,
};
