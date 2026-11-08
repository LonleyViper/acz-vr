case PROC
$LL1:
 push rbx
 je SHORT $LL2
 pop rbx
$LL2:
 jmp SHORT $LL1
 ret
case ENDP
