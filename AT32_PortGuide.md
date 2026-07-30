# cAT Library — AT32 Porting Guide

> **Target MCU**: AT32F422GBU7-4 (ARM Cortex-M4, 120 MHz, 128 KB Flash, 20 KB RAM)  
> **Toolchain**: GCC ARM Embedded (arm-none-eabi) + EIDE (CMake/Ninja backend)  
> **Module Structure**:
> - `project/usersrc/user_cat_portable.c` + `project/userinc/user_cat_portable.h` — USART + libUartMgr portable layer
> - `cAT/src/cat_cmds.c` + `cAT/src/cat_cmds.h` — built-in AT commands (hardware-independent, part of cAT library)
> - `cAT/src/cat.c` + `cAT/src/cat.h` — cAT core parser

---

## Overview

The [cAT library](https://github.com/marcinbor85/cAT) is a lightweight AT command parser written in pure C. This guide explains how to port cAT to any AT32 MCU project using USART as the physical transport layer.

The reference implementation (MC2010APP) uses **USART1 with libUartMgr** (DMA TX + DMA RX with idle-line detection, RS485 half-duplex). The portable layer buffers TX bytes and flushes as a single DMA transaction on `'\n'` to minimize RS485 direction-toggle overhead.

**Resource Usage** (measured on AT32F422, libUartMgr-based):
| Component | Flash | RAM |
|-----------|-------|-----|
| cAT core + built-in commands (`cat.o` + `cat_cmds.o`) | ~2.5 KB | 0 bytes |
| User portable layer (`user_cat_portable.o`) | ~0.6 KB | ~260 B (TX buffer) |

---

## Porting Steps

### Step 1: Add cAT Source to Build System

Add the cAT source files and include path to your `CMakeLists.txt`:

```cmake
# cAT — AT command parser
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    cAT/src/cat.c
    # cAT — built-in commands (hardware-independent)
    cAT/src/cat_cmds.c
    # cAT user portable layer
    project/usersrc/user_cat_portable.c
)

# cAT library headers
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    cAT/src
)
```

> **EIDE**: The same sources must be listed in your builder params / EIDE source list.
> **Reference**: See `CMakeLists.txt` lines 56-62, `build/Debug/builder.params` sourceList.

---

### Step 2: Create Portable Layer Files

Create the following files in your project:

**Portable layer header** — `project/userinc/user_cat_portable.h`:
```c
#ifndef __USER_CAT_PORTABLE_H
#define __USER_CAT_PORTABLE_H

#include <stdint.h>

void user_cat_portable_init(void);
void user_cat_portable_service(void);

/* I/O callback — used by cAT internally and by command handlers */
int cat_write_char(char ch);

#endif
```

**Portable layer source** — `project/usersrc/user_cat_portable.c`:
```c
#include "user_cat_portable.h"
#include "cat.h"
#include "cat_cmds.h"
#include "uart_mgr.h"
/* ... other project includes ... */
```

> **Note**: There are **no separate `user_cat_cmds.c/h` files** needed if using the built-in commands from `cAT/src/cat_cmds.c`. The built-in commands provide `AT+INFO`, `AT+UPTIME`, `AT+VER`, `AT+HELP`, `AT+RESET`, `AT+RESTORE`, and `AT+UARTCFG` — see Step 5 for details. If you need custom commands, implement them alongside the built-in group.

---

### Step 3: Implement the I/O Interface

cAT requires two low-level I/O callbacks: **read one char** and **write one char**.

The MC2010APP reference implementation uses **libUartMgr** (USART1 + DMA TX/RX + RS485) with a **line-buffered TX** strategy. This is the recommended approach for half-duplex RS485 buses.

#### 3.1 Line-Buffered TX with libUartMgr (Reference — MC2010APP)

To avoid one DMA+RS485 transaction per character (which would be extremely inefficient on half-duplex RS485), outgoing bytes are buffered locally and flushed via `uart_mgr_send_data()` when a `'\n'` terminator is seen or the buffer fills up.

**TX ring buffer** (file-scope static):

```c
#define CAT_TX_BUF_SIZE    256

static struct {
    uint8_t  buf[CAT_TX_BUF_SIZE];
    uint16_t len;
} s_cat_tx;

static void cat_tx_flush(void)
{
    if (s_cat_tx.len == 0)
        return;
    uart_mgr_send_data(&uart1_hw, (const char *)s_cat_tx.buf, s_cat_tx.len);
    s_cat_tx.len = 0;
}
```

**Write callback** — buffers, flushes on `'\n'`:

```c
int cat_write_char(char ch)
{
    s_cat_tx.buf[s_cat_tx.len++] = (uint8_t)ch;

    /* Flush on newline or when buffer is full */
    if (ch == '\n' || s_cat_tx.len >= CAT_TX_BUF_SIZE)
        cat_tx_flush();

    return 1;
}
```

**Read callback** — reads from libUartMgr RX ring buffer:

```c
static int cat_read_char(char *ch)
{
    uint8_t byte;
    int32_t ret = uart_mgr_read_rx_data(&uart1_hw, &byte, 1);
    if (ret <= 0)
        return 0;   /* no data */

    *ch = (char)byte;
    return 1;
}
```

> **Key points**:
> - RS485 half-duplex direction control is handled automatically by `uart_mgr_send_data()` — no manual DE/RE pin toggling needed
> - `uart1_hw` is defined in `uart_mgr_portable.c` — declare `extern uart_hw_TypeDef uart1_hw;`
> - The TX buffer must be large enough to hold a full AT response line (256 bytes is sufficient)

#### 3.2 Direct DMA Ring Buffers (Alternative)

If you are **not** using libUartMgr and want to manage DMA directly, implement a pair of ring buffers (TX + RX) with DMA kick-off on TX and idle-line ISR for RX frame detection. See the earlier revision of this guide or the cAT library examples for the raw DMA approach.

#### 3.3 Polling-Based Approach (Simplest, No DMA)

For targets without DMA or to minimize resource usage, use polling USART directly:

```c
static int cat_write_char(char ch)
{
    while ((USART1->sts & USART_TDBE_FLAG) == 0) { }
    USART1->dt = (uint32_t)ch;
    return 1;
}
```

RX requires an interrupt-driven ring buffer (see the `cAT/example/` directory for reference).

---

### Step 4: Create the cAT Descriptor & I/O Interface Struct

```c
/* Working buffer for cAT parser (holds argument string being parsed) */
static uint8_t s_cat_work_buf[256];

/* cAT parser instance */
static struct cat_object s_cat;

/* cAT I/O interface */
static const struct cat_io_interface s_cat_io = {
    .read  = cat_read_char,     /* from Step 3 */
    .write = cat_write_char,    /* from Step 3 */
};

/* Command groups: built-in commands + project-specific groups */
static struct cat_command_group *s_cmd_groups[] = {
    &cat_builtin_cmd_group,     /* defined in cAT/src/cat_cmds.c */
};
```

> The working buffer size should be large enough to hold the longest AT command argument string. 256 bytes is sufficient for typical use cases.

---

### Step 5: Use the Built-in AT Commands

The `cAT/src/cat_cmds.c` file provides a **reusable command group** (`cat_builtin_cmd_group`) with the following built-in commands. Uptime information is included in `AT+INFO` output (no separate `AT+UPTIME`). All commands are hardware-independent — platform-specific behavior is provided via __weak callbacks (see Step 6):

| Handler | AT Syntax | Purpose |
|---------|-----------|--------|
| `cmd_info_run` | `AT+INFO` | Print system clock info |
| *(merged into `cmd_info_run`)* | *(see `AT+INFO`)* | Uptime is printed as part of `AT+INFO` |
| `cmd_ver_run` | `AT+VER` | Print firmware version + build time |
| `cmd_help_run` | `AT+HELP` | List all registered commands |
| `cmd_reset_run` | `AT+RESET` | Reset MCU |
| `cmd_restore_run` | `AT+RESTORE` | Restore factory defaults |
| `cmd_uartcfg_read` | `AT+UARTCFG?` | Query UART baudrate |
| `cmd_uartcfg_write` | `AT+UARTCFG=<baud>` | Set UART baudrate |

**Registration** — in `user_cat_portable.c`, reference the external command group:

```c
static struct cat_command_group *s_cmd_groups[] = {
    &cat_builtin_cmd_group,   /* from cat_cmds.c */
};
```

**Adding custom commands**: Create additional command groups in your own source files and add them to the `s_cmd_groups` array:

```c
extern struct cat_command_group my_custom_cmd_group;

static struct cat_command_group *s_cmd_groups[] = {
    &cat_builtin_cmd_group,
    &my_custom_cmd_group,
};
```

---

### Step 6: Override Platform Callbacks (`__weak`)

The built-in commands depend on four platform-specific callbacks declared in `cat_cmds.h`. These are defined as **weak symbols** in `cat_cmds.c` — override them in your portable layer to provide real implementations.

| Callback | Weak Default | Purpose | Override Example |
|----------|-------------|---------|-----------------|
| `cat_get_sys_tick()` | Returns `0` | Milliseconds since boot (for `AT+UPTIME`) | `return getSysTick();` |
| `cat_get_fw_version()` | Returns `"unknown"` | Firmware version string (for `AT+VER`) | `return user_fwVer_GetVersionString();` |
| `cat_get_build_time()` | Returns `"unknown"` | Build timestamp string (for `AT+VER`) | `return user_fwVer_GetBuildTimeString();` |
| `cat_system_reset()` | Spins forever | System reset (for `AT+RESET`) | `NVIC_SystemReset();` |
| `cat_system_restore()` | No-op | Factory defaults restore (for `AT+RESTORE`) | `user_flash_erase_config(); NVIC_SystemReset();` |
| `cat_get_baudrate()` | Returns `0` | Get UART baudrate (for `AT+UARTCFG?`) | `return 921600;` |
| `cat_set_baudrate(baud)` | No-op | Set UART baudrate (for `AT+UARTCFG=<n>`) | `usart_init(USART1, baudrate, ...);` |

**Implementation** (in `user_cat_portable.c`):

```c
uint32_t cat_get_sys_tick(void)
{
    return getSysTick();
}

const char* cat_get_fw_version(void)
{
    return user_fwVer_GetVersionString();
}

const char* cat_get_build_time(void)
{
    return user_fwVer_GetBuildTimeString();
}

void cat_system_reset(void)
{
    NVIC_SystemReset();
}

void cat_system_restore(void)
{
    /* Override with factory reset logic, e.g.:
     * user_flash_erase_config();
     * NVIC_SystemReset(); */
}

uint32_t cat_get_baudrate(void)
{
    return 921600;  /* match USART1 init baudrate */
}

void cat_set_baudrate(uint32_t baudrate)
{
    /* Reconfigure USART1 with the new baudrate */
    usart_init(USART1, baudrate, USART_DATA_8BITS, USART_STOP_1_BIT);
}
```

> **⚠ Critical — `__attribute__((weak))` on declarations**:  
> The `cat_cmds.h` header declares these functions. The `__attribute__((weak))` MUST be placed **only on the definitions** in `cat_cmds.c`, **NOT** on the declarations in `cat_cmds.h`.  
> If `__attribute__((weak))` appears on the declaration in the header, GCC propagates it to all definitions that include that header — including your "strong" overrides — making them weak too. The linker may then pick the stub instead of your override, causing `AT+VER` to return `"unknown"` even though your override is linked.  
> **Fixed in this project**: `cat_cmds.h` declarations are clean (no `__attribute__((weak))`); the attribute is only on the definitions in `cat_cmds.c`.

---

### Step 7: Initialize cAT in `main()`

After USART + DMA + libUartMgr initialization, call the portable init function:

```c
/* In main(), after uart_mgr_create() and DMA IRQ setup */

/* Initialize cAT AT command parser on USART1 */
user_cat_portable_init();
```

The `user_cat_portable_init()` implementation (see `user_cat_portable.c`):

```c
void user_cat_portable_init(void)
{
    /* Reset TX buffer */
    s_cat_tx.len = 0;

    /* Build cAT descriptor */
    static const struct cat_descriptor s_cat_desc = {
        .cmd_group     = s_cmd_groups,
        .cmd_group_num = 1,                     /* add more groups as needed */
        .buf           = s_cat_work_buf,
        .buf_size      = sizeof(s_cat_work_buf),
    };

    /* Initialize cAT parser (mutex = NULL for bare-metal single-threaded) */
    cat_init(&s_cat, &s_cat_desc, &s_cat_io, NULL);
}
```

---

### Step 8: Service cAT in the Main Loop

Call `user_cat_portable_service()` periodically in the main `while(1)` loop:

```c
while (1)
{
    /* ... other tasks ... */

    /* Service cAT AT command parser (reads from libUartMgr RX ring) */
    user_cat_portable_service();

    /* ... other tasks ... */
}
```

This calls `cat_service(&s_cat)` which runs the parser FSM — processing characters from the RX ring buffer and generating responses via the write callback.

---

### Step 9: No Extra Interrupt Handlers Needed (with libUartMgr)

libUartMgr handles all USART and DMA interrupts internally. You do **not** need to write custom ISR glue for cAT when using libUartMgr — the existing DMA TX complete, DMA RX half/full, and USART idle-line interrupts are already wired in `uart_mgr_portable.c` and `at32f422_426_int.c`.

If you are **not** using libUartMgr and manage DMA directly, you will need to wire:
- USART idle-line ISR → push received frame data into the cAT RX ring buffer
- DMA TX complete ISR → advance the TX ring buffer

---

## Complete Integration Checklist

| # | Step | File | Done? |
|---|------|------|-------|
| 1 | Add `cAT/src/cat.c`, `cAT/src/cat_cmds.c` to build sources | `CMakeLists.txt` / EIDE builder.params | ☐ |
| 2 | Add `cAT/src` to include paths | `CMakeLists.txt` / EIDE builder.params | ☐ |
| 3 | Create port header | `project/userinc/user_cat_portable.h` | ☐ |
| 4 | Create port source with I/O callbacks | `project/usersrc/user_cat_portable.c` | ☐ |
| 5 | Override `__weak` callbacks (7 callbacks total — see Step 6) | `user_cat_portable.c` | ☐ |
| 6 | Add `#include "user_cat_portable.h"` | `main.c` | ☐ |
| 7 | Call `user_cat_portable_init()` after USART/DMA init | `main.c` | ☐ |
| 8 | Call `user_cat_portable_service()` in main loop | `main.c` | ☐ |
| 9 | **(libUartMgr)** Ensure USART1/DMA IRQs are wired | `at32f422_426_int.c` (already done) | ☐ |
| 10 | **(raw DMA)** Wire USART idle-line ISR + DMA TX ISR | `at32fxxx_int.c` | ☐ |

---

## AT Command Protocol

The cAT parser implements the standard Hayes AT command syntax over USART:

| Command | Example | Response |
|---------|---------|----------|
| `AT+CMD` | `AT+INFO` | Text → `OK` |
| `AT+CMD?` | `AT+UARTCFG?` | `+UARTCFG:<baud>` → `OK` |
| `AT+CMD=<value>` | `AT+UARTCFG=115200` | `OK` |
| `AT+CMD=?` | `AT+LED=?` | `+LED: (0-2),(0-3)` → `OK` |
| `AT+HELP` | `AT+HELP` | Command list → `OK` |

**Newline Handling**: Commands are terminated with `\r\n` (CR+LF). The parser handles `\r` as the command terminator and ignores `\n`.

---

## Troubleshooting

| Symptom | Likely Cause | Solution |
|---------|-------------|----------|
| No response to AT commands | USART not initialized, or wrong baud rate | Check USART1 config (921600 8N1 default) |
| Characters echoed but no `OK` | RX not feeding cAT parser | Verify `user_cat_portable_service()` is called in main loop |
| `AT+VER` returns `"unknown"` | `__weak` attribute leaked from header | Ensure `cat_cmds.h` declarations do NOT have `__attribute__((weak))` (see Step 6 ⚠) |
| `AT+UPTIME` returns `0:00:00` | `cat_get_sys_tick()` returns 0 | Override `cat_get_sys_tick()` to return real SysTick count |
| `AT+UARTCFG?` returns `0` | `cat_get_baudrate()` not overridden | Override `cat_get_baudrate()` to return actual baudrate |
| `AT+UARTCFG=<n>` returns `ERROR` | Baudrate value is 0 or invalid | Ensure argument is a positive integer (e.g., `AT+UARTCFG=115200`) |
| `AT+UARTCFG=<n>` changes nothing | `cat_set_baudrate()` not overridden | Override `cat_set_baudrate()` to reconfigure USART hardware |
| `AT+RESTORE` does nothing | `cat_system_restore()` not overridden | Override `cat_system_restore()` with factory reset logic |
| `ERROR` for valid commands | Working buffer too small | Increase `CAT_WORK_BUF_SIZE` (try 512) |
| Random characters / garbage | Baud rate mismatch | Verify terminal matches USART1 baud rate |
| `AT+HELP` prints nothing | No commands registered | Check `s_cmd_groups` array contains `&cat_builtin_cmd_group` |
| Garbled or truncated output | TX buffer overflow on RS485 | Increase `CAT_TX_BUF_SIZE` or flush earlier |
| No response on RS485 | DE/RE pin not toggling | Verify RS485 direction control in `uart_mgr_send_data()` |

---

## Reference

- **cAT Library**: [https://github.com/marcinbor85/cAT](https://github.com/marcinbor85/cAT)
- **AT32F422 Reference Manual**: Artery AT32F422/426 series
- **Project Reference Files**:
  - `project/usersrc/user_cat_portable.c` — USART1 + libUartMgr portable layer (I/O callbacks + __weak overrides)
  - `project/userinc/user_cat_portable.h` — portable layer API
  - `cAT/src/cat_cmds.c` — built-in AT command implementations (INFO, UPTIME, VER, HELP, RESET, RESTORE, UARTCFG)
  - `cAT/src/cat_cmds.h` — built-in command declarations + 7 __weak callback declarations
  - `project/usersrc/user_fwVer.c` — firmware version + build time implementation
  - `project/usersrc/user_crm.c` — `getSysTick()` implementation
