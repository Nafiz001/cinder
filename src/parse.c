// The parser: tokens in, a typed AST out.
//
// Recursive descent, one function per grammar production, named after the
// production. The C grammar is ambiguous in exactly one place that matters --
// `(x)` is a cast if x names a type and a parenthesised expression otherwise
// -- and that is resolved by looking the identifier up in scope, which is why
// the parser and the symbol table cannot be separated in C the way they can in
// most languages.
//
// Two scopes are kept, not one. C has separate namespaces for ordinary
// identifiers and for struct/union/enum tags, which is what lets
// `struct stat stat;` compile.

#include "cinder.h"

// A local scope. Blocks push and pop these; the outermost one is file scope.
typedef struct Scope Scope;
struct Scope {
  Scope *next;
  HashMap vars; // VarScope: variables, typedefs and enum constants
  HashMap tags; // Type: struct, union and enum tags
};

// What an ordinary identifier can denote. Exactly one of these is set.
typedef struct {
  Obj *var;
  Type *type_def;
  Type *enum_ty;
  int enum_val;
} VarScope;

// Storage class and alignment, gathered while reading a declaration specifier.
typedef struct {
  bool is_typedef;
  bool is_static;
  bool is_extern;
  bool is_inline;
  int align;
} VarAttr;

// An initialiser is a tree, mirroring the shape of the type being initialised.
// Building the tree first and generating assignments from it afterwards is
// what makes partial and nested initialisers tractable: `int a[3][2] = {{1},
// 2}` has to be understood completely before any of it can be emitted.
typedef struct Initializer Initializer;
struct Initializer {
  Initializer *next;
  Type *ty;
  Token *tok;
  bool is_flexible;

  // leaf
  Node *expr;

  // aggregate
  Initializer **children;

  // union: which member the initialiser picked
  Member *mem;
};

// A designator names where in an object an initialiser lands. The list is
// built from the outside in, so `a[3].b[2]` is a chain of three of these.
typedef struct InitDesg InitDesg;
struct InitDesg {
  InitDesg *next;
  int idx;
  Member *member;
  Obj *var;
};

// All local variables of the function being parsed, and all globals.
static Obj *locals;
static Obj *globals;

static Scope *scope = &(Scope){0};

// The function being parsed, so that `return` knows the return type and local
// variables know where to attach.
static Obj *current_fn;

// Labels and gotos are collected per function and resolved at the end, because
// a goto may precede its label.
static Node *gotos;
static Node *labels;

// The innermost enclosing loop or switch, for break and continue.
static char *brk_label;
static char *cont_label;

// The switch being parsed, so that `case` can attach itself.
static Node *current_switch;

static bool is_typename(Token *tok);
static void skip_attributes(Token **rest, Token *tok);
static Type *declspec(Token **rest, Token *tok, VarAttr *attr);
static Type *typename(Token **rest, Token *tok);
static Type *enum_specifier(Token **rest, Token *tok);
static Type *type_suffix(Token **rest, Token *tok, Type *ty);
static Type *declarator(Token **rest, Token *tok, Type *ty);
static Node *declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr);
static void initializer2(Token **rest, Token *tok, Initializer *init);
static Initializer *initializer(Token **rest, Token *tok, Type *ty,
                                Type **new_ty);
static Node *lvar_initializer(Token **rest, Token *tok, Obj *var);
static void gvar_initializer(Token **rest, Token *tok, Obj *var);
static Node *compound_stmt(Token **rest, Token *tok);
static Node *stmt(Token **rest, Token *tok);
static Node *expr_stmt(Token **rest, Token *tok);
static Node *expr(Token **rest, Token *tok);
static int64_t eval(Node *node);
static int64_t eval2(Node *node, char ***label);
static double eval_double(Node *node);
static Node *assign(Token **rest, Token *tok);
static Node *conditional(Token **rest, Token *tok);
static Node *logor(Token **rest, Token *tok);
static Node *logand(Token **rest, Token *tok);
static Node *bitor_(Token **rest, Token *tok);
static Node *bitxor_(Token **rest, Token *tok);
static Node *bitand_(Token **rest, Token *tok);
static Node *equality(Token **rest, Token *tok);
static Node *relational(Token **rest, Token *tok);
static Node *shift(Token **rest, Token *tok);
static Node *add(Token **rest, Token *tok);
static Node *new_add(Node *lhs, Node *rhs, Token *tok);
static Node *new_sub(Node *lhs, Node *rhs, Token *tok);
static Node *mul(Token **rest, Token *tok);
static Node *cast(Token **rest, Token *tok);
static Type *struct_decl(Token **rest, Token *tok);
static Type *union_decl(Token **rest, Token *tok);
static Node *postfix(Token **rest, Token *tok);
static Node *unary(Token **rest, Token *tok);
static Node *primary(Token **rest, Token *tok);
static Token *parse_typedef(Token *tok, Type *basety);
static bool is_function(Token *tok);
static Token *function(Token *tok, Type *basety, VarAttr *attr);
static Token *global_variable(Token *tok, Type *basety, VarAttr *attr);

//
// Scopes
//

static void enter_scope(void) {
  Scope *sc = xcalloc(1, sizeof(Scope));
  sc->next = scope;
  scope = sc;
}

static void leave_scope(void) { scope = scope->next; }

// Look an identifier up, innermost scope first.
static VarScope *find_var(Token *tok) {
  for (Scope *sc = scope; sc; sc = sc->next) {
    VarScope *sc2 = hashmap_get2(&sc->vars, tok->loc, tok->len);
    if (sc2)
      return sc2;
  }
  return NULL;
}

static Type *find_tag(Token *tok) {
  for (Scope *sc = scope; sc; sc = sc->next) {
    Type *ty = hashmap_get2(&sc->tags, tok->loc, tok->len);
    if (ty)
      return ty;
  }
  return NULL;
}

static VarScope *push_scope(char *name) {
  VarScope *sc = xcalloc(1, sizeof(VarScope));
  hashmap_put(&scope->vars, name, sc);
  return sc;
}

static void push_tag_scope(Token *tok, Type *ty) {
  hashmap_put2(&scope->tags, tok->loc, tok->len, ty);
}

//
// Node and object constructors
//

static Node *new_node(NodeKind kind, Token *tok) {
  Node *node = xcalloc(1, sizeof(Node));
  node->kind = kind;
  node->tok = tok;
  return node;
}

static Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  Node *node = new_node(kind, tok);
  node->lhs = lhs;
  node->rhs = rhs;
  return node;
}

static Node *new_unary(NodeKind kind, Node *expr, Token *tok) {
  Node *node = new_node(kind, tok);
  node->lhs = expr;
  return node;
}

static Node *new_num(int64_t val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->val = val;
  return node;
}

static Node *new_long(int64_t val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->val = val;
  node->ty = ty_llong;
  return node;
}

static Node *new_ulong(long val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->val = val;
  node->ty = ty_ullong;
  return node;
}

static Node *new_var_node(Obj *var, Token *tok) {
  Node *node = new_node(ND_VAR, tok);
  node->var = var;
  return node;
}

Node *new_cast(Node *expr, Type *ty) {
  add_type(expr);

  Node *node = xcalloc(1, sizeof(Node));
  node->kind = ND_CAST;
  node->tok = expr->tok;
  node->lhs = expr;
  node->ty = copy_type(ty);
  return node;
}

static Obj *new_var(char *name, Type *ty) {
  Obj *var = xcalloc(1, sizeof(Obj));
  var->name = name;
  var->ty = ty;
  var->align = ty->align;
  push_scope(name)->var = var;
  return var;
}

static Obj *new_lvar(char *name, Type *ty) {
  Obj *var = new_var(name, ty);
  var->is_local = true;
  var->next = locals;
  locals = var;
  return var;
}

static Obj *new_gvar(char *name, Type *ty) {
  Obj *var = new_var(name, ty);
  var->next = globals;
  var->is_static = true;
  var->is_definition = true;
  globals = var;
  return var;
}

// Anonymous globals hold string literals and compound literals. The name has
// to be unique and cannot collide with a user identifier, which `.L..n` cannot
// because a C identifier may not contain a dot.
static char *new_unique_name(void) {
  static int id = 0;
  return format(".L..%d", id++);
}

static Obj *new_anon_gvar(Type *ty) { return new_gvar(new_unique_name(), ty); }

static Obj *new_string_literal(char *p, Type *ty) {
  Obj *var = new_anon_gvar(ty);
  var->init_data = p;
  return var;
}

static char *get_ident(Token *tok) {
  if (tok->kind != TK_IDENT)
    error_tok(tok, "expected an identifier");
  return xstrndup(tok->loc, tok->len);
}

static Type *find_typedef(Token *tok) {
  if (tok->kind == TK_IDENT) {
    VarScope *sc = find_var(tok);
    if (sc)
      return sc->type_def;
  }
  return NULL;
}

// __attribute__((...)) and __declspec(...) carry no meaning for cinder, which
// does not implement any of them. They are skipped by matching parentheses
// rather than by parsing, because the contents are an open-ended vocabulary
// and the alternative is failing on any code that uses one.
static void skip_attributes(Token **rest, Token *tok) {
  for (;;) {
    if (equal(tok, "__attribute__") || equal(tok, "__declspec") ||
        equal(tok, "__extension__")) {
      tok = tok->next;
      if (!equal(tok, "(")) {
        continue;
      }
      int level = 0;
      do {
        if (equal(tok, "("))
          level++;
        else if (equal(tok, ")"))
          level--;
        tok = tok->next;
      } while (level > 0 && tok->kind != TK_EOF);
      continue;
    }
    break;
  }
  *rest = tok;
}

//
// Declaration specifiers
//
// The type specifier keywords may appear in any order and repeat in the ways
// the standard allows: `unsigned long int` and `int long unsigned` name the
// same type. Rather than enumerate the orderings, each keyword adds a fixed
// amount to a counter, so every legal spelling of a type sums to the same
// number and one switch decides them all. `long` is spaced so that adding it
// twice cannot collide with any other combination.
//

enum {
  VOID = 1 << 0,
  BOOL = 1 << 2,
  CHAR = 1 << 4,
  SHORT = 1 << 6,
  INT = 1 << 8,
  LONG = 1 << 10,
  FLOAT = 1 << 12,
  DOUBLE = 1 << 14,
  OTHER = 1 << 16,
  SIGNED = 1 << 17,
  UNSIGNED = 1 << 18,
};

static Type *declspec(Token **rest, Token *tok, VarAttr *attr) {
  Type *ty = ty_int;
  int counter = 0;

  while (is_typename(tok)) {
    skip_attributes(&tok, tok);
    if (!is_typename(tok))
      break;

    // Storage class specifiers.
    if (equal(tok, "typedef") || equal(tok, "static") || equal(tok, "extern") ||
        equal(tok, "inline")) {
      if (!attr)
        error_tok(tok, "a storage class specifier is not allowed here");

      if (equal(tok, "typedef"))
        attr->is_typedef = true;
      else if (equal(tok, "static"))
        attr->is_static = true;
      else if (equal(tok, "extern"))
        attr->is_extern = true;
      else
        attr->is_inline = true;

      if (attr->is_typedef && (attr->is_static || attr->is_extern))
        error_tok(tok, "typedef cannot be combined with static or extern");
      tok = tok->next;
      continue;
    }

    // Qualifiers cinder accepts and ignores. `const` is not enforced: without
    // it the compiler still produces correct code, and diagnosing const
    // violations is a checker, not a code generator.
    if (equal(tok, "const") || equal(tok, "volatile") || equal(tok, "auto") ||
        equal(tok, "register") || equal(tok, "restrict") ||
        equal(tok, "_Noreturn")) {
      tok = tok->next;
      continue;
    }

    if (equal(tok, "_Alignas")) {
      tok = skip(tok->next, "(");
      if (is_typename(tok))
        attr->align = typename(&tok, tok)->align;
      else
        attr->align = const_expr(&tok, tok);
      tok = skip(tok, ")");
      continue;
    }

    // A user-defined type ends the specifier: two of them in a row would be a
    // declarator, not more specifiers.
    Type *ty2 = find_typedef(tok);
    if (equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum") ||
        ty2) {
      if (counter)
        break;

      if (equal(tok, "struct")) {
        ty = struct_decl(&tok, tok->next);
      } else if (equal(tok, "union")) {
        ty = union_decl(&tok, tok->next);
      } else if (equal(tok, "enum")) {
        ty = enum_specifier(&tok, tok->next);
      } else {
        ty = ty2;
        tok = tok->next;
      }

      counter += OTHER;
      continue;
    }

    if (equal(tok, "void"))
      counter += VOID;
    else if (equal(tok, "_Bool"))
      counter += BOOL;
    else if (equal(tok, "char"))
      counter += CHAR;
    else if (equal(tok, "short"))
      counter += SHORT;
    else if (equal(tok, "int"))
      counter += INT;
    else if (equal(tok, "long"))
      counter += LONG;
    else if (equal(tok, "float"))
      counter += FLOAT;
    else if (equal(tok, "double"))
      counter += DOUBLE;
    else if (equal(tok, "signed"))
      counter |= SIGNED;
    else if (equal(tok, "unsigned"))
      counter |= UNSIGNED;
    else
      unreachable();

    switch (counter) {
    case VOID:
      ty = ty_void;
      break;
    case BOOL:
      ty = ty_bool;
      break;
    case CHAR:
    case SIGNED + CHAR:
      ty = ty_char;
      break;
    case UNSIGNED + CHAR:
      ty = ty_uchar;
      break;
    case SHORT:
    case SHORT + INT:
    case SIGNED + SHORT:
    case SIGNED + SHORT + INT:
      ty = ty_short;
      break;
    case UNSIGNED + SHORT:
    case UNSIGNED + SHORT + INT:
      ty = ty_ushort;
      break;
    case INT:
    case SIGNED:
    case SIGNED + INT:
      ty = ty_int;
      break;
    case UNSIGNED:
    case UNSIGNED + INT:
      ty = ty_uint;
      break;
    // On Windows `long` is 32 bits. Two of them are 64.
    case LONG:
    case LONG + INT:
    case SIGNED + LONG:
    case SIGNED + LONG + INT:
      ty = ty_long;
      break;
    case UNSIGNED + LONG:
    case UNSIGNED + LONG + INT:
      ty = ty_ulong;
      break;
    case LONG + LONG:
    case LONG + LONG + INT:
    case SIGNED + LONG + LONG:
    case SIGNED + LONG + LONG + INT:
      ty = ty_llong;
      break;
    case UNSIGNED + LONG + LONG:
    case UNSIGNED + LONG + LONG + INT:
      ty = ty_ullong;
      break;
    case FLOAT:
      ty = ty_float;
      break;
    case DOUBLE:
    // cinder has no 80-bit type, so `long double` is `double`. That is what
    // the Microsoft ABI does as well.
    case LONG + DOUBLE:
      ty = ty_double;
      break;
    default:
      error_tok(tok, "invalid combination of type specifiers");
    }

    tok = tok->next;
  }

  *rest = tok;
  return ty;
}

// func-params = ("void" | param ("," param)* ("," "...")?)? ")"
static Type *func_params(Token **rest, Token *tok, Type *ty) {
  // `f(void)` takes no arguments. `f()` in C means "unspecified", which cinder
  // treats as variadic-compatible rather than diagnosing.
  if (equal(tok, "void") && equal(tok->next, ")")) {
    *rest = tok->next->next;
    return func_type(ty);
  }

  Type head = {0};
  Type *cur = &head;
  bool is_variadic = false;

  while (!equal(tok, ")")) {
    if (cur != &head)
      tok = skip(tok, ",");

    if (equal(tok, "...")) {
      is_variadic = true;
      tok = tok->next;
      skip(tok, ")");
      break;
    }

    Type *ty2 = declspec(&tok, tok, NULL);
    ty2 = declarator(&tok, tok, ty2);

    Token *name = ty2->name;

    if (ty2->kind == TY_ARRAY) {
      // An array parameter is a pointer. This is not an optimisation, it is
      // what the standard says the type *is*, which is why sizeof on it gives
      // the pointer size and surprises everyone once.
      ty2 = pointer_to(ty2->base);
      ty2->name = name;
    } else if (ty2->kind == TY_FUNC) {
      ty2 = pointer_to(ty2);
      ty2->name = name;
    }

    cur = cur->next = copy_type(ty2);
  }

  if (cur == &head)
    is_variadic = true;

  ty = func_type(ty);
  ty->params = head.next;
  ty->is_variadic = is_variadic;
  *rest = tok->next;
  return ty;
}

// array-dimensions = const-expr? "]" type-suffix
static Type *array_dimensions(Token **rest, Token *tok, Type *ty) {
  while (equal(tok, "static") || equal(tok, "restrict"))
    tok = tok->next;

  if (equal(tok, "]")) {
    ty = type_suffix(rest, tok->next, ty);
    // A negative length marks "incomplete", as in `extern int a[];`.
    ty = array_of(ty, 0);
    ty->array_len = -1;
    ty->size = 0;
    return ty;
  }

  int64_t sz = const_expr(&tok, tok);
  tok = skip(tok, "]");
  ty = type_suffix(rest, tok, ty);
  return array_of(ty, sz);
}

static Type *type_suffix(Token **rest, Token *tok, Type *ty) {
  if (equal(tok, "("))
    return func_params(rest, tok->next, ty);
  if (equal(tok, "["))
    return array_dimensions(rest, tok->next, ty);
  *rest = tok;
  return ty;
}

static Type *pointers(Token **rest, Token *tok, Type *ty) {
  while (consume(&tok, tok, "*")) {
    ty = pointer_to(ty);
    while (equal(tok, "const") || equal(tok, "volatile") ||
           equal(tok, "restrict"))
      tok = tok->next;
  }
  *rest = tok;
  return ty;
}

// declarator = pointers ("(" declarator ")" | ident) type-suffix
//
// The parenthesised form is what makes `int (*fp)(void)` a pointer to
// function rather than a function returning a pointer. It is handled by
// parsing the inner declarator against a placeholder, building the real type
// from the suffix, and then re-parsing the inner declarator against that. Two
// passes over a handful of tokens, and it avoids threading a continuation
// through the whole declarator grammar.
static Type *declarator(Token **rest, Token *tok, Type *ty) {
  ty = pointers(&tok, tok, ty);

  if (equal(tok, "(")) {
    Token *start = tok;
    Type dummy = {0};
    declarator(&tok, start->next, &dummy);
    tok = skip(tok, ")");
    ty = type_suffix(rest, tok, ty);
    return declarator(&tok, start->next, ty);
  }

  Token *name = NULL;
  Token *name_pos = tok;

  if (tok->kind == TK_IDENT) {
    name = tok;
    tok = tok->next;
  }

  ty = type_suffix(rest, tok, ty);
  ty->name = name;
  ty->name_pos = name_pos;
  return ty;
}

// An abstract declarator has no identifier: the `int *` in `sizeof(int *)`.
static Type *abstract_declarator(Token **rest, Token *tok, Type *ty) {
  ty = pointers(&tok, tok, ty);

  if (equal(tok, "(")) {
    Token *start = tok;
    Type dummy = {0};
    abstract_declarator(&tok, start->next, &dummy);
    tok = skip(tok, ")");
    ty = type_suffix(rest, tok, ty);
    return abstract_declarator(&tok, start->next, ty);
  }

  return type_suffix(rest, tok, ty);
}

static Type *typename(Token **rest, Token *tok) {
  Type *ty = declspec(&tok, tok, NULL);
  return abstract_declarator(rest, tok, ty);
}

static bool is_end(Token *tok) {
  return equal(tok, "}") || (equal(tok, ",") && equal(tok->next, "}"));
}

static bool consume_end(Token **rest, Token *tok) {
  if (equal(tok, "}")) {
    *rest = tok->next;
    return true;
  }
  // A trailing comma before the closing brace is allowed.
  if (equal(tok, ",") && equal(tok->next, "}")) {
    *rest = tok->next->next;
    return true;
  }
  return false;
}

// enum-specifier = ident? "{" enum-list? "}" | ident
static Type *enum_specifier(Token **rest, Token *tok) {
  Type *ty = enum_type();

  Token *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  if (tag && !equal(tok, "{")) {
    Type *ty2 = find_tag(tag);
    if (!ty2)
      error_tok(tag, "unknown enum type");
    if (ty2->kind != TY_ENUM)
      error_tok(tag, "not an enum tag");
    *rest = tok;
    return ty2;
  }

  tok = skip(tok, "{");

  int i = 0;
  int val = 0;
  while (!consume_end(rest, tok)) {
    if (i++ > 0)
      tok = skip(tok, ",");

    char *name = get_ident(tok);
    tok = tok->next;

    if (equal(tok, "="))
      val = const_expr(&tok, tok->next);

    // Enum constants live in the ordinary identifier namespace, not the tag
    // namespace, which is why they can collide with variable names.
    VarScope *sc = push_scope(name);
    sc->enum_ty = ty;
    sc->enum_val = val++;
  }

  if (tag)
    push_tag_scope(tag, ty);
  return ty;
}

//
// Declarations
//

static Node *declaration(Token **rest, Token *tok, Type *basety,
                         VarAttr *attr) {
  Node head = {0};
  Node *cur = &head;
  int i = 0;

  while (!equal(tok, ";")) {
    if (i++ > 0) {
      // Only a comma or a semicolon may follow a declarator. Reporting the
      // comma alone is technically true and unhelpful: the mistake is almost
      // always a missing semicolon, so the message should name that first.
      if (!equal(tok, ","))
        error_tok(tok, "expected ';' or ',' after the declaration");
      tok = tok->next;
    }

    Type *ty = declarator(&tok, tok, basety);
    if (ty->kind == TY_VOID)
      error_tok(tok, "a variable cannot have type void");
    if (!ty->name)
      error_tok(ty->name_pos, "a variable name is required");

    // A `static` local is a global with a scoped name: it lives for the whole
    // program but is only visible here.
    if (attr && attr->is_static) {
      Obj *var = new_anon_gvar(ty);
      push_scope(get_ident(ty->name))->var = var;
      if (equal(tok, "="))
        gvar_initializer(&tok, tok->next, var);
      continue;
    }

    Obj *var = new_lvar(get_ident(ty->name), ty);
    if (attr && attr->align)
      var->align = attr->align;

    if (equal(tok, "=")) {
      Node *expr = lvar_initializer(&tok, tok->next, var);
      cur = cur->next = new_unary(ND_EXPR_STMT, expr, tok);
    }

    if (var->ty->size < 0)
      error_tok(ty->name, "variable has incomplete type");
    if (var->ty->kind == TY_VOID)
      error_tok(ty->name, "a variable cannot have type void");
  }

  Node *node = new_node(ND_BLOCK, tok);
  node->body = head.next;
  *rest = tok->next;
  return node;
}

//
// Initialisers
//

static Initializer *new_initializer(Type *ty, bool is_flexible) {
  Initializer *init = xcalloc(1, sizeof(Initializer));
  init->ty = ty;

  if (ty->kind == TY_ARRAY) {
    // A flexible array member, or `int a[] = {...}`, has no length yet. Mark
    // it and let the initialiser decide.
    //
    // The test is on array_len, not on size. An incomplete array has size 0
    // but length -1, and reading the length instead allocated a negative
    // number of children.
    if (is_flexible && ty->array_len < 0) {
      init->is_flexible = true;
      return init;
    }

    init->children = xcalloc(ty->array_len, sizeof(Initializer *));
    for (int i = 0; i < ty->array_len; i++)
      init->children[i] = new_initializer(ty->base, false);
    return init;
  }

  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    int len = 0;
    for (Member *mem = ty->members; mem; mem = mem->next)
      len++;

    init->children = xcalloc(len, sizeof(Initializer *));

    for (Member *mem = ty->members; mem; mem = mem->next) {
      if (is_flexible && ty->is_flexible && !mem->next) {
        Initializer *child = xcalloc(1, sizeof(Initializer));
        child->ty = mem->ty;
        child->is_flexible = true;
        init->children[mem->idx] = child;
      } else {
        init->children[mem->idx] = new_initializer(mem->ty, false);
      }
    }
    return init;
  }

  return init;
}

static void skip_excess_element(Token **rest, Token *tok) {
  if (equal(tok, "{")) {
    skip_excess_element(&tok, tok->next);
    *rest = skip(tok, "}");
    return;
  }
  assign(rest, tok);
}

// A string literal initialising a char array copies characters, not a pointer.
static void string_initializer(Token **rest, Token *tok, Initializer *init) {
  if (init->is_flexible)
    *init = *new_initializer(array_of(init->ty->base, tok->ty->array_len),
                             false);

  // The terminating NUL is dropped when the array is exactly the length of the
  // text: `char a[3] = "abc";` is legal and a is not NUL-terminated.
  int len = (init->ty->array_len < tok->ty->array_len) ? init->ty->array_len
                                                       : tok->ty->array_len;

  for (int i = 0; i < len; i++)
    init->children[i]->expr = new_num(tok->str[i], tok);
  *rest = tok->next;
}

// Count how many elements an initialiser list gives, so that `int a[] = {...}`
// can size the array. Nested braces and designators both have to be accounted
// for, which is why this cannot simply count commas.
static int count_array_init_elements(Token *tok, Type *ty) {
  bool first = true;
  Initializer *dummy = new_initializer(ty->base, true);

  int i = 0, max = 0;

  while (!consume_end(&tok, tok)) {
    if (!first)
      tok = skip(tok, ",");
    first = false;

    if (equal(tok, "[")) {
      i = const_expr(&tok, tok->next);
      if (equal(tok, "..."))
        i = const_expr(&tok, tok->next);
      tok = skip(tok, "]");
      initializer2(&tok, tok, dummy);
    } else {
      initializer2(&tok, tok, dummy);
    }

    i++;
    if (i > max)
      max = i;
  }
  return max;
}

// array-designator = "[" const-expr "]"
static void array_designator(Token **rest, Token *tok, Type *ty, int *begin) {
  *begin = const_expr(&tok, tok->next);
  if (*begin >= ty->array_len)
    error_tok(tok, "array designator index exceeds the array bounds");
  *rest = skip(tok, "]");
}

// struct-designator = "." ident
static Member *struct_designator(Token **rest, Token *tok, Type *ty) {
  Token *start = tok;
  tok = skip(tok, ".");
  if (tok->kind != TK_IDENT)
    error_tok(tok, "expected a field designator");

  for (Member *mem = ty->members; mem; mem = mem->next) {
    if (mem->name->len == tok->len &&
        !strncmp(mem->name->loc, tok->loc, tok->len)) {
      *rest = tok->next;
      return mem;
    }
  }

  error_tok(start, "struct has no member named '%.*s'", tok->len, tok->loc);
}

static void designation(Token **rest, Token *tok, Initializer *init) {
  if (equal(tok, "[")) {
    if (init->ty->kind != TY_ARRAY)
      error_tok(tok, "an array index designator needs an array");
    int i;
    array_designator(&tok, tok, init->ty, &i);
    designation(&tok, tok, init->children[i]);
    return;
  }

  if (equal(tok, ".") && init->ty->kind == TY_STRUCT) {
    Member *mem = struct_designator(&tok, tok, init->ty);
    designation(&tok, tok, init->children[mem->idx]);
    return;
  }

  if (equal(tok, ".") && init->ty->kind == TY_UNION) {
    Member *mem = struct_designator(&tok, tok, init->ty);
    init->mem = mem;
    designation(rest, tok, init->children[mem->idx]);
    return;
  }

  if (equal(tok, "."))
    error_tok(tok, "a field designator needs a struct or union");

  if (equal(tok, "="))
    tok = tok->next;
  initializer2(rest, tok, init);
}

static void array_initializer(Token **rest, Token *tok, Initializer *init) {
  bool has_brace = consume(&tok, tok, "{");

  if (init->is_flexible) {
    int len = count_array_init_elements(tok, init->ty);
    *init = *new_initializer(array_of(init->ty->base, len), false);
  }

  for (int i = 0; !is_end(tok); i++) {
    if (i > 0)
      tok = skip(tok, ",");

    if (equal(tok, "[")) {
      array_designator(&tok, tok, init->ty, &i);
      designation(&tok, tok, init->children[i]);
      continue;
    }

    if (i < init->ty->array_len)
      initializer2(&tok, tok, init->children[i]);
    else
      skip_excess_element(&tok, tok);
  }

  // consume_end rather than skip("}"), because the loop stops at `,` when the
  // list has a trailing comma -- `{1, 2, 3,}` -- and the brace is one token
  // further along.
  if (has_brace && !consume_end(&tok, tok))
    error_tok(tok, "expected '}'");
  *rest = tok;
}

// An aggregate nested inside a braced initialiser may omit its own braces:
// `int a[2][2] = {1, 2, 3, 4}` is legal. That case stops at the end of the
// enclosing list rather than at a brace.
static void array_initializer_no_brace(Token **rest, Token *tok,
                                       Initializer *init) {
  if (init->is_flexible) {
    int len = count_array_init_elements(tok, init->ty);
    *init = *new_initializer(array_of(init->ty->base, len), false);
  }

  for (int i = 0; i < init->ty->array_len && !is_end(tok); i++) {
    if (i > 0)
      tok = skip(tok, ",");
    initializer2(&tok, tok, init->children[i]);
  }
  *rest = tok;
}

static void struct_initializer(Token **rest, Token *tok, Initializer *init) {
  bool has_brace = consume(&tok, tok, "{");

  Member *mem = init->ty->members;
  bool first = true;

  while (!is_end(tok)) {
    if (!first)
      tok = skip(tok, ",");
    first = false;

    if (equal(tok, ".")) {
      mem = struct_designator(&tok, tok, init->ty);
      designation(&tok, tok, init->children[mem->idx]);
      mem = mem->next;
      continue;
    }

    if (!mem)
      break;

    initializer2(&tok, tok, init->children[mem->idx]);
    mem = mem->next;
  }

  if (has_brace) {
    // Anything left over is skipped rather than diagnosed, matching what
    // compilers do in practice for an over-long initialiser.
    while (!consume_end(&tok, tok)) {
      if (!first)
        tok = skip(tok, ",");
      first = false;
      skip_excess_element(&tok, tok);
    }
  }
  *rest = tok;
}

static void struct_initializer_no_brace(Token **rest, Token *tok,
                                        Initializer *init) {
  bool first = true;

  for (Member *mem = init->ty->members; mem && !is_end(tok); mem = mem->next) {
    if (!first)
      tok = skip(tok, ",");
    first = false;
    initializer2(&tok, tok, init->children[mem->idx]);
  }
  *rest = tok;
}

static void union_initializer(Token **rest, Token *tok, Initializer *init) {
  // Only the first member is initialised, unless a designator says otherwise.
  if (equal(tok, "{") && equal(tok->next, ".")) {
    tok = tok->next;
    Member *mem = struct_designator(&tok, tok, init->ty);
    init->mem = mem;
    designation(&tok, tok, init->children[mem->idx]);
    *rest = skip(tok, "}");
    return;
  }

  init->mem = init->ty->members;

  if (equal(tok, "{")) {
    initializer2(&tok, tok->next, init->children[0]);
    consume(&tok, tok, ",");
    *rest = skip(tok, "}");
  } else {
    initializer2(rest, tok, init->children[0]);
  }
}

static void initializer2(Token **rest, Token *tok, Initializer *init) {
  if (init->ty->kind == TY_ARRAY && tok->kind == TK_STR) {
    string_initializer(rest, tok, init);
    return;
  }

  if (init->ty->kind == TY_ARRAY) {
    if (equal(tok, "{"))
      array_initializer(rest, tok, init);
    else
      array_initializer_no_brace(rest, tok, init);
    return;
  }

  if (init->ty->kind == TY_STRUCT) {
    if (equal(tok, "{")) {
      struct_initializer(rest, tok, init);
      return;
    }

    // A struct can be initialised from another struct of the same type, as in
    // `struct T x = y;`, which is an assignment rather than a list.
    Node *expr = assign(rest, tok);
    add_type(expr);
    if (expr->ty->kind == TY_STRUCT) {
      init->expr = expr;
      return;
    }

    struct_initializer_no_brace(rest, tok, init);
    return;
  }

  if (init->ty->kind == TY_UNION) {
    union_initializer(rest, tok, init);
    return;
  }

  // A scalar wrapped in braces: `int x = {3};`
  if (equal(tok, "{")) {
    initializer2(&tok, tok->next, init);
    *rest = skip(tok, "}");
    return;
  }

  init->expr = assign(rest, tok);
}

static Type *copy_struct_type(Type *ty) {
  ty = copy_type(ty);

  Member head = {0};
  Member *cur = &head;
  for (Member *mem = ty->members; mem; mem = mem->next) {
    Member *m = xcalloc(1, sizeof(Member));
    *m = *mem;
    cur = cur->next = m;
  }

  ty->members = head.next;
  return ty;
}

static Initializer *initializer(Token **rest, Token *tok, Type *ty,
                                Type **new_ty) {
  Initializer *init = new_initializer(ty, true);
  initializer2(rest, tok, init);

  // A struct whose last member is a flexible array is re-sized to fit what
  // the initialiser actually provided.
  if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->is_flexible) {
    ty = copy_struct_type(ty);

    Member *mem = ty->members;
    while (mem->next)
      mem = mem->next;
    mem->ty = init->children[mem->idx]->ty;
    ty->size += mem->ty->size;

    *new_ty = ty;
    return init;
  }

  *new_ty = init->ty;
  return init;
}

// init_desg_expr turns a designator chain into the address expression it
// denotes: `a[3].b` becomes the AST for exactly that.
static Node *init_desg_expr(InitDesg *desg, Token *tok) {
  if (desg->var)
    return new_var_node(desg->var, tok);

  if (desg->member) {
    Node *node = new_unary(ND_MEMBER, init_desg_expr(desg->next, tok), tok);
    node->member = desg->member;
    return node;
  }

  Node *lhs = init_desg_expr(desg->next, tok);
  Node *rhs = new_num(desg->idx, tok);
  return new_unary(ND_DEREF, new_add(lhs, rhs, tok), tok);
}

// Walk the initialiser tree and emit one assignment per leaf, chained with
// commas so the whole thing is a single expression.
static Node *create_lvar_init(Initializer *init, Type *ty, InitDesg *desg,
                              Token *tok) {
  if (ty->kind == TY_ARRAY) {
    Node *node = new_node(ND_NULL_EXPR, tok);
    for (int i = 0; i < ty->array_len; i++) {
      InitDesg desg2 = {desg, i, NULL, NULL};
      Node *rhs = create_lvar_init(init->children[i], ty->base, &desg2, tok);
      node = new_binary(ND_COMMA, node, rhs, tok);
    }
    return node;
  }

  if (ty->kind == TY_STRUCT && !init->expr) {
    Node *node = new_node(ND_NULL_EXPR, tok);
    for (Member *mem = ty->members; mem; mem = mem->next) {
      InitDesg desg2 = {desg, 0, mem, NULL};
      Node *rhs =
          create_lvar_init(init->children[mem->idx], mem->ty, &desg2, tok);
      node = new_binary(ND_COMMA, node, rhs, tok);
    }
    return node;
  }

  if (ty->kind == TY_UNION) {
    Member *mem = init->mem ? init->mem : ty->members;
    InitDesg desg2 = {desg, 0, mem, NULL};
    return create_lvar_init(init->children[mem->idx], mem->ty, &desg2, tok);
  }

  if (!init->expr)
    return new_node(ND_NULL_EXPR, tok);

  Node *lhs = init_desg_expr(desg, tok);
  return new_binary(ND_ASSIGN, lhs, init->expr, tok);
}

static Node *lvar_initializer(Token **rest, Token *tok, Obj *var) {
  Initializer *init = initializer(rest, tok, var->ty, &var->ty);
  InitDesg desg = {NULL, 0, NULL, var};

  // A partial initialiser leaves the rest of the object zero, so the whole
  // variable is cleared first and then the given members are assigned. That is
  // one memset plus the explicit stores, rather than a store for every element
  // of a large mostly-zero array.
  Node *lhs = new_node(ND_MEMZERO, tok);
  lhs->var = var;

  Node *rhs = create_lvar_init(init, var->ty, &desg, tok);
  return new_binary(ND_COMMA, lhs, rhs, tok);
}

//
// Global initialisers become bytes in the object file rather than code.
//

static void write_buf(char *buf, uint64_t val, int sz) {
  if (sz == 1)
    *buf = val;
  else if (sz == 2)
    *(uint16_t *)buf = val;
  else if (sz == 4)
    *(uint32_t *)buf = val;
  else if (sz == 8)
    *(uint64_t *)buf = val;
  else
    unreachable();
}

static Relocation *write_gvar_data(Relocation *cur, Initializer *init,
                                   Type *ty, char *buf, int offset) {
  if (ty->kind == TY_ARRAY) {
    int sz = ty->base->size;
    for (int i = 0; i < ty->array_len; i++)
      cur = write_gvar_data(cur, init->children[i], ty->base, buf,
                            offset + sz * i);
    return cur;
  }

  if (ty->kind == TY_STRUCT) {
    for (Member *mem = ty->members; mem; mem = mem->next)
      cur = write_gvar_data(cur, init->children[mem->idx], mem->ty, buf,
                            offset + mem->offset);
    return cur;
  }

  if (ty->kind == TY_UNION) {
    if (!init->mem)
      return cur;
    return write_gvar_data(cur, init->children[init->mem->idx], init->mem->ty,
                           buf, offset);
  }

  if (!init->expr)
    return cur;

  if (ty->kind == TY_FLOAT) {
    *(float *)(buf + offset) = eval_double(init->expr);
    return cur;
  }

  if (ty->kind == TY_DOUBLE) {
    *(double *)(buf + offset) = eval_double(init->expr);
    return cur;
  }

  // If the initialiser names another object, its address is not known until
  // link time. Record a relocation and let the assembler emit the symbol.
  char **label = NULL;
  uint64_t val = eval2(init->expr, &label);

  if (!label) {
    write_buf(buf + offset, val, ty->size);
    return cur;
  }

  Relocation *rel = xcalloc(1, sizeof(Relocation));
  rel->offset = offset;
  rel->label = label;
  rel->addend = val;
  cur->next = rel;
  return cur->next;
}

static void gvar_initializer(Token **rest, Token *tok, Obj *var) {
  Initializer *init = initializer(rest, tok, var->ty, &var->ty);

  Relocation head = {0};
  char *buf = xcalloc(1, var->ty->size);
  write_gvar_data(&head, init, var->ty, buf, 0);
  var->init_data = buf;
  var->rel = head.next;
}

//
// Statements
//

static bool is_typename(Token *tok) {
  static HashMap map;

  if (map.capacity == 0) {
    static char *kw[] = {
        "void",     "_Bool",  "char",     "short",    "int",      "long",
        "struct",   "union",  "typedef",  "enum",     "static",   "extern",
        "signed",   "unsigned", "const",  "volatile", "auto",     "register",
        "restrict", "float",  "double",   "inline",   "_Alignas", "_Noreturn",
        // Not type specifiers, but they may stand where one is expected. If
        // is_typename rejected them the specifier loop would never start, and
        // the attribute would never be skipped.
        "__attribute__", "__declspec", "__extension__",
    };
    for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
      hashmap_put(&map, kw[i], (void *)1);
  }

  return hashmap_get2(&map, tok->loc, tok->len) || find_typedef(tok);
}

// asm-like GNU statement expression, and every other statement form.
static Node *stmt(Token **rest, Token *tok) {
  if (equal(tok, "return")) {
    Node *node = new_node(ND_RETURN, tok);
    if (consume(rest, tok->next, ";"))
      return node;

    Node *exp = expr(&tok, tok->next);
    *rest = skip(tok, ";");

    add_type(exp);
    Type *ty = current_fn->ty->return_ty;
    if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
      exp = new_cast(exp, ty);

    node->lhs = exp;
    return node;
  }

  if (equal(tok, "if")) {
    Node *node = new_node(ND_IF, tok);
    tok = skip(tok->next, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");
    node->then = stmt(&tok, tok);
    if (equal(tok, "else"))
      node->els = stmt(&tok, tok->next);
    *rest = tok;
    return node;
  }

  if (equal(tok, "switch")) {
    Node *node = new_node(ND_SWITCH, tok);
    tok = skip(tok->next, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");

    // Save and restore rather than push and pop: the C stack is the stack.
    Node *sw = current_switch;
    current_switch = node;

    char *brk = brk_label;
    brk_label = node->brk_label = new_unique_name();

    node->then = stmt(rest, tok);

    current_switch = sw;
    brk_label = brk;
    return node;
  }

  if (equal(tok, "case")) {
    if (!current_switch)
      error_tok(tok, "case outside of a switch");

    Node *node = new_node(ND_CASE, tok);
    int64_t val = const_expr(&tok, tok->next);
    tok = skip(tok, ":");
    node->label = new_unique_name();
    node->lhs = stmt(rest, tok);
    node->begin = val;
    node->case_next = current_switch->case_next;
    current_switch->case_next = node;
    return node;
  }

  if (equal(tok, "default")) {
    if (!current_switch)
      error_tok(tok, "default outside of a switch");

    Node *node = new_node(ND_CASE, tok);
    tok = skip(tok->next, ":");
    node->label = new_unique_name();
    node->lhs = stmt(rest, tok);
    current_switch->default_case = node;
    return node;
  }

  if (equal(tok, "for")) {
    Node *node = new_node(ND_FOR, tok);
    tok = skip(tok->next, "(");

    // The init clause of a for loop gets its own scope, so that a variable
    // declared there does not leak into the enclosing block.
    enter_scope();

    char *brk = brk_label;
    char *cont = cont_label;
    brk_label = node->brk_label = new_unique_name();
    cont_label = node->cont_label = new_unique_name();

    if (is_typename(tok)) {
      Type *basety = declspec(&tok, tok, NULL);
      node->init = declaration(&tok, tok, basety, NULL);
    } else {
      node->init = expr_stmt(&tok, tok);
    }

    if (!equal(tok, ";"))
      node->cond = expr(&tok, tok);
    tok = skip(tok, ";");

    if (!equal(tok, ")"))
      node->inc = expr(&tok, tok);
    tok = skip(tok, ")");

    node->then = stmt(rest, tok);

    leave_scope();
    brk_label = brk;
    cont_label = cont;
    return node;
  }

  if (equal(tok, "while")) {
    Node *node = new_node(ND_FOR, tok);
    tok = skip(tok->next, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");

    char *brk = brk_label;
    char *cont = cont_label;
    brk_label = node->brk_label = new_unique_name();
    cont_label = node->cont_label = new_unique_name();

    node->then = stmt(rest, tok);

    brk_label = brk;
    cont_label = cont;
    return node;
  }

  if (equal(tok, "do")) {
    Node *node = new_node(ND_DO, tok);

    char *brk = brk_label;
    char *cont = cont_label;
    brk_label = node->brk_label = new_unique_name();
    cont_label = node->cont_label = new_unique_name();

    node->then = stmt(&tok, tok->next);

    brk_label = brk;
    cont_label = cont;

    tok = skip(tok, "while");
    tok = skip(tok, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");
    *rest = skip(tok, ";");
    return node;
  }

  if (equal(tok, "goto")) {
    Node *node = new_node(ND_GOTO, tok);
    node->label = get_ident(tok->next);
    node->goto_next = gotos;
    gotos = node;
    *rest = skip(tok->next->next, ";");
    return node;
  }

  if (equal(tok, "break")) {
    if (!brk_label)
      error_tok(tok, "break outside of a loop or switch");
    Node *node = new_node(ND_GOTO, tok);
    node->unique_label = brk_label;
    *rest = skip(tok->next, ";");
    return node;
  }

  if (equal(tok, "continue")) {
    if (!cont_label)
      error_tok(tok, "continue outside of a loop");
    Node *node = new_node(ND_GOTO, tok);
    node->unique_label = cont_label;
    *rest = skip(tok->next, ";");
    return node;
  }

  // A label: an identifier followed by a colon, which is only distinguishable
  // from an expression statement by that one token of lookahead.
  if (tok->kind == TK_IDENT && equal(tok->next, ":")) {
    Node *node = new_node(ND_LABEL, tok);
    node->label = xstrndup(tok->loc, tok->len);
    node->unique_label = new_unique_name();
    node->lhs = stmt(rest, tok->next->next);
    node->goto_next = labels;
    labels = node;
    return node;
  }

  if (equal(tok, "{"))
    return compound_stmt(rest, tok->next);

  return expr_stmt(rest, tok);
}

static Node *compound_stmt(Token **rest, Token *tok) {
  Node *node = new_node(ND_BLOCK, tok);
  Node head = {0};
  Node *cur = &head;

  enter_scope();

  while (!equal(tok, "}")) {
    if (is_typename(tok) && !equal(tok->next, ":")) {
      VarAttr attr = {0};
      Type *basety = declspec(&tok, tok, &attr);

      if (attr.is_typedef) {
        tok = parse_typedef(tok, basety);
        continue;
      }

      // A function declared inside a block still declares a global.
      if (is_function(tok)) {
        tok = function(tok, basety, &attr);
        continue;
      }

      if (attr.is_extern) {
        tok = global_variable(tok, basety, &attr);
        continue;
      }

      cur = cur->next = declaration(&tok, tok, basety, &attr);
    } else {
      cur = cur->next = stmt(&tok, tok);
    }
    add_type(cur);
  }

  leave_scope();

  node->body = head.next;
  *rest = tok->next;
  return node;
}

static Node *expr_stmt(Token **rest, Token *tok) {
  if (equal(tok, ";")) {
    *rest = tok->next;
    return new_node(ND_BLOCK, tok);
  }

  Node *node = new_node(ND_EXPR_STMT, tok);
  node->lhs = expr(&tok, tok);
  *rest = skip(tok, ";");
  return node;
}

//
// Expressions
//

static Node *expr(Token **rest, Token *tok) {
  Node *node = assign(&tok, tok);

  if (equal(tok, ","))
    return new_binary(ND_COMMA, node, expr(rest, tok->next), tok);

  *rest = tok;
  return node;
}

// Constant folding, used by `case`, array dimensions, enum values and global
// initialisers -- everywhere the standard requires a value at compile time.
static int64_t eval(Node *node) { return eval2(node, NULL); }

// eval2 also accepts an address constant, as in `int *p = &x + 3;`. When it
// finds one it hands back the label so the caller can emit a relocation.
static int64_t eval2(Node *node, char ***label) {
  add_type(node);

  if (is_flonum(node->ty))
    return eval_double(node);

  switch (node->kind) {
  case ND_ADD:
    return eval2(node->lhs, label) + eval(node->rhs);
  case ND_SUB:
    return eval2(node->lhs, label) - eval(node->rhs);
  case ND_MUL:
    return eval(node->lhs) * eval(node->rhs);
  case ND_DIV:
    if (node->ty->is_unsigned)
      return (uint64_t)eval(node->lhs) / eval(node->rhs);
    return eval(node->lhs) / eval(node->rhs);
  case ND_NEG:
    return -eval(node->lhs);
  case ND_MOD:
    if (node->ty->is_unsigned)
      return (uint64_t)eval(node->lhs) % eval(node->rhs);
    return eval(node->lhs) % eval(node->rhs);
  case ND_BITAND:
    return eval(node->lhs) & eval(node->rhs);
  case ND_BITOR:
    return eval(node->lhs) | eval(node->rhs);
  case ND_BITXOR:
    return eval(node->lhs) ^ eval(node->rhs);
  case ND_SHL:
    return eval(node->lhs) << eval(node->rhs);
  case ND_SHR:
    if (node->ty->is_unsigned && node->ty->size == 8)
      return (uint64_t)eval(node->lhs) >> eval(node->rhs);
    return eval(node->lhs) >> eval(node->rhs);
  case ND_EQ:
    return eval(node->lhs) == eval(node->rhs);
  case ND_NE:
    return eval(node->lhs) != eval(node->rhs);
  case ND_LT:
    if (node->lhs->ty->is_unsigned)
      return (uint64_t)eval(node->lhs) < eval(node->rhs);
    return eval(node->lhs) < eval(node->rhs);
  case ND_LE:
    if (node->lhs->ty->is_unsigned)
      return (uint64_t)eval(node->lhs) <= eval(node->rhs);
    return eval(node->lhs) <= eval(node->rhs);
  case ND_COND:
    return eval(node->cond) ? eval2(node->then, label)
                            : eval2(node->els, label);
  case ND_COMMA:
    return eval2(node->rhs, label);
  case ND_NOT:
    return !eval(node->lhs);
  case ND_BITNOT:
    return ~eval(node->lhs);
  case ND_LOGAND:
    return eval(node->lhs) && eval(node->rhs);
  case ND_LOGOR:
    return eval(node->lhs) || eval(node->rhs);
  case ND_CAST: {
    int64_t val = eval2(node->lhs, label);
    if (is_integer(node->ty)) {
      switch (node->ty->size) {
      case 1:
        return node->ty->is_unsigned ? (uint8_t)val : (int8_t)val;
      case 2:
        return node->ty->is_unsigned ? (uint16_t)val : (int16_t)val;
      case 4:
        return node->ty->is_unsigned ? (uint32_t)val : (int32_t)val;
      }
    }
    return val;
  }
  case ND_ADDR:
    if (!label)
      error_tok(node->tok, "not a compile-time constant");
    if (node->lhs->kind != ND_VAR || node->lhs->var->is_local)
      error_tok(node->tok, "not a compile-time constant");
    *label = &node->lhs->var->name;
    return 0;
  case ND_MEMBER:
    if (!label)
      error_tok(node->tok, "not a compile-time constant");
    if (node->lhs->kind != ND_VAR || node->lhs->var->is_local)
      error_tok(node->tok, "not a compile-time constant");
    *label = &node->lhs->var->name;
    return node->member->offset;
  case ND_VAR:
    // An array or a function used as a value is its address, which is a
    // constant if the object is global.
    if (!label)
      error_tok(node->tok, "not a compile-time constant");
    if (node->var->ty->kind != TY_ARRAY && node->var->ty->kind != TY_FUNC)
      error_tok(node->tok, "not a compile-time constant");
    if (node->var->is_local)
      error_tok(node->tok, "not a compile-time constant");
    *label = &node->var->name;
    return 0;
  case ND_NUM:
    return node->val;
  default:
    error_tok(node->tok, "not a compile-time constant");
  }
}

static double eval_double(Node *node) {
  add_type(node);

  if (is_integer(node->ty)) {
    if (node->ty->is_unsigned)
      return (unsigned long)eval(node);
    return eval(node);
  }

  switch (node->kind) {
  case ND_ADD:
    return eval_double(node->lhs) + eval_double(node->rhs);
  case ND_SUB:
    return eval_double(node->lhs) - eval_double(node->rhs);
  case ND_MUL:
    return eval_double(node->lhs) * eval_double(node->rhs);
  case ND_DIV:
    return eval_double(node->lhs) / eval_double(node->rhs);
  case ND_NEG:
    return -eval_double(node->lhs);
  case ND_COND:
    return eval_double(node->cond) ? eval_double(node->then)
                                   : eval_double(node->els);
  case ND_COMMA:
    return eval_double(node->rhs);
  case ND_CAST:
    if (is_flonum(node->lhs->ty))
      return eval_double(node->lhs);
    return eval(node->lhs);
  case ND_NUM:
    return node->fval;
  default:
    error_tok(node->tok, "not a compile-time constant");
  }
}

int64_t const_expr(Token **rest, Token *tok) {
  Node *node = conditional(rest, tok);
  return eval(node);
}

// `a op= b` has to evaluate `a` exactly once, so it cannot simply expand to
// `a = a op b` when `a` has side effects: `*p++ += 1` would increment twice.
// The address is taken once, stored in a temporary, and used on both sides.
static Node *to_assign(Node *binary) {
  add_type(binary->lhs);
  add_type(binary->rhs);
  Token *tok = binary->tok;

  Obj *var = new_lvar("", pointer_to(binary->lhs->ty));

  Node *expr1 =
      new_binary(ND_ASSIGN, new_var_node(var, tok),
                 new_unary(ND_ADDR, binary->lhs, tok), tok);

  Node *expr2 = new_binary(
      ND_ASSIGN, new_unary(ND_DEREF, new_var_node(var, tok), tok),
      new_binary(binary->kind,
                 new_unary(ND_DEREF, new_var_node(var, tok), tok), binary->rhs,
                 tok),
      tok);

  return new_binary(ND_COMMA, expr1, expr2, tok);
}

static Node *assign(Token **rest, Token *tok) {
  Node *node = conditional(&tok, tok);

  if (equal(tok, "="))
    return new_binary(ND_ASSIGN, node, assign(rest, tok->next), tok);

  if (equal(tok, "+="))
    return to_assign(new_add(node, assign(rest, tok->next), tok));
  if (equal(tok, "-="))
    return to_assign(new_sub(node, assign(rest, tok->next), tok));
  if (equal(tok, "*="))
    return to_assign(
        new_binary(ND_MUL, node, assign(rest, tok->next), tok));
  if (equal(tok, "/="))
    return to_assign(
        new_binary(ND_DIV, node, assign(rest, tok->next), tok));
  if (equal(tok, "%="))
    return to_assign(
        new_binary(ND_MOD, node, assign(rest, tok->next), tok));
  if (equal(tok, "&="))
    return to_assign(
        new_binary(ND_BITAND, node, assign(rest, tok->next), tok));
  if (equal(tok, "|="))
    return to_assign(
        new_binary(ND_BITOR, node, assign(rest, tok->next), tok));
  if (equal(tok, "^="))
    return to_assign(
        new_binary(ND_BITXOR, node, assign(rest, tok->next), tok));
  if (equal(tok, "<<="))
    return to_assign(
        new_binary(ND_SHL, node, assign(rest, tok->next), tok));
  if (equal(tok, ">>="))
    return to_assign(
        new_binary(ND_SHR, node, assign(rest, tok->next), tok));

  *rest = tok;
  return node;
}

static Node *conditional(Token **rest, Token *tok) {
  Node *cond = logor(&tok, tok);

  if (!equal(tok, "?")) {
    *rest = tok;
    return cond;
  }

  Node *node = new_node(ND_COND, tok);
  node->cond = cond;
  node->then = expr(&tok, tok->next);
  tok = skip(tok, ":");
  node->els = conditional(rest, tok);
  return node;
}

static Node *logor(Token **rest, Token *tok) {
  Node *node = logand(&tok, tok);
  while (equal(tok, "||")) {
    Token *start = tok;
    node = new_binary(ND_LOGOR, node, logand(&tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

static Node *logand(Token **rest, Token *tok) {
  Node *node = bitor_(&tok, tok);
  while (equal(tok, "&&")) {
    Token *start = tok;
    node = new_binary(ND_LOGAND, node, bitor_(&tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

static Node *bitor_(Token **rest, Token *tok) {
  Node *node = bitxor_(&tok, tok);
  while (equal(tok, "|")) {
    Token *start = tok;
    node = new_binary(ND_BITOR, node, bitxor_(&tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

static Node *bitxor_(Token **rest, Token *tok) {
  Node *node = bitand_(&tok, tok);
  while (equal(tok, "^")) {
    Token *start = tok;
    node = new_binary(ND_BITXOR, node, bitand_(&tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

static Node *bitand_(Token **rest, Token *tok) {
  Node *node = equality(&tok, tok);
  while (equal(tok, "&")) {
    Token *start = tok;
    node = new_binary(ND_BITAND, node, equality(&tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

static Node *equality(Token **rest, Token *tok) {
  Node *node = relational(&tok, tok);

  for (;;) {
    Token *start = tok;

    if (equal(tok, "==")) {
      node = new_binary(ND_EQ, node, relational(&tok, tok->next), start);
      continue;
    }
    if (equal(tok, "!=")) {
      node = new_binary(ND_NE, node, relational(&tok, tok->next), start);
      continue;
    }

    *rest = tok;
    return node;
  }
}

static Node *relational(Token **rest, Token *tok) {
  Node *node = shift(&tok, tok);

  for (;;) {
    Token *start = tok;

    if (equal(tok, "<")) {
      node = new_binary(ND_LT, node, shift(&tok, tok->next), start);
      continue;
    }
    if (equal(tok, "<=")) {
      node = new_binary(ND_LE, node, shift(&tok, tok->next), start);
      continue;
    }
    // `a > b` is `b < a`. Emitting only two comparison node kinds instead of
    // four halves the cases the code generator has to handle.
    if (equal(tok, ">")) {
      node = new_binary(ND_LT, shift(&tok, tok->next), node, start);
      continue;
    }
    if (equal(tok, ">=")) {
      node = new_binary(ND_LE, shift(&tok, tok->next), node, start);
      continue;
    }

    *rest = tok;
    return node;
  }
}

static Node *shift(Token **rest, Token *tok) {
  Node *node = add(&tok, tok);

  for (;;) {
    Token *start = tok;

    if (equal(tok, "<<")) {
      node = new_binary(ND_SHL, node, add(&tok, tok->next), start);
      continue;
    }
    if (equal(tok, ">>")) {
      node = new_binary(ND_SHR, node, add(&tok, tok->next), start);
      continue;
    }

    *rest = tok;
    return node;
  }
}

// `+` is scaled when either side is a pointer: `p + 1` advances by
// sizeof(*p) bytes, not one. Doing the scaling here, in the parser, keeps the
// code generator free of type knowledge.
static Node *new_add(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);

  if (is_numeric(lhs->ty) && is_numeric(rhs->ty))
    return new_binary(ND_ADD, lhs, rhs, tok);

  if (lhs->ty->base && rhs->ty->base)
    error_tok(tok, "invalid operands: cannot add two pointers");

  // Canonicalise `num + ptr` to `ptr + num`.
  if (!lhs->ty->base && rhs->ty->base) {
    Node *tmp = lhs;
    lhs = rhs;
    rhs = tmp;
  }

  rhs = new_binary(ND_MUL, rhs, new_long(lhs->ty->base->size, tok), tok);
  return new_binary(ND_ADD, lhs, rhs, tok);
}

static Node *new_sub(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);

  if (is_numeric(lhs->ty) && is_numeric(rhs->ty))
    return new_binary(ND_SUB, lhs, rhs, tok);

  // ptr - num
  if (lhs->ty->base && is_integer(rhs->ty)) {
    rhs = new_binary(ND_MUL, rhs, new_long(lhs->ty->base->size, tok), tok);
    add_type(rhs);
    Node *node = new_binary(ND_SUB, lhs, rhs, tok);
    node->ty = lhs->ty;
    return node;
  }

  // ptr - ptr is how many elements apart they are, so the byte difference is
  // divided by the element size.
  if (lhs->ty->base && rhs->ty->base) {
    Node *node = new_binary(ND_SUB, lhs, rhs, tok);
    node->ty = ty_llong;
    return new_binary(ND_DIV, node, new_num(lhs->ty->base->size, tok), tok);
  }

  error_tok(tok, "invalid operands to -");
}

static Node *add(Token **rest, Token *tok) {
  Node *node = mul(&tok, tok);

  for (;;) {
    Token *start = tok;

    if (equal(tok, "+")) {
      node = new_add(node, mul(&tok, tok->next), start);
      continue;
    }
    if (equal(tok, "-")) {
      node = new_sub(node, mul(&tok, tok->next), start);
      continue;
    }

    *rest = tok;
    return node;
  }
}

static Node *mul(Token **rest, Token *tok) {
  Node *node = cast(&tok, tok);

  for (;;) {
    Token *start = tok;

    if (equal(tok, "*")) {
      node = new_binary(ND_MUL, node, cast(&tok, tok->next), start);
      continue;
    }
    if (equal(tok, "/")) {
      node = new_binary(ND_DIV, node, cast(&tok, tok->next), start);
      continue;
    }
    if (equal(tok, "%")) {
      node = new_binary(ND_MOD, node, cast(&tok, tok->next), start);
      continue;
    }

    *rest = tok;
    return node;
  }
}

static Node *cast(Token **rest, Token *tok) {
  // The one genuinely ambiguous production in C: `(x)` is a cast only if x
  // names a type, which requires the symbol table.
  if (equal(tok, "(") && is_typename(tok->next)) {
    Token *start = tok;
    Type *ty = typename(&tok, tok->next);
    tok = skip(tok, ")");

    // A brace after the closing paren makes it a compound literal, not a cast.
    if (equal(tok, "{"))
      return unary(rest, start);

    Node *node = new_cast(cast(rest, tok), ty);
    node->tok = start;
    return node;
  }

  return unary(rest, tok);
}

static Node *unary(Token **rest, Token *tok) {
  if (equal(tok, "+"))
    return cast(rest, tok->next);

  if (equal(tok, "-"))
    return new_unary(ND_NEG, cast(rest, tok->next), tok);

  if (equal(tok, "&")) {
    Node *lhs = cast(rest, tok->next);
    add_type(lhs);
    return new_unary(ND_ADDR, lhs, tok);
  }

  if (equal(tok, "*")) {
    // Dereferencing a function pointer is a no-op: `*f` and `f` and `(****f)`
    // all denote the same function, which is why `(*printf)("x")` works.
    Node *node = cast(rest, tok->next);
    add_type(node);
    if (node->ty->kind == TY_FUNC)
      return node;
    return new_unary(ND_DEREF, node, tok);
  }

  if (equal(tok, "!"))
    return new_unary(ND_NOT, cast(rest, tok->next), tok);

  if (equal(tok, "~"))
    return new_unary(ND_BITNOT, cast(rest, tok->next), tok);

  // ++i is i += 1
  if (equal(tok, "++"))
    return to_assign(new_add(unary(rest, tok->next), new_num(1, tok), tok));

  if (equal(tok, "--"))
    return to_assign(new_sub(unary(rest, tok->next), new_num(1, tok), tok));

  return postfix(rest, tok);
}

// struct-members = (declspec declarator ("," declarator)* ";")*
static void struct_members(Token **rest, Token *tok, Type *ty) {
  Member head = {0};
  Member *cur = &head;
  int idx = 0;

  while (!equal(tok, "}")) {
    VarAttr attr = {0};
    Type *basety = declspec(&tok, tok, &attr);
    int first = true;

    while (!consume(&tok, tok, ";")) {
      if (!first)
        tok = skip(tok, ",");
      first = false;

      Member *mem = xcalloc(1, sizeof(Member));
      mem->ty = declarator(&tok, tok, basety);
      mem->name = mem->ty->name;
      mem->idx = idx++;
      mem->align = attr.align ? attr.align : mem->ty->align;
      cur = cur->next = mem;
    }
  }

  // A trailing array of unspecified length is a flexible array member: it
  // contributes nothing to sizeof, and the allocation is expected to be larger
  // than the struct.
  if (cur != &head && cur->ty->kind == TY_ARRAY && cur->ty->array_len < 0) {
    cur->ty = array_of(cur->ty->base, 0);
    ty->is_flexible = true;
  }

  *rest = tok->next;
  ty->members = head.next;
}

// struct-union-decl = ident? ("{" struct-members)?
static Type *struct_union_decl(Token **rest, Token *tok) {
  Token *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  if (tag && !equal(tok, "{")) {
    *rest = tok;

    Type *ty = find_tag(tag);
    if (ty)
      return ty;

    // A forward reference: `struct T;` or a pointer to an as-yet undefined
    // struct. The size is -1 until the definition is seen, which is what makes
    // "incomplete type" diagnosable.
    ty = struct_type();
    ty->size = -1;
    push_tag_scope(tag, ty);
    return ty;
  }

  tok = skip(tok, "{");

  Type *ty = struct_type();
  struct_members(rest, tok, ty);

  if (tag) {
    // If the tag was already declared in *this* scope, redefine it in place, so
    // that pointers taken to the incomplete type now see the members.
    Type *ty2 = hashmap_get2(&scope->tags, tag->loc, tag->len);
    if (ty2) {
      *ty2 = *ty;
      return ty2;
    }
    push_tag_scope(tag, ty);
  }
  return ty;
}

static Type *struct_decl(Token **rest, Token *tok) {
  Type *ty = struct_union_decl(rest, tok);
  ty->kind = TY_STRUCT;

  if (ty->size < 0)
    return ty;

  // Lay the members out. Each is aligned to its own alignment, and the struct
  // as a whole is aligned to the widest member -- which is what makes an array
  // of structs keep every element aligned.
  int bits = 0;
  for (Member *mem = ty->members; mem; mem = mem->next) {
    bits = align_to(bits, mem->align * 8);
    mem->offset = bits / 8;
    bits += mem->ty->size * 8;

    if (ty->align < mem->align)
      ty->align = mem->align;
  }
  ty->size = align_to(bits, ty->align * 8) / 8;
  return ty;
}

static Type *union_decl(Token **rest, Token *tok) {
  Type *ty = struct_union_decl(rest, tok);
  ty->kind = TY_UNION;

  if (ty->size < 0)
    return ty;

  // Every member of a union is at offset zero, and the union is as large as
  // its largest member.
  for (Member *mem = ty->members; mem; mem = mem->next) {
    if (ty->align < mem->align)
      ty->align = mem->align;
    if (ty->size < mem->ty->size)
      ty->size = mem->ty->size;
  }
  ty->size = align_to(ty->size, ty->align);
  return ty;
}

static Member *get_struct_member(Type *ty, Token *tok) {
  for (Member *mem = ty->members; mem; mem = mem->next)
    if (mem->name->len == tok->len &&
        !strncmp(mem->name->loc, tok->loc, tok->len))
      return mem;
  return NULL;
}

static Node *struct_ref(Node *node, Token *tok) {
  add_type(node);
  if (node->ty->kind != TY_STRUCT && node->ty->kind != TY_UNION)
    error_tok(node->tok, "not a struct or union");

  Member *mem = get_struct_member(node->ty, tok);
  if (!mem)
    error_tok(tok, "no member named '%.*s'", tok->len, tok->loc);

  Node *n = new_unary(ND_MEMBER, node, tok);
  n->member = mem;
  return n;
}

// `i++` must yield the old value. Rather than a dedicated node kind, it is
// rewritten as `(i += 1) - 1`, which produces the right value with the
// machinery that already exists for compound assignment.
static Node *new_inc_dec(Node *node, Token *tok, int addend) {
  add_type(node);
  return new_cast(
      new_add(to_assign(new_add(node, new_num(addend, tok), tok)),
              new_num(-addend, tok), tok),
      node->ty);
}

static Node *postfix(Token **rest, Token *tok) {
  Node *node = primary(&tok, tok);

  for (;;) {
    if (equal(tok, "(")) {
      // A function call.
      Token *start = tok;
      tok = tok->next;

      add_type(node);

      Type *ty;
      if (node->ty->kind == TY_FUNC) {
        ty = node->ty;
      } else if (node->ty->kind == TY_PTR && node->ty->base->kind == TY_FUNC) {
        ty = node->ty->base;
      } else {
        error_tok(node->tok, "not a function");
      }

      Type *param_ty = ty->params;

      Node head = {0};
      Node *cur = &head;

      while (!equal(tok, ")")) {
        if (cur != &head)
          tok = skip(tok, ",");

        Node *arg = assign(&tok, tok);
        add_type(arg);

        if (param_ty) {
          if (param_ty->kind != TY_STRUCT && param_ty->kind != TY_UNION)
            arg = new_cast(arg, param_ty);
          param_ty = param_ty->next;
        } else if (arg->ty->kind == TY_FLOAT) {
          // Default argument promotion: a float passed to a variadic function
          // is promoted to double. printf("%f") reads eight bytes, so getting
          // this wrong prints garbage.
          arg = new_cast(arg, ty_double);
        } else if (arg->ty->kind == TY_ARRAY) {
          arg = new_cast(arg, pointer_to(arg->ty->base));
        }

        cur = cur->next = arg;
      }

      Node *call = new_unary(ND_FUNCALL, node, start);
      call->func_ty = ty;
      call->ty = ty->return_ty;
      call->args = head.next;

      // A call that returns a struct gets a buffer in this frame, whatever its
      // size. A large one is written there by the callee through a hidden
      // pointer; a small one comes back in a register and is spilled into it.
      // Either way the call then behaves like every other struct expression --
      // it leaves an address in RAX -- so assignment and member access need no
      // special case.
      Type *rty = ty->return_ty;
      if (rty->kind == TY_STRUCT || rty->kind == TY_UNION)
        call->ret_buffer = new_lvar("", rty);

      node = call;

      tok = skip(tok, ")");
      continue;
    }

    if (equal(tok, "[")) {
      // `a[i]` is defined as `*(a + i)`, which is why `3[a]` compiles.
      Token *start = tok;
      Node *idx = expr(&tok, tok->next);
      tok = skip(tok, "]");
      node = new_unary(ND_DEREF, new_add(node, idx, start), start);
      continue;
    }

    if (equal(tok, ".")) {
      node = struct_ref(node, tok->next);
      tok = tok->next->next;
      continue;
    }

    if (equal(tok, "->")) {
      // `p->x` is `(*p).x`.
      node = new_unary(ND_DEREF, node, tok);
      node = struct_ref(node, tok->next);
      tok = tok->next->next;
      continue;
    }

    if (equal(tok, "++")) {
      node = new_inc_dec(node, tok, 1);
      tok = tok->next;
      continue;
    }

    if (equal(tok, "--")) {
      node = new_inc_dec(node, tok, -1);
      tok = tok->next;
      continue;
    }

    *rest = tok;
    return node;
  }
}

static Node *primary(Token **rest, Token *tok) {
  Token *start = tok;

  // A GNU statement expression: the value is that of the last statement.
  if (equal(tok, "(") && equal(tok->next, "{")) {
    Node *node = new_node(ND_STMT_EXPR, tok);
    node->body = compound_stmt(&tok, tok->next->next)->body;
    *rest = skip(tok, ")");
    return node;
  }

  // A compound literal: (struct T){...}. It denotes an anonymous object, with
  // static storage at file scope and automatic storage inside a function --
  // which is the whole reason it cannot simply be an expression.
  if (equal(tok, "(") && is_typename(tok->next)) {
    Type *ty = typename(&tok, tok->next);
    tok = skip(tok, ")");

    if (scope->next == NULL) {
      Obj *var = new_anon_gvar(ty);
      gvar_initializer(rest, tok, var);
      return new_var_node(var, start);
    }

    Obj *var = new_lvar("", ty);
    Node *lhs = lvar_initializer(rest, tok, var);
    Node *rhs = new_var_node(var, tok);
    return new_binary(ND_COMMA, lhs, rhs, start);
  }

  if (equal(tok, "(")) {
    Node *node = expr(&tok, tok->next);
    *rest = skip(tok, ")");
    return node;
  }

  // va_start is the one part of <stdarg.h> that cannot be written in C: it has
  // to know where this function's arguments were left. va_arg and va_end are
  // ordinary macros in the bundled header.
  if (equal(tok, "__builtin_va_start")) {
    Node *node = new_node(ND_VA_START, tok);
    tok = skip(tok->next, "(");
    node->lhs = assign(&tok, tok);
    // The second argument names the last fixed parameter. cinder does not need
    // it -- the count of named parameters is already known -- but it is
    // accepted so that the standard spelling compiles.
    if (equal(tok, ","))
      assign(&tok, tok->next);
    *rest = skip(tok, ")");
    return node;
  }

  if (equal(tok, "sizeof") && equal(tok->next, "(") && is_typename(tok->next->next)) {
    Type *ty = typename(&tok, tok->next->next);
    *rest = skip(tok, ")");
    return new_ulong(ty->size, start);
  }

  if (equal(tok, "sizeof")) {
    Node *node = unary(rest, tok->next);
    add_type(node);
    return new_ulong(node->ty->size, tok);
  }

  if (equal(tok, "_Alignof") && equal(tok->next, "(") &&
      is_typename(tok->next->next)) {
    Type *ty = typename(&tok, tok->next->next);
    *rest = skip(tok, ")");
    return new_ulong(ty->align, tok);
  }

  if (equal(tok, "_Alignof")) {
    Node *node = unary(rest, tok->next);
    add_type(node);
    return new_ulong(node->ty->align, tok);
  }

  if (tok->kind == TK_IDENT) {
    VarScope *sc = find_var(tok);
    *rest = tok->next;

    if (sc) {
      if (sc->var)
        return new_var_node(sc->var, tok);
      if (sc->enum_ty)
        return new_num(sc->enum_val, tok);
    }

    if (equal(tok->next, "("))
      error_tok(tok, "implicit declaration of function '%.*s'", tok->len,
                tok->loc);
    error_tok(tok, "undefined variable '%.*s'", tok->len, tok->loc);
  }

  if (tok->kind == TK_STR) {
    Obj *var = new_string_literal(tok->str, tok->ty);
    *rest = tok->next;
    return new_var_node(var, tok);
  }

  if (tok->kind == TK_NUM) {
    Node *node;
    if (is_flonum(tok->ty)) {
      node = new_node(ND_NUM, tok);
      node->fval = tok->fval;
    } else {
      node = new_num(tok->val, tok);
    }
    node->ty = tok->ty;
    *rest = tok->next;
    return node;
  }

  error_tok(tok, "expected an expression");
}

static Token *parse_typedef(Token *tok, Type *basety) {
  bool first = true;

  while (!consume(&tok, tok, ";")) {
    if (!first)
      tok = skip(tok, ",");
    first = false;

    Type *ty = declarator(&tok, tok, basety);
    if (!ty->name)
      error_tok(ty->name_pos, "a typedef name is required");
    push_scope(get_ident(ty->name))->type_def = ty;
  }
  return tok;
}

static void create_param_lvars(Type *param) {
  if (param) {
    // Recurse first, so that parameters end up in the locals list in source
    // order rather than reversed.
    create_param_lvars(param->next);
    if (!param->name)
      error_tok(param->name_pos, "a parameter name is required");
    new_lvar(get_ident(param->name), param);
  }
}

// Match every goto to its label. This happens after the whole function is
// parsed, because a goto may refer to a label further down.
static void resolve_goto_labels(void) {
  for (Node *x = gotos; x; x = x->goto_next) {
    for (Node *y = labels; y; y = y->goto_next) {
      if (!strcmp(x->label, y->label)) {
        x->unique_label = y->unique_label;
        break;
      }
    }

    if (x->unique_label == NULL)
      error_tok(x->tok->next, "use of an undeclared label");
  }

  gotos = labels = NULL;
}

static Obj *find_func(char *name) {
  Scope *sc = scope;
  while (sc->next)
    sc = sc->next;

  VarScope *sc2 = hashmap_get(&sc->vars, name);
  if (sc2 && sc2->var && sc2->var->is_function)
    return sc2->var;
  return NULL;
}

static bool is_function(Token *tok) {
  if (equal(tok, ";"))
    return false;

  Type dummy = {0};
  Type *ty = declarator(&tok, tok, &dummy);
  return ty->kind == TY_FUNC;
}

static Token *function(Token *tok, Type *basety, VarAttr *attr) {
  Type *ty = declarator(&tok, tok, basety);
  if (!ty->name)
    error_tok(ty->name_pos, "a function name is required");

  char *name_str = get_ident(ty->name);

  Obj *fn = find_func(name_str);
  if (fn) {
    // A redeclaration. Only the definition matters; a prototype seen after a
    // definition must not erase the body.
    if (!fn->is_function)
      error_tok(tok, "redeclared as a different kind of symbol");
    if (fn->is_definition && equal(tok, "{"))
      error_tok(tok, "redefinition of '%s'", name_str);
    if (!fn->is_static && attr->is_static)
      error_tok(tok, "static declaration follows a non-static declaration");
    fn->is_definition = fn->is_definition || equal(tok, "{");
  } else {
    fn = new_gvar(name_str, ty);
    fn->is_function = true;
    fn->is_definition = equal(tok, "{");
    fn->is_static = attr->is_static || (attr->is_inline && !attr->is_extern);
  }

  if (consume(&tok, tok, ";"))
    return tok;

  current_fn = fn;
  locals = NULL;
  enter_scope();
  create_param_lvars(ty->params);

  // A function returning a struct larger than a register receives a hidden
  // pointer to the caller's buffer as its first argument. Reserving it here,
  // before the real parameters, is what keeps the register assignment in the
  // code generator honest.
  Type *rty = ty->return_ty;
  if ((rty->kind == TY_STRUCT || rty->kind == TY_UNION) &&
      rty->size != 1 && rty->size != 2 && rty->size != 4 && rty->size != 8)
    new_lvar("", pointer_to(rty));

  fn->params = locals;

  // Nothing needs to be reserved for the variable arguments: on Windows they
  // are already in the caller's outgoing area, contiguous with the named ones,
  // which is why va_list here is a plain pointer rather than the register-save
  // structure System V requires.
  if (ty->is_variadic)
    fn->va_area = (Obj *)1;

  tok = skip(tok, "{");
  fn->body = compound_stmt(&tok, tok);
  fn->locals = locals;
  leave_scope();
  resolve_goto_labels();
  current_fn = NULL;
  return tok;
}

static Token *global_variable(Token *tok, Type *basety, VarAttr *attr) {
  bool first = true;

  while (!consume(&tok, tok, ";")) {
    if (!first)
      tok = skip(tok, ",");
    first = false;

    Type *ty = declarator(&tok, tok, basety);
    if (!ty->name)
      error_tok(ty->name_pos, "a variable name is required");

    Obj *var = new_gvar(get_ident(ty->name), ty);
    var->is_definition = !attr->is_extern;
    var->is_static = attr->is_static;
    if (attr->align)
      var->align = attr->align;

    if (equal(tok, "="))
      gvar_initializer(&tok, tok->next, var);
    else if (!attr->is_extern)
      // A definition without an initialiser is "tentative": it becomes a
      // definition only if nothing else in the program defines it.
      var->is_tentative = true;
  }
  return tok;
}

Obj *parse(Token *tok) {
  globals = NULL;

  while (tok->kind != TK_EOF) {
    VarAttr attr = {0};
    Type *basety = declspec(&tok, tok, &attr);

    // A lone `typedef` or `struct T;` declares a type and nothing else.
    if (attr.is_typedef) {
      tok = parse_typedef(tok, basety);
      continue;
    }

    if (equal(tok, ";")) {
      tok = tok->next;
      continue;
    }

    if (is_function(tok)) {
      tok = function(tok, basety, &attr);
      continue;
    }

    tok = global_variable(tok, basety, &attr);
  }

  return globals;
}
