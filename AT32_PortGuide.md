# cAT Library — AT32 Porting Guide

> **Target MCU**: AT32F435 (ARM Cortex-M4, 288 MHz)  
> **Toolchain**: GCC ARM Embedded (arm-none-eabi) + EIDE / CMake/Ninja  
> **Module Structure**:
> - `project/usersrc/user_cat_portable.c` + `project/userinc/user_cat_portable.h` — USART+DMA hardware porting layer
> - `project/usersrc/user_cat_cmds.c` + `project/userinc/user_cat_cmds.h` — AT command handler implementations

---

## Overview

The [cAT library](https://github.com/marcinbor85/cAT) is a lightweight AT command parser written in pure C. This guide explains how to port cAT to any AT32 MCU project using USART as the physical transport layer.

The reference implementation uses **USART6 with DMA** (DMA2_CH4 for TX, DMA2_CH5 for RX with idle-line detection). A simpler polling-based USART1 variant is also described for minimum-resource targets.

**Resource Usage** (measured on AT32F435, DMA-based):
| Component | Flash | RAM |
|-----------|-------|-----|
| cAT core (`cat.o`) | ~13 KB | 0 bytes |
| User portable layer + commands | ~1.2 KB | ~1.2 KB |

---

## Porting Steps

### Step 1: Add cAT Source to Build System

Add the cAT source file and include path to your `CMakeLists.txt`:

```cmake
# cAT library sources
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/cAT/src/cat.c
)

# cAT library headers
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/cAT/src
)
```

> **Reference**: See `CMakeLists.txt` lines 39-40 and 51-52.

---

### Step 2: Create Portable Layer Files

Create the following files in your project:

**Portable layer header** — `project/userinc/user_cat_portable.h`:
```c
#ifndef __USER_CAT_PORTABLE_H
#define __USER_CAT_PORTABLE_H

#include <stdint.h>

int  cat_write_char(char ch);
void user_cat_portable_init(void);
void user_cat_portable_service(void);
void user_cat_portable_rx_isr(uint8_t byte);

#endif
```

**Portable layer source** — `project/usersrc/user_cat_portable.c`:
```c
#include "user_cat_portable.h"
#include "cat.h"
#include "at32f435_437_usart.h"
#include "at32f435_437_dma.h"
#include "at32f435_437_int.h"
```

**Command handlers header** — `project/userinc/user_cat_cmds.h`:
```c
#ifndef __USER_CAT_CMDS_H
#define __USER_CAT_CMDS_H

#include "cat.h"

cat_return_state cmd_help_run(const struct cat_command *cmd);
cat_return_state cmd_reset_run(const struct cat_command *cmd);
/* ... other handlers ... */

#endif
```

**Command handlers source** — `project/usersrc/user_cat_cmds.c`:
```c
#include "user_cat_cmds.h"
#include "user_cat_portable.h"   /* for cat_write_char() */
/* ... handler implementations ... */
```

> **Note**: Separating command handlers (`user_cat_cmds.c`) from the hardware porting layer (`user_cat_portable.c`) keeps the porting layer reusable across projects — only the commands need to change.

---

### Step 3: Implement the I/O Interface

cAT requires two low-level I/O callbacks: **read one char** and **write one char**.

Choose one of the following approaches:

#### 3.1 DMA-Based Approach (Recommended, Reference Implementation)

The reference implementation uses **USART6** with:
- **TX**: DMA2 Channel 4 (ring-buffer driven)
- **RX**: DMA2 Channel 5 (normal mode, idle-line frame detection)

All DMA-related state is grouped into a single `s_cat_dma` struct:

```c
#define CAT_DMA_RX_RING_BUF_SIZE        128
#define CAT_DMA_TX_RING_BUF_SIZE        512
#define CAT_DMA_RX_BUF_SIZE             256

static volatile struct {
    /* RX ring buffer (filled by USART6 idle-line DMA ISR) */
    struct {
        uint8_t  buf[CAT_DMA_RX_RING_BUF_SIZE];
        volatile uint16_t head;
        volatile uint16_t tail;
    } rx;

    /* TX ring buffer (consumed by DMA2_CH4) */
    struct {
        uint8_t  buf[CAT_DMA_TX_RING_BUF_SIZE];
        volatile uint16_t head;
        volatile uint16_t tail;
    } tx;

    volatile uint8_t  tx_busy;       /* DMA TX busy flag */
    volatile uint16_t tx_count;      /* cached TX byte count */
    uint8_t  rx_dma_buf[CAT_DMA_RX_BUF_SIZE];  /* DMA2_CH5 direct RX buffer */
} s_cat_dma;
```

**Write callback** — pushes to TX ring buffer, kicks DMA if idle:

```c
int cat_write_char(char ch)
{
    uint16_t next = (s_cat_dma.tx.head + 1) & (CAT_DMA_TX_RING_BUF_SIZE - 1);
    while (next == s_cat_dma.tx.tail) { }  /* wait if full */

    s_cat_dma.tx.buf[s_cat_dma.tx.head] = (uint8_t)ch;
    s_cat_dma.tx.head = next;

    if (!s_cat_dma.tx_busy)
        cat_tx_dma_kick();
    return 1;
}
```

**Read callback** — pops from RX ring buffer (filled by DMA idle-line ISR):

```c
static int cat_read_char(char *ch)
{
    if (s_cat_dma.rx.tail == s_cat_dma.rx.head)
        return 0;  /* empty */

    *ch = (char)s_cat_dma.rx.buf[s_cat_dma.rx.tail];
    s_cat_dma.rx.tail = (s_cat_dma.rx.tail + 1) & (CAT_DMA_RX_RING_BUF_SIZE - 1);
    return 1;
}
```

#### 3.2 Polling-Based Approach (Simpler, No DMA)

For targets without DMA or to minimize resource usage, use polling USART directly:

**Write callback** — polling TX empty flag:

```c
static int cat_write_char(char ch)
{
    while ((USART1->sts & USART_TDBE_FLAG) == 0) { }
    USART1->dt = (uint32_t)ch;
    return 1;
}
```

**RX ring buffer** — interrupt-driven:

```c
#define CAT_RX_BUF_SIZE  128

static volatile struct {
    uint8_t  buf[CAT_RX_BUF_SIZE];
    volatile uint16_t head;
    volatile uint16_t tail;
} s_cat_rx;

static int rx_buf_push(uint8_t byte) { /* ring buffer push */ }
static int rx_buf_pop(void)          { /* ring buffer pop  */ }

static int cat_read_char(char *ch)
{
    int byte = rx_buf_pop();
    if (byte < 0) return 0;
    *ch = (char)byte;
    return 1;
}
```

> **Key points**:
> - Ring buffer size must be a power of 2 (for efficient masking)
> - `head` is written only in ISR, `tail` only in main context — ISR-safe by design

---

### Step 4: Create the cAT Descriptor & I/O Interface Struct

```c
/* Working buffer for cAT parser */
static uint8_t s_cat_work_buf[256];

/* cAT parser instance */
static struct cat_object s_cat;

/* cAT I/O interface */
static const struct cat_io_interface s_cat_io = {
    .read  = cat_read_char,
    .write = cat_write_char,
};

/* cAT descriptor (commands + buffer) */
static const struct cat_descriptor s_cat_desc = {
    .cmd_group     = s_cmd_groups,     /* see Step 5 */
    .cmd_group_num = 1,
    .buf           = s_cat_work_buf,
    .buf_size      = sizeof(s_cat_work_buf),
};
```

> The working buffer size should be large enough to hold the longest AT command argument string. 256 bytes is sufficient for typical use cases.

---

### Step 5: Define AT Commands

Define your AT commands using the `cat_command` struct. The command array is registered in `user_cat_portable.c`, while handler implementations live in `user_cat_cmds.c`:

**Command registration** (in `user_cat_portable.c`):

```c
static struct cat_command s_cmds[] = {
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
};

static struct cat_command_group s_cmd_group = {
    .cmd     = s_cmds,
    .cmd_num = sizeof(s_cmds) / sizeof(s_cmds[0]),
};

static struct cat_command_group *s_cmd_groups[] = {
    &s_cmd_group,
};
```

**Command handlers** (in `user_cat_cmds.c`):

| Handler | AT Syntax | Purpose |
|---------|-----------|--------|
| `cmd_info_run` | `AT+INFO` | Print system clock info |
| `cmd_uptime_run` | `AT+UPTIME` | Print system uptime |
| `cmd_ver_run` | `AT+VER` | Print firmware version |
| `cmd_help_run` | `AT+HELP` | List all commands + max cmd length |
| `cmd_reset_run` | `AT+RESET` | Reset MCU |

> Separating handlers into `user_cat_cmds.c` keeps the portable layer clean and reusable.
> See `user_cat_cmds.c` for a complete example.

**Variable Descriptors** (for read/write commands with typed parameters):

```c
static struct cat_variable s_led_vars[] = {
    {
        .type      = CAT_VAR_UINT_DEC,
        .data      = &s_led_color,
        .data_size = sizeof(s_led_color),
        .name      = "color",
        .access    = CAT_VAR_ACCESS_READ_WRITE,
    },
    /* ... more variables ... */
};
```

---

### Step 6: Initialize cAT in `main()`

After USART and DMA initialization, call the portable init function:

```c
/* In main(), after USART6 + DMA2_CH4/CH5 initialization */

/* Initialize cAT AT command parser on USART6 */
user_cat_portable_init();
```

The `user_cat_portable_init()` implementation (see `user_cat_portable.c`):

```c
void user_cat_portable_init(void)
{
    /* Reset RX/TX ring buffers and DMA state */
    s_cat_dma.rx.head = 0;
    s_cat_dma.rx.tail = 0;
    s_cat_dma.tx.head = 0;
    s_cat_dma.tx.tail = 0;
    s_cat_dma.tx_busy = 0;
    s_cat_dma.tx_count = 0;

    /* Initialize cAT parser */
    cat_init(&s_cat, &s_cat_desc, &s_cat_io, NULL);
    /*                                  ^^^^
     *  mutex interface — pass NULL in bare-metal (single-threaded) environments
     */

    /* Configure DMA2_CH5 for USART6 RX (normal mode, idle-line detection) */
    /* Configure DMA2_CH4 for USART6 TX (ring buffer driven) */
    /* Enable USART6 idle-line interrupt + NVIC */
    /* ... (see full source for DMA register setup) ... */
}
```

---

### Step 7: Service cAT in the Main Loop

Call `user_cat_portable_service()` periodically in the main `while(1)` loop:

```c
while (1)
{
    /* ... other tasks ... */

    /* Process incoming AT commands */
    user_cat_portable_service();

    /* ... other tasks ... */
}
```

This calls `cat_service(&s_cat)` which runs the parser FSM — processing characters from the RX ring buffer and generating responses via the write callback.

---

### Step 8: Connect the USART6 Interrupts

In `USART6_IRQHandler()` (typically in `project/src/at32f435_437_int.c`):

```c
void USART6_IRQHandler(void)
{
    /* Check idle line flag — DMA frame reception complete */
    if (usart_flag_get(USART6, USART_IDLEF_FLAG) != RESET)
    {
        user_cat_portable_rx_idle_isr();   /* <-- process received DMA frame */
    }

    /* Handle other USART6 interrupts as needed */
}
```

And in `DMA2_Channel4_IRQHandler()` (TX complete):

```c
void DMA2_Channel4_IRQHandler(void)
{
    if (dma_flag_get(DMA2_FDT4_FLAG) != RESET)
    {
        user_cat_portable_tx_isr();   /* <-- advance TX ring buffer */
    }
}
```

> **Polling variant**: For USART1 without DMA, use `USART_RDBF_INT` and call `user_cat_portable_rx_isr(byte)` in the USART1 IRQ instead.
```

---

## Complete Integration Checklist

| # | Step | File | Done? |
|---|------|------|-------|
| 1 | Add `cAT/src/cat.c` to build sources | `CMakeLists.txt` / EIDE | ☐ |
| 2 | Add `cAT/src` to include paths | `CMakeLists.txt` / EIDE | ☐ |
| 3 | Create port header | `project/userinc/user_cat_portable.h` | ☐ |
| 4 | Create port source with I/O callbacks | `project/usersrc/user_cat_portable.c` | ☐ |
| 5 | Create command handler files | `project/userinc/user_cat_cmds.h` + `project/usersrc/user_cat_cmds.c` | ☐ |
| 6 | Define AT commands array + group | `user_cat_portable.c` (registration) | ☐ |
| 7 | Add `#include "user_cat_portable.h"` | `main.c` | ☐ |
| 8 | Call `user_cat_portable_init()` after USART+DMA init | `main.c` | ☐ |
| 9 | Call `user_cat_portable_service()` in main loop | `main.c` | ☐ |
| 10 | Connect USART6 idle-line ISR → `user_cat_portable_rx_idle_isr()` | `at32f435_437_int.c` | ☐ |
| 11 | Connect DMA2_CH4 TX ISR → `user_cat_portable_tx_isr()` | `at32f435_437_int.c` | ☐ |

---

## AT Command Protocol

The cAT parser implements the standard Hayes AT command syntax over USART:

| Command | Example | Response |
|---------|---------|----------|
| `AT+CMD` | `AT+LED` | `OK` / `ERROR` |
| `AT+CMD?` | `AT+LED?` | `+LED: 0,1` → `OK` |
| `AT+CMD=<value>` | `AT+LED=0,1` | `OK` / `ERROR` |
| `AT+CMD=?` | `AT+LED=?` | `+LED: (0-2),(0-3)` → `OK` |
| `AT+HELP` | `AT+HELP` | Command list → `OK` |

**Newline Handling**: Commands are terminated with `\r\n` (CR+LF). The parser handles `\r` as the command terminator and ignores `\n`.

---

## Troubleshooting

| Symptom | Likely Cause | Solution |
|---------|-------------|----------|
| No response to AT commands | USART not initialized, or wrong baud rate | Check USART6 config (921600 8N1 default) |
| Characters echoed but no `OK` | RX not connected to cAT | Verify `user_cat_portable_rx_idle_isr()` is called in USART6 IRQ |
| `ERROR` for valid commands | Working buffer too small | Increase `CAT_WORK_BUF_SIZE` (try 512) |
| Random characters / garbage | baud rate mismatch | Verify terminal matches USART6 baud rate |
| `AT+HELP` prints nothing | No commands registered | Check command group array and pointers |
| cAT hangs after one command | Ring buffer full | Increase `CAT_DMA_RX_RING_BUF_SIZE` or process data faster |
| DMA RX not triggering | DMA2_CH5 not properly configured | Verify DMA channel setup in `user_cat_portable_init()` |

---

## Reference

- **cAT Library**: [https://github.com/marcinbor85/cAT](https://github.com/marcinbor85/cAT)
- **AT32F435 Reference Manual**: Artery AT32F435/437 series
- **Project Reference Files**:
  - `project/usersrc/user_cat_portable.c` — USART6 DMA hardware porting
  - `project/userinc/user_cat_portable.h` — portable layer API
  - `project/usersrc/user_cat_cmds.c` — AT command handler implementations
  - `project/userinc/user_cat_cmds.h` — command handler declarations
