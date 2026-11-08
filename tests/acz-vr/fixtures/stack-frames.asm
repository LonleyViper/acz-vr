; Hand-written synthetic x64 /FAs shapes. No production measurement.
hookedDrawInstanced PROC
  00000  53             push rbx
  00001  48 83 ec 20    sub rsp, 32
  00005  e8 00 00 00 00 call metadata
  0000a  e8 00 00 00 00 call cycleA
  0000f  ff 50 10       call QWORD PTR [rax+16]
  00012  48 83 c4 20    add rsp, 32
  00016  5b             pop rbx
  00017  c3             ret 0
hookedDrawInstanced ENDP
metadata PROC
  push rsi
  sub rsp, 50h
  call probe
  ret 0
metadata ENDP
probe PROC
  push rbp
  mov eax, 8192
  call __chkstk
  sub rsp, rax
  ret 0
probe ENDP
cycleA PROC
  sub rsp, 20h
  call cycleB
  ret 0
cycleA ENDP
cycleB PROC
  call cycleA
  ret 0
cycleB ENDP
dynamic PROC
  sub rsp, rcx
  lea rsp, QWORD PTR [rbp-32]
  ret 0
dynamic ENDP
