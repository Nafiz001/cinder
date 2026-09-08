// The code generator: typed AST in, x86-64 assembly out.
//
// The strategy is a stack machine. Every expression leaves its value in RAX
// (or XMM0 if it is floating point), and a binary operator evaluates the right
// side, pushes it, evaluates the left side, and pops. That produces more
// instructions than a register allocator would -- there is no attempt here to
// keep values in registers across statements -- and it is correct by
// construction for arbitrarily nested expressions, which is the right trade
// for a compiler whose point is to be understood.
//
// The target is the Microsoft x64 calling convention. It differs from System V
// in five ways that all actually bite, and every one of them is a bug you only
// find by running the output:
//
//   1. Integer arguments go in RCX, RDX, R8, R9 -- not RDI, RSI, RDX, RCX.
//   2. RDI and RSI are CALLEE-saved here. On System V they are argument
//      registers. Clobbering them silently corrupts the caller.
//   3. The register index is the argument's POSITION, not a per-class counter.
//      In f(int, double, int) the arguments go to RCX, XMM1, R8 -- XMM1, not
//      XMM0, because the double is the second parameter. System V would use
//      XMM0.
//   4. The caller must allocate 32 bytes of "shadow space" above the return
//      address, always, even when the callee takes no arguments at all. The
//      callee is entitled to spill its four register arguments there.
//   5. A struct is passed in a register only if its size is exactly 1, 2, 4 or
//      8 bytes. Anything else is passed as a pointer to a caller-made copy,
//      and returned through a hidden pointer passed as the first argument.
//
// Point 4 makes the layout unusually tidy: argument i always has a home at
// [RSP + i*8] at the moment of the call, with the first four also loaded into
// registers. Arguments are evaluated straight into those homes, which means a
// nested call inside an argument simply allocates its own area further down
// and cannot disturb this one.

#include "cinder.h"

#define GP_MAX 4 // integer argument registers
#define FP_MAX 4 // floating-point argument registers

static FILE *output_file;
static int depth; // 8-byte slots currently pushed, for stack alignment
static Obj *current_fn;

static char *argreg8[] = {"%cl", "%dl", "%r8b", "%r9b"};
static char *argreg16[] = {"%cx", "%dx", "%r8w", "%r9w"};
static char *argreg32[] = {"%ecx", "%edx", "%r8d", "%r9d"};
static char *argreg64[] = {"%rcx", "%rdx", "%r8", "%r9"};

static void gen_expr(Node *node);
static void gen_stmt(Node *node);

__attribute__((format(printf, 1, 2))) static void println(char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(output_file, fmt, ap);
  va_end(ap);
  fprintf(output_file, "\n");
}

static int count(void) {
  static int i = 1;
  return i++;
}

static void push(void) {
  println("  push %%rax");
  depth++;
}

static void pop(char *arg) {
  println("  pop %s", arg);
  depth--;
}

static void pushf(void) {
  println("  sub $8, %%rsp");
  println("  movsd %%xmm0, (%%rsp)");
  depth++;
}

static void popf(int reg) {
  println("  movsd (%%rsp), %%xmm%d", reg);
  println("  add $8, %%rsp");
  depth--;
}

// A struct is register-sized only at exactly these widths. A 3-byte or 12-byte
// struct is not, even though it would fit.
static bool is_reg_sized(Type *ty) {
  int sz = ty->size;
  return sz == 1 || sz == 2 || sz == 4 || sz == 8;
}

static bool is_by_pointer(Type *ty) {
  return (ty->kind == TY_STRUCT || ty->kind == TY_UNION) && !is_reg_sized(ty);
}

//
// Addresses
//

// gen_addr computes the address of an lvalue into RAX.
static void gen_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR:
    if (node->var->is_local) {
      println("  lea %d(%%rbp), %%rax", node->var->offset);
      return;
    }
    // Globals are addressed RIP-relative, which is what makes the output
    // position-independent without a GOT.
    println("  lea %s(%%rip), %%rax", node->var->name);
    return;
  case ND_DEREF:
    gen_expr(node->lhs);
    return;
  case ND_COMMA:
    gen_expr(node->lhs);
    gen_addr(node->rhs);
    return;
  case ND_MEMBER:
    gen_addr(node->lhs);
    println("  add $%d, %%rax", node->member->offset);
    return;
  case ND_FUNCALL:
    if (node->ret_buffer) {
      gen_expr(node);
      return;
    }
    break;
  default:
    break;
  }

  error_tok(node->tok, "not an lvalue");
}

// load dereferences RAX, leaving the value in RAX or XMM0.
static void load(Type *ty) {
  switch (ty->kind) {
  case TY_ARRAY:
  case TY_STRUCT:
  case TY_UNION:
  case TY_FUNC:
    // An array evaluates to its own address rather than to its contents; there
    // is no way to load "an array" into a register. Same for a struct, which
    // is copied by whoever consumes it.
    return;
  case TY_FLOAT:
    println("  movss (%%rax), %%xmm0");
    return;
  case TY_DOUBLE:
    println("  movsd (%%rax), %%xmm0");
    return;
  default:
    break;
  }

  // A narrow value is sign- or zero-extended on load, so that the rest of the
  // generator can assume a full register. `movsbl` rather than `movb` is what
  // makes `char c = -1; c == -1` true.
  char *insn = ty->is_unsigned ? "movz" : "movs";

  if (ty->size == 1)
    println("  %sbl (%%rax), %%eax", insn);
  else if (ty->size == 2)
    println("  %swl (%%rax), %%eax", insn);
  else if (ty->size == 4)
    println("  movsxd (%%rax), %%rax");
  else
    println("  mov (%%rax), %%rax");
}

// store writes RAX (or XMM0) to the address on top of the stack.
static void store(Type *ty) {
  pop("%rdi");

  switch (ty->kind) {
  case TY_STRUCT:
  case TY_UNION:
    // Struct assignment is a byte copy. A rep movsb would be shorter; a plain
    // loop unrolled at compile time avoids depending on the direction flag
    // being clear, which is one less thing to get wrong.
    for (int i = 0; i < ty->size; i++) {
      println("  mov %d(%%rax), %%r8b", i);
      println("  mov %%r8b, %d(%%rdi)", i);
    }
    return;
  case TY_FLOAT:
    println("  movss %%xmm0, (%%rdi)");
    return;
  case TY_DOUBLE:
    println("  movsd %%xmm0, (%%rdi)");
    return;
  default:
    break;
  }

  if (ty->size == 1)
    println("  mov %%al, (%%rdi)");
  else if (ty->size == 2)
    println("  mov %%ax, (%%rdi)");
  else if (ty->size == 4)
    println("  mov %%eax, (%%rdi)");
  else
    println("  mov %%rax, (%%rdi)");
}

// cmp_zero sets the flags from a value, which is what `if`, `!` and the
// logical operators all need.
static void cmp_zero(Type *ty) {
  switch (ty->kind) {
  case TY_FLOAT:
    println("  xorps %%xmm1, %%xmm1");
    println("  ucomiss %%xmm1, %%xmm0");
    return;
  case TY_DOUBLE:
    println("  xorpd %%xmm1, %%xmm1");
    println("  ucomisd %%xmm1, %%xmm0");
    return;
  default:
    break;
  }

  if (is_integer(ty) && ty->size <= 4)
    println("  cmp $0, %%eax");
  else
    println("  cmp $0, %%rax");
}

//
// Casts
//

enum { I8, I16, I32, I64, U8, U16, U32, U64, F32, F64 };

static int getTypeId(Type *ty) {
  switch (ty->kind) {
  case TY_CHAR:
    return ty->is_unsigned ? U8 : I8;
  case TY_SHORT:
    return ty->is_unsigned ? U16 : I16;
  case TY_INT:
  case TY_LONG: // 4 bytes on Windows
    return ty->is_unsigned ? U32 : I32;
  case TY_LLONG:
    return ty->is_unsigned ? U64 : I64;
  case TY_FLOAT:
    return F32;
  case TY_DOUBLE:
    return F64;
  case TY_ENUM:
    return I32;
  default:
    return U64; // pointers, arrays, functions
  }
}

// Truncation and extension between integer widths.
static char i32i8[] = "movsbl %al, %eax";
static char i32u8[] = "movzbl %al, %eax";
static char i32i16[] = "movswl %ax, %eax";
static char i32u16[] = "movzwl %ax, %eax";
static char i32i64[] = "movsxd %eax, %rax";
static char u32i64[] = "mov %eax, %eax";

// Integer to float, and back. The truncating form (cvtt*) is the right one:
// C says a float-to-integer conversion discards the fraction, it does not
// round.
static char i32f32[] = "cvtsi2ssl %eax, %xmm0";
static char i32f64[] = "cvtsi2sdl %eax, %xmm0";
static char i64f32[] = "cvtsi2ss %rax, %xmm0";
static char i64f64[] = "cvtsi2sd %rax, %xmm0";

// x86-64 has no unsigned-to-float instruction. A 32-bit unsigned value is
// widened to 64-bit signed first, which is exact and costs one instruction.
static char u32f32[] = "mov %eax, %eax; cvtsi2ss %rax, %xmm0";
static char u32f64[] = "mov %eax, %eax; cvtsi2sd %rax, %xmm0";

// A 64-bit unsigned value does not fit in a signed 64-bit register, so values
// with the top bit set are halved, converted, and doubled back. The low bit is
// preserved through the halving so the result is correctly rounded.
static char u64f32[] =
    "test %rax,%rax; js 1f; cvtsi2ss %rax, %xmm0; jmp 2f; "
    "1: mov %rax,%rdi; and $1,%eax; shr %rdi; or %rax,%rdi; "
    "cvtsi2ss %rdi, %xmm0; addss %xmm0, %xmm0; 2:";
static char u64f64[] =
    "test %rax,%rax; js 1f; cvtsi2sd %rax, %xmm0; jmp 2f; "
    "1: mov %rax,%rdi; and $1,%eax; shr %rdi; or %rax,%rdi; "
    "cvtsi2sd %rdi, %xmm0; addsd %xmm0, %xmm0; 2:";

static char f32i8[] = "cvttss2sil %xmm0, %eax; movsbl %al, %eax";
static char f32u8[] = "cvttss2sil %xmm0, %eax; movzbl %al, %eax";
static char f32i16[] = "cvttss2sil %xmm0, %eax; movswl %ax, %eax";
static char f32u16[] = "cvttss2sil %xmm0, %eax; movzwl %ax, %eax";
static char f32i32[] = "cvttss2sil %xmm0, %eax";
static char f32u32[] = "cvttss2siq %xmm0, %rax";
static char f32i64[] = "cvttss2siq %xmm0, %rax";
static char f32u64[] = "cvttss2siq %xmm0, %rax";
static char f32f64[] = "cvtss2sd %xmm0, %xmm0";

static char f64i8[] = "cvttsd2sil %xmm0, %eax; movsbl %al, %eax";
static char f64u8[] = "cvttsd2sil %xmm0, %eax; movzbl %al, %eax";
static char f64i16[] = "cvttsd2sil %xmm0, %eax; movswl %ax, %eax";
static char f64u16[] = "cvttsd2sil %xmm0, %eax; movzwl %ax, %eax";
static char f64i32[] = "cvttsd2sil %xmm0, %eax";
static char f64u32[] = "cvttsd2siq %xmm0, %rax";
static char f64i64[] = "cvttsd2siq %xmm0, %rax";
static char f64u64[] = "cvttsd2siq %xmm0, %rax";
static char f64f32[] = "cvtsd2ss %xmm0, %xmm0";

// NULL means "no instruction needed": the value already has the right
// representation, which is true for every widening between same-signed
// integers of 32 bits and below, because loads already extended them.
static char *cast_table[][10] = {
    // to i8     i16       i32     i64       u8     u16       u32     u64       f32     f64
    {NULL,       NULL,     NULL,   i32i64,   i32u8, i32u16,   NULL,   i32i64,   i32f32, i32f64}, // from i8
    {i32i8,      NULL,     NULL,   i32i64,   i32u8, i32u16,   NULL,   i32i64,   i32f32, i32f64}, // i16
    {i32i8,      i32i16,   NULL,   i32i64,   i32u8, i32u16,   NULL,   i32i64,   i32f32, i32f64}, // i32
    {i32i8,      i32i16,   NULL,   NULL,     i32u8, i32u16,   NULL,   NULL,     i64f32, i64f64}, // i64
    {i32i8,      NULL,     NULL,   i32i64,   NULL,  NULL,     NULL,   i32i64,   i32f32, i32f64}, // u8
    {i32i8,      i32i16,   NULL,   i32i64,   i32u8, NULL,     NULL,   i32i64,   i32f32, i32f64}, // u16
    {i32i8,      i32i16,   NULL,   u32i64,   i32u8, i32u16,   NULL,   u32i64,   u32f32, u32f64}, // u32
    {i32i8,      i32i16,   NULL,   NULL,     i32u8, i32u16,   NULL,   NULL,     u64f32, u64f64}, // u64
    {f32i8,      f32i16,   f32i32, f32i64,   f32u8, f32u16,   f32u32, f32u64,   NULL,   f32f64}, // f32
    {f64i8,      f64i16,   f64i32, f64i64,   f64u8, f64u16,   f64u32, f64u64,   f64f32, NULL},   // f64
};

static void cast(Type *from, Type *to) {
  if (to->kind == TY_VOID)
    return;

  if (to->kind == TY_BOOL) {
    // Converting to _Bool is a comparison against zero, not a truncation:
    // (_Bool)256 is 1, whereas (char)256 is 0.
    cmp_zero(from);
    println("  setne %%al");
    println("  movzx %%al, %%eax");
    return;
  }

  int t1 = getTypeId(from);
  int t2 = getTypeId(to);
  if (cast_table[t1][t2])
    println("  %s", cast_table[t1][t2]);
}

//
// Function calls
//

// Whether argument i, given its type, travels in an XMM register. On Windows
// the slot is decided by position, so this only has to answer "is it a float".
static bool is_fp_arg(Type *ty) { return is_flonum(ty); }

// A struct too large to fit in a register is passed as a pointer to a copy the
// caller makes. copy_struct_to writes that copy into the outgoing area.
static void copy_struct_to(int offset, int size) {
  for (int i = 0; i < size; i++) {
    println("  mov %d(%%rax), %%r10b", i);
    println("  mov %%r10b, %d(%%rsp)", offset + i);
  }
}

static void gen_funcall(Node *node) {
  // 1. Lay out the outgoing argument area.
  int nargs = 0;
  for (Node *arg = node->args; arg; arg = arg->next)
    nargs++;

  // Two separate things. `ret_buffer` says the result lands in this frame,
  // which is true of any struct return. `hidden` says the callee is handed a
  // pointer to it as argument zero, which is true only when the struct is too
  // large to come back in RAX.
  bool has_ret_buffer = node->ret_buffer != NULL;
  bool hidden = has_ret_buffer && is_by_pointer(node->func_ty->return_ty);
  int total_args = nargs + (hidden ? 1 : 0);

  // Every argument has a home at [rsp + i*8], and the first four of those
  // homes are the shadow space the callee may spill into. So the area is
  // never smaller than 32 bytes even for a call taking nothing.
  int arg_area = (total_args > 4 ? total_args : 4) * 8;

  // Copies of oversized structs go above the argument homes.
  int copy_area = 0;
  for (Node *arg = node->args; arg; arg = arg->next)
    if (is_by_pointer(arg->ty))
      copy_area += align_to(arg->ty->size, 8);

  int area = arg_area + copy_area;

  // RSP must be 16-byte aligned at the call instruction. `depth` says how many
  // 8-byte pushes are outstanding, which is what its alignment currently is.
  int want = (depth % 2) ? 8 : 0;
  if (area % 16 != want)
    area += 8;

  println("  sub $%d, %%rsp", area);
  depth += area / 8;

  // 2. Evaluate each argument straight into its home. Evaluating into the
  //    final location rather than into a register means a nested call inside
  //    an argument allocates its own area below this one and cannot disturb
  //    it.
  int slot = hidden ? 1 : 0;
  int copy_off = arg_area;

  for (Node *arg = node->args; arg; arg = arg->next, slot++) {
    gen_expr(arg);

    if (is_by_pointer(arg->ty)) {
      // gen_expr left the struct's address in RAX. Copy it, then store the
      // address of the copy.
      copy_struct_to(copy_off, arg->ty->size);
      println("  lea %d(%%rsp), %%rax", copy_off);
      println("  mov %%rax, %d(%%rsp)", slot * 8);
      copy_off += align_to(arg->ty->size, 8);
      continue;
    }

    if (arg->ty->kind == TY_STRUCT || arg->ty->kind == TY_UNION) {
      // Register-sized: pass the bytes themselves, not the address.
      println("  mov (%%rax), %%rax");
      println("  mov %%rax, %d(%%rsp)", slot * 8);
      continue;
    }

    if (is_fp_arg(arg->ty)) {
      if (arg->ty->kind == TY_FLOAT)
        println("  movss %%xmm0, %d(%%rsp)", slot * 8);
      else
        println("  movsd %%xmm0, %d(%%rsp)", slot * 8);
      continue;
    }

    println("  mov %%rax, %d(%%rsp)", slot * 8);
  }

  // The hidden return buffer is argument zero when the return type is too
  // large for a register.
  if (hidden) {
    println("  lea %d(%%rbp), %%rax", node->ret_buffer->offset);
    println("  mov %%rax, (%%rsp)");
  }

  // 3. Load the first four homes into registers. This happens after every
  //    argument has been evaluated, so an argument's own code cannot clobber a
  //    register already loaded.
  if (hidden)
    println("  mov (%%rsp), %%rcx");

  int i = hidden ? 1 : 0;
  for (Node *arg = node->args; arg && i < GP_MAX; arg = arg->next, i++) {
    if (is_fp_arg(arg->ty)) {
      if (arg->ty->kind == TY_FLOAT)
        println("  movss %d(%%rsp), %%xmm%d", i * 8, i);
      else
        println("  movsd %d(%%rsp), %%xmm%d", i * 8, i);

      // A float passed to a variadic function must ALSO be placed in the
      // matching integer register. printf reads it from there, and without
      // this every "%f" prints garbage. This is the single most surprising
      // rule in the Microsoft ABI.
      if (node->func_ty->is_variadic)
        println("  mov %d(%%rsp), %s", i * 8, argreg64[i]);
    } else {
      println("  mov %d(%%rsp), %s", i * 8, argreg64[i]);
    }
  }

  // 4. Call, then release the area.
  if (node->lhs->kind == ND_VAR && !node->lhs->var->is_local &&
      node->lhs->var->is_function) {
    println("  call %s", node->lhs->var->name);
  } else {
    gen_expr(node->lhs);
    println("  call *%%rax");
  }

  println("  add $%d, %%rsp", area);
  depth -= area / 8;

  // A function returning a narrow integer leaves only the low bits defined, so
  // the result is re-extended before anything else looks at it.
  Type *ret = node->func_ty->return_ty;
  if (is_integer(ret) && ret->size < 4) {
    if (ret->kind == TY_BOOL)
      println("  movzx %%al, %%eax");
    else if (ret->size == 1)
      println("  %s %%al, %%eax", ret->is_unsigned ? "movzbl" : "movsbl");
    else
      println("  %s %%ax, %%eax", ret->is_unsigned ? "movzwl" : "movswl");
  }

  if (has_ret_buffer) {
    // A register-sized struct came back in RAX as a value. Spill it into the
    // buffer so that the result of the call is an address, like every other
    // struct expression.
    if (!hidden) {
      int sz = node->func_ty->return_ty->size;
      int off = node->ret_buffer->offset;
      if (sz == 1)
        println("  mov %%al, %d(%%rbp)", off);
      else if (sz == 2)
        println("  mov %%ax, %d(%%rbp)", off);
      else if (sz == 4)
        println("  mov %%eax, %d(%%rbp)", off);
      else
        println("  mov %%rax, %d(%%rbp)", off);
    }
    println("  lea %d(%%rbp), %%rax", node->ret_buffer->offset);
  }
}

//
// Expressions
//

static void gen_expr(Node *node) {
  switch (node->kind) {
  case ND_NULL_EXPR:
    return;

  case ND_NUM: {
    if (node->ty->kind == TY_FLOAT) {
      // A float constant is emitted as its bit pattern, because there is no
      // instruction that loads an immediate into an XMM register.
      union {
        float f32;
        uint32_t u32;
      } u;
      u.f32 = node->fval;
      println("  mov $%u, %%eax  # float %f", u.u32, node->fval);
      println("  movd %%eax, %%xmm0");
      return;
    }
    if (node->ty->kind == TY_DOUBLE) {
      union {
        double f64;
        uint64_t u64;
      } u;
      u.f64 = node->fval;
      println("  mov $%llu, %%rax  # double %f", (unsigned long long)u.u64,
              node->fval);
      println("  movq %%rax, %%xmm0");
      return;
    }
    println("  mov $%lld, %%rax", (long long)node->val);
    return;
  }

  case ND_NEG:
    gen_expr(node->lhs);

    if (node->ty->kind == TY_FLOAT) {
      // Negating a float flips the sign bit rather than subtracting from zero,
      // so that -0.0 and NaN behave.
      println("  mov $1, %%rax");
      println("  shl $31, %%rax");
      println("  movq %%rax, %%xmm1");
      println("  xorps %%xmm1, %%xmm0");
      return;
    }
    if (node->ty->kind == TY_DOUBLE) {
      println("  mov $1, %%rax");
      println("  shl $63, %%rax");
      println("  movq %%rax, %%xmm1");
      println("  xorpd %%xmm1, %%xmm0");
      return;
    }
    println("  neg %%rax");
    return;

  case ND_VAR:
  case ND_MEMBER:
    gen_addr(node);
    load(node->ty);
    return;

  case ND_DEREF:
    gen_expr(node->lhs);
    load(node->ty);
    return;

  case ND_ADDR:
    gen_addr(node->lhs);
    return;

  case ND_ASSIGN:
    gen_addr(node->lhs);
    push();
    gen_expr(node->rhs);
    store(node->ty);
    return;

  case ND_STMT_EXPR:
    for (Node *n = node->body; n; n = n->next)
      gen_stmt(n);
    return;

  case ND_COMMA:
    gen_expr(node->lhs);
    gen_expr(node->rhs);
    return;

  case ND_CAST:
    gen_expr(node->lhs);
    cast(node->lhs->ty, node->ty);
    return;

  case ND_MEMZERO:
    // Clear a local before its explicit initialisers run, so that a partial
    // initialiser leaves the rest zero without emitting a store per element.
    println("  mov $%d, %%rcx", node->var->ty->size);
    println("  lea %d(%%rbp), %%rdi", node->var->offset);
    println("  mov $0, %%al");
    println("  rep stosb");
    return;

  case ND_COND: {
    int c = count();
    gen_expr(node->cond);
    cmp_zero(node->cond->ty);
    println("  je .L.else.%d", c);
    gen_expr(node->then);
    println("  jmp .L.end.%d", c);
    println(".L.else.%d:", c);
    gen_expr(node->els);
    println(".L.end.%d:", c);
    return;
  }

  case ND_NOT:
    gen_expr(node->lhs);
    cmp_zero(node->lhs->ty);
    println("  sete %%al");
    println("  movzx %%al, %%rax");
    return;

  case ND_BITNOT:
    gen_expr(node->lhs);
    println("  not %%rax");
    return;

  case ND_LOGAND: {
    // Short-circuit: the right side is not evaluated if the left is false.
    int c = count();
    gen_expr(node->lhs);
    cmp_zero(node->lhs->ty);
    println("  je .L.false.%d", c);
    gen_expr(node->rhs);
    cmp_zero(node->rhs->ty);
    println("  je .L.false.%d", c);
    println("  mov $1, %%rax");
    println("  jmp .L.end.%d", c);
    println(".L.false.%d:", c);
    println("  mov $0, %%rax");
    println(".L.end.%d:", c);
    return;
  }

  case ND_LOGOR: {
    int c = count();
    gen_expr(node->lhs);
    cmp_zero(node->lhs->ty);
    println("  jne .L.true.%d", c);
    gen_expr(node->rhs);
    cmp_zero(node->rhs->ty);
    println("  jne .L.true.%d", c);
    println("  mov $0, %%rax");
    println("  jmp .L.end.%d", c);
    println(".L.true.%d:", c);
    println("  mov $1, %%rax");
    println(".L.end.%d:", c);
    return;
  }

  case ND_FUNCALL:
    gen_funcall(node);
    return;

  default:
    break;
  }

  // Binary operators. The right side is evaluated first and pushed, so that
  // the left side ends up in RAX where the arithmetic instructions want it.
  if (is_flonum(node->lhs->ty)) {
    gen_expr(node->rhs);
    pushf();
    gen_expr(node->lhs);
    popf(1);

    char *sz = (node->lhs->ty->kind == TY_FLOAT) ? "ss" : "sd";

    switch (node->kind) {
    case ND_ADD:
      println("  add%s %%xmm1, %%xmm0", sz);
      return;
    case ND_SUB:
      println("  sub%s %%xmm1, %%xmm0", sz);
      return;
    case ND_MUL:
      println("  mul%s %%xmm1, %%xmm0", sz);
      return;
    case ND_DIV:
      println("  div%s %%xmm1, %%xmm0", sz);
      return;
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE:
      println("  ucomi%s %%xmm0, %%xmm1", sz);

      if (node->kind == ND_EQ) {
        // A NaN compares unordered, which sets the parity flag. Equality must
        // be false in that case, so the parity flag is folded in explicitly --
        // `sete` alone would report NaN == NaN as true.
        println("  sete %%al");
        println("  setnp %%dl");
        println("  and %%dl, %%al");
      } else if (node->kind == ND_NE) {
        println("  setne %%al");
        println("  setp %%dl");
        println("  or %%dl, %%al");
      } else if (node->kind == ND_LT) {
        println("  seta %%al");
      } else {
        println("  setae %%al");
      }

      println("  and $1, %%al");
      println("  movzb %%al, %%rax");
      return;
    default:
      error_tok(node->tok, "invalid floating-point expression");
    }
  }

  gen_expr(node->rhs);
  push();
  gen_expr(node->lhs);
  pop("%rdi");

  // Operate at the operand's own width. Using the 64-bit form for a 32-bit
  // operation would give the wrong answer on overflow, because C says int
  // arithmetic wraps at 32 bits.
  char *ax, *di, *dx;
  if (node->lhs->ty->kind == TY_LLONG || node->lhs->ty->base) {
    ax = "%rax";
    di = "%rdi";
    dx = "%rdx";
  } else {
    ax = "%eax";
    di = "%edi";
    dx = "%edx";
  }

  switch (node->kind) {
  case ND_ADD:
    println("  add %s, %s", di, ax);
    return;
  case ND_SUB:
    println("  sub %s, %s", di, ax);
    return;
  case ND_MUL:
    println("  imul %s, %s", di, ax);
    return;
  case ND_DIV:
  case ND_MOD:
    if (node->ty->is_unsigned) {
      println("  mov $0, %s", dx);
      println("  div %s", di);
    } else {
      // cqo/cdq sign-extends RAX into RDX:RAX, which is what idiv divides.
      // Forgetting it leaves whatever was in RDX and produces a wrong
      // quotient or a divide-error trap.
      if (node->lhs->ty->size == 8)
        println("  cqo");
      else
        println("  cdq");
      println("  idiv %s", di);
    }

    if (node->kind == ND_MOD)
      println("  mov %%rdx, %%rax");
    return;
  case ND_BITAND:
    println("  and %s, %s", di, ax);
    return;
  case ND_BITOR:
    println("  or %s, %s", di, ax);
    return;
  case ND_BITXOR:
    println("  xor %s, %s", di, ax);
    return;
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
    println("  cmp %s, %s", di, ax);

    if (node->kind == ND_EQ)
      println("  sete %%al");
    else if (node->kind == ND_NE)
      println("  setne %%al");
    else if (node->kind == ND_LT)
      // Signedness picks the instruction. Using `setl` on pointers would make
      // an address above 2^63 compare as negative.
      println("  %s %%al", node->lhs->ty->is_unsigned ? "setb" : "setl");
    else
      println("  %s %%al", node->lhs->ty->is_unsigned ? "setbe" : "setle");

    println("  movzb %%al, %%rax");
    return;
  case ND_SHL:
    println("  mov %%rdi, %%rcx");
    println("  shl %%cl, %s", ax);
    return;
  case ND_SHR:
    println("  mov %%rdi, %%rcx");
    // An unsigned shift brings in zeros, a signed one copies the sign bit.
    // Using the wrong one turns -8 >> 1 into a huge positive number.
    if (node->lhs->ty->is_unsigned)
      println("  shr %%cl, %s", ax);
    else
      println("  sar %%cl, %s", ax);
    return;
  default:
    break;
  }

  error_tok(node->tok, "invalid expression");
}

//
// Statements
//

static void gen_stmt(Node *node) {
  switch (node->kind) {
  case ND_IF: {
    int c = count();
    gen_expr(node->cond);
    cmp_zero(node->cond->ty);
    println("  je  .L.else.%d", c);
    gen_stmt(node->then);
    println("  jmp .L.end.%d", c);
    println(".L.else.%d:", c);
    if (node->els)
      gen_stmt(node->els);
    println(".L.end.%d:", c);
    return;
  }

  case ND_FOR: {
    int c = count();
    if (node->init)
      gen_stmt(node->init);
    println(".L.begin.%d:", c);
    if (node->cond) {
      gen_expr(node->cond);
      cmp_zero(node->cond->ty);
      println("  je %s", node->brk_label);
    }
    gen_stmt(node->then);
    // `continue` lands here, so that the increment still runs -- a subtlety
    // that separates a for loop from a while loop with the increment inlined.
    println("%s:", node->cont_label);
    if (node->inc)
      gen_expr(node->inc);
    println("  jmp .L.begin.%d", c);
    println("%s:", node->brk_label);
    return;
  }

  case ND_DO: {
    int c = count();
    println(".L.begin.%d:", c);
    gen_stmt(node->then);
    println("%s:", node->cont_label);
    gen_expr(node->cond);
    cmp_zero(node->cond->ty);
    println("  jne .L.begin.%d", c);
    println("%s:", node->brk_label);
    return;
  }

  case ND_SWITCH:
    gen_expr(node->cond);

    // A linear chain of comparisons. A jump table would be faster for a dense
    // switch; this is O(cases) and correct for any case values at all,
    // including sparse and negative ones.
    for (Node *n = node->case_next; n; n = n->case_next) {
      char *ax = (node->cond->ty->size == 8) ? "%rax" : "%eax";
      char *di = (node->cond->ty->size == 8) ? "%rdi" : "%edi";
      println("  mov $%ld, %s", n->begin, di);
      println("  cmp %s, %s", di, ax);
      println("  je %s", n->label);
    }

    if (node->default_case)
      println("  jmp %s", node->default_case->label);

    println("  jmp %s", node->brk_label);
    gen_stmt(node->then);
    println("%s:", node->brk_label);
    return;

  case ND_CASE:
    println("%s:", node->label);
    gen_stmt(node->lhs);
    return;

  case ND_BLOCK:
    for (Node *n = node->body; n; n = n->next)
      gen_stmt(n);
    return;

  case ND_GOTO:
    println("  jmp %s", node->unique_label);
    return;

  case ND_LABEL:
    println("%s:", node->unique_label);
    gen_stmt(node->lhs);
    return;

  case ND_RETURN:
    if (node->lhs) {
      gen_expr(node->lhs);
      Type *ty = node->lhs->ty;

      if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION)) {
        if (is_reg_sized(ty)) {
          // Small struct: the value itself goes back in RAX.
          println("  mov (%%rax), %%rax");
        } else {
          // Large struct: copy it into the buffer the caller passed as the
          // hidden first argument, and return that pointer.
          println("  mov %d(%%rbp), %%rdi", current_fn->params->offset);
          for (int i = 0; i < ty->size; i++) {
            println("  mov %d(%%rax), %%r8b", i);
            println("  mov %%r8b, %d(%%rdi)", i);
          }
          println("  mov %d(%%rbp), %%rax", current_fn->params->offset);
        }
      }
    }
    println("  jmp .L.return.%s", current_fn->name);
    return;

  case ND_EXPR_STMT:
    gen_expr(node->lhs);
    return;

  default:
    break;
  }

  error_tok(node->tok, "invalid statement");
}

//
// Frame layout
//

static void assign_lvar_offsets(Obj *prog) {
  for (Obj *fn = prog; fn; fn = fn->next) {
    if (!fn->is_function || !fn->is_definition)
      continue;

    int offset = 0;
    for (Obj *var = fn->locals; var; var = var->next) {
      offset += var->ty->size;
      offset = align_to(offset, var->align);
      var->offset = -offset;
    }

    // The frame is a multiple of 16 so that RSP stays 16-byte aligned inside
    // the body, which every call depends on.
    fn->stack_size = align_to(offset, 16);
  }
}

//
// Data
//

static void emit_data(Obj *prog, FILE *out) {
  for (Obj *var = prog; var; var = var->next) {
    if (var->is_function || !var->is_definition)
      continue;

    if (!var->is_static)
      println("  .globl %s", var->name);

    if (!var->init_data) {
      // No initialiser: reserve zeroed space in .bss rather than emitting
      // thousands of zero bytes into the object file.
      println("  .bss");
      println("  .balign %d", var->align);
      println("%s:", var->name);
      println("  .zero %d", var->ty->size);
      continue;
    }

    println("  .data");
    println("  .balign %d", var->align);
    println("%s:", var->name);

    Relocation *rel = var->rel;
    int pos = 0;
    while (pos < var->ty->size) {
      if (rel && rel->offset == pos) {
        // The value is an address, which only the linker knows. Emit the
        // symbol and let it fill in the number.
        println("  .quad %s%+ld", *rel->label, rel->addend);
        rel = rel->next;
        pos += 8;
      } else {
        println("  .byte %d", (unsigned char)var->init_data[pos++]);
      }
    }
  }
}

//
// Functions
//

static void store_param(int slot, int offset, int sz, bool is_float,
                        Type *ty) {
  if (is_float) {
    if (ty->kind == TY_FLOAT)
      println("  movss %%xmm%d, %d(%%rbp)", slot, offset);
    else
      println("  movsd %%xmm%d, %d(%%rbp)", slot, offset);
    return;
  }

  switch (sz) {
  case 1:
    println("  mov %s, %d(%%rbp)", argreg8[slot], offset);
    return;
  case 2:
    println("  mov %s, %d(%%rbp)", argreg16[slot], offset);
    return;
  case 4:
    println("  mov %s, %d(%%rbp)", argreg32[slot], offset);
    return;
  default:
    println("  mov %s, %d(%%rbp)", argreg64[slot], offset);
    return;
  }
}

static void emit_text(Obj *prog, FILE *out) {
  for (Obj *fn = prog; fn; fn = fn->next) {
    if (!fn->is_function || !fn->is_definition)
      continue;

    // A static function is simply one without .globl. GAS's COFF backend has
    // no .local directive at all, unlike its ELF one.
    if (!fn->is_static)
      println("  .globl %s", fn->name);

    println("  .text");
    // The .def block is what makes the linker treat the symbol as a function
    // rather than data, which COFF cares about and ELF does not.
    println("  .def %s; .scl 2; .type 32; .endef", fn->name);
    println("%s:", fn->name);
    current_fn = fn;

    // Prologue.
    println("  push %%rbp");
    println("  mov %%rsp, %%rbp");
    println("  sub $%d, %%rsp", fn->stack_size);

    // Callee-saved registers this generator touches. RDI is callee-saved on
    // Windows -- it is an argument register on System V -- and the code above
    // uses it freely as a scratch register, so it must be preserved here.
    println("  push %%rdi");
    println("  push %%rsi");

    // Spill the register arguments into their stack homes. Every parameter is
    // addressed through the frame from here on, which is what lets `&param`
    // work.
    int slot = 0;
    for (Obj *var = fn->params; var; var = var->next) {
      if (slot >= GP_MAX)
        break;

      if (is_by_pointer(var->ty)) {
        // The caller passed a pointer to its copy. Copy it into this frame so
        // that modifying the parameter cannot be seen by the caller.
        println("  mov %s, %%rsi", argreg64[slot]);
        println("  lea %d(%%rbp), %%rdi", var->offset);
        for (int i = 0; i < var->ty->size; i++) {
          println("  mov %d(%%rsi), %%r10b", i);
          println("  mov %%r10b, %d(%%rdi)", i);
        }
      } else if (var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION) {
        store_param(slot, var->offset, var->ty->size, false, var->ty);
      } else {
        store_param(slot, var->offset, var->ty->size, is_flonum(var->ty),
                    var->ty);
      }
      slot++;
    }

    // Arguments beyond the fourth were left on the caller's stack. They start
    // above the return address and the saved RBP, at rbp+48: 16 for those two
    // plus the 32 bytes of shadow space.
    int stack_slot = 0;
    for (Obj *var = fn->params; var; var = var->next) {
      if (stack_slot >= GP_MAX) {
        int src = 16 + stack_slot * 8;
        println("  mov %d(%%rbp), %%rax", src);
        if (var->ty->size == 1)
          println("  mov %%al, %d(%%rbp)", var->offset);
        else if (var->ty->size == 2)
          println("  mov %%ax, %d(%%rbp)", var->offset);
        else if (var->ty->size == 4)
          println("  mov %%eax, %d(%%rbp)", var->offset);
        else
          println("  mov %%rax, %d(%%rbp)", var->offset);
      }
      stack_slot++;
    }

    gen_stmt(fn->body);

    // Every push must have been matched by a pop, or the stack pointer is
    // wrong and the failure will look like a mysterious crash somewhere else.
    // Checking here turns that into an immediate internal error.
    if (depth != 0)
      error("internal error: stack depth is %d at the end of %s", depth,
            fn->name);

    // C says falling off the end of main returns 0. This has to sit BEFORE the
    // return label, so that only fall-through reaches it: after the label it
    // would overwrite the value of every explicit `return` as well.
    if (!strcmp(fn->name, "main"))
      println("  mov $0, %%eax");

    println(".L.return.%s:", fn->name);
    println("  pop %%rsi");
    println("  pop %%rdi");
    println("  mov %%rbp, %%rsp");
    println("  pop %%rbp");
    println("  ret");
  }
}

void codegen(Obj *prog, FILE *out) {
  output_file = out;

  assign_lvar_offsets(prog);
  emit_data(prog, out);
  emit_text(prog, out);
}
