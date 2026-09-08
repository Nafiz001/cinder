// The preprocessor.
//
// It runs on the token list, not on text. That is what the standard describes
// and it is also the only way to get `##` right: pasting operates on tokens,
// so `a ## b` has to produce one token that is then re-lexed, and a text-based
// implementation cannot tell where the boundaries were.
//
// The subtle part is recursion. A macro that mentions itself must expand once
// and then stop, or `#define f f(x)` never terminates. The standard's answer is
// a "hide set": every token carries the set of macros that were being expanded
// when it was produced, and a macro will not expand a token that already has
// its own name in that set. Function-like macros need the hide set of the
// closing parenthesis intersected in as well, which is the rule that makes
//
//   #define f(x) x
//   f(f)(1)
//
// expand the way the standard says rather than infinitely.

#include "cinder.h"

typedef struct MacroParam MacroParam;
struct MacroParam {
  MacroParam *next;
  char *name;
};

typedef struct MacroArg MacroArg;
struct MacroArg {
  MacroArg *next;
  char *name;
  bool is_va_args;
  Token *tok;
};

typedef struct Macro Macro;
typedef Token *macro_handler_fn(Token *);

struct Macro {
  char *name;
  bool is_objlike; // an object-like macro takes no arguments
  MacroParam *params;
  char *va_args_name;
  Token *body;

  // A macro whose value depends on where it is used, such as __LINE__, cannot
  // be stored as a token list. It is computed at each point of expansion.
  macro_handler_fn *handler;
};

// A conditional inclusion currently being processed. They nest, so this is a
// stack.
typedef struct CondIncl CondIncl;
struct CondIncl {
  CondIncl *next;
  enum { IN_THEN, IN_ELIF, IN_ELSE } ctx;
  Token *tok;
  bool included;
};

struct Hideset {
  Hideset *next;
  char *name;
};

static HashMap macros;
static CondIncl *cond_incl;

// Files already included that carry an include guard, so that a second
// #include of the same header can be skipped without re-reading it.
static HashMap pragma_once;

static Token *preprocess2(Token *tok);
static Macro *find_macro(Token *tok);

static bool is_hash(Token *tok) { return tok->at_bol && equal(tok, "#"); }

// A directive occupies one line. After reading one, everything up to the
// newline must be consumed; some directives ignore trailing tokens, and
// skipping them here keeps every caller from having to.
static Token *skip_line(Token *tok) {
  if (tok->at_bol)
    return tok;
  warn_tok(tok, "extra tokens after a preprocessor directive");
  while (!tok->at_bol)
    tok = tok->next;
  return tok;
}

static Token *new_num_token(int val, Token *tmpl) {
  char *buf = format("%d\n", val);
  File *f = new_file(tmpl->file->name, tmpl->file->file_no, buf);
  Token *tok = tokenize(f);
  tok->at_bol = tmpl->at_bol;
  tok->has_space = tmpl->has_space;
  return tok;
}

static Token *append(Token *tok1, Token *tok2) {
  if (!tok1 || tok1->kind == TK_EOF)
    return tok2;

  Token head = {0};
  Token *cur = &head;

  for (; tok1 && tok1->kind != TK_EOF; tok1 = tok1->next)
    cur = cur->next = copy_token(tok1);
  cur->next = tok2;
  return head.next;
}

//
// Hide sets
//

static Hideset *new_hideset(char *name) {
  Hideset *hs = xcalloc(1, sizeof(Hideset));
  hs->name = name;
  return hs;
}

static Hideset *hideset_union(Hideset *hs1, Hideset *hs2) {
  Hideset head = {0};
  Hideset *cur = &head;

  for (; hs1; hs1 = hs1->next)
    cur = cur->next = new_hideset(hs1->name);
  cur->next = hs2;
  return head.next;
}

static bool hideset_contains(Hideset *hs, char *s, int len) {
  for (; hs; hs = hs->next)
    if (strlen(hs->name) == len && !strncmp(hs->name, s, len))
      return true;
  return false;
}

static Hideset *hideset_intersection(Hideset *hs1, Hideset *hs2) {
  Hideset head = {0};
  Hideset *cur = &head;

  for (; hs1; hs1 = hs1->next)
    if (hideset_contains(hs2, hs1->name, strlen(hs1->name)))
      cur = cur->next = new_hideset(hs1->name);
  return head.next;
}

static Token *add_hideset(Token *tok, Hideset *hs) {
  Token head = {0};
  Token *cur = &head;

  for (; tok; tok = tok->next) {
    Token *t = copy_token(tok);
    t->hideset = hideset_union(t->hideset, hs);
    cur = cur->next = t;
  }
  return head.next;
}

//
// Conditional inclusion
//

// Skip to the matching #endif or #else, tracking nesting so that a nested
// #if inside a false branch does not terminate the outer one early.
static Token *skip_cond_incl2(Token *tok) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) &&
        (equal(tok->next, "if") || equal(tok->next, "ifdef") ||
         equal(tok->next, "ifndef"))) {
      tok = skip_cond_incl2(tok->next->next);
      continue;
    }
    if (is_hash(tok) && equal(tok->next, "endif"))
      return tok->next->next;
    tok = tok->next;
  }
  return tok;
}

static Token *skip_cond_incl(Token *tok) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) &&
        (equal(tok->next, "if") || equal(tok->next, "ifdef") ||
         equal(tok->next, "ifndef"))) {
      tok = skip_cond_incl2(tok->next->next);
      continue;
    }

    if (is_hash(tok) &&
        (equal(tok->next, "elif") || equal(tok->next, "else") ||
         equal(tok->next, "endif")))
      break;
    tok = tok->next;
  }
  return tok;
}

static CondIncl *push_cond_incl(Token *tok, bool included) {
  CondIncl *ci = xcalloc(1, sizeof(CondIncl));
  ci->next = cond_incl;
  ci->ctx = IN_THEN;
  ci->tok = tok;
  ci->included = included;
  cond_incl = ci;
  return ci;
}

// Copy one line of tokens, terminated by an EOF marker, so that the constant
// expression parser has something self-contained to chew on.
static Token *copy_line(Token **rest, Token *tok) {
  Token head = {0};
  Token *cur = &head;

  for (; !tok->at_bol; tok = tok->next)
    cur = cur->next = copy_token(tok);

  cur->next = new_eof(tok);
  *rest = tok;
  return head.next;
}

// read_const_expr evaluates the expression after #if.
//
// Two rewrites happen before it is parsed. `defined(x)` becomes 1 or 0, and
// then every identifier that survived macro expansion becomes 0 -- which is
// what makes `#if UNDEFINED_THING` false rather than an error.
static Token *read_const_expr(Token **rest, Token *tok) {
  tok = copy_line(rest, tok);

  Token head = {0};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    // `defined(x)` and `defined x` are both legal spellings.
    if (equal(tok, "defined")) {
      Token *start = tok;
      bool has_paren = consume(&tok, tok->next, "(");

      if (tok->kind != TK_IDENT)
        error_tok(start, "macro name must be an identifier");
      Macro *m = find_macro(tok);
      tok = tok->next;

      if (has_paren)
        tok = skip(tok, ")");

      cur = cur->next = new_num_token(m ? 1 : 0, start);
      continue;
    }

    cur = cur->next = tok;
    tok = tok->next;
  }

  cur->next = tok;
  return head.next;
}

static long eval_const_expr(Token **rest, Token *tok) {
  Token *start = tok;
  Token *expr = read_const_expr(rest, tok->next);
  expr = preprocess2(expr);

  if (expr->kind == TK_EOF)
    error_tok(start, "no expression after #%.*s", tok->len, tok->loc);

  // Any identifier left after expansion is not a macro, and the standard says
  // it evaluates to zero. This is why `#if __linux__` is false rather than a
  // diagnostic on a machine where it is not defined.
  for (Token *t = expr; t->kind != TK_EOF; t = t->next) {
    if (t->kind == TK_IDENT) {
      Token *next = t->next;
      *t = *new_num_token(0, t);
      t->next = next;
    }
  }

  convert_pp_tokens(expr);

  Token *rest2;
  long val = const_expr(&rest2, expr);
  if (rest2->kind != TK_EOF)
    error_tok(rest2, "extra tokens after the expression");

  return val;
}

//
// Macro expansion
//

static Macro *find_macro(Token *tok) {
  if (tok->kind != TK_IDENT)
    return NULL;
  return hashmap_get2(&macros, tok->loc, tok->len);
}

static Macro *add_macro(char *name, bool is_objlike, Token *body) {
  Macro *m = xcalloc(1, sizeof(Macro));
  m->name = name;
  m->is_objlike = is_objlike;
  m->body = body;
  hashmap_put(&macros, name, m);
  return m;
}

static MacroParam *read_macro_params(Token **rest, Token *tok,
                                     char **va_args_name) {
  MacroParam head = {0};
  MacroParam *cur = &head;

  while (!equal(tok, ")")) {
    if (cur != &head)
      tok = skip(tok, ",");

    if (equal(tok, "...")) {
      *va_args_name = "__VA_ARGS__";
      *rest = skip(tok->next, ")");
      return head.next;
    }

    if (tok->kind != TK_IDENT)
      error_tok(tok, "expected a macro parameter name");

    // A named variadic parameter is a GNU extension: `#define f(args...)`.
    if (equal(tok->next, "...")) {
      *va_args_name = xstrndup(tok->loc, tok->len);
      *rest = skip(tok->next->next, ")");
      return head.next;
    }

    MacroParam *m = xcalloc(1, sizeof(MacroParam));
    m->name = xstrndup(tok->loc, tok->len);
    cur = cur->next = m;
    tok = tok->next;
  }

  *rest = tok->next;
  return head.next;
}

static void read_macro_definition(Token **rest, Token *tok) {
  if (tok->kind != TK_IDENT)
    error_tok(tok, "a macro name must be an identifier");
  char *name = xstrndup(tok->loc, tok->len);
  tok = tok->next;

  // The distinction between `#define f(x) ...` and `#define f (x) ...` is the
  // space before the paren, and nothing else. This is exactly why every token
  // carries has_space.
  if (!tok->has_space && equal(tok, "(")) {
    char *va_args_name = NULL;
    MacroParam *params = read_macro_params(&tok, tok->next, &va_args_name);

    Macro *m = add_macro(name, false, copy_line(rest, tok));
    m->params = params;
    m->va_args_name = va_args_name;
  } else {
    add_macro(name, true, copy_line(rest, tok));
  }
}

// Read one macro argument: tokens up to a comma or the closing paren, with
// nested parentheses tracked so that `f((a,b))` passes one argument.
static MacroArg *read_macro_arg_one(Token **rest, Token *tok, bool read_rest) {
  Token head = {0};
  Token *cur = &head;
  int level = 0;

  for (;;) {
    if (level == 0 && equal(tok, ")"))
      break;
    if (level == 0 && !read_rest && equal(tok, ","))
      break;

    if (tok->kind == TK_EOF)
      error_tok(tok, "unterminated macro argument list");

    if (equal(tok, "("))
      level++;
    else if (equal(tok, ")"))
      level--;

    cur = cur->next = copy_token(tok);
    tok = tok->next;
  }

  cur->next = new_eof(tok);

  MacroArg *arg = xcalloc(1, sizeof(MacroArg));
  arg->tok = head.next;
  *rest = tok;
  return arg;
}

static MacroArg *read_macro_args(Token **rest, Token *tok, MacroParam *params,
                                 char *va_args_name) {
  Token *start = tok;
  tok = tok->next->next; // skip the macro name and the '('

  MacroArg head = {0};
  MacroArg *cur = &head;

  MacroParam *pp = params;
  for (; pp; pp = pp->next) {
    if (cur != &head)
      tok = skip(tok, ",");
    cur = cur->next = read_macro_arg_one(&tok, tok, false);
    cur->name = pp->name;
  }

  if (va_args_name) {
    MacroArg *arg;
    if (equal(tok, ")")) {
      // A variadic macro called with no variable arguments still needs an
      // empty __VA_ARGS__ to exist.
      arg = xcalloc(1, sizeof(MacroArg));
      arg->tok = new_eof(tok);
    } else {
      if (pp != params)
        tok = skip(tok, ",");
      arg = read_macro_arg_one(&tok, tok, true);
    }
    arg->name = va_args_name;
    arg->is_va_args = true;
    cur = cur->next = arg;
  } else if (pp) {
    error_tok(start, "too few arguments for macro '%.*s'", start->len,
              start->loc);
  }

  skip(tok, ")");
  *rest = tok;
  return head.next;
}

static MacroArg *find_arg(MacroArg *args, Token *tok) {
  for (MacroArg *ap = args; ap; ap = ap->next)
    if (tok->len == strlen(ap->name) && !strncmp(tok->loc, ap->name, tok->len))
      return ap;
  return NULL;
}

// Concatenate the source text of a token run, which is what `#` produces.
static char *join_tokens(Token *tok, Token *end) {
  int len = 1;
  for (Token *t = tok; t != end && t->kind != TK_EOF; t = t->next) {
    if (t != tok && t->has_space)
      len++;
    len += t->len;
  }

  char *buf = xcalloc(1, len);

  int pos = 0;
  for (Token *t = tok; t != end && t->kind != TK_EOF; t = t->next) {
    if (t != tok && t->has_space)
      buf[pos++] = ' ';
    strncpy(buf + pos, t->loc, t->len);
    pos += t->len;
  }
  buf[pos] = '\0';
  return buf;
}

// `#arg` turns the argument into a string literal. Quotes and backslashes
// inside it have to be escaped, or `#` on a string argument produces something
// that will not re-lex.
static Token *stringize(Token *hash, Token *arg) {
  char *s = join_tokens(arg, NULL);

  int len = 3;
  for (char *p = s; *p; p++)
    len += (*p == '"' || *p == '\\') ? 2 : 1;

  char *buf = xcalloc(1, len + 1);
  int pos = 0;
  buf[pos++] = '"';
  for (char *p = s; *p; p++) {
    if (*p == '"' || *p == '\\')
      buf[pos++] = '\\';
    buf[pos++] = *p;
  }
  buf[pos++] = '"';
  buf[pos++] = '\n';
  buf[pos] = '\0';

  File *f = new_file(hash->file->name, hash->file->file_no, buf);
  Token *tok = tokenize(f);
  tok->at_bol = hash->at_bol;
  tok->has_space = hash->has_space;
  return tok;
}

// `lhs ## rhs` glues the two spellings together and re-lexes the result, which
// must come out as exactly one token.
static Token *paste(Token *lhs, Token *rhs) {
  char *buf = format("%.*s%.*s\n", lhs->len, lhs->loc, rhs->len, rhs->loc);

  File *f = new_file(lhs->file->name, lhs->file->file_no, buf);
  Token *tok = tokenize(f);
  if (tok->next->kind != TK_EOF)
    error_tok(lhs, "pasting formed '%s', which is not a valid token", buf);
  return tok;
}

static bool has_varargs(MacroArg *args) {
  for (MacroArg *ap = args; ap; ap = ap->next)
    if (!strcmp(ap->name, "__VA_ARGS__"))
      return ap->tok->kind != TK_EOF;
  return false;
}

// Substitute arguments into the macro body, applying `#` and `##`.
static Token *subst(Token *tok, MacroArg *args) {
  Token head = {0};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    if (equal(tok, "#") && tok->next) {
      MacroArg *arg = find_arg(args, tok->next);
      if (arg) {
        cur = cur->next = stringize(tok, arg->tok);
        tok = tok->next->next;
        continue;
      }
    }

    // `, ## __VA_ARGS__` is a GNU extension: the comma disappears when the
    // variable argument list is empty. Without it, every logging macro that
    // takes optional arguments breaks on the zero-argument case.
    if (equal(tok, ",") && equal(tok->next, "##")) {
      MacroArg *arg = find_arg(args, tok->next->next);
      if (arg && arg->is_va_args) {
        if (arg->tok->kind == TK_EOF) {
          tok = tok->next->next->next;
        } else {
          cur = cur->next = copy_token(tok);
          tok = tok->next->next;
        }
        continue;
      }
    }

    if (equal(tok, "##") && cur != &head) {
      if (tok->next->kind == TK_EOF)
        error_tok(tok, "'##' cannot appear at the end of a macro body");

      MacroArg *arg = find_arg(args, tok->next);
      if (arg) {
        if (arg->tok->kind != TK_EOF) {
          *cur = *paste(cur, arg->tok);
          for (Token *t = arg->tok->next; t->kind != TK_EOF; t = t->next)
            cur = cur->next = copy_token(t);
        }
        tok = tok->next->next;
        continue;
      }

      *cur = *paste(cur, tok->next);
      tok = tok->next->next;
      continue;
    }

    MacroArg *arg = find_arg(args, tok);

    if (arg && equal(tok->next, "##")) {
      Token *rhs = tok->next->next;

      if (arg->tok->kind == TK_EOF) {
        // An empty argument on the left of ## leaves the right side alone.
        MacroArg *arg2 = find_arg(args, rhs);
        if (arg2) {
          for (Token *t = arg2->tok; t->kind != TK_EOF; t = t->next)
            cur = cur->next = copy_token(t);
        } else {
          cur = cur->next = copy_token(rhs);
        }
        tok = rhs->next;
        continue;
      }

      for (Token *t = arg->tok; t->kind != TK_EOF; t = t->next)
        cur = cur->next = copy_token(t);
      tok = tok->next;
      continue;
    }

    if (arg) {
      // An argument is fully macro-expanded before substitution, unless it is
      // an operand of # or ##. That ordering is what the standard specifies
      // and it is observable.
      Token *t = preprocess2(arg->tok);
      t->at_bol = tok->at_bol;
      t->has_space = tok->has_space;
      for (; t->kind != TK_EOF; t = t->next)
        cur = cur->next = copy_token(t);
      tok = tok->next;
      continue;
    }

    cur = cur->next = copy_token(tok);
    tok = tok->next;
  }

  cur->next = tok;
  return head.next;
}

// Try to expand the macro at `tok`. Returns false if it is not a macro, or if
// its own name is in the token's hide set.
static bool expand_macro(Token **rest, Token *tok) {
  if (hideset_contains(tok->hideset, tok->loc, tok->len))
    return false;

  Macro *m = find_macro(tok);
  if (!m)
    return false;

  if (m->handler) {
    *rest = m->handler(tok);
    (*rest)->next = tok->next;
    return true;
  }

  if (m->is_objlike) {
    Hideset *hs = hideset_union(tok->hideset, new_hideset(m->name));
    Token *body = add_hideset(m->body, hs);
    *rest = append(body, tok->next);
    (*rest)->at_bol = tok->at_bol;
    (*rest)->has_space = tok->has_space;
    return true;
  }

  // A function-like macro name not followed by '(' is just an identifier.
  if (!equal(tok->next, "("))
    return false;

  Token *macro_token = tok;
  MacroArg *args = read_macro_args(&tok, tok, m->params, m->va_args_name);
  Token *rparen = tok;

  // The hide set of the result is the intersection of the macro name's and the
  // closing parenthesis's, unioned with the macro itself. Intersecting is what
  // stops `f(f)(1)` from looping while still allowing legitimate re-expansion.
  Hideset *hs = hideset_intersection(macro_token->hideset, rparen->hideset);
  hs = hideset_union(hs, new_hideset(m->name));

  Token *body = subst(m->body, args);
  body = add_hideset(body, hs);
  *rest = append(body, tok->next);
  (*rest)->at_bol = macro_token->at_bol;
  (*rest)->has_space = macro_token->has_space;
  return true;
}

//
// #include
//

char *search_include_paths(char *filename) {
  // An absolute path, or one with a drive letter, is used as given.
  if (filename[0] == '/' || filename[0] == '\\' ||
      (filename[0] && filename[1] == ':'))
    return filename;

  for (int i = 0; i < include_paths.len; i++) {
    char *path = format("%s/%s", include_paths.data[i], filename);
    FILE *fp = fopen(path, "r");
    if (fp) {
      fclose(fp);
      return path;
    }
  }
  return NULL;
}

// Take the directory part of a path, so that `#include "x.h"` resolves
// relative to the file doing the including rather than the working directory.
static char *dirname_of(char *path) {
  char *slash = NULL;
  for (char *p = path; *p; p++)
    if (*p == '/' || *p == '\\')
      slash = p;
  if (!slash)
    return ".";
  return xstrndup(path, slash - path);
}

// `#include <a/b.h>` arrives as several tokens, because `<`, `a`, `/`, `b`,
// `.` and `h` all lex separately. They are re-joined from their source text,
// which is why the original spacing has to be reconstructed.
static char *read_include_filename(Token **rest, Token *tok, bool *is_angle) {
  if (tok->kind == TK_STR) {
    // A quoted name is not an ordinary string literal: escape sequences are
    // not processed, so the raw text between the quotes is used.
    *is_angle = false;
    *rest = skip_line(tok->next);
    return xstrndup(tok->loc + 1, tok->len - 2);
  }

  if (equal(tok, "<")) {
    Token *start = tok;

    for (; !equal(tok, ">"); tok = tok->next)
      if (tok->at_bol || tok->kind == TK_EOF)
        error_tok(tok, "expected '>'");

    *is_angle = true;
    *rest = skip_line(tok->next);
    return join_tokens(start->next, tok);
  }

  // `#include MACRO` -- expand it and try again.
  if (tok->kind == TK_IDENT) {
    Token *tok2 = preprocess2(copy_line(rest, tok));
    return read_include_filename(&tok2, tok2, is_angle);
  }

  error_tok(tok, "expected a filename");
}

static Token *include_file(Token *tok, char *path, Token *filename_tok) {
  // #pragma once, and the include-guard optimisation, both land here.
  if (hashmap_get(&pragma_once, path))
    return tok;

  Token *tok2 = tokenize_file(path);
  if (!tok2)
    error_tok(filename_tok, "cannot open %s: %s", path, strerror(errno));

  return append(tok2, tok);
}

//
// The main loop
//

static Token *preprocess2(Token *tok) {
  Token head = {0};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    if (expand_macro(&tok, tok))
      continue;

    if (!is_hash(tok)) {
      cur = cur->next = tok;
      tok = tok->next;
      continue;
    }

    Token *start = tok;
    tok = tok->next;

    if (equal(tok, "include")) {
      bool is_angle;
      Token *filename_tok = tok->next;
      char *filename = read_include_filename(&tok, tok->next, &is_angle);

      if (!is_angle) {
        // A quoted include looks next to the including file first.
        char *path =
            format("%s/%s", dirname_of(start->file->name), filename);
        FILE *fp = fopen(path, "r");
        if (fp) {
          fclose(fp);
          tok = include_file(tok, path, filename_tok);
          continue;
        }
      }

      char *path = search_include_paths(filename);
      tok = include_file(tok, path ? path : filename, filename_tok);
      continue;
    }

    if (equal(tok, "define")) {
      read_macro_definition(&tok, tok->next);
      continue;
    }

    if (equal(tok, "undef")) {
      tok = tok->next;
      if (tok->kind != TK_IDENT)
        error_tok(tok, "a macro name must be an identifier");
      hashmap_delete2(&macros, tok->loc, tok->len);
      tok = skip_line(tok->next);
      continue;
    }

    if (equal(tok, "if")) {
      long val = eval_const_expr(&tok, tok);
      push_cond_incl(start, val);
      if (!val)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "ifdef")) {
      bool defined = find_macro(tok->next);
      push_cond_incl(tok, defined);
      tok = skip_line(tok->next->next);
      if (!defined)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "ifndef")) {
      bool defined = find_macro(tok->next);
      push_cond_incl(tok, !defined);
      tok = skip_line(tok->next->next);
      if (defined)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "elif")) {
      if (!cond_incl || cond_incl->ctx == IN_ELSE)
        error_tok(start, "stray #elif");
      cond_incl->ctx = IN_ELIF;

      // Once a branch has been taken, every later one is skipped without
      // evaluating its condition -- which matters, because that condition may
      // not even be well-formed.
      if (!cond_incl->included && eval_const_expr(&tok, tok))
        cond_incl->included = true;
      else
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "else")) {
      if (!cond_incl || cond_incl->ctx == IN_ELSE)
        error_tok(start, "stray #else");
      cond_incl->ctx = IN_ELSE;
      tok = skip_line(tok->next);

      if (cond_incl->included)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "endif")) {
      if (!cond_incl)
        error_tok(start, "stray #endif");
      cond_incl = cond_incl->next;
      tok = skip_line(tok->next);
      continue;
    }

    if (equal(tok, "error"))
      error_tok(start, "#error: %s", join_tokens(tok->next, NULL));

    if (equal(tok, "warning")) {
      warn_tok(start, "#warning: %s", join_tokens(tok->next, NULL));
      while (!tok->at_bol)
        tok = tok->next;
      continue;
    }

    if (equal(tok, "pragma") && equal(tok->next, "once")) {
      hashmap_put(&pragma_once, tok->file->name, (void *)1);
      tok = skip_line(tok->next->next);
      continue;
    }

    if (equal(tok, "pragma")) {
      // Any other pragma is ignored, which is what the standard permits and
      // what every compiler does for pragmas it does not know.
      do {
        tok = tok->next;
      } while (!tok->at_bol);
      continue;
    }

    if (equal(tok, "line") || tok->kind == TK_PP_NUM) {
      // #line and the "# 1 file" form left by another preprocessor. cinder
      // does not track the remapping, so the line is skipped rather than
      // producing wrong locations.
      while (!tok->at_bol)
        tok = tok->next;
      continue;
    }

    // A `#` alone on a line is a null directive and is legal.
    if (tok->at_bol)
      continue;

    error_tok(tok, "invalid preprocessor directive");
  }

  cur->next = tok;
  return head.next;
}

// Adjacent string literals are one string: "a" "b" is "ab". This is
// translation phase 6, and it has to happen after macro expansion, because
// most of the strings that end up adjacent were put there by a macro:
//
//   #define LOG(fmt, ...) printf("log: " fmt "\n", ##__VA_ARGS__)
//
// expands to three string tokens in a row, and without this step the parser
// sees a second string where it expected a comma.
static void join_adjacent_string_literals(Token *tok) {
  while (tok->kind != TK_EOF) {
    if (tok->kind != TK_STR || tok->next->kind != TK_STR) {
      tok = tok->next;
      continue;
    }

    // Measure the whole run before copying, so the buffer is allocated once.
    Token *end = tok;
    int len = 0;
    while (end->kind == TK_STR) {
      // array_len rather than strlen: a literal may contain an embedded NUL,
      // as "a\0b" does, and strlen would stop at it.
      len += end->ty->array_len - 1;
      end = end->next;
    }

    char *buf = xcalloc(1, len + 1);
    int pos = 0;
    for (Token *t = tok; t != end; t = t->next) {
      memcpy(buf + pos, t->str, t->ty->array_len - 1);
      pos += t->ty->array_len - 1;
    }
    buf[len] = '\0';

    tok->str = buf;
    tok->ty = array_of(ty_char, len + 1);
    tok->next = end;
    tok = end;
  }
}

// preprocess is the entry point. Keywords are recognised only here, after
// expansion, because until the macros are gone an identifier might still turn
// into one -- or stop being one.
Token *preprocess(Token *tok) {
  tok = preprocess2(tok);
  if (cond_incl)
    error_tok(cond_incl->tok, "unterminated conditional directive");
  convert_pp_tokens(tok);
  join_adjacent_string_literals(tok);
  return tok;
}

//
// Predefined macros
//

static void define_macro(char *name, char *buf) {
  char *text = format("%s\n", buf);
  File *f = new_file("<built-in>", 1, text);
  Token *tok = tokenize(f);
  add_macro(name, true, tok);
}

void define_cli_macro(char *arg) {
  char *eq = strchr(arg, '=');
  if (eq)
    define_macro(xstrndup(arg, eq - arg), eq + 1);
  else
    // `-DFOO` with no value defines it as 1, matching every other compiler.
    define_macro(arg, "1");
}

void undef_cli_macro(char *name) { hashmap_delete(&macros, name); }

// A macro whose value depends on where it is written cannot be stored as
// tokens: __LINE__ has to mean the line it appears on, not the line it was
// defined on. These are registered with a handler that runs at each expansion.
static Macro *add_builtin(char *name, macro_handler_fn *fn) {
  Macro *m = add_macro(name, true, NULL);
  m->handler = fn;
  return m;
}

static Token *file_macro(Token *tmpl) {
  char *name = tmpl->file->display_name;
  char *buf = format("\"%s\"\n", name);
  File *f = new_file(tmpl->file->name, tmpl->file->file_no, buf);
  return tokenize(f);
}

static Token *line_macro(Token *tmpl) {
  return new_num_token(tmpl->line_no, tmpl);
}

// __COUNTER__ yields a different number each time it is expanded, which is how
// a header generates identifiers that cannot collide.
static Token *counter_macro(Token *tmpl) {
  static int i = 0;
  return new_num_token(i++, tmpl);
}

void init_macros(void) {
  define_macro("_WIN32", "1");
  define_macro("_WIN64", "1");
  define_macro("__x86_64__", "1");
  define_macro("__STDC__", "1");
  define_macro("__STDC_VERSION__", "201112L");
  define_macro("__STDC_HOSTED__", "1");
  define_macro("__CINDER__", "1");

  // Sizes, so that a header can branch on them.
  define_macro("__SIZEOF_INT__", "4");
  define_macro("__SIZEOF_LONG__", "4");
  define_macro("__SIZEOF_LONG_LONG__", "8");
  define_macro("__SIZEOF_POINTER__", "8");

  add_builtin("__FILE__", file_macro);
  add_builtin("__LINE__", line_macro);
  add_builtin("__COUNTER__", counter_macro);
}
