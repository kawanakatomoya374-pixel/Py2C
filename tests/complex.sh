#!/bin/sh
set -eu
CC=${CC:-cc}
mkdir -p build/tests
python3 tests/complex_alpha10.py > build/tests/complex_alpha10.python.out
./build/python-code-to-c tests/complex_alpha10.py -o build/tests/complex_alpha10.c
$CC -I./include -Wall -Wextra -Werror -std=gnu11 build/tests/complex_alpha10.c \
  src/runtime/python_code_to_c_runtime.c \
  src/common/python_code_to_c_common.c \
  src/platform/python_code_to_c_platform.c \
  src/platform/python_code_to_c_platform_hosted.c \
  src/platform/python_code_to_c_gui.c \
  src/modules/python_code_to_c_pygame.c -lm -o build/tests/complex_alpha10
build/tests/complex_alpha10 > build/tests/complex_alpha10.c.out
diff -u build/tests/complex_alpha10.python.out build/tests/complex_alpha10.c.out
cases=$(wc -l < build/tests/complex_alpha10.c.out | tr -d ' ')
test "$cases" = "60"
printf '%s\n' 'complex_alpha10: 60 CPython-differential cases passed'
