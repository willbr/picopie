.global _start
.align 2

_start:
    mov X0, #1          // stdout fd (1)
    adr X1, msg         // address of message
    mov X2, #13         // message length (note: 13 for "Hello World!\n")
    mov X16, #4         // macOS syscall: write (in X16)
    svc #0x80           // invoke syscall

    mov X0, #0          // exit status (0)
    mov X16, #1         // macOS syscall: exit (in X16)
    svc #0x80           // invoke syscall

msg: .ascii "Hello World!\n"
