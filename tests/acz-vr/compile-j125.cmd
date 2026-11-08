@call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
@if errorlevel 1 exit /b %errorlevel%
@cl /nologo /Zs /std:c++17 /EHsc /Isrc/acz-vr src/acz-vr/native_binding_observer.cpp
@if errorlevel 1 exit /b %errorlevel%
@cl /nologo /Zs /std:c++17 /EHsc /Isrc/acz-vr /DACZ_DIAGNOSTICS_OFFLINE_TEST tests/acz-vr/native_binding_observer_offline_tests.cpp tests/acz-vr/diagnostics_offline_tests.cpp
@if errorlevel 1 exit /b %errorlevel%
@cl /nologo /Zs /std:c++17 /EHsc /Isrc/acz-vr /DACZ_BINDING_ABI_CHECK tests/acz-vr/native_binding_observer_offline_tests.cpp
