#!/bin/sh
set -eu

P2C_COMPILER=${P2C_COMPILER:-./build/python-code-to-c}
mkdir -p build/tests

expect_failure() {
    source_file=$1
    expected_message=$2
    stem=$(basename "$source_file" .py)
    if "$P2C_COMPILER" "$source_file" -o "build/tests/${stem}.c" >"build/tests/${stem}.out" 2>"build/tests/${stem}.err"; then
        printf '%s\n' "decorator diagnostic unexpectedly succeeded: ${source_file}" >&2
        exit 1
    fi
    grep -F "$expected_message" "build/tests/${stem}.err" >/dev/null
}

expect_failure tests/decorator_keyword_call_rejection_alpha06.py 'keyword arguments for decorated functions are not supported yet'
expect_failure tests/decorator_vararg_rejection_alpha06.py 'functions with *args or **kwargs cannot be used as decorators yet'
expect_failure tests/decorator_nested_rejection_alpha06.py 'decorators are currently supported only on module-level functions'
expect_failure tests/decorator_method_rejection_alpha06.py 'decorators are currently supported only on module-level functions'
printf '%s\n' 'decorator_diagnostics_ok'
