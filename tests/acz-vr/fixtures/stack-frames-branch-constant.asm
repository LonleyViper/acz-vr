case PROC
 mov eax,32
 je SHORT $LL1
 mov eax,64
$LL1:
 sub rsp,rax
 ret
case ENDP
