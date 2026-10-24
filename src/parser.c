#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Memory Helpers                                                             */
/* ========================================================================== */

static ASTNode *node_alloc(ASTNodeType type, Token tok) {
    ASTNode *n = calloc(1, sizeof(ASTNode));
    if (!n) { fprintf(stderr, "oom\n"); exit(1); }
    n->type = type;
    n->token = tok;
    return n;
}

ASTNodeList *list_new(void) {
    ASTNodeList *l = malloc(sizeof(ASTNodeList));
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
    return l;
}

static void list_push(ASTNodeList *l, ASTNode *node) {
    if (l->count == l->capacity) {
        l->capacity = l->capacity ? l->capacity * 2 : 8;
        l->items = realloc(l->items, l->capacity * sizeof(ASTNode*));
    }
    l->items[l->count++] = node;
}

void ast_list_free(ASTNodeList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) ast_free(list->items[i]);
    free(list->items);
    free(list);
}

void type_free(Type *t) {
    if (!t) return;
    if (t->kind == TYPE_POINTER) type_free(t->data.pointer.pointee);
    free(t);
}

void ast_free(ASTNode *n) {
    if (!n) return;
    switch (n->type) {
        case AST_VAR_DECL:
            type_free(n->data.var_decl.var_type);
            ast_free(n->data.var_decl.init);
            break;
        case AST_CONST_DECL:
            type_free(n->data.var_decl.var_type);
            ast_free(n->data.var_decl.init);
            break;
        case AST_FN_DECL:
            ast_list_free(n->data.fn_decl.params);
            type_free(n->data.fn_decl.ret_type);
            ast_free(n->data.fn_decl.body);
            break;
        case AST_STRUCT_DECL: case AST_UNION_DECL:
            ast_list_free(n->data.aggregate_decl.fields);
            break;
        case AST_ENUM_DECL:
            ast_list_free(n->data.enum_decl.variants);
            break;
        case AST_TYPE_ALIAS:
            type_free(n->data.type_alias.aliased_type);
            break;
        case AST_EXTERN_DECL:
            ast_free(n->data.extern_decl.decl);
            break;
        case AST_BLOCK:
            ast_list_free(n->data.block.stmts);
            break;
        case AST_IF_STMT:
            ast_free(n->data.if_stmt.cond);
            ast_free(n->data.if_stmt.then_branch);
            ast_free(n->data.if_stmt.else_branch);
            break;
        case AST_FOR_STMT:
            ast_free(n->data.for_stmt.init);
            ast_free(n->data.for_stmt.cond);
            ast_free(n->data.for_stmt.step);
            ast_free(n->data.for_stmt.body);
            break;
        case AST_WHILE_STMT:
            ast_free(n->data.while_stmt.cond);
            ast_free(n->data.while_stmt.body);
            break;
        case AST_SWITCH_STMT:
            ast_free(n->data.switch_stmt.expr);
            ast_list_free(n->data.switch_stmt.cases);
            break;
        case AST_CASE_STMT:
            ast_free(n->data.case_stmt.expr);
            ast_free(n->data.case_stmt.body);
            break;
        case AST_RETURN_STMT:
            ast_free(n->data.return_stmt.value);
            break;
        case AST_DEFER_STMT:
            ast_free(n->data.defer_stmt.body);
            break;
        case AST_EXPR_STMT:
            ast_free(n->data.expr_stmt.expr);
            break;
        case AST_BINARY:
            ast_free(n->data.binary.left);
            ast_free(n->data.binary.right);
            break;
        case AST_UNARY:
            ast_free(n->data.unary.operand);
            break;
        case AST_CALL:
            ast_free(n->data.call.callee);
            ast_list_free(n->data.call.args);
            break;
        case AST_INDEX:
            ast_free(n->data.index.base);
            ast_free(n->data.index.index);
            break;
        case AST_FIELD:
            ast_free(n->data.field.base);
            break;
        case AST_CAST:
            ast_free(n->data.cast.expr);
            type_free(n->data.cast.target_type);
            break;
        case AST_SIZEOF: case AST_ALIGNOF:
            type_free(n->data.sizeof_alignof.target_type);
            break;
        default: break;
    }
    free(n);
}

/* ========================================================================== */
/* Deep Clone                                                                 */
/* ========================================================================== */

/*
 * Why this exists: compound assignment desugaring reuses the same sub-tree
 * twice.  `a[i] += v` becomes  a[i] = a[i] + v  — the `a[i]` node is needed
 * in two places, but the AST is a tree, not a graph: every node is freed by
 * its parent.  Sharing one node between two parents means it gets freed
 * twice (use-after-free / double free).  So we clone.
 */
static ASTNode *ast_clone(ASTNode *n);
static ASTNodeList *list_clone(ASTNodeList *l);

/* Types are heap-allocated and owned by their node (ast_free frees them),
   so a cloned node needs its own copy.  Pointers are walked; names alias
   the source text, which lives as long as the program. */
static Type *type_clone(Type *t) {
    if (!t) return NULL;
    Type *c = calloc(1, sizeof(Type));
    if (!c) { fprintf(stderr, "oom\n"); exit(1); }
    memcpy(c, t, sizeof(Type));
    if (t->kind == TYPE_POINTER)
        c->data.pointer.pointee = type_clone(t->data.pointer.pointee);
    return c;
}

static ASTNode *ast_clone(ASTNode *n) {
    if (!n) return NULL;
    ASTNode *c = calloc(1, sizeof(ASTNode));
    if (!c) { fprintf(stderr, "oom\n"); exit(1); }
    memcpy(c, n, sizeof(ASTNode));   /* copy token + scalar fields */

    switch (n->type) {
        case AST_BINARY:
            c->data.binary.left  = ast_clone(n->data.binary.left);
            c->data.binary.right = ast_clone(n->data.binary.right);
            break;
        case AST_UNARY:
            c->data.unary.operand = ast_clone(n->data.unary.operand);
            break;
        case AST_FIELD:
            c->data.field.base = ast_clone(n->data.field.base);
            break;
        case AST_INDEX:
            c->data.index.base  = ast_clone(n->data.index.base);
            c->data.index.index = ast_clone(n->data.index.index);
            break;
        case AST_CALL:
            c->data.call.callee = ast_clone(n->data.call.callee);
            c->data.call.args   = list_clone(n->data.call.args);
            break;
        case AST_CAST:
            c->data.cast.expr = ast_clone(n->data.cast.expr);
            c->data.cast.target_type = type_clone(n->data.cast.target_type);
            break;
        case AST_SIZEOF: case AST_ALIGNOF:
            c->data.sizeof_alignof.target_type = type_clone(n->data.sizeof_alignof.target_type);
            break;
        case AST_VAR_DECL: case AST_CONST_DECL:
            c->data.var_decl.var_type = type_clone(n->data.var_decl.var_type);
            c->data.var_decl.init = ast_clone(n->data.var_decl.init);
            break;
        default:
            break;  /* leaves: literals, var refs, field names */
    }
    return c;
}

static ASTNodeList *list_clone(ASTNodeList *l) {
    ASTNodeList *out = list_new();
    if (!l) return out;
    for (size_t i = 0; i < l->count; i++)
        list_push(out, ast_clone(l->items[i]));
    return out;
}

/* ========================================================================== */
/* Type Helpers                                                               */
/* ========================================================================== */

Type *type_primitive(TokenType kind) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TYPE_PRIMITIVE;
    t->data.primitive = kind;
    return t;
}

Type *type_pointer(Type *pointee) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TYPE_POINTER;
    t->data.pointer.pointee = pointee;
    return t;
}

Type *type_named(const char *name, size_t len) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TYPE_NAMED;
    t->data.named.name = name;
    t->data.named.len = len;
    return t;
}

const char *type_to_string(Type *type) {
    static char buf[256];
    if (!type) return "void";
    switch (type->kind) {
        case TYPE_PRIMITIVE:
            return token_type_name(type->data.primitive);
        case TYPE_POINTER: {
            /* Recursive call returns this same static buf, so copy it out
               first — writing to buf while reading from it corrupts the name. */
            char sub[248];
            snprintf(sub, sizeof(sub), "%s", type_to_string(type->data.pointer.pointee));
            snprintf(buf, sizeof(buf), "*%s", sub);
            return buf;
        }
        case TYPE_NAMED:
            snprintf(buf, sizeof(buf), "%.*s", (int)type->data.named.len, type->data.named.name);
            return buf;
    }
    return "?";
}

/* ========================================================================== */
/* Parser State                                                               */
/* ========================================================================== */

static void error_at(Parser *p, Token *tok, const char *msg) {
    if (p->panic_mode) return;
    p->panic_mode = true;
    p->had_error = true;
    fprintf(stderr, "[parse error] %zu:%zu: %s (at '%.*s')\n",
            tok->line, tok->column, msg, (int)tok->length, tok->start);
}

static void advance(Parser *p) {
    p->previous = p->current;
    p->current = lexer_next_token(&p->lexer);
    while (p->current.type == TOKEN_ERROR) {
        fprintf(stderr, "[lex error] %zu:%zu: %.*s\n",
                p->current.line, p->current.column,
                (int)p->current.length, p->current.start);
        p->current = lexer_next_token(&p->lexer);
    }
}

static bool check(Parser *p, TokenType type) { return p->current.type == type; }

static bool match(Parser *p, TokenType type) {
    if (check(p, type)) { advance(p); return true; }
    return false;
}

static Token expect(Parser *p, TokenType type, const char *msg) {
    if (check(p, type)) { advance(p); return p->previous; }
    error_at(p, &p->current, msg);
    Token t = p->current;
    t.type = TOKEN_ERROR;
    return t;
}

void parser_init(Parser *p, const char *source, size_t length) {
    memset(p, 0, sizeof(Parser));
    lexer_init(&p->lexer, source, length);
    advance(p);
}

/* ========================================================================== */
/* Type Parsing                                                               */
/* ========================================================================== */

static bool is_type_start(TokenType t) {
    return token_is_type_keyword(t) || t == TOKEN_IDENT || t == KW_STRUCT;
}

static Type *parse_type(Parser *p) {
    if (match(p, OP_MUL)) {
        Type *pointee = parse_type(p);
        return type_pointer(pointee);
    }
    if (token_is_type_keyword(p->current.type)) {
        TokenType kind = p->current.type;
        advance(p);
        return type_primitive(kind);
    }
    if (check(p, TOKEN_IDENT)) {
        Token name = p->current;
        advance(p);
        return type_named(name.start, name.length);
    }
    if (check(p, KW_STRUCT)) {
        Token name;
        advance(p);
        if (check(p, TOKEN_IDENT)) { name = p->current; advance(p); }
        else { name = p->current; name.length = 0; }
        return type_named(name.start, name.length);
    }
    error_at(p, &p->current, "expected type");
    return type_primitive(KW_VOID);
}

/* ========================================================================== */
/* Expression Parsing (Precedence Climbing)                                   */
/* ========================================================================== */

static ASTNode *parse_expression(Parser *p);

/* Binding powers */
static int infix_bp(TokenType t) {
    switch (t) {
        case OP_OR: return 10;
        case OP_AND: return 11;
        case OP_BIT_OR: return 12;
        case OP_BIT_XOR: return 13;
        case OP_BIT_AND: return 14;
        case OP_EQ: case OP_NE: return 15;
        case OP_LT: case OP_GT: case OP_LE: case OP_GE: return 16;
        case OP_SHL: case OP_SHR: return 17;
        case OP_ADD: case OP_SUB: return 18;
        case OP_MUL: case OP_DIV: case OP_MOD: return 19;
        case KW_AS: return 25;
        /* Postfix: call, index, field access bind tightest */
        case LPAREN: case LBRACKET: case OP_DOT: case OP_ARROW: return 30;
        default: return -1;
    }
}

static int prefix_bp(TokenType t) {
    switch (t) {
        case OP_SUB: case OP_NOT: case OP_BIT_NOT:
        case OP_MUL: case OP_BIT_AND: /* prefix * is deref, prefix & is address-of */
            return 22;
        default: return -1;
    }
}

static ASTNode *parse_prefix(Parser *p);
static ASTNode *parse_infix(Parser *p, ASTNode *left, int min_bp);

static ASTNode *parse_expr_bp(Parser *p, int min_bp) {
    ASTNode *left = parse_prefix(p);
    while (true) {
        int bp = infix_bp(p->current.type);
        if (bp < 0 || bp < min_bp) break;
        left = parse_infix(p, left, bp);
    }
    return left;
}

static ASTNode *parse_prefix(Parser *p) {
    Token tok = p->current;

    /* Prefix operators */
    if (prefix_bp(tok.type) >= 0) {
        advance(p);
        ASTNode *operand = parse_expr_bp(p, prefix_bp(tok.type));
        ASTNode *n = node_alloc(AST_UNARY, tok);
        /* Context-sensitive operators: prefix * = deref, prefix & = address-of.
           (In infix position they stay OP_MUL / OP_BIT_AND.) */
        if (tok.type == OP_BIT_AND) n->data.unary.op = OP_ADDR_OF;
        else if (tok.type == OP_MUL) n->data.unary.op = OP_DEREF;
        else n->data.unary.op = tok.type;
        n->data.unary.operand = operand;
        return n;
    }

    /* Literals */
    if (match(p, TOKEN_INT_LIT)) {
        ASTNode *n = node_alloc(AST_INT_LIT, p->previous);
        n->data.int_lit.value = p->previous.value.int_val;
        return n;
    }
    if (match(p, TOKEN_FLOAT_LIT)) {
        ASTNode *n = node_alloc(AST_FLOAT_LIT, p->previous);
        n->data.float_lit.value = p->previous.value.float_val;
        return n;
    }
    if (match(p, TOKEN_STRING_LIT)) {
        ASTNode *n = node_alloc(AST_STRING_LIT, p->previous);
        n->data.string_lit.value = p->previous.start;
        n->data.string_lit.length = p->previous.length;
        return n;
    }
    if (match(p, TOKEN_CHAR_LIT)) {
        ASTNode *n = node_alloc(AST_CHAR_LIT, p->previous);
        n->data.char_lit.value = p->previous.start[1];
        if (p->previous.start[1] == '\\') {
            char esc = p->previous.start[2];
            switch (esc) {
                case 'n': n->data.char_lit.value = '\n'; break;
                case 't': n->data.char_lit.value = '\t'; break;
                case 'r': n->data.char_lit.value = '\r'; break;
                case '0': n->data.char_lit.value = '\0'; break;
                case '\\': n->data.char_lit.value = '\\'; break;
                case '\'': n->data.char_lit.value = '\''; break;
                default: n->data.char_lit.value = esc; break;
            }
        }
        return n;
    }
    if (match(p, KW_TRUE) || match(p, KW_FALSE)) {
        ASTNode *n = node_alloc(AST_BOOL_LIT, p->previous);
        n->data.bool_lit.value = (p->previous.type == KW_TRUE);
        return n;
    }

    /* sizeof / alignof */
    if (match(p, KW_SIZEOF) || match(p, KW_ALIGNOF)) {
        Token kw = p->previous;
        expect(p, LPAREN, "expected '(' after sizeof");
        Type *t = parse_type(p);
        expect(p, RPAREN, "expected ')'");
        ASTNode *n = node_alloc(
            kw.type == KW_SIZEOF ? AST_SIZEOF : AST_ALIGNOF, kw);
        n->data.sizeof_alignof.target_type = t;
        return n;
    }

    /* Parenthesized expression */
    if (match(p, LPAREN)) {
        ASTNode *e = parse_expression(p);
        expect(p, RPAREN, "expected ')'");
        return e;
    }

    /* Identifier */
    if (check(p, TOKEN_IDENT)) {
        Token name = p->current;
        advance(p);
        ASTNode *n = node_alloc(AST_VAR_REF, name);
        n->data.var_ref.name = name;
        return n;
    }

    error_at(p, &p->current, "expected expression");
    advance(p);
    return node_alloc(AST_INT_LIT, tok);
}

static ASTNode *parse_infix(Parser *p, ASTNode *left, int min_bp) {
    Token op_tok = p->current;
    advance(p);

    /* Cast: expr as Type */
    if (op_tok.type == KW_AS) {
        Type *t = parse_type(p);
        ASTNode *n = node_alloc(AST_CAST, op_tok);
        n->data.cast.expr = left;
        n->data.cast.target_type = t;
        return n;
    }

    /* Call: expr(args) */
    if (op_tok.type == LPAREN) {
        ASTNodeList *args = list_new();
        if (!check(p, RPAREN)) {
            do {
                list_push(args, parse_expression(p));
            } while (match(p, COMMA));
        }
        expect(p, RPAREN, "expected ')' after arguments");
        ASTNode *n = node_alloc(AST_CALL, op_tok);
        n->data.call.callee = left;
        n->data.call.args = args;
        return n;
    }

    /* Index: expr[index] */
    if (op_tok.type == LBRACKET) {
        ASTNode *idx = parse_expression(p);
        expect(p, RBRACKET, "expected ']' after index");
        ASTNode *n = node_alloc(AST_INDEX, op_tok);
        n->data.index.base = left;
        n->data.index.index = idx;
        return n;
    }

    /* Field: expr.name or expr->name (arrow = pointer deref in C) */
    if (op_tok.type == OP_DOT || op_tok.type == OP_ARROW) {
        Token field = expect(p, TOKEN_IDENT, "expected field name after '.'");
        ASTNode *n = node_alloc(AST_FIELD, op_tok);
        n->data.field.base = left;
        n->data.field.field_name = field;
        /* Remember which operator was used: token.type = OP_DOT or OP_ARROW */
        n->token.type = op_tok.type;
        return n;
    }

    /* Binary operator - right associativity for assignment ops */
    int right_bp = min_bp;
    if (op_tok.type == OP_ASSIGN) right_bp = min_bp;
    ASTNode *right = parse_expr_bp(p, right_bp);
    ASTNode *n = node_alloc(AST_BINARY, op_tok);
    n->data.binary.op = op_tok.type;
    n->data.binary.left = left;
    n->data.binary.right = right;
    return n;
}

static ASTNode *parse_expression(Parser *p) {
    return parse_expr_bp(p, 10);
}

/* Assignment is handled as expression statement target */
static ASTNode *parse_assignment(Parser *p) {
    ASTNode *left = parse_expression(p);
    if (check(p, OP_ASSIGN)) {
        Token op = p->current;
        advance(p);
        ASTNode *right = parse_assignment(p);
        ASTNode *n = node_alloc(AST_BINARY, op);
        n->data.binary.op = OP_ASSIGN;
        n->data.binary.left = left;
        n->data.binary.right = right;
        return n;
    }
    /* Compound assignment */
    TokenType ct = p->current.type;
    if (ct == OP_ADD_ASSIGN || ct == OP_SUB_ASSIGN || ct == OP_MUL_ASSIGN ||
        ct == OP_DIV_ASSIGN || ct == OP_MOD_ASSIGN || ct == OP_SHL_ASSIGN ||
        ct == OP_SHR_ASSIGN || ct == OP_BIT_AND_ASSIGN ||
        ct == OP_BIT_OR_ASSIGN || ct == OP_BIT_XOR_ASSIGN) {
        Token op = p->current;
        advance(p);
        ASTNode *right = parse_assignment(p);
        /* Desugar: a += b  =>  a = a + b */
        TokenType binop;
        switch (ct) {
            case OP_ADD_ASSIGN: binop = OP_ADD; break;
            case OP_SUB_ASSIGN: binop = OP_SUB; break;
            case OP_MUL_ASSIGN: binop = OP_MUL; break;
            case OP_DIV_ASSIGN: binop = OP_DIV; break;
            case OP_MOD_ASSIGN: binop = OP_MOD; break;
            case OP_SHL_ASSIGN: binop = OP_SHL; break;
            case OP_SHR_ASSIGN: binop = OP_SHR; break;
            case OP_BIT_AND_ASSIGN: binop = OP_BIT_AND; break;
            case OP_BIT_OR_ASSIGN: binop = OP_BIT_OR; break;
            default: binop = OP_BIT_XOR; break;
        }
        ASTNode *bin = node_alloc(AST_BINARY, op);
        bin->data.binary.op = binop;
        /* `left` appears twice (target of = and operand of +). The AST frees
           each node exactly once, so the second copy must be a deep clone —
           otherwise it's freed twice and the heap corrupts. */
        bin->data.binary.left = ast_clone(left);
        bin->data.binary.right = right;

        ASTNode *assign = node_alloc(AST_BINARY, op);
        assign->data.binary.op = OP_ASSIGN;
        assign->data.binary.left = left;
        assign->data.binary.right = bin;
        return assign;
    }
    return left;
}

/* ========================================================================== */
/* Statement Parsing                                                          */
/* ========================================================================== */

static ASTNode *parse_block(Parser *p) {
    Token lbrace = expect(p, LBRACE, "expected '{'");
    ASTNodeList *stmts = list_new();
    while (!check(p, RBRACE) && !check(p, TOKEN_EOF)) {
        /* We'll parse statements in the statement function */
        ASTNode *stmt = NULL;
        /* Use statement parser - forward decl handled below */
        extern ASTNode *parse_statement(Parser *p);
        stmt = parse_statement(p);
        if (stmt) list_push(stmts, stmt);
        if (p->had_error && p->panic_mode) {
            /* recovery: skip to ; or } */
            while (!check(p, SEMICOLON) && !check(p, RBRACE) && !check(p, TOKEN_EOF))
                advance(p);
            match(p, SEMICOLON);
            p->panic_mode = false;
        }
    }
    expect(p, RBRACE, "expected '}'");
    ASTNode *n = node_alloc(AST_BLOCK, lbrace);
    n->data.block.stmts = stmts;
    return n;
}

/* Parse declaration inside struct: [Type name;] */
static ASTNode *parse_field(Parser *p) {
    Type *t = parse_type(p);
    Token name = expect(p, TOKEN_IDENT, "expected field name");
    expect(p, SEMICOLON, "expected ';' after field");
    ASTNode *n = node_alloc(AST_VAR_DECL, name);
    n->data.var_decl.is_mutable = true;
    n->data.var_decl.name = name;
    n->data.var_decl.var_type = t;
    n->data.var_decl.init = NULL;
    return n;
}

/* Parse function parameter: name: Type */
static ASTNode *parse_param(Parser *p) {
    Token name = expect(p, TOKEN_IDENT, "expected parameter name");
    expect(p, COLON, "expected ':' after parameter name");
    Type *t = parse_type(p);
    ASTNode *n = node_alloc(AST_VAR_DECL, name);
    n->data.var_decl.is_mutable = true;
    n->data.var_decl.name = name;
    n->data.var_decl.var_type = t;
    n->data.var_decl.init = NULL;
    return n;
}

ASTNode *parse_statement(Parser *p) {
    Token tok = p->current;

    /* volatile prefix: skip it, it's a storage qualifier */
    if (match(p, KW_VOLATILE)) tok = p->current;

    /* Block statement */
    if (check(p, LBRACE)) return parse_block(p);

    /* Bare typed declaration: [Type] name = value; (spec 3.1) */
    if (is_type_start(p->current.type) &&
        p->current.type != KW_VAR && p->current.type != KW_CONST) {
        Token peek = lexer_peek_token(&p->lexer);
        if (peek.type == TOKEN_IDENT) {
            Type *t = parse_type(p);
            Token name = p->current;
            advance(p);
            ASTNode *init = NULL;
            expect(p, OP_ASSIGN, "expected '=' in declaration");
            init = parse_expression(p);
            expect(p, SEMICOLON, "expected ';' after declaration");
            ASTNode *n = node_alloc(AST_VAR_DECL, name);
            n->data.var_decl.is_mutable = true;
            n->data.var_decl.name = name;
            n->data.var_decl.var_type = t;
            n->data.var_decl.init = init;
            return n;
        }
    }

    /* Variable declaration: var name: Type = init; */
    if (match(p, KW_VAR)) {
        Token name = expect(p, TOKEN_IDENT, "expected variable name");
        expect(p, COLON, "expected ':' after variable name");
        Type *t = parse_type(p);
        ASTNode *init = NULL;
        if (match(p, OP_ASSIGN)) init = parse_expression(p);
        expect(p, SEMICOLON, "expected ';' after declaration");
        ASTNode *n = node_alloc(AST_VAR_DECL, tok);
        n->data.var_decl.is_mutable = true;
        n->data.var_decl.name = name;
        n->data.var_decl.var_type = t;
        n->data.var_decl.init = init;
        return n;
    }

    /* Const declaration: const [Type] NAME = value; */
    if (match(p, KW_CONST)) {
        Type *t = NULL;
        Token name;
        if (token_is_type_keyword(p->current.type)) {
            t = parse_type(p);
            name = expect(p, TOKEN_IDENT, "expected constant name");
        } else {
            name = expect(p, TOKEN_IDENT, "expected constant name");
            if (match(p, COLON)) t = parse_type(p);
        }
        expect(p, OP_ASSIGN, "expected '=' in const declaration");
        ASTNode *init = parse_expression(p);
        expect(p, SEMICOLON, "expected ';' after declaration");
        ASTNode *n = node_alloc(AST_CONST_DECL, tok);
        n->data.var_decl.is_mutable = false;
        n->data.var_decl.name = name;
        n->data.var_decl.var_type = t;
        n->data.var_decl.init = init;
        return n;
    }

    /* If statement */
    if (match(p, KW_IF)) {
        expect(p, LPAREN, "expected '(' after if");
        ASTNode *cond = parse_expression(p);
        expect(p, RPAREN, "expected ')' after condition");
        ASTNode *then_b = parse_block(p);
        ASTNode *else_b = NULL;
        if (match(p, KW_ELSE)) {
            if (check(p, KW_IF)) else_b = parse_statement(p);
            else else_b = parse_block(p);
        }
        ASTNode *n = node_alloc(AST_IF_STMT, tok);
        n->data.if_stmt.cond = cond;
        n->data.if_stmt.then_branch = then_b;
        n->data.if_stmt.else_branch = else_b;
        return n;
    }

    /* While statement */
    if (match(p, KW_WHILE)) {
        expect(p, LPAREN, "expected '(' after while");
        ASTNode *cond = parse_expression(p);
        expect(p, RPAREN, "expected ')' after condition");
        ASTNode *body = parse_block(p);
        ASTNode *n = node_alloc(AST_WHILE_STMT, tok);
        n->data.while_stmt.cond = cond;
        n->data.while_stmt.body = body;
        return n;
    }

    /* For statement */
    if (match(p, KW_FOR)) {
        expect(p, LPAREN, "expected '(' after for");
        ASTNode *init = NULL;
        if (!check(p, SEMICOLON)) {
            if (match(p, KW_VAR)) {
                /* inline var decl without trailing semicolon here */
                Token name = expect(p, TOKEN_IDENT, "expected variable name");
                expect(p, COLON, "expected ':'");
                Type *t = parse_type(p);
                expect(p, OP_ASSIGN, "expected '='");
                ASTNode *iv = parse_expression(p);
                ASTNode *vn = node_alloc(AST_VAR_DECL, name);
                vn->data.var_decl.is_mutable = true;
                vn->data.var_decl.name = name;
                vn->data.var_decl.var_type = t;
                vn->data.var_decl.init = iv;
                init = vn;
            } else {
                ASTNode *e = parse_expression(p);
                ASTNode *es = node_alloc(AST_EXPR_STMT, e->token);
                es->data.expr_stmt.expr = e;
                init = es;
            }
        }
        expect(p, SEMICOLON, "expected ';' in for header");
        ASTNode *cond = NULL;
        if (!check(p, SEMICOLON)) cond = parse_expression(p);
        expect(p, SEMICOLON, "expected ';' in for header");
        ASTNode *step = NULL;
        if (!check(p, RPAREN)) step = parse_assignment(p);
        expect(p, RPAREN, "expected ')' after for header");
        ASTNode *body = parse_block(p);
        ASTNode *n = node_alloc(AST_FOR_STMT, tok);
        n->data.for_stmt.init = init;
        n->data.for_stmt.cond = cond;
        n->data.for_stmt.step = step;
        n->data.for_stmt.body = body;
        return n;
    }

    /* Switch statement */
    if (match(p, KW_SWITCH)) {
        expect(p, LPAREN, "expected '(' after switch");
        ASTNode *expr = parse_expression(p);
        expect(p, RPAREN, "expected ')' after switch expression");
        expect(p, LBRACE, "expected '{'");
        ASTNodeList *cases = list_new();
        while (check(p, KW_CASE) || check(p, KW_ELSE)) {
            Token ctok = p->current;
            bool is_default = false;
            if (match(p, KW_ELSE)) is_default = true;
            else advance(p); /* consume 'case' */
            ASTNode *cexpr = NULL;
            if (!is_default) {
                if (match(p, KW_CASE)) is_default = true; /* tolerate 'case else' style */
                else cexpr = parse_expression(p);
            }
            expect(p, COLON, "expected ':' after case value");
            /* parse statements until next case or } */
            ASTNodeList *body = list_new();
            while (!check(p, KW_CASE) && !check(p, KW_ELSE) && !check(p, RBRACE)
                   && !check(p, TOKEN_EOF)) {
                ASTNode *s = parse_statement(p);
                if (s) list_push(body, s);
            }
            ASTNode *cs = node_alloc(AST_CASE_STMT, ctok);
            cs->data.case_stmt.expr = cexpr;
            ASTNode *blk = node_alloc(AST_BLOCK, ctok);
            blk->data.block.stmts = body;
            cs->data.case_stmt.body = blk;
            list_push(cases, cs);
        }
        expect(p, RBRACE, "expected '}' after switch");
        ASTNode *n = node_alloc(AST_SWITCH_STMT, tok);
        n->data.switch_stmt.expr = expr;
        n->data.switch_stmt.cases = cases;
        return n;
    }

    /* Return statement */
    if (match(p, KW_RETURN)) {
        ASTNode *val = NULL;
        if (!check(p, SEMICOLON)) val = parse_expression(p);
        expect(p, SEMICOLON, "expected ';' after return");
        ASTNode *n = node_alloc(AST_RETURN_STMT, tok);
        n->data.return_stmt.value = val;
        return n;
    }

    /* Break / Continue */
    if (match(p, KW_BREAK) || match(p, KW_CONTINUE)) {
        bool is_break = (p->previous.type == KW_BREAK);
        expect(p, SEMICOLON, "expected ';'");
        return node_alloc(is_break ? AST_BREAK_STMT : AST_CONTINUE_STMT, tok);
    }

    /* Defer statement */
    if (match(p, KW_DEFER)) {
        ASTNode *body;
        if (check(p, LBRACE)) body = parse_block(p);
        else {
            ASTNode *s = parse_statement(p);
            body = s;
        }
        ASTNode *n = node_alloc(AST_DEFER_STMT, tok);
        n->data.defer_stmt.body = body;
        return n;
    }

    /* Expression statement */
    ASTNode *e = parse_assignment(p);
    expect(p, SEMICOLON, "expected ';' after expression");
    ASTNode *n = node_alloc(AST_EXPR_STMT, tok);
    n->data.expr_stmt.expr = e;
    return n;
}

/* ========================================================================== */
/* Top-Level Declarations                                                     */
/* ========================================================================== */

static ASTNode *parse_fn_decl(Parser *p, bool is_pub, bool is_extern) {
    Token kw = expect(p, KW_FN, "expected 'fn'");
    Token name = expect(p, TOKEN_IDENT, "expected function name");

    expect(p, LPAREN, "expected '(' after function name");
    ASTNodeList *params = list_new();
    if (!check(p, RPAREN)) {
        do { list_push(params, parse_param(p)); } while (match(p, COMMA));
    }
    expect(p, RPAREN, "expected ')' after parameters");

    Type *ret = NULL;
    if (is_type_start(p->current.type)) ret = parse_type(p);

    ASTNode *body = NULL;
    if (is_extern || check(p, SEMICOLON)) {
        expect(p, SEMICOLON, "expected ';' after extern fn");
    } else {
        body = parse_block(p);
    }

    ASTNode *n = node_alloc(AST_FN_DECL, kw);
    n->data.fn_decl.is_pub = is_pub;
    n->data.fn_decl.is_extern = is_extern;
    n->data.fn_decl.name = name;
    n->data.fn_decl.params = params;
    n->data.fn_decl.ret_type = ret;
    n->data.fn_decl.body = body;
    return n;
}

static ASTNode *parse_struct_decl(Parser *p, bool is_pub, bool is_union) {
    Token kw = is_union ? expect(p, KW_UNION, "expected 'union'")
                        : expect(p, KW_STRUCT, "expected 'struct'");
    Token name = expect(p, TOKEN_IDENT, "expected struct name");
    expect(p, LBRACE, "expected '{'");
    ASTNodeList *fields = list_new();
    while (!check(p, RBRACE) && !check(p, TOKEN_EOF)) {
        list_push(fields, parse_field(p));
    }
    expect(p, RBRACE, "expected '}'");
    ASTNode *n = node_alloc(is_union ? AST_UNION_DECL : AST_STRUCT_DECL, kw);
    n->data.aggregate_decl.is_pub = is_pub;
    n->data.aggregate_decl.is_union = is_union;
    n->data.aggregate_decl.name = name;
    n->data.aggregate_decl.fields = fields;
    return n;
}

static ASTNode *parse_enum_decl(Parser *p, bool is_pub) {
    Token kw = expect(p, KW_ENUM, "expected 'enum'");
    Token name = expect(p, TOKEN_IDENT, "expected enum name");
    expect(p, LBRACE, "expected '{'");
    ASTNodeList *variants = list_new();
    while (!check(p, RBRACE) && !check(p, TOKEN_EOF)) {
        Token vname = expect(p, TOKEN_IDENT, "expected variant name");
        ASTNode *v = node_alloc(AST_VAR_REF, vname);
        v->data.var_ref.name = vname;
        /* optional = value */
        if (match(p, OP_ASSIGN)) {
            ASTNode *val = parse_expression(p);
            /* Wrap as: variant = value */
            ASTNode *w = node_alloc(AST_BINARY, vname);
            w->data.binary.op = OP_ASSIGN;
            w->data.binary.left = v;
            w->data.binary.right = val;
            list_push(variants, w);
        } else {
            list_push(variants, v);
        }
        if (!match(p, COMMA)) break;
    }
    expect(p, RBRACE, "expected '}'");
    ASTNode *n = node_alloc(AST_ENUM_DECL, kw);
    n->data.enum_decl.is_pub = is_pub;
    n->data.enum_decl.name = name;
    n->data.enum_decl.variants = variants;
    return n;
}

static ASTNode *parse_use_decl(Parser *p) {
    Token kw = expect(p, KW_USE, "expected 'use'");
    /* Build path: std.io -> single token spanning full path */
    Token first = expect(p, TOKEN_IDENT, "expected module name");
    Token path = first;
    while (check(p, OP_DOT)) {
        advance(p); /* consume dot */
        Token part = expect(p, TOKEN_IDENT, "expected module name after '.'");
        path.length = (part.start - path.start) + part.length;
    }
    expect(p, SEMICOLON, "expected ';' after use");
    ASTNode *n = node_alloc(AST_USE_DECL, kw);
    n->data.use_decl.path = path;
    return n;
}

static ASTNode *parse_declaration(Parser *p) {
    bool is_pub = false;
    bool is_extern = false;

    if (match(p, KW_PUB)) is_pub = true;
    if (match(p, KW_EXTERN)) is_extern = true;
    if (check(p, KW_PUB)) { advance(p); is_pub = true; }

    switch (p->current.type) {
        case KW_FN:
            return parse_fn_decl(p, is_pub, is_extern);
        case KW_STRUCT:
            if (is_extern) {
                error_at(p, &p->current, "extern struct not supported");
                advance(p);
                return NULL;
            }
            return parse_struct_decl(p, is_pub, false);
        case KW_UNION:
            return parse_struct_decl(p, is_pub, true);
        case KW_ENUM:
            return parse_enum_decl(p, is_pub);
        case KW_USE:
            return parse_use_decl(p);
        case KW_TYPE: {
            Token kw; advance(p);
            kw = p->previous;
            Token name = expect(p, TOKEN_IDENT, "expected type name");
            expect(p, OP_ASSIGN, "expected '=' in type alias");
            Type *t = parse_type(p);
            expect(p, SEMICOLON, "expected ';' after type alias");
            ASTNode *n = node_alloc(AST_TYPE_ALIAS, kw);
            n->data.type_alias.is_pub = is_pub;
            n->data.type_alias.name = name;
            n->data.type_alias.aliased_type = t;
            return n;
        }
        case KW_CONST: {
            /* top-level const: [pub] const [Type] NAME = value; */
            Token kw; advance(p);
            kw = p->previous;
            Type *t = NULL;
            Token name;
            if (token_is_type_keyword(p->current.type)) {
                t = parse_type(p);
                name = expect(p, TOKEN_IDENT, "expected constant name");
            } else {
                name = expect(p, TOKEN_IDENT, "expected constant name");
                if (match(p, COLON)) t = parse_type(p);
            }
            expect(p, OP_ASSIGN, "expected '='");
            ASTNode *init = parse_expression(p);
            expect(p, SEMICOLON, "expected ';' after const");
            ASTNode *n = node_alloc(AST_CONST_DECL, kw);
            n->data.var_decl.is_mutable = false;
            n->data.var_decl.name = name;
            n->data.var_decl.var_type = t;
            n->data.var_decl.init = init;
            return n;
        }
        case KW_VAR:
        case KW_VOLATILE: {
            /* top-level global: [pub] [volatile] var name: Type = init; */
            bool volatile_ = false;
            if (match(p, KW_VOLATILE)) volatile_ = true;
            Token kw = p->current;
            expect(p, KW_VAR, "expected 'var'");
            Token name = expect(p, TOKEN_IDENT, "expected variable name");
            expect(p, COLON, "expected ':' after variable name");
            Type *t = parse_type(p);
            expect(p, OP_ASSIGN, "expected '=' in declaration");
            ASTNode *init = parse_expression(p);
            expect(p, SEMICOLON, "expected ';' after declaration");
            ASTNode *n = node_alloc(AST_VAR_DECL, kw);
            n->data.var_decl.is_mutable = true;
            n->data.var_decl.name = name;
            n->data.var_decl.var_type = t;
            n->data.var_decl.init = init;
            (void)volatile_;
            return n;
        }
        default: {
            /* Bare typed top-level declaration: Type NAME = value; */
            if (is_type_start(p->current.type)) {
                Type *t = parse_type(p);
                Token name = expect(p, TOKEN_IDENT, "expected name");
                expect(p, OP_ASSIGN, "expected '='");
                ASTNode *init = parse_expression(p);
                expect(p, SEMICOLON, "expected ';' after declaration");
                ASTNode *n = node_alloc(AST_VAR_DECL, name);
                n->data.var_decl.is_mutable = true;
                n->data.var_decl.name = name;
                n->data.var_decl.var_type = t;
                n->data.var_decl.init = init;
                return n;
            }
            error_at(p, &p->current, "expected declaration");
            advance(p);
            return NULL;
        }
    }
}

ASTNodeList *parser_parse(Parser *p) {
    ASTNodeList *list = list_new();
    while (!check(p, TOKEN_EOF)) {
        ASTNode *decl = parse_declaration(p);
        if (decl) list_push(list, decl);
        if (p->panic_mode) {
            while (!check(p, TOKEN_EOF)) advance(p);
            break;
        }
    }
    return list;
}

/* ========================================================================== */
/* Debug Printing                                                             */
/* ========================================================================== */

static void indent_print(int n) { for (int i = 0; i < n; i++) printf("  "); }

void ast_print(const ASTNode *n, int ind) {
    if (!n) return;
    indent_print(ind);
    switch (n->type) {
        case AST_FN_DECL:
            printf("FnDecl%s %.*s(", n->data.fn_decl.is_pub ? " [pub]" : "",
                   (int)n->data.fn_decl.name.length, n->data.fn_decl.name.start);
            for (size_t i = 0; i < n->data.fn_decl.params->count; i++) {
                ASTNode *p = n->data.fn_decl.params->items[i];
                if (i) printf(", ");
                printf("%.*s: %s", (int)p->data.var_decl.name.length,
                       p->data.var_decl.name.start,
                       type_to_string(p->data.var_decl.var_type));
            }
            printf(") %s\n", n->data.fn_decl.ret_type ? type_to_string(n->data.fn_decl.ret_type) : "void");
            if (n->data.fn_decl.body) ast_print(n->data.fn_decl.body, ind + 1);
            break;
        case AST_VAR_DECL:
            printf("%s %.*s: %s",
                   n->data.var_decl.is_mutable ? "Var" : "Const",
                   (int)n->data.var_decl.name.length, n->data.var_decl.name.start,
                   n->data.var_decl.var_type ? type_to_string(n->data.var_decl.var_type) : "?");
            if (n->data.var_decl.init) { printf(" =\n"); ast_print(n->data.var_decl.init, ind + 1); }
            else printf("\n");
            break;
        case AST_CONST_DECL:
            printf("Const %.*s =\n", (int)n->data.var_decl.name.length, n->data.var_decl.name.start);
            if (n->data.var_decl.init) ast_print(n->data.var_decl.init, ind + 1);
            break;
        case AST_STRUCT_DECL: case AST_UNION_DECL:
            printf("%s %.*s {\n", n->type == AST_UNION_DECL ? "Union" : "Struct",
                   (int)n->data.aggregate_decl.name.length, n->data.aggregate_decl.name.start);
            for (size_t i = 0; i < n->data.aggregate_decl.fields->count; i++)
                ast_print(n->data.aggregate_decl.fields->items[i], ind + 1);
            indent_print(ind); printf("}\n");
            break;
        case AST_ENUM_DECL:
            printf("Enum %.*s {\n", (int)n->data.enum_decl.name.length, n->data.enum_decl.name.start);
            for (size_t i = 0; i < n->data.enum_decl.variants->count; i++) {
                ASTNode *v = n->data.enum_decl.variants->items[i];
                indent_print(ind + 1);
                if (v->type == AST_BINARY)
                    printf("%.*s = ...\n", (int)v->data.binary.left->data.var_ref.name.length,
                           v->data.binary.left->data.var_ref.name.start);
                else
                    printf("%.*s\n", (int)v->data.var_ref.name.length, v->data.var_ref.name.start);
            }
            indent_print(ind); printf("}\n");
            break;
        case AST_USE_DECL:
            printf("Use %.*s\n", (int)n->data.use_decl.path.length, n->data.use_decl.path.start);
            break;
        case AST_TYPE_ALIAS:
            printf("TypeAlias %.*s = %s\n", (int)n->data.type_alias.name.length,
                   n->data.type_alias.name.start,
                   type_to_string(n->data.type_alias.aliased_type));
            break;
        case AST_BLOCK:
            printf("Block {\n");
            for (size_t i = 0; i < n->data.block.stmts->count; i++)
                ast_print(n->data.block.stmts->items[i], ind + 1);
            indent_print(ind); printf("}\n");
            break;
        case AST_IF_STMT:
            printf("If\n");
            indent_print(ind+1); printf("cond:\n");
            ast_print(n->data.if_stmt.cond, ind + 2);
            indent_print(ind+1); printf("then:\n");
            ast_print(n->data.if_stmt.then_branch, ind + 2);
            if (n->data.if_stmt.else_branch) {
                indent_print(ind+1); printf("else:\n");
                ast_print(n->data.if_stmt.else_branch, ind + 2);
            }
            break;
        case AST_FOR_STMT:
            printf("For\n");
            if (n->data.for_stmt.init) { indent_print(ind+1); printf("init:\n"); ast_print(n->data.for_stmt.init, ind+2); }
            if (n->data.for_stmt.cond) { indent_print(ind+1); printf("cond:\n"); ast_print(n->data.for_stmt.cond, ind+2); }
            if (n->data.for_stmt.step) { indent_print(ind+1); printf("step:\n"); ast_print(n->data.for_stmt.step, ind+2); }
            ast_print(n->data.for_stmt.body, ind + 1);
            break;
        case AST_WHILE_STMT:
            printf("While\n");
            ast_print(n->data.while_stmt.cond, ind + 1);
            ast_print(n->data.while_stmt.body, ind + 1);
            break;
        case AST_SWITCH_STMT:
            printf("Switch\n");
            ast_print(n->data.switch_stmt.expr, ind + 1);
            for (size_t i = 0; i < n->data.switch_stmt.cases->count; i++)
                ast_print(n->data.switch_stmt.cases->items[i], ind + 1);
            break;
        case AST_CASE_STMT:
            printf("Case");
            if (!n->data.case_stmt.expr) printf(" [default]");
            printf("\n");
            if (n->data.case_stmt.expr) ast_print(n->data.case_stmt.expr, ind + 1);
            ast_print(n->data.case_stmt.body, ind + 1);
            break;
        case AST_RETURN_STMT:
            printf("Return\n");
            if (n->data.return_stmt.value) ast_print(n->data.return_stmt.value, ind + 1);
            break;
        case AST_BREAK_STMT: printf("Break\n"); break;
        case AST_CONTINUE_STMT: printf("Continue\n"); break;
        case AST_DEFER_STMT:
            printf("Defer\n");
            ast_print(n->data.defer_stmt.body, ind + 1);
            break;
        case AST_EXPR_STMT:
            printf("ExprStmt\n");
            ast_print(n->data.expr_stmt.expr, ind + 1);
            break;
        case AST_BINARY:
            printf("Binary[%s]\n", token_type_name(n->data.binary.op));
            ast_print(n->data.binary.left, ind + 1);
            ast_print(n->data.binary.right, ind + 1);
            break;
        case AST_UNARY:
            printf("Unary[%s]\n", token_type_name(n->data.unary.op));
            ast_print(n->data.unary.operand, ind + 1);
            break;
        case AST_CALL:
            printf("Call\n");
            ast_print(n->data.call.callee, ind + 1);
            for (size_t i = 0; i < n->data.call.args->count; i++)
                ast_print(n->data.call.args->items[i], ind + 1);
            break;
        case AST_INDEX:
            printf("Index\n");
            ast_print(n->data.index.base, ind + 1);
            ast_print(n->data.index.index, ind + 1);
            break;
        case AST_FIELD:
            printf("Field .%.*s\n", (int)n->data.field.field_name.length,
                   n->data.field.field_name.start);
            ast_print(n->data.field.base, ind + 1);
            break;
        case AST_VAR_REF:
            printf("Var %.*s\n", (int)n->data.var_ref.name.length, n->data.var_ref.name.start);
            break;
        case AST_INT_LIT: printf("Int %lld\n", n->data.int_lit.value); break;
        case AST_FLOAT_LIT: printf("Float %g\n", n->data.float_lit.value); break;
        case AST_STRING_LIT: printf("Str \"%.*s\"\n", (int)n->data.string_lit.length, n->data.string_lit.value); break;
        case AST_CHAR_LIT: printf("Char '%c'\n", n->data.char_lit.value); break;
        case AST_BOOL_LIT: printf("Bool %s\n", n->data.bool_lit.value ? "true" : "false"); break;
        case AST_CAST:
            printf("Cast as %s\n", type_to_string(n->data.cast.target_type));
            ast_print(n->data.cast.expr, ind + 1);
            break;
        case AST_SIZEOF: printf("Sizeof(%s)\n", type_to_string(n->data.sizeof_alignof.target_type)); break;
        case AST_ALIGNOF: printf("Alignof(%s)\n", type_to_string(n->data.sizeof_alignof.target_type)); break;
        default: printf("Unknown node\n");
    }
}

void ast_list_print(const ASTNodeList *list) {
    for (size_t i = 0; i < list->count; i++)
        ast_print(list->items[i], 0);
}