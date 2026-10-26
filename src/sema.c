#include "sema.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Error Reporting                                                            */
/* ========================================================================== */

static int error_count;

static void err(Token *t, const char *msg) {
    fprintf(stderr, "[type error] %zu:%zu: %s", t->line, t->column, msg);
    if (t->length > 0 && t->start)
        fprintf(stderr, " (at '%.*s')", (int)t->length, t->start);
    fprintf(stderr, "\n");
    error_count++;
}

/* ========================================================================== */
/* Symbol Tables                                                              */
/* ========================================================================== */

#define MAX_FUNCS 512
#define MAX_TYPES 512
#define MAX_VARS  1024
#define MAX_FIELDS 64
#define MAX_SCOPES 64

typedef struct { const char *name; size_t len; } Name;

typedef struct {
    Name name;
    Type **params;
    size_t nparams;
    Type *ret;
    bool is_extern;
} FnSym;

typedef struct {
    Name name;
    Name field_names[MAX_FIELDS];
    Type *field_types[MAX_FIELDS];
    size_t nfields;
} StructSym;

typedef struct {
    Name name;
    Type *type;
    bool is_const;
} VarSym;

static FnSym fn_table[MAX_FUNCS];    static size_t fn_count;
static StructSym struct_table[MAX_TYPES]; static size_t struct_count;
static VarSym global_table[MAX_VARS];    static size_t global_count;
static VarSym alias_table[MAX_TYPES];    static size_t alias_count;   /* type X = Y */

/* Local scope stack: each scope is a list of vars */
static VarSym scope_stack[MAX_SCOPES][128];
static size_t scope_counts[MAX_SCOPES];
static int scope_depth; /* 0 = no local scope */

static bool name_eq(Name *a, const char *b, size_t blen) {
    return a->len == blen && memcmp(a->name, b, blen) == 0;
}
static bool name_eq_tok(Name *a, Token *t) {
    return a->len == t->length && memcmp(a->name, t->start, t->length) == 0;
}

static void push_scope(void) { if (scope_depth < MAX_SCOPES) scope_counts[scope_depth++] = 0; }
static void pop_scope(void) { if (scope_depth > 0) scope_depth--; }

static void declare_local(Token *name, Type *type, bool is_const) {
    if (scope_depth == 0) return;
    int d = scope_depth - 1;
    if (scope_counts[d] >= 128) return;
    VarSym *v = &scope_stack[d][scope_counts[d]++];
    v->name = (Name){ name->start, name->length };
    v->type = type;
    v->is_const = is_const;
}

/*
 * How much of global_table is currently in scope.
 *
 * Top-level `var`/`const` are emitted in source order, so an initializer may
 * only name a global that appears above it. While checking a global's
 * initializer this is clamped to the entries declared so far; a name that
 * exists below it is reported as "declared later" instead of "undeclared".
 */
static size_t globals_visible;
static bool saw_future_global;

static VarSym *lookup_var(Token *name) {
    saw_future_global = false;
    for (int d = scope_depth - 1; d >= 0; d--)
        for (size_t i = scope_counts[d]; i-- > 0;)
            if (name_eq_tok(&scope_stack[d][i].name, name))
                return &scope_stack[d][i];
    for (size_t i = globals_visible; i-- > 0;)
        if (name_eq_tok(&global_table[i].name, name))
            return &global_table[i];
    for (size_t i = globals_visible; i < global_count; i++)
        if (name_eq_tok(&global_table[i].name, name)) {
            saw_future_global = true;
            break;
        }
    return NULL;
}

static FnSym *lookup_fn(Token *name) {
    for (size_t i = 0; i < fn_count; i++)
        if (name_eq_tok(&fn_table[i].name, name))
            return &fn_table[i];
    return NULL;
}

static StructSym *lookup_struct(const char *name, size_t len) {
    for (size_t i = 0; i < struct_count; i++)
        if (name_eq(&struct_table[i].name, name, len))
            return &struct_table[i];
    return NULL;
}

/* ========================================================================== */
/* Type Helpers                                                               */
/* ========================================================================== */

/* Integer literals are untyped until context gives them a type.
 * This sentinel type means "integer literal" - assignable to any
 * numeric type, so `var i: u32 = 0;` works. */
static Type g_int_lit = { .kind = TYPE_NAMED, .data.named = { "__int_lit__", 11 } };
#define INT_LIT (&g_int_lit)
static bool is_int_lit(Type *t) { return t == &g_int_lit; }

static Type *resolve_alias(Type *t);

static bool type_is_float(Type *t) {
    t = resolve_alias(t);
    return t && t->kind == TYPE_PRIMITIVE && (t->data.primitive == KW_F32 || t->data.primitive == KW_F64);
}
static bool type_is_int(Type *t) {
    t = resolve_alias(t);
    if (!t || t->kind != TYPE_PRIMITIVE) return false;
    TokenType k = t->data.primitive;
    return (k >= KW_I8 && k <= KW_U64) || k == KW_UINTPTR;
}
/* Signed vs unsigned matters when printing: u64's top half has no
   negative counterpart, so %lld would print it as a minus number. */
static bool type_is_unsigned(Type *t) {
    t = resolve_alias(t);
    if (!t || t->kind != TYPE_PRIMITIVE) return false;
    TokenType k = t->data.primitive;
    return (k >= KW_U8 && k <= KW_U64) || k == KW_UINTPTR;
}
/* Width in bits -- a literal shift count has to fit in the left operand,
   otherwise C calls it undefined and the C compiler warns about it. */
static int type_int_bits(Type *t) {
    t = resolve_alias(t);
    if (!t || t->kind != TYPE_PRIMITIVE) return 0;
    switch (t->data.primitive) {
        case KW_I8:  case KW_U8:  return 8;
        case KW_I16: case KW_U16: return 16;
        case KW_I32: case KW_U32: return 32;
        case KW_I64: case KW_U64: return 64;
        case KW_UINTPTR: return (int)(sizeof(void *) * 8);
        default: return 0;
    }
}
static bool type_is_bool(Type *t) {
    return t && t->kind == TYPE_PRIMITIVE && t->data.primitive == KW_BOOL;
}
static bool type_is_void(Type *t) {
    return !t || (t->kind == TYPE_PRIMITIVE && t->data.primitive == KW_VOID);
}
static bool type_is_str(Type *t) {
    return t && t->kind == TYPE_PRIMITIVE && t->data.primitive == KW_STR;
}

/* Follow `type X = Y` aliases so checks see the underlying type.
   A named type that isn't an alias (a struct/enum) is returned unchanged. */
static Type *resolve_alias(Type *t) {
    for (int guard = 0; t && t->kind == TYPE_NAMED && guard < 64; guard++) {
        const char *name = t->data.named.name;
        size_t len = t->data.named.len;
        bool found = false;
        for (size_t i = 0; i < alias_count; i++) {
            if (alias_table[i].name.len == len &&
                memcmp(alias_table[i].name.name, name, len) == 0) {
                t = alias_table[i].type;
                found = true;
                break;
            }
        }
        if (!found) break;
    }
    return t;
}

/* Structural type equality (primitive kind + pointer chains + names) */
static bool type_eq(Type *a, Type *b) {
    a = resolve_alias(a);
    b = resolve_alias(b);
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TYPE_PRIMITIVE: return a->data.primitive == b->data.primitive;
        case TYPE_POINTER: return type_eq(a->data.pointer.pointee, b->data.pointer.pointee);
        case TYPE_NAMED:
            return a->data.named.len == b->data.named.len &&
                   memcmp(a->data.named.name, b->data.named.name, a->data.named.len) == 0;
    }
    return false;
}

/* Can we assign `from` to a variable of type `to`? */
static bool assignable(Type *to, Type *from) {
    if (!to || !from) return true; /* void / untyped - don't be noisy */
    if (type_eq(to, from)) return true;
    /* Integer literals fit any numeric type */
    if (is_int_lit(from) && (type_is_int(to) || type_is_float(to))) return true;
    /* int literal initializes any pointer (null pointer), like C's `T *p = 0;` */
    if (is_int_lit(from) && to->kind == TYPE_POINTER) return true;
    /* C-style implicit numeric promotion: int<->int, int<->float, float<->float.
       (C! stays C-like here; use `as` when YOU want to be explicit.) */
    if ((type_is_int(to) || type_is_float(to)) &&
        (type_is_int(from) || type_is_float(from))) return true;
    return false;
}

/*
 * Declaration-time type check.
 *
 * These are the mistakes that used to sail straight through to the C
 * compiler, which then complained about a file the user never wrote --
 * `unknown type name 'Foo'', `variable or field 'v' declared void'.
 * The type checker sees the declaration, so it is the one that should say it.
 *
 * `program` and `at_index` add the ordering rule: top-level declarations are
 * emitted in source order, so a struct field or a global may only name a type
 * defined above it by value. Through a pointer it may name any type, because
 * the forward `typedef struct X X;` we emit keeps the name in scope.
 * at_index < 0 means "inside a function body", where everything above is
 * already on the page -- no ordering constraint at all.
 */
static long type_decl_index(ASTNodeList *program, const char *name, size_t len) {
    if (!program) return -1;
    for (size_t i = 0; i < program->count; i++) {
        ASTNode *n = program->items[i];
        Token *t = NULL;
        if (n->type == AST_STRUCT_DECL || n->type == AST_UNION_DECL)
            t = &n->data.aggregate_decl.name;
        else if (n->type == AST_ENUM_DECL)
            t = &n->data.enum_decl.name;
        else if (n->type == AST_TYPE_ALIAS)
            t = &n->data.type_alias.name;
        if (t && t->length == len && memcmp(t->start, name, len) == 0)
            return (long)i;
    }
    return -1;
}

static void check_type_at(Type *t, Token *at, const char *what,
                          ASTNodeList *program, long at_index, bool behind_ptr) {
    if (!t) return;
    if (t->kind == TYPE_PRIMITIVE) {
        if (t->data.primitive == KW_VOID) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s cannot have type 'void'", what);
            err(at, msg);
        }
        return;
    }
    if (t->kind == TYPE_POINTER) {
        /* `*Node` inside `struct Node` is legal: a pointer to an
           incomplete type is exactly what forward references need. */
        check_type_at(t->data.pointer.pointee, at, what, program, at_index, true);
        return;
    }
    if (t->kind != TYPE_NAMED) return;

    const char *nm = t->data.named.name;
    size_t len = t->data.named.len;
    long di = type_decl_index(program, nm, len);

    if (at_index >= 0 && !behind_ptr && di >= at_index && di >= 0) {
        char msg[256];
        if (di == at_index)
            snprintf(msg, sizeof(msg),
                     "type '%.*s' cannot contain itself by value (use a pointer: '*%.*s')",
                     (int)len, nm, (int)len, nm);
        else
            snprintf(msg, sizeof(msg),
                     "type '%.*s' is used before it is declared -- move the definition up, or use a pointer",
                     (int)len, nm);
        err(at, msg);
        return;
    }
    /* A name may be a struct/enum/union or a `type X = Y` alias -- each lives
       in its own table, so resolve first and only then call it unknown. */
    Type *r = resolve_alias(t);
    if (di < 0 && r == t && !lookup_struct(nm, len)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "unknown type '%.*s'", (int)len, nm);
        err(at, msg);
        return;
    }
    if (r != t) check_type_at(r, at, what, program, at_index, behind_ptr);
}

/*
 * Assignment needs something that can hold a value.
 *
 * `5 = x` and `f() = x` are C errors (lvalue required). Catching them here
 * gives one clear line instead of a compiler message about generated code.
 */
static bool is_lvalue(ASTNode *n) {
    if (!n) return false;
    switch (n->type) {
        case AST_VAR_REF:                       /* x = ... */
        case AST_FIELD:                         /* p.x = ...  (*p).x too */
        case AST_INDEX:                         /* a[i] = ... , s[i] = ... */
            return true;
        case AST_UNARY:
            return n->data.unary.op == OP_DEREF;  /* *p = ... */
        default:
            return false;
    }
}

/* Any `break` anywhere below, even in a nested loop -- conservative on
   purpose: assuming a loop may terminate is the safe direction. */
static bool has_break(ASTNode *n) {
    if (!n) return false;
    switch (n->type) {
        case AST_BREAK_STMT: return true;
        case AST_BLOCK:
            for (size_t i = 0; i < n->data.block.stmts->count; i++)
                if (has_break(n->data.block.stmts->items[i])) return true;
            return false;
        case AST_IF_STMT:
            return has_break(n->data.if_stmt.then_branch) ||
                   has_break(n->data.if_stmt.else_branch);
        case AST_FOR_STMT:
            return has_break(n->data.for_stmt.body);
        case AST_WHILE_STMT:
            return has_break(n->data.while_stmt.body);
        case AST_SWITCH_STMT:
            for (size_t i = 0; i < n->data.switch_stmt.cases->count; i++)
                if (has_break(n->data.switch_stmt.cases->items[i])) return true;
            return false;
        case AST_CASE_STMT:
            return has_break(n->data.case_stmt.body);
        case AST_DEFER_STMT:
            return has_break(n->data.defer_stmt.body);
        case AST_RETURN_STMT:
            return false;
        default:
            return false;
    }
}

/*
 * Does every path through this statement definitely return?
 *
 * Deliberately conservative: it only says "yes" when it is certain, so a
 * function is flagged only when control can really fall off the end. In C
 * that returns whatever happened to be in the register -- our tests once
 * exited with code 1 for that reason, which is a terrible bug to debug.
 */
static bool definitely_returns(ASTNode *n) {
    if (!n) return false;
    switch (n->type) {
        case AST_RETURN_STMT:
            return true;
        case AST_BLOCK:
            for (size_t i = 0; i < n->data.block.stmts->count; i++)
                if (definitely_returns(n->data.block.stmts->items[i])) return true;
            return false;
        case AST_IF_STMT:
            /* an if without else can always fall through */
            if (!n->data.if_stmt.else_branch) return false;
            return definitely_returns(n->data.if_stmt.then_branch) &&
                   definitely_returns(n->data.if_stmt.else_branch);
        case AST_WHILE_STMT:
            /* while (true) with no break never comes back */
            return n->data.while_stmt.cond &&
                   n->data.while_stmt.cond->type == AST_BOOL_LIT &&
                   n->data.while_stmt.cond->data.bool_lit.value &&
                   !has_break(n->data.while_stmt.body);
        case AST_FOR_STMT:
            /* for (;;) with no break never comes back */
            return !n->data.for_stmt.cond && !has_break(n->data.for_stmt.body);
        case AST_EXPR_STMT:
            /* os.exit() is declared void but never returns */
            return n->data.expr_stmt.expr &&
                   n->data.expr_stmt.expr->type == AST_CALL &&
                   n->data.expr_stmt.expr->data.call.callee->type == AST_FIELD &&
                   n->data.expr_stmt.expr->data.call.callee->data.field.base->type == AST_VAR_REF &&
                   n->data.expr_stmt.expr->data.call.callee->data.field.base->data.var_ref.name.length == 2 &&
                   memcmp(n->data.expr_stmt.expr->data.call.callee->data.field.base->data.var_ref.name.start,
                          "os", 2) == 0;
        default:
            return false;
    }
}

/* Display name for error messages */
static const char *type_name(Type *t) {
    if (is_int_lit(t)) return "int literal";
    return type_to_string(t);
}

/* ========================================================================== */
/* Name Collision Helpers (duplicate detection)                               */
/* ========================================================================== */

static bool struct_name_exists(Token *name) {
    for (size_t i = 0; i < struct_count; i++)
        if (name_eq_tok(&struct_table[i].name, name)) return true;
    return false;
}

/* ========================================================================== */
/* Forward Declarations                                                       */
/* ========================================================================== */

static Type *sema_expr(ASTNode *n);
static void sema_stmt(ASTNode *n);
static void sema_block(ASTNode *n);

/* ========================================================================== */
/* std.math                                                                   */
/* ========================================================================== */

/*
 * Type-check a math.* call and write the MATH_* note codegen needs.
 *
 * codegen sees only the tree, not the types, and it cannot expand
 * `math.min(a,b)` into `(a<b?a:b)` on its own because that would evaluate
 * its arguments twice -- math.min(i++, 5) would bump i twice. So sema picks
 * the exact C helper here and codegen just emits the call.
 *
 * Returns the call's result type.
 */
static Type *sema_math(ASTNode *n, Token *fn) {
    ASTNodeList *args = n->data.call.args;
    size_t nargs = args->count;
    char msg[256];
    Type *a = NULL, *b = NULL;

    n->data.call.math_kind = MATH_NONE;

    if (fn->length == 3 && memcmp(fn->start, "abs", 3) == 0) {
        if (nargs != 1) { err(&n->token, "math.abs() takes exactly 1 argument"); return type_primitive(KW_I32); }
        a = sema_expr(args->items[0]);
        if (!a) return type_primitive(KW_I32);
        if (is_int_lit(a)) { n->data.call.math_kind = MATH_ABS_INT; return type_primitive(KW_I32); }
        if (type_is_int(a)) {
            /* unsigned: every value is >= 0, so abs is a no-op */
            n->data.call.math_kind = type_is_unsigned(a) ? MATH_ABS_UINT : MATH_ABS_INT;
            return a;
        }
        if (type_is_float(a)) { n->data.call.math_kind = MATH_ABS_FLT; return a; }
        snprintf(msg, sizeof(msg), "math.abs() expects a number, got '%s'", type_name(a));
        err(&n->token, msg);
        return type_primitive(KW_I32);
    }

    if ((fn->length == 5 && memcmp(fn->start, "floor", 5) == 0) ||
        (fn->length == 4 && memcmp(fn->start, "ceil", 4) == 0) ||
        (fn->length == 5 && memcmp(fn->start, "round", 5) == 0)) {
        if (nargs != 1) {
            snprintf(msg, sizeof(msg), "math.%.*s() takes exactly 1 argument",
                     (int)fn->length, fn->start);
            err(&n->token, msg);
            return type_primitive(KW_F64);
        }
        a = sema_expr(args->items[0]);
        if (a && !type_is_float(a) && !type_is_int(a) && !is_int_lit(a)) {
            snprintf(msg, sizeof(msg), "math.%.*s() expects a number, got '%s'",
                     (int)fn->length, fn->start, type_name(a));
            err(&n->token, msg);
        }
        if (fn->start[0] == 'f')      n->data.call.math_kind = MATH_FLOOR;
        else if (fn->start[0] == 'c') n->data.call.math_kind = MATH_CEIL;
        else                          n->data.call.math_kind = MATH_ROUND;
        return type_primitive(KW_F64);
    }

    if (fn->length == 4 && memcmp(fn->start, "sqrt", 4) == 0) {
        if (nargs != 1) { err(&n->token, "math.sqrt() takes exactly 1 argument"); return type_primitive(KW_F64); }
        a = sema_expr(args->items[0]);
        if (a && !type_is_float(a) && !type_is_int(a) && !is_int_lit(a)) {
            snprintf(msg, sizeof(msg), "math.sqrt() expects a number, got '%s'", type_name(a));
            err(&n->token, msg);
        }
        n->data.call.math_kind = MATH_SQRT;
        return type_primitive(KW_F64);
    }

    if (fn->length == 3 && memcmp(fn->start, "pow", 3) == 0) {
        if (nargs != 2) { err(&n->token, "math.pow() takes exactly 2 arguments"); return type_primitive(KW_F64); }
        a = sema_expr(args->items[0]);
        b = sema_expr(args->items[1]);
        if (a && (!type_is_float(a) && !type_is_int(a) && !is_int_lit(a))) {
            snprintf(msg, sizeof(msg), "math.pow() expects a number, got '%s'", type_name(a));
            err(&n->token, msg);
        }
        if (b && (!type_is_float(b) && !type_is_int(b) && !is_int_lit(b))) {
            snprintf(msg, sizeof(msg), "math.pow() expects a number, got '%s'", type_name(b));
            err(&n->token, msg);
        }
        n->data.call.math_kind = MATH_POW;
        return type_primitive(KW_F64);
    }

    if ((fn->length == 3 && memcmp(fn->start, "min", 3) == 0) ||
        (fn->length == 3 && memcmp(fn->start, "max", 3) == 0)) {
        bool is_min = fn->start[0] == 'm' && fn->start[1] == 'i';
        if (nargs != 2) {
            snprintf(msg, sizeof(msg), "math.%.*s() takes exactly 2 arguments",
                     (int)fn->length, fn->start);
            err(&n->token, msg);
            return type_primitive(KW_I32);
        }
        a = sema_expr(args->items[0]);
        b = sema_expr(args->items[1]);
        if (!a || !b) return type_primitive(KW_I32);

        bool a_num = type_is_int(a) || is_int_lit(a) || type_is_float(a);
        bool b_num = type_is_int(b) || is_int_lit(b) || type_is_float(b);
        if (!a_num || !b_num) {
            snprintf(msg, sizeof(msg), "math.%.*s() expects two numbers, got '%s' and '%s'",
                     (int)fn->length, fn->start, type_name(a), type_name(b));
            err(&n->token, msg);
            return type_primitive(KW_I32);
        }

        if (type_is_float(a) || type_is_float(b)) {
            /* A float side wins and the other side is promoted. Only a float
               or an int literal may sit next to it: mixing a u64 variable
               with a f64 silently loses the top half of the value, so that
               gets caught instead. */
            if ((!type_is_float(a) && !is_int_lit(a)) ||
                (!type_is_float(b) && !is_int_lit(b))) {
                snprintf(msg, sizeof(msg), "math.%.*s() cannot mix '%s' and '%s'",
                         (int)fn->length, fn->start, type_name(a), type_name(b));
                err(&n->token, msg);
            }
            n->data.call.math_kind = is_min ? MATH_MIN_FLT : MATH_MAX_FLT;
            return type_is_float(a) ? a : b;
        }

        /* Two integers. An int literal has no signedness of its own, so it
           takes the signedness of the other side -- that is what lets
           math.max(count, 0) work without a cast. */
        bool a_u = type_is_unsigned(a), b_u = type_is_unsigned(b);
        if (type_is_int(a) && type_is_int(b) && a_u != b_u) {
            snprintf(msg, sizeof(msg), "math.%.*s() cannot mix signed and unsigned ('%s' vs '%s')",
                     (int)fn->length, fn->start, type_name(a), type_name(b));
            err(&n->token, msg);
            return type_primitive(KW_I32);
        }
        if (a_u || b_u) {
            n->data.call.math_kind = is_min ? MATH_MIN_UINT : MATH_MAX_UINT;
        } else {
            n->data.call.math_kind = is_min ? MATH_MIN_INT : MATH_MAX_INT;
        }
        return type_is_int(a) ? a : (type_is_int(b) ? b : type_primitive(KW_I32));
    }

    snprintf(msg, sizeof(msg), "unknown math function '%.*s'",
             (int)fn->length, fn->start);
    err(fn, msg);
    for (size_t i = 0; i < nargs; i++) sema_expr(args->items[i]);
    return type_primitive(KW_F64);
}

/* ========================================================================== */
/* Expression Checking  - returns the type of the expression                  */
/* ========================================================================== */

static Type *sema_expr(ASTNode *n) {
    if (!n) return NULL;
    switch (n->type) {
        case AST_INT_LIT:
            /* Untyped - context decides (fits any numeric type) */
            return INT_LIT;
        case AST_FLOAT_LIT: return type_primitive(KW_F64);
        case AST_STRING_LIT: return type_primitive(KW_STR);
        case AST_CHAR_LIT: return type_primitive(KW_I32);
        case AST_BOOL_LIT: return type_primitive(KW_BOOL);

        case AST_VAR_REF: {
            VarSym *v = lookup_var(&n->data.var_ref.name);
            if (!v) {
                char msg[256];
                if (saw_future_global)
                    /* exists, but below us: C would say "undeclared here" */
                    snprintf(msg, sizeof(msg),
                             "global '%.*s' is used before it is declared",
                             (int)n->data.var_ref.name.length,
                             n->data.var_ref.name.start);
                else
                    snprintf(msg, sizeof(msg), "undeclared variable '%.*s'",
                             (int)n->data.var_ref.name.length, n->data.var_ref.name.start);
                err(&n->data.var_ref.name, msg);
                return type_primitive(KW_I32); /* assume i32 to continue checking */
            }
            return v->type;
        }

        case AST_UNARY: {
            Type *t = sema_expr(n->data.unary.operand);
            switch (n->data.unary.op) {
                case OP_DEREF:
                    if (t && t->kind == TYPE_POINTER) return t->data.pointer.pointee;
                    err(&n->token, "cannot dereference non-pointer type");
                    return type_primitive(KW_I32);
                case OP_ADDR_OF: return type_pointer(t);
                case OP_NOT: return type_primitive(KW_BOOL);
                default: return t;
            }
        }

        case AST_BINARY: {
            Type *lt = sema_expr(n->data.binary.left);
            Type *rt = sema_expr(n->data.binary.right);
            TokenType op = n->data.binary.op;

            if (op == OP_ASSIGN) {
                /* Assignment target first, so `5 = x` reports the real
                   problem instead of a confusing type mismatch. */
                if (!is_lvalue(n->data.binary.left)) {
                    err(&n->token, "invalid assignment target");
                    return lt;
                }
                if (lt && !assignable(lt, rt)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "cannot assign '%s' to '%s'",
                             type_name(rt), type_name(lt));
                    err(&n->token, msg);
                }
                /* const is read-only. Compound assign desugars to `=` too,
                   so K += 1 is caught here as well. */
                if (n->data.binary.left->type == AST_VAR_REF) {
                    VarSym *v = lookup_var(&n->data.binary.left->data.var_ref.name);
                    if (v && v->is_const) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "cannot assign to const '%.*s'",
                                 (int)v->name.len, v->name.name);
                        err(&n->token, msg);
                    }
                }
                return lt;
            }

            /* Division and remainder by a literal zero: the C compiler warns
               about it and the program dies with SIGFPE at run time. */
            if ((op == OP_DIV || op == OP_MOD) &&
                n->data.binary.right->type == AST_INT_LIT &&
                n->data.binary.right->data.int_lit.value == 0) {
                err(&n->token, op == OP_DIV ? "division by zero"
                                            : "remainder by zero");
                return lt;
            }

            /* % in C is integer-only, and C rejects floats for << >> & | ^.
               Left unchecked these reach the C compiler as errors. */
            if (op == OP_MOD) {
                if (!type_is_int(lt) && !is_int_lit(lt)) {
                    err(&n->token, "'%' requires integer operands");
                    return type_primitive(KW_I32);
                }
            }
            if (op == OP_SHL || op == OP_SHR || op == OP_BIT_AND ||
                op == OP_BIT_OR || op == OP_BIT_XOR) {
                if (!type_is_int(lt) && !is_int_lit(lt)) {
                    err(&n->token, "bitwise and shift operators require integer operands");
                    return type_primitive(KW_I32);
                }
                if (op == OP_SHL || op == OP_SHR) {
                    /* a literal shift past the width is undefined in C and
                       gcc warns about it in the user's build */
                    if (n->data.binary.right->type == AST_INT_LIT) {
                        int width = type_is_int(lt) ? type_int_bits(lt) : 32;
                        unsigned long long s = n->data.binary.right->data.int_lit.value;
                        if (width > 0 && s >= (unsigned long long)width) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "shift count %llu is too large for '%s'",
                                     s, type_name(lt));
                            err(&n->token, msg);
                        }
                    }
                }
            }

            /* Comparison -> bool */
            if (op == OP_EQ || op == OP_NE || op == OP_LT || op == OP_GT ||
                op == OP_LE || op == OP_GE || op == OP_AND || op == OP_OR)
                return type_primitive(KW_BOOL);
            /* Arithmetic -> common type */
            if (lt && rt && !assignable(lt, rt) && !assignable(rt, lt)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "invalid operands '%s' and '%s' to binary operator",
                         type_name(lt), type_name(rt));
                err(&n->token, msg);
            }
            /* Prefer the concrete side over int literal */
            if (is_int_lit(lt) && !is_int_lit(rt)) return rt;
            if (is_int_lit(rt) && !is_int_lit(lt)) return lt;
            if (is_int_lit(lt) && is_int_lit(rt)) return type_primitive(KW_I32);
            return type_is_float(lt) || type_is_float(rt) ? (type_is_float(lt) ? lt : rt) : lt;
        }

        case AST_CALL: {
            /* Module call: io.print(...) - check via special table */
            ASTNode *callee = n->data.call.callee;
            if (callee->type == AST_FIELD) {
                ASTNode *base = callee->data.field.base;
                if (base->type == AST_VAR_REF) {
                    Token *mod = &base->data.var_ref.name;
                    Token *fn = &callee->data.field.field_name;
                    if (mod->length == 2 && memcmp(mod->start, "io", 2) == 0) {
                        if (fn->length == 5 && memcmp(fn->start, "print", 5) == 0) {
                            if (n->data.call.args->count != 1) {
                                err(&n->token, "io.print() takes exactly 1 argument");
                            } else {
                                Type *at = sema_expr(n->data.call.args->items[0]);
                                /*
                                 * codegen can't see types, so leave it a note
                                 * telling it which C helper to call.
                                 */
                                int kind = PRINT_NONE;
                                if (!at) {
                                    /* type unknown - sema_expr already reported */
                                } else if (type_is_str(at)) {
                                    kind = PRINT_STR;
                                } else if (is_int_lit(at)) {
                                    kind = PRINT_SINT;      /* io.print(42) */
                                } else if (type_is_int(at)) {
                                    kind = type_is_unsigned(at) ? PRINT_UINT
                                                                : PRINT_SINT;
                                } else if (type_is_float(at)) {
                                    kind = PRINT_FLOAT;
                                } else if (type_is_bool(at)) {
                                    kind = PRINT_BOOL;
                                } else {
                                    char msg[256];
                                    snprintf(msg, sizeof(msg),
                                             "io.print() cannot print type '%s'",
                                             type_name(at));
                                    err(&n->token, msg);
                                }
                                n->data.call.print_kind = kind;
                            }
                            return type_primitive(KW_VOID);
                        }
                        char msg[256];
                        snprintf(msg, sizeof(msg), "unknown io function '%.*s'",
                                 (int)fn->length, fn->start);
                        err(fn, msg);
                        return type_primitive(KW_VOID);
                    }
                    if (mod->length == 2 && memcmp(mod->start, "os", 2) == 0) {
                        if (fn->length == 4 && memcmp(fn->start, "exit", 4) == 0) {
                            if (n->data.call.args->count != 1) {
                                err(&n->token, "os.exit() takes exactly 1 argument");
                            } else {
                                Type *at = sema_expr(n->data.call.args->items[0]);
                                if (at && !type_is_int(at) && !is_int_lit(at)) {
                                    err(&n->token, "os.exit() expects an integer");
                                }
                            }
                            return type_primitive(KW_VOID);
                        }
                        char msg[256];
                        snprintf(msg, sizeof(msg), "unknown os function '%.*s'",
                                 (int)fn->length, fn->start);
                        err(fn, msg);
                        return type_primitive(KW_VOID);
                    }
                    if (mod->length == 4 && memcmp(mod->start, "math", 4) == 0) {
                        return sema_math(n, fn);
                    }
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "unknown module '%.*s' (declare it with 'use %.*s.xxx')",
                             (int)mod->length, mod->start,
                             (int)mod->length, mod->start);
                    err(mod, msg);
                    return type_primitive(KW_I32);
                }
            }

            /* Regular function call */
            if (callee->type != AST_VAR_REF) {
                err(&n->token, "expression is not callable");
                return type_primitive(KW_I32);
            }
            FnSym *f = lookup_fn(&callee->data.var_ref.name);
            if (!f) {
                char msg[256];
                snprintf(msg, sizeof(msg), "unknown function '%.*s'",
                         (int)callee->data.var_ref.name.length,
                         callee->data.var_ref.name.start);
                err(&callee->data.var_ref.name, msg);
                for (size_t i = 0; i < n->data.call.args->count; i++)
                    sema_expr(n->data.call.args->items[i]);
                return type_primitive(KW_I32);
            }
            if (n->data.call.args->count != f->nparams) {
                char msg[256];
                snprintf(msg, sizeof(msg), "'%.*s' expects %zu argument(s), got %zu",
                         (int)f->name.len, f->name.name, f->nparams,
                         n->data.call.args->count);
                err(&n->token, msg);
            }
            for (size_t i = 0; i < n->data.call.args->count; i++) {
                Type *at = sema_expr(n->data.call.args->items[i]);
                if (i < f->nparams && f->params[i] && at &&
                    !assignable(f->params[i], at)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "argument %zu: expected '%s', got '%s'",
                             i + 1, type_name(f->params[i]), type_name(at));
                    err(&n->token, msg);
                }
            }
            return f->ret ? f->ret : type_primitive(KW_VOID);
        }

        case AST_INDEX: {
            Type *bt = sema_expr(n->data.index.base);
            sema_expr(n->data.index.index);
            if (bt && bt->kind == TYPE_POINTER) return bt->data.pointer.pointee;
            if (type_is_str(bt)) {
                /* cb_str is a {ptr,len} struct, not an array — codegen needs
                   to know to emit `s.ptr[i]` instead of `s[i]` */
                n->data.index.base_is_str = true;
                return type_primitive(KW_U8);
            }
            if (bt) {
                char msg[256];
                snprintf(msg, sizeof(msg), "cannot index type '%s'", type_name(bt));
                err(&n->token, msg);
            }
            return type_primitive(KW_U8);
        }

        case AST_FIELD: {
            Type *bt = sema_expr(n->data.field.base);
            if (!bt) return type_primitive(KW_I32);

            /* Auto-deref: p.x where p is *Point -> emit p->x (spec 3.3) */
            if (bt->kind == TYPE_POINTER) {
                if (n->token.type == OP_ARROW) {
                    /* user wrote -> on a pointer: also fine, same result */
                } else {
                    /* rewrite . to -> so codegen emits C's -> */
                    n->token.type = OP_ARROW;
                }
                bt = bt->data.pointer.pointee;
            } else if (n->token.type == OP_ARROW) {
                err(&n->token, "'->' requires a pointer type");
                return type_primitive(KW_I32);
            }

            /* str has .len and .ptr */
            if (type_is_str(bt)) {
                n->data.field.on_str = true;   /* .ptr needs a const cast in C */
                if (n->data.field.field_name.length == 3 &&
                    memcmp(n->data.field.field_name.start, "len", 3) == 0)
                    return type_primitive(KW_U64);
                if (n->data.field.field_name.length == 3 &&
                    memcmp(n->data.field.field_name.start, "ptr", 3) == 0)
                    return type_pointer(type_primitive(KW_U8));
                err(&n->data.field.field_name, "str has only .len and .ptr");
                return type_primitive(KW_I32);
            }

            if (bt->kind == TYPE_NAMED) {
                StructSym *st = lookup_struct(bt->data.named.name, bt->data.named.len);
                if (!st) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "unknown type '%.*s'",
                             (int)bt->data.named.len, bt->data.named.name);
                    err(&n->token, msg);
                    return type_primitive(KW_I32);
                }
                for (size_t i = 0; i < st->nfields; i++)
                    if (name_eq_tok(&st->field_names[i], &n->data.field.field_name))
                        return st->field_types[i];
                char msg[256];
                snprintf(msg, sizeof(msg), "type '%.*s' has no field '%.*s'",
                         (int)st->name.len, st->name.name,
                         (int)n->data.field.field_name.length,
                         n->data.field.field_name.start);
                err(&n->data.field.field_name, msg);
                return type_primitive(KW_I32);
            }

            char msg[256];
            snprintf(msg, sizeof(msg), "type '%s' has no fields", type_name(bt));
            err(&n->token, msg);
            return type_primitive(KW_I32);
        }

        case AST_CAST: {
            sema_expr(n->data.cast.expr);
            return n->data.cast.target_type;
        }
        case AST_SIZEOF: case AST_ALIGNOF:
            return type_primitive(KW_U64);

        default:
            return NULL;
    }
}

/* ========================================================================== */
/* Statement Checking                                                         */
/* ========================================================================== */

static Type *current_ret_type;

static void sema_block(ASTNode *n) {
    if (!n || n->type != AST_BLOCK) return;
    push_scope();
    for (size_t i = 0; i < n->data.block.stmts->count; i++)
        sema_stmt(n->data.block.stmts->items[i]);
    pop_scope();
}

static bool in_defer;

/* Loop/switch nesting for break and continue. Without this, `break;` at
   function top level reached the C compiler, which complained about a
   generated file instead of the line the user typed. */
static int loop_depth;
static int switch_depth;

static void sema_stmt(ASTNode *n) {
    if (!n) return;
    switch (n->type) {
        case AST_VAR_DECL: {
            Type *it = n->data.var_decl.init ? sema_expr(n->data.var_decl.init) : NULL;
            if (n->data.var_decl.var_type) {
                char what[128];
                snprintf(what, sizeof(what), "variable '%.*s'",
                         (int)n->data.var_decl.name.length, n->data.var_decl.name.start);
                check_type_at(n->data.var_decl.var_type, &n->data.var_decl.name,
                              what, NULL, -1, false);
            }
            if (n->data.var_decl.init && n->data.var_decl.var_type && it &&
                !assignable(n->data.var_decl.var_type, it)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "cannot initialize '%s' with '%s'",
                         type_name(n->data.var_decl.var_type), type_name(it));
                err(&n->data.var_decl.name, msg);
            }
            declare_local(&n->data.var_decl.name, n->data.var_decl.var_type,
                          !n->data.var_decl.is_mutable);
            break;
        }
        case AST_CONST_DECL: {
            Type *it = n->data.var_decl.init ? sema_expr(n->data.var_decl.init) : NULL;
            Type *t = n->data.var_decl.var_type ? n->data.var_decl.var_type : it;
            if (n->data.var_decl.var_type) {
                char what[128];
                snprintf(what, sizeof(what), "constant '%.*s'",
                         (int)n->data.var_decl.name.length, n->data.var_decl.name.start);
                check_type_at(n->data.var_decl.var_type, &n->data.var_decl.name,
                              what, NULL, -1, false);
            }
            declare_local(&n->data.var_decl.name, t, true);
            break;
        }
        case AST_BLOCK: sema_block(n); break;
        case AST_IF_STMT: {
            Type *ct = sema_expr(n->data.if_stmt.cond);
            if (ct && !type_is_bool(ct) && !type_is_int(ct) && !is_int_lit(ct)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "if condition must be bool or int, got '%s'",
                         type_name(ct));
                err(&n->token, msg);
            }
            if (n->data.if_stmt.then_branch->type == AST_BLOCK) sema_block(n->data.if_stmt.then_branch);
            else sema_stmt(n->data.if_stmt.then_branch);
            if (n->data.if_stmt.else_branch) {
                if (n->data.if_stmt.else_branch->type == AST_BLOCK) sema_block(n->data.if_stmt.else_branch);
                else sema_stmt(n->data.if_stmt.else_branch);
            }
            break;
        }
        case AST_FOR_STMT:
            push_scope();
            loop_depth++;
            if (n->data.for_stmt.init) sema_stmt(n->data.for_stmt.init);
            if (n->data.for_stmt.cond) sema_expr(n->data.for_stmt.cond);
            if (n->data.for_stmt.step) sema_expr(n->data.for_stmt.step);
            if (n->data.for_stmt.body->type == AST_BLOCK) sema_block(n->data.for_stmt.body);
            else sema_stmt(n->data.for_stmt.body);
            loop_depth--;
            pop_scope();
            break;
        case AST_WHILE_STMT: {
            Type *ct = sema_expr(n->data.while_stmt.cond);
            if (ct && !type_is_bool(ct) && !type_is_int(ct) && !is_int_lit(ct)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "while condition must be bool or int, got '%s'",
                         type_name(ct));
                err(&n->token, msg);
            }
            loop_depth++;
            if (n->data.while_stmt.body->type == AST_BLOCK) sema_block(n->data.while_stmt.body);
            else sema_stmt(n->data.while_stmt.body);
            loop_depth--;
            break;
        }
        case AST_SWITCH_STMT:
            sema_expr(n->data.switch_stmt.expr);
            switch_depth++;
            for (size_t i = 0; i < n->data.switch_stmt.cases->count; i++) {
                ASTNode *c = n->data.switch_stmt.cases->items[i];
                if (c->data.case_stmt.expr) sema_expr(c->data.case_stmt.expr);
                if (c->data.case_stmt.body) sema_block(c->data.case_stmt.body);
            }
            switch_depth--;
            break;
        case AST_RETURN_STMT: {
            if (in_defer) {
                err(&n->token, "return not allowed inside defer block");
                break;
            }
            Type *vt = n->data.return_stmt.value ? sema_expr(n->data.return_stmt.value) : NULL;
            if (n->data.return_stmt.value && current_ret_type &&
                !assignable(current_ret_type, vt)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "cannot return '%s' from function returning '%s'",
                         type_name(vt), type_name(current_ret_type));
                err(&n->token, msg);
            }
            if (!n->data.return_stmt.value && current_ret_type &&
                !type_is_void(current_ret_type)) {
                err(&n->token, "function must return a value");
            }
            break;
        }
        case AST_BREAK_STMT:
            if (loop_depth == 0 && switch_depth == 0)
                err(&n->token, "'break' outside a loop or switch");
            break;
        case AST_CONTINUE_STMT:
            if (loop_depth == 0)
                err(&n->token, "'continue' outside a loop");
            break;
        case AST_DEFER_STMT: {
            bool saved = in_defer;
            in_defer = true;
            if (n->data.defer_stmt.body) {
                if (n->data.defer_stmt.body->type == AST_BLOCK) sema_block(n->data.defer_stmt.body);
                else sema_stmt(n->data.defer_stmt.body);
            }
            in_defer = saved;
            break;
        }
        case AST_EXPR_STMT: sema_expr(n->data.expr_stmt.expr); break;
        default: break;
    }
}

/* ========================================================================== */
/* Top-Level: Pass 1 = collect symbols, Pass 2 = check bodies                 */
/* ========================================================================== */

static void collect_top_level(ASTNodeList *program) {
    /* Pass 1a: functions */
    for (size_t i = 0; i < program->count; i++) {
        ASTNode *n = program->items[i];
        if (n->type != AST_FN_DECL) continue;
        if (lookup_fn(&n->data.fn_decl.name)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "duplicate function '%.*s'",
                     (int)n->data.fn_decl.name.length, n->data.fn_decl.name.start);
            err(&n->data.fn_decl.name, msg);
            continue;
        }
        if (fn_count >= MAX_FUNCS) break;
        FnSym *f = &fn_table[fn_count++];
        f->name = (Name){ n->data.fn_decl.name.start, n->data.fn_decl.name.length };
        f->nparams = n->data.fn_decl.params->count;
        f->params = calloc(f->nparams ? f->nparams : 1, sizeof(Type*));
        for (size_t j = 0; j < f->nparams; j++) {
            ASTNode *p = n->data.fn_decl.params->items[j];
            f->params[j] = p->data.var_decl.var_type;
            /* `fn f(a: i32, a: i32)` -- C says "redefinition of parameter" */
            for (size_t k = 0; k < j; k++) {
                Token *q = &n->data.fn_decl.params->items[k]->data.var_decl.name;
                if (q->length == p->data.var_decl.name.length &&
                    memcmp(q->start, p->data.var_decl.name.start, q->length) == 0) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "duplicate parameter '%.*s'",
                             (int)p->data.var_decl.name.length, p->data.var_decl.name.start);
                    err(&p->data.var_decl.name, msg);
                    break;
                }
            }
        }
        f->ret = n->data.fn_decl.ret_type;
        f->is_extern = n->data.fn_decl.is_extern;
    }

    /* Pass 1b: structs/unions/enums (enums registered as empty structs for name check) */
    for (size_t i = 0; i < program->count; i++) {
        ASTNode *n = program->items[i];
        bool is_agg = (n->type == AST_STRUCT_DECL || n->type == AST_UNION_DECL);
        bool is_enum = (n->type == AST_ENUM_DECL);
        if (!is_agg && !is_enum) continue;
        Token *name = is_agg ? &n->data.aggregate_decl.name : &n->data.enum_decl.name;
        if (struct_name_exists(name)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "duplicate type '%.*s'", (int)name->length, name->start);
            err(name, msg);
            continue;
        }
        if (struct_count >= MAX_TYPES) break;
        StructSym *s = &struct_table[struct_count++];
        s->name = (Name){ name->start, name->length };
        s->nfields = 0;
        if (is_agg) {
            for (size_t j = 0; j < n->data.aggregate_decl.fields->count && s->nfields < MAX_FIELDS; j++) {
                ASTNode *fld = n->data.aggregate_decl.fields->items[j];
                /* `struct S { i32 x; i32 x; }` -- C says "duplicate member" */
                for (size_t k = 0; k < s->nfields; k++)
                    if (name_eq_tok(&s->field_names[k], &fld->data.var_decl.name)) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "duplicate field '%.*s'",
                                 (int)fld->data.var_decl.name.length,
                                 fld->data.var_decl.name.start);
                        err(&fld->data.var_decl.name, msg);
                        break;
                    }
                s->field_names[s->nfields] = (Name){ fld->data.var_decl.name.start,
                                                     fld->data.var_decl.name.length };
                s->field_types[s->nfields] = fld->data.var_decl.var_type;
                s->nfields++;
            }
        } else {
            /* enums: register variants as global consts of the enum type */
            Type *et = type_named(name->start, name->length);
            for (size_t j = 0; j < n->data.enum_decl.variants->count; j++) {
                ASTNode *v = n->data.enum_decl.variants->items[j];
                Token *vn = (v->type == AST_BINARY)
                    ? &v->data.binary.left->data.var_ref.name
                    : &v->data.var_ref.name;
                if (global_count < MAX_VARS) {
                    VarSym *gv = &global_table[global_count++];
                    gv->name = (Name){ vn->start, vn->length };
                    gv->type = et;
                    gv->is_const = true;
                }
            }
        }
    }

    /* Pass 1d: type aliases (type X = Y) so checks can follow them */
    for (size_t i = 0; i < program->count; i++) {
        ASTNode *n = program->items[i];
        if (n->type != AST_TYPE_ALIAS) continue;
        Token *name = &n->data.type_alias.name;
        if (alias_count < MAX_TYPES) {
            VarSym *a = &alias_table[alias_count++];
            a->name = (Name){ name->start, name->length };
            a->type = n->data.type_alias.aliased_type;
        }
    }

    /* Pass 1c: globals (const/var at top level) */
    for (size_t i = 0; i < program->count; i++) {
        ASTNode *n = program->items[i];
        if (n->type != AST_CONST_DECL && n->type != AST_VAR_DECL) continue;
        Token *name = &n->data.var_decl.name;
        for (size_t j = 0; j < global_count; j++)
            if (name_eq_tok(&global_table[j].name, name)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "duplicate global '%.*s'",
                         (int)name->length, name->start);
                err(name, msg);
                goto next;
            }
        if (global_count < MAX_VARS) {
            VarSym *gv = &global_table[global_count++];
            gv->name = (Name){ name->start, name->length };
            gv->type = n->data.var_decl.var_type;
            gv->is_const = (n->type == AST_CONST_DECL);
        }
        next:;
    }

    /* Pass 1e: validate every declared type -- existence, no void storage,
       and the source-order rule for the positions that land in the C file
       before the type's own definition (struct fields, aliases, globals).
       Runs last so every type and alias is known by name. */
    for (size_t i = 0; i < program->count; i++) {
        ASTNode *n = program->items[i];
        long idx = (long)i;
        if (n->type == AST_STRUCT_DECL || n->type == AST_UNION_DECL) {
            for (size_t j = 0; j < n->data.aggregate_decl.fields->count; j++) {
                ASTNode *fld = n->data.aggregate_decl.fields->items[j];
                char what[128];
                snprintf(what, sizeof(what), "field '%.*s'",
                         (int)fld->data.var_decl.name.length,
                         fld->data.var_decl.name.start);
                check_type_at(fld->data.var_decl.var_type, &fld->data.var_decl.name,
                              what, program, idx, false);
            }
        } else if (n->type == AST_TYPE_ALIAS) {
            char what[128];
            snprintf(what, sizeof(what), "alias '%.*s'",
                     (int)n->data.type_alias.name.length,
                     n->data.type_alias.name.start);
            check_type_at(n->data.type_alias.aliased_type, &n->data.type_alias.name,
                          what, program, idx, false);
        } else if (n->type == AST_VAR_DECL || n->type == AST_CONST_DECL) {
            char what[128];
            snprintf(what, sizeof(what), "global '%.*s'",
                     (int)n->data.var_decl.name.length, n->data.var_decl.name.start);
            check_type_at(n->data.var_decl.var_type, &n->data.var_decl.name,
                          what, program, idx, false);
        } else if (n->type == AST_FN_DECL) {
            /* function bodies are emitted last: no ordering rule, but the
               parameter and return types still have to exist and not be void */
            for (size_t j = 0; j < n->data.fn_decl.params->count; j++) {
                ASTNode *p = n->data.fn_decl.params->items[j];
                char what[128];
                snprintf(what, sizeof(what), "parameter '%.*s'",
                         (int)p->data.var_decl.name.length, p->data.var_decl.name.start);
                check_type_at(p->data.var_decl.var_type, &p->data.var_decl.name,
                              what, program, -1, false);
            }
        }
    }
}

int sema_check(ASTNodeList *program) {
    error_count = 0;
    fn_count = struct_count = global_count = alias_count = 0;
    scope_depth = 0;
    in_defer = false;
    loop_depth = switch_depth = 0;

    collect_top_level(program);

    /* Everything is visible from a function body; global initializers clamp
       this while they run (top-level items are emitted in source order). */
    globals_visible = global_count;
    size_t n_globals = 0;
    for (size_t i = 0; i < program->count; i++)
        if (program->items[i]->type == AST_CONST_DECL ||
            program->items[i]->type == AST_VAR_DECL) n_globals++;
    size_t gbase = global_count - n_globals;
    size_t gseen = 0;

    /* Check use declarations: only std.io, std.os and std.math exist so far */
    for (size_t i = 0; i < program->count; i++) {
        ASTNode *n = program->items[i];
        if (n->type != AST_USE_DECL) continue;
        Token *p = &n->data.use_decl.path;
        bool known =
            (p->length == 6 && memcmp(p->start, "std.io", 6) == 0) ||
            (p->length == 6 && memcmp(p->start, "std.os", 6) == 0) ||
            (p->length == 8 && memcmp(p->start, "std.math", 8) == 0);
        if (!known) {
            char msg[256];
            snprintf(msg, sizeof(msg), "unknown module '%.*s' (available: std.io, std.os, std.math)",
                     (int)p->length, p->start);
            err(p, msg);
        }
    }

    /* Pass 2: check bodies */
    for (size_t i = 0; i < program->count; i++) {
        ASTNode *n = program->items[i];

        /* Check global initializers. Only the globals declared above this
           one are on the page yet, so hide the rest for the duration. */
        if (n->type == AST_CONST_DECL || n->type == AST_VAR_DECL) {
            if (n->data.var_decl.init) {
                globals_visible = gbase + gseen + 1;
                sema_expr(n->data.var_decl.init);
                globals_visible = global_count;
            }
            gseen++;
        }

        if (n->type != AST_FN_DECL || !n->data.fn_decl.body) continue;

        current_ret_type = n->data.fn_decl.ret_type;
        scope_depth = 0;
        push_scope();
        /* declare params */
        for (size_t j = 0; j < n->data.fn_decl.params->count; j++) {
            ASTNode *p = n->data.fn_decl.params->items[j];
            declare_local(&p->data.var_decl.name, p->data.var_decl.var_type, false);
        }
        sema_block(n->data.fn_decl.body);
        pop_scope();
        scope_depth = 0;

        /* Falling off the end of a non-void function returns whatever
           happened to be in the register -- undefined, and miserable to
           debug. Say so here instead. */
        if (current_ret_type && !type_is_void(current_ret_type) &&
            !definitely_returns(n->data.fn_decl.body)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "control reaches the end of '%.*s' without returning a value",
                     (int)n->data.fn_decl.name.length, n->data.fn_decl.name.start);
            err(&n->data.fn_decl.name, msg);
        }
    }

    /* Check main exists when building executables - always warn if pub main missing */
    bool has_main = false;
    for (size_t i = 0; i < fn_count; i++)
        if (fn_table[i].name.len == 4 && memcmp(fn_table[i].name.name, "main", 4) == 0)
            has_main = true;
    if (!has_main)
        fprintf(stderr, "[warning] no 'main' function found - program will not be runnable\n");

    /* cleanup function params */
    for (size_t i = 0; i < fn_count; i++) free(fn_table[i].params);

    return error_count;
}
