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

static VarSym *lookup_var(Token *name) {
    for (int d = scope_depth - 1; d >= 0; d--)
        for (size_t i = scope_counts[d]; i-- > 0;)
            if (name_eq_tok(&scope_stack[d][i].name, name))
                return &scope_stack[d][i];
    for (size_t i = global_count; i-- > 0;)
        if (name_eq_tok(&global_table[i].name, name))
            return &global_table[i];
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

static bool type_is_float(Type *t) {
    return t && t->kind == TYPE_PRIMITIVE && (t->data.primitive == KW_F32 || t->data.primitive == KW_F64);
}
static bool type_is_int(Type *t) {
    if (!t || t->kind != TYPE_PRIMITIVE) return false;
    TokenType k = t->data.primitive;
    return (k >= KW_I8 && k <= KW_U64) || k == KW_UINTPTR;
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

/* Structural type equality (primitive kind + pointer chains + names) */
static bool type_eq(Type *a, Type *b) {
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
                /* Left must be assignable, must be mutable (skip const check for now) */
                if (lt && !assignable(lt, rt)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "cannot assign '%s' to '%s'",
                             type_name(rt), type_name(lt));
                    err(&n->token, msg);
                }
                return lt;
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
                                if (at && !type_is_str(at)) {
                                    char msg[256];
                                    snprintf(msg, sizeof(msg),
                                             "io.print() expects str, got '%s'",
                                             type_name(at));
                                    err(&n->token, msg);
                                }
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
            if (type_is_str(bt)) return type_primitive(KW_U8);
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

static void sema_stmt(ASTNode *n) {
    if (!n) return;
    switch (n->type) {
        case AST_VAR_DECL: {
            Type *it = n->data.var_decl.init ? sema_expr(n->data.var_decl.init) : NULL;
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
            if (n->data.for_stmt.init) sema_stmt(n->data.for_stmt.init);
            if (n->data.for_stmt.cond) sema_expr(n->data.for_stmt.cond);
            if (n->data.for_stmt.step) sema_expr(n->data.for_stmt.step);
            if (n->data.for_stmt.body->type == AST_BLOCK) sema_block(n->data.for_stmt.body);
            else sema_stmt(n->data.for_stmt.body);
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
            if (n->data.while_stmt.body->type == AST_BLOCK) sema_block(n->data.while_stmt.body);
            else sema_stmt(n->data.while_stmt.body);
            break;
        }
        case AST_SWITCH_STMT:
            sema_expr(n->data.switch_stmt.expr);
            for (size_t i = 0; i < n->data.switch_stmt.cases->count; i++) {
                ASTNode *c = n->data.switch_stmt.cases->items[i];
                if (c->data.case_stmt.expr) sema_expr(c->data.case_stmt.expr);
                if (c->data.case_stmt.body) sema_block(c->data.case_stmt.body);
            }
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
        case AST_BREAK_STMT: case AST_CONTINUE_STMT: break;
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
        for (size_t j = 0; j < f->nparams; j++)
            f->params[j] = n->data.fn_decl.params->items[j]->data.var_decl.var_type;
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
}

int sema_check(ASTNodeList *program) {
    error_count = 0;
    fn_count = struct_count = global_count = 0;
    scope_depth = 0;
    in_defer = false;

    collect_top_level(program);

    /* Check use declarations: only std.io and std.os exist so far */
    for (size_t i = 0; i < program->count; i++) {
        ASTNode *n = program->items[i];
        if (n->type != AST_USE_DECL) continue;
        Token *p = &n->data.use_decl.path;
        bool known =
            (p->length == 6 && memcmp(p->start, "std.io", 6) == 0) ||
            (p->length == 6 && memcmp(p->start, "std.os", 6) == 0);
        if (!known) {
            char msg[256];
            snprintf(msg, sizeof(msg), "unknown module '%.*s' (available: std.io, std.os)",
                     (int)p->length, p->start);
            err(p, msg);
        }
    }

    /* Pass 2: check bodies */
    for (size_t i = 0; i < program->count; i++) {
        ASTNode *n = program->items[i];

        /* Check global initializers */
        if ((n->type == AST_CONST_DECL || n->type == AST_VAR_DECL) && n->data.var_decl.init)
            sema_expr(n->data.var_decl.init);

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