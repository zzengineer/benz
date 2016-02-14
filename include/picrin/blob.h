/**
 * See Copyright Notice in picrin.h
 */

#ifndef PICRIN_BLOB_H
#define PICRIN_BLOB_H

#if defined(__cplusplus)
extern "C" {
#endif

struct pic_blob {
  PIC_OBJECT_HEADER
  unsigned char *data;
  int len;
};

#define pic_blob_ptr(v) ((struct pic_blob *)pic_ptr(v))

struct pic_blob *pic_make_blob(pic_state *, int);

#if defined(__cplusplus)
}
#endif

#endif
