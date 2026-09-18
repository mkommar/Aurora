; Copied to physical 0x8000 after the BIOS loader has finished.
bits 16
org 0x8000
    cli
    cld
    xor ax,ax
    mov ds,ax
    mov es,ax
    mov ss,ax
    lgdt [gdt_pointer]
    mov eax,cr0
    or eax,1
    mov cr0,eax
    jmp 8:protected
align 8
gdt:
    dq 0,0x00cf9a000000ffff,0x00cf92000000ffff,0x00af9a000000ffff
gdt_pointer: dw 31
    dd gdt
bits 32
protected:
    mov ax,16
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov eax,cr4
    or eax,0x620
    mov cr4,eax
    mov eax,0x200000
    mov cr3,eax
    mov ecx,0xc0000080
    rdmsr
    or eax,0x901
    wrmsr
    mov eax,cr0
    or eax,0x80010003
    and eax,~12
    mov cr0,eax
    jmp 24:long_mode
bits 64
long_mode:
    mov rsp,[0x8f00]
    mov edi,[0x8f10]
    mov rax,[0x8f08]
    call rax
    ud2
