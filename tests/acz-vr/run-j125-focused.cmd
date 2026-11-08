@call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
@if errorlevel 1 exit /b %errorlevel%
@python -c "from pathlib import Path; p=Path('tests/acz-vr/.j125-native'); p.mkdir(exist_ok=True); (p/'.gitignore').write_text('*\n')"
@if errorlevel 1 exit /b %errorlevel%
@if "%1"=="diagnostics" goto diagnostics
@cl /nologo /c /O2 /MD /std:c++17 /EHsc /Isrc/acz-vr /DACZ_BINDING_ABI_CHECK /Fotests/acz-vr/.j125-native/abi.obj tests/acz-vr/native_binding_observer_offline_tests.cpp
@if errorlevel 1 exit /b %errorlevel%
@cl /nologo /O2 /MD /std:c++17 /EHsc /Isrc/acz-vr /DACZ_DIAGNOSTICS_OFFLINE_TEST /Fotests/acz-vr/.j125-native/ /Fetests/acz-vr/.j125-native/native_binding_observer_offline_tests.exe tests/acz-vr/native_binding_observer_offline_tests.cpp src/acz-vr/native_binding_observer.cpp src/acz-vr/buffer_binding_registry.cpp src/acz-vr/draw_capture.cpp src/acz-vr/shader_metadata.cpp src/acz-vr/root_binding_metadata.cpp src/acz-vr/srt_payload.cpp src/acz-vr/diagnostics_log.cpp src/acz-vr/runtime_config.cpp src/acz-vr/capture_enablement.cpp src/acz-vr/capture_input_provider.cpp tests/acz-vr/.j125-native/abi.obj /link d3d12.lib bcrypt.lib
@if errorlevel 1 exit /b %errorlevel%
@tests\acz-vr\.j125-native\native_binding_observer_offline_tests.exe

@if errorlevel 1 exit /b %errorlevel%
:diagnostics
@cl /nologo /O2 /MD /std:c++17 /EHsc /Isrc/acz-vr /DACZ_DIAGNOSTICS_OFFLINE_TEST /Fotests/acz-vr/.j125-native/ /Fetests/acz-vr/.j125-native/diagnostics_offline_tests.exe tests/acz-vr/diagnostics_offline_tests.cpp src/acz-vr/native_binding_observer.cpp src/acz-vr/buffer_binding_registry.cpp src/acz-vr/draw_capture.cpp src/acz-vr/shader_metadata.cpp src/acz-vr/root_binding_metadata.cpp src/acz-vr/srt_payload.cpp src/acz-vr/diagnostics_log.cpp src/acz-vr/runtime_config.cpp src/acz-vr/capture_enablement.cpp src/acz-vr/capture_input_provider.cpp /link d3d12.lib bcrypt.lib
@if errorlevel 1 exit /b %errorlevel%
@pushd tests\acz-vr\.j125-native
@.\diagnostics_offline_tests.exe "..\..\..\tools\evidence\e10-drawcpt-pso.json"
@set "J125_TEST_RESULT=%errorlevel%"
@popd
@exit /b %J125_TEST_RESULT%
