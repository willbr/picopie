nasm -f win64 hello.asm -o hello.obj -l hello.lst
link /subsystem:console /defaultlib:kernel32.lib /entry:main hello.obj

