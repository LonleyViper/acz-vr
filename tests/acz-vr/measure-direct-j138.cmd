@call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
@if errorlevel 1 exit /b %errorlevel%
@cl /nologo /O2 /Ob2 /std:c++17 /EHsc /FAs /Fatests/acz-vr/.j138-native/hook.asm /c /Fotests/acz-vr/.j138-native/hook.obj src/acz-vr/d3d12_hook.cpp
@if errorlevel 1 exit /b %errorlevel%
@cl /nologo /O2 /Ob2 /std:c++17 /EHsc /FAs /Fatests/acz-vr/.j138-native/log.asm /c /Fotests/acz-vr/.j138-native/log.obj src/acz-vr/diagnostics_log.cpp
@exit /b %errorlevel%
