// The lexer: source text in, a linked list of tokens out.
//
// Tokenizing C is less mechanical than it looks, and most of the subtlety is
// here rather than in the parser:
//
//   - Punctuators must be matched longest-first, or `>>=` lexes as `>` `>=`.
//   - A `.` may begin a number (`.5`) or be a member access, and `1.5e+10`
//     contains a `+` that is not an operator. The standard solves this with
//     "preprocessing numbers": lex something number-shaped now, decide what it
//     means later. That is what TK_PP_NUM is.
//   - Keywords are not recognised until after preprocessing, because until
//     macros are expanded you do not yet know whether a given identifier is
//     one.
//   - Every token records whether it began a line and whether whitespace
//     preceded it. The parser never looks at either; the preprocessor needs
//     both, since `#` only starts a directive at the beginning of a line and
//     `f (x)` is a different macro invocation from `f(x)` when defining.

#include "cinder.h"

static File *current_file;
static File **input_files; // every file read, for diagnostics
static bool at_bol;        // the next token begins a line
static bool has_space;     // whitespace has been seen since the last token

//
// Diagnostics
//

// verror_at prints the offending line with a caret under the column. Finding
// the line means scanning backwards to the previous newline, which is cheap
// and means tokens do not have to carry a column number.
static void verror_at(char *filename, char *input, int line_no, char *loc,
                      char *fmt, va_list ap) {
  char *line = loc;
  while (input < line && line[-1] != '\n')
    line--;

  char *end = loc;
  while (*end && *end != '\n')
    end++;

  int indent = fprintf(stderr, "%s:%d: ", filename, line_no);
  fprintf(stderr, "%.*s\n", (int)(end - line), line);

  int pos = 0;
  for (char *p = line; p < loc; p++)
    pos += (*p == '\t') ? 8 - (pos % 8) : 1;
  pos += indent;

  fprintf(stderr, "%*s", pos, "");
  fprintf(stderr, "^ ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
}

void error(char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

void error_at(char *loc, char *fmt, ...) {
  int line_no = 1;
  for (char *p = current_file->contents; p < loc; p++)
    if (*p == '\n')
      line_no++;

  va_list ap;
  va_start(ap, fmt);
  verror_at(current_file->name, current_file->contents, line_no, loc, fmt, ap);
  va_end(ap);
  exit(1);
}

void error_tok(Token *tok, char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  verror_at(tok->file->name, tok->file->contents, tok->line_no, tok->loc, fmt,
            ap);
  va_end(ap);
  exit(1);
}

void warn_tok(Token *tok, char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  verror_at(tok->file->name, tok->file->contents, tok->line_no, tok->loc, fmt,
            ap);
  va_end(ap);
}

//
// Token helpers
//

bool equal(Token *tok, char *op) {
  return memcmp(tok->loc, op, tok->len) == 0 && op[tok->len] == '\0';
}

Token *skip(Token *tok, char *op) {
  if (!equal(tok, op))
    error_tok(tok, "expected '%s'", op);
  return tok->next;
}

bool consume(Token **rest, Token *tok, char *str) {
  if (equal(tok, str)) {
    *rest = tok->next;
    return true;
  }
  *rest = tok;
  return false;
}

static Token *new_token(TokenKind kind, char *start, char *end) {
  Token *tok = xcalloc(1, sizeof(Token));
  tok->kind = kind;
  tok->loc = start;
  tok->len = end - start;
  tok->file = current_file;
  tok->at_bol = at_bol;
  tok->has_space = has_space;

  at_bol = has_space = false;
  return tok;
}

Token *copy_token(Token *tok) {
  Token *t = xcalloc(1, sizeof(Token));
  *t = *tok;
  t->next = NULL;
  return t;
}

Token *new_eof(Token *tok) {
  Token *t = copy_token(tok);
  t->kind = TK_EOF;
  t->len = 0;
  return t;
}

static bool startswith(char *p, char *q) {
  return strncmp(p, q, strlen(q)) == 0;
}

// C identifiers allow `$` in most implementations; cinder does not, to keep
// what it accepts to what the standard describes.
static bool is_ident1(char c) {
  return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || c == '_';
}

static bool is_ident2(char c) { return is_ident1(c) || ('0' <= c && c <= '9'); }

static int from_hex(char c) {
  if ('0' <= c && c <= '9')
    return c - '0';
  if ('a' <= c && c <= 'f')
    return c - 'a' + 10;
  return c - 'A' + 10;
}

// Punctuators, longest first. The order is the whole algorithm: `<<=` has to
// be tried before `<<`, which has to be tried before `<`.
static int read_punct(char *p) {
  static char *kw[] = {
      "<<=", ">>=", "...", "==", "!=", "<=", ">=", "->", "+=", "-=",
      "*=",  "/=",  "%=",  "&=", "|=", "^=", "&&", "||", "<<", ">>",
      "++",  "--",  "##",
  };

  for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
    if (startswith(p, kw[i]))
      return strlen(kw[i]);

  return ispunct((unsigned char)*p) ? 1 : 0;
}

static bool is_keyword(Token *tok) {
  static HashMap map;

  if (map.capacity == 0) {
    static char *kw[] = {
        "return",   "if",       "else",     "for",      "while",
        "int",      "sizeof",   "char",     "struct",   "union",
        "short",    "long",     "void",     "typedef",  "_Bool",
        "enum",     "static",   "goto",     "break",    "continue",
        "switch",   "case",     "default",  "extern",   "do",
        "signed",   "unsigned", "const",    "volatile", "auto",
        "register", "restrict", "float",    "double",   "inline",
        "_Alignof", "_Alignas", "_Noreturn",
    };
    for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
      hashmap_put(&map, kw[i], (void *)1);
  }

  return hashmap_get2(&map, tok->loc, tok->len) != NULL;
}

//
// Character and string literals
//

static int read_escaped_char(char **new_pos, char *p) {
  if ('0' <= *p && *p <= '7') {
    // Octal, at most three digits.
    int c = *p++ - '0';
    if ('0' <= *p && *p <= '7') {
      c = (c << 3) + (*p++ - '0');
      if ('0' <= *p && *p <= '7')
        c = (c << 3) + (*p++ - '0');
    }
    *new_pos = p;
    return c;
  }

  if (*p == 'x') {
    // Hexadecimal, unbounded length, which is what the standard says.
    p++;
    if (!isxdigit((unsigned char)*p))
      error_at(p, "\\x used with no following hex digits");

    int c = 0;
    for (; isxdigit((unsigned char)*p); p++)
      c = (c << 4) + from_hex(*p);
    *new_pos = p;
    return c;
  }

  *new_pos = p + 1;

  switch (*p) {
  case 'a': return '\a';
  case 'b': return '\b';
  case 't': return '\t';
  case 'n': return '\n';
  case 'v': return '\v';
  case 'f': return '\f';
  case 'r': return '\r';
  // \e is a GNU extension for the escape character. Widely used, and harmless
  // to accept.
  case 'e': return 27;
  default:  return *p;
  }
}

// Find the closing quote, skipping escapes. This is separate from reading the
// contents so that an unterminated literal is reported at the opening quote,
// where the mistake actually is, rather than at the end of the file.
static char *string_literal_end(char *p) {
  char *start = p;
  for (; *p != '"'; p++) {
    if (*p == '\n' || *p == '\0')
      error_at(start, "unterminated string literal");
    if (*p == '\\')
      p++;
  }
  return p;
}

static Token *read_string_literal(char *start, char *quote) {
  char *end = string_literal_end(quote + 1);
  char *buf = xcalloc(1, end - quote);
  int len = 0;

  for (char *p = quote + 1; p < end;) {
    if (*p == '\\')
      buf[len++] = read_escaped_char(&p, p + 1);
    else
      buf[len++] = *p++;
  }

  Token *tok = new_token(TK_STR, start, end + 1);
  tok->ty = array_of(ty_char, len + 1);
  tok->str = buf;
  return tok;
}

static Token *read_char_literal(char *start, char *quote) {
  char *p = quote + 1;
  if (*p == '\0')
    error_at(start, "unterminated character literal");

  int c;
  if (*p == '\\')
    c = read_escaped_char(&p, p + 1);
  else
    c = (unsigned char)*p++;

  char *end = strchr(p, '\'');
  if (!end)
    error_at(start, "unterminated character literal");

  Token *tok = new_token(TK_NUM, start, end + 1);
  // A character constant has type int in C, not char. This matters: it is why
  // sizeof('a') is 4 and not 1, which surprises people often enough that it is
  // worth the comment.
  tok->val = (char)c;
  tok->ty = ty_int;
  return tok;
}

//
// Numbers
//

static bool convert_pp_int(Token *tok) {
  char *p = tok->loc;

  int base = 10;
  if (!strncasecmp(p, "0x", 2) && isxdigit((unsigned char)p[2])) {
    p += 2;
    base = 16;
  } else if (!strncasecmp(p, "0b", 2) && (p[2] == '0' || p[2] == '1')) {
    // Binary literals are a GNU extension, standardised in C23.
    p += 2;
    base = 2;
  } else if (*p == '0') {
    base = 8;
  }

  int64_t val = strtoull(p, &p, base);

  // Suffixes, in any order and any case. `l` and `ll` have to be tracked
  // separately: on Windows `long` is 32 bits and `long long` is 64, so 1L and
  // 1LL are genuinely different types.
  bool l = false, ll = false, u = false;

  for (;;) {
    if (!strncasecmp(p, "ll", 2)) {
      p += 2;
      ll = true;
    } else if (*p == 'L' || *p == 'l') {
      p++;
      l = true;
    } else if (*p == 'U' || *p == 'u') {
      p++;
      u = true;
    } else {
      break;
    }
  }

  if (p != tok->loc + tok->len)
    return false; // not an integer after all; try it as a float

  // The type of an unsuffixed constant is the first of int, long, ... that can
  // represent it. A decimal constant never becomes unsigned implicitly; a hex
  // or octal one can, which is why 0xffffffff is unsigned int and 4294967295
  // is long.
  Type *ty;
  if (ll)
    ty = u ? ty_ullong : ty_llong;
  else if (l)
    // `long` is 32 bits here, so a value too wide for it becomes long long --
    // which is what "the first type that can represent it" means on LLP64.
    ty = u ? (val >> 32 ? ty_ullong : ty_ulong)
           : (val >> 31 ? ty_llong : ty_long);
  else if (u)
    ty = (val >> 32) ? ty_ullong : ty_uint;
  else if (base == 10)
    // A decimal constant never becomes unsigned implicitly; a hex or octal one
    // can, which is why 0xffffffff is unsigned int and 4294967295 is long long.
    ty = (val >> 31) ? ty_llong : ty_int;
  else if (val >> 63)
    ty = ty_ullong;
  else if (val >> 32)
    ty = ty_llong;
  else if (val >> 31)
    ty = ty_uint;
  else
    ty = ty_int;

  tok->kind = TK_NUM;
  tok->val = val;
  tok->ty = ty;
  return true;
}

// A preprocessing number becomes a real number here, once the preprocessor is
// finished and nothing can paste more characters onto it.
static void convert_pp_number(Token *tok) {
  if (convert_pp_int(tok))
    return;

  char *end;
  double val = strtod(tok->loc, &end);

  Type *ty;
  if (*end == 'f' || *end == 'F') {
    ty = ty_float;
    end++;
  } else if (*end == 'l' || *end == 'L') {
    // cinder has no 80-bit type; long double is double. That is the MSVC ABI's
    // choice too, so it is at least consistent with the platform.
    ty = ty_double;
    end++;
  } else {
    ty = ty_double;
  }

  if (tok->loc + tok->len != end)
    error_tok(tok, "invalid numeric constant");

  tok->kind = TK_NUM;
  tok->fval = val;
  tok->ty = ty;
}

void convert_pp_tokens(Token *tok) {
  for (Token *t = tok; t->kind != TK_EOF; t = t->next) {
    if (is_keyword(t))
      t->kind = TK_KEYWORD;
    else if (t->kind == TK_PP_NUM)
      convert_pp_number(t);
  }
}

//
// The scanner
//

static void add_line_numbers(Token *tok) {
  char *p = current_file->contents;
  int n = 1;

  do {
    if (p == tok->loc) {
      tok->line_no = n;
      tok = tok->next;
    }
    if (*p == '\n')
      n++;
  } while (*p++);
}

Token *tokenize(File *file) {
  current_file = file;

  char *p = file->contents;
  Token head = {0};
  Token *cur = &head;

  at_bol = true;
  has_space = false;

  while (*p) {
    // A line comment runs to the end of the line.
    if (startswith(p, "//")) {
      p += 2;
      while (*p != '\n' && *p != '\0')
        p++;
      has_space = true;
      continue;
    }

    // A block comment does not nest, which trips people up but is what the
    // standard says.
    if (startswith(p, "/*")) {
      char *q = strstr(p + 2, "*/");
      if (!q)
        error_at(p, "unterminated block comment");
      p = q + 2;
      has_space = true;
      continue;
    }

    if (*p == '\n') {
      p++;
      at_bol = true;
      has_space = false;
      continue;
    }

    if (isspace((unsigned char)*p)) {
      p++;
      has_space = true;
      continue;
    }

    // A preprocessing number: a digit, or a dot followed by a digit, then any
    // run of identifier characters, dots, and exponent signs. Deliberately
    // over-permissive -- `0x1p+3` and `1.2e-4` both have to survive to the
    // point where they can be interpreted.
    if (isdigit((unsigned char)*p) ||
        (*p == '.' && isdigit((unsigned char)p[1]))) {
      char *q = p++;
      for (;;) {
        if (p[0] && p[1] && strchr("eEpP", p[0]) && strchr("+-", p[1]))
          p += 2;
        else if (is_ident2(*p) || *p == '.')
          p++;
        else
          break;
      }
      cur = cur->next = new_token(TK_PP_NUM, q, p);
      continue;
    }

    if (*p == '"') {
      cur = cur->next = read_string_literal(p, p);
      p += cur->len;
      continue;
    }

    if (*p == '\'') {
      cur = cur->next = read_char_literal(p, p);
      p += cur->len;
      continue;
    }

    if (is_ident1(*p)) {
      char *start = p;
      do {
        p++;
      } while (is_ident2(*p));
      cur = cur->next = new_token(TK_IDENT, start, p);
      continue;
    }

    int punct_len = read_punct(p);
    if (punct_len) {
      cur = cur->next = new_token(TK_PUNCT, p, p + punct_len);
      p += cur->len;
      continue;
    }

    error_at(p, "invalid token");
  }

  cur = cur->next = new_token(TK_EOF, p, p);
  add_line_numbers(head.next);
  return head.next;
}

//
// Reading files
//

static char *read_file(char *path) {
  FILE *fp;

  if (strcmp(path, "-") == 0) {
    fp = stdin;
  } else {
    // Binary mode, deliberately. In text mode the CRT would translate CRLF for
    // us, but then the byte offsets stored in tokens would not match the file
    // on disk and every caret in a diagnostic would drift. Newlines are
    // normalised below instead, before any token points into the buffer.
    fp = fopen(path, "rb");
    if (!fp)
      return NULL;
  }

  int capacity = 4096;
  int len = 0;
  char *buf = xcalloc(1, capacity);

  for (;;) {
    int n = fread(buf + len, 1, capacity - len - 2, fp);
    if (n == 0)
      break;
    len += n;
    if (len + 2 >= capacity) {
      char *old = buf;
      capacity *= 2;
      buf = xcalloc(1, capacity);
      memcpy(buf, old, len);
    }
  }

  if (fp != stdin)
    fclose(fp);

  // The standard requires a source file to end in a newline; supplying one is
  // friendlier than diagnosing it, and it means the scanner never has to
  // special-case a token that runs to the end of the buffer.
  if (len == 0 || buf[len - 1] != '\n')
    buf[len++] = '\n';
  buf[len] = '\0';
  return buf;
}

File **get_input_files(void) { return input_files; }

File *new_file(char *name, int file_no, char *contents) {
  File *file = xcalloc(1, sizeof(File));
  file->name = name;
  file->display_name = name;
  file->file_no = file_no;
  file->contents = contents;
  return file;
}

// Translation phase 1: normalise line endings. This runs on Windows source
// far more often than not, and every later stage is written assuming '\n'.
static void canonicalize_newline(char *p) {
  char *q = p;
  while (*p) {
    if (p[0] == '\r' && p[1] == '\n') {
      p += 2;
      *q++ = '\n';
    } else if (p[0] == '\r') {
      p++;
      *q++ = '\n';
    } else {
      *q++ = *p++;
    }
  }
  *q = '\0';
}

// Translation phase 2: splice lines ending in a backslash.
//
// The removed newlines are not simply dropped -- they are re-emitted at the
// end of the spliced logical line, so that a token after the splice still
// reports the line number it was written on. Getting this wrong makes every
// diagnostic after a multi-line macro point at the wrong place.
static void remove_backslash_newline(char *p) {
  char *q = p;
  int n = 0;

  while (*p) {
    if (p[0] == '\\' && p[1] == '\n') {
      p += 2;
      n++;
    } else if (*p == '\n') {
      *q++ = *p++;
      for (; n > 0; n--)
        *q++ = '\n';
    } else {
      *q++ = *p++;
    }
  }

  for (; n > 0; n--)
    *q++ = '\n';
  *q = '\0';
}

Token *tokenize_file(char *path) {
  char *p = read_file(path);
  if (!p)
    return NULL;

  canonicalize_newline(p);
  remove_backslash_newline(p);

  // Files are numbered from 1 so that 0 can mean "no file".
  static int file_no;
  File *file = new_file(path, file_no + 1, p);

  // Keep every file, so a diagnostic about a token from a header can still
  // print that header's source line.
  input_files = realloc(input_files, sizeof(char *) * (file_no + 2));
  input_files[file_no] = file;
  input_files[file_no + 1] = NULL;
  file_no++;

  return tokenize(file);
}
