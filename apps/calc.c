#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv){if(argc!=3){puts("Usage: calc NUMBER NUMBER (adds two integers)");return 1;}printf("Result: %d\n",(int)((unsigned)atoi(argv[1])+(unsigned)atoi(argv[2])));return 0;}
