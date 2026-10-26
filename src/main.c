#include "token.h"
#include "ast.h"
#include "codegen.h"
#include "sema.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read entire file into memory */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    *out_len = len;

    return buf;
}

/* math.* lowers to sqrt/pow/floor/... which live in libm. On Linux and most
   Unix linkers libm is a separate library, so without -lm the link fails with
   `undefined reference to `sqrt''. The generator knows what it emitted, so it
   reports it through codegen_used_libm() rather than main guessing. */
/* Modes:
   cbang file.cb          -> parse & print AST
   cbang file.cb --tokens -> lexer only, print tokens
   cbang --test           -> run self tests
*/
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "C! (C-Bang) compiler\n");
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s build <file.cb> [-o out]  Compile to native binary\n", argv[0]);
        fprintf(stderr, "  %s <file.cb>                 Parse and print AST\n", argv[0]);
        fprintf(stderr, "  %s <file.cb> --tokens        Print tokens only\n", argv[0]);
        fprintf(stderr, "  %s --test                    Run self tests\n", argv[0]);
        return 1;
    }

    /* Build mode: cbang build file.cb [-o out] */
    if (strcmp(argv[1], "build") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: %s build <file.cb>\n", argv[0]); return 1; }
        const char *in_path = argv[2];
        const char *out_name = NULL;
        for (int i = 3; i < argc - 1; i++) {
            if (strcmp(argv[i], "-o") == 0) out_name = argv[i + 1];
        }

        size_t len;
        char *source = read_file(in_path, &len);
        if (!source) return 1;

        Parser parser;
        parser_init(&parser, source, len);
        ASTNodeList *ast = parser_parse(&parser);
        if (parser.had_error) {
            fprintf(stderr, "error: parse failed\n");
            ast_list_free(ast);
            free(source);
            return 1;
        }

        /* Semantic analysis: type checking before codegen */
        int type_errors = sema_check(ast);
        if (type_errors > 0) {
            fprintf(stderr, "error: %d type error(s), compilation aborted\n", type_errors);
            ast_list_free(ast);
            free(source);
            return 1;
        }

        /* Generate C: file.cb -> file.c */
        char c_path[512];
        snprintf(c_path, sizeof(c_path), "%s", in_path);
        char *dot = strrchr(c_path, '.');
        if (dot) strcpy(dot, ".c"); else strcat(c_path, ".c");

        if (!codegen_emit_c(ast, c_path)) {
            ast_list_free(ast);
            free(source);
            return 1;
        }
        printf("generated %s\n", c_path);

        /* Compile: cc file.c -o out */
        char cmd[1024];
        const char *lm = codegen_used_libm() ? " -lm" : "";
        if (out_name)
            snprintf(cmd, sizeof(cmd), "cc -std=c11 -Wall -O2 -o %s %s%s",
                     out_name, c_path, lm);
        else {
            char exe[512];
            snprintf(exe, sizeof(exe), "%s", in_path);
            dot = strrchr(exe, '.');
            if (dot) *dot = '\0';
            snprintf(cmd, sizeof(cmd), "cc -std=c11 -Wall -O2 -o %s %s%s",
                     exe, c_path, lm);
        }
        printf("$ %s\n", cmd);
        int rc = system(cmd);

        ast_list_free(ast);
        free(source);
        if (rc != 0) { fprintf(stderr, "error: C compilation failed\n"); return 1; }
        printf("build OK\n");
        return 0;
    }

    bool tokens_only = argc >= 3 && strcmp(argv[2], "--tokens") == 0;
    bool test_mode = strcmp(argv[1], "--test") == 0;

    if (test_mode) {
        printf("=== C! Lexer/Parser Self Test ===\n");
        const char *src =
            "fn add(a: i32, b: i32) i32 { return a + b; }\n"
            "pub const u64 MAX = 0xFF;\n"
            "struct Point { i32 x; i32 y; }\n";
        Parser p;
        parser_init(&p, src, strlen(src));
        ASTNodeList *ast = parser_parse(&p);
        if (p.had_error) {
            printf("RESULT: FAIL (parse errors)\n");
            ast_list_free(ast);
            return 1;
        }
        printf("Parsed %zu top-level declarations.\n", ast->count);
        ast_list_print(ast);
        ast_list_free(ast);
        printf("RESULT: PASS\n");
        return 0;
    }

    size_t len;
    char *source = read_file(argv[1], &len);
    if (!source) return 1;

    if (tokens_only) {
        Lexer lexer;
        lexer_init(&lexer, source, len);
        Token token;
        do {
            token = lexer_next_token(&lexer);
            printf("%zu:%zu  %-14s", token.line, token.column,
                   token_type_name(token.type));
            if (token.type == TOKEN_IDENT || token.type == TOKEN_INT_LIT ||
                token.type == TOKEN_FLOAT_LIT || token.type == TOKEN_STRING_LIT)
                printf("  '%.*s'", (int)token.length, token.start);
            printf("\n");
        } while (token.type != TOKEN_EOF && token.type != TOKEN_ERROR);
        free(source);
        return 0;
    }

    /* Parse mode */
    Parser parser;
    parser_init(&parser, source, len);
    ASTNodeList *ast = parser_parse(&parser);

    if (parser.had_error) {
        fprintf(stderr, "Compilation failed with parse errors.\n");
        ast_list_free(ast);
        free(source);
        return 1;
    }

    printf("Parsed %zu top-level declarations.\n\n", ast->count);
    ast_list_print(ast);

    ast_list_free(ast);
    free(source);
    return 0;
}