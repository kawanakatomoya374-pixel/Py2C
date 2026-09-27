# Python Code to C Alpha0.6
# Hosted build: make
# Freestanding build: make TOOLCHAIN=templates/toolchains/freestanding-c11.mk
PROJECT := python-code-to-c
VERSION := 0.6.0
CC ?= cc
AR ?= ar
PYTHON ?= python3
INPUT ?= examples/fib.py
FREESTANDING_ARCH_CFLAGS ?=
FREESTANDING_EXTRA_CFLAGS ?=
P2C_FUZZ_CASES ?= 48
P2C_FUZZ_SEEDS ?= 12648430 24237 17412
# コンパイラ本体とfreestandingコアが共有する厳格な警告基準。
#   -Wconversion / -Wsign-conversion ... 暗黙の切り詰めと符号反転
#   -Wcast-qual / -Wwrite-strings   ... const契約の破壊
#   -Wvla                           ... 組込み向けに可変長スタック配列を排除
# 意図的に有効化しないフラグ:
#   -Wswitch-enum: ASTノード種別のswitchはdefault:節で未対応種別を診断する設計で
#     あり、全case列挙(1200件超)に見合う安全性を生まない。網羅性はtests/の
#     CPython差分回帰で担保する。
#   -Wnull-dereference: TU毎の通常ビルドでは0件だが、単一ヘッダ構成では
#     全ソースが1TUになりGCCの関数間解析が効くため、未チェック確保と誤検出が
#     混在した約300件の "potential null pointer dereference" を報告する。
#     有効化には全域のNULL契約監査が前提となるため、別途監査タスクとして扱う。
WARN_CFLAGS ?= -Wall -Wextra -Werror -Wpedantic \
	-Wshadow -Wformat=2 -Wno-format-nonliteral \
	-Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition \
	-Wredundant-decls -Wundef \
	-Wconversion -Wsign-conversion -Wcast-qual -Wwrite-strings \
	-Wdouble-promotion -Wvla -Wfloat-equal
CFLAGS ?= -std=c11 -O2 $(WARN_CFLAGS)
CPPFLAGS ?= -I./include
LDFLAGS ?=
LDLIBS ?= -lm
SANITIZER_CFLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer -g
# 変換済みCコードは、内包表記などの式位置一時構築でGNU statement expressionを
# 使うため、プロジェクト本体の -Wpedantic 基準ではなく、既存の回帰テストと
# 同じ「生成コード基準」でコンパイルする。ISO C11だけを対象にする場合は
# --c11 でその構文を変換時に拒否する。
GENERATED_CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror
# 自作OS統合テスト(embed)用: 標準Cライブラリのプラットフォーム層を使わず、
# p2c_embed が互換フックと malloc/free を提供する構成でビルドする。
EMBED_CFLAGS ?= -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME -DPYTHON_CODE_TO_C_NO_LIBC_STUBS -DP2C_EMBED_PROVIDE_LIBC_HEAP -DP2C_EMBED_PROVIDE_PLATFORM_COMPAT -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS)
# ホスト側（カーネル相当のタスク情報を提供する側）は標準Cライブラリ込みでビルドする。
HOST_STACK_CFLAGS ?= -std=c11 -O2 $(WARN_CFLAGS)
SANITIZER_ENV ?= ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
BUILD ?= build
OBJ ?= obj

# The GUI frontend has its own main(); keep it out of the default CLI link.
SRC := $(shell find src -type f -name '*.c' ! -path 'src/tools/gui_main.c' | sort)
OBJFILES := $(patsubst src/%.c,$(OBJ)/%.o,$(SRC))
DEPS := $(OBJFILES:.o=.d)

.PHONY: all help check-tools gui run run-gui clean test test-sanitizers test-parser-sanitizers test-gc-lifecycle test-gc-allocation-failure test-gc-leaks test-gc-stack-scan-scope test-gc-temp-roots install freestanding freestanding-clean test-set test-set-comprehension-c11 test-decorator-diagnostics test-conformance test-container-fuzz test-portability full-build single-header test-single-header test-single-header-c11 test-single-header-freestanding test-integer-overflow test-platform-adapter test-generator-async-runtime test-async-generator test-baremetal-runtime test-baremetal-exceptions test-baremetal-build test-baremetal-generated test-py313-syntax test-embed-runtime test-freestanding-setjmp test-embed-compile test-embed-generated test-hobby-os-template test-crlf test-allocator-injection test-setjmp-hook test-heap-unification
all: $(BUILD)/python-code-to-c
	@mkdir -p bin
	@ln -sf ../$(BUILD)/python-code-to-c bin/python_code_to_c

help:
	@printf '%s\n' 'Python Code to C Alpha0.6 build targets:'
	@printf '%s\n' '  make all                         Build the hosted CLI.'
	@printf '%s\n' '  make gui                         Build the local GUI frontend.'
	@printf '%s\n' '  make run INPUT=path/to/file.py   Build, transpile, compile, and run Python input.'
	@printf '%s\n' '  make run-gui                     Build and start the local GUI frontend.'
	@printf '%s\n' '  make freestanding CC=<cross-cc> AR=<cross-ar>  Build the C11 freestanding core.'
	@printf '%s\n' '  make single-header               Regenerate include/python_code_to_c_single.h.'
	@printf '%s\n' '  make test                        Run all hosted, C11, freestanding, CPython-differential, and deterministic fuzz tests.'
	@printf '%s\n' '  make test-container-fuzz         Run seed-reproducible dict/set CPython-differential fuzzing.'
	@printf '%s\n' '  make test-async-generator       Run CPython-differential generator and async/await smoke tests.'
	@printf '%s\n' '  make test-py313-syntax          Validate Python 3.13 type statements and type parameters by C11 conversion.'
	@printf '%s\n' '  make test-embed-runtime         Run the embedded (kernel-style) heap, lifecycle, GC and OOM regression.'
	@printf '%s\n' '  make test-freestanding-setjmp   Run the kernel-provided setjmp/longjmp exception regression.'
	@printf '%s\n' '  make test-embed-compile         Run the transpiler core inside the embedded environment.'
	@printf '%s\n' '  make test-embed-generated       Run a --embed-entry module on the kernel-style driver and diff CPython.'
	@printf '%s\n' '  make test-hobby-os-template     Compile the hobby OS templates with warnings as errors.'
	@printf '%s\n' '  make test-crlf                  Check LF/CRLF/CR sources produce identical C.'
	@printf '%s\n' '  make test-sanitizers            Run hosted parser/runtime regression under AddressSanitizer and UBSan.'
	@printf '%s\n' '  make test-baremetal-runtime     Run the static-heap, platform-write, GC, and coroutine bare-metal harness.'
	@printf '%s\n' '  make test-baremetal-exceptions  Check bare-metal raise/except, stack roots and the platform allocator.'
	@printf '%s\n' '  make test-gc-stack-scan-scope   Check conservative stack scanning stays within the used stack range.'
	@printf '%s\n' '  make test-gc-temp-roots         Check TLS expression temporaries are GC roots and unwound on exceptions.'
	@printf '%s\n' '  make test-allocator-injection   Check allocator injection (P2C_Platform/P2C_Allocator) for the compiler core.'
	@printf '%s\n' '  make test-setjmp-hook           Check the P2C_SETJMP/P2C_LONGJMP OS replacement hook.'
	@printf '%s\n' '  make test-heap-unification      Check that the runtime libc stub malloc shares the platform heap.'
	@printf '%s\n' '  make check-tools                 Verify the configured compiler, archiver, and Python launcher.'

check-tools:
	@command -v "$(CC)" >/dev/null || { printf '%s\n' "compiler not found: $(CC)" >&2; exit 1; }
	@command -v "$(AR)" >/dev/null || { printf '%s\n' "archiver not found: $(AR)" >&2; exit 1; }
	@command -v "$(PYTHON)" >/dev/null || { printf '%s\n' "Python launcher not found: $(PYTHON)" >&2; exit 1; }
	@printf '%s\n' "toolchain_ok: CC=$(CC) AR=$(AR) PYTHON=$(PYTHON)"

gui: $(BUILD)/python-code-to-c-gui

run: all
	./$(BUILD)/python-code-to-c run "$(INPUT)"

run-gui: gui
	./$(BUILD)/python-code-to-c-gui

GUI_OBJFILES := $(filter-out $(OBJ)/tools/main.o,$(OBJFILES))

$(BUILD)/python-code-to-c-gui: $(GUI_OBJFILES) $(OBJ)/tools/gui_main.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/python-code-to-c: $(OBJFILES)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJ)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# Build the compiler core without hosted OS facilities. A target OS supplies
# allocation, I/O, diagnostics and process hooks through platform callbacks.
freestanding:
	$(MAKE) -C "$(CURDIR)" -f templates/toolchains/freestanding-c11.mk PROJECT_ROOT=. CC="$(CC)" AR="$(AR)" WARN_CFLAGS="$(WARN_CFLAGS)" FREESTANDING_ARCH_CFLAGS="$(FREESTANDING_ARCH_CFLAGS)" EXTRA_CFLAGS="$(FREESTANDING_EXTRA_CFLAGS)"

freestanding-clean:
	$(MAKE) -C "$(CURDIR)" -f templates/toolchains/freestanding-c11.mk PROJECT_ROOT=. BUILD=build/freestanding clean

single-header:
	sh tools/generate_single_header.sh

test-single-header: single-header
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_single_header.c $(LDLIBS) -o build/tests/test_single_header
	build/tests/test_single_header

test-single-header-c11: single-header
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -pedantic-errors tests/test_single_header.c $(LDLIBS) -o build/tests/test_single_header_c11
	build/tests/test_single_header_c11

test-single-header-freestanding: single-header
	@mkdir -p build/tests
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) -c tests/test_single_header_freestanding.c -o build/tests/test_single_header_freestanding.o
	@if nm -u build/tests/test_single_header_freestanding.o | grep -E '(^| )U (malloc|realloc|free|fwrite|fputs|clock)$$' >/dev/null; then echo 'freestanding single header has hosted libc references'; exit 1; fi

full-build: check-tools
	$(MAKE) clean
	$(MAKE) CC="$(CC)" AR="$(AR)" all gui
	$(MAKE) CC="$(CC)" AR="$(AR)" freestanding
	$(MAKE) CC="$(CC)" AR="$(AR)" single-header

test: all
	sh tests/smoke.sh
	$(MAKE) test-gc-gui
	$(MAKE) test-gc-lifecycle
	$(MAKE) test-gc-stack-scan-scope
	$(MAKE) test-gc-temp-roots
	$(MAKE) test-gc-allocation-failure
	$(MAKE) test-integer-overflow
	$(MAKE) test-platform-adapter
	$(MAKE) test-allocator-injection
	$(MAKE) test-setjmp-hook
	$(MAKE) test-heap-unification
	$(MAKE) test-generator-async-runtime
	$(MAKE) test-async-generator
	$(MAKE) test-py313-syntax
	$(MAKE) test-baremetal-runtime
	$(MAKE) test-baremetal-exceptions
	$(MAKE) test-baremetal-build
	$(MAKE) test-baremetal-generated
	$(MAKE) test-embed-runtime
	$(MAKE) test-freestanding-setjmp
	$(MAKE) test-embed-compile
	$(MAKE) test-embed-generated
	$(MAKE) test-crlf
	$(MAKE) test-hobby-os-template
	$(MAKE) test-starred-unpack
	$(MAKE) test-set
	$(MAKE) test-set-comprehension-c11
	$(MAKE) test-decorator-diagnostics
	$(MAKE) test-single-header
	$(MAKE) test-single-header-c11
	$(MAKE) test-single-header-freestanding
	$(MAKE) test-conformance
	$(MAKE) test-container-fuzz
	sh tests/audit_regression.sh
	$(MAKE) test-portability

test-sanitizers:
	$(MAKE) clean
	$(MAKE) BUILD=build/sanitize OBJ=obj/sanitize CFLAGS="$(CFLAGS) $(SANITIZER_CFLAGS)" LDFLAGS="$(LDFLAGS) $(SANITIZER_CFLAGS)" test-parser-sanitizers
	$(MAKE) BUILD=build/sanitize OBJ=obj/sanitize CFLAGS="$(CFLAGS) $(SANITIZER_CFLAGS)" LDFLAGS="$(LDFLAGS) $(SANITIZER_CFLAGS)" test-gc-lifecycle
	$(MAKE) BUILD=build/sanitize OBJ=obj/sanitize CFLAGS="$(CFLAGS) $(SANITIZER_CFLAGS)" LDFLAGS="$(LDFLAGS) $(SANITIZER_CFLAGS)" test-gc-allocation-failure
	$(SANITIZER_ENV) P2C_COMPILER=./build/sanitize/python-code-to-c P2C_TEST_CFLAGS="-Wall -Wextra -Werror -std=gnu11 $(SANITIZER_CFLAGS)" P2C_TEST_LDFLAGS="$(SANITIZER_CFLAGS)" CC="$(CC)" sh tests/conformance_regression.sh

test-parser-sanitizers: all
	@mkdir -p $(BUILD)/tests
	$(SANITIZER_ENV) ./$(BUILD)/python-code-to-c tests/parser_starred_unpack_asan_alpha06.py -o $(BUILD)/tests/parser_starred_unpack_asan_alpha06.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BUILD)/tests/parser_starred_unpack_asan_alpha06.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDFLAGS) $(LDLIBS) -o $(BUILD)/tests/parser_starred_unpack_asan_alpha06
	$(SANITIZER_ENV) $(BUILD)/tests/parser_starred_unpack_asan_alpha06 > $(BUILD)/tests/parser_starred_unpack_asan_alpha06.out
	printf '10 [20, 30] 40\n1 (2, 3, 4) 5\n' > $(BUILD)/tests/parser_starred_unpack_asan_alpha06.expected
	diff -u $(BUILD)/tests/parser_starred_unpack_asan_alpha06.expected $(BUILD)/tests/parser_starred_unpack_asan_alpha06.out

test-starred-unpack:
	@mkdir -p build/tests
	./build/python-code-to-c tests/starred_unpack_alpha06.py -o build/tests/starred_unpack_alpha06.c
	$(CC) $(CPPFLAGS) $(CFLAGS) build/tests/starred_unpack_alpha06.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDLIBS) -o build/tests/starred_unpack_alpha06
	build/tests/starred_unpack_alpha06 > build/tests/starred_unpack_alpha06.out
	printf '1 [2, 3, 4] 5\n[10, 20] 30\n7 [8, 9]\n' > build/tests/starred_unpack_alpha06.expected
	diff -u build/tests/starred_unpack_alpha06.expected build/tests/starred_unpack_alpha06.out

test-set: all
	sh tests/set_regression.sh

test-set-comprehension-c11: all
	@mkdir -p build/tests
	@if ./build/python-code-to-c --c11 tests/set_comprehension_alpha06.py -o build/tests/set_comprehension_alpha06.c11.c >build/tests/set_comprehension_alpha06.c11.out 2>build/tests/set_comprehension_alpha06.c11.err; then printf '%s\n' 'set comprehension unexpectedly passed --c11' >&2; exit 1; fi
	@grep -F 'comprehensions are unavailable in strict ISO C11 mode' build/tests/set_comprehension_alpha06.c11.err >/dev/null
	@printf '%s\n' 'set_comprehension_c11_diagnostic_ok'

test-decorator-diagnostics: all
	sh tests/decorator_diagnostics_alpha06.sh

test-conformance: all
	sh tests/conformance_regression.sh

test-container-fuzz: all
	CC="$(CC)" P2C_FUZZ_CASES="$(P2C_FUZZ_CASES)" P2C_FUZZ_SEEDS="$(P2C_FUZZ_SEEDS)" sh tests/container_fuzz_regression.sh

test-portability:
	$(MAKE) gui
	$(MAKE) freestanding
	test -s build/python-code-to-c-gui
	test -s build/freestanding/libpython-code-to-c-core.a
	@printf '%s\n' 'C80: GUI and freestanding portability artifacts passed'

test-gc-gui:
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_gc_gui.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDLIBS) -o build/tests/test_gc_gui
	build/tests/test_gc_gui

test-gc-lifecycle:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_gc_runtime_reinit.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDFLAGS) $(LDLIBS) -o $(BUILD)/tests/test_gc_runtime_reinit
	$(SANITIZER_ENV) $(BUILD)/tests/test_gc_runtime_reinit

test-gc-allocation-failure:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_gc_allocation_failure.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDFLAGS) -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc $(LDLIBS) -o $(BUILD)/tests/test_gc_allocation_failure
	$(SANITIZER_ENV) $(BUILD)/tests/test_gc_allocation_failure

test-gc-leaks:
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZER_CFLAGS) tests/test_gc_runtime_reinit.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(SANITIZER_CFLAGS) $(LDLIBS) -o build/tests/test_gc_runtime_reinit_lsan
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 build/tests/test_gc_runtime_reinit_lsan

test-integer-overflow:
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_integer_overflow_runtime.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDLIBS) -o build/tests/test_integer_overflow_runtime
	build/tests/test_integer_overflow_runtime

test-platform-adapter:
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_platform_adapter.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDLIBS) -o build/tests/test_platform_adapter
	build/tests/test_platform_adapter

# 変換器コア / ランタイムへのアロケータ注入（自作OSの kmalloc/arena を唯一の
# ヒープにする）と、静的フォールバックへ落ちる条件の回帰。
test-allocator-injection: all
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_allocator_injection.c \
		$(filter-out $(OBJ)/tools/main.o,$(OBJFILES)) $(LDLIBS) -o build/tests/test_allocator_injection
	build/tests/test_allocator_injection

# setjmp/longjmp のOS差し替えフック (P2C_SETJMP/P2C_LONGJMP) の回帰。
# テスト本体とランタイム本体の両方を同じオーバーライドでコンパイルし、
# ランタイム/生成コードが本当にフックを通ることを検証する。
test-setjmp-hook: all
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -include tests/setjmp_hook_override.h \
		tests/test_setjmp_hook.c src/runtime/python_code_to_c_runtime.c \
		$(filter-out $(OBJ)/runtime/python_code_to_c_runtime.o $(OBJ)/tools/main.o,$(OBJFILES)) \
		$(LDLIBS) -o build/tests/test_setjmp_hook
	build/tests/test_setjmp_hook

# 共有ヒープの一本化（NO_STDLIB 構成）の回帰。ランタイム同梱 libc スタブの
# malloc 系がカーネルアロケータ（共有ヒープ）へ委譲されること、raw malloc と
# p2c_heap_alloc() のブロックが相互に解放できること、プラットフォーム未設定時は
# 線形ヒープが唯一のヒープになること、ヒープ未設定では確保が NULL になることを検証する。
test-heap-unification:
	@mkdir -p build/tests
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) \
		tests/test_heap_unification.c src/core/python_code_to_c.c src/lexer/python_code_to_c_lexer.c \
		src/parser/python_code_to_c_ast.c src/parser/python_code_to_c_astdump.c src/parser/python_code_to_c_parser.c \
		src/semantic/python_code_to_c_semantic.c src/codegen/python_code_to_c_codegen.c \
		src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c \
		src/platform/python_code_to_c_platform.c $(LDLIBS) -o build/tests/test_heap_unification
	build/tests/test_heap_unification

test-generator-async-runtime:
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_generator_async_runtime.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDLIBS) -o build/tests/test_generator_async_runtime
	build/tests/test_generator_async_runtime

test-async-generator: all
	@mkdir -p build/tests
	$(PYTHON) tests/cases_async_generator_smoke.py > build/tests/async_generator.expected
	./build/python-code-to-c run tests/cases_async_generator_smoke.py > build/tests/async_generator.actual
	diff -u build/tests/async_generator.expected build/tests/async_generator.actual

test-py313-syntax: all
	@mkdir -p build/tests
	./build/python-code-to-c tests/py313_type_params_syntax_alpha06.py -o build/tests/py313_type_params_syntax_alpha06.c
	$(CC) $(CPPFLAGS) $(CFLAGS) build/tests/py313_type_params_syntax_alpha06.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDLIBS) -o build/tests/py313_type_params_syntax_alpha06
	printf '42\n' > build/tests/py313_type_params_syntax_alpha06.expected
	build/tests/py313_type_params_syntax_alpha06 > build/tests/py313_type_params_syntax_alpha06.actual
	diff -u build/tests/py313_type_params_syntax_alpha06.expected build/tests/py313_type_params_syntax_alpha06.actual
	./build/python-code-to-c tests/py313_type_params_extended_syntax_alpha06.py -o build/tests/py313_type_params_extended_syntax_alpha06.c
	$(CC) $(CPPFLAGS) $(CFLAGS) build/tests/py313_type_params_extended_syntax_alpha06.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDLIBS) -o build/tests/py313_type_params_extended_syntax_alpha06
	printf '15\n' > build/tests/py313_type_params_extended_syntax_alpha06.expected
	build/tests/py313_type_params_extended_syntax_alpha06 > build/tests/py313_type_params_extended_syntax_alpha06.actual
	diff -u build/tests/py313_type_params_extended_syntax_alpha06.expected build/tests/py313_type_params_extended_syntax_alpha06.actual

test-baremetal-runtime:
	@mkdir -p build/tests
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) tests/test_baremetal_runtime.c examples/baremetal/python_code_to_c_baremetal.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c $(LDLIBS) -o build/tests/test_baremetal_runtime
	build/tests/test_baremetal_runtime

# 自作OS統合ファサード(p2c_embed)の回帰: 境界タグ付きヒープ、ライフサイクル、
# GCの安全側停止、OOM通知とMemoryError化。NO_STDLIB + カーネル提供setjmpで、
# Hostedプラットフォームを一切使わずに検証する。
test-embed-runtime:
	@mkdir -p build/tests
	$(CC) -I./include -I./examples/embed -std=c11 -O2 $(WARN_CFLAGS) -c examples/embed/host_stack_bounds.c -o build/tests/host_stack_bounds.o
	$(CC) -I./include -I./examples/embed -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME -DPYTHON_CODE_TO_C_NO_LIBC_STUBS -DP2C_EMBED_PROVIDE_LIBC_HEAP -DP2C_EMBED_PROVIDE_PLATFORM_COMPAT -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) \
		tests/test_embed_runner.c examples/embed/x86_64_setjmp.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_embed.c build/tests/host_stack_bounds.o $(LDLIBS) -o build/tests/test_embed_runner
	build/tests/test_embed_runner

# freestanding な setjmp/longjmp 契約（カーネル提供）と例外機構の回帰。
# x86-64 参考実装をリンクし、NO_STDLIB で raise/except/入れ子フレームを検証する。
test-freestanding-setjmp:
	@mkdir -p build/tests
	$(CC) -I./include -I./examples/embed $(HOST_STACK_CFLAGS) -c examples/embed/host_stack_bounds.c -o build/tests/host_stack_bounds.o
	$(CC) -I./include -I./examples/embed $(EMBED_CFLAGS) \
		tests/test_freestanding_setjmp.c examples/embed/x86_64_setjmp.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_embed.c build/tests/host_stack_bounds.o $(LDLIBS) -o build/tests/test_freestanding_setjmp
	build/tests/test_freestanding_setjmp

# カーネル相当環境で変換器コア自体を動かす回帰（オンデバイス変換）。
# NO_STDLIB で Python ソース→C 変換が完走し、--embed-entry相当のAPIも動くことを確認する。
test-embed-compile:
	@mkdir -p build/tests
	$(CC) -I./include $(EMBED_CFLAGS) \
		tests/test_embed_transpile.c src/core/python_code_to_c.c src/lexer/python_code_to_c_lexer.c \
		src/parser/python_code_to_c_ast.c src/parser/python_code_to_c_astdump.c src/parser/python_code_to_c_parser.c \
		src/semantic/python_code_to_c_semantic.c src/codegen/python_code_to_c_codegen.c \
		src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c \
		src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_embed.c $(LDLIBS) -o build/tests/test_embed_transpile
	build/tests/test_embed_transpile

# 改行コード（CRLF/CR/LF）に対して生成Cが完全に一致することを確認する回帰。
# Windowsで保存されたPythonソースが誤変換されないための品質ゲート。
test-crlf:
	@mkdir -p build/tests
	sh tests/crlf_source_regression.sh

# 自作OSテンプレートのコンパイル検証（警告即エラー）。テンプレートが
# 現行APIから乖離してビルドできなくなることを防ぐ。
test-hobby-os-template: all
	@mkdir -p build/tests
	$(CC) -I./include -I./templates/hobby_os $(EMBED_CFLAGS) -c templates/hobby_os/embed/hobby_os_embed.c -o build/tests/hobby_os_embed.o
	$(CC) -I./include -I./templates/hobby_os $(EMBED_CFLAGS) -c templates/hobby_os/platform/python_code_to_c_platform_user.c -o build/tests/hobby_os_platform_user.o
	$(MAKE) -f templates/hobby_os/hobby_os.mk PROJECT_ROOT=. clean all
	@test -s build/hobby_os/libpython-code-to-c-hobby-os.a

# 変換済みPythonモジュールをカーネル相当のドライバで実際に走らせる統合テスト。
# --embed-entry で main の無いモジュールを生成し、p2c_embed が提供する
# ヒープ/シンク/スタック境界/panic経路だけで実行してCPythonと差分比較する。
test-embed-generated: all
	@if [ "$$(uname -m)" != "x86_64" ]; then printf '%s\n' 'test-embed-generated: skipped (x86-64 setjmp reference implementation required)'; exit 0; fi
	@mkdir -p build/embed
	./build/python-code-to-c examples/embed/embed_boot.py --embed-entry p2c_embed_program -o build/embed/embed_boot.c
	$(CC) -I./include -I./examples/embed -std=c11 -O2 $(WARN_CFLAGS) -c examples/embed/host_stack_bounds.c -o build/embed/host_stack_bounds.o
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME $(GENERATED_CFLAGS) -c build/embed/embed_boot.c -o build/embed/embed_boot.o
	$(CC) -I./include -I./examples/embed $(EMBED_CFLAGS) -c examples/embed/embed_driver.c -o build/embed/embed_driver.o
	$(CC) $(EMBED_CFLAGS) -c examples/embed/x86_64_setjmp.c -o build/embed/x86_64_setjmp.o
	$(CC) -I./include $(EMBED_CFLAGS) -c src/runtime/python_code_to_c_runtime.c -o build/embed/runtime.o
	$(CC) -I./include $(EMBED_CFLAGS) -c src/common/python_code_to_c_common.c -o build/embed/common.o
	$(CC) -I./include $(EMBED_CFLAGS) -c src/platform/python_code_to_c_platform.c -o build/embed/platform.o
	$(CC) -I./include $(EMBED_CFLAGS) -c src/platform/python_code_to_c_embed.c -o build/embed/embed.o
	$(CC) build/embed/embed_driver.o build/embed/embed_boot.o build/embed/x86_64_setjmp.o build/embed/runtime.o build/embed/common.o build/embed/platform.o build/embed/embed.o build/embed/host_stack_bounds.o $(LDLIBS) -o build/embed/embed_boot
	$(PYTHON) examples/embed/embed_boot.py > build/embed/embed_boot.expected
	build/embed/embed_boot > build/embed/embed_boot.actual
	diff -u build/embed/embed_boot.expected build/embed/embed_boot.actual

# ベアメタル構成（自作libcスタブ + libcなし）での例外機構とスタックスキャンの回帰。
# 以前は longjmp スタブが無限ループし、非Linuxではスタック境界が未登録のため
# 自動GCが安全側停止していた。プラットフォームのアロケータが実際に使われることも確認する。
test-baremetal-exceptions:
	@mkdir -p build/tests
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) tests/test_baremetal_exceptions.c examples/baremetal/python_code_to_c_baremetal.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c $(LDLIBS) -o build/tests/test_baremetal_exceptions
	build/tests/test_baremetal_exceptions

# 保守的スタックスキャンが「使用中のスタック範囲」だけを走査することの回帰。
# 以前は宣言区間（Linuxでは既定8MiB）全体を毎回走査し、しきい値を小さくすると
# 「収集回数 × スタック長」で劣化していた。
# ASanはローカル変数をfake stack（実スタック外）へ置くため、保守的スキャンから
# 見えなくなる。走査範囲の意味を保つためdetect_stack_use_after_return=0を付ける。
test-gc-stack-scan-scope:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_gc_stack_scan_scope.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDFLAGS) -pthread $(LDLIBS) -o $(BUILD)/tests/test_gc_stack_scan_scope
	ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(BUILD)/tests/test_gc_stack_scan_scope

# 式評価中のTLS一時値（binopの左オペランド、処理中の例外、f-stringビルダ）が
# GCのルートに入っていることと、式の途中で例外が脱出したときに深さを巻き戻す
# ことの回帰。生成Cのtryは開始時の深さを保存し、例外ハンドラへ到達した時点で
# p2c_binop_rewind()/p2c_fstr_rewind() で元の深さへ戻す。以前はこれが無く、
#   - 右オペランドの評価中の自動GCが左オペランドを回収し（解放済みポインタを
#     演算関数へ渡す）、
#   - 例外が脱出するたびに一時値が残って64回でnesting limitを誤発火し、
#     f-stringビルダがリークしていた。
# 2つ目（_lsan）はLeakSanitizerでf-stringビルダの解放も確認する。
test-gc-temp-roots:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_gc_temp_roots.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDFLAGS) -pthread $(LDLIBS) -o $(BUILD)/tests/test_gc_temp_roots
	ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(BUILD)/tests/test_gc_temp_roots
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZER_CFLAGS) tests/test_gc_temp_roots.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(SANITIZER_CFLAGS) -pthread $(LDLIBS) -o $(BUILD)/tests/test_gc_temp_roots_lsan
	ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(BUILD)/tests/test_gc_temp_roots_lsan

test-baremetal-build:
	$(MAKE) -f templates/toolchains/baremetal-example.mk PROJECT_ROOT=. CC="$(CC)" AR="$(AR)" clean all
	test -s build/baremetal/libpython-code-to-c-baremetal-example.a

test-baremetal-generated: all
	@mkdir -p build/baremetal
	./build/python-code-to-c examples/baremetal/baremetal_hello.py -o build/baremetal/baremetal_hello.generated.c
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) -c build/baremetal/baremetal_hello.generated.c -o build/baremetal/baremetal_hello.generated.o

install: all
	install -Dm755 $(BUILD)/python-code-to-c $(DESTDIR)/usr/local/bin/python-code-to-c

clean:
	rm -rf $(BUILD) $(OBJ)

-include $(DEPS)
