// The type system, and the conversion rules the standard calls "the usual
// arithmetic conversions".
//
// These rules are the part of C that people carry wrong assumptions about for
// years, and getting them right is most of what separates a compiler from a
// calculator. Two in particular:
//
//   - Integer promotion. Anything narrower than int becomes int before it is
//     operated on, which is why `char a = 200, b = 200; a + b` is 400 and not
//     an overflow.
//   - Signed/unsigned mixing. When the ranks match and one side is unsigned,
//     the signed side converts to unsigned, which is why `-1 < 1u` is false.
//
// Windows is LLP64: long is 4 bytes and long long is 8. Every other 64-bit
// platform C compilers usually target is LP64, where long is 8. cinder follows
// the platform it runs on, because the alternative is a `long` that disagrees
// with the C runtime it links against, and printf("%ld") would then print
// nonsense.

#include "cinder.h"

Type *ty_void = &(Type){TY_VOID, 1, 1};
Type *ty_bool = &(Type){TY_BOOL, 1, 1};

Type *ty_char = &(Type){TY_CHAR, 1, 1};
Type *ty_short = &(Type){TY_SHORT, 2, 2};
Type *ty_int = &(Type){TY_INT, 4, 4};
Type *ty_long = &(Type){TY_LONG, 4, 4};
Type *ty_llong = &(Type){TY_LLONG, 8, 8};

Type *ty_uchar = &(Type){TY_CHAR, 1, 1, true};
Type *ty_ushort = &(Type){TY_SHORT, 2, 2, true};
Type *ty_uint = &(Type){TY_INT, 4, 4, true};
Type *ty_ulong = &(Type){TY_LONG, 4, 4, true};
Type *ty_ullong = &(Type){TY_LLONG, 8, 8, true};

Type *ty_float = &(Type){TY_FLOAT, 4, 4};
Type *ty_double = &(Type){TY_DOUBLE, 8, 8};

static Type *new_type(TypeKind kind, int size, int align) {
  Type *ty = xcalloc(1, sizeof(Type));
  ty->kind = kind;
  ty->size = size;
  ty->align = align;
  return ty;
}

bool is_integer(Type *ty) {
  TypeKind k = ty->kind;
  return k == TY_BOOL || k == TY_CHAR || k == TY_SHORT || k == TY_INT ||
         k == TY_LONG || k == TY_LLONG || k == TY_ENUM;
}

bool is_flonum(Type *ty) {
  return ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE;
}

bool is_numeric(Type *ty) { return is_integer(ty) || is_flonum(ty); }

Type *copy_type(Type *ty) {
  Type *ret = xcalloc(1, sizeof(Type));
  *ret = *ty;
  ret->origin = ty;
  return ret;
}

Type *pointer_to(Type *base) {
  Type *ty = new_type(TY_PTR, 8, 8);
  ty->base = base;
  // A pointer is always unsigned. This is not pedantry: it decides whether
  // comparing two pointers uses `setb` or `setl`, and a signed comparison
  // gives the wrong answer for addresses above 2^63.
  ty->is_unsigned = true;
  return ty;
}

Type *func_type(Type *return_ty) {
  // A function type has a size of 1 rather than 0, because `sizeof(func)` is a
  // GNU extension that yields 1 and because a zero size breaks pointer
  // arithmetic on function pointers.
  Type *ty = new_type(TY_FUNC, 1, 1);
  ty->return_ty = return_ty;
  return ty;
}

Type *array_of(Type *base, int len) {
  Type *ty = new_type(TY_ARRAY, base->size * len, base->align);
  ty->base = base;
  ty->array_len = len;
  return ty;
}

Type *enum_type(void) { return new_type(TY_ENUM, 4, 4); }

Type *struct_type(void) { return new_type(TY_STRUCT, 0, 1); }

bool is_compatible(Type *t1, Type *t2) {
  if (t1 == t2)
    return true;

  if (t1->origin)
    return is_compatible(t1->origin, t2);
  if (t2->origin)
    return is_compatible(t1, t2->origin);

  if (t1->kind != t2->kind)
    return false;

  switch (t1->kind) {
  case TY_CHAR:
  case TY_SHORT:
  case TY_INT:
  case TY_LONG:
  case TY_LLONG:
    return t1->is_unsigned == t2->is_unsigned;
  case TY_FLOAT:
  case TY_DOUBLE:
    return true;
  case TY_PTR:
    return is_compatible(t1->base, t2->base);
  case TY_FUNC: {
    if (!is_compatible(t1->return_ty, t2->return_ty))
      return false;
    if (t1->is_variadic != t2->is_variadic)
      return false;

    Type *p1 = t1->params;
    Type *p2 = t2->params;
    for (; p1 && p2; p1 = p1->next, p2 = p2->next)
      if (!is_compatible(p1, p2))
        return false;
    return p1 == NULL && p2 == NULL;
  }
  case TY_ARRAY:
    if (!is_compatible(t1->base, t2->base))
      return false;
    // An array of unspecified length is compatible with any length, which is
    // what makes `extern int a[];` match a later `int a[10];`.
    return t1->array_len < 0 || t2->array_len < 0 ||
           t1->array_len == t2->array_len;
  default:
    return false;
  }
}

// get_common_type implements the usual arithmetic conversions.
static Type *get_common_type(Type *ty1, Type *ty2) {
  // An array or a function decays to a pointer before anything else happens.
  if (ty1->base)
    return pointer_to(ty1->base);

  if (ty1->kind == TY_FUNC)
    return pointer_to(ty1);
  if (ty2->kind == TY_FUNC)
    return pointer_to(ty2);

  // Floating point wins over integer, and the wider float wins.
  if (ty1->kind == TY_DOUBLE || ty2->kind == TY_DOUBLE)
    return ty_double;
  if (ty1->kind == TY_FLOAT || ty2->kind == TY_FLOAT)
    return ty_float;

  // Integer promotion: anything narrower than int becomes int first.
  if (ty1->size < 4)
    ty1 = ty_int;
  if (ty2->size < 4)
    ty2 = ty_int;

  // The wider rank wins.
  if (ty1->size != ty2->size)
    return (ty1->size < ty2->size) ? ty2 : ty1;

  // Equal rank: if either is unsigned, the result is unsigned. This is the
  // rule behind the classic surprise that (-1 < 1u) is false.
  if (ty2->is_unsigned)
    return ty2;
  return ty1;
}

// usual_arith_conv inserts casts on both operands so that they meet in the
// middle. Doing it as an explicit AST rewrite rather than inside the code
// generator means the conversions are visible when the tree is dumped, and the
// generator only ever sees operands that already agree.
static void usual_arith_conv(Node **lhs, Node **rhs) {
  Type *ty = get_common_type((*lhs)->ty, (*rhs)->ty);
  *lhs = new_cast(*lhs, ty);
  *rhs = new_cast(*rhs, ty);
}

void add_type(Node *node) {
  if (!node || node->ty)
    return;

  add_type(node->lhs);
  add_type(node->rhs);
  add_type(node->cond);
  add_type(node->then);
  add_type(node->els);
  add_type(node->init);
  add_type(node->inc);

  for (Node *n = node->body; n; n = n->next)
    add_type(n);
  for (Node *n = node->args; n; n = n->next)
    add_type(n);

  switch (node->kind) {
  case ND_NUM:
    node->ty = node->ty ? node->ty : ty_int;
    return;
  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
  case ND_DIV:
  case ND_MOD:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
    usual_arith_conv(&node->lhs, &node->rhs);
    node->ty = node->lhs->ty;
    return;
  case ND_NEG: {
    Type *ty = get_common_type(ty_int, node->lhs->ty);
    node->lhs = new_cast(node->lhs, ty);
    node->ty = ty;
    return;
  }
  case ND_ASSIGN:
    if (node->lhs->ty->kind == TY_ARRAY)
      error_tok(node->lhs->tok, "not an lvalue");
    if (node->lhs->ty->kind != TY_STRUCT && node->lhs->ty->kind != TY_UNION)
      node->rhs = new_cast(node->rhs, node->lhs->ty);
    node->ty = node->lhs->ty;
    return;
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
    usual_arith_conv(&node->lhs, &node->rhs);
    node->ty = ty_int;
    return;
  case ND_FUNCALL:
    node->ty = node->func_ty->return_ty;
    return;
  case ND_NOT:
  case ND_LOGOR:
  case ND_LOGAND:
    node->ty = ty_int;
    return;
  case ND_BITNOT:
  case ND_SHL:
  case ND_SHR:
    // A shift does not use the usual arithmetic conversions: each operand is
    // promoted on its own, and the result has the type of the left operand.
    // So `1 << 40L` is still an int expression, and overflows.
    node->ty = node->lhs->ty->size < 4 ? ty_int : node->lhs->ty;
    return;
  case ND_VAR:
    node->ty = node->var->ty;
    return;
  case ND_COND:
    if (node->then->ty->kind == TY_VOID || node->els->ty->kind == TY_VOID) {
      node->ty = ty_void;
    } else {
      usual_arith_conv(&node->then, &node->els);
      node->ty = node->then->ty;
    }
    return;
  case ND_COMMA:
    node->ty = node->rhs->ty;
    return;
  case ND_MEMBER:
    node->ty = node->member->ty;
    return;
  case ND_ADDR: {
    Type *ty = node->lhs->ty;
    // &arr has type "pointer to element", not "pointer to array". This is the
    // decay rule, and skipping it makes `&a[0]` and `&a` disagree.
    if (ty->kind == TY_ARRAY)
      node->ty = pointer_to(ty->base);
    else
      node->ty = pointer_to(ty);
    return;
  }
  case ND_DEREF:
    if (!node->lhs->ty->base)
      error_tok(node->tok, "invalid pointer dereference");
    if (node->lhs->ty->base->kind == TY_VOID)
      error_tok(node->tok, "dereferencing a pointer to void");
    node->ty = node->lhs->ty->base;
    return;
  case ND_STMT_EXPR:
    if (node->body) {
      Node *stmt = node->body;
      while (stmt->next)
        stmt = stmt->next;
      if (stmt->kind == ND_EXPR_STMT) {
        node->ty = stmt->lhs->ty;
        return;
      }
    }
    error_tok(node->tok,
              "a statement expression returning void is not supported");
    return;
  default:
    return;
  }
}
