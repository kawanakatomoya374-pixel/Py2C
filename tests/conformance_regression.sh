#!/bin/sh
set -eu

CC=${CC:-cc}
# 出力先（build/tests）は他のターゲットの clean で消えることがあるため、
# 呼び出し側に依存せずここで用意する。
mkdir -p build/tests
P2C_COMPILER=${P2C_COMPILER:-./build/python-code-to-c}
P2C_TEST_CFLAGS=${P2C_TEST_CFLAGS:--Wall -Wextra -Werror -std=gnu11}
P2C_TEST_LDFLAGS=${P2C_TEST_LDFLAGS:-}
# 実行時の環境変数（ASAN_OPTIONS=... など）。厳格プロファイルが設定する。
P2C_TEST_ENV=${P2C_TEST_ENV:-}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p build/tests

run_differential_case() {
    case_id="$1"
    pyfile="$2"
    expected_lines="$3"
    stem=$(basename "$pyfile" .py)
    cfile="build/tests/${stem}.c"
    pyout="build/tests/${stem}.python.out"
    cout="build/tests/${stem}.c.out"
    bin="build/tests/${stem}"

    python3 "$pyfile" > "$pyout"
    "$P2C_COMPILER" "$pyfile" -o "$cfile"
    $CC -I./include $P2C_TEST_CFLAGS "$cfile" \
        src/runtime/python_code_to_c_runtime.c \
        src/common/python_code_to_c_common.c \
        src/platform/python_code_to_c_platform.c \
        src/platform/python_code_to_c_platform_hosted.c \
        src/platform/python_code_to_c_gui.c \
        src/modules/python_code_to_c_pygame.c $P2C_TEST_LDFLAGS -lm -o "$bin"
    # shellcheck disable=SC2086
    if ! env $P2C_TEST_ENV "$bin" > "$cout" 2> "$cout.stderr"; then
        printf '%s\n' "${case_id}: generated program exited non-zero" >&2
        head -20 "$cout.stderr" | sed 's/^/    /' >&2
        exit 1
    fi
    # サニタイザ実行時は、レポート自体を失敗として扱う（CPython差分より先に検出する）。
    if [ -n "$P2C_TEST_ENV" ] && grep -qE 'Sanitizer|runtime error:|SUMMARY: ' "$cout.stderr"; then
        printf '%s\n' "${case_id}: sanitizer report" >&2
        grep -m4 -oE '[A-Za-z]*Sanitizer[^:]*|runtime error: [^ ]+ [^ ]+|SUMMARY: [^ ]+' "$cout.stderr" | sed 's/^/    /' >&2
        exit 1
    fi
    if [ -n "$P2C_TEST_ENV" ] && [ -s "$cout.stderr" ]; then
        printf '%s\n' "${case_id}: stderr under sanitizer profile:" >&2
        head -10 "$cout.stderr" | sed 's/^/    /' >&2
    fi
    diff -u "$pyout" "$cout"

    actual_lines=$(wc -l < "$cout" | tr -d ' ')
    if [ "$actual_lines" != "$expected_lines" ]; then
        printf '%s\n' "${case_id}: expected ${expected_lines} assertions, got ${actual_lines}" >&2
        exit 1
    fi
    printf '%s\n' "${case_id}: ${actual_lines} CPython-differential assertions passed"
}

run_differential_case C01-C60 tests/complex_alpha10.py 60
run_differential_case C61-C76 tests/semantic_conformance_alpha10.py 16
run_differential_case C81-C83 tests/dict_comprehension_alpha10.py 3
run_differential_case C84-C85 tests/multi_with_alpha10.py 2
run_differential_case C86-C92 tests/dict_builtin_alpha10.py 7
run_differential_case C93-C97 tests/empty_and_reversed_alpha10.py 5
run_differential_case C98-C100 tests/enumerate_start_alpha10.py 3
run_differential_case C101-C108 tests/dict_order_mutation_alpha10.py 8
run_differential_case C109-C117 tests/set_methods_alpha10.py 9
run_differential_case C118-C126 tests/set_algebra_alpha10.py 9
run_differential_case C127-C128 tests/dict_equality_alpha10.py 2
run_differential_case C129-C132 tests/numeric_hash_contract_alpha10.py 4
run_differential_case C133-C144 tests/unhashable_keys_alpha10.py 12
run_differential_case C145-C148 tests/tuple_hash_contract_alpha10.py 4
run_differential_case C149-C161 tests/slice_and_index_alpha10.py 13
run_differential_case C162-C167 tests/integer_operator_semantics_alpha10.py 6
run_differential_case C168-C173 tests/fstring_evaluation_alpha10.py 6
run_differential_case C174 tests/type_annotations_alpha10.py 1
run_differential_case C175-C178 tests/walrus_alpha10.py 4
run_differential_case C179-C189 tests/container_methods_alpha10.py 11
run_differential_case C190-C194 tests/match_case_alpha10.py 5
run_differential_case C195-C197 tests/dict_merge_alpha10.py 3
run_differential_case C198-C199 tests/bare_raise_alpha10.py 2
run_differential_case C200-C203 tests/string_methods_alpha10.py 4
run_differential_case C204-C207 tests/for_starred_unpack_alpha10.py 4
run_differential_case C208-C211 tests/print_keywords_alpha10.py 4
run_differential_case C212-C215 tests/string_index_alpha10.py 4
run_differential_case C216-C220 tests/string_replace_count_alpha10.py 5
run_differential_case C221-C224 tests/list_index_bounds_alpha10.py 4
run_differential_case C225-C234 tests/string_search_bounds_alpha10.py 10
run_differential_case C235-C240 tests/string_split_maxsplit_alpha10.py 6
run_differential_case C241-C250 tests/string_edge_methods_alpha10.py 10
run_differential_case C251-C256 tests/string_partition_alpha10.py 6
run_differential_case C257-C268 tests/string_classification_and_lines_alpha10.py 12
run_differential_case C269-C274 tests/string_expandtabs_alpha10.py 7
run_differential_case C275-C282 tests/dict_set_advanced_alpha10.py 8
run_differential_case C283-C287 tests/async_generator_alpha10.py 5
run_differential_case C288-C299 tests/numeric_text_builtins_alpha10.py 12
run_differential_case C300-C302 tests/posonly_args_alpha10.py 3
run_differential_case C303 tests/yield_from_alpha10.py 1
run_differential_case C304 tests/raise_from_alpha10.py 1
run_differential_case C305-C314 tests/match_patterns_alpha10.py 10
run_differential_case C315-C322 tests/class_patterns_alpha10.py 8
run_differential_case C323-C328 tests/class_positional_patterns_alpha10.py 6
run_differential_case C329-C331 tests/dotted_value_patterns_alpha10.py 3
run_differential_case C332-C334 tests/dotted_class_patterns_alpha10.py 3
run_differential_case C335-C339 tests/generator_expression_alpha10.py 5
run_differential_case C340 tests/await_compare_bool_alpha10.py 1
run_differential_case C341 tests/async_for_alpha10.py 1
run_differential_case C342 tests/async_for_control_alpha10.py 1
run_differential_case C343-C346 tests/nonlocal_capture_alpha10.py 4
run_differential_case C347 tests/lambda_capture_alpha10.py 1
run_differential_case C348 tests/generator_expression_capture_alpha10.py 1
run_differential_case C349 tests/generator_expression_call_asan_alpha10.py 1
run_differential_case C350-C353 tests/builtin_key_semantics_alpha10.py 4
run_differential_case C354 tests/exception_cause_alpha10.py 1
run_differential_case C355 tests/with_parenthesized_alpha10.py 1
run_differential_case C356-C357 tests/with_unpack_target_alpha10.py 2
run_differential_case C358-C362 tests/async_with_alpha10.py 5
run_differential_case C363-C365 tests/set_comprehension_alpha10.py 3
run_differential_case C366-C372 tests/decorator_alpha10.py 7
run_differential_case C373-C377 tests/async_with_multiple_alpha10.py 5
run_differential_case C378-C383 tests/async_with_multiple_exception_alpha10.py 6
run_differential_case C384-C402 tests/finally_unwind_alpha10.py 19
run_differential_case C403-C428 tests/builtin_gap_alpha10.py 26
run_differential_case C429-C443 tests/ellipsis_alpha10.py 15
run_differential_case C444-C460 tests/string_escapes_alpha10.py 17
run_differential_case C461-C479 tests/nested_class_alpha10.py 19
run_differential_case C480-C486 tests/c_identifier_collision_alpha10.py 7
run_differential_case C487-C493 tests/multiple_inheritance_alpha10.py 7
run_differential_case C494-C499 tests/bound_method_alpha10.py 6
run_differential_case C500-C505 tests/finally_control_flow_alpha10.py 6
run_differential_case C506-C515 tests/generator_expression_multi_alpha10.py 10
run_differential_case C516-C525 tests/tuple_ordering_alpha10.py 10
run_differential_case C526-C528 tests/super_mro_alpha10.py 3
run_differential_case C529-C538 tests/method_decorators_alpha10.py 10
run_differential_case C539-C549 tests/math_module_alpha10.py 11
run_differential_case C550-C555 tests/property_setter_alpha10.py 6
run_differential_case C556-C576 tests/percent_format_alpha10.py 21
run_differential_case C577-C588 tests/complex_program_alpha10.py 12
run_differential_case C589-C593 tests/repro_super3_alpha10.py 5
run_differential_case C594-C615 tests/strict_convert_alpha10.py 22
run_differential_case C616-C647 tests/unicode_codepoint_alpha10.py 32
run_differential_case C648-C657 tests/dict_set_resize_alpha10.py 10
run_differential_case C658-C667 tests/range_lazy_alpha10.py 23
run_differential_case C668-C681 tests/range_slice_hash_alpha10.py 14
run_differential_case C682-C691 tests/slice_assign_alpha10.py 10
run_differential_case C692-C702 tests/unicode_width_alpha10.py 11
run_differential_case C703-C712 tests/number_literals_alpha10.py 10
run_differential_case C713-C722 tests/format_spec_alpha10.py 10
run_differential_case C723-C732 tests/semicolon_block_alpha10.py 13
run_differential_case C733-C740 tests/rsplit_alpha10.py 8
run_differential_case C741-C746 tests/format_kw_alpha10.py 6
run_differential_case C747-C749 tests/key_builtin_alpha10.py 3
run_differential_case C750-C763 tests/stress_runtime_alpha10.py 14
run_differential_case C764-C770 tests/empty_split_alpha10.py 7
# バグ修正の回帰（C11予約語、連鎖代入、関数の値参照、in-place拡張代入、
# tuple連結、__bool__/__len__、isinstance(True,int)、round負桁、min/max空、
# repr引用符、range表示/属性、例外args、format %、starred代入、lambda既定引数、
# 高階関数、str.istitle）
run_differential_case F01-F33 tests/fixes_alpha10.py 33
# Round-3 で追加した構文の回帰（format フィールド/入れ子書式、クラスからの
# メソッド取り出し、type(None) 判定、ループ内 yield を持つジェネレータ、
# ジェネレータメソッド、x = yield v）。
run_differential_case R01-R29 tests/round3_alpha10.py 29
# Round-4 で追加・修正した機能の回帰（リテラル展開、send()、PEP380 の return 値、
# ジェネレータ内 assert/型注釈、入れ子アンパック、__lt__ によるソート、例外 .args、
# type(x).__name__、組み込み例外基底の super().__init__）。
run_differential_case R30-R44 tests/round4_alpha10.py 15
printf '%s\n' 'conformance_regression_ok: 847 semantic assertions passed'
