/**
 * See Copyright Notice in picrin.h
 */

#include "picrin.h"
#include "picrin/extra.h"
#include "picrin/private/object.h"

struct chunk {
  char *str;
  int refcnt;
  size_t len;
  char buf[1];
};

struct rope {
  int refcnt;
  size_t weight;
  struct chunk *chunk;
  size_t offset;
  struct rope *left, *right;
};

#define CHUNK_INCREF(c) do {                    \
    (c)->refcnt++;                              \
  } while (0)

#define CHUNK_DECREF(c) do {                    \
    struct chunk *c_ = (c);                 \
    if (! --c_->refcnt) {                       \
      pic_free(pic, c_);                        \
    }                                           \
  } while (0)

void
pic_rope_incref(pic_state *PIC_UNUSED(pic), struct rope *x) {
  x->refcnt++;
}

void
pic_rope_decref(pic_state *pic, struct rope *x) {
  if (! --x->refcnt) {
    if (x->chunk) {
      CHUNK_DECREF(x->chunk);
      pic_free(pic, x);
      return;
    }
    pic_rope_decref(pic, x->left);
    pic_rope_decref(pic, x->right);
    pic_free(pic, x);
  }
}

static struct chunk *
pic_make_chunk(pic_state *pic, const char *str, size_t len)
{
  struct chunk *c;

  c = pic_malloc(pic, offsetof(struct chunk, buf) + len + 1);
  c->refcnt = 1;
  c->str = c->buf;
  c->len = len;
  c->buf[len] = 0;
  memcpy(c->buf, str, len);

  return c;
}

static struct chunk *
pic_make_chunk_lit(pic_state *pic, const char *str, size_t len)
{
  struct chunk *c;

  c = pic_malloc(pic, sizeof(struct chunk));
  c->refcnt = 1;
  c->str = (char *)str;
  c->len = len;

  return c;
}

static struct rope *
pic_make_rope(pic_state *pic, struct chunk *c)
{
  struct rope *x;

  x = pic_malloc(pic, sizeof(struct rope));
  x->refcnt = 1;
  x->left = NULL;
  x->right = NULL;
  x->weight = c->len;
  x->offset = 0;
  x->chunk = c;                 /* delegate ownership */

  return x;
}

static pic_value
pic_make_str(pic_state *pic, struct rope *rope)
{
  struct string *str;

  str = (struct string *)pic_obj_alloc(pic, sizeof(struct string), PIC_TYPE_STRING);
  str->rope = rope;             /* delegate ownership */

  return pic_obj_value(str);
}

static size_t
rope_len(struct rope *x)
{
  return x->weight;
}

static char
rope_at(struct rope *x, size_t i)
{
  while (i < x->weight) {
    if (x->chunk) {
      return x->chunk->str[x->offset + i];
    }
    if (i < x->left->weight) {
      x = x->left;
    } else {
      i -= x->left->weight;
      x = x->right;
    }
  }
  return -1;
}

static struct rope *
rope_cat(pic_state *pic, struct rope *x, struct rope *y)
{
  struct rope *z;

  z = pic_malloc(pic, sizeof(struct rope));
  z->refcnt = 1;
  z->left = x;
  z->right = y;
  z->weight = x->weight + y->weight;
  z->offset = 0;
  z->chunk = NULL;

  pic_rope_incref(pic, x);
  pic_rope_incref(pic, y);

  return z;
}

static struct rope *
rope_sub(pic_state *pic, struct rope *x, size_t i, size_t j)
{
  assert(i <= j);
  assert(j <= x->weight);

  if (i == 0 && x->weight == j) {
    pic_rope_incref(pic, x);
    return x;
  }

  if (x->chunk) {
    struct rope *y;

    y = pic_malloc(pic, sizeof(struct rope));
    y->refcnt = 1;
    y->left = NULL;
    y->right = NULL;
    y->weight = j - i;
    y->offset = x->offset + i;
    y->chunk = x->chunk;

    CHUNK_INCREF(x->chunk);

    return y;
  }

  if (j <= x->left->weight) {
    return rope_sub(pic, x->left, i, j);
  }
  else if (x->left->weight <= i) {
    return rope_sub(pic, x->right, i - x->left->weight, j - x->left->weight);
  }
  else {
    struct rope *r, *l;

    l = rope_sub(pic, x->left, i, x->left->weight);
    r = rope_sub(pic, x->right, 0, j - x->left->weight);
    x = rope_cat(pic, l, r);

    pic_rope_decref(pic, l);
    pic_rope_decref(pic, r);

    return x;
  }
}

static void
flatten(pic_state *pic, struct rope *x, struct chunk *c, size_t offset)
{
  if (x->chunk) {
    memcpy(c->str + offset, x->chunk->str + x->offset, x->weight);
    CHUNK_DECREF(x->chunk);

    x->chunk = c;
    x->offset = offset;
    CHUNK_INCREF(c);
    return;
  }
  flatten(pic, x->left, c, offset);
  flatten(pic, x->right, c, offset + x->left->weight);

  pic_rope_decref(pic, x->left);
  pic_rope_decref(pic, x->right);
  x->left = x->right = NULL;
  x->chunk = c;
  x->offset = offset;
  CHUNK_INCREF(c);
}

static const char *
rope_cstr(pic_state *pic, struct rope *x)
{
  struct chunk *c;

  if (x->chunk && x->offset == 0 && x->weight == x->chunk->len) {
    return x->chunk->str;       /* reuse cached chunk */
  }

  c = pic_malloc(pic, offsetof(struct chunk, buf) + x->weight + 1);
  c->refcnt = 1;
  c->len = x->weight;
  c->str = c->buf;
  c->str[c->len] = '\0';

  flatten(pic, x, c, 0);

  CHUNK_DECREF(c);
  return c->str;
}

static void
str_update(pic_state *pic, pic_value dst, pic_value src)
{
  pic_rope_incref(pic, pic_str_ptr(pic, src)->rope);
  pic_rope_decref(pic, pic_str_ptr(pic, dst)->rope);
  pic_str_ptr(pic, dst)->rope = pic_str_ptr(pic, src)->rope;
}

pic_value
pic_str_value(pic_state *pic, const char *str, int len)
{
  struct chunk *c;

  if (len > 0) {
    c = pic_make_chunk(pic, str, len);
  } else {
    if (len == 0) {
      str = "";
    }
    c = pic_make_chunk_lit(pic, str, -len);
  }
  return pic_make_str(pic, pic_make_rope(pic, c));
}

int
pic_str_len(pic_state *PIC_UNUSED(pic), pic_value str)
{
  return rope_len(pic_str_ptr(pic, str)->rope);
}

char
pic_str_ref(pic_state *pic, pic_value str, int i)
{
  int c;

  c = rope_at(pic_str_ptr(pic, str)->rope, i);
  if (c == -1) {
    pic_errorf(pic, "index out of range %d", i);
  }
  return (char)c;
}

pic_value
pic_str_cat(pic_state *pic, pic_value a, pic_value b)
{
  return pic_make_str(pic, rope_cat(pic, pic_str_ptr(pic, a)->rope, pic_str_ptr(pic, b)->rope));
}

pic_value
pic_str_sub(pic_state *pic, pic_value str, int s, int e)
{
  return pic_make_str(pic, rope_sub(pic, pic_str_ptr(pic, str)->rope, s, e));
}

int
pic_str_cmp(pic_state *pic, pic_value str1, pic_value str2)
{
  return strcmp(pic_str(pic, str1), pic_str(pic, str2));
}

int
pic_str_hash(pic_state *pic, pic_value str)
{
  const char *s;
  int h = 0;

  s = pic_str(pic, str);
  while (*s) {
    h = (h << 5) - h + *s++;
  }
  return h;
}

const char *
pic_str(pic_state *pic, pic_value str)
{
  return rope_cstr(pic, pic_str_ptr(pic, str)->rope);
}

static void
vfstrf(pic_state *pic, xFILE *file, const char *fmt, va_list ap)
{
  char c;

  while ((c = *fmt++) != '\0') {
    switch (c) {
    default:
      xfputc(pic, c, file);
      break;
    case '%':
      c = *fmt++;
      if (! c)
        goto exit;
      switch (c) {
      default:
        xfputc(pic, c, file);
        break;
      case '%':
        xfputc(pic, '%', file);
        break;
      case 'c':
        xfprintf(pic, file, "%c", va_arg(ap, int));
        break;
      case 's':
        xfprintf(pic, file, "%s", va_arg(ap, const char *));
        break;
      case 'd':
        xfprintf(pic, file, "%d", va_arg(ap, int));
        break;
      case 'p':
        xfprintf(pic, file, "%p", va_arg(ap, void *));
        break;
      case 'f':
        xfprintf(pic, file, "%f", va_arg(ap, double));
        break;
      }
      break;
    case '~':
      c = *fmt++;
      if (! c)
        goto exit;
      switch (c) {
      default:
        xfputc(pic, c, file);
        break;
      case '~':
        xfputc(pic, '~', file);
        break;
      case '%':
        xfputc(pic, '\n', file);
        break;
      case 'a':
        pic_fdisplay(pic, va_arg(ap, pic_value), file);
        break;
      case 's':
        pic_fwrite(pic, va_arg(ap, pic_value), file);
        break;
      }
      break;
    }
  }
 exit:
  return;
}

pic_value
pic_vstrf_value(pic_state *pic, const char *fmt, va_list ap)
{
  pic_value str;
  xFILE *file;
  const char *buf;
  int len;

  file = xfopen_buf(pic, NULL, 0, "w");

  vfstrf(pic, file, fmt, ap);
  xfget_buf(pic, file, &buf, &len);
  str = pic_str_value(pic, buf, len);
  xfclose(pic, file);
  return str;
}

pic_value
pic_strf_value(pic_state *pic, const char *fmt, ...)
{
  va_list ap;
  pic_value str;

  va_start(ap, fmt);
  str = pic_vstrf_value(pic, fmt, ap);
  va_end(ap);

  return str;
}

static pic_value
pic_str_string_p(pic_state *pic)
{
  pic_value v;

  pic_get_args(pic, "o", &v);

  return pic_bool_value(pic, pic_str_p(pic, v));
}

static pic_value
pic_str_string(pic_state *pic)
{
  int argc, i;
  pic_value *argv;
  char *buf;

  pic_get_args(pic, "*", &argc, &argv);

  buf = pic_alloca(pic, argc);

  for (i = 0; i < argc; ++i) {
    pic_assert_type(pic, argv[i], char);
    buf[i] = pic_char(pic, argv[i]);
  }

  return pic_str_value(pic, buf, argc);
}

static pic_value
pic_str_make_string(pic_state *pic)
{
  int len;
  char c = ' ';
  char *buf;

  pic_get_args(pic, "i|c", &len, &c);

  if (len < 0) {
    pic_errorf(pic, "make-string: negative length given %d", len);
  }

  buf = pic_alloca(pic, len);

  memset(buf, c, len);

  return pic_str_value(pic, buf, len);
}

static pic_value
pic_str_string_length(pic_state *pic)
{
  pic_value str;

  pic_get_args(pic, "s", &str);

  return pic_int_value(pic, pic_str_len(pic, str));
}

static pic_value
pic_str_string_ref(pic_state *pic)
{
  pic_value str;
  int k;

  pic_get_args(pic, "si", &str, &k);

  VALID_INDEX(pic, pic_str_len(pic, str), k);

  return pic_char_value(pic, pic_str_ref(pic, str, k));
}

static pic_value
pic_str_string_set(pic_state *pic)
{
  pic_value str, x, y, z;
  char c;
  int k, len;

  pic_get_args(pic, "sic", &str, &k, &c);

  len = pic_str_len(pic, str);

  VALID_INDEX(pic, len, k);

  x = pic_str_sub(pic, str, 0, k);
  y = pic_str_value(pic, &c, 1);
  z = pic_str_sub(pic, str, k + 1, len);

  str_update(pic, str, pic_str_cat(pic, x, pic_str_cat(pic, y, z)));

  return pic_undef_value(pic);
}

#define DEFINE_STRING_CMP(name, op)                             \
  static pic_value                                              \
  pic_str_string_##name(pic_state *pic)                         \
  {                                                             \
    int argc, i;                                                \
    pic_value *argv;                                            \
                                                                \
    pic_get_args(pic, "*", &argc, &argv);                       \
                                                                \
    if (argc < 1 || ! pic_str_p(pic, argv[0])) {                \
      return pic_false_value(pic);                              \
    }                                                           \
                                                                \
    for (i = 1; i < argc; ++i) {                                \
      if (! pic_str_p(pic, argv[i])) {                          \
        return pic_false_value(pic);                            \
      }                                                         \
      if (! (pic_str_cmp(pic, argv[i-1], argv[i]) op 0)) {      \
        return pic_false_value(pic);                            \
      }                                                         \
    }                                                           \
    return pic_true_value(pic);                                 \
  }

DEFINE_STRING_CMP(eq, ==)
DEFINE_STRING_CMP(lt, <)
DEFINE_STRING_CMP(gt, >)
DEFINE_STRING_CMP(le, <=)
DEFINE_STRING_CMP(ge, >=)

static pic_value
pic_str_string_copy(pic_state *pic)
{
  pic_value str;
  int n, start, end, len;

  n = pic_get_args(pic, "s|ii", &str, &start, &end);

  len = pic_str_len(pic, str);

  switch (n) {
  case 1:
    start = 0;
  case 2:
    end = len;
  }

  VALID_RANGE(pic, len, start, end);

  return pic_str_sub(pic, str, start, end);
}

static pic_value
pic_str_string_copy_ip(pic_state *pic)
{
  pic_value to, from, x, y, z;
  int n, at, start, end, tolen, fromlen;

  n = pic_get_args(pic, "sis|ii", &to, &at, &from, &start, &end);

  tolen = pic_str_len(pic, to);
  fromlen = pic_str_len(pic, from);

  switch (n) {
  case 3:
    start = 0;
  case 4:
    end = fromlen;
  }

  VALID_ATRANGE(pic, tolen, at, fromlen, start, end);

  x = pic_str_sub(pic, to, 0, at);
  y = pic_str_sub(pic, from, start, end);
  z = pic_str_sub(pic, to, at + end - start, tolen);

  str_update(pic, to, pic_str_cat(pic, x, pic_str_cat(pic, y, z)));

  return pic_undef_value(pic);
}

static pic_value
pic_str_string_fill_ip(pic_state *pic)
{
  pic_value str, x, y, z;
  char c, *buf;
  int n, start, end, len;

  n = pic_get_args(pic, "sc|ii", &str, &c, &start, &end);

  len = pic_str_len(pic, str);

  switch (n) {
  case 2:
    start = 0;
  case 3:
    end = len;
  }

  VALID_RANGE(pic, len, start, end);

  buf = pic_alloca(pic, end - start);
  memset(buf, c, end - start);

  x = pic_str_sub(pic, str, 0, start);
  y = pic_str_value(pic, buf, end - start);
  z = pic_str_sub(pic, str, end, len);

  str_update(pic, str, pic_str_cat(pic, x, pic_str_cat(pic, y, z)));

  return pic_undef_value(pic);
}

static pic_value
pic_str_string_append(pic_state *pic)
{
  int argc, i;
  pic_value *argv;
  pic_value str = pic_lit_value(pic, "");

  pic_get_args(pic, "*", &argc, &argv);

  for (i = 0; i < argc; ++i) {
    pic_assert_type(pic, argv[i], str);
    str = pic_str_cat(pic, str, argv[i]);
  }
  return str;
}

static pic_value
pic_str_string_map(pic_state *pic)
{
  pic_value proc, *argv, vals, val;
  int argc, i, len, j;
  char *buf;

  pic_get_args(pic, "l*", &proc, &argc, &argv);

  if (argc == 0) {
    pic_errorf(pic, "string-map: one or more strings expected, but got zero");
  }

  len = INT_MAX;
  for (i = 0; i < argc; ++i) {
    int l;
    pic_assert_type(pic, argv[i], str);
    l = pic_str_len(pic, argv[i]);
    len = len < l ? len : l;
  }

  buf = pic_alloca(pic, len);

  for (i = 0; i < len; ++i) {
    vals = pic_nil_value(pic);
    for (j = 0; j < argc; ++j) {
      pic_push(pic, pic_char_value(pic, pic_str_ref(pic, argv[j], i)), vals);
    }
    vals = pic_reverse(pic, vals);
    val = pic_funcall(pic, "picrin.base", "apply", 2, proc, vals);

    pic_assert_type(pic, val, char);

    buf[i] = pic_char(pic, val);
  }
  return pic_str_value(pic, buf, len);
}

static pic_value
pic_str_string_for_each(pic_state *pic)
{
  pic_value proc, *argv, vals;
  int argc, i, len, j;

  pic_get_args(pic, "l*", &proc, &argc, &argv);

  if (argc == 0) {
    pic_errorf(pic, "string-map: one or more strings expected, but got zero");
  }

  len = INT_MAX;
  for (i = 0; i < argc; ++i) {
    int l;
    pic_assert_type(pic, argv[i], str);
    l = pic_str_len(pic, argv[i]);
    len = len < l ? len : l;
  }

  for (i = 0; i < len; ++i) {
    vals = pic_nil_value(pic);
    for (j = 0; j < argc; ++j) {
      pic_push(pic, pic_char_value(pic, pic_str_ref(pic, argv[j], i)), vals);
    }
    vals = pic_reverse(pic, vals);
    pic_funcall(pic, "picrin.base", "apply", 2, proc, vals);
  }
  return pic_undef_value(pic);
}

static pic_value
pic_str_list_to_string(pic_state *pic)
{
  pic_value list, e, it;
  int i;
  char *buf;

  pic_get_args(pic, "o", &list);

  buf = pic_alloca(pic, pic_length(pic, list));

  i = 0;
  pic_for_each (e, list, it) {
    pic_assert_type(pic, e, char);

    buf[i++] = pic_char(pic, e);
  }

  return pic_str_value(pic, buf, i);
}

static pic_value
pic_str_string_to_list(pic_state *pic)
{
  pic_value str, list;
  int n, start, end, len, i;

  n = pic_get_args(pic, "s|ii", &str, &start, &end);

  len = pic_str_len(pic, str);

  switch (n) {
  case 1:
    start = 0;
  case 2:
    end = len;
  }

  VALID_RANGE(pic, len, start, end);

  list = pic_nil_value(pic);
  for (i = start; i < end; ++i) {
    pic_push(pic, pic_char_value(pic, pic_str_ref(pic, str, i)), list);
  }
  return pic_reverse(pic, list);
}
 
void
pic_init_str(pic_state *pic)
{
  pic_defun(pic, "string?", pic_str_string_p);
  pic_defun(pic, "string", pic_str_string);
  pic_defun(pic, "make-string", pic_str_make_string);
  pic_defun(pic, "string-length", pic_str_string_length);
  pic_defun(pic, "string-ref", pic_str_string_ref);
  pic_defun(pic, "string-set!", pic_str_string_set);
  pic_defun(pic, "string-copy", pic_str_string_copy);
  pic_defun(pic, "string-copy!", pic_str_string_copy_ip);
  pic_defun(pic, "string-fill!", pic_str_string_fill_ip);
  pic_defun(pic, "string-append", pic_str_string_append);
  pic_defun(pic, "string-map", pic_str_string_map);
  pic_defun(pic, "string-for-each", pic_str_string_for_each);
  pic_defun(pic, "list->string", pic_str_list_to_string);
  pic_defun(pic, "string->list", pic_str_string_to_list);

  pic_defun(pic, "string=?", pic_str_string_eq);
  pic_defun(pic, "string<?", pic_str_string_lt);
  pic_defun(pic, "string>?", pic_str_string_gt);
  pic_defun(pic, "string<=?", pic_str_string_le);
  pic_defun(pic, "string>=?", pic_str_string_ge);
}
