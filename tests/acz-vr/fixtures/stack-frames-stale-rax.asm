case PROC
mov eax,32
push 16384
pop rax
sub rsp,rax
add rsp,16384
ret
case ENDP
