/**
 * See Copyright Notice in picrin.h
 */

#ifndef PICRIN_OBJECT_H
#define PICRIN_OBJECT_H

#if defined(__cplusplus)
extern "C" {
#endif

#include "picrin/private/khash.h"

typedef struct pic_identifier identifier;
typedef identifier symbol;

KHASH_DECLARE(env, identifier *, symbol *)
KHASH_DECLARE(dict, symbol *, pic_value)
KHASH_DECLARE(weak, struct pic_object *, pic_value)

#define PIC_OBJECT_HEADER			\
  unsigned char tt;                             \
  char gc_mark;

struct pic_object;              /* defined in gc.c */

struct pic_basic {
  PIC_OBJECT_HEADER
};

struct pic_identifier {
  PIC_OBJECT_HEADER
  union {
    struct pic_string *str;
    struct pic_identifier *id;
  } u;
  struct pic_env *env;
};

struct pic_env {
  PIC_OBJECT_HEADER
  khash_t(env) map;
  struct pic_env *up;
  struct pic_string *lib;
};

struct pic_pair {
  PIC_OBJECT_HEADER
  pic_value car;
  pic_value cdr;
};

struct pic_blob {
  PIC_OBJECT_HEADER
  unsigned char *data;
  int len;
};

struct pic_string {
  PIC_OBJECT_HEADER
  struct pic_rope *rope;
};

struct pic_dict {
  PIC_OBJECT_HEADER
  khash_t(dict) hash;
};

struct pic_weak {
  PIC_OBJECT_HEADER
  khash_t(weak) hash;
  struct pic_weak *prev;         /* for GC */
};

struct pic_vector {
  PIC_OBJECT_HEADER
  pic_value *data;
  int len;
};

struct pic_data {
  PIC_OBJECT_HEADER
  const pic_data_type *type;
  void *data;
};

struct pic_context {
  PIC_OBJECT_HEADER
  pic_value *regs;
  int regc;
  struct pic_context *up;
  pic_value storage[1];
};

struct pic_proc {
  PIC_OBJECT_HEADER
  enum {
    PIC_PROC_TAG_IREP,
    PIC_PROC_TAG_FUNC
  } tag;
  union {
    struct {
      pic_func_t func;
      int localc;
    } f;
    struct {
      struct pic_irep *irep;
      struct pic_context *cxt;
    } i;
  } u;
  pic_value locals[1];
};

struct pic_record {
  PIC_OBJECT_HEADER
  pic_value type;
  pic_value datum;
};

struct pic_error {
  PIC_OBJECT_HEADER
  symbol *type;
  struct pic_string *msg;
  pic_value irrs;
  struct pic_string *stack;
};

struct pic_port {
  PIC_OBJECT_HEADER
  xFILE *file;
};

struct pic_checkpoint {
  PIC_OBJECT_HEADER
  struct pic_proc *in;
  struct pic_proc *out;
  int depth;
  struct pic_checkpoint *prev;
};

struct pic_object *pic_obj_ptr(pic_value);

#define pic_id_ptr(pic, o) (assert(pic_id_p(pic, o)), (identifier *)pic_obj_ptr(o))
#define pic_sym_ptr(pic, o) (assert(pic_sym_p(pic, o)), (symbol *)pic_obj_ptr(o))
#define pic_str_ptr(pic, o) (assert(pic_str_p(pic, o)), (struct pic_string *)pic_obj_ptr(o))
#define pic_blob_ptr(pic, o) (assert(pic_blob_p(pic, o)), (struct pic_blob *)pic_obj_ptr(o))
#define pic_pair_ptr(pic, o) (assert(pic_pair_p(pic, o)), (struct pic_pair *)pic_obj_ptr(o))
#define pic_vec_ptr(pic, o) (assert(pic_vec_p(pic, o)), (struct pic_vector *)pic_obj_ptr(o))
#define pic_dict_ptr(pic, o) (assert(pic_dict_p(pic, o)), (struct pic_dict *)pic_obj_ptr(o))
#define pic_weak_ptr(pic, o) (assert(pic_weak_p(pic, o)), (struct pic_weak *)pic_obj_ptr(o))
#define pic_data_ptr(pic, o) (assert(pic_data_p(pic, o, NULL)), (struct pic_data *)pic_obj_ptr(o))
#define pic_proc_ptr(pic, o) (assert(pic_proc_p(pic, o)), (struct pic_proc *)pic_obj_ptr(o))
#define pic_env_ptr(pic, o) (assert(pic_env_p(pic, o)), (struct pic_env *)pic_obj_ptr(o))
#define pic_port_ptr(pic, o) (assert(pic_port_p(pic, o)), (struct pic_port *)pic_obj_ptr(o))
#define pic_error_ptr(pic, o) (assert(pic_error_p(pic, o)), (struct pic_error *)pic_obj_ptr(o))
#define pic_rec_ptr(pic, o) (assert(pic_rec_p(pic, o)), (struct pic_record *)pic_obj_ptr(o))

#define pic_obj_p(pic,v) (pic_type(pic,v) > PIC_IVAL_END)
#define pic_env_p(pic, v) (pic_type(pic, v) == PIC_TYPE_ENV)
#define pic_error_p(pic, v) (pic_type(pic, v) == PIC_TYPE_ERROR)
#define pic_rec_p(pic, v) (pic_type(pic, v) == PIC_TYPE_RECORD)
#define pic_id_p(pic, v) (pic_type(pic, v) == PIC_TYPE_ID || pic_type(pic, v) == PIC_TYPE_SYMBOL)

pic_value pic_obj_value(void *ptr);
struct pic_object *pic_obj_alloc(pic_state *, size_t, int type);

#define VALID_INDEX(pic, len, i) do {                                   \
    if (i < 0 || len <= i) pic_errorf(pic, "index out of range: %d", i); \
  } while (0)
#define VALID_RANGE(pic, len, s, e) do {                                \
    if (s < 0 || len < s) pic_errorf(pic, "invalid start index: %d", s); \
    if (e < s || len < e) pic_errorf(pic, "invalid end index: %d", e);  \
  } while (0)
#define VALID_ATRANGE(pic, tolen, at, fromlen, s, e) do {       \
    VALID_INDEX(pic, tolen, at);                                \
    VALID_RANGE(pic, fromlen, s, e);                            \
    if (tolen - at < e - s) pic_errorf(pic, "invalid range");   \
  } while (0)

pic_value pic_make_identifier(pic_state *, pic_value id, pic_value env);
pic_value pic_make_proc(pic_state *, pic_func_t, int, pic_value *);
pic_value pic_make_proc_irep(pic_state *, struct pic_irep *, struct pic_context *);
pic_value pic_make_env(pic_state *, pic_value env);
pic_value pic_make_error(pic_state *, const char *type, const char *msg, pic_value irrs);
pic_value pic_make_rec(pic_state *, pic_value type, pic_value datum);

pic_value pic_add_identifier(pic_state *, pic_value id, pic_value env);
pic_value pic_put_identifier(pic_state *, pic_value id, pic_value uid, pic_value env);
pic_value pic_find_identifier(pic_state *, pic_value id, pic_value env);
pic_value pic_id_name(pic_state *, pic_value id);

void pic_rope_incref(pic_state *, struct pic_rope *);
void pic_rope_decref(pic_state *, struct pic_rope *);

#define pic_proc_func_p(proc) ((proc)->tag == PIC_PROC_TAG_FUNC)
#define pic_proc_irep_p(proc) ((proc)->tag == PIC_PROC_TAG_IREP)

void pic_wind(pic_state *, struct pic_checkpoint *, struct pic_checkpoint *);


#if defined(__cplusplus)
}
#endif

#endif
