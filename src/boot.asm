bits 16
org 0x7c00
start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    mov [drive], dl
    mov si, dap
    mov ah, 0x42
    int 0x13
    jc fail
    mov dl, [drive]
    jmp 0:0x8000
fail:
    mov ax, 0x0e45
    int 0x10
    cli
    hlt
    jmp fail
drive: db 0
align 4
dap: db 16,0
    dw 8
    dw 0x8000,0
    dq 1
times 510-($-$$) db 0
dw 0xaa55
