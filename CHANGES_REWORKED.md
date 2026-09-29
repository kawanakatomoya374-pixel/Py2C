# Python Code to C 0.2 - change summary

## Core goals
- build reliability first
- traceable generated C first
- portable hosted / hobby-OS split first

## Added / improved in 0.2
- list literal / get / set
- dict literal / get / set
- tuple literal / get
- class transpilation
  - constructor path via `__init__`
  - instance attrs via `self.x`
  - method dispatch via runtime method table
  - simple class attributes
- import support
  - `import math`
  - `from math import sqrt`
  - runtime module registry API for future modules
- try / except support with exception name matching
- hosted I/O hooks split into `python_code_to_c_platform_hosted.c`
- platform boundary formalized in `python_code_to_c_platform.h`
- cross-compile and hobby-OS templates added under `templates/`
- lexer dedent stability fix for deeper block exits
- tuple parsing support for `(a, b, c)`

## Verification performed
- `make`
- `make test`
- `make smoke`
- end-to-end compile/run for:
  - containers
  - class instance + method + attrs
  - import math + from import
  - try / except ZeroDivisionError

## Output compile line
```sh
cc -I./include output.c src/python_code_to_c_runtime.c src/python_code_to_c_common.c src/python_code_to_c_platform_hosted.c -lm -o output
```
