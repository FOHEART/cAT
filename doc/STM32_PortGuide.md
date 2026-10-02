# cAT on STM32 (HAL) — 移植指南

> **目标平台**：STM32 + STM32Cube **HAL** 驱动（CubeMX 生成的 CMake 工程）
> **样例平台（已实测）**：**STM32H563ZI** —— NUCLEO-H563ZI / 板级代号 MB1404 C01，Cortex-M33，SYSCLK 250 MHz，2 MB Flash / 640 KB RAM
> **工具链**：GCC arm-none-eabi + CMake + Ninja（STM32CubeCLT / STM32 VS Code 扩展内置工具链）
> **参考工程**：DHCap —— 其在 `Drivers/BSP/user_cat_port.{c,h}` 与 `user_cat_cmds.{c,h}` 中的实现即为本指南的可编译版本
> **可复制模板**：[`add_to_main_codespace/stm32h5/`](../add_to_main_codespace/stm32h5/) —— 与本指南配套，直接复制进你的工程

库无关的文档（特性、API、内置命令、AT 协议、弱回调契约）见 [README.md](../README.md)；
AT32 参考移植（USART + libUartMgr + DMA + RS485 半双工）见 [AT32_PortGuide.md](AT32_PortGuide.md)。

---

## 目录

- [1. 与 AT32 参考实现的差异](#1-与-at32-参考实现的差异)
- [2. 参考实现文件](#2-参考实现文件)
- [3. 接入构建系统](#3-接入构建系统)
- [4. 复制移植层模板](#4-复制移植层模板)
- [5. 实现 I/O 回调](#5-实现-io-回调)
- [6. 接上接收中断](#6-接上接收中断)
- [7. 建立描述符与命令组](#7-建立描述符与命令组)
- [8. 实现平台弱回调](#8-实现平台弱回调)
- [9. 自定义命令组](#9-自定义命令组)
- [10. 在 main 中初始化与服务](#10-在-main-中初始化与服务)
- [11. 资源占用实测](#11-资源占用实测)
- [12. 移植检查清单](#12-移植检查清单)
- [13. 常见坑](#13-常见坑)
- [14. 故障排查](#14-故障排查)
- [15. 外部参考](#15-外部参考)

---

## 1. 与 AT32 参考实现的差异

| 项 | AT32 参考（MC2010APP） | 本指南（STM32 / DHCap） |
|----|------------------------|--------------------------|
| 串口 | USART1 + libUartMgr | 任意 USART（样例 USART3 / PD8 TX、PD9 RX、921600 8N1） |
| 发送 | DMA 整帧发送 | 行缓冲 + `HAL_UART_Transmit()` 阻塞发送 |
| 接收 | DMA + 空闲线检测 | 中断逐字节 `HAL_UART_Receive_IT()` + 环形缓冲 |
| 物理层 | RS485 半双工（需方向控制） | 全双工 TTL |
| 移植层文件名 | `user_cat_portable.{c,h}` | `user_cat_port.{c,h}` |
| 自定义命令 | 与移植层写在一起 | 独立命令组 `user_cat_cmds.{c,h}` |
| 中断胶水 | libUartMgr 内部完成 | 需自己实现 HAL 回调（见第 6 步） |

**为什么选「中断 RX + 阻塞 TX」**：不依赖 DMA 通道与空闲线中断，代码量小、易审查，任意 STM32 都能照搬。代价是 TX 阻塞 —— 一条响应行在 921600 8N1 下约数百微秒，因此**不要在中断上下文里触发 AT 输出**（`cat_service()` 只在主循环调用即可自然满足这一点）。

---

## 2. 参考实现文件

| 文件 | 说明 |
|------|------|
| `Drivers/BSP/user_cat_port.h` | 移植层对外 API：`user_cat_port_init()`、`user_cat_port_service()`、`cat_write_char()` |
| `Drivers/BSP/user_cat_port.c` | I/O 回调、环形缓冲、HAL 中断回调、描述符、全部平台弱回调 |
| `Drivers/BSP/user_cat_cmds.h` | 自定义命令组声明 |
| `Drivers/BSP/user_cat_cmds.c` | 示例自定义命令（`+LED`、`+LOG`） |

模板目录 [`add_to_main_codespace/stm32h5/`](../add_to_main_codespace/stm32h5/) 与本表一一对应，可直接复制。

---

## 3. 接入构建系统

在根 `CMakeLists.txt` 中把库源码与移植层加入目标：

```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # cAT (git submodule: cAT/) - parser core + built-in commands
    cAT/src/cat.c
    cAT/src/cat_cmds.c
    # this project's port layer
    Drivers/BSP/user_cat_port.c
    Drivers/BSP/user_cat_cmds.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Drivers/BSP
    cAT/src
)
```

> **⚠ 不要改 `cmake/stm32cubemx/CMakeLists.txt`** —— 它由 CubeMX 重新生成时会整体覆盖。所有手写源码与头文件路径都加在**根** `CMakeLists.txt`。

`cAT/CMakeLists.txt` 只 `GLOB src/*.c`，用于构建库自身的测试与示例；把 cAT 作为 submodule 引入的工程**不**需要它，直接按上面显式列出源文件即可。

---

## 4. 复制移植层模板

把 [`add_to_main_codespace/stm32h5/`](../add_to_main_codespace/stm32h5/) 下的四个文件复制到你的工程（参考实现放在 `Drivers/BSP/`），然后按下面的清单逐项修改：

| # | 修改点 | 位置 |
|---|--------|------|
| 1 | 串口句柄与实例名（样例 `huart3` / `USART3`，取自 CubeMX 生成的 `usart.h`） | `user_cat_port.c` |
| 2 | 波特率 `CAT_UART_BAUDRATE`（须与 `.ioc` / `usart.c` 一致）。**主频不要写死** —— 用 `HAL_RCC_GetSysClockFreq()` 实时读取 | `user_cat_port.c` |
| 3 | 固件版本 `CAT_FW_VERSION_STR` | `user_cat_port.c` |
| 4 | 缓冲大小：`CAT_RX_BUF_SIZE` **必须是 2 的幂**（用掩码取模）；`CAT_TX_BUF_SIZE` 要放得下最长响应行 | `user_cat_port.c` |
| 5 | 日志：模板用 EasyLogger（`elog_*`）；不引入日志库时删掉 `#include <elog.h>` 与所有 `elog_*` 调用 | `user_cat_port.{c,h}` |
| 6 | 命令组：在 `s_cmd_groups[]` 中注册要用的组（内置组在前） | `user_cat_port.c` |
| 7 | 复位 / 模式 / `UARTCFG`：按目标平台需求改写或保留默认行为 | `user_cat_port.c` |

---

## 5. 实现 I/O 回调

cAT 只要求两个字节级回调。**写回调的函数名固定为 `cat_write_char`** —— 库内 `cat_cmds.c` 以 `extern int cat_write_char(char);` 引用它，改名会导致内置命令的链接失败。

**写：行缓冲 + 整行阻塞发送**

```c
#define CAT_TX_BUF_SIZE   256U
#define CAT_TX_TIMEOUT_MS 1000U

static char     s_cat_tx_buf[CAT_TX_BUF_SIZE];
static uint16_t s_cat_tx_len;

static void cat_tx_flush(void)
{
  if (s_cat_tx_len == 0U)
  {
    return;
  }

  /* 形参是 const uint8_t *，char * 必须显式转换（否则 GCC 14+ 报
     -Wincompatible-pointer-types） */
  (void)HAL_UART_Transmit(&huart3, (const uint8_t *)s_cat_tx_buf, s_cat_tx_len, CAT_TX_TIMEOUT_MS);
  s_cat_tx_len = 0U;
}

int cat_write_char(char ch)
{
  s_cat_tx_buf[s_cat_tx_len++] = ch;

  /* 遇 '\n' 或缓冲将满时整行发出：既减少 HAL 调用次数，
     也让 AT 响应与日志按「整行」交错而不是字节级交错 */
  if ((ch == '\n') || (s_cat_tx_len >= CAT_TX_BUF_SIZE))
  {
    cat_tx_flush();
  }

  return 1;  /* cAT 约定：1 = 写入成功 */
}
```

**读：从接收环形缓冲弹一个字节，无数据返回 0**

```c
#define CAT_RX_BUF_SIZE 256U                       /* 必须是 2 的幂 */
#define CAT_RX_BUF_MASK (CAT_RX_BUF_SIZE - 1U)

static uint8_t           s_cat_rx_buf[CAT_RX_BUF_SIZE];
static volatile uint16_t s_cat_rx_head;            /* 中断侧写入 */
static volatile uint16_t s_cat_rx_tail;            /* 主循环侧读取 */

static int cat_io_read(char *ch)
{
  if (s_cat_rx_tail == s_cat_rx_head)
  {
    return 0;                                      /* 无数据（非阻塞） */
  }

  *ch = (char)s_cat_rx_buf[s_cat_rx_tail];
  s_cat_rx_tail = (uint16_t)((s_cat_rx_tail + 1U) & CAT_RX_BUF_MASK);
  return 1;
}
```

> `head` / `tail` 都是 `uint16_t` 且用掩码回绕；单生产者（中断）+ 单消费者（主循环）在 32 位 MCU 上对 `uint16_t` 的读写是原子的，因此不需要临界区。

---

## 6. 接上接收中断

### 6.1 CubeMX 前置配置

在 `.ioc` 中确认（本步**只改 `.ioc` 并让 CubeMX 重新生成**，不要手写生成的初始化代码）：

- **USARTx global interrupt 已在 NVIC 中使能**（`.ioc` 里形如 `NVIC.USART3_IRQn=true:14:0`）；
- 生成的 `stm32h5xx_it.c` 中 `USART3_IRQHandler()` 调用 `HAL_UART_IRQHandler(&huart3)`；
- `HAL_UART_MspInit()` 中已 `HAL_NVIC_SetPriority()` + `HAL_NVIC_EnableIRQ()`。

### 6.2 两个 HAL 回调（缺一不可）

```c
static uint8_t s_rx_byte;   /* HAL_UART_Receive_IT 的单字节目标 */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART3)
  {
    uint16_t next = (uint16_t)((s_cat_rx_head + 1U) & CAT_RX_BUF_MASK);

    if (next != s_cat_rx_tail)
    {
      s_cat_rx_buf[s_cat_rx_head] = s_rx_byte;
      s_cat_rx_head = next;
    }
    /* else：缓冲已满 → 丢弃这个新字节，保护尚未处理的数据 */

    (void)HAL_UART_Receive_IT(&huart3, &s_rx_byte, 1U);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART3)
  {
    (void)HAL_UART_Receive_IT(&huart3, &s_rx_byte, 1U);
  }
}
```

> **⚠ 必须有 `HAL_UART_ErrorCallback()`**：一次溢出（ORE）或帧错误（FE）就会让 HAL 终止接收，若不在错误回调里重新挂接收，此后**所有** AT 命令都会静默丢失，而日志却照常输出 —— 这是最隐蔽的陷阱之一。

中断路径内**只做搬运与重新挂接收**：不打印日志、不解析命令（打印是阻塞的，会拉长中断延迟并与主循环输出交错）。

### 6.3 初始化时挂上第一个字节

```c
void user_cat_port_init(void)
{
  s_cat_tx_len = 0U;
  s_cat_rx_head = 0U;
  s_cat_rx_tail = 0U;

  cat_init(&s_cat, &s_cat_desc, &s_cat_io, NULL);

  /* 挂钩：此后每个字节由 USART3_IRQHandler -> HAL_UART_IRQHandler 推进 */
  (void)HAL_UART_Receive_IT(&huart3, &s_rx_byte, 1U);
}
```

---

## 7. 建立描述符与命令组

```c
static uint8_t s_cat_work_buf[CAT_WORK_BUF_SIZE];   /* 存放待解析的参数串，256 B 起 */

static struct cat_object s_cat;

static struct cat_command_group *s_cmd_groups[] = {
    &cat_builtin_cmd_group,          /* 库内置组（cat_cmds.c）—— 放前面 */
    &user_cat_custom_cmd_group,      /* 本工程自定义组（可选） */
};

static const struct cat_io_interface s_cat_io = {
    .write = cat_io_write,           /* 内部转发到 cat_write_char() */
    .read  = cat_io_read,
};

static const struct cat_descriptor s_cat_desc = {
    .cmd_group        = s_cmd_groups,
    .cmd_group_num    = sizeof(s_cmd_groups) / sizeof(s_cmd_groups[0]),
    .buf              = s_cat_work_buf,
    .buf_size         = sizeof(s_cat_work_buf),
    .unsolicited_buf  = NULL,        /* 不用主动上报（unsolicited）就留空 */
    .unsolicited_buf_size = 0,
};

/* 单线程裸机：mutex = NULL */
cat_init(&s_cat, &s_cat_desc, &s_cat_io, NULL);
```

`CAT_WORK_BUF_SIZE` 要能装下**最长一条命令的参数串**；不够时表现为「合法命令却返回 `ERROR`」。

---

## 8. 实现平台弱回调

`cat_cmds.h` 声明的弱回调是内置命令的运行期依赖。STM32 / HAL 版参考实现：

```c
uint32_t cat_get_sys_tick(void)      { return HAL_GetTick(); }        /* AT+INFO 的运行时间 */
uint32_t cat_get_sys_clk(void)       { return HAL_RCC_GetSysClockFreq(); }  /* 见下方说明 */
uint32_t cat_get_baudrate(void)      { return CAT_UART_BAUDRATE; }    /* AT+UARTCFG? */
const char *cat_get_fw_version(void) { return CAT_FW_VERSION_STR; }   /* AT+VER */
const char *cat_get_build_time(void) { return __DATE__ " " __TIME__; }/* AT+VER */

void cat_system_reset(void)  { NVIC_SystemReset(); }                  /* AT+RESET */
void cat_system_restore(void) { /* 有 Flash/EEPROM 配置时在此实现恢复出厂；否则留空 */ }

void cat_set_baudrate(uint32_t baudrate) { (void)baudrate; }          /* 见 cat_uartcfg_write_allowed() */
```

**`cat_get_sys_clk()` 用 `HAL_RCC_GetSysClockFreq()` 而不是常量**：该函数每次调用都从 RCC 寄存器重新计算 SYSCLK，因此时钟切换、低功耗模式换频后 `AT+INFO` 仍报正确值；返回常量在频率动态变化时会失真。它按 `stm32h5xx_hal_conf.h` 中的 `HSI_VALUE` / `HSE_VALUE` 参与计算，若与板载晶振不一致结果会偏。

**`cat_uartcfg_write_allowed()` —— 与日志共用串口时必看**：

```c
bool cat_uartcfg_write_allowed(void)
{
  /* 本工程的 AT 口同时是日志口：不允许 AT 命令改波特率
     （否则日志与会话同时失效）。AT 口独占串口时返回 true 即可。 */
  return false;
}
```

返回 `false` 时库让 `AT+UARTCFG=<n>` 直接回 `ERROR` 且不动硬件，`cat_set_baudrate()` 不会被调用。若你的 AT 口独占串口并希望支持运行期改波特率，返回 `true` 并在 `cat_set_baudrate()` 中调用 `HAL_UART_Init()` 重配。

> **⚠ `__attribute__((weak))` 只能写在定义上**：`cat_cmds.h` 中是普通声明，**不要**加 `__weak` / `__attribute__`。属性若出现在声明上，GCC 会把它传播到所有包含该头文件的定义 —— 包括你的「强」覆盖 —— 使覆盖也变成弱符号，链接器可能选中库内空桩，表现为 `AT+VER` 返回 `unknown`。

---

## 9. 自定义命令组

命令组把「变量」（`struct cat_variable`）注册成可读写的 AT 变量，`read` / `write` 是读写前后的钩子：返回 `0` 成功，返回 `-1` 让 cAT 回 `ERROR`。纯内存变量把两个钩子置 `NULL` 即可。

```c
struct cat_command_group user_cat_custom_cmd_group = {
    .name    = "custom",
    .cmd     = s_custom_cmds,
    .cmd_num = sizeof(s_custom_cmds) / sizeof(s_custom_cmds[0]),
    .disable = false,
};
```

参考实现里的两个例子：

- `+LED`：三个 `CAT_VAR_UINT_DEC` 变量（GREEN / YELLOW / RED）。`read` 钩子先 `BSP_LED_GetState()` 刷新真实状态；`write` 钩子对非法值（>1）先把变量改回真实状态再返回 `-1`，从而「报 `ERROR` 且不改硬件」。
- `+LOG`：一个 `LEVEL` 变量（0..5）。`write` 钩子调用 `elog_set_filter_lvl()`；因日志库没有读取接口，读回值用「已生效值」影子变量维护。

> 新增命令组后，**必须**在 `s_cmd_groups[]` 中注册，否则 `AT+HELP` 与解析都看不到它。

---

## 10. 在 main 中初始化与服务

```c
/* USER CODE BEGIN Includes */
#include "user_cat_port.h"
/* USER CODE END Includes */

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_USART3_UART_Init();          /* 必须先完成串口初始化 */
  /* 若移植层要打日志，日志库初始化也要在 user_cat_port_init() 之前 */
  /* USER CODE BEGIN 2 */
  user_cat_port_init();
  /* USER CODE END 2 */

  while (1)
  {
    /* USER CODE BEGIN WHILE */
    user_cat_port_service();      /* 非阻塞；无数据时立即返回 */
    /* ... 其它任务 ... */
  }
}
```

- `user_cat_port_init()` 必须在 `MX_USARTx_UART_Init()` **之后**；移植层内有日志调用时还要在日志库初始化之后。
- `user_cat_port_service()` 是主循环里的常规任务，**不要**在中断里调用。
- 主循环里的长阻塞（如 `HAL_Delay()`）会推迟 AT 响应 —— 对响应时延敏感就别在循环体里长时间阻塞。
- 所有手写代码只放在 `/* USER CODE BEGIN/END */` 之间，CubeMX 重新生成时只有这些区域会被保留。

---

## 11. 资源占用实测

样例平台的实测值（STM32H563ZI，Debug / `-O0`，含库 + 移植层 + 示例命令组）：

| 组件 | Flash | RAM |
|------|-------|-----|
| 整个 AT 通道（相对接入前） | **+29.3 KB** | **+1144 B** |
| 其中：`cat.c` 解析状态机 | ~20.5 KB | 0 |
| 其中：`cat_cmds.c` 内置命令 | ~2.0 KB | 0 |
| 其中：移植层 + 命令组 | ~1.6 KB | — |
| 其中：RX 缓冲 + TX 行缓冲 + 工作缓冲（各 256 B） | — | 768 B |

全部静态分配、无堆使用（链接映射中无新增 `malloc`）。AT32 指南中引用的「核心 ~2.5 KB」是优化构建（`-O2/-Os`）的值，Debug 构建会显著偏大 —— 用 `-O2` 可大幅回收。

---

## 12. 移植检查清单

| # | 步骤 | 文件 | 完成 |
|---|------|------|------|
| 1 | 把 `cAT/src/cat.c`、`cAT/src/cat_cmds.c` 加入构建 | 根 `CMakeLists.txt` | ☐ |
| 2 | 把 `cAT/src` 加入头文件路径 | 根 `CMakeLists.txt` | ☐ |
| 3 | 复制并修改移植层模板（含第 4 节的 7 项清单） | `Drivers/BSP/user_cat_port.{c,h}` | ☐ |
| 4 | 实现读写两个 I/O 回调，导出名保持 `cat_write_char` | `user_cat_port.c` | ☐ |
| 5 | 在 `.ioc` 中使能 USARTx 全局中断并重新生成 | `DHCap.ioc` / `stm32h5xx_it.c` | ☐ |
| 6 | 实现 `HAL_UART_RxCpltCallback()` **和** `HAL_UART_ErrorCallback()` | `user_cat_port.c` | ☐ |
| 7 | 覆盖全部平台弱回调（见第 8 步） | `user_cat_port.c` | ☐ |
| 8 | 注册命令组（内置组 + 自定义组） | `user_cat_port.c` | ☐ |
| 9 | `#include "user_cat_port.h"` | `main.c` | ☐ |
| 10 | 串口初始化之后调用 `user_cat_port_init()` | `main.c` | ☐ |
| 11 | 主循环内调用 `user_cat_port_service()` | `main.c` | ☐ |
| 12 | 编译通过，并逐条跑 `AT+VER` / `AT+INFO` / `AT+HELP` / `AT+UARTCFG?` | 串口助手 | ☐ |
| 13 | 只让库内改动提交在 cAT 仓库，子模块工作区保持干净 | `cAT/` | ☐ |

---

## 13. 常见坑

1. **忘了 `HAL_UART_ErrorCallback()`** → 一次 ORE/FE 之后 HAL 停止接收，之后所有命令无响应（日志正常）→ 见 6.2。
2. **把接收中断里的活做多了** → 在 `HAL_UART_RxCpltCallback()` 里打印日志或解析：阻塞发送拉长中断延迟，且与主循环输出字节级交错 → 中断里只搬运 + 重挂。
3. **`cat_write_char` 改名** → 内置命令链接失败；库以 `extern int cat_write_char(char);` 引用这个名字。
4. **`CAT_RX_BUF_SIZE` 不是 2 的幂** → 掩码取模失效、索引越界 → 必须 2 的幂。
5. **主频写死** → 低功耗 / 时钟切换后 `AT+INFO` 的 SCLK 失真 → 用 `HAL_RCC_GetSysClockFreq()`。
6. **与日志共用串口却不加行缓冲** → 响应与日志字节级交错，读起来像乱码 → 用 `'\n'` 触发的整行发送；并把 `cat_uartcfg_write_allowed()` 返回 `false` 保护波特率。
7. **主循环里的 `HAL_Delay()` 过长** → 命令响应被推迟（甚至看着像丢包） → 缩短阻塞，或把 `user_cat_port_service()` 放在循环体前部。
8. **`CAT_WORK_BUF_SIZE` 太小** → 长命令被截断，合法命令回 `ERROR` → 调大（256 起，必要时 512）。
9. **`__attribute__((weak))` 写到 `cat_cmds.h` 的声明上** → 你的覆盖也变弱符号，可能选中空桩（`AT+VER` 回 `unknown`） → 见第 8 步警告。
10. **手写代码写到 `USER CODE` 区之外** → CubeMX 重新生成时被覆盖丢失。
11. **改了 `cmake/stm32cubemx/CMakeLists.txt`** → 同样会被 CubeMX 覆盖；源码清单加在根 `CMakeLists.txt`。
12. **`HAL_UART_Transmit()` 传 `char *`** → GCC 14+ 报 `-Wincompatible-pointer-types` → 显式转 `(const uint8_t *)`。
13. **`__DATE__ " " __TIME__` 在未重编译时不变** → 构建时间只在编译该文件时刷新，属预期行为。

---

## 14. 故障排查

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| 完全无响应 | 串口未初始化 / 波特率不符 | 核对 USART 配置（样例 921600 8N1）与接线（TX/RX 是否交叉） |
| 一开始正常，之后所有命令都不响应（日志仍正常） | 溢出/帧错误后 HAL 停止接收 | 实现 `HAL_UART_ErrorCallback()` 并重新挂 `HAL_UART_Receive_IT()` |
| 收到字符但没有 `OK` | RX 没喂给解析器 | 确认主循环调用了 `user_cat_port_service()` |
| `AT+VER` 返回 `unknown` | `__weak` 属性从头文件声明泄漏 | 确认 `cat_cmds.h` 的声明中没有 `__attribute__((weak))` |
| `AT+INFO` 的 Uptime 恒为 `0:00:00` | `cat_get_sys_tick()` 未覆盖 | 返回真实毫秒计数（`HAL_GetTick()`） |
| `AT+INFO` 的 SCLK 不对 | `cat_get_sys_clk()` 返回常量或未覆盖 | 用 `HAL_RCC_GetSysClockFreq()` 实时读取 |
| `AT+UARTCFG?` 返回 `0` | `cat_get_baudrate()` 未覆盖 | 返回实际波特率 |
| `AT+UARTCFG=<n>` 返回 `ERROR` | 库内 `cat_uartcfg_write_allowed()` 被覆盖为 `false`（有意行为），或参数非法 | 期望允许时返回 `true`；否则确认参数是正整数 |
| `AT+RESTORE` 什么都不做 | `cat_system_restore()` 未覆盖 | 有持久化配置时在其中实现恢复出厂 |
| `AT+MODE` 写入无效 | `cat_set_mode()` / `cat_get_mode()` 未覆盖 | 让二者读写你的应用模式变量 |
| 合法命令却回 `ERROR` | 工作缓冲太小 | 调大 `CAT_WORK_BUF_SIZE`（试 512） |
| `AT+HELP` 列表为空 | 未注册命令组 | 确认 `s_cmd_groups[]` 至少含 `&cat_builtin_cmd_group` |
| 输出乱码 | 波特率不匹配 / 接线问题 | 核对串口助手与 USART 配置 |
| 长命令（>64 B）被截断 | 工作缓冲或 RX 缓冲偏小 | 调大对应缓冲 |
| 响应偶发拖延 | 主循环里有长阻塞 | 缩短 `HAL_Delay()` 等阻塞，或把 `user_cat_port_service()` 前置 |

---

## 15. 外部参考

- **cAT 库文档**：[README.md](../README.md) —— 内置命令、AT 协议、弱回调契约、单元测试
- **可复制模板**：[`add_to_main_codespace/stm32h5/`](../add_to_main_codespace/stm32h5/)
- **AT32 参考移植**：[AT32_PortGuide.md](AT32_PortGuide.md) —— USART + libUartMgr + DMA + RS485 半双工
- **STM32 参考手册 / 数据手册**：按你的型号取 ST 官方文档（样例为 STM32H563ZI + RM0481）
- **上游项目**：<https://github.com/marcinbor85/cAT>
