/**
 * See Copyright Notice in picrin.h
 */

#ifndef PICRIN_VM_H
#define PICRIN_VM_H

#if defined(__cplusplus)
extern "C" {
#endif

enum {
  OP_NOP,
  OP_POP,
  OP_PUSHUNDEF,
  OP_PUSHNIL,
  OP_PUSHTRUE,
  OP_PUSHFALSE,
  OP_PUSHINT,
  OP_PUSHFLOAT,
  OP_PUSHCHAR,
  OP_PUSHEOF,
  OP_PUSHCONST,
  OP_GREF,
  OP_GSET,
  OP_LREF,
  OP_LSET,
  OP_CREF,
  OP_CSET,
  OP_JMP,
  OP_JMPIF,
  OP_NOT,
  OP_CALL,
  OP_TAILCALL,
  OP_RET,
  OP_LAMBDA,
  OP_CONS,
  OP_CAR,
  OP_CDR,
  OP_NILP,
  OP_SYMBOLP,
  OP_PAIRP,
  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
  OP_EQ,
  OP_LT,
  OP_LE,
  OP_GT,
  OP_GE,
  OP_STOP
};

typedef struct {
  int insn;
  int a;
  int b;
} pic_code;

struct pic_list {
  struct pic_list *prev, *next;
};

struct pic_irep {
  struct pic_list list;
  unsigned refc;
  int argc, localc, capturec;
  bool varg;
  pic_code *code;
  struct pic_irep **irep;
  int *ints;
  double *nums;
  struct pic_object **pool;
  size_t ncode, nirep, nints, nnums, npool;
};

void pic_irep_incref(pic_state *, struct pic_irep *);
void pic_irep_decref(pic_state *, struct pic_irep *);

#if defined(__cplusplus)
}
#endif

#endif
