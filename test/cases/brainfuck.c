// A Brainfuck interpreter, compiled by cinder.
//
// This is here as a demonstration that cinder compiles a real program rather
// than a test case: it uses pointers, arrays, a struct, a switch, recursion
// through a bracket-matching pass, string handling, and the standard library.
//
//   cinder examples/brainfuck.c -o bf.exe
//   ./bf.exe

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAPE_SIZE 30000
#define MAX_PROGRAM 65536

struct Machine {
  unsigned char tape[TAPE_SIZE];
  int head;
  char program[MAX_PROGRAM];
  int jumps[MAX_PROGRAM]; // for each bracket, the index of its partner
  int length;
};

// Precompute where every bracket jumps to, so the interpreter never has to
// scan for a matching one at run time. Without this, a loop body is rescanned
// on every iteration and a nested loop becomes quadratic.
static int build_jump_table(struct Machine *m) {
  int stack[1024];
  int depth = 0;

  for (int i = 0; i < m->length; i++) {
    if (m->program[i] == '[') {
      if (depth == 1024) {
        printf("error: brackets nested too deeply\n");
        return 0;
      }
      stack[depth++] = i;
    } else if (m->program[i] == ']') {
      if (depth == 0) {
        printf("error: unmatched ] at position %d\n", i);
        return 0;
      }
      int open = stack[--depth];
      m->jumps[open] = i;
      m->jumps[i] = open;
    }
  }

  if (depth != 0) {
    printf("error: unmatched [ at position %d\n", stack[depth - 1]);
    return 0;
  }
  return 1;
}

static void load(struct Machine *m, const char *source) {
  m->length = 0;
  m->head = 0;
  memset(m->tape, 0, TAPE_SIZE);

  // Anything that is not an instruction is a comment.
  for (const char *p = source; *p; p++) {
    if (strchr("><+-.,[]", *p) && m->length < MAX_PROGRAM)
      m->program[m->length++] = *p;
  }
}

static void run(struct Machine *m) {
  for (int pc = 0; pc < m->length; pc++) {
    switch (m->program[pc]) {
    case '>':
      m->head = (m->head + 1) % TAPE_SIZE;
      break;
    case '<':
      m->head = (m->head - 1 + TAPE_SIZE) % TAPE_SIZE;
      break;
    case '+':
      m->tape[m->head]++;
      break;
    case '-':
      m->tape[m->head]--;
      break;
    case '.':
      putchar(m->tape[m->head]);
      break;
    case ',': {
      int c = getchar();
      m->tape[m->head] = (c == EOF) ? 0 : (unsigned char)c;
      break;
    }
    case '[':
      if (m->tape[m->head] == 0)
        pc = m->jumps[pc];
      break;
    case ']':
      if (m->tape[m->head] != 0)
        pc = m->jumps[pc];
      break;
    }
  }
}

static void execute(const char *label, const char *source) {
  struct Machine *m = malloc(sizeof(struct Machine));
  if (!m) {
    printf("out of memory\n");
    return;
  }

  printf("%s: ", label);
  load(m, source);
  if (build_jump_table(m))
    run(m);
  printf("\n");
  free(m);
}

int main(void) {
  execute("hello",
          "++++++++[>++++[>++>+++>+++>+<<<<-]>+>+>->>+[<]<-]>>.>---.+++++++"
          "..+++.>>.<-.<.+++.------.--------.>>+.>++.");

  // Print the digits 0 through 9. Set one cell to '0' (48) with a multiply
  // loop, set another to 10 as a counter, then print-and-increment.
  execute("digits", "++++++[>++++++++<-]++++++++++[>.+<-]");

  // The same, counting down from '9'.
  execute("countdown", "+++++++[>++++++++<-]>+<++++++++++[>.-<-]");

  return 0;
}
