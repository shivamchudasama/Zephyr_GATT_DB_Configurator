# GATT Configurator for Zephyr RTOS

A browser-based visual tool for defining Bluetooth Low Energy GATT databases and generating ready-to-compile [Zephyr RTOS](https://zephyrproject.org/) source code. Ships with the `GATT_GenericCallbacks` C library that the generated code targets.

---

## Features

- Visual three-panel UI: profile/GATT tree sidebar, editor canvas, live code preview
- Custom 128-bit UUID scheme with domain · service · characteristic encoding
- Two callback modes: **generic** (shared `GATT_GenericCallbacks` library) or **per-characteristic** (individual stubs)
- XML import/export for version-controlling GATT definitions
- One-click ZIP download of all generated files (`BaseUUIDs.h`, `*.h`, `*.c`, `project.xml`)
- No build step, no external dependencies — open `index.html` directly in any modern browser

---

## Getting Started

### 1. Open the Tool

Open `index.html` in Chrome, Firefox, or Edge. No server required.

### 2. Configure the Profile

In the **Profile** sidebar card:

| Field | Purpose |
|-------|---------|
| **Domains** | Named hex byte values (`MY_DOMAIN = 0x01`). Each service is assigned a domain. |
| **Base UUID** | The 96-bit suffix shared by all UUIDs in the project (parts 2–5 of the 128-bit UUID). |
| **Project / Author / Organization** | Used in `@file`, `@author`, and `@copyright` tags of generated file headers. |
| **Use Generic GATT Callbacks** | Switches the callback mode for all generated `.c` files (see [Callback Modes](#callback-modes)). |

### 3. Add Services and Characteristics

1. Click **+ Add Service** at the bottom of the GATT tree sidebar.
2. Select the new service in the tree to open it in the editor.
3. Fill in the service name, variable name, domain, and optional brief description.
4. Click **+ Add Characteristic** in the editor canvas.
5. For each characteristic configure:
   - **Name** — used to derive macro names, variable names, and callback names.
   - **ID (hex)** — 16-bit characteristic ID, unique within the service.
   - **CUD** — Characteristic User Description string (shown by GATT browsers).
   - **Properties** — Read, Write, Write Without Response, Notify, Indicate, etc.
   - **Permissions** — BT_GATT_PERM_READ, BT_GATT_PERM_WRITE, encrypted variants, etc.
   - **Length / Variable Length** — fixed byte count or variable-length buffer.
   - **Custom Read / Write Hook** *(Generic CB mode only)* — optional post-read/post-write function name.

### 4. Preview and Export Code

The right panel shows a live preview in four tabs:

| Tab | Content |
|-----|---------|
| `.h` | Header: UUID `#define` macros for the service and all its characteristics. |
| `.c` | Source: `BT_GATT_SERVICE_DEFINE`, local data variables, descriptors, and callback stubs. |
| `BaseUUIDs.h` | Shared header with base UUID parts and domain macros. |
| `.xml (project)` | Consolidated project XML that round-trips back through **⬆ Load XML**. |

Click **⚡ Generate Code** (top-right) to download a `.zip` containing all files.

---

## UUID Scheme

Every UUID is a 128-bit value structured as:

```
[ 32-bit first part ]–[ 16-bit p2 ]–[ 16-bit p3 ]–[ 16-bit p4 ]–[ 48-bit p5 ]
        ↑                         ↑──────── base UUID suffix (set in Profile) ─────────↑
        |  8-bit domain | 8-bit service ID | 16-bit characteristic ID  |
```

The first 32 bits are computed by the `UUID_FIRST_PART_32BIT(domain, svc, char)` macro defined in `BaseUUIDs.h`. The 96-bit suffix (parts 2–5) is project-wide and defined once.

**Example** — domain `0x01`, service ID `0x02`, characteristic ID `0x0003`:

```
First 32 bits  =  0x01020003
Full UUID      =  01020003-XXXX-XXXX-XXXX-XXXXXXXXXXXX
```

This scheme keeps all UUIDs RFC 4122 (UUID v4) layout-compatible while encoding device-specific structure in the first word.

---

## Callback Modes

### Per-Characteristic (default)

Each characteristic gets its own `static ssize_t` read/write callback stub generated in the `.c` file. Implement the body yourself.

### Generic (recommended)

All characteristics share two callbacks — `gt_GATT_GenericRead` and `gt_GATT_GenericWrite` — from the `GATT_GenericCallbacks` library. Each characteristic gets a `GATTCharDescriptor_T` descriptor that links it to:

- A local data variable (the authoritative value mirror)
- Length constraints (fixed or variable)
- An optional `struct k_mutex *` for thread-safe access from application threads
- Optional post-read / post-write hook function pointers for side-effect logic

Enable this mode via the **Use Generic GATT Callbacks** checkbox in the Profile card.

---

## GATT_CB Library

Located in [`lib/GATT_CB/`](lib/GATT_CB/). A lightweight Zephyr C library that implements the generic callback pattern.

| File | Purpose |
|------|---------|
| `GATT_CB_Types.h` | `GATTCharDescriptor_T` struct and hook function pointer typedefs |
| `GATT_GenericCallbacks.h` | Public API declarations |
| `GATT_GenericCallbacks.c` | Full implementation |
| `CMakeLists.txt` | Zephyr CMake integration |

### CMake Integration

Add the following to your application's top-level `CMakeLists.txt`:

```cmake
add_subdirectory(lib/GATT_CB)
add_subdirectory(lib/AppLog)   # if using the provided AppLog wrapper
```

### Log Module Registration

`GATT_GenericCallbacks.c` uses `APP_LOG_ERR()` from [`lib/AppLog/AppLog.h`](lib/AppLog/AppLog.h). The module must be registered **once** before any logging call — typically in `main.c`:

```c
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(APP_LOG, LOG_LEVEL_INF);
```

All other source files that include `AppLog.h` will use `LOG_MODULE_DECLARE()` automatically.

---

## API Reference

### BLE Stack Callbacks

Pass these to `BT_GATT_CHARACTERISTIC` as the read/write callback arguments:

```c
ssize_t gt_GATT_GenericRead(struct bt_conn *stpt_connHandle,
                             const struct bt_gatt_attr *stpt_attr,
                             void *vpt_buf,
                             uint16_t u16_length,
                             uint16_t u16_offset);

ssize_t gt_GATT_GenericWrite(struct bt_conn *stpt_connHandle,
                              const struct bt_gatt_attr *stpt_attr,
                              const void *vpt_buf,
                              uint16_t u16_length,
                              uint16_t u16_offset,
                              uint8_t u8_flags);
```

### Application-Side Local DB Access

Use these from application threads to read or update characteristic values locally:

```c
/* Read current value into caller buffer. *bytesRead = bytes actually copied. */
void gt_GATT_LocalRead(const GATTCharDescriptor_T *stpt_desc,
                       void *vpt_buf,
                       uint16_t u16_bufLen,
                       uint16_t *u16pt_bytesRead);

/* Write new value into local DB. Does NOT invoke the custom write hook. */
void gv_GATT_LocalWrite(GATTCharDescriptor_T *stpt_desc,
                        const void *vpt_buf,
                        uint16_t u16_length);

/* Query current valid data length (useful for variable-length characteristics). */
void gt_GATT_GetActualLen(const GATTCharDescriptor_T *stpt_desc,
                          uint16_t *u16pt_actualLen);
```

### `GATTCharDescriptor_T` Fields

| Field | Type | Description |
|-------|------|-------------|
| `vpt_data` | `void *` | Pointer to the local value variable. Must not be `NULL`. |
| `u16_dataLen` | `uint16_t` | Maximum byte length (buffer size). Never changes after init. |
| `u16_actualLen` | `uint16_t` | Current valid byte count. Updated by `gt_GATT_GenericWrite` for variable-length chars. |
| `b_variableLength` | `bool` | `false` → exact-length writes only. `true` → partial writes accepted. |
| `stpt_mutex` | `struct k_mutex *` | Optional Zephyr mutex for thread-safe access. `NULL` = no locking. |
| `fpt_customReadCb` | `GATTCustomReadCb_F` | Post-read hook called after `bt_gatt_attr_read()`. `NULL` = plain DB read. |
| `fpt_customWriteCb` | `GATTCustomWriteCb_F` | Post-write hook called after `memcpy` into local var. `NULL` = plain DB write. |

### Custom Hook Signatures

```c
/* Post-read hook: return 0 to keep the generic return value, non-zero to override. */
typedef ssize_t (*GATTCustomReadCb_F)(struct bt_conn *stpt_connHandle,
                                      const struct bt_gatt_attr *stpt_attr,
                                      void *vpt_buf,
                                      uint16_t u16_length,
                                      uint16_t u16_offset);

/* Post-write hook: return 0 to keep the generic return value, non-zero to override.
   The local variable is already updated when this hook fires. To reject a write,
   return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED) and roll back vpt_data yourself. */
typedef ssize_t (*GATTCustomWriteCb_F)(struct bt_conn *stpt_connHandle,
                                       const struct bt_gatt_attr *stpt_attr,
                                       const void *vpt_buf,
                                       uint16_t u16_length,
                                       uint16_t u16_offset,
                                       uint8_t u8_flags);
```

---

## Usage Examples

### Fixed-length characteristic, no hooks, no mutex

```c
static uint8_t gu8_status = 0U;

static GATTCharDescriptor_T gst_statusDesc = {
    .vpt_data          = &gu8_status,
    .u16_dataLen       = sizeof(gu8_status),
    .u16_actualLen     = sizeof(gu8_status),
    .b_variableLength  = false,
    .stpt_mutex        = NULL,
    .fpt_customReadCb  = NULL,
    .fpt_customWriteCb = NULL,
};

BT_GATT_SERVICE_DEFINE(my_service,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_MY_SERVICE),
    BT_GATT_CHARACTERISTIC(
        BT_UUID_MY_STATUS_CHAR,
        BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ,
        gt_GATT_GenericRead,   /* read callback  */
        NULL,                  /* write callback */
        &gst_statusDesc        /* user_data      */
    ),
    BT_GATT_CUD("Status", BT_GATT_PERM_READ)
);
```

### Variable-length characteristic with mutex and write hook

```c
static uint8_t  gu8ar_payload[244U] = { 0U };
static K_MUTEX_DEFINE(sst_payloadMutex);

/* Forward-declare the hook (implement below BT_GATT_SERVICE_DEFINE). */
static ssize_t st_OnPayloadWrite(struct bt_conn *stpt_connHandle,
    const struct bt_gatt_attr *stpt_attr, const void *vpt_buf,
    uint16_t u16_length, uint16_t u16_offset, uint8_t u8_flags);

static GATTCharDescriptor_T gst_payloadDesc = {
    .vpt_data          = gu8ar_payload,
    .u16_dataLen       = sizeof(gu8ar_payload),
    .u16_actualLen     = 0U,                    /* no data yet */
    .b_variableLength  = true,
    .stpt_mutex        = &sst_payloadMutex,
    .fpt_customReadCb  = NULL,
    .fpt_customWriteCb = st_OnPayloadWrite,
};
```

### Reading a characteristic from an application thread

```c
uint8_t  u8ar_buf[244U];
uint16_t u16_bytesRead = 0U;

gt_GATT_LocalRead(&gst_payloadDesc, u8ar_buf, sizeof(u8ar_buf), &u16_bytesRead);
/* u16_bytesRead now contains the number of valid bytes copied. */
```

### Writing a characteristic from an application thread

```c
uint8_t u8_newStatus = 0x01U;

/* gv_GATT_LocalWrite does NOT invoke the custom write hook. */
gv_GATT_LocalWrite(&gst_statusDesc, &u8_newStatus, sizeof(u8_newStatus));
```

---

## Generated File Structure

For each service the configurator produces three files:

```
<ServiceName>.h       — UUID macros (BT_UUID_128_ENCODE wrappers)
<ServiceName>.c       — BT_GATT_SERVICE_DEFINE + descriptors / stubs
BaseUUIDs.h           — Shared base UUID parts and domain macros
project.xml           — Round-trip project XML
```

The `.h` file must be included by any translation unit that needs to reference the service's UUIDs. The `.c` file is self-contained; it includes its own `.h`.

---

## Dependencies

| Dependency | Notes |
|------------|-------|
| Zephyr RTOS | `CONFIG_BT=y`, `CONFIG_BT_PERIPHERAL=y` (or `CENTRAL`) |
| `CONFIG_LOG=y` | Required by `AppLog.h` |
| `AppLog.h` | Provided in [`lib/AppLog/`](lib/AppLog/). Call `LOG_MODULE_REGISTER(APP_LOG, LOG_LEVEL_INF)` once in your application. |

---

## Directory Layout

```
gatt-configurator/
├── index.html                  Open this in a browser — no server needed
├── css/
│   └── styles.css              Light/dark theme, all UI components
├── js/
│   ├── state.js                Central application state
│   ├── codegen.js              .h / .c / BaseUUIDs.h / .xml generators
│   ├── xmlio.js                XML import/export (round-trip)
│   ├── editor.js               Service and characteristic form editor
│   ├── tree.js                 GATT tree sidebar
│   ├── modals.js               Add-service / add-characteristic dialogs
│   ├── domains.js              Domain management
│   ├── profile.js              Profile metadata (project, author, org)
│   ├── download.js             ZIP export and individual file download
│   ├── zip.js                  Inline ZIP builder (no external dependencies)
│   ├── resize.js               Resizable sidebar and right panel
│   ├── theme.js                Light/dark theme toggle (persisted to localStorage)
│   ├── tooltip.js              Tooltip engine for property/permission labels
│   └── app.js                  Entry point — initialises all modules
└── lib/
    ├── GATT_CB/                Generic GATT callbacks library (Zephyr C)
    │   ├── GATT_CB_Types.h     GATTCharDescriptor_T + hook typedefs
    │   ├── GATT_GenericCallbacks.h  Public API
    │   ├── GATT_GenericCallbacks.c  Implementation
    │   └── CMakeLists.txt
    └── AppLog/
        └── AppLog.h            Logging wrapper — requires LOG_MODULE_REGISTER
```

---

## License

MIT — see [LICENSE](LICENSE).
