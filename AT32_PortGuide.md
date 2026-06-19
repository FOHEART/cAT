# cAT Library — AT32 Porting Guide

> **Target MCU**: AT32F435 (ARM Cortex-M4, 288 MHz)  
> **Toolchain**: GCC ARM Embedded (arm-none-eabi) + CMake/Ninja  
> **Reference Implementation**: `project/usersrc/user_cat_portable.c` + `project/userinc/user_cat_portable.h`

---

## Overview

The [cAT library](https://github.com/marcinbor85/cAT) is a lightweight AT command parser written in pure C. This guide explains how to port cAT to any AT32 MCU project using USART as the physical transport layer.

**Resource Usage** (measured on AT32F435):
| Component | Flash | RAM |
|-----------|-------|-----|
| cAT core (`cat.o`) | ~13 KB | 0 bytes |
| User portable layer (`user_cat_portable.o`) | ~1 KB | ~0.8 KB |

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

### Step 2: Create a Portable Layer Files

Create two files in your project:

**Header** — `project/userinc/user_cat_portable.h`:
```c
#ifndef __USER_CAT_PORTABLE_H
#define __USER_CAT_PORTABLE_H

#include <stdint.h>

void user_cat_portable_init(void);
void user_cat_portable_service(void);
void user_cat_portable_rx_isr(uint8_t byte);

#endif
```

**Source** — `project/usersrc/user_cat_portable.c`:
```c
#include "user_cat_portable.h"
#include "cat.h"
#include "at32f435_437_usart.h"
#include "at32f435_437_int.h"
```

> See the full reference at `project/usersrc/user_cat_portable.c` and `project/userinc/user_cat_portable.h`.

---

### Step 3: Implement the I/O Interface

cAT requires two low-level I/O callbacks: **read one char** and **write one char**.

#### 3.1 Write Callback (Polling TX)

Send one character over USART by polling the TX empty flag:

```c
static int cat_write_char(char ch)
{
    /* Wait for TX buffer empty */
    while ((USART1->sts & USART_TDBE_FLAG) == 0) { }

    USART1->dt = (uint32_t)ch;
    return 1;
}
```

> **Note**: For AT32, use `USART_TDBE_FLAG` and the `USART1->sts` / `USART1->dt` registers directly. Alternatively use `usart_flag_get()` + `usart_data_transmit()`.

#### 3.2 Read Callback (Ring Buffer)

Receive data via interrupt. Create a ring buffer to decouple ISR from the parser:

```c
#define CAT_RX_BUF_SIZE  128

static volatile struct {
    uint8_t  buf[CAT_RX_BUF_SIZE];
    volatile uint16_t head;
    volatile uint16_t tail;
} s_cat_rx;

/* Called from ISR */
static int rx_buf_push(uint8_t byte)
{
    uint16_t next = (s_cat_rx.head + 1) & (CAT_RX_BUF_SIZE - 1);
    if (next == s_cat_rx.tail) return -1;   /* buffer full */

    s_cat_rx.buf[s_cat_rx.head] = byte;
    s_cat_rx.head = next;
    return 0;
}

/* Called from main context */
static int rx_buf_pop(void)
{
    if (s_cat_rx.tail == s_cat_rx.head) return -1;  /* empty */

    int byte = s_cat_rx.buf[s_cat_rx.tail];
    s_cat_rx.tail = (s_cat_rx.tail + 1) & (CAT_RX_BUF_SIZE - 1);
    return byte;
}

/* cAT read callback */
static int cat_read_char(char *ch)
{
    int byte = rx_buf_pop();
    if (byte < 0) return 0;
    *ch = (char)byte;
    return 1;
}
```

> **Key points**:
> - `CAT_RX_BUF_SIZE` must be a power of 2 (for efficient masking)
> - Size 128 is sufficient for most AT command workloads
> - The ring buffer is ISR-safe: `head` is written only in ISR, `tail` only in main context

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

Define your AT commands using the `cat_command` struct:

```c
static struct cat_command s_cmds[] = {
    {
        .name        = "+LED",
        .description = "Control LEDs (RUN=all on 1Hz, WRITE=<color,freq>)",
        .write       = cmd_led_write,
        .read        = cmd_led_read,
        .run         = cmd_led_run,
        .var         = s_led_vars,       /* optional variable descriptors */
        .var_num     = 2,
        .need_all_vars = true,
    },
    {
        .name        = "+UPTIME",
        .description = "Print system uptime",
        .run         = cmd_uptime_run,
    },
    {
        .name        = "+HELP",
        .description  = "List all AT commands",
        .run          = cmd_help_run,
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

**Command Handler Prototypes**:

| Handler | AT Syntax | Purpose |
|---------|-----------|---------|
| `run` | `AT+CMD` | Execute a command, no arguments |
| `read` | `AT+CMD?` | Query current values |
| `write` | `AT+CMD=<value>` | Set values / parameters |
| `test` | `AT+CMD=?` | List supported parameters (auto-generated if variables defined) |

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

> For a complete example with all handler implementations, see `user_cat_portable.c`.

---

### Step 6: Initialize cAT in `main()`

After USART1 initialization, call the portable init function:

```c
/* In main(), after wk_usart1_init() and other peripheral setup */

/* Initialize cAT AT command parser on USART1 */
user_cat_portable_init();
```

The `user_cat_portable_init()` implementation (see `user_cat_portable.c`):

```c
void user_cat_portable_init(void)
{
    /* Reset RX ring buffer */
    s_cat_rx.head = 0;
    s_cat_rx.tail = 0;

    /* Initialize cAT parser */
    cat_init(&s_cat, &s_cat_desc, &s_cat_io, NULL);
    /*                                  ^^^^
     *  mutex interface — pass NULL in bare-metal (single-threaded) environments
     */

    /* Enable USART1 RX interrupt */
    usart_interrupt_enable(USART1, USART_RDBF_INT, TRUE);
    nvic_irq_enable(USART1_IRQn, 3, 0);
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

### Step 8: Connect the USART1 RX Interrupt

In `USART1_IRQHandler()` (typically in `project/src/at32f435_437_int.c`):

```c
void USART1_IRQHandler(void)
{
    /* Check RX data register full flag */
    if (usart_flag_get(USART1, USART_RDBF_FLAG) != RESET)
    {
        uint8_t byte = (uint8_t)usart_data_receive(USART1);
        user_cat_portable_rx_isr(byte);   /* <-- feed the byte to cAT */
    }

    /* Handle other USART1 interrupts (idle, error, etc.) as needed */
}
```

---

## Complete Integration Checklist

| # | Step | File | Done? |
|---|------|------|-------|
| 1 | Add `cAT/src/cat.c` to CMake sources | `CMakeLists.txt` | ☐ |
| 2 | Add `cAT/src` to CMake include paths | `CMakeLists.txt` | ☐ |
| 3 | Create port header | `project/userinc/user_cat_portable.h` | ☐ |
| 4 | Create port source with I/O callbacks | `project/usersrc/user_cat_portable.c` | ☐ |
| 5 | Define AT commands and variables | `user_cat_portable.c` | ☐ |
| 6 | Add `#include "user_cat_portable.h"` | `main.c` (user code zone) | ☐ |
| 7 | Call `user_cat_portable_init()` after USART1 init | `main.c` (user code begin 2) | ☐ |
| 8 | Call `user_cat_portable_service()` in main loop | `main.c` (user code begin 3) | ☐ |
| 9 | Call `user_cat_portable_rx_isr()` in USART1 IRQ | `at32f435_437_int.c` | ☐ |

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
| No response to AT commands | USART not initialized, or wrong baud rate | Check USART1 config (115200 8N1) |
| Characters echoed but no `OK` | RX interrupt not connected to cAT | Verify `user_cat_portable_rx_isr()` is called in USART1 IRQ |
| `ERROR` for valid commands | Working buffer too small | Increase `CAT_WORK_BUF_SIZE` (try 512) |
| Random characters / garbage | baud rate mismatch | Verify terminal matches USART1 baud rate |
| `AT+HELP` prints nothing | No commands registered | Check command group array and pointers |
| cAT hangs after one command | Ring buffer full | Increase `CAT_RX_BUF_SIZE` or process data faster |

---

## Reference

- **cAT Library**: [https://github.com/marcinbor85/cAT](https://github.com/marcinbor85/cAT)
- **AT32F435 Reference Manual**: Artery AT32F435/437 series
- **Project Example**: See `project/usersrc/user_cat_portable.c` for the full working implementation.
