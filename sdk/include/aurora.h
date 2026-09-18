#ifndef AURORA_H
#define AURORA_H
typedef unsigned long long size_t;
#define NULL ((void *)0)
int printf(const char *format,...);
int puts(const char *s);
int putchar(int c);
size_t strlen(const char *s);
int strcmp(const char *a,const char *b);
int atoi(const char *s);
void *memcpy(void *d,const void *s,size_t n);
void *memset(void *d,int c,size_t n);
void *malloc(size_t n);
void free(void *p);
void exit(int status) __attribute__((noreturn));
/* Whole-file operations: read returns bytes copied, write replaces the file.
   Names: 1-31 ASCII letters/digits/dot/underscore/hyphen. Negative = error.
   File limit 64 KiB; directory limit 32 entries. No implicit trailing NUL. */
long long aurora_readfile(const char *name,void *buffer,size_t capacity);
long long aurora_writefile(const char *name,const void *buffer,size_t size);
#endif
