#include <aurora.h>
int main(int argc,char **argv){(void)argc;(void)argv;puts("Testing application fault isolation");__asm__ volatile("ud2");return 0;}
