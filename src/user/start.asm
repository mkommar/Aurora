bits 64
section .text.entry
global _start
extern user_main
_start:
    call user_main
    mov eax, 8                 ; SYS_EXIT
    int 0x80
    ud2
