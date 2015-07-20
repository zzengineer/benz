/**
 * See Copyright Notice in picrin.h
 */

#include "picrin.h"

union header {
  struct {
    union header *ptr;
    size_t size;
    char mark;
  } s;
};

struct heap_page {
  union header *basep, *endp;
  struct heap_page *next;
};

union object {
  struct pic_object obj;
  struct pic_symbol sym;
  struct pic_string str;
  struct pic_blob blob;
  struct pic_pair pair;
  struct pic_vector vec;
  struct pic_dict dict;
  struct pic_reg reg;
  struct pic_data data;
  struct pic_record rec;
  struct pic_id id;
  struct pic_env env;
  struct pic_proc proc;
  struct pic_context cxt;
  struct pic_irep irep;
  struct pic_port port;
  struct pic_error err;
  struct pic_lib lib;
  struct pic_checkpoint cp;
};

struct pic_heap {
  union header base, *freep;
  struct heap_page *pages;
};

struct pic_heap *
pic_heap_open(pic_state *pic)
{
  struct pic_heap *heap;

  heap = pic_malloc(pic, sizeof(struct pic_heap));

  heap->base.s.ptr = &heap->base;
  heap->base.s.size = 0; /* not 1, since it must never be used for allocation */

  heap->freep = &heap->base;
  heap->pages = NULL;

  return heap;
}

void
pic_heap_close(pic_state *pic, struct pic_heap *heap)
{
  struct heap_page *page;

  while (heap->pages) {
    page = heap->pages;
    heap->pages = heap->pages->next;
    pic_free(pic, page->basep);
    pic_free(pic, page);
  }
  pic_free(pic, heap);
}

#if PIC_ENABLE_LIBC
void *
pic_default_allocf(void *ptr, size_t size)
{
  if (size == 0) {
    if (ptr) {
      free(ptr);
    }
    return NULL;
  }
  if (ptr) {
    return realloc(ptr, size);
  } else {
    return malloc(size);
  }
}
#endif

void *
pic_malloc(pic_state *pic, size_t size)
{
  void *ptr;

  ptr = pic->allocf(NULL, size);
  if (ptr == NULL && size > 0) {
    pic_panic(pic, "memory exhausted");
  }
  return ptr;
}

void *
pic_realloc(pic_state *pic, void *ptr, size_t size)
{
  ptr = pic->allocf(ptr, size);
  if (ptr == NULL && size > 0) {
    pic_panic(pic, "memory exhausted");
  }
  return ptr;
}

void *
pic_calloc(pic_state *pic, size_t count, size_t size)
{
  void *ptr;

  size *= count;
  ptr = pic->allocf(NULL, size);
  if (ptr == NULL && size > 0) {
    pic_panic(pic, "memory exhausted");
  }
  memset(ptr, 0, size);
  return ptr;
}

void
pic_free(pic_state *pic, void *ptr)
{
  pic->allocf(ptr, 0);
}

static void
gc_protect(pic_state *pic, struct pic_object *obj)
{
  if (pic->arena_idx >= pic->arena_size) {
    pic->arena_size = pic->arena_size * 2 + 1;
    pic->arena = pic_realloc(pic, pic->arena, sizeof(struct pic_object *) * pic->arena_size);
  }
  pic->arena[pic->arena_idx++] = obj;
}

pic_value
pic_gc_protect(pic_state *pic, pic_value v)
{
  if (! pic_obj_p(v))
    return v;

  gc_protect(pic, pic_obj_ptr(v));

  return v;
}

size_t
pic_gc_arena_preserve(pic_state *pic)
{
  return pic->arena_idx;
}

void
pic_gc_arena_restore(pic_state *pic, size_t state)
{
  pic->arena_idx = state;
}

static void *
heap_alloc(pic_state *pic, size_t size)
{
  union header *p, *prevp;
  size_t nunits;

  assert(size > 0);

  nunits = (size + sizeof(union header) - 1) / sizeof(union header) + 1;

  prevp = pic->heap->freep;
  for (p = prevp->s.ptr; ; prevp = p, p = p->s.ptr) {
    if (p->s.size >= nunits)
      break;
    if (p == pic->heap->freep) {
      return NULL;
    }
  }

  if (p->s.size == nunits) {
    prevp->s.ptr = p->s.ptr;
  }
  else {
    p->s.size -= nunits;
    p += p->s.size;
    p->s.size = nunits;
  }
  pic->heap->freep = prevp;

  return (void *)(p + 1);
}

static void
heap_free(pic_state *pic, void *ap)
{
  union header *bp, *p;

  assert(ap != NULL);

  bp = (union header *)ap - 1;

  for (p = pic->heap->freep; ! (bp > p && bp < p->s.ptr); p = p->s.ptr) {
    if (p >= p->s.ptr && (bp > p || bp < p->s.ptr)) {
      break;
    }
  }
  if (bp + bp->s.size == p->s.ptr && p->s.ptr->s.size > 0) { /* don't melt base header */
    bp->s.size += p->s.ptr->s.size;
    bp->s.ptr = p->s.ptr->s.ptr;
  } else {
    bp->s.ptr = p->s.ptr;
  }
  if (p + p->s.size == bp && bp->s.size > 0) { /* don't melt base header */
    p->s.size += bp->s.size;
    p->s.ptr = bp->s.ptr;
  } else {
    p->s.ptr = bp;
  }
  pic->heap->freep = p;
}

static void
heap_morecore(pic_state *pic)
{
  union header *bp, *np;
  struct heap_page *page;
  size_t nunits;

  nunits = PIC_HEAP_PAGE_SIZE / sizeof(union header);

  assert(nunits >= 2);

  bp = pic_malloc(pic, PIC_HEAP_PAGE_SIZE);
  bp->s.size = 0;               /* bp is never used for allocation */
  heap_free(pic, bp + 1);

  np = bp + 1;
  np->s.size = nunits - 1;
  heap_free(pic, np + 1);

  page = pic_malloc(pic, sizeof(struct heap_page));
  page->basep = bp;
  page->endp = bp + nunits;
  page->next = pic->heap->pages;

  pic->heap->pages = page;
}

/* MARK */

static void gc_mark_object(pic_state *, union object *);

static bool
gc_obj_is_marked(struct pic_object *obj)
{
  union header *p;

  p = ((union header *)obj) - 1;

  return p->s.mark == PIC_GC_MARK;
}

static void
gc_unmark_object(struct pic_object *obj)
{
  union header *p;

  p = ((union header *)obj) - 1;

  p->s.mark = PIC_GC_UNMARK;
}

static void
gc_mark(pic_state *pic, pic_value v)
{
  if (! pic_obj_p(v))
    return;

  gc_mark_object(pic, (union object *)pic_obj_ptr(v));
}

static void
gc_mark_object(pic_state *pic, union object *obj)
{
  union header *p;

 loop:

  p = ((union header *)obj) - 1;

  if (p->s.mark == PIC_GC_MARK)
    return;
  p->s.mark = PIC_GC_MARK;

#define LOOP(o) obj = (union object *)(o); goto loop

  switch (obj->obj.tt) {
  case PIC_TT_PAIR: {
    gc_mark(pic, obj->pair.car);
    if (pic_obj_p(obj->pair.cdr)) {
      LOOP(pic_obj_ptr(obj->pair.cdr));
    }
    break;
  }
  case PIC_TT_CXT: {
    int i;

    for (i = 0; i < obj->cxt.regc; ++i) {
      gc_mark(pic, obj->cxt.regs[i]);
    }
    if (obj->cxt.up) {
      LOOP(obj->cxt.up);
    }
    break;
  }
  case PIC_TT_PROC: {
    if (pic_proc_irep_p(&obj->proc)) {
      gc_mark_object(pic, (union object *)obj->proc.u.i.irep);
      if (obj->proc.u.i.cxt) {
        LOOP(obj->proc.u.i.cxt);
      }
    } else {
      if (obj->proc.u.f.env) {
        LOOP(obj->proc.u.f.env);
      }
    }
    break;
  }
  case PIC_TT_PORT: {
    break;
  }
  case PIC_TT_ERROR: {
    gc_mark_object(pic, (union object *)obj->err.type);
    gc_mark_object(pic, (union object *)obj->err.msg);
    gc_mark(pic, obj->err.irrs);
    LOOP(obj->err.stack);
    break;
  }
  case PIC_TT_STRING: {
    break;
  }
  case PIC_TT_VECTOR: {
    size_t i;
    for (i = 0; i < obj->vec.len; ++i) {
      gc_mark(pic, obj->vec.data[i]);
    }
    break;
  }
  case PIC_TT_BLOB: {
    break;
  }
  case PIC_TT_ID: {
    gc_mark(pic, obj->id.var);
    LOOP(obj->id.env);
    break;
  }
  case PIC_TT_ENV: {
    khash_t(env) *h = &obj->env.map;
    khiter_t it;

    for (it = kh_begin(h); it != kh_end(h); ++it) {
      if (kh_exist(h, it)) {
        gc_mark_object(pic, kh_key(h, it));
        gc_mark_object(pic, (union object *)kh_val(h, it));
      }
    }
    if (obj->env.up) {
      LOOP(obj->env.up);
    }
    break;
  }
  case PIC_TT_LIB: {
    gc_mark(pic, obj->lib.name);
    gc_mark_object(pic, (union object *)obj->lib.env);
    LOOP(obj->lib.exports);
    break;
  }
  case PIC_TT_IREP: {
    size_t i;

    for (i = 0; i < obj->irep.ilen; ++i) {
      gc_mark_object(pic, (union object *)obj->irep.irep[i]);
    }
    for (i = 0; i < obj->irep.plen; ++i) {
      gc_mark(pic, obj->irep.pool[i]);
    }
    break;
  }
  case PIC_TT_DATA: {
    if (obj->data.type->mark) {
      obj->data.type->mark(pic, obj->data.data, gc_mark);
    }
    LOOP(obj->data.storage);
    break;
  }
  case PIC_TT_DICT: {
    pic_sym *sym;
    khiter_t it;

    pic_dict_for_each (sym, &obj->dict, it) {
      gc_mark_object(pic, (union object *)sym);
      gc_mark(pic, pic_dict_ref(pic, &obj->dict, sym));
    }
    break;
  }
  case PIC_TT_RECORD: {
    struct pic_record *rec = (struct pic_record *)obj;

    LOOP((struct pic_object *)rec->data);
    break;
  }
  case PIC_TT_SYMBOL: {
    break;
  }
  case PIC_TT_REG: {
    struct pic_reg *reg = (struct pic_reg *)obj;

    reg->prev = pic->regs;
    pic->regs = reg;
    break;
  }
  case PIC_TT_CP: {
    if (obj->cp.prev) {
      gc_mark_object(pic, (union object *)obj->cp.prev);
    }
    if (obj->cp.in) {
      gc_mark_object(pic, (union object *)obj->cp.in);
    }
    if (obj->cp.out) {
      LOOP((struct pic_object *)obj->cp.out);
    }
    break;
  }
  case PIC_TT_NIL:
  case PIC_TT_BOOL:
  case PIC_TT_FLOAT:
  case PIC_TT_INT:
  case PIC_TT_CHAR:
  case PIC_TT_EOF:
  case PIC_TT_UNDEF:
  case PIC_TT_INVALID:
    pic_panic(pic, "logic flaw");
  }
}

#define M(x) gc_mark_object(pic, (union object *)pic->x)
#define P(x) gc_mark(pic, pic->x)

static void
gc_mark_phase(pic_state *pic)
{
  pic_value *stack;
  pic_callinfo *ci;
  struct pic_proc **xhandler;
  size_t j;

  assert(pic->regs == NULL);

  /* checkpoint */
  if (pic->cp) {
    gc_mark_object(pic, (union object *)pic->cp);
  }

  /* stack */
  for (stack = pic->stbase; stack != pic->sp; ++stack) {
    gc_mark(pic, *stack);
  }

  /* callinfo */
  for (ci = pic->ci; ci != pic->cibase; --ci) {
    if (ci->cxt) {
      gc_mark_object(pic, (union object *)ci->cxt);
    }
  }

  /* exception handlers */
  for (xhandler = pic->xpbase; xhandler != pic->xp; ++xhandler) {
    gc_mark_object(pic, (union object *)*xhandler);
  }

  /* arena */
  for (j = 0; j < pic->arena_idx; ++j) {
    gc_mark_object(pic, (union object *)pic->arena[j]);
  }

  /* mark reserved symbols */
  M(sQUOTE); M(sQUASIQUOTE); M(sUNQUOTE); M(sUNQUOTE_SPLICING);
  M(sSYNTAX_QUOTE); M(sSYNTAX_QUASIQUOTE); M(sSYNTAX_UNQUOTE); M(sSYNTAX_UNQUOTE_SPLICING);
  M(sDEFINE_LIBRARY); M(sIMPORT); M(sEXPORT); M(sCOND_EXPAND);
  M(sCALL); M(sGREF); M(sLREF); M(sCREF);

  M(uDEFINE); M(uLAMBDA); M(uIF); M(uBEGIN); M(uQUOTE); M(uSETBANG); M(uDEFINE_MACRO);
  M(uDEFINE_LIBRARY); M(uIMPORT); M(uEXPORT); M(uCOND_EXPAND);
  M(uCONS); M(uCAR); M(uCDR); M(uNILP); M(uSYMBOLP); M(uPAIRP);
  M(uADD); M(uSUB); M(uMUL); M(uDIV); M(uEQ); M(uLT); M(uLE); M(uGT); M(uGE); M(uNOT);

  /* mark system procedures */
  P(pCONS); P(pCAR); P(pCDR); P(pNILP); P(pSYMBOLP); P(pPAIRP); P(pNOT);
  P(pADD); P(pSUB); P(pMUL); P(pDIV); P(pEQ); P(pLT); P(pLE); P(pGT); P(pGE);

  /* global variables */
  if (pic->globals) {
    gc_mark_object(pic, (union object *)pic->globals);
  }

  /* macro objects */
  if (pic->macros) {
    gc_mark_object(pic, (union object *)pic->macros);
  }

  /* attribute table */
  if (pic->attrs) {
    gc_mark_object(pic, (union object *)pic->attrs);
  }

  /* error object */
  gc_mark(pic, pic->err);

  /* features */
  gc_mark(pic, pic->features);

  /* library table */
  gc_mark(pic, pic->libs);

  /* parameter table */
  gc_mark(pic, pic->ptable);

  /* registries */
  do {
    struct pic_object *key;
    pic_value val;
    khiter_t it;
    khash_t(reg) *h;
    struct pic_reg *reg;

    j = 0;
    reg = pic->regs;

    while (reg != NULL) {
      h = &reg->hash;
      for (it = kh_begin(h); it != kh_end(h); ++it) {
        if (! kh_exist(h, it))
          continue;
        key = kh_key(h, it);
        val = kh_val(h, it);
        if (gc_obj_is_marked(key)) {
          if (pic_obj_p(val) && ! gc_obj_is_marked(pic_obj_ptr(val))) {
            gc_mark(pic, val);
            ++j;
          }
        }
      }
      reg = reg->prev;
    }
  } while (j > 0);
}

/* SWEEP */

static void
gc_finalize_object(pic_state *pic, union object *obj)
{
  switch (obj->obj.tt) {
  case PIC_TT_VECTOR: {
    pic_free(pic, obj->vec.data);
    break;
  }
  case PIC_TT_BLOB: {
    pic_free(pic, obj->blob.data);
    break;
  }
  case PIC_TT_STRING: {
    pic_rope_decref(pic, obj->str.rope);
    break;
  }
  case PIC_TT_ENV: {
    kh_destroy(env, &obj->env.map);
    break;
  }
  case PIC_TT_IREP: {
    pic_free(pic, obj->irep.code);
    pic_free(pic, obj->irep.irep);
    pic_free(pic, obj->irep.pool);
    break;
  }
  case PIC_TT_DATA: {
    if (obj->data.type->dtor) {
      obj->data.type->dtor(pic, obj->data.data);
    }
    break;
  }
  case PIC_TT_DICT: {
    kh_destroy(dict, &obj->dict.hash);
    break;
  }
  case PIC_TT_SYMBOL: {
    pic_free(pic, (void *)obj->sym.cstr);
    break;
  }
  case PIC_TT_REG: {
    kh_destroy(reg, &obj->reg.hash);
    break;
  }

  case PIC_TT_PAIR:
  case PIC_TT_CXT:
  case PIC_TT_PROC:
  case PIC_TT_PORT:
  case PIC_TT_ERROR:
  case PIC_TT_ID:
  case PIC_TT_LIB:
  case PIC_TT_RECORD:
  case PIC_TT_CP:
    break;

  case PIC_TT_NIL:
  case PIC_TT_BOOL:
  case PIC_TT_FLOAT:
  case PIC_TT_INT:
  case PIC_TT_CHAR:
  case PIC_TT_EOF:
  case PIC_TT_UNDEF:
  case PIC_TT_INVALID:
    pic_panic(pic, "logic flaw");
  }
}

static size_t
gc_sweep_page(pic_state *pic, struct heap_page *page)
{
  union header *bp, *p, *head = NULL, *tail = NULL;
  size_t alive = 0;

  for (bp = page->basep; ; bp = bp->s.ptr) {
    p = bp + (bp->s.size ? bp->s.size : 1); /* first bp's size is 0, so force advnce */
    for (; p != bp->s.ptr; p += p->s.size) {
      if (p < page->basep || page->endp <= p) {
        goto escape;
      }
      if (p->s.mark == PIC_GC_MARK) {
        p->s.mark = PIC_GC_UNMARK;
        alive += p->s.size;
      } else {
        if (head == NULL) {
          head = p;
        }
        if (tail != NULL) {
          tail->s.ptr = p;
        }
        tail = p;
        tail->s.ptr = NULL; /* We can safely reuse ptr field of dead object */
      }
    }
  }
 escape:

  /* free! */
  while (head != NULL) {
    p = head;
    head = head->s.ptr;
    gc_finalize_object(pic, (union object *)(p + 1));
    heap_free(pic, p + 1);
  }

  return alive;
}

static void
gc_sweep_phase(pic_state *pic)
{
  struct heap_page *page;
  khiter_t it;
  khash_t(reg) *h;
  khash_t(s) *s = &pic->syms;
  pic_sym *sym;
  size_t total = 0, inuse = 0;

  /* registries */
  while (pic->regs != NULL) {
    h = &pic->regs->hash;
    for (it = kh_begin(h); it != kh_end(h); ++it) {
      if (! kh_exist(h, it))
        continue;
      if (! gc_obj_is_marked(kh_key(h, it))) {
        kh_del(reg, h, it);
      }
    }
    pic->regs = pic->regs->prev;
  }

  /* symbol table */
  for (it = kh_begin(s); it != kh_end(s); ++it) {
    if (! kh_exist(s, it))
      continue;
    sym = kh_val(s, it);
    if (! gc_obj_is_marked((struct pic_object *)sym)) {
      kh_del(s, s, it);
    }
  }

  page = pic->heap->pages;
  while (page) {
    inuse += gc_sweep_page(pic, page);
    total += page->endp - page->basep;
    page = page->next;
  }

  if (PIC_PAGE_REQUEST_THRESHOLD(total) <= inuse) {
    heap_morecore(pic);
  }
}

void
pic_gc_run(pic_state *pic)
{
  if (! pic->gc_enable) {
    return;
  }

  gc_mark_phase(pic);
  gc_sweep_phase(pic);
}

struct pic_object *
pic_obj_alloc_unsafe(pic_state *pic, size_t size, enum pic_tt tt)
{
  struct pic_object *obj;

#if GC_STRESS
  pic_gc_run(pic);
#endif

  obj = (struct pic_object *)heap_alloc(pic, size);
  if (obj == NULL) {
    pic_gc_run(pic);
    obj = (struct pic_object *)heap_alloc(pic, size);
    if (obj == NULL) {
      heap_morecore(pic);
      obj = (struct pic_object *)heap_alloc(pic, size);
      if (obj == NULL)
	pic_panic(pic, "GC memory exhausted");
    }
  }
  gc_unmark_object(obj);
  obj->tt = tt;

  return obj;
}

struct pic_object *
pic_obj_alloc(pic_state *pic, size_t size, enum pic_tt tt)
{
  struct pic_object *obj;

  obj = pic_obj_alloc_unsafe(pic, size, tt);

  gc_protect(pic, obj);
  return obj;
}
