C! (C-Bang)
===========

C! is a systems programming language. It compiles to C, then
calls your C compiler to build a binary. Files end in .cb.

There is no preprocessor, no macros, no headers.

    use std.io;

    pub fn main() i32 {
        io.print("hello, world\n");
        return 0;
    }


Building
--------

You need a C compiler and make.

    make
    ./build/cbang build hello.cb
    ./hello


Types
-----

Every type is fixed width. You always know how big things are.

    i8  i16  i32  i64
    u8  u16  u32  u64
    f32  f64
    bool  str  void  uintptr

    var age: u8 = 25;
    var price: f64 = 9.99;
    var name: str = "kanan";
    const MAX: u32 = 100;


Functions
---------

    fn add(a: i32, b: i32) i32 {
        return a + b;
    }

    add(2, 3);

pub makes a function visible outside its file. Without pub it
stays local, like static in C.


Control flow
------------

    if (x < 10) {
        ...
    } else {
        ...
    }

    while (x < 10) {
        x = x + 1;
    }

    for (var i: i32 = 0; i < 10; i = i + 1) {
        io.print("tick\n");
    }

    switch (day) {
        case 1: { io.print("monday\n"); }
        case 2: { io.print("tuesday\n"); }
    }

Conditions must be bool or int. No truthiness beyond that.


Structs and pointers
--------------------

    struct Point {
        i32 x;
        i32 y;
    }

    var p: Point;
    p.x = 10;

    var q: *Point = &p;
    q.x = 20;          /* same as q->x in C */

Pointers look like C: *T to declare, & to take an address, *
to dereference. The . operator works through a pointer, so you
rarely need ->.


defer
-----

defer runs a block when its scope is left, early return
included. They run in reverse order, last one registered runs
first.

    fn run() i32 {
        defer { io.print("second\n"); }
        defer { io.print("first\n"); }

        io.print("working\n");
        return 0;
    }

prints: working, first, second.


Casting
-------

Casts are explicit. Nothing converts behind your back.

    var x: i32 = 42;
    var big: i64 = x as i64;
    var addr: uintptr = &x as uintptr;


Modules
-------

    use std.io;     io.print("text\n")
    use std.os;     os.exit(1)

That is all there is for now. More will come.


The compiler
------------

cbang is written in C, about 3500 lines. It reads your .cb
file, type-checks it, writes a .c file, and runs cc.

    .cb -> lexer -> parser -> checker -> .c -> cc -> binary

The generated C is clean under -Wall -Wextra. You can read it,
it is not obfuscated.


Status
------

v0.1. It works, but it is young. The standard library is
small and nothing big has been written in it yet.


License
-------

MIT.

Copyright (c) Kanan Majidzada. All rights reserved.
