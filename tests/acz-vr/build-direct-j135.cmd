@node -e "const fs=require('fs');fs.mkdirSync('tests/acz-vr/.j135-native',{recursive:true});fs.writeFileSync('tests/acz-vr/.j135-native/.gitignore','*\n');"
@if errorlevel 1 exit /b %errorlevel%
@call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
@if errorlevel 1 exit /b %errorlevel%
@cl /nologo /O2 /std:c++17 /EHsc /DACZ_DIAGNOSTICS_OFFLINE_TEST /Isrc/acz-vr /Fotests/acz-vr/.j135-native/ /Fetests/acz-vr/.j135-native/diagnostics_offline_tests.exe tests/acz-vr/diagnostics_offline_tests.cpp src/acz-vr/diagnostics_log.cpp src/acz-vr/runtime_config.cpp src/acz-vr/shader_metadata.cpp src/acz-vr/draw_capture.cpp src/acz-vr/root_binding_metadata.cpp src/acz-vr/srt_payload.cpp src/acz-vr/native_binding_observer.cpp src/acz-vr/buffer_binding_registry.cpp src/acz-vr/capture_enablement.cpp src/acz-vr/capture_input_provider.cpp /link d3d12.lib bcrypt.lib
@if errorlevel 1 exit /b %errorlevel%
@cl /nologo /c /std:c++17 /EHsc /DACZ_BINDING_ABI_CHECK /Fotests/acz-vr/.j135-native/sdk_abi.obj tests/acz-vr/native_binding_observer_offline_tests.cpp
@if errorlevel 1 exit /b %errorlevel%
@cl /nologo /O2 /std:c++17 /EHsc /DACZ_DIAGNOSTICS_OFFLINE_TEST /Isrc/acz-vr /Fotests/acz-vr/.j135-native/ /Fetests/acz-vr/.j135-native/native_binding_observer_offline_tests.exe tests/acz-vr/native_binding_observer_offline_tests.cpp tests/acz-vr/.j135-native/sdk_abi.obj src/acz-vr/diagnostics_log.cpp src/acz-vr/runtime_config.cpp src/acz-vr/shader_metadata.cpp src/acz-vr/draw_capture.cpp src/acz-vr/root_binding_metadata.cpp src/acz-vr/srt_payload.cpp src/acz-vr/native_binding_observer.cpp src/acz-vr/buffer_binding_registry.cpp src/acz-vr/capture_enablement.cpp src/acz-vr/capture_input_provider.cpp /link d3d12.lib bcrypt.lib
