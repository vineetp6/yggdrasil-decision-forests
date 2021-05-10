:: Copyright 2021 Google LLC.
::
:: Licensed under the Apache License, Version 2.0 (the "License");
:: you may not use this file except in compliance with the License.
:: You may obtain a copy of the License at
::
::     https://www.apache.org/licenses/LICENSE-2.0
::
:: Unless required by applicable law or agreed to in writing, software
:: distributed under the License is distributed on an "AS IS" BASIS,
:: WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
:: See the License for the specific language governing permissions and
:: limitations under the License.

:: Compile and runs the unit tests.

set BAZEL=bazel-4.0.0-windows-x86_64.exe
set FLAGS_WO_TF=--config=windows_cpp17
set FLAGS_W_TF=--config=windows_cpp14 --config=use_tensorflow_io

%BAZEL% version

%BAZEL% build %FLAGS_WO_TF% //yggdrasil_decision_forests/cli/...:all || goto :error

%BAZEL% build %FLAGS_W_TF% //yggdrasil_decision_forests/cli/...:all || goto :error

setlocal enabledelayedexpansion
for %%x in (
cli
dataset
learner
metric
model
serving
  ) do (
  %BAZEL% test %FLAGS_W_TF% --config=use_tensorflow_io //yggdrasil_decision_forests/%%x/...:all || goto :error
  )

goto :EOF

:error
echo Failed with error #%errorlevel%.
exit /b %ERRORLEVEL%
