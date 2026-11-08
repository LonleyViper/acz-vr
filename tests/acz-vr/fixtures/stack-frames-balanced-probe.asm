case PROC
$LL1:
 mov eax,32
 call __chkstk
 sub rsp,rax
 add rsp,32
 jne SHORT $LL1
 ret
case ENDP
