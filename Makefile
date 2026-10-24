CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2 -g -Iinclude
SRC = src/main.c src/lexer.c src/parser.c src/codegen.c src/sema.c
TARGET = build/cbang

all: $(TARGET)

$(TARGET): $(SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -rf build

test: $(TARGET)
	@./build/cbang examples/hello.cb

run: $(TARGET)
	@./build/cbang examples/hello.cb