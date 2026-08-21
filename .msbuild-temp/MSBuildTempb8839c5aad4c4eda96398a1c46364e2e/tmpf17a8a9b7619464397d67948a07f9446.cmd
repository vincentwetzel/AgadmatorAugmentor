setlocal
set errorlevel=dummy
set errorlevel=
echo Building Custom Rule E:/coding_workspaces/CPP/chess-tube-analyzer/CMakeLists.txt
setlocal
C:\Users\vince\AppData\Local\Programs\Python\Python314\Lib\site-packages\cmake\data\bin\cmake.exe -SE:/coding_workspaces/CPP/chess-tube-analyzer -BE:/coding_workspaces/CPP/chess-tube-analyzer/build_tests --check-stamp-file E:/coding_workspaces/CPP/chess-tube-analyzer/build_tests/CMakeFiles/generate.stamp
if %errorlevel% neq 0 goto :cmEnd
:cmEnd
endlocal & call :cmErrorLevel %errorlevel% & goto :cmDone
:cmErrorLevel
exit /b %1
:cmDone
if %errorlevel% neq 0 goto :VCEnd

:VCEnd
exit %errorlevel%
