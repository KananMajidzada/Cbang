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

CBang translates source to C and invokes the system C compiler to produce the final binary. When `std.math` is used, `-lm` is passed to the linker so the libm functions resolve.

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
| `std.io` | `io.print(value)` | Write a value to stdout (no newline added). Accepts `str`, every integer type, `f32`, `f64`, `bool`, and integer literals. |
| `std.os` | `os.exit(code)` | Exit with the given code |
| `std.math` | `math.abs` `math.min` `math.max` `math.sqrt` `math.pow` `math.floor` `math.ceil` `math.round` | Numeric functions. `abs` keeps the argument's type; `min`/`max` work on signed, unsigned and floating-point values; the rest return `f64`. |

```c
use std.math;

pub fn main() i32 {
    var root: f64 = math.sqrt(144.0);      // 12
    var bigger: i32 = math.max(3, 7);      // 7
    var rounded: f64 = math.floor(2.7);    // 2
    if (root == 12.0 && bigger == 7 && rounded == 2.0) {
        return 0;
    }
    return 1;
}
```

Each argument is evaluated exactly once, so `math.min(bump(), 5)` calls `bump()` one time. Anything that reaches libm is linked with `-lm` automatically.

## Diagnostics

The type checker performs 54 checks, and every error includes a line and column. These cover undeclared names, unknown types and modules, argument count and type mismatches, invalid assignments and returns, missing fields, duplicate definitions, values that cannot be printed, writes to `const` values, and the argument rules of `std.math` — including mixing signed with unsigned in `math.min`.

It also catches the mistakes that would otherwise surface inside the generated C: `break` and `continue` outside a loop, assigning to something that is not a variable, division or remainder by zero, a shift wider than its operand, duplicate parameters and duplicate struct fields, a field or global naming a type or value declared after it, `void` used as storage, and a non-void function whose control can fall off the end without returning.

The lexer additionally warns when a string contains `/n`, which is usually a mistyped `\n`. A private function that nothing calls is reported as `[warning] line:col: private function 'x' is never used`, against your own line rather than the generated file.

## Limitations

Not yet supported:

- `default` in `switch`
- Fixed-size arrays
- Generics, closures, and nested functions
- Methods on structs (use free functions)
- `foreach`-style loops
- Multi-file projects
- String building in `io.print` (it writes one value and adds no newline)
- Modules beyond `std.io`, `std.os` and `std.math`
- `sin`, `cos`, `log` and the other transcendental functions in `std.math`

Known issues:

- Small memory leaks at exit (about 1 KB for a larger program — `Type` objects that live for the whole run). AddressSanitizer reports no invalid access.
- Error messages do not yet include a source snippet.

## Project Status

CBang is under active development. The built-in self test (`cbang --test`) passes, all programs in `examples/` build and run, and the test suite in `tests/` passes 30 of 30:

```sh
sh tests/run.sh
```

Eleven programs must compile and exit 0, nineteen must be rejected by the compiler — each pinned to the exact text of its message with `// expect:`, so a diagnostic cannot drift unnoticed — `defer` is checked against expected output, and every generated C file is additionally compiled with `-Wall -Wextra` — a C warning counts as a failure. The whole suite also passes with the compiler built under AddressSanitizer and UndefinedBehaviorSanitizer. Last verified 2026-10-26.

## License

Copyright © 2026 Kanan Majidzada. All rights reserved.
