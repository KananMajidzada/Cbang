#include "token.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ========================================================================== */
/* Helper Macros & Forward Declarations                                       */
/* ========================================================================== */

#define PEEK() (lexer->current < lexer->source + lexer->source_len ? *lexer->current : '\0')
#define ADVANCE() (lexer->current < lexer->source + lexer->source_len ? *lexer->current++ : '\0')
#define MATCH(expected) (PEEK() == (expected) ? (ADVANCE(), true) : false)

static Token make_token(Lexer *lexer, TokenType type);
static Token error_token(Lexer *lexer, const char *msg);
static void skip_whitespace(Lexer *lexer);
static Token scan_identifier(Lexer *lexer);
static Token scan_number(Lexer *lexer);
static Token scan_string(Lexer *lexer);
static Token scan_char(Lexer *lexer);
static TokenType keyword_type(const char *start, size_t length);

/* ========================================================================== */
/* Lexer Init & Main Loop                                                     */
/* ========================================================================== */

void lexer_init(Lexer *lexer, const char *source, size_t length) {
    lexer->source = source;
    lexer->current = source;
    lexer->line = 1;
    lexer->column = 1;
    lexer->source_len = length;
}

Token lexer_next_token(Lexer *lexer) {
    skip_whitespace(lexer);
    
    if (lexer->current >= lexer->source + lexer->source_len) {
        return make_token(lexer, TOKEN_EOF);
    }
    
    size_t token_start_line = lexer->line;
    size_t token_start_col = lexer->column;
    (void)token_start_line; (void)token_start_col;
    
    char c = ADVANCE();
    
    switch (c) {
        /* Single-character tokens */
        case '(': return make_token(lexer, LPAREN);
        case ')': return make_token(lexer, RPAREN);
        case '{': return make_token(lexer, LBRACE);
        case '}': return make_token(lexer, RBRACE);
        case '[': return make_token(lexer, LBRACKET);
        case ']': return make_token(lexer, RBRACKET);
        case ',': return make_token(lexer, COMMA);
        case ';': return make_token(lexer, SEMICOLON);
        case ':': return make_token(lexer, COLON);
        case '.': return make_token(lexer, OP_DOT);
        
        /* Operators that can be 1 or 2 chars */
        case '+': return MATCH('=') ? make_token(lexer, OP_ADD_ASSIGN) : make_token(lexer, OP_ADD);
        case '-': 
            if (MATCH('>')) return make_token(lexer, OP_ARROW);
            return MATCH('=') ? make_token(lexer, OP_SUB_ASSIGN) : make_token(lexer, OP_SUB);
        case '*': return MATCH('=') ? make_token(lexer, OP_MUL_ASSIGN) : make_token(lexer, OP_MUL);
        case '/': 
            if (MATCH('/')) { skip_whitespace(lexer); return lexer_next_token(lexer); }
            if (MATCH('*')) { /* block comment */ skip_whitespace(lexer); return lexer_next_token(lexer); }
            return MATCH('=') ? make_token(lexer, OP_DIV_ASSIGN) : make_token(lexer, OP_DIV);
        case '%': return MATCH('=') ? make_token(lexer, OP_MOD_ASSIGN) : make_token(lexer, OP_MOD);
        case '=': return MATCH('=') ? make_token(lexer, OP_EQ) : make_token(lexer, OP_ASSIGN);
        case '!': return MATCH('=') ? make_token(lexer, OP_NE) : make_token(lexer, OP_NOT);
        case '<': 
            if (MATCH('=')) return make_token(lexer, OP_LE);
            if (MATCH('<')) return MATCH('=') ? make_token(lexer, OP_SHL_ASSIGN) : make_token(lexer, OP_SHL);
            return make_token(lexer, OP_LT);
        case '>': 
            if (MATCH('=')) return make_token(lexer, OP_GE);
            if (MATCH('>')) return MATCH('=') ? make_token(lexer, OP_SHR_ASSIGN) : make_token(lexer, OP_SHR);
            return make_token(lexer, OP_GT);
        case '&': 
            if (MATCH('&')) return make_token(lexer, OP_AND);
            return MATCH('=') ? make_token(lexer, OP_BIT_AND_ASSIGN) : make_token(lexer, OP_BIT_AND);
        case '|': 
            if (MATCH('|')) return make_token(lexer, OP_OR);
            return MATCH('=') ? make_token(lexer, OP_BIT_OR_ASSIGN) : make_token(lexer, OP_BIT_OR);
        case '^': return MATCH('=') ? make_token(lexer, OP_BIT_XOR_ASSIGN) : make_token(lexer, OP_BIT_XOR);
        case '~': return make_token(lexer, OP_BIT_NOT);
        
        /* Literals & Identifiers */
        case '"': return scan_string(lexer);
        case '\'': return scan_char(lexer);
        default:
            if (isdigit(c)) {
                lexer->current--; lexer->column--;
                return scan_number(lexer);
            }
            if (isalpha(c) || c == '_') {
                lexer->current--; lexer->column--;
                return scan_identifier(lexer);
            }
            return error_token(lexer, "Unexpected character");
    }
}

/* ========================================================================== */
/* Token Creation Helpers                                                     */
/* ========================================================================== */

static Token make_token(Lexer *lexer, TokenType type) {
    Token token;
    token.type = type;
    token.start = lexer->current - 1; // We already advanced
    token.length = 1;
    token.line = lexer->line;
    token.column = lexer->column - 1;
    token.value.int_val = 0;
    token.value.float_val = 0.0;
    return token;
}

static Token make_token_len(TokenType type, const char *start, size_t len, size_t line, size_t col) {
    Token token;
    token.type = type;
    token.start = start;
    token.length = len;
    token.line = line;
    token.column = col;
    token.value.int_val = 0;
    token.value.float_val = 0.0;
    return token;
}

static Token error_token(Lexer *lexer, const char *msg) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = msg;
    token.length = strlen(msg);
    token.line = lexer->line;
    token.column = lexer->column;
    return token;
}

/* ========================================================================== */
/* Whitespace & Comments                                                      */
/* ========================================================================== */

static void skip_whitespace(Lexer *lexer) {
    while (true) {
        char c = PEEK();
        switch (c) {
            case ' ': case '\r': case '\t':
                ADVANCE(); lexer->column++; break;
            case '\n':
                ADVANCE(); lexer->line++; lexer->column = 1; break;
            case '/':
                if (lexer->current + 1 < lexer->source + lexer->source_len) {
                    char next = *(lexer->current + 1);
                    if (next == '/') {
                        while (PEEK() != '\n' && PEEK()) ADVANCE();
                    } else if (next == '*') {
                        ADVANCE(); ADVANCE(); lexer->column += 2;
                        while (true) {
                            if (PEEK() == '*' && *(lexer->current + 1) == '/') {
                                ADVANCE(); ADVANCE(); lexer->column += 2; break;
                            }
                            if (!PEEK()) break;
                            if (PEEK() == '\n') { lexer->line++; lexer->column = 1; }
                            ADVANCE(); lexer->column++;
                        }
                    } else return;
                } else return;
                break;
            default: return;
        }
    }
}

/* ========================================================================== */
/* Scanners                                                                   */
/* ========================================================================== */

static Token scan_identifier(Lexer *lexer) {
    const char *start = lexer->current;
    size_t line = lexer->line;
    size_t col = lexer->column;
    
    while (true) {
        char c = PEEK();
        if (isalnum(c) || c == '_') {
            ADVANCE(); lexer->column++;
        } else break;
    }
    
    size_t len = lexer->current - start;
    TokenType kw = keyword_type(start, len);
    return make_token_len(kw, start, len, line, col);
}

static Token scan_number(Lexer *lexer) {
    const char *start = lexer->current;
    size_t line = lexer->line;
    size_t col = lexer->column;
    bool is_float = false;
    
    /* Handle hex (0x), binary (0b), octal (0o) */
    if (PEEK() == '0') {
        ADVANCE(); lexer->column++;
        char c = PEEK();
        if (c == 'x' || c == 'X') { ADVANCE(); lexer->column++; 
            while (isxdigit(PEEK())) { ADVANCE(); lexer->column++; }
        } else if (c == 'b' || c == 'B') { ADVANCE(); lexer->column++;
            while (PEEK() == '0' || PEEK() == '1') { ADVANCE(); lexer->column++; }
        } else if (c == 'o' || c == 'O') { ADVANCE(); lexer->column++;
            while (PEEK() >= '0' && PEEK() <= '7') { ADVANCE(); lexer->column++; }
        } else {
            while (isdigit(PEEK())) { ADVANCE(); lexer->column++; }
        }
    } else {
        while (isdigit(PEEK())) { ADVANCE(); lexer->column++; }
    }
    
    /* Float suffix */
    if (PEEK() == '.' && isdigit(*(lexer->current + 1))) {
        is_float = true;
        ADVANCE(); lexer->column++; // consume '.'
        while (isdigit(PEEK())) { ADVANCE(); lexer->column++; }
    }
    
    if (PEEK() == 'e' || PEEK() == 'E') {
        is_float = true;
        ADVANCE(); lexer->column++;
        if (PEEK() == '+' || PEEK() == '-') { ADVANCE(); lexer->column++; }
        while (isdigit(PEEK())) { ADVANCE(); lexer->column++; }
    }
    
    size_t len = lexer->current - start;
    
    Token token = make_token_len(is_float ? TOKEN_FLOAT_LIT : TOKEN_INT_LIT, start, len, line, col);
    
    if (is_float) {
        token.value.float_val = strtod(start, NULL);
    } else {
        /* Handle different bases */
        if (len >= 2 && start[0] == '0') {
            char c = start[1];
            if (c == 'x' || c == 'X') token.value.int_val = strtoll(start, NULL, 16);
            else if (c == 'b' || c == 'B') token.value.int_val = strtoll(start + 2, NULL, 2);
            else if (c == 'o' || c == 'O') token.value.int_val = strtoll(start + 2, NULL, 8);
            else token.value.int_val = strtoll(start, NULL, 10);
        } else {
            token.value.int_val = strtoll(start, NULL, 10);
        }
    }
    return token;
}

static Token scan_string(Lexer *lexer) {
    const char *start = lexer->current;
    size_t line = lexer->line;
    size_t col = lexer->column;
    
    while (true) {
        char c = PEEK();
        if (c == '\0' || c == '\n') return error_token(lexer, "Unterminated string");
        if (c == '"') break;
        if (c == '\\') { ADVANCE(); lexer->column++; if (PEEK()) { ADVANCE(); lexer->column++; } }
        else { ADVANCE(); lexer->column++; }
    }
    
    ADVANCE(); lexer->column++; // closing quote
    size_t len = lexer->current - start;

    /* Warn: string ends with /n or /t (common typo for \n or \t) */
    if (len >= 3 && start[len - 3] == '/' &&
        (start[len - 2] == 'n' || start[len - 2] == 't')) {
        fprintf(stderr,
                "[warning] %zu:%zu: string ends with '/%c' - did you mean \"\\%c\"? "
                "(forward slash, but newline/tab needs a backslash '\\')\n",
                line, col, start[len - 2], start[len - 2]);
    }

    return make_token_len(TOKEN_STRING_LIT, start, len, line, col);
}

static Token scan_char(Lexer *lexer) {
    /* The main loop already consumed the opening quote — back up so the
       token text includes it. The parser expects start[0]=quote, start[1]=char. */
    const char *start = lexer->current - 1;
    size_t line = lexer->line;
    size_t col = lexer->column - 1;
    
    if (PEEK() == '\\') { ADVANCE(); lexer->column++; if (PEEK()) { ADVANCE(); lexer->column++; } }
    else if (PEEK()) { ADVANCE(); lexer->column++; }
    
    if (PEEK() != '\'') return error_token(lexer, "Unterminated char literal");
    ADVANCE(); lexer->column++;
    
    size_t len = lexer->current - start;
    return make_token_len(TOKEN_CHAR_LIT, start, len, line, col);
}

/* ========================================================================== */
/* Keyword Lookup                                                             */
/* ========================================================================== */

static TokenType keyword_type(const char *start, size_t len) {
    #define KW(name, type) if (len == sizeof(#name)-1 && memcmp(start, #name, len) == 0) return type
    KW(i8, KW_I8); KW(i16, KW_I16); KW(i32, KW_I32); KW(i64, KW_I64);
    KW(u8, KW_U8); KW(u16, KW_U16); KW(u32, KW_U32); KW(u64, KW_U64);
    KW(f32, KW_F32); KW(f64, KW_F64);
    KW(bool, KW_BOOL); KW(str, KW_STR); KW(void, KW_VOID); KW(uintptr, KW_UINTPTR);
    KW(var, KW_VAR); KW(const, KW_CONST); KW(volatile, KW_VOLATILE);
    KW(struct, KW_STRUCT); KW(union, KW_UNION); KW(enum, KW_ENUM); KW(type, KW_TYPE);
    KW(if, KW_IF); KW(else, KW_ELSE);
    KW(while, KW_WHILE); KW(for, KW_FOR);
    KW(switch, KW_SWITCH); KW(case, KW_CASE);
    KW(return, KW_RETURN); KW(break, KW_BREAK); KW(continue, KW_CONTINUE);
    KW(true, KW_TRUE); KW(false, KW_FALSE);
    KW(fn, KW_FN); KW(pub, KW_PUB); KW(use, KW_USE); KW(extern, KW_EXTERN);
    KW(defer, KW_DEFER); KW(as, KW_AS);
    KW(sizeof, KW_SIZEOF); KW(alignof, KW_ALIGNOF);
    #undef KW
    return TOKEN_IDENT;
}

/* ========================================================================== */
/* Public Utility Functions                                                   */
/* ========================================================================== */

Token lexer_peek_token(Lexer *lexer) {
    const char *saved = lexer->current;
    size_t saved_line = lexer->line;
    size_t saved_col = lexer->column;
    Token t = lexer_next_token(lexer);
    lexer->current = saved;
    lexer->line = saved_line;
    lexer->column = saved_col;
    return t;
}

const char *token_type_name(TokenType type) {
    static const char *names[] = {
        "EOF", "ERROR",
        "INT_LIT", "FLOAT_LIT", "STRING_LIT", "CHAR_LIT", "IDENT",
        "I8", "I16", "I32", "I64", "U8", "U16", "U32", "U64",
        "F32", "F64", "BOOL", "STR", "VOID", "UINTPTR", "VAR", "CONST", "VOLATILE",
        "STRUCT", "UNION", "ENUM", "TYPE",
        "IF", "ELSE", "WHILE", "FOR", "SWITCH", "CASE",
        "RETURN", "BREAK", "CONTINUE", "TRUE", "FALSE",
        "FN", "PUB", "USE", "EXTERN", "DEFER", "AS",
        "SIZEOF", "ALIGNOF",
        "ADD", "SUB", "MUL", "DIV", "MOD",
        "ADD_ASSIGN", "SUB_ASSIGN", "MUL_ASSIGN", "DIV_ASSIGN", "MOD_ASSIGN",
        "EQ", "NE", "LT", "GT", "LE", "GE",
        "AND", "OR", "NOT",
        "BIT_AND", "BIT_OR", "BIT_XOR", "BIT_NOT", "SHL", "SHR",
        "BIT_AND_ASSIGN", "BIT_OR_ASSIGN", "BIT_XOR_ASSIGN", "SHL_ASSIGN", "SHR_ASSIGN",
        "DEREF", "ADDR_OF", "DOT", "ARROW", "ASSIGN",
        "LPAREN", "RPAREN", "LBRACE", "RBRACE", "LBRACKET", "RBRACKET",
        "COMMA", "SEMICOLON", "COLON",
    };
    return (type < TOKEN_COUNT) ? names[type] : "UNKNOWN";
}

void token_print(const Token *token) {
    printf("%s:%zu:%zu: %s", token->start, token->line, token->column, token_type_name(token->type));
    if (token->type == TOKEN_IDENT || token->type == TOKEN_INT_LIT || 
        token->type == TOKEN_FLOAT_LIT || token->type == TOKEN_STRING_LIT) {
        printf(" \"%.*s\"", (int)token->length, token->start);
    }
    printf("\n");
}

bool token_is_keyword(TokenType type) {
    return type >= KW_I8 && type <= KW_ALIGNOF;
}

bool token_is_type_keyword(TokenType type) {
    return type >= KW_I8 && type <= KW_VOLATILE;
}

bool token_is_operator(TokenType type) {
    return type >= OP_ADD && type <= OP_ARROW;
}