setlocal
set errorlevel=dummy
set errorlevel=
setlocal
C:\Users\vince\AppData\Local\Programs\Python\Python314\Lib\site-packages\cmake\data\bin\cmake.exe -D TEST_TARGET=test_extract_moves -D TEST_EXECUTABLE=E:/coding_workspaces/CPP/chess-tube-analyzer/build_tests/Release/test_extract_moves.exe -D TEST_EXECUTOR= -D TEST_WORKING_DIR=E:/coding_workspaces/CPP/chess-tube-analyzer/build_tests -D TEST_EXTRA_ARGS= -D TEST_PROPERTIES= -D TEST_PREFIX= -D TEST_SUFFIX= -D TEST_FILTER= -D NO_PRETTY_TYPES=FALSE -D NO_PRETTY_VALUES=FALSE -D TEST_LIST=test_extract_moves_TESTS -D CTEST_FILE=E:/coding_workspaces/CPP/chess-tube-analyzer/build_tests/test_extract_moves[1]_tests.cmake -D TEST_DISCOVERY_TIMEOUT=5 -D TEST_DISCOVERY_EXTRA_ARGS= -D TEST_XML_OUTPUT_DIR= -P C:/Users/vince/AppData/Local/Programs/Python/Python314/Lib/site-packages/cmake/data/share/cmake-4.2/Modules/GoogleTestAddTests.cmake
if %errorlevel% neq 0 goto :cmEnd
:cmEnd
endlocal & call :cmErrorLevel %errorlevel% & goto :cmDone
:cmErrorLevel
exit /b %1
:cmDone
if %errorlevel% neq 0 goto :VCEnd
:VCEnd
exit %errorlevel%
