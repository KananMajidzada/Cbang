#ifndef CBANG_TOKEN_H
#define CBANG_TOKEN_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TOKEN_EOF = 0,
    TOKEN_ERROR,

    /* Literals */
    TOKEN_INT_LIT,
    TOKEN_FLOAT_LIT,
    TOKEN_STRING_LIT,
    TOKEN_CHAR_LIT,
    TOKEN_IDENT,

    /* Keywords - Types */
    KW_I8, KW_I16, KW_I32, KW_I64,
    KW_U8, KW_U16, KW_U32, KW_U64,
    KW_F32, KW_F64,
    KW_BOOL,
    KW_STR,
    KW_VOID,
    KW_UINTPTR,
    KW_VAR,
    KW_CONST,
    KW_VOLATILE,

    /* Keywords - Type Definitions */
    KW_STRUCT,
    KW_UNION,
    KW_ENUM,
    KW_TYPE,

    /* Keywords - Control Flow */
    KW_IF, KW_ELSE,
    KW_WHILE, KW_FOR,
    KW_SWITCH, KW_CASE,
    KW_RETURN, KW_BREAK, KW_CONTINUE,
    KW_TRUE, KW_FALSE,

    /* Keywords - Functions & Modules */
    KW_FN,
    KW_PUB,
    KW_USE,
    KW_EXTERN,
    KW_DEFER,
    KW_AS,

    /* Keywords - Memory */
    KW_SIZEOF, KW_ALIGNOF,

    /* Operators - Arithmetic */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_ADD_ASSIGN, OP_SUB_ASSIGN, OP_MUL_ASSIGN, OP_DIV_ASSIGN, OP_MOD_ASSIGN,

    /* Operators - Comparison */
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,

    /* Operators - Logical */
    OP_AND, OP_OR, OP_NOT,

    /* Operators - Bitwise */
    OP_BIT_AND, OP_BIT_OR, OP_BIT_XOR, OP_BIT_NOT,
    OP_SHL, OP_SHR,
    OP_BIT_AND_ASSIGN, OP_BIT_OR_ASSIGN, OP_BIT_XOR_ASSIGN,
    OP_SHL_ASSIGN, OP_SHR_ASSIGN,

    /* Operators - Pointer/Access */
    OP_DEREF,
    OP_ADDR_OF,
    OP_DOT,
    OP_ARROW,

    /* Assignment */
    OP_ASSIGN,

    /* Delimiters */
    LPAREN, RPAREN,
    LBRACE, RBRACE,
    LBRACKET, RBRACKET,
    COMMA,
    SEMICOLON,
    COLON,

    TOKEN_COUNT
} TokenType;

typedef struct {
    TokenType type;
    const char *start;
    size_t length;
    size_t line;
    size_t column;
    union {
        long long int_val;
        unsigned long long uint_val;   /* integer literals: full u64 range */
        double float_val;
    } value;
} Token;

typedef struct {
    const char *source;
    const char *current;
    size_t line;
    size_t column;
    size_t source_len;
} Lexer;

void lexer_init(Lexer *lexer, const char *source, size_t length);
Token lexer_next_token(Lexer *lexer);
Token lexer_peek_token(Lexer *lexer);
const char *token_type_name(TokenType type);
void token_print(const Token *token);
bool token_is_keyword(TokenType type);
bool token_is_type_keyword(TokenType type);
bool token_is_operator(TokenType type);

#endif