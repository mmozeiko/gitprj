@echo off
setlocal enabledelayedexpansion

where /Q cl.exe || (
  set __VSCMD_ARG_NO_LOGO=1
  for /f "tokens=*" %%i in ('"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath') do set VS=%%i
  if "!VS!" equ "" (
    echo ERROR: Visual Studio installation not found
    exit /b 1
  )  
  call "!VS!\VC\Auxiliary\Build\vcvarsall.bat" amd64 || exit /b 1
)

if "%VSCMD_ARG_TGT_ARCH%" neq "x64" (
  echo ERROR: please run this from MSVC x64 native tools command prompt, 32-bit target is not supported!
  exit /b 1
)

if not exist "libgit2\lib\git2.lib" (
  call git clone --depth 1 --branch v1.7.0 https://github.com/libgit2/libgit2.git libgit2.src || exit /b 1
  call git apply -p0 --directory=libgit2.src libgit2.patch || exit /b 1
  cmake -B libgit2.build -S libgit2.src -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DBUILD_CLI=OFF -DUSE_HTTPS=OFF || exit /b 1
  set CL=/Zl
  cmake --build libgit2.build --config Release || exit /b 1
  set CL=
  cmake --install libgit2.build --prefix %CD%/libgit2 || exit /b 1
  rd /Q /S libgit2.build libgit2.src
)

if "%1" equ "debug" (
  set CL=/MTd /Od /Zi /D_DEBUG /RTC1 /fsanitize=address
  set LINK=/DEBUG
) else (
  set CL=/GL /O2 /DNDEBUG /GS-
  set LINK=/LTCG /OPT:REF /OPT:ICF
)

cl.exe /nologo /FC /W3 /WX gitprj.c /Ilibgit2/include /link /INCREMENTAL:NO /LIBPATH:libgit2/lib
