bits 64
section .text
extern trap_dispatch
extern save_context
extern restore_context
extern task_kernel_sp
extern current_task
extern kernel_stack_top
extern schedule

global syscall_entry
syscall_entry:
    mov [rel syscall_user_rsp], rsp
    mov rsp, [rel kernel_stack_top]
    push qword 0x1b
    push qword [rel syscall_user_rsp]
    push r11
    push qword 0x23
    push rcx
    push qword 0
    push qword 128
    jmp trap_common
global install_gdt
install_gdt:
    lgdt [rdi]
    push 8
    lea rax, [rel .reload]
    push rax
    retfq
.reload:
    mov ax, 16
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov ax, 40
    ltr ax
    ret

%macro STUB 1
global isr%1
isr%1:
%if %1 != 8 && %1 != 10 && %1 != 11 && %1 != 12 && %1 != 13 && %1 != 14 && %1 != 17 && %1 != 21 && %1 != 29 && %1 != 30
    push qword 0
%endif
    push qword %1
    jmp trap_common
%endmacro
%assign n 0
%rep 64
    STUB n
%assign n n+1
%endrep
STUB 128

trap_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rdi
    push rsi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rbx, rsp
    mov eax, 0x200000
    mov cr3, rax
    and rsp, -16
    call save_context
    mov rdi, rbx
    call trap_dispatch
select_context:
    mov edx, [rel current_task]
    lea rcx, [rel task_kernel_sp]
    mov rsi, [rcx+rdx*8]
    test rsi, rsi
    jnz resume_kernel
    mov rbx, rax
    call restore_context
    mov rsp, rbx
    jmp restore

global enter_user
enter_user:
    mov rax, rdi
    sub rsp, 8
    jmp select_context

; Explicit blocking continuation. Interrupts remain disabled in kernel code.
global kernel_suspend
kernel_suspend:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov edx, [rel current_task]
    lea rcx, [rel task_kernel_sp]
    mov [rcx+rdx*8], rsp
    sub rsp, 8
    call schedule
    jmp select_context
resume_kernel:
    mov qword [rcx+rdx*8], 0
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
restore:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rsi
    pop rdi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

section .rodata
global isr_table
isr_table:
%assign n 0
%rep 64
    dq isr%+n
%assign n n+1
%endrep
    dq isr128

section .bss
align 8
syscall_user_rsp: resq 1

