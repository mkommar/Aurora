static __thread int value=70;
static int initialized;
__attribute__((constructor)) static void init(void){initialized=7;}
int plugin_value(void){return initialized+value++;}
