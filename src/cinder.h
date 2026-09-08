// cinder -- a C compiler written in C.
//
// The pipeline is the classical one, and each stage is a separate file:
//
//   tokenize.c    text          -> token list
//   preprocess.c  token list    -> token list (macros expanded, #includes spliced)
//   parse.c       token list    -> typed AST + symbol tables
//   type.c        type construction and the C conversion rules
//   codegen.c     typed AST     -> x86-64 assembly
//
// The parser is hand-written recursive descent. That is a choice, not a
// compromise: C's grammar is small enough to read directly off the standard,
// and a hand-written parser can point at the exact column that broke, which a
// generated one does not without a lot of extra work.
//
// The target is x86-64 Windows -- the Microsoft x64 calling convention --
// because that is the machine this was written on, and a compiler whose output
// you cannot run is a compiler you cannot test. Every ABI difference from
// System V that actually bites is called out in codegen.c.

#ifndef CINDER_H
#define CINDER_H

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __GNUC__
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif

typedef struct Type Type;
typedef struct Node Node;
typedef struct Member Member;
typedef struct Relocation Relocation;
typedef struct Hideset Hideset;
typedef struct Token Token;

//
// util.c -- small shared helpers
//

// StrArray is a growable array of strings: include paths, and the argument
// vector handed to the assembler.
typedef struct {
  char **data;
  int capacity;
  int len;
} StrArray;

void strarray_push(StrArray *arr, char *s);
char *format(char *fmt, ...);
void *xcalloc(size_t n, size_t size);
char *xstrndup(char *p, int len);

// HashMap is open-addressed with linear probing. A hash map rather than the
// linked list a small compiler can get away with, because identifier and macro
// lookup happens on nearly every token, and a list makes the compiler
// quadratic in the size of the scope.
typedef struct {
  char *key;
  int keylen;
  void *val;
} HashEntry;

typedef struct {
  HashEntry *buckets;
  int capacity;
  int used;
} HashMap;

void *hashmap_get(HashMap *map, char *key);
void *hashmap_get2(HashMap *map, char *key, int keylen);
void hashmap_put(HashMap *map, char *key, void *val);
void hashmap_put2(HashMap *map, char *key, int keylen, void *val);
void hashmap_delete(HashMap *map, char *key);
void hashmap_delete2(HashMap *map, char *key, int keylen);

//
// tokenize.c
//

typedef enum {
  TK_IDENT,   // identifiers
  TK_PUNCT,   // punctuators
  TK_KEYWORD, // keywords
  TK_STR,     // string literals
  TK_NUM,     // numeric literals
  TK_PP_NUM,  // preprocessing numbers, before they are given a type
  TK_EOF,     // end of input
} TokenKind;

typedef struct {
  char *name;
  int file_no;
  char *contents;
  char *display_name;
} File;

struct Token {
  TokenKind kind;
  Token *next;
  int64_t val;  // TK_NUM, integer value
  double fval;  // TK_NUM, floating value
  char *loc;    // start of the token, inside file->contents
  int len;
  Type *ty;     // TK_NUM and TK_STR: the literal's type
  char *str;    // TK_STR: contents, including the terminating NUL

  File *file;
  int line_no;
  bool at_bol;      // first token on its line
  bool has_space;   // preceded by whitespace
  Hideset *hideset; // macros already expanded here, which is what stops
                    // a macro from expanding itself forever
};

// These report and exit. The `_at` and `_tok` forms print the offending source
// line with a caret under the column, which is most of what makes a compiler's
// diagnostics usable rather than merely present.
NORETURN void error(char *fmt, ...);
NORETURN void error_at(char *loc, char *fmt, ...);
NORETURN void error_tok(Token *tok, char *fmt, ...);
void warn_tok(Token *tok, char *fmt, ...);

bool equal(Token *tok, char *op);
Token *skip(Token *tok, char *op);
bool consume(Token **rest, Token *tok, char *str);
Token *copy_token(Token *tok);
Token *new_eof(Token *tok);
void convert_pp_tokens(Token *tok);
File *new_file(char *name, int file_no, char *contents);
File **get_input_files(void);
Token *tokenize(File *file);
Token *tokenize_file(char *filename);

#define unreachable() error("internal error at %s:%d", __FILE__, __LINE__)

//
// preprocess.c
//

char *search_include_paths(char *filename);
void init_macros(void);
void define_cli_macro(char *arg);
void undef_cli_macro(char *name);
Token *preprocess(Token *tok);

//
// parse.c
//

// Obj is a variable or a function. They share a representation because most of
// what the parser does with them is identical; `is_local` and `is_function`
// pick out the differences.
typedef struct Obj Obj;
struct Obj {
  Obj *next;
  char *name;
  Type *ty;
  Token *tok;
  bool is_local;
  int align;

  // local variable: offset from the frame pointer
  int offset;

  // global variable or function
  bool is_function;
  bool is_definition;
  bool is_static;

  // global variable
  bool is_tentative;
  char *init_data;
  Relocation *rel;

  // function
  Obj *params;
  Node *body;
  Obj *locals;
  Obj *va_area;
  int stack_size;
  int max_call_args; // widest outgoing argument list, for the frame
};

// Relocation records that a global's initialiser refers to another global, as
// in `int *p = &x;`. The address is not known until link time, so the
// assembler is handed a symbol and an addend rather than a number.
struct Relocation {
  Relocation *next;
  int offset;
  char **label;
  long addend;
};

typedef enum {
  ND_NULL_EXPR, // do nothing
  ND_ADD,       // +
  ND_SUB,       // -
  ND_MUL,       // *
  ND_DIV,       // /
  ND_NEG,       // unary -
  ND_MOD,       // %
  ND_BITAND,    // &
  ND_BITOR,     // |
  ND_BITXOR,    // ^
  ND_SHL,       // <<
  ND_SHR,       // >>
  ND_EQ,        // ==
  ND_NE,        // !=
  ND_LT,        // <
  ND_LE,        // <=
  ND_ASSIGN,    // =
  ND_COND,      // ?:
  ND_COMMA,     // ,
  ND_MEMBER,    // . and ->
  ND_ADDR,      // unary &
  ND_DEREF,     // unary *
  ND_NOT,       // !
  ND_BITNOT,    // ~
  ND_LOGAND,    // &&
  ND_LOGOR,     // ||
  ND_RETURN,    // return
  ND_IF,        // if
  ND_FOR,       // for and while
  ND_DO,        // do-while
  ND_SWITCH,    // switch
  ND_CASE,      // case and default
  ND_BLOCK,     // { ... }
  ND_GOTO,      // goto
  ND_LABEL,     // labelled statement
  ND_FUNCALL,   // function call
  ND_EXPR_STMT, // expression statement
  ND_STMT_EXPR, // GNU statement expression, ({ ... })
  ND_VAR,       // variable reference
  ND_NUM,       // numeric literal
  ND_CAST,      // type cast
  ND_MEMZERO,   // zero-fill a local, for partial initialisers
} NodeKind;

struct Node {
  NodeKind kind;
  Node *next;
  Type *ty;   // the expression's type; null for statements
  Token *tok; // representative token, for diagnostics

  Node *lhs;
  Node *rhs;

  // if, for, while, do, switch
  Node *cond;
  Node *then;
  Node *els;
  Node *init;
  Node *inc;

  // break and continue jump to these. Carrying them on the node rather than in
  // a global stack is what makes nested loops and switches fall out for free.
  char *brk_label;
  char *cont_label;

  // block and statement expression
  Node *body;

  // struct or union member access
  Member *member;

  // function call
  Type *func_ty;
  Node *args;
  Obj *ret_buffer; // for a call returning a struct too large for a register

  // goto and labelled statement
  char *label;
  char *unique_label;
  Node *goto_next;

  // switch
  Node *case_next;
  Node *default_case;
  long begin; // case value

  // variable
  Obj *var;

  // numeric literal
  int64_t val;
  double fval;
};

Node *new_cast(Node *expr, Type *ty);
int64_t const_expr(Token **rest, Token *tok);
Obj *parse(Token *tok);

//
// type.c
//

typedef enum {
  TY_VOID,
  TY_BOOL,
  TY_CHAR,
  TY_SHORT,
  TY_INT,
  TY_LONG,  // 4 bytes: Windows is LLP64, not LP64
  TY_LLONG, // 8 bytes
  TY_FLOAT,
  TY_DOUBLE,
  TY_ENUM,
  TY_PTR,
  TY_FUNC,
  TY_ARRAY,
  TY_STRUCT,
  TY_UNION,
} TypeKind;

struct Type {
  TypeKind kind;
  int size;
  int align;
  bool is_unsigned;
  Type *origin; // the type this was copied from, for compatibility checks

  // A pointer and an array are both "something to base" as far as the code
  // generator is concerned; they differ only in whether the value is the
  // address itself or the object at it. Sharing `base` is what makes pointer
  // arithmetic and array subscripting the same code path.
  Type *base;

  // declarator
  Token *name;
  Token *name_pos;

  // array
  int array_len;

  // struct or union
  Member *members;
  bool is_flexible;

  // function
  Type *return_ty;
  Type *params;
  bool is_variadic;
  Type *next;
};

struct Member {
  Member *next;
  Type *ty;
  Token *tok;
  Token *name;
  int idx;
  int align;
  int offset;
};

extern Type *ty_void;
extern Type *ty_bool;
extern Type *ty_char;
extern Type *ty_short;
extern Type *ty_int;
extern Type *ty_long;
extern Type *ty_llong;
extern Type *ty_uchar;
extern Type *ty_ushort;
extern Type *ty_uint;
extern Type *ty_ulong;
extern Type *ty_ullong;
extern Type *ty_float;
extern Type *ty_double;

bool is_integer(Type *ty);
bool is_flonum(Type *ty);
bool is_numeric(Type *ty);
bool is_compatible(Type *t1, Type *t2);
Type *copy_type(Type *ty);
Type *pointer_to(Type *base);
Type *func_type(Type *return_ty);
Type *array_of(Type *base, int len);
Type *enum_type(void);
Type *struct_type(void);
void add_type(Node *node);

//
// codegen.c
//

void codegen(Obj *prog, FILE *out);
int align_to(int n, int align);

//
// main.c
//

extern StrArray include_paths;
extern char *base_file;

#endif // CINDER_H
