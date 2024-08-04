section .data
    hello db 'Hello, World!', 0

section .bss
    bytes_written resq 1

section .text
    global main
    extern GetStdHandle
    extern WriteFile
    extern ExitProcess

main:
    ; Reserve shadow space
    sub rsp, 28h

    ; Get the handle to the standard output
    mov ecx, -11                 ; STD_OUTPUT_HANDLE
    call GetStdHandle

    ; Store the handle
    mov rbx, rax

    ; Write "Hello, World!" to the standard output
    lea rdx, [rel hello]         ; Pointer to the string
    mov r8, 13                   ; Length of the string
    lea r9, [rel bytes_written]  ; Address of number of bytes written
    mov rcx, rbx                 ; Handle to the standard output
    call WriteFile

    ; Exit the process
    xor ecx, ecx                 ; Exit code 0
    call ExitProcess
