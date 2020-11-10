global _my_print

; len(s) -> length !pure
len:
    mov rax, rdi
    .iter:
        cmp byte [rax], 0
        jz .done
        inc rax
        jmp .iter
    .done:
    sub rax, rdi
    ret
; my_print(s, err)
_my_print:
    push rdi
    call len
    mov rdx, rax

    mov rdi, 1
    cmp rsi, 1
    jnz .no_err
        mov rdi, 2
    .no_err:

    pop     rsi
    mov     rax, 2000004h
    syscall
    ret