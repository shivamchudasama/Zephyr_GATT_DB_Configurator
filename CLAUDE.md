# CLAUDE.md

Guidance for AI agents working in this repository.

## Library documentation rule

Every C library under `lib/<Name>/` must ship an **`API_REFERENCE.md`** next to its sources.

When you **create a new library**, or change the public API of an existing one:

1. Write or update `lib/<Name>/API_REFERENCE.md`, generated from the public headers and
   the `@public` doc comments in the `.c` files. Include:
   - the headers and what each contains, plus dependencies (other libs, Kconfig)
   - a minimal quick-start / integration snippet
   - every public function: signature, behaviour, sync or async, **every return code**
   - every public type, enum (with values), callback signature and its threading context
   - every compile-time configuration macro, with its default and constraints
   - the behavioural rules a caller must follow (lifetimes, concurrency, ISR safety)
2. Add or update the library's row in the **Library API index** below.
3. Keep `README.md` for design rationale and usage. Keep `API_REFERENCE.md` as the exact contract.

Before using a library's API, read its `API_REFERENCE.md` first and treat it as the contract.
If it disagrees with the code, the code wins; fix the reference.

## Library API index

| Library | Purpose | API reference |
|---|---|---|
| `lib/BulkXfer` | Reliable bulk transfer over BLE GATT: Client role writes DATA (Write Without Response), Server role hosts the service and ACKs via CTRL notify (windowed ACK, Go-Back-N, CRC-32) | [lib/BulkXfer/API_REFERENCE.md](lib/BulkXfer/API_REFERENCE.md) |
| `lib/GATT_CB` | Generic GATT read/write callbacks used by generated services | *Not written yet*: see `GATT_GenericCallbacks.h`, `GATT_CB_Types.h` |
| `lib/AppLog` | Logging macros (`APP_LOG_*`) | *Not written yet*: see `AppLog.h` |

## Code conventions (C libraries)

- Hungarian-style prefixes: scope `g` (global) / `s` (static), then the type (`i`, `v`, `b`, `u8`, `u16`, `u32`, `t`, `pt`, `ar`, `st`, `fpt`, `e`), then `_<MODULE>_`, then the name. Variable names are lowerCamelCase and function names are PascalCase, so `sst_BLK_cfg` is a variable and `gi_BLK_Init` is a function. The module field is required for global, static and static-local variables and for global functions.
- Scope `sl` for a `static` variable inside a function, e.g. `slst_mtuParams`. Objects created by Zephyr macros with external linkage (`K_THREAD_DEFINE`, `BT_GATT_SERVICE_DEFINE`, `BT_CONN_CB_DEFINE`) take scope `g`.
- Types: `_T` struct, `_U` union, `_E` enum, `_F` function pointer. Union variables and members use `u`, e.g. `u_body`.
- Enum members: `e<acronym of enum type>_<UPPER_NAME>`, e.g. `eBS_OK` in `BlkStatus_E`. This keeps them distinct from `#define`s.
- Source of these rules: `DOC/BATL Coding Guidelines/` (the PDF plus `Sample_Format.c/.h`). This repo keeps its MIT header and has no BATL copyright footer.
- File layout uses the boxed section banners (INCLUDES, DEFINES, ENUMS, …). Doxygen `@public` / `@private` blocks go on every function.
- Header: `SPDX-License-Identifier: MIT`, author Shivam Chudasama.
