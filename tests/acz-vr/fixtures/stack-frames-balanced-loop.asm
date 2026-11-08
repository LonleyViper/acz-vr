case PROC
$LL1@case:
 push rbx
 sub rsp,32
 add rsp,32
 pop rbx
 loopne SHORT $LL1@case
 ret
case ENDP
