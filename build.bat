@echo off
REM -O3            Aggressive optimization + auto-vectorization
REM -march=native  Target this machine's full instruction set (AVX, etc.)
REM -DNDEBUG       Disable asserts and standard-library debug checks
g++^
 -std=c++17^
 -Wall^
 -Wextra^
 -Wpedantic^
 -O3^
 -march=native^
 -DNDEBUG^
 -I. "src/main.cpp"^
 -o mapgen.exe
