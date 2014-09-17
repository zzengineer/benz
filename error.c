/**
 * See Copyright Notice in picrin.h
 */

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "picrin.h"
#include "picrin/pair.h"
#include "picrin/string.h"
#include "picrin/error.h"

void
pic_panic(pic_state *pic, const char *msg)
{
  UNUSED(pic);

  fprintf(stderr, "abort: %s\n", msg);
  abort();
}

void
pic_warnf(pic_state *pic, const char *fmt, ...)
{
  va_list ap;
  pic_value err_line;

  va_start(ap, fmt);
  err_line = pic_xvformat(pic, fmt, ap);
  va_end(ap);

  fprintf(stderr, "warn: %s\n", pic_str_cstr(pic_str_ptr(pic_car(pic, err_line))));
}

void
pic_errorf(pic_state *pic, const char *fmt, ...)
{
  va_list ap;
  pic_value err_line, irrs;
  const char *msg;

  va_start(ap, fmt);
  err_line = pic_xvformat(pic, fmt, ap);
  va_end(ap);

  msg = pic_str_cstr(pic_str_ptr(pic_car(pic, err_line)));
  irrs = pic_cdr(pic, err_line);

  pic_error(pic, msg, irrs);
}

const char *
pic_errmsg(pic_state *pic)
{
  pic_str *str;

  assert(! pic_undef_p(pic->err));

  if (! pic_error_p(pic->err)) {
    str = pic_format(pic, "~s", pic->err);
  } else {
    str = pic_error_ptr(pic->err)->msg;
  }

  return pic_str_cstr(str);
}

void
pic_push_try(pic_state *pic, struct pic_proc *handler)
{
  struct pic_jmpbuf *try_jmp;

  if (pic->try_jmp_idx >= pic->try_jmp_size) {
    pic->try_jmp_size *= 2;
    pic->try_jmps = pic_realloc(pic, pic->try_jmps, sizeof(struct pic_jmpbuf) * pic->try_jmp_size);
  }

  try_jmp = pic->try_jmps + pic->try_jmp_idx++;

  try_jmp->handler = handler;

  try_jmp->ci_offset = pic->ci - pic->cibase;
  try_jmp->sp_offset = pic->sp - pic->stbase;
  try_jmp->ip = pic->ip;

  try_jmp->prev_jmp = pic->jmp;
  pic->jmp = &try_jmp->here;
}

void
pic_pop_try(pic_state *pic)
{
  struct pic_jmpbuf *try_jmp;

  try_jmp = pic->try_jmps + --pic->try_jmp_idx;

  /* assert(pic->jmp == &try_jmp->here); */

  pic->ci = try_jmp->ci_offset + pic->cibase;
  pic->sp = try_jmp->sp_offset + pic->stbase;
  pic->ip = try_jmp->ip;

  pic->jmp = try_jmp->prev_jmp;
}

struct pic_error *
pic_make_error(pic_state *pic, pic_sym type, const char *msg, pic_value irrs)
{
  struct pic_error *e;
  pic_str *stack;

  stack = pic_get_backtrace(pic);

  e = (struct pic_error *)pic_obj_alloc(pic, sizeof(struct pic_error), PIC_TT_ERROR);
  e->type = type;
  e->msg = pic_make_str_cstr(pic, msg);
  e->irrs = irrs;
  e->stack = stack;

  return e;
}

pic_value
pic_raise_continuable(pic_state *pic, pic_value err)
{
  pic_value v;

  if (pic->try_jmp_idx == 0) {
    pic_errorf(pic, "no exception handler registered");
  }
  if (pic->try_jmps[pic->try_jmp_idx - 1].handler == NULL) {
    pic_errorf(pic, "uncontinuable exception handler is on top");
  }
  else {
    pic->try_jmp_idx--;
    v = pic_apply1(pic, pic->try_jmps[pic->try_jmp_idx].handler, err);
    ++pic->try_jmp_idx;
  }
  return v;
}

noreturn void
pic_raise(pic_state *pic, pic_value err)
{
  void pic_vm_tear_off(pic_state *);

  pic_vm_tear_off(pic);         /* tear off */

  pic->err = err;
  if (! pic->jmp) {
    puts(pic_errmsg(pic));
    pic_panic(pic, "no handler found on stack");
  }

  longjmp(*pic->jmp, 1);
}

noreturn void
pic_throw(pic_state *pic, pic_sym type, const char *msg, pic_value irrs)
{
  struct pic_error *e;

  e = pic_make_error(pic, type, msg, irrs);

  pic_raise(pic, pic_obj_value(e));
}

noreturn void
pic_error(pic_state *pic, const char *msg, pic_value irrs)
{
  pic_throw(pic, pic_intern_cstr(pic, ""), msg, irrs);
}

static pic_value
pic_error_with_exception_handler(pic_state *pic)
{
  struct pic_proc *handler, *thunk;
  pic_value val;

  pic_get_args(pic, "ll", &handler, &thunk);

  pic_push_try(pic, handler);
  if (setjmp(*pic->jmp) == 0) {

    val = pic_apply0(pic, thunk);

    pic_pop_try(pic);
  }
  else {
    pic_pop_try(pic);

    pic_value e = pic->err;

    pic->err = pic_undef_value();

    val = pic_apply1(pic, handler, e);

    pic_errorf(pic, "error handler returned with ~s on error ~s", val, e);
  }
  return val;
}

noreturn static pic_value
pic_error_raise(pic_state *pic)
{
  pic_value v;

  pic_get_args(pic, "o", &v);

  pic_raise(pic, v);
}

static pic_value
pic_error_raise_continuable(pic_state *pic)
{
  pic_value v;

  pic_get_args(pic, "o", &v);

  return pic_raise_continuable(pic, v);
}

noreturn static pic_value
pic_error_error(pic_state *pic)
{
  const char *str;
  size_t argc;
  pic_value *argv;

  pic_get_args(pic, "z*", &str, &argc, &argv);

  pic_error(pic, str, pic_list_by_array(pic, argc, argv));
}

static pic_value
pic_error_make_error_object(pic_state *pic)
{
  struct pic_error *e;
  pic_sym type;
  pic_str *msg;
  size_t argc;
  pic_value *argv;

  pic_get_args(pic, "ms*", &type, &msg, &argc, &argv);

  e = pic_make_error(pic, type, pic_str_cstr(msg), pic_list_by_array(pic, argc, argv));

  return pic_obj_value(e);
}

static pic_value
pic_error_error_object_p(pic_state *pic)
{
  pic_value v;

  pic_get_args(pic, "o", &v);

  return pic_bool_value(pic_error_p(v));
}

static pic_value
pic_error_error_object_message(pic_state *pic)
{
  struct pic_error *e;

  pic_get_args(pic, "e", &e);

  return pic_obj_value(e->msg);
}

static pic_value
pic_error_error_object_irritants(pic_state *pic)
{
  struct pic_error *e;

  pic_get_args(pic, "e", &e);

  return e->irrs;
}

static pic_value
pic_error_error_object_type(pic_state *pic)
{
  struct pic_error *e;

  pic_get_args(pic, "e", &e);

  return pic_sym_value(e->type);
}

void
pic_init_error(pic_state *pic)
{
  pic_defun(pic, "with-exception-handler", pic_error_with_exception_handler);
  pic_defun(pic, "raise", pic_error_raise);
  pic_defun(pic, "raise-continuable", pic_error_raise_continuable);
  pic_defun(pic, "error", pic_error_error);
  pic_defun(pic, "make-error-object", pic_error_make_error_object);
  pic_defun(pic, "error-object?", pic_error_error_object_p);
  pic_defun(pic, "error-object-message", pic_error_error_object_message);
  pic_defun(pic, "error-object-irritants", pic_error_error_object_irritants);
  pic_defun(pic, "error-object-type", pic_error_error_object_type);
}
