case PROC
mov ecx,2
$LL1:
sub rsp,8192
dec ecx
jne SHORT $LL1
add rsp,16384
ret
case ENDP
