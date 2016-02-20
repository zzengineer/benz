/**
 * See Copyright Notice in picrin.h
 */

#include "picrin.h"
#include "picrin/object.h"

void
pic_panic(pic_state PIC_UNUSED(*pic), const char *msg)
{
  extern PIC_NORETURN void abort();

#if DEBUG
  fprintf(stderr, "abort: %s\n", msg);
#else
  (void)msg;
#endif
  PIC_ABORT(pic);
}

void
pic_warnf(pic_state *pic, const char *fmt, ...)
{
  va_list ap;
  pic_value err;

  va_start(ap, fmt);
  err = pic_vstrf_value(pic, fmt, ap);
  va_end(ap);

  pic_fprintf(pic, pic_stderr(pic), "warn: %s\n", pic_str(pic, err));
}

void
pic_errorf(pic_state *pic, const char *fmt, ...)
{
  va_list ap;
  const char *msg;
  pic_value err;

  va_start(ap, fmt);
  err = pic_vstrf_value(pic, fmt, ap);
  va_end(ap);

  msg = pic_str(pic, err);

  pic_error(pic, "", msg, pic_nil_value(pic));
}

pic_value
pic_native_exception_handler(pic_state *pic)
{
  pic_value err;

  pic_get_args(pic, "o", &err);

  pic->err = err;

  pic_call(pic, pic_closure_ref(pic, 0), 1, pic_false_value(pic));

  PIC_UNREACHABLE();
}

void
pic_push_handler(pic_state *pic, pic_value handler)
{
  size_t xp_len;
  ptrdiff_t xp_offset;

  if (pic->xp >= pic->xpend) {
    xp_len = (size_t)(pic->xpend - pic->xpbase) * 2;
    xp_offset = pic->xp - pic->xpbase;
    pic->xpbase = pic_realloc(pic, pic->xpbase, sizeof(struct pic_proc *) * xp_len);
    pic->xp = pic->xpbase + xp_offset;
    pic->xpend = pic->xpbase + xp_len;
  }

  *pic->xp++ = pic_proc_ptr(pic, handler);
}

pic_value
pic_pop_handler(pic_state *pic)
{
  if (pic->xp == pic->xpbase) {
    pic_panic(pic, "no exception handler registered");
  }

  return pic_obj_value(*--pic->xp);
}

struct pic_error *
pic_make_error(pic_state *pic, const char *type, const char *msg, pic_value irrs)
{
  struct pic_error *e;
  pic_value stack, ty = pic_intern_cstr(pic, type);

  stack = pic_get_backtrace(pic);

  e = (struct pic_error *)pic_obj_alloc(pic, sizeof(struct pic_error), PIC_TYPE_ERROR);
  e->type = pic_sym_ptr(pic, ty);
  e->msg = pic_str_ptr(pic, pic_cstr_value(pic, msg));
  e->irrs = irrs;
  e->stack = pic_str_ptr(pic, stack);

  return e;
}

pic_value
pic_raise_continuable(pic_state *pic, pic_value err)
{
  pic_value handler, v;

  handler = pic_pop_handler(pic);

  pic_protect(pic, handler);

  v = pic_call(pic, handler, 1, err);

  pic_push_handler(pic, handler);

  return v;
}

void
pic_raise(pic_state *pic, pic_value err)
{
  pic_value val;

  val = pic_raise_continuable(pic, err);

  pic_pop_handler(pic);

  pic_errorf(pic, "error handler returned with ~s on error ~s", val, err);
}

void
pic_error(pic_state *pic, const char *type, const char *msg, pic_value irrs)
{
  struct pic_error *e;

  e = pic_make_error(pic, type, msg, irrs);

  pic_raise(pic, pic_obj_value(e));
}

static pic_value
pic_error_with_exception_handler(pic_state *pic)
{
  pic_value handler, thunk, val;

  pic_get_args(pic, "ll", &handler, &thunk);

  pic_push_handler(pic, handler);

  val = pic_call(pic, thunk, 0);

  pic_pop_handler(pic);

  return val;
}

static pic_value
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

static pic_value
pic_error_error(pic_state *pic)
{
  const char *str;
  int argc;
  pic_value *argv;

  pic_get_args(pic, "z*", &str, &argc, &argv);

  pic_error(pic, "", str, pic_make_list(pic, argc, argv));
}

static pic_value
pic_error_error_object_p(pic_state *pic)
{
  pic_value v;

  pic_get_args(pic, "o", &v);

  return pic_bool_value(pic, pic_error_p(pic, v));
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

  return pic_obj_value(e->type);
}

void
pic_init_error(pic_state *pic)
{
  pic_defun(pic, "with-exception-handler", pic_error_with_exception_handler);
  pic_defun(pic, "raise", pic_error_raise);
  pic_defun(pic, "raise-continuable", pic_error_raise_continuable);
  pic_defun(pic, "error", pic_error_error);
  pic_defun(pic, "error-object?", pic_error_error_object_p);
  pic_defun(pic, "error-object-message", pic_error_error_object_message);
  pic_defun(pic, "error-object-irritants", pic_error_error_object_irritants);
  pic_defun(pic, "error-object-type", pic_error_error_object_type);
}
