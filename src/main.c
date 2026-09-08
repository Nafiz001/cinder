// The driver: argument handling, and the pipeline from .c to .exe.
//
// cinder generates assembly, not object code. Writing a COFF or ELF emitter is
// a well-understood problem that teaches nothing about compiling C, so the
// assembler and linker are the platform's -- exactly as gcc and clang do,
// which also shell out for both. What cinder does not delegate is any part of
// understanding the language.

#include "cinder.h"

#include <time.h>

StrArray include_paths;
char *base_file;

static char *opt_o;
static bool opt_S;      // stop after generating assembly
static bool opt_E;      // stop after preprocessing
static bool opt_c;      // compile and assemble, but do not link
static bool opt_tokens; // dump the token stream, for debugging the lexer
static StrArray opt_extra_link;
static StrArray inputs;
static StrArray tmpfiles;

static void usage(int status) {
  fprintf(stderr,
          "cinder -- a C compiler\n"
          "\n"
          "usage: cinder [options] file...\n"
          "\n"
          "  -o <file>   write output to <file>\n"
          "  -S          emit assembly and stop\n"
          "  -c          compile and assemble, but do not link\n"
          "  -E          preprocess only\n"
          "  -I<dir>     add <dir> to the include search path\n"
          "  -D<name>[=<value>]  define a macro\n"
          "  -U<name>    undefine a macro\n"
          "  --tokens    dump the token stream (for debugging cinder itself)\n"
          "  --help      show this message\n");
  exit(status);
}

static bool take_arg(char *arg) {
  static char *args[] = {"-o", "-I", "-D", "-U", "-include"};
  for (int i = 0; i < sizeof(args) / sizeof(*args); i++)
    if (!strcmp(arg, args[i]))
      return true;
  return false;
}

static char *dir_of(char *path) {
  char *slash = NULL;
  for (char *p = path; *p; p++)
    if (*p == '/' || *p == '\\')
      slash = p;
  if (!slash)
    return ".";
  return xstrndup(path, slash - path);
}

// cinder ships its own headers rather than reading the platform's.
//
// MinGW's stdio.h is written for gcc: it uses __declspec, __attribute__,
// __inline and a good deal besides that cinder does not implement, so parsing
// it would fail on the first line that matters. The bundled headers declare
// the same functions with the same prototypes, and because cinder follows the
// platform ABI exactly, those declarations describe the real C library and the
// linker resolves them against it. What is bundled is the interface; the
// implementation is the platform's own.
static void add_default_include_paths(char *argv0) {
  char *dir = dir_of(argv0);
  strarray_push(&include_paths, format("%s/include", dir));
  strarray_push(&include_paths, format("%s/../include", dir));
  strarray_push(&include_paths, "include");
}

static void parse_args(int argc, char **argv) {
  // A flag that takes a separate argument must actually have one; otherwise
  // "cinder -o" would read past the end of argv.
  for (int i = 1; i < argc; i++)
    if (take_arg(argv[i]))
      if (!argv[++i])
        usage(1);

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--help")) {
      usage(0);
    }

    if (!strcmp(argv[i], "-o")) {
      opt_o = argv[++i];
      continue;
    }
    if (!strncmp(argv[i], "-o", 2)) {
      opt_o = argv[i] + 2;
      continue;
    }

    if (!strcmp(argv[i], "-S")) {
      opt_S = true;
      continue;
    }
    if (!strcmp(argv[i], "-c")) {
      opt_c = true;
      continue;
    }
    if (!strcmp(argv[i], "-E")) {
      opt_E = true;
      continue;
    }
    if (!strcmp(argv[i], "--tokens")) {
      opt_tokens = true;
      continue;
    }

    if (!strcmp(argv[i], "-I")) {
      strarray_push(&include_paths, argv[++i]);
      continue;
    }
    if (!strncmp(argv[i], "-I", 2)) {
      strarray_push(&include_paths, argv[i] + 2);
      continue;
    }

    if (!strcmp(argv[i], "-D")) {
      define_cli_macro(argv[++i]);
      continue;
    }
    if (!strncmp(argv[i], "-D", 2)) {
      define_cli_macro(argv[i] + 2);
      continue;
    }

    if (!strcmp(argv[i], "-U")) {
      undef_cli_macro(argv[++i]);
      continue;
    }
    if (!strncmp(argv[i], "-U", 2)) {
      undef_cli_macro(argv[i] + 2);
      continue;
    }

    // Flags that only mean something to the linker are passed straight
    // through, so that `cinder x.c -lm` works.
    if (!strncmp(argv[i], "-l", 2) || !strncmp(argv[i], "-L", 2)) {
      strarray_push(&opt_extra_link, argv[i]);
      continue;
    }

    // Optimisation and warning flags are accepted and ignored. cinder does not
    // optimise, and saying so by accepting -O2 silently is friendlier than
    // failing on a Makefile that passes it.
    if (!strncmp(argv[i], "-O", 2) || !strncmp(argv[i], "-W", 2) ||
        !strncmp(argv[i], "-g", 2) || !strcmp(argv[i], "-std=c11") ||
        !strncmp(argv[i], "-std=", 5) || !strcmp(argv[i], "-pedantic"))
      continue;

    if (argv[i][0] == '-' && argv[i][1] != '\0')
      error("unknown argument: %s", argv[i]);

    strarray_push(&inputs, argv[i]);
  }

  if (inputs.len == 0)
    error("no input files");
}

static FILE *open_file(char *path) {
  if (!path || strcmp(path, "-") == 0)
    return stdout;

  FILE *out = fopen(path, "w");
  if (!out)
    error("cannot open %s: %s", path, strerror(errno));
  return out;
}

static bool endswith(char *p, char *q) {
  int len1 = strlen(p);
  int len2 = strlen(q);
  return (len1 >= len2) && !strcmp(p + len1 - len2, q);
}

// Replace the extension of a path: foo.c -> foo.s
static char *replace_extn(char *tmpl, char *extn) {
  char *filename = tmpl;
  for (char *p = tmpl; *p; p++)
    if (*p == '/' || *p == '\\')
      filename = p + 1;

  char *dot = strrchr(filename, '.');
  if (dot)
    return format("%.*s%s", (int)(dot - tmpl), tmpl, extn);
  return format("%s%s", tmpl, extn);
}

static char *create_tmpfile(char *suffix) {
  static int counter;
  char *dir = getenv("TEMP");
  if (!dir)
    dir = getenv("TMP");
  if (!dir)
    dir = ".";

  static int run_id;
  if (!run_id)
    run_id = (int)time(NULL) & 0xffffff;

  char *path = format("%s/cinder-%d-%d%s", dir, run_id, counter++, suffix);
  strarray_push(&tmpfiles, path);
  return path;
}

static void cleanup(void) {
  for (int i = 0; i < tmpfiles.len; i++)
    remove(tmpfiles.data[i]);
}

// Quote a path for the shell. Windows paths contain spaces often enough that
// skipping this breaks on any normal installation.
static char *quote(char *s) { return format("\"%s\"", s); }

static void run_subprocess(char *cmd) {
  int status = system(cmd);
  if (status != 0) {
    cleanup();
    error("cinder: %s failed", cmd);
  }
}

static void assemble(char *input, char *output) {
  run_subprocess(format("as -c %s -o %s", quote(input), quote(output)));
}

static void link_exe(StrArray *objects, char *output) {
  // gcc rather than ld, so that the C runtime startup files and the default
  // libraries come along without cinder having to know their names or their
  // order.
  char *cmd = format("gcc -o %s", quote(output));
  for (int i = 0; i < objects->len; i++)
    cmd = format("%s %s", cmd, quote(objects->data[i]));
  for (int i = 0; i < opt_extra_link.len; i++)
    cmd = format("%s %s", cmd, opt_extra_link.data[i]);
  run_subprocess(cmd);
}

// --tokens exists for debugging the lexer, and is the first thing worth having
// when a program fails to parse for reasons that make no sense.
static void print_tokens(Token *tok) {
  FILE *out = open_file(opt_o ? opt_o : "-");
  int line = 1;
  for (; tok->kind != TK_EOF; tok = tok->next) {
    if (line > 1 && tok->at_bol)
      fprintf(out, "\n");
    if (tok->has_space && !tok->at_bol)
      fprintf(out, " ");
    fprintf(out, "%.*s", tok->len, tok->loc);
    line++;
  }
  fprintf(out, "\n");
}

static void print_preprocessed(Token *tok) {
  FILE *out = open_file(opt_o ? opt_o : "-");

  int line = 1;
  for (; tok->kind != TK_EOF; tok = tok->next) {
    if (line > 1 && tok->at_bol)
      fprintf(out, "\n");
    if (tok->has_space && !tok->at_bol)
      fprintf(out, " ");
    fprintf(out, "%.*s", tok->len, tok->loc);
    line++;
  }
  fprintf(out, "\n");
}

static void compile(char *input, char *output) {
  base_file = input;

  Token *tok = tokenize_file(input);
  if (!tok)
    error("cannot open %s: %s", input, strerror(errno));

  if (opt_tokens) {
    print_tokens(tok);
    return;
  }

  tok = preprocess(tok);

  if (opt_E) {
    print_preprocessed(tok);
    return;
  }

  Obj *prog = parse(tok);

  FILE *out = open_file(output);
  codegen(prog, out);
  if (out != stdout)
    fclose(out);
}

int main(int argc, char **argv) {
  atexit(cleanup);
  init_macros();
  add_default_include_paths(argv[0]);
  parse_args(argc, argv);

  if (opt_o && (opt_S || opt_c || opt_E) && inputs.len > 1)
    error("cannot specify -o with -S, -c or -E and multiple input files");

  StrArray objects = {0};

  for (int i = 0; i < inputs.len; i++) {
    char *input = inputs.data[i];

    // A file that is already an object or a library goes straight to the
    // linker, so that cinder can be used to link a mixed project.
    if (endswith(input, ".o") || endswith(input, ".obj") ||
        endswith(input, ".a")) {
      strarray_push(&objects, input);
      continue;
    }

    if (opt_E) {
      compile(input, NULL);
      continue;
    }

    if (opt_S) {
      compile(input, opt_o ? opt_o : replace_extn(input, ".s"));
      continue;
    }

    if (opt_tokens) {
      compile(input, NULL);
      continue;
    }

    // Compile to a temporary .s, then assemble.
    char *tmp_s = create_tmpfile(".s");
    compile(input, tmp_s);

    if (opt_c) {
      assemble(tmp_s, opt_o ? opt_o : replace_extn(input, ".o"));
      continue;
    }

    char *tmp_o = create_tmpfile(".o");
    assemble(tmp_s, tmp_o);
    strarray_push(&objects, tmp_o);
  }

  if (objects.len > 0)
    link_exe(&objects, opt_o ? opt_o : "a.exe");

  return 0;
}
