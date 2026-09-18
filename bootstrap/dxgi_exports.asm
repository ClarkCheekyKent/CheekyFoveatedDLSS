; Win64 transparent forwarding, including undocumented DXGI entry points.
; Tail jumps preserve stack arguments and every register used for parameters.
; A C++ "void()" forwarder is not ABI-correct for these varying signatures.
option casemap:none
EXTERN cheeky_dxgi_targets:QWORD
EXTERN cheeky_dxgi_resolve:PROC

.code
FORWARD MACRO symbol, slot
LOCAL unresolved
PUBLIC symbol
symbol PROC FRAME
    ; Use one unwindable frame on both paths. 0A8h aligns RSP for the resolver.
    sub rsp, 0A8h
    .allocstack 0A8h
    .endprolog
    mov rax, QWORD PTR [cheeky_dxgi_targets + slot * 8]
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
    call cheeky_dxgi_resolve
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

FORWARD cheeky_proxy_ApplyCompatResolutionQuirking, 0
FORWARD cheeky_proxy_CompatString, 1
FORWARD cheeky_proxy_CompatValue, 2
FORWARD cheeky_proxy_DXGIDumpJournal, 3
FORWARD cheeky_proxy_PIXBeginCapture, 4
FORWARD cheeky_proxy_PIXEndCapture, 5
FORWARD cheeky_proxy_PIXGetCaptureState, 6
FORWARD cheeky_proxy_SetAppCompatStringPointer, 7
FORWARD cheeky_proxy_UpdateHMDEmulationStatus, 8
; Slots 9-11 have typed C++ wrappers to finish bootstrap after a successful
; real factory call, before the application can create its first swap chain.
FORWARD cheeky_proxy_DXGID3D10CreateDevice, 12
FORWARD cheeky_proxy_DXGID3D10CreateLayeredDevice, 13
FORWARD cheeky_proxy_DXGID3D10GetLayeredDeviceSize, 14
FORWARD cheeky_proxy_DXGID3D10RegisterLayers, 15
; Slots 16 and 18 have typed wrappers to guard third-party hook recursion.
FORWARD cheeky_proxy_DXGIDisableVBlankVirtualization, 17
FORWARD cheeky_proxy_DXGIReportAdapterConfiguration, 19
END
