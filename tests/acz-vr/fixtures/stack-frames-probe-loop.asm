case PROC
$LL1:
 mov eax,8192
 call __chkstk
 sub rsp,rax
 jne SHORT $LL1
 ret
case ENDP
