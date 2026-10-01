# cAT on AT32 — Porting Guide

> **Target MCU**: AT32F422GBU7-4 (ARM Cortex-M4, 120 MHz, 128 KB Flash, 20 KB RAM)
> **Toolchain**: GCC ARM Embedded (arm-none-eabi) + EIDE (CMake/Ninja backend)
> **Reference project**: MC2010APP
>
> For the hardware-independent library documentation (features, API, built-in commands,
> weak callbacks, AT protocol), see the [README](../README.md).

---

## AT32 Module Structure

| Component | Files |
|-----------|-------|
| cAT core parser | `cAT/src/cat.c`, `cAT/src/cat.h` |
| cAT built-in commands (hardware-independent) | `cAT/src/cat_cmds.c`, `cAT/src/cat_cmds.h` |
| AT32 portable layer (USART1 + libUartMgr) | `project/usersrc/user_cat_portable.c`, `project/userinc/user_cat_portable.h` |

The reference implementation uses **USART1 with libUartMgr** (DMA TX + DMA RX with idle-line
detection, RS485 half-duplex). The portable layer buffers TX bytes and flushes them as a single
DMA transaction on `'\n'` to minimize RS485 direction-toggle overhead.

**Resource Usage** (measured on AT32F422, libUartMgr-based):

| Component | Flash | RAM |
|-----------|-------|-----|
| cAT core + built-in commands (`cat.o` + `cat_cmds.o`) | ~2.5 KB | 0 bytes |
| AT32 portable layer (`user_cat_portable.o`) | ~0.6 KB | ~260 B (TX buffer) |

---

## Porting Steps

### Step 1: Add cAT Source to the Build System

Add the cAT sources and include path to your `CMakeLists.txt`:

```cmake
# cAT — AT command parser
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    cAT/src/cat.c
    # cAT — built-in commands (hardware-independent)
    cAT/src/cat_cmds.c
    # cAT AT32 portable layer
    project/usersrc/user_cat_portable.c
)

# cAT library headers
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    cAT/src
)
```

> **EIDE**: The same sources must be listed in your builder params / EIDE source list.
> **Reference**: see `CMakeLists.txt` lines 56-62 and `build/Debug/builder.params` `sourceList`.

---

### Step 2: Create the Portable Layer Files

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

> There are **no separate `user_cat_cmds.c/h` files** needed — the built-in commands from
> `cAT/src/cat_cmds.c` are used directly. If you need custom commands, implement them alongside
> the built-in group (see Step 5).

---

### Step 3: Implement the I/O Interface

cAT requires two low-level I/O callbacks: **read one char** and **write one char**.

#### 3.1 Line-Buffered TX with libUartMgr (reference — MC2010APP)

Buffering outgoing bytes and flushing them on `'\n'` avoids one DMA + RS485 transaction per
character, which would be extremely inefficient on a half-duplex RS485 bus.

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

    /* Flush on newline or when the buffer is full */
    if (ch == '\n' || s_cat_tx.len >= CAT_TX_BUF_SIZE)
        cat_tx_flush();

    return 1;
}
```

**Read callback** — reads from the libUartMgr RX ring buffer:

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
> - RS485 half-duplex direction control is handled automatically by `uart_mgr_send_data()` — no manual DE/RE pin toggling needed.
> - `uart1_hw` is defined in `uart_mgr_portable.c` — declare `extern uart_hw_TypeDef uart1_hw;`.
> - The TX buffer must be large enough to hold a full AT response line (256 bytes is sufficient).

#### 3.2 Direct DMA Ring Buffers (alternative)

If you are **not** using libUartMgr and want to manage DMA directly, implement a pair of ring
buffers (TX + RX) with DMA kick-off on TX and an idle-line ISR for RX frame detection.

#### 3.3 Polling-Based Approach (simplest, no DMA)

For AT32 targets without DMA, or to minimize resource usage, drive USART directly:

```c
static int cat_write_char(char ch)
{
    while ((USART1->sts & USART_TDBE_FLAG) == 0) { }
    USART1->dt = (uint32_t)ch;
    return 1;
}
```

RX still requires an interrupt-driven ring buffer (see `cAT/example/` for reference).

---

### Step 4: Create the cAT Descriptor & I/O Interface Struct

```c
/* Working buffer for the cAT parser (holds the argument string being parsed) */
static uint8_t s_cat_work_buf[256];

/* cAT parser instance */
static struct cat_object s_cat;

/* cAT I/O interface */
static const struct cat_io_interface s_cat_io = {
    .read  = cat_read_char,     /* from Step 3 */
    .write = cat_write_char,    /* from Step 3 */
};

/* Command groups: built-in commands + AT32 project-specific groups */
static struct cat_command_group *s_cmd_groups[] = {
    &cat_builtin_cmd_group,     /* defined in cAT/src/cat_cmds.c */
};
```

> The working buffer must be large enough to hold the longest AT command argument string.
> 256 bytes is sufficient for typical use cases.

---

### Step 5: Register the Built-in Commands

`cAT/src/cat_cmds.c` provides the reusable `cat_builtin_cmd_group` plus a set of `__weak`
platform callbacks. The full command list and the callback contract are documented in the
[README](../README.md); in the AT32 project they are registered like this:

```c
static struct cat_command_group *s_cmd_groups[] = {
    &cat_builtin_cmd_group,   /* from cat_cmds.c */
};
```

**Adding AT32-specific commands**: create additional command groups in your own source files and
append them to the array:

```c
extern struct cat_command_group my_custom_cmd_group;

static struct cat_command_group *s_cmd_groups[] = {
    &cat_builtin_cmd_group,
    &my_custom_cmd_group,
};
```

---

### Step 6: Override the Platform Callbacks (`__weak`)

Override the weak callbacks listed in the README with AT32 implementations inside
`user_cat_portable.c`:

```c
uint32_t cat_get_sys_tick(void)
{
    return getSysTick();
}

uint32_t cat_get_sys_clk(void)
{
    return 48000000;  /* match actual SYSCLK frequency */
}

const char *cat_get_fw_version(void)
{
    return user_fwVer_GetVersionString();
}

const char *cat_get_build_time(void)
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

cat_mode_t cat_get_mode(void)
{
    /* Return the AT32 application's current operating mode */
    return s_app_mode;
}

void cat_set_mode(cat_mode_t mode)
{
    /* Switch the AT32 application between config/measurement modes,
       and trigger a one-shot measurement + data upload for
       CAT_MODE_REQUEST_MEASUREMENT */
    s_app_mode = mode;
}
```

> **⚠ Critical — `__attribute__((weak))` on declarations**:
> The `cat_cmds.h` header declares these functions. `__attribute__((weak))` MUST be placed
> **only on the definitions** in `cat_cmds.c`, **NOT** on the declarations in `cat_cmds.h`.
> If the attribute appears on the declaration in the header, GCC propagates it to every
> definition that includes that header — including your "strong" overrides — making them weak
> too. The linker may then pick the stub instead of your override, so `AT+VER` returns
> `"unknown"` even though your override is linked.
> **Status in this project**: `cat_cmds.h` declarations are clean; the attribute is only on
> the definitions in `cat_cmds.c`.

---

### Step 7: Initialize cAT in `main()`

After USART + DMA + libUartMgr initialization, call the portable init function:

```c
/* In main(), after uart_mgr_create() and DMA IRQ setup */

/* Initialize the cAT AT command parser on USART1 */
user_cat_portable_init();
```

`user_cat_portable_init()` implementation (see `user_cat_portable.c`):

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

    /* Service the cAT AT command parser (reads from the libUartMgr RX ring) */
    user_cat_portable_service();

    /* ... other tasks ... */
}
```

This calls `cat_service(&s_cat)`, which runs the parser FSM — processing characters from the RX
ring buffer and generating responses through the write callback.

---

### Step 9: No Extra Interrupt Handlers Needed (with libUartMgr)

libUartMgr handles all USART and DMA interrupts internally. You do **not** need custom ISR glue
for cAT when using libUartMgr — the DMA TX complete, DMA RX half/full, and USART idle-line
interrupts are already wired in `uart_mgr_portable.c` and `at32f422_426_int.c`.

If you are **not** using libUartMgr and manage DMA directly, you must wire:

- USART idle-line ISR → push received frame data into the cAT RX ring buffer
- DMA TX complete ISR → advance the TX ring buffer

---

## Complete Integration Checklist

| # | Step | File | Done? |
|---|------|------|-------|
| 1 | Add `cAT/src/cat.c`, `cAT/src/cat_cmds.c` to the build sources | `CMakeLists.txt` / EIDE builder.params | ☐ |
| 2 | Add `cAT/src` to the include paths | `CMakeLists.txt` / EIDE builder.params | ☐ |
| 3 | Create the port header | `project/userinc/user_cat_portable.h` | ☐ |
| 4 | Create the port source with I/O callbacks | `project/usersrc/user_cat_portable.c` | ☐ |
| 5 | Override all `__weak` callbacks (see README) | `user_cat_portable.c` | ☐ |
| 6 | Add `#include "user_cat_portable.h"` | `main.c` | ☐ |
| 7 | Call `user_cat_portable_init()` after USART/DMA init | `main.c` | ☐ |
| 8 | Call `user_cat_portable_service()` in the main loop | `main.c` | ☐ |
| 9 | **(libUartMgr)** Ensure USART1/DMA IRQs are wired | `at32f422_426_int.c` (already done) | ☐ |
| 10 | **(raw DMA)** Wire USART idle-line ISR + DMA TX ISR | `at32fxxx_int.c` | ☐ |

---

## AT32 Troubleshooting

| Symptom | Likely Cause | Solution |
|---------|-------------|----------|
| No response to AT commands | USART not initialized, or wrong baud rate | Check the USART1 config (921600 8N1 default) |
| Characters echoed but no `OK` | RX is not feeding the cAT parser | Verify `user_cat_portable_service()` is called in the main loop |
| `AT+VER` returns `"unknown"` | `__weak` attribute leaked from the header | Ensure `cat_cmds.h` declarations do NOT have `__attribute__((weak))` (see Step 6 ⚠) |
| `AT+INFO` reports `Uptime: 0:00:00` | `cat_get_sys_tick()` returns 0 | Override `cat_get_sys_tick()` to return the real SysTick count |
| `AT+INFO` reports the wrong SCLK | `cat_get_sys_clk()` not overridden | Override `cat_get_sys_clk()` to return the actual SYSCLK |
| `AT+UARTCFG?` returns `0` | `cat_get_baudrate()` not overridden | Override `cat_get_baudrate()` to return the actual baudrate |
| `AT+UARTCFG=<n>` returns `ERROR` | Baudrate value is 0 or invalid | Ensure the argument is a positive integer (e.g. `AT+UARTCFG=115200`) |
| `AT+UARTCFG=<n>` changes nothing | `cat_set_baudrate()` not overridden | Override `cat_set_baudrate()` to reconfigure the USART hardware |
| `AT+RESTORE` does nothing | `cat_system_restore()` not overridden | Override `cat_system_restore()` with factory reset logic |
| `AT+MODE` changes nothing | `cat_set_mode()` not overridden | Override `cat_set_mode()` / `cat_get_mode()` |
| `ERROR` for valid commands | Working buffer too small | Increase `CAT_WORK_BUF_SIZE` (try 512) |
| Random characters / garbage | Baud rate mismatch | Verify the terminal matches the USART1 baud rate |
| `AT+HELP` prints nothing | No commands registered | Check that `s_cmd_groups` contains `&cat_builtin_cmd_group` |
| Garbled or truncated output | TX buffer overflow on RS485 | Increase `CAT_TX_BUF_SIZE` or flush earlier |
| No response on RS485 | DE/RE pin not toggling | Verify RS485 direction control in `uart_mgr_send_data()` |

---

## AT32 Reference Files

- `project/usersrc/user_cat_portable.c` — USART1 + libUartMgr portable layer (I/O callbacks + `__weak` overrides)
- `project/userinc/user_cat_portable.h` — portable layer API
- `project/usersrc/user_fwVer.c` — firmware version + build time implementation
- `project/usersrc/user_crm.c` — `getSysTick()` implementation

## External References

- **cAT library**: <https://github.com/marcinbor85/cAT>
- **AT32F422/426 Reference Manual**: Artery AT32F422/426 series
