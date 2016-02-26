/**
 * See Copyright Notice in picrin.h
 */

#include "picrin.h"
#include "picrin/extra.h"
#include "picrin/private/object.h"

struct chunk {
  int refcnt;
  const char *str;
  int len;
  char buf[1];
};

struct rope {
  int refcnt;
  int weight;
  bool isleaf;
  union {
    struct {
      struct chunk *chunk;
      int offset;
    } leaf;
    struct {
      struct rope *left, *right;
    } node;
  } u;
};

#define CHUNK_INCREF(c) do {                    \
    (c)->refcnt++;                              \
  } while (0)

#define CHUNK_DECREF(c) do {                    \
    if (! --(c)->refcnt) {                      \
      pic_free(pic, (c));                       \
    }                                           \
  } while (0)

struct rope *
pic_rope_incref(struct rope *rope) {
  rope->refcnt++;
  return rope;
}

void
pic_rope_decref(pic_state *pic, struct rope *rope) {
  if (! --rope->refcnt) {
    if (rope->isleaf) {
      CHUNK_DECREF(rope->u.leaf.chunk);
    } else {
      pic_rope_decref(pic, rope->u.node.left);
      pic_rope_decref(pic, rope->u.node.right);
    }
    pic_free(pic, rope);
  }
}

static struct chunk *
make_chunk(pic_state *pic, const char *str, int len)
{
  struct chunk *c;

  c = pic_malloc(pic, offsetof(struct chunk, buf) + len + 1);
  c->refcnt = 1;
  c->str = c->buf;
  c->len = len;
  c->buf[len] = 0;
  if (str) {
    memcpy(c->buf, str, len);
  }
  return c;
}

static struct chunk *
make_chunk_lit(pic_state *pic, const char *str, int len)
{
  struct chunk *c;

  c = pic_malloc(pic, offsetof(struct chunk, buf));
  c->refcnt = 1;
  c->str = (char *)str;
  c->len = len;

  return c;
}

static struct rope *
make_rope_leaf(pic_state *pic, struct chunk *c)
{
  struct rope *rope;

  rope = pic_malloc(pic, sizeof(struct rope));
  rope->refcnt = 1;
  rope->weight = c->len;
  rope->isleaf = true;
  rope->u.leaf.offset = 0;
  rope->u.leaf.chunk = c;          /* delegate ownership */

  return rope;
}

static struct rope *
make_rope_node(pic_state *pic, struct rope *left, struct rope *right)
{
  struct rope *rope;

  if (left == 0)
    return pic_rope_incref(right);
  if (right == 0)
    return pic_rope_incref(left);

  rope = pic_malloc(pic, sizeof(struct rope));
  rope->refcnt = 1;
  rope->weight = left->weight + right->weight;
  rope->isleaf = false;
  rope->u.node.left = pic_rope_incref(left);
  rope->u.node.right = pic_rope_incref(right);

  return rope;
}

static pic_value
make_str(pic_state *pic, struct rope *rope)
{
  struct string *str;

  str = (struct string *)pic_obj_alloc(pic, sizeof(struct string), PIC_TYPE_STRING);
  str->rope = rope;             /* delegate ownership */

  return pic_obj_value(str);
}

static struct rope *
merge(pic_state *pic, struct rope *left, struct rope *right)
{
  return make_rope_node(pic, left, right);
}

static struct rope *
slice(pic_state *pic, struct rope *rope, int i, int j)
{
  assert(i <= j);
  assert(j <= rope->weight);

  if (i == 0 && rope->weight == j) {
    return pic_rope_incref(rope);
  }

  if (rope->isleaf) {
    struct rope *y;

    y = make_rope_leaf(pic, rope->u.leaf.chunk);
    y->weight = j - i;
    y->u.leaf.offset = rope->u.leaf.offset + i;

    CHUNK_INCREF(rope->u.leaf.chunk);

    return y;
  }

  if (j <= rope->u.node.left->weight) {
    return slice(pic, rope->u.node.left, i, j);
  } else if (rope->u.node.left->weight <= i) {
    return slice(pic, rope->u.node.right, i - rope->u.node.left->weight, j - rope->u.node.left->weight);
  } else {
    struct rope *r, *l;

    l = slice(pic, rope->u.node.left, i, rope->u.node.left->weight);
    r = slice(pic, rope->u.node.right, 0, j - rope->u.node.left->weight);
    rope = merge(pic, l, r);

    pic_rope_decref(pic, l);
    pic_rope_decref(pic, r);

    return rope;
  }
}

static void
flatten(pic_state *pic, struct rope *rope, struct chunk *c, int offset)
{
  if (rope->isleaf) {
    memcpy(c->buf + offset, rope->u.leaf.chunk->str + rope->u.leaf.offset, rope->weight);
    CHUNK_DECREF(rope->u.leaf.chunk);
    rope->u.leaf.chunk = c;
    rope->u.leaf.offset = offset;
    CHUNK_INCREF(c);
  } else {
    flatten(pic, rope->u.node.left, c, offset);
    flatten(pic, rope->u.node.right, c, offset + rope->u.node.left->weight);

    pic_rope_decref(pic, rope->u.node.left);
    pic_rope_decref(pic, rope->u.node.right);
    rope->isleaf = true;
    rope->u.leaf.chunk = c;
    rope->u.leaf.offset = offset;
    CHUNK_INCREF(c);
  }
}

static const char *
rope_cstr(pic_state *pic, struct rope *rope)
{
  struct chunk *c;

  if (rope->isleaf && rope->u.leaf.offset == 0 && rope->weight == rope->u.leaf.chunk->len) {
    return rope->u.leaf.chunk->str; /* reuse cached chunk */
  }

  c = make_chunk(pic, 0, rope->weight);

  flatten(pic, rope, c, 0);

  CHUNK_DECREF(c);
  return c->str;
}

static void
str_update(pic_state *pic, pic_value dst, pic_value src)
{
  pic_rope_incref(pic_str_ptr(pic, src)->rope);
  pic_rope_decref(pic, pic_str_ptr(pic, dst)->rope);
  pic_str_ptr(pic, dst)->rope = pic_str_ptr(pic, src)->rope;
}

pic_value
pic_str_value(pic_state *pic, const char *str, int len)
{
  struct chunk *c;

  if (len > 0) {
    c = make_chunk(pic, str, len);
  } else {
    if (len == 0) {
      str = "";
    }
    c = make_chunk_lit(pic, str, -len);
  }
  return make_str(pic, make_rope_leaf(pic, c));
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

pic_value
pic_vstrf_value(pic_state *pic, const char *fmt, va_list ap)
{
  pic_value str;
  xFILE *file;
  const char *buf;
  int len;

  file = xfopen_buf(pic, NULL, 0, "w");

  xvfprintf(pic, file, fmt, ap);
  xfget_buf(pic, file, &buf, &len);
  str = pic_str_value(pic, buf, len);
  xfclose(pic, file);
  return str;
}

int
pic_str_len(pic_state *PIC_UNUSED(pic), pic_value str)
{
  return pic_str_ptr(pic, str)->rope->weight;
}

char
pic_str_ref(pic_state *PIC_UNUSED(pic), pic_value str, int i)
{
  struct rope *rope = pic_str_ptr(pic, str)->rope;

  while (i < rope->weight) {
    if (rope->isleaf) {
      return rope->u.leaf.chunk->str[rope->u.leaf.offset + i];
    }
    if (i < rope->u.node.left->weight) {
      rope = rope->u.node.left;
    } else {
      i -= rope->u.node.left->weight;
      rope = rope->u.node.right;
    }
  }
  PIC_UNREACHABLE();
}

pic_value
pic_str_cat(pic_state *pic, pic_value a, pic_value b)
{
  return make_str(pic, merge(pic, pic_str_ptr(pic, a)->rope, pic_str_ptr(pic, b)->rope));
}

pic_value
pic_str_sub(pic_state *pic, pic_value str, int s, int e)
{
  return make_str(pic, slice(pic, pic_str_ptr(pic, str)->rope, s, e));
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
    TYPE_CHECK(pic, argv[i], char);
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
    pic_error(pic, "make-string: negative length given", 1, pic_int_value(pic, len));
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
    TYPE_CHECK(pic, argv[i], str);
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
    pic_error(pic, "string-map: one or more strings expected, but got zero", 0);
  }

  len = INT_MAX;
  for (i = 0; i < argc; ++i) {
    int l;
    TYPE_CHECK(pic, argv[i], str);
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

    TYPE_CHECK(pic, val, char);

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
    pic_error(pic, "string-map: one or more strings expected, but got zero", 0);
  }

  len = INT_MAX;
  for (i = 0; i < argc; ++i) {
    int l;
    TYPE_CHECK(pic, argv[i], str);
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
    TYPE_CHECK(pic, e, char);

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
