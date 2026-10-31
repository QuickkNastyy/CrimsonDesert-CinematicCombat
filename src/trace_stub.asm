; Generic call-trace stub (x64 MASM). Entered by a per-trace thunk with r11 = TraceCtx*.
; Stack/registers are exactly as at the traced function's entry (return address at [rsp]).
; Saves the argument registers into a TraceRegs block, calls TraceHit(ctx, regs) which returns the
; address of MinHook's trampoline (original function), restores everything and tail-jumps there,
; so the original sees an untouched stack and registers (any calling convention, stack args, varargs).
;
; TraceRegs layout (160 bytes, at rsp+32 after the sub):
;   0 rcx  8 rdx  16 r8  24 r9  32 rax  40 r10
;   48 xmm0  64 xmm1  80 xmm2  96 xmm3  112 xmm4  128 xmm5
;   144 return address   152 rsp at entry
; Frame = 32 shadow + 160 regs + 8 pad = 200 (keeps rsp 16-byte aligned for the call).

EXTERN TraceHit:PROC

.code

TraceCommon PROC FRAME
    sub rsp, 200
    .allocstack 200
    .endprolog

    mov [rsp+32+0],  rcx
    mov [rsp+32+8],  rdx
    mov [rsp+32+16], r8
    mov [rsp+32+24], r9
    mov [rsp+32+32], rax
    mov [rsp+32+40], r10
    movups xmmword ptr [rsp+32+48],  xmm0
    movups xmmword ptr [rsp+32+64],  xmm1
    movups xmmword ptr [rsp+32+80],  xmm2
    movups xmmword ptr [rsp+32+96],  xmm3
    movups xmmword ptr [rsp+32+112], xmm4
    movups xmmword ptr [rsp+32+128], xmm5
    mov rax, [rsp+200]
    mov [rsp+32+144], rax
    lea rax, [rsp+200]
    mov [rsp+32+152], rax

    mov rcx, r11
    lea rdx, [rsp+32]
    call TraceHit
    mov r11, rax                     ; original (trampoline) address

    mov rcx, [rsp+32+0]
    mov rdx, [rsp+32+8]
    mov r8,  [rsp+32+16]
    mov r9,  [rsp+32+24]
    mov rax, [rsp+32+32]
    mov r10, [rsp+32+40]
    movups xmm0, xmmword ptr [rsp+32+48]
    movups xmm1, xmmword ptr [rsp+32+64]
    movups xmm2, xmmword ptr [rsp+32+80]
    movups xmm3, xmmword ptr [rsp+32+96]
    movups xmm4, xmmword ptr [rsp+32+112]
    movups xmm5, xmmword ptr [rsp+32+128]
    add rsp, 200
    jmp r11
TraceCommon ENDP

END
