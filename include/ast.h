#ifndef CBANG_AST_H
#define CBANG_AST_H

#include "token.h"
#include <stddef.h>

/* ========================================================================== */
/* Forward Declarations                                                       */
/* ========================================================================== */

typedef struct ASTNode ASTNode;
typedef struct ASTNodeList ASTNodeList;

/*
 * io.print dispatch kinds.
 *
 * codegen cannot see types — it only walks the tree. sema, which does know
 * the type, writes one of these numbers onto the call node so codegen knows
 * which C helper to emit. See sema.c (io.print) and codegen.c (emit_expr).
 */
#define PRINT_NONE   0   /* not a builtin print, or unknown */
#define PRINT_STR    1   /* fwrite the slice */
#define PRINT_SINT   2   /* printf %lld */
#define PRINT_UINT   3   /* printf %llu  (u64 max > i64 max, so separate) */
#define PRINT_FLOAT  4   /* printf %g */
#define PRINT_BOOL   5   /* fputs "true"/"false" */

/*
 * std.math dispatch kinds. Same reason as PRINT_*: codegen cannot see types,
 * and min/max must not evaluate their arguments twice (a ternary would),
 * so sema picks the exact C helper and leaves it a note.
 */
#define MATH_NONE      0
#define MATH_ABS_INT   1   /* llabs  */
#define MATH_ABS_UINT  2   /* identity: abs of unsigned is itself */
#define MATH_ABS_FLT   3   /* fabs   */
#define MATH_SQRT      4
#define MATH_POW       5
#define MATH_FLOOR     6
#define MATH_CEIL      7
#define MATH_ROUND     8
#define MATH_MIN_INT   9   /* cb_min_i64 */
#define MATH_MAX_INT  10   /* cb_max_i64 */
#define MATH_MIN_UINT 11   /* cb_min_u64 */
#define MATH_MAX_UINT 12   /* cb_max_u64 */
#define MATH_MIN_FLT  13   /* cb_min_f64 */
#define MATH_MAX_FLT  14   /* cb_max_f64 */

/* ========================================================================== */
/* AST Node Types                                                             */
/* ========================================================================== */

typedef enum {
    /* Declarations */
    AST_VAR_DECL,       /* var x: i32 = 1; */
    AST_CONST_DECL,     /* const X = 1; */
    AST_FN_DECL,        /* fn name(params) ret { body } */
    AST_STRUCT_DECL,    /* struct Name { fields } */
    AST_UNION_DECL,     /* union Name { fields } */
    AST_ENUM_DECL,      /* enum Name { variants } */
    AST_TYPE_ALIAS,     /* type Name = Type; */
    AST_USE_DECL,       /* use std.io; */
    AST_EXTERN_DECL,    /* extern fn name(...); */

    /* Statements */
    AST_BLOCK,          /* { stmt* } */
    AST_IF_STMT,        /* if (cond) then [else] */
    AST_FOR_STMT,       /* for (init; cond; step) body */
    AST_WHILE_STMT,     /* while (cond) body */
    AST_SWITCH_STMT,    /* switch (expr) { case ... } */
    AST_CASE_STMT,      /* case expr: */
    AST_RETURN_STMT,    /* return [expr]; */
    AST_BREAK_STMT,     /* break; */
    AST_CONTINUE_STMT,  /* continue; */
    AST_DEFER_STMT,     /* defer { block } */
    AST_EXPR_STMT,      /* expr; */

    /* Expressions */
    AST_BINARY,         /* left op right */
    AST_UNARY,          /* op expr */
    AST_CALL,           /* fn(args...) */
    AST_INDEX,          /* expr[index] */
    AST_FIELD,          /* expr.field */
    AST_VAR_REF,        /* identifier */
    AST_INT_LIT,        /* 123 */
    AST_FLOAT_LIT,      /* 3.14 */
    AST_STRING_LIT,     /* "hello" */
    AST_CHAR_LIT,       /* 'a' */
    AST_BOOL_LIT,       /* true / false */
    AST_CAST,           /* expr as Type */
    AST_SIZEOF,         /* sizeof(Type) */
    AST_ALIGNOF,        /* alignof(Type) */

    /* Types (appear in declarations) */
    AST_TYPE_PRIMITIVE, /* i32, u64, f32, bool, str, void, uintptr */
    AST_TYPE_POINTER,   /* *Type */
    AST_TYPE_ARRAY,     /* [N]Type (future) */
    AST_TYPE_NAMED,     /* UserType */
} ASTNodeType;

/* ========================================================================== */
/* Type Representation                                                        */
/* ========================================================================== */

typedef struct Type Type;

struct Type {
    enum {
        TYPE_PRIMITIVE,
        TYPE_POINTER,
        TYPE_NAMED,
    } kind;
    union {
        TokenType primitive;  /* KW_I32, KW_U64, etc. */
        struct { Type *pointee; } pointer;
        struct { const char *name; size_t len; } named;
    } data;
};

/* ========================================================================== */
/* AST Node Structures                                                        */
/* ========================================================================== */

struct ASTNode {
    ASTNodeType type;
    Token token;           /* Leading token (for position info) */
    union {
        /* VAR_DECL, CONST_DECL */
        struct {
            bool is_mutable;      /* var vs const */
            Token name;
            Type *var_type;       /* NULL = infer */
            ASTNode *init;        /* NULL = uninitialized */
        } var_decl;

        /* FN_DECL */
        struct {
            bool is_pub;
            Token name;
            ASTNodeList *params;  /* VarDecl nodes */
            Type *ret_type;       /* NULL = void */
            ASTNode *body;        /* Block node */
            bool is_extern;
        } fn_decl;

        /* STRUCT_DECL, UNION_DECL */
        struct {
            bool is_pub;
            bool is_union;
            Token name;
            ASTNodeList *fields;  /* VarDecl nodes */
        } aggregate_decl;

        /* ENUM_DECL */
        struct {
            bool is_pub;
            Token name;
            ASTNodeList *variants; /* Identifiers (with optional values) */
        } enum_decl;

        /* TYPE_ALIAS */
        struct {
            bool is_pub;
            Token name;
            Type *aliased_type;
        } type_alias;

        /* USE_DECL */
        struct {
            Token path;  /* e.g., "std.io" as single token or we parse dot-separated */
        } use_decl;

        /* EXTERN_DECL */
        struct {
            ASTNode *decl;  /* FnDecl or VarDecl */
        } extern_decl;

        /* BLOCK */
        struct {
            ASTNodeList *stmts;
        } block;

        /* IF_STMT */
        struct {
            ASTNode *cond;
            ASTNode *then_branch;
            ASTNode *else_branch;  /* NULL if no else */
        } if_stmt;

        /* FOR_STMT */
        struct {
            ASTNode *init;      /* VarDecl or ExprStmt or NULL */
            ASTNode *cond;      /* Expr or NULL (infinite) */
            ASTNode *step;      /* Expr or NULL */
            ASTNode *body;      /* Block */
        } for_stmt;

        /* WHILE_STMT */
        struct {
            ASTNode *cond;
            ASTNode *body;
        } while_stmt;

        /* SWITCH_STMT */
        struct {
            ASTNode *expr;
            ASTNodeList *cases;  /* CaseStmt nodes */
        } switch_stmt;

        /* CASE_STMT */
        struct {
            ASTNode *expr;    /* NULL = default */
            ASTNode *body;    /* Block */
        } case_stmt;

        /* RETURN_STMT */
        struct {
            ASTNode *value;  /* NULL = return; */
        } return_stmt;

        /* DEFER_STMT */
        struct {
            ASTNode *body;  /* Block */
        } defer_stmt;

        /* EXPR_STMT */
        struct {
            ASTNode *expr;
        } expr_stmt;

        /* BINARY */
        struct {
            TokenType op;      /* OP_ADD, OP_EQ, etc. */
            ASTNode *left;
            ASTNode *right;
        } binary;

        /* UNARY */
        struct {
            TokenType op;      /* OP_SUB, OP_NOT, OP_BIT_NOT, OP_DEREF, OP_ADDR_OF */
            ASTNode *operand;
        } unary;

        /* CALL */
        struct {
            ASTNode *callee;
            ASTNodeList *args;
            int print_kind;     /* PRINT_* : set by sema for io.print only */
            int math_kind;      /* MATH_* : set by sema for math.* only */
        } call;

        /* INDEX */
        struct {
            ASTNode *base;
            ASTNode *index;
            bool base_is_str;   /* set by sema: base is a `str`, not a pointer */
        } index;

        /* FIELD */
        struct {
            ASTNode *base;
            Token field_name;
            bool on_str;        /* set by sema: base is a `str` */
        } field;

        /* VAR_REF */
        struct {
            Token name;
        } var_ref;

        /* INT_LIT */
        struct {
            unsigned long long value;   /* always non-negative: `-x` is unary minus */
        } int_lit;

        /* FLOAT_LIT */
        struct {
            double value;
        } float_lit;

        /* STRING_LIT */
        struct {
            const char *value;
            size_t length;
        } string_lit;

        /* CHAR_LIT */
        struct {
            char value;
        } char_lit;

        /* BOOL_LIT */
        struct {
            bool value;
        } bool_lit;

        /* CAST */
        struct {
            ASTNode *expr;
            Type *target_type;
        } cast;

        /* SIZEOF, ALIGNOF */
        struct {
            Type *target_type;
        } sizeof_alignof;

        /* TYPE_PRIMITIVE */
        struct {
            TokenType kind;  /* KW_I32, etc. */
        } type_primitive;

        /* TYPE_POINTER */
        struct {
            Type *pointee;
        } type_pointer;

        /* TYPE_NAMED */
        struct {
            Token name;
        } type_named;
    } data;
};

/* ========================================================================== */
/* Node List (Dynamic Array)                                                  */
/* ========================================================================== */

struct ASTNodeList {
    ASTNode **items;
    size_t count;
    size_t capacity;
};

/* ========================================================================== */
/* Parser                                                                     */
/* ========================================================================== */

typedef struct {
    Lexer lexer;
    Token current;
    Token previous;
    bool had_error;
    bool panic_mode;
    ASTNodeList *errors;
} Parser;

/* Initialize parser with source */
void parser_init(Parser *parser, const char *source, size_t length);

/* Parse entire file - returns list of top-level declarations */
ASTNodeList *parser_parse(Parser *parser);

/* Free AST nodes */
void ast_free(ASTNode *node);
void ast_list_free(ASTNodeList *list);

/* Debug printing */
void ast_print(const ASTNode *node, int indent);
void ast_list_print(const ASTNodeList *list);

/* Type helpers */
Type *type_primitive(TokenType kind);
Type *type_pointer(Type *pointee);
Type *type_named(const char *name, size_t len);
void type_free(Type *type);
const char *type_to_string(Type *type);

#endif