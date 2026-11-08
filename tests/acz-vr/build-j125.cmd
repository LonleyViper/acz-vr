@call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
@if errorlevel 1 exit /b %errorlevel%
@set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
@python -c "from pathlib import Path; p=Path('src/acz-vr/.j125-build'); p.mkdir(exist_ok=True); (p/'.gitignore').write_text('*\n')"
@if errorlevel 1 exit /b %errorlevel%
@cmake -S src/acz-vr -B src/acz-vr/.j125-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
@if errorlevel 1 exit /b %errorlevel%
@cmake --build src/acz-vr/.j125-build
@if errorlevel 1 exit /b %errorlevel%
@ctest --test-dir src/acz-vr/.j125-build -C Release --output-on-failure
