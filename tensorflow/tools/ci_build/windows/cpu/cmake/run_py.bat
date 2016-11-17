:: This script assumes the standard setup on tensorflow Jenkins windows machines.
:: It is NOT guaranteed to work on any other machine. Use at your own risk!
::
:: REQUIREMENTS:
:: * All installed in standard locations:
::   - JDK8, and JAVA_HOME set.
::   - Microsoft Visual Studio 2015 Community Edition
::   - Msys2
::   - Anaconda3
::   - CMake

:: Record the directory we are in. Script should be invoked from the root of the repository.
SET REPO_ROOT=%cd%

:: Make sure we have a clean directory to build things in.
SET BUILD_DIR=cmake_build
RMDIR %BUILD_DIR% /S /Q
MKDIR %BUILD_DIR%
CD %BUILD_DIR%

:: Set which tests to build
SET BUILD_CC_TESTS=OFF
SET BUILD_PYTHON_TESTS=ON

SET CONDA_EXE="C:\Program Files\Anaconda3\Scripts\conda"
SET ACTIVATE_EXE="C:\Program Files\Anaconda3\Scripts\activate"
SET DEACTIVATE_EXE="C:\Program Files\Anaconda3\Scripts\deactivate"
SET PIP_EXE="C:\Program Files\Anaconda3\Scripts\pip.exe"

:: Run the CMAKE build to build the pip package.
CALL %REPO_ROOT%\tensorflow\tools\ci_build\windows\cpu\cmake\run_build.bat
if %errorlevel% neq 0 exit /b %errorlevel%

:: Create a conda environment with a unique name.
:: Import all bunch of variables Visual Studio needs.
CALL "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat"
uuidgen > uuid.txt
set /p ENV_NAME=<uuid.txt

%CONDA_EXE% create --name %ENV_NAME% numpy
%ACTIVATE_EXE% %ENV_NAME%

:: Install the pip package.
%PIP_EXE% install --upgrade %REPO_ROOT%\%BUILD_DIR%\tf_python\dist\tensorflow-0.11.0rc2_cmake_experimental-py3-none-any.whl
if %errorlevel% neq 0 exit /b %errorlevel%

:: Run all python tests
ctest -C Release --output-on-failure
if %errorlevel% neq 0 exit /b %errorlevel%

%DEACTIVATE_EXE%
%CONDA_EXE% env remove %ENV_NAME%