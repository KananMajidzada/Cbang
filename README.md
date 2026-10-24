# C! (CBang)

CBang is a systems programming language built to be a modern alternative to C. It is in early development and currently compiles to C, with fixed-width types, defer, structs, unions, enums, pointers, and precise compiler diagnostics.

The compiler is currently written in C and targets C as its backend. Its generated output builds warning-free under -Wall -Wextra.

## Table of Contents

- [Getting Started](#getting-started)
- [Compilation Pipeline](#compilation-pipeline)
- [Command Line](#command-line)
- [Language Overview](#language-overview)
- [Diagnostics](#diagnostics)
- [Limitations](#limitations)
- [Project Status](#project-status)
- [License](#license)

## Getting Started

Create `hello.cb`:

```
use std.io;

fn main() i32 {
    io.print("Hello, world!\n");
    return 0;
}
```

Build and run:

```
cbang build hello.cb -o hello
./hello
```

A larger example:

```
use std.io;
use std.os;

struct Point { i32 x; i32 y; }

fn add(a: i32, b: i32) i32 {
    return a + b;
}

pub fn main() i32 {
    var p: Point;
    p.x = 3;
    p.y = add(p.x, 4);

    defer { io.print("done\n"); }

    if (p.y == 7) {
        io.print("ok\n");
    }

    os.exit(0);
    return 0;
}
```

`examples/keywords.cb` exercises every keyword in the language.

## Compilation Pipeline

```
file.cb -> lexer -> parser -> sema -> file.c -> cc -> binary
```

CBang translates source to C and invokes the system C compiler to produce the final binary.

## Command Line

| Command | Description |
|---|---|
| `cbang build file.cb` | Compile to a binary |
| `cbang build file.cb -o name` | Compile and name the output |
| `cbang file.cb` | Parse only and print the AST |
| `cbang file.cb --tokens` | Print the token stream |
| `cbang --test` | Run the built-in self test |

## Language Overview

### Types

| Category | Types |
|---|---|
| Signed integers | `i8` `i16` `i32` `i64` |
| Unsigned integers | `u8` `u16` `u32` `u64` |
| Floating point | `f32` `f64` |
| Other primitives | `bool` `str` `void` `uintptr` |
| Composite | pointers (`*T`), `struct`, `union`, `enum`, `type` aliases |

All sizes are fixed and guaranteed. `str` is a string slice with `.ptr` and `.len`.

### Declarations

```
var x: i32 = 10;
const MAX: u32 = 100;
volatile var reg: u32 = 0;

struct Point { i32 x; i32 y; }
union Data { i32 i; f32 f; }
enum Code { OK = 0, FAIL = 1 }
type usize2 = u64;

fn add(a: i32, b: i32) i32 { return a + b; }
extern fn abs(x: i32) i32;
```

Functions are private by default and emitted as `static` in C. Use `pub` to export them. `main` and `extern` functions are never `static`.

### Control Flow

`if` / `else`, `while`, three-clause `for`, `switch` (C-style fallthrough), `break`, `continue`, `return`.

### Other Features

- **`defer`** runs a block when the scope exits or the function returns, in reverse order of registration.
- **Casts** with `as`: `x as i64`.
- **`sizeof` and `alignof`.**
- **Pointer field access**: `p.x` and `p->x` are both supported.
- **Integer literals** in decimal, hexadecimal (`0xFF`), binary (`0b1010`) and octal (`0o17`).
- **`extern fn`** for calling C functions. Declarations only; no headers are included.

### Standard Library

| Module | Function | Description |
|---|---|---|
| `std.io` | `io.print(str)` | Write a string to stdout (no newline added) |
| `std.os` | `os.exit(code)` | Exit with the given code |

## Diagnostics

The type checker performs 24 checks, and every error includes a line and column. These cover undeclared names, unknown types and modules, argument count and type mismatches, invalid assignments and returns, missing fields, duplicate definitions, and writes to `const` values.

The lexer additionally warns when a string contains `/n`, which is usually a mistyped `\n`.

## Limitations

Not yet supported:

- `default` in `switch`
- Fixed-size arrays
- Generics, closures, and nested functions
- Methods on structs (use free functions)
- `foreach`-style loops
- Multi-file projects
- String building and number printing in `io.print`
- Modules beyond `std.io` and `std.os`

Known issues:

- Small memory leaks at exit (144 to 336 bytes). AddressSanitizer reports no invalid access.
- `break` and `continue` outside a loop are not caught by the type checker and are left to the C compiler.
- Error messages do not yet include a source snippet.

## Project Status

CBang is under active development. The built-in self test (`cbang --test`) passes, and all programs in `examples/` build and run. Last verified 2026-10-24.

## License

Copyright © 2026 Kanan Majidzada. All rights reserved.
