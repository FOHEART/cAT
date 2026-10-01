[![Build Status](https://travis-ci.org/marcinbor85/cat.svg?branch=master)](https://travis-ci.org/marcinbor85/cat)
# libcat (cAT)
Plain C library for parsing AT commands for use in host devices.

## Features
* blazing fast, non-blocking, robust implementation
* 100% static implementation (without any dynamic memory allocation)
* very small footprint (both RAM and ROM)
* support for READ, WRITE, TEST and RUN type commands
* commands shortcuts (auto select best command candidate)
* single request - multiple responses
* unsolicited read/test command support
* hold state for delayed responses for time-consuming tasks
* high-level memory variables mapping arguments parsing
* variables accessors (read and write, read only, write only)
* automatic arguments types validating
* automatic format test responses for commands with variables
* CRLF and LF compatible
* case-insensitive
* dedicated for embedded systems
* object-oriented architecture
* separated interface for low-level layer
* fully asynchronous input/output operations
* multiplatform and portable
* asynchronous api with event callbacks
* print registered commands list feature
* optional reusable built-in command group (INFO, VER, HELP, RESET, RESTORE, UARTCFG, MODE)
* platform abstraction for the built-in commands through weak callbacks
* portable: only the I/O layer is platform-specific
* only two source files in the core
* wide unit tests

## Documentation

| Document | Scope |
|----------|-------|
| `README.md` (this file) | Platform-independent library documentation: features, build, API usage, built-in commands, AT protocol |
| [`doc/AT32_PortGuide.md`](doc/AT32_PortGuide.md) | Reference port for **AT32** (AT32F422 + USART1 + libUartMgr, RS485 half-duplex) — build integration, portable layer, checklists, troubleshooting |

> Additional platform ports (e.g. STM32) should be added as new files under `doc/`, keeping this
> README free of MCU-specific details.

## Build

Build and install:

```sh
cmake .
make
make test
sudo make install
```

## Example basic demo posibilities

```console
AT+PRINT=?                                              # TEST command
+PRINT=<X:UINT8[RW]>,<Y:UINT8[RW]>,<MESSAGE:STRING[RW]> # Automatic response
Printing something special at (X,Y).                    # Automatic response
OK                                                      # Automatic acknowledge

AT+PRINT?                                               # READ command
+PRINT=0,0,""                                           # Automatic response
OK                                                      # Automatic acknowledge

AT+PRINT=xyz,-2                                         # WRITE command
ERROR                                                   # Automatic acknowledge

AT+PRINT=1,2,"test"                                     # WRITE command
OK                                                      # Automatic acknowledge

AT+PRINT                                                # RUN command
some printing at (1,2) with text "test"                 # Manual response
OK                                                      # Automatic acknowledge
```

## Example unsolicited demo posibilities

```console
AT+START=?                                              # TEST command
+START=<MODE:UINT32[WO]>                                # Automatic response
Start scanning after write (0 - wifi, 1 - bluetooth).   # Automatic response
OK                                                      # Automatic acknowledge

AT+START=0                                              # WRITE command
+SCAN=-10,"wifi1"                                       # Unsolicited read response
+SCAN=-50,"wifi2"                                       # Unsolicited read response
+SCAN=-20,"wifi3"                                       # Unsolicited read response
OK                                                      # Unsolicited acknowledge

AT+START=1                                              # WRITE command
+SCAN=-20,"bluetooth1"                                  # Unsolicited read response
OK                                                      # Unsolicited acknowledge

AT+SCAN=?                                               # TEST command
+SCAN=<RSSI:INT32[RO]>,<SSID:STRING[RO]>                # Automatic response
Scan result record.                                     # Automatic response
OK                                                      # Automatic acknowledge
```

## Usage

Define High-Level variables:

```c

static uint8_t x;
static uint8_t y;
static char msg[32];

static struct cat_variable go_vars[] = {
        {
                .type = CAT_VAR_UINT_DEC, /* unsigned int variable */
                .data = &x,
                .data_size = sizeof(x),
                .write = x_write,
                .name = "X",
                .access = CAT_VAR_ACCESS_READ_WRITE,
        },
        {
                .type = CAT_VAR_UINT_DEC, /* unsigned int variable */
                .data = &y,
                .data_size = sizeof(y),
                .write = y_write,
                .access = CAT_VAR_ACCESS_READ_WRITE,
        },
        {
                .type = CAT_VAR_BUF_STRING, /* string variable */
                .data = msg,
                .data_size = sizeof(msg),
                .write = msg_write,
                .access = CAT_VAR_ACCESS_READ_WRITE,
        }
};
```

Define AT commands descriptor:

```c
static struct cat_command cmds[] = {
        {
                .name = "TEST",
                .read = test_read, /* read handler for ATTEST? command */
                .write = test_write, /* write handler for ATTEST={val} command */
                .run = test_run /* run handler for ATTEST command */
        },
        {
                .name = "+NUM",
                .write = num_write, /* write handler for AT+NUM={val} command */
                .read = num_read /* read handler for AT+NUM? command */
        },
        {
                .name = "+GO",
                .write = go_write, /* write handler for AT+GO={x},{y},{msg} command */
                .var = go_vars, /* attach variables to command */
                .var_num = sizeof(go_vars) / sizeof(go_vars[0]),
                .need_all_vars = true
        },
        {
                .name = "RESTART",
                .run = restart_run /* run handler for ATRESTART command */
        }
};
```

Define AT command parser descriptor:

```c

static char working_buf[128]; /* working buffer, must be declared manually */

static struct cat_command_group cmd_group = {
        .cmd = cmds,
        .cmd_num = sizeof(cmds) / sizeof(cmds[0]),
};

static struct cat_command_group *cmd_desc[] = {
        &cmd_group
};

static struct cat_descriptor desc = {
        .cmd_group = cmd_desc,
        .cmd_group_num = sizeof(cmd_desc) / sizeof(cmd_desc[0]),

        .buf = working_buf,
        .buf_size = sizeof(working_buf),
};
```

Define IO low-level layer interface:

```c
static int write_char(char ch)
{
        putc(ch, stdout);
        return 1;
}

static int read_char(char *ch)
{
        *ch = getch();
        return 1;
}

static struct cat_io_interface iface = {
        .read = read_char,
        .write = write_char
};
```

Initialize AT command parser and run:

```c
struct cat_object at; /* at command parser object */

cat_init(&at, &desc, &iface, NULL); /* initialize at command parser object */

while (1) {
        cat_service(&at) /* periodically call at command parser service */

        ... /* other stuff, running in main loop */
}

```

## Built-in AT Commands

`src/cat_cmds.c` provides a reusable command group (`cat_builtin_cmd_group`) with a standard set
of commands. All of them are **hardware-independent** — platform behaviour is supplied through
weak callbacks (see next section), which lets you register the whole group with a single pointer.

| Handler | AT Syntax | Purpose |
|---------|-----------|---------|
| `cmd_info_run` | `AT+INFO` | Print SYSCLK and uptime |
| `cmd_ver_run` | `AT+VER` | Print firmware version + build time |
| `cmd_help_run` | `AT+HELP` | List all registered commands |
| `cmd_reset_run` | `AT+RESET` | Reset the MCU |
| `cmd_restore_run` | `AT+RESTORE` | Restore factory defaults |
| `cmd_uartcfg_read` / `cmd_uartcfg_write` | `AT+UARTCFG?` / `AT+UARTCFG=<baud>` | Query / set the UART baudrate |
| `cmd_mode_read` / `cmd_mode_write` | `AT+MODE?` / `AT+MODE=<mode>` | Query / set the operating mode (`cat_mode_t`) |

Register the built-in group alongside your own command groups:

```c
extern struct cat_command_group cat_builtin_cmd_group; /* src/cat_cmds.c */
extern struct cat_command_group my_custom_cmd_group;

static struct cat_command_group *cmd_desc[] = {
        &cat_builtin_cmd_group,
        &my_custom_cmd_group,
};
```

## Platform Callback Contract

`cat_cmds.h` declares the callbacks used by the built-in commands. Default stubs are defined with
`__attribute__((weak))` in `cat_cmds.c`, so a port only has to override the ones it needs.
`cat_write_char()` is **not** weak — the portable layer must always provide it.

| Callback | Default | Used by | Port responsibility |
|----------|---------|---------|---------------------|
| `cat_get_sys_tick()` | `0` | `AT+INFO` (uptime) | Return milliseconds since boot |
| `cat_get_sys_clk()` | `48000000` | `AT+INFO` (SCLK) | Return the actual SYSCLK in Hz |
| `cat_get_fw_version()` | `"unknown"` | `AT+VER` | Return the firmware version string |
| `cat_get_build_time()` | `"unknown"` | `AT+VER` | Return the build timestamp string |
| `cat_system_reset()` | spin forever | `AT+RESET` | Call the platform reset (`NVIC_SystemReset()`, ...) |
| `cat_system_restore()` | no-op | `AT+RESTORE` | Erase config / restore factory defaults |
| `cat_get_baudrate()` | `0` | `AT+UARTCFG?` | Return the current UART baudrate |
| `cat_set_baudrate(baud)` | no-op | `AT+UARTCFG=<baud>` | Reconfigure the UART |
| `cat_get_mode()` | `CAT_MODE_MEASUREMENT` | `AT+MODE?` | Return the current `cat_mode_t` |
| `cat_set_mode(mode)` | no-op | `AT+MODE=<mode>` | Switch mode; `CAT_MODE_REQUEST_MEASUREMENT` triggers a one-shot measurement + upload |
| `cat_write_char(ch)` | *(required, not weak)* | all output | Write one byte to the transport |

> **⚠ Keep `__attribute__((weak))` off the declarations.** The attribute must appear **only on the
> definitions** in `cat_cmds.c`, never on the prototypes in `cat_cmds.h`. If it is inherited from
> the header, GCC also marks your strong overrides as weak and the linker may keep the stub
> instead — e.g. `AT+VER` returns `"unknown"` even though your override is linked.

Accessible modes:

```c
typedef enum {
        CAT_MODE_CONFIG = 0,          /* AT commands active */
        CAT_MODE_MEASUREMENT,         /* sensor data streaming */
        CAT_MODE_REQUEST_MEASUREMENT, /* trigger one-shot measurement + upload */
} cat_mode_t;
```

## AT Command Protocol

The parser implements the standard Hayes AT command syntax on top of any byte-oriented transport:

| Command | Example | Response |
|---------|---------|----------|
| `AT+CMD` | `AT+INFO` | Text → `OK` |
| `AT+CMD?` | `AT+UARTCFG?` | `+UARTCFG:<baud>` → `OK` |
| `AT+CMD=<value>` | `AT+UARTCFG=115200` | `OK` |
| `AT+CMD=?` | `AT+LED=?` | `+LED: (0-2),(0-3)` → `OK` |
| `AT+HELP` | `AT+HELP` | Command list → `OK` |

**Newline handling**: commands are terminated with `\r\n` (CR+LF). The parser treats `\r` as the
command terminator and ignores `\n`.

## Porting to a New Platform

Only the low-level I/O layer is platform-specific. A port provides:

1. A **read-one-char** callback returning `0` when no data is available (non-blocking).
2. A **write-one-char** callback (the required `cat_write_char()` symbol).
3. A **working buffer** for the parser plus a `cat_descriptor` / `cat_object` instance.
4. A periodic call to `cat_service()` — or a wrapper such as `user_cat_portable_service()`.
5. Implementations of the [platform callbacks](#platform-callback-contract) the built-in
   commands rely on.

See [`doc/AT32_PortGuide.md`](doc/AT32_PortGuide.md) for a complete, working reference port
(AT32F422 + USART1 + DMA/RS485) that can be used as a template for other MCUs.
