#include <pthread.h>
static __thread int value=17;
static int initialized;
__attribute__((constructor)) static void init(void){initialized=23;}
int library_value(void){return initialized+value++;}
