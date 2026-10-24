# C! (C-Bang)

A small systems programming language that compiles to C.

No preprocessor. No macros. No headers. Fixed-width types, `use` for modules,
and a `defer` that actually works. You write `.cb`, you get a binary.

```c
use std.io;

pub fn main() i32 {
    io.print("Hello, World!\n");
    return 0;
}
```

## Build

Needs a C compiler (`cc`) and `make`.

```sh
make
./build/cbang build hello.cb
./hello
```

## Tutorial

### Variables

Every type is fixed-width. You always know how big things are.

```c
var age: u8 = 25;
var price: f64 = 9.99;
var on: bool = true;
var name: str = "Kanan";
```

`const` for things that don't change.

```c
const MAX: u32 = 100;
```

### Functions

```c
fn add(a: i32, b: i32) i32 {
    return a + b;
}

pub fn main() i32 {
    var sum: i32 = add(2, 3);
    return 0;
}
```

`pub` means other files can call it. Without `pub`, it stays private to the file.

### Control flow

```c
if (age >= 18) {
    io.print("adult\n");
} else {
    io.print("kid\n");
}

var i: i32 = 0;
while (i < 10) {
    i = i + 1;
}

for (var j: i32 = 0; j < 10; j = j + 1) {
    io.print("tick\n");
}

switch (day) {
    case 1: { io.print("monday\n"); }
    case 2: { io.print("tuesday\n"); }
}
```

### Structs and pointers

```c
struct Point {
    i32 x;
    i32 y;
}

pub fn main() i32 {
    var p: Point;
    p.x = 10;

    var q: *Point = &p;     // q points at p
    q.x = 20;               // pointer auto-derefs, same as q->x in C

    return p.x;             // 20
}
```

Pointers are explicit like C. But `.` works through them, so you don't
drown in arrows.

### defer

Run cleanup when a scope exits — even on early `return`. Last one registered,
first one to run (LIFO).

```c
fn process() i32 {
    defer { io.print("done\n"); }
    defer { io.print("closing file\n"); }

    io.print("working\n");
    return 0;
}
// prints: working, closing file, done
```

### Casting

Explicit with `as`. No surprise conversions.

```c
var x: i32 = 42;
var big: i64 = x as i64;
var addr: uintptr = &x as uintptr;
```

### Modules

```c
use std.io;    // io.print("text\n")
use std.os;    // os.exit(1)
```

## What's in the box

| Module   | Functions      |
|----------|----------------|
| `std.io` | `io.print(str)` |
| `std.os` | `os.exit(code)` |

Small on purpose. More coming.

## How it works

`cbang` reads your `.cb` file, checks it, and writes a `.c` file. Then it
calls `cc` to build the binary. You can read the generated C — it's clean.

```
hello.cb  ->  lexer  ->  parser  ->  type checker  ->  hello.c  ->  cc  ->  hello
```

## Status

Working but young. Type checker, structs, pointers, defer, switch — all
functional and tested. Standard library is minimal. Not yet battle-tested
on anything big.

## License

MIT.

Written by Kanan Majidzada.
