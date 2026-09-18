bits 64
section .text.entry
global _start
extern aurora_start
_start:
    call aurora_start
    ud2
