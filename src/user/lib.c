#include "lib.h"
void *memset(void *p,int v,u64 n) { u8 *s=p;while(n--)*s++=(u8)v;return p; }
void *memcpy(void *d,const void *s,u64 n) { u8 *a=d;const u8 *b=s;while(n--)*a++=*b++;return d; }
