bits 16
org 0x8000
    mov [drive], dl
    mov byte [chunks], 4
.load:
    mov word [dap+2], 120
    mov si, dap
    mov dl, [drive]
    mov ah, 0x42
    int 0x13
    jc fail
    add word [dap+6], 0x0f00
    add dword [dap+8], 120
    dec byte [chunks]
    jnz .load
    ; Copy the firmware's 8x16 glyphs before leaving real mode.
    mov ax, 0x1130
    mov bh, 6
    int 0x10
    push ds
    mov ax, es
    mov ds, ax
    mov si, bp
    mov ax, 0x7000
    mov es, ax
    xor di, di
    mov cx, 4096
    rep movsb
    pop ds
    ; Preserve the BIOS RAM map above the copied font and kernel image.
    mov ax, 0x7200
    mov es, ax
    mov di, 16
    xor ebx, ebx
    xor bp, bp
.e820:
    mov eax, 0xe820
    mov edx, 0x534d4150
    mov ecx, 24
    mov dword [es:di+20], 1
    int 0x15
    jc .e820_done
    cmp eax, 0x534d4150
    jne .e820_done
    inc bp
    add di, 24
    cmp bp, 64
    jae .e820_done
    test ebx, ebx
    jnz .e820
.e820_done:
    movzx eax, bp
    mov [es:0], eax
    xor ax, ax
    mov es, ax
    mov di, 0x500
    mov dword [es:di], 0x32454256
    mov ax, 0x4f00
    int 0x10
    cmp ax, 0x4f
    jne fail
    mov si, [0x50e]
    mov ax, [0x510]
    mov fs, ax
.mode:
    mov cx, [fs:si]
    add si, 2
    cmp cx, 0xffff
    je fail
    push si
    push fs
    push cx
    mov di, 0x900
    mov ax, 0x4f01
    int 0x10
    pop cx
    pop fs
    pop si
    cmp ax, 0x4f
    jne .mode
    cmp word [0x912], 1024
    jne .mode
    cmp word [0x914], 768
    jne .mode
    cmp byte [0x919], 32
    jne .mode
    test word [0x900], 0x80
    jz .mode
    mov bx, cx
    or bx, 0x4000
    mov ax, 0x4f02
    int 0x10
    cmp ax, 0x4f
    jne fail
    cli
    in al, 0x92
    or al, 2
    out 0x92, al
    lgdt [gdt_ptr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 8:protected
fail:
    mov ax, 0x0e46
    int 0x10
    cli
    hlt
    jmp fail
drive: db 0
chunks: db 0
align 4
dap: db 16,0
    dw 120
    dw 0,0x1000
    dq 9
align 8
gdt:
    dq 0
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
    dq 0x00af9a000000ffff
gdt_ptr: dw $-gdt-1
    dd gdt
bits 32
protected:
    mov ax, 16
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000
    cld
    mov edi, 0x1000
    xor eax, eax
    mov ecx, 6144
    rep stosd
    mov dword [0x1000], 0x2003
    mov edi, 0x2000
    mov eax, 0x3003
    mov ecx, 4
.pdpt:
    mov [edi], eax
    add eax, 0x1000
    add edi, 8
    loop .pdpt
    mov edi, 0x3000
    mov eax, 0x83
    mov ecx, 2048
.pages:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .pages
    mov eax, cr4
    or eax, 0x20
    mov cr4, eax
    mov eax, 0x1000
    mov cr3, eax
    mov ecx, 0xc0000080
    rdmsr
    or eax, 0x100
    wrmsr
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    jmp 24:long_mode
bits 64
long_mode:
    mov rsp, 0x90000
    mov rax, 0x10000
    jmp rax
times 4096-($-$$) db 0
