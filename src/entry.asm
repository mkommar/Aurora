bits 64
section .text.entry
global _start
extern kernel_main
extern __bss_start
extern __bss_end
_start:
    cld
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    xor eax, eax
    rep stosb
    call kernel_main
.halt:
    cli
    hlt
    jmp .halt
