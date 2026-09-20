; Shared Win64 ABI-preserving forwarding thunk.
FORWARD MACRO symbol, slot, targets, resolver
LOCAL unresolved
PUBLIC symbol
symbol PROC FRAME
    ; Use one unwindable frame on both paths. 0A8h aligns RSP for the resolver.
    sub rsp, 0A8h
    .allocstack 0A8h
    .endprolog
    mov rax, QWORD PTR [targets + slot * 8]
    test rax, rax
    jz unresolved
    add rsp, 0A8h
    jmp rax
unresolved:
    mov QWORD PTR [rsp + 20h], rcx
    mov QWORD PTR [rsp + 28h], rdx
    mov QWORD PTR [rsp + 30h], r8
    mov QWORD PTR [rsp + 38h], r9
    movdqu XMMWORD PTR [rsp + 40h], xmm0
    movdqu XMMWORD PTR [rsp + 50h], xmm1
    movdqu XMMWORD PTR [rsp + 60h], xmm2
    movdqu XMMWORD PTR [rsp + 70h], xmm3
    mov ecx, slot
    call resolver
    mov rcx, QWORD PTR [rsp + 20h]
    mov rdx, QWORD PTR [rsp + 28h]
    mov r8, QWORD PTR [rsp + 30h]
    mov r9, QWORD PTR [rsp + 38h]
    movdqu xmm0, XMMWORD PTR [rsp + 40h]
    movdqu xmm1, XMMWORD PTR [rsp + 50h]
    movdqu xmm2, XMMWORD PTR [rsp + 60h]
    movdqu xmm3, XMMWORD PTR [rsp + 70h]
    add rsp, 0A8h
    jmp rax
symbol ENDP
ENDM
