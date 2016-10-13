:: Copyright 2015 Google Inc. All Rights Reserved.
::
:: Licensed under the Apache License, Version 2.0 (the "License");
:: you may not use this file except in compliance with the License.
:: You may obtain a copy of the License at
::
::     http://www.apache.org/licenses/LICENSE-2.0
::
:: Unless required by applicable law or agreed to in writing, software
:: distributed under the License is distributed on an "AS IS" BASIS,
:: WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
:: See the License for the specific language governing permissions and
:: limitations under the License.
:: ==============================================================================

:: If possible, read swig path out of "swig_path" generated by configure
@echo OFF
set SWIG=swig
set SWIG_PATH=tensorflow\tools\swig\swig_path

if EXIST "%SWIG_PATH%" (for /f %%i in ('type %SWIG_PATH%') do set SWIG="%%i")
:: If this line fails, rerun configure to set the path to swig correctly
%SWIG% %*