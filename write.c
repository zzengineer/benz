/**
 * See Copyright Notice in picrin.h
 */

#include "picrin.h"

KHASH_DECLARE(l, void *, int)
KHASH_DECLARE(v, void *, int)
KHASH_DEFINE2(l, void *, int, 1, kh_ptr_hash_func, kh_ptr_hash_equal)
KHASH_DEFINE2(v, void *, int, 0, kh_ptr_hash_func, kh_ptr_hash_equal)

struct writer_control {
  pic_state *pic;
  xFILE *file;
  int mode;
  int op;
  khash_t(l) labels;            /* object -> int */
  khash_t(v) visited;           /* object -> int */
  int cnt;
};

#define WRITE_MODE 1
#define DISPLAY_MODE 2

#define OP_WRITE 1
#define OP_WRITE_SHARED 2
#define OP_WRITE_SIMPLE 3

static void
writer_control_init(struct writer_control *p, pic_state *pic, xFILE *file, int mode, int op)
{
  p->pic = pic;
  p->file = file;
  p->mode = mode;
  p->op = op;
  p->cnt = 0;
  kh_init(l, &p->labels);
  kh_init(v, &p->visited);
}

static void
writer_control_destroy(struct writer_control *p)
{
  pic_state *pic = p->pic;
  kh_destroy(l, &p->labels);
  kh_destroy(v, &p->visited);
}

static void
write_blob(pic_state *pic, pic_blob *blob, xFILE *file)
{
  size_t i;

  xfprintf(pic, file, "#u8(");
  for (i = 0; i < blob->len; ++i) {
    xfprintf(pic, file, "%d", blob->data[i]);
    if (i + 1 < blob->len) {
      xfprintf(pic, file, " ");
    }
  }
  xfprintf(pic, file, ")");
}

static void
write_char(pic_state *pic, char c, xFILE *file, int mode)
{
  if (mode == DISPLAY_MODE) {
    xfputc(pic, c, file);
    return;
  }
  switch (c) {
  default: xfprintf(pic, file, "#\\%c", c); break;
  case '\a': xfprintf(pic, file, "#\\alarm"); break;
  case '\b': xfprintf(pic, file, "#\\backspace"); break;
  case 0x7f: xfprintf(pic, file, "#\\delete"); break;
  case 0x1b: xfprintf(pic, file, "#\\escape"); break;
  case '\n': xfprintf(pic, file, "#\\newline"); break;
  case '\r': xfprintf(pic, file, "#\\return"); break;
  case ' ': xfprintf(pic, file, "#\\space"); break;
  case '\t': xfprintf(pic, file, "#\\tab"); break;
  }
}

static void
write_str(pic_state *pic, pic_str *str, xFILE *file, int mode)
{
  size_t i;
  const char *cstr = pic_str_cstr(pic, str);

  if (mode == DISPLAY_MODE) {
    xfprintf(pic, file, "%s", pic_str_cstr(pic, str));
    return;
  }
  xfprintf(pic, file, "\"");
  for (i = 0; i < pic_str_len(str); ++i) {
    if (cstr[i] == '"' || cstr[i] == '\\') {
      xfputc(pic, '\\', file);
    }
    xfputc(pic, cstr[i], file);
  }
  xfprintf(pic, file, "\"");
}

static void
write_float(pic_state *pic, double f, xFILE *file)
{
  if (f != f) {
    xfprintf(pic, file, "+nan.0");
  } else if (f == 1.0 / 0.0) {
    xfprintf(pic, file, "+inf.0");
  } else if (f == -1.0 / 0.0) {
    xfprintf(pic, file, "-inf.0");
  } else {
    xfprintf(pic, file, "%f", f);
  }
}

static void write_core(struct writer_control *p, pic_value);

static void
write_pair_help(struct writer_control *p, struct pic_pair *pair)
{
  pic_state *pic = p->pic;
  khash_t(l) *lh = &p->labels;
  khash_t(v) *vh = &p->visited;
  khiter_t it;
  int ret;

  write_core(p, pair->car);

  if (pic_nil_p(pair->cdr)) {
    return;
  }
  else if (pic_pair_p(pair->cdr)) {

    /* shared objects */
    if ((it = kh_get(l, lh, pic_ptr(pair->cdr))) != kh_end(lh) && kh_val(lh, it) != -1) {
      xfprintf(pic, p->file, " . ");

      kh_put(v, vh, pic_ptr(pair->cdr), &ret);
      if (ret == 0) {           /* if exists */
        xfprintf(pic, p->file, "#%d#", kh_val(lh, it));
        return;
      }
      xfprintf(pic, p->file, "#%d=", kh_val(lh, it));
    }
    else {
      xfprintf(pic, p->file, " ");
    }

    write_pair_help(p, pic_pair_ptr(pair->cdr));

    if (p->op == OP_WRITE) {
      if ((it = kh_get(l, lh, pic_ptr(pair->cdr))) != kh_end(lh) && kh_val(lh, it) != -1) {
        it = kh_get(v, vh, pic_ptr(pair->cdr));
        kh_del(v, vh, it);
      }
    }
    return;
  }
  else {
    xfprintf(pic, p->file, " . ");
    write_core(p, pair->cdr);
  }
}

static void
write_pair(struct writer_control *p, struct pic_pair *pair)
{
  pic_state *pic = p->pic;
  xFILE *file = p->file;
  pic_sym *tag;

  if (pic_pair_p(pair->cdr) && pic_nil_p(pic_cdr(pic, pair->cdr)) && pic_sym_p(pair->car)) {
    tag = pic_sym_ptr(pair->car);
    if (tag == pic->sQUOTE) {
      xfprintf(pic, file, "'");
      write_core(p, pic_car(pic, pair->cdr));
      return;
    }
    else if (tag == pic->sUNQUOTE) {
      xfprintf(pic, file, ",");
      write_core(p, pic_car(pic, pair->cdr));
      return;
    }
    else if (tag == pic->sUNQUOTE_SPLICING) {
      xfprintf(pic, file, ",@");
      write_core(p, pic_car(pic, pair->cdr));
      return;
    }
    else if (tag == pic->sQUASIQUOTE) {
      xfprintf(pic, file, "`");
      write_core(p, pic_car(pic, pair->cdr));
      return;
    }
    else if (tag == pic->sSYNTAX_QUOTE) {
      xfprintf(pic, file, "#'");
      write_core(p, pic_car(pic, pair->cdr));
      return;
    }
    else if (tag == pic->sSYNTAX_UNQUOTE) {
      xfprintf(pic, file, "#,");
      write_core(p, pic_car(pic, pair->cdr));
      return;
    }
    else if (tag == pic->sSYNTAX_UNQUOTE_SPLICING) {
      xfprintf(pic, file, "#,@");
      write_core(p, pic_car(pic, pair->cdr));
      return;
    }
    else if (tag == pic->sSYNTAX_QUASIQUOTE) {
      xfprintf(pic, file, "#`");
      write_core(p, pic_car(pic, pair->cdr));
      return;
    }
  }
  xfprintf(pic, file, "(");
  write_pair_help(p, pair);
  xfprintf(pic, file, ")");
}

static void
write_vec(struct writer_control *p, pic_vec *vec)
{
  pic_state *pic = p->pic;
  xFILE *file = p->file;
  size_t i;

  xfprintf(pic, file, "#(");
  for (i = 0; i < vec->len; ++i) {
    write_core(p, vec->data[i]);
    if (i + 1 < vec->len) {
      xfprintf(pic, file, " ");
    }
  }
  xfprintf(pic, file, ")");
}

static void
write_dict(struct writer_control *p, struct pic_dict *dict)
{
  pic_state *pic = p->pic;
  xFILE *file = p->file;
  pic_sym *sym;
  khiter_t it;

  xfprintf(pic, file, "#.(dictionary");
  pic_dict_for_each (sym, dict, it) {
    xfprintf(pic, file, " '%s ", pic_symbol_name(pic, sym));
    write_core(p, pic_dict_ref(pic, dict, sym));
  }
  xfprintf(pic, file, ")");
}

static void
write_core(struct writer_control *p, pic_value obj)
{
  pic_state *pic = p->pic;
  khash_t(l) *lh = &p->labels;
  khash_t(v) *vh = &p->visited;
  xFILE *file = p->file;
  khiter_t it;
  int ret;

  /* shared objects */
  if (pic_vtype(obj) == PIC_VTYPE_HEAP && ((it = kh_get(l, lh, pic_ptr(obj))) != kh_end(lh)) && kh_val(lh, it) != -1) {
    kh_put(v, vh, pic_ptr(obj), &ret);
    if (ret == 0) {             /* if exists */
      xfprintf(pic, file, "#%d#", kh_val(lh, it));
      return;
    }
    xfprintf(pic, file, "#%d=", kh_val(lh, it));
  }

  switch (pic_type(obj)) {
  case PIC_TT_UNDEF:
    xfprintf(pic, file, "#undefined");
    break;
  case PIC_TT_NIL:
    xfprintf(pic, file, "()");
    break;
  case PIC_TT_BOOL:
    xfprintf(pic, file, pic_true_p(obj) ? "#t" : "#f");
    break;
  case PIC_TT_ID:
    xfprintf(pic, file, "#<identifier %s>", pic_symbol_name(pic, pic_var_name(pic, obj)));
    break;
  case PIC_TT_EOF:
    xfprintf(pic, file, "#.(eof-object)");
    break;
  case PIC_TT_INT:
    xfprintf(pic, file, "%d", pic_int(obj));
    break;
  case PIC_TT_FLOAT:
    write_float(pic, pic_float(obj), file);
    break;
  case PIC_TT_SYMBOL:
    xfprintf(pic, file, "%s", pic_symbol_name(pic, pic_sym_ptr(obj)));
    break;
  case PIC_TT_BLOB:
    write_blob(pic, pic_blob_ptr(obj), file);
    break;
  case PIC_TT_CHAR:
    write_char(pic, pic_char(obj), file, p->mode);
    break;
  case PIC_TT_STRING:
    write_str(pic, pic_str_ptr(obj), file, p->mode);
    break;
  case PIC_TT_PAIR:
    write_pair(p, pic_pair_ptr(obj));
    break;
  case PIC_TT_VECTOR:
    write_vec(p, pic_vec_ptr(obj));
    break;
  case PIC_TT_DICT:
    write_dict(p, pic_dict_ptr(obj));
    break;
  default:
    xfprintf(pic, file, "#<%s %p>", pic_type_repr(pic_type(obj)), pic_ptr(obj));
    break;
  }

  if (p->op == OP_WRITE) {
    if (pic_obj_p(obj) && ((it = kh_get(l, lh, pic_ptr(obj))) != kh_end(lh)) && kh_val(lh, it) != -1) {
      it = kh_get(v, vh, pic_ptr(obj));
      kh_del(v, vh, it);
    }
  }
}

static void
traverse(struct writer_control *p, pic_value obj)
{
  pic_state *pic = p->pic;

  if (p->op == OP_WRITE_SIMPLE) {
    return;
  }

  switch (pic_type(obj)) {
  case PIC_TT_PAIR:
  case PIC_TT_VECTOR:
  case PIC_TT_DICT: {
    khash_t(l) *h = &p->labels;
    khiter_t it;
    int ret;

    it = kh_put(l, h, pic_ptr(obj), &ret);
    if (ret != 0) {
      /* first time */
      kh_val(h, it) = -1;

      if (pic_pair_p(obj)) {
        /* pair */
        traverse(p, pic_car(pic, obj));
        traverse(p, pic_cdr(pic, obj));
      } else if (pic_vec_p(obj)) {
        /* vector */
        size_t i;
        for (i = 0; i < pic_vec_ptr(obj)->len; ++i) {
          traverse(p, pic_vec_ptr(obj)->data[i]);
        }
      } else {
        /* dictionary */
        pic_sym *sym;
        pic_dict_for_each (sym, pic_dict_ptr(obj), it) {
          traverse(p, pic_dict_ref(pic, pic_dict_ptr(obj), sym));
        }
      }

      if (p->op == OP_WRITE) {
        it = kh_get(l, h, pic_ptr(obj));
        if (kh_val(h, it) == -1) {
          kh_del(l, h, it);
        }
      }
    } else if (kh_val(h, it) == -1) {
      /* second time */
      kh_val(h, it) = p->cnt++;
    }
    break;
  }
  default:
    break;
  }
}

static void
write(pic_state *pic, pic_value obj, xFILE *file, int mode, int op)
{
  struct writer_control p;

  writer_control_init(&p, pic, file, mode, op);

  traverse(&p, obj);

  write_core(&p, obj);

  writer_control_destroy(&p);
}


pic_value
pic_write(pic_state *pic, pic_value obj)
{
  return pic_fwrite(pic, obj, pic_stdout(pic)->file);
}

pic_value
pic_fwrite(pic_state *pic, pic_value obj, xFILE *file)
{
  write(pic, obj, file, WRITE_MODE, OP_WRITE);
  xfflush(pic, file);
  return obj;
}

pic_value
pic_display(pic_state *pic, pic_value obj)
{
  return pic_fdisplay(pic, obj, pic_stdout(pic)->file);
}

pic_value
pic_fdisplay(pic_state *pic, pic_value obj, xFILE *file)
{
  write(pic, obj, file, DISPLAY_MODE, OP_WRITE);
  xfflush(pic, file);
  return obj;
}

void
pic_printf(pic_state *pic, const char *fmt, ...)
{
  xFILE *file = pic_stdout(pic)->file;
  va_list ap;
  pic_str *str;

  va_start(ap, fmt);

  str = pic_str_ptr(pic_car(pic, pic_xvformat(pic, fmt, ap)));

  va_end(ap);

  xfprintf(pic, file, "%s", pic_str_cstr(pic, str));
  xfflush(pic, file);
}

static pic_value
pic_write_write(pic_state *pic)
{
  pic_value v;
  struct pic_port *port = pic_stdout(pic);

  pic_get_args(pic, "o|p", &v, &port);
  write(pic, v, port->file, WRITE_MODE, OP_WRITE);
  return pic_undef_value();
}

static pic_value
pic_write_write_simple(pic_state *pic)
{
  pic_value v;
  struct pic_port *port = pic_stdout(pic);

  pic_get_args(pic, "o|p", &v, &port);
  write(pic, v, port->file, WRITE_MODE, OP_WRITE_SIMPLE);
  return pic_undef_value();
}

static pic_value
pic_write_write_shared(pic_state *pic)
{
  pic_value v;
  struct pic_port *port = pic_stdout(pic);

  pic_get_args(pic, "o|p", &v, &port);
  write(pic, v, port->file, WRITE_MODE, OP_WRITE_SHARED);
  return pic_undef_value();
}

static pic_value
pic_write_display(pic_state *pic)
{
  pic_value v;
  struct pic_port *port = pic_stdout(pic);

  pic_get_args(pic, "o|p", &v, &port);
  write(pic, v, port->file, DISPLAY_MODE, OP_WRITE);
  return pic_undef_value();
}

void
pic_init_write(pic_state *pic)
{
  pic_defun(pic, "write", pic_write_write);
  pic_defun(pic, "write-simple", pic_write_write_simple);
  pic_defun(pic, "write-shared", pic_write_write_shared);
  pic_defun(pic, "display", pic_write_display);
}
