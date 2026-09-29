# Python Code to C Alpha0.6 — 自作OS組み込みと追加構文 (2026-09-27)

本変更は「自作OSへ最大限組み込みやすくする」「対応構文を増やす」「品質を上げる」の
3点を同時に満たすためのまとまった改修である。すべてCPython差分、警告即エラー、
ASan/UBSan、単一ヘッダー再生成の品質ゲートを通している。

## 0. ベアメタル移植性の重大修正（GC・共有ヒープ・例外）

自作OS移植で実際に破綻する4件を修正した。いずれも再現テストを追加している。

| # | 症状（修正前） | 原因 | 修正 |
|---|---|---|---|
| 1 | GCが極端に遅い。`p2c_gc_set_threshold(1)`のテストがタイムアウトする | 保守的スタックスキャンが「宣言区間の全体」（Linuxのmainスレッドでは既定8MiB）を毎回走査し、収集回数×スタック長で劣化 | 走査を**現在のフレームから宣言上端まで**に限定（未使用領域は走査しない）。スタック語数ぶんの索引引きも、GCオブジェクトのアドレス範囲外を先に弾く。`p2c_gc_last_stack_words()`を追加 |
| 2 | 非Linux（ベアメタル）で自動GCが一度も走らない、または生存ローカルが回収される | `p2c_gc_init()`の非Linux分岐が境界を`NULL`にしてスキャン不能＝安全側停止にしていた | `p2c_gc_init(stack_hint)`の「スタック上のアドレス」を上端として扱い、それだけでスキャンを有効化（`P2C_GC_ENTER_MAIN()`／`P2C_GC_ENTER_TASK()`経路）。正確な区間は`p2c_gc_set_stack_bounds()`で宣言 |
| 3 | freestandingで`raise`するとハングする | 組込みスタブの`longjmp`が無限ループ（`while (1) {}`）だった | freestanding既定で`__builtin_setjmp`/`__builtin_longjmp`を使う本物の非局所脱出に変更。`PYTHON_CODE_TO_C_NO_LIBC_STUBS`／`PYTHON_CODE_TO_C_NO_COMPILER_SETJMP`でカーネル実装へ差し替え可。実装が無い場合は無限ループせず`p2c_platform_abort()`で診断 |
| 4 | プラットフォームの`alloc/realloc/free`が呼ばれない（計測で0回） | ランタイムがlibc/自作スタブの`malloc`を直接使い、`P2C_Platform`のアロケータを無視していた | 共通層の共有ヒープ抽象`p2c_heap_alloc`／`p2c_heap_calloc`／`p2c_heap_realloc`／`p2c_heap_free`へ一本化。プラットフォーム登録時は**変換器コア（文字列ビルダ・AST・生成C文字列）とランタイムの両方**がそのヒープを使う |

> 4の修正過程で、プラットフォームアロケータと`p2c_runtime_init(heap, size)`の
> 線形ヒープを**同じ領域**に向けると、独立した2つのバンプポインタが同じアドレスを
> 配ってヒープが壊れることをASanで再現した。共有ヒープへ一本化して解消し、
> 契約として§4.1（`PORTING.md`）に明記している。

さらに、既定アロケータを持たないカーネル構成のフォールバック（64KiB静的バッファ）が
「1回使ったら同じプロセスで2回目の変換ができない」状態だったのを、呼び出しごとに
解放して再利用できるようにした（再入のみ拒否）。

追加ゲート:

- `make test-gc-stack-scan-scope` — 走査範囲がスタック全長に比例しないことと、ローカルのみ到達可能なオブジェクトの生存。
- `make test-baremetal-exceptions` — 自作libcスタブ構成で`raise`/`except`が機能し、スタック上の生存ローカルが保護され、プラットフォームアロケータが実際に使われること。

## 0.1 アロケータ注入と `P2C_SETJMP`/`P2C_LONGJMP` 差し替え点（追補）

自作OSで変換器を「常駐サービス」として使う場合に必要な、ヒープと非局所脱出の
差し替え点を API として明示した。

| # | 追加 | 内容 |
|---|---|---|
| 1 | `p2c_platform_set_allocator(alloc, realloc, free, user)` | カーネルの kmalloc/krealloc/kfree だけを1回で注入する。`P2C_Platform` 一式（`write`/`clock_ms`）は既定実装を継承する。これ1回でランタイムと変換器コアの内部確保が「1つのOSヒープ」になる |
| 2 | `p2c_set_default_allocator(P2C_Allocator*)` | arena/slab を変換器コアの既定アロケータとして明示注入する（`NULL`で解除） |
| 3 | `p2c_heap_usable()` / `p2c_heap_note_stub_ready()` | 共有ヒープが実際に確保できるかの判定。NO_STDLIB既定で`p2c_runtime_init()`前は`false` |
| 4 | `P2C_SETJMP` / `P2C_LONGJMP` | ランタイム本体と生成Cの例外機構が通る唯一の2マクロ。定義すればカーネル実装（コンテキストスイッチ等）へ完全に差し替えられる |
| 5 | `P2C_COMPILER_FALLBACK_HEAP_SIZE` | 変換器コアの静的フォールバック（既定64KiB）のサイズ。`0`で無効化（未注入構成は`P2C_ERR_NOMEM`と注入方法を返す） |
| 6 | `p2c_core_static_allocator_active()` | 直近の変換が静的フォールバックを使ったかどうかの診断。常駐サービスでは`false`が期待値 |
| 7 | 共有ヒープの耐久性 | プラットフォーム解除後に残ったブロック（magic付き）はブロック先頭から解放する。magic判定は「プラットフォームブロックが1つでも生きている間」だけ行うため、純粋なlibcブロックの手前を読まない |

静的フォールバックの判定は「変換開始時にアロケータへ8バイトのプローブを1回」に統一した。
注入アロケータ／共有ヒープが確保できる限り静的バッファは一切使われない。

追加ゲート:

- `make test-allocator-injection` — 注入が変換器コア・共有ヒープの両方へ効くこと、注入失敗時のみ静的フォールバックへ落ちること、停止順序でヒープが壊れないこと。
- `make test-setjmp-hook` — ランタイム本体と生成Cが `P2C_SETJMP`/`P2C_LONGJMP` だけを通ること（`tests/setjmp_hook_override.h` を `-include` してフック呼び出し回数で検証）。
- `make test-heap-unification` — NO_STDLIB 構成で、ランタイム同梱 libc スタブの `malloc/calloc/realloc/free` がカーネルアロケータ（共有ヒープ）へ委譲され、別ヒープを確保しないこと。`p2c_heap_alloc()` のブロックと raw malloc のブロックが相互に解放できること。プラットフォーム解除後はスタブの線形ヒープが唯一のヒープとなり、`realloc` がブロックヘッダの旧サイズだけをコピーすること。ヒープ未設定では確保が NULL を返し、暗黙の静的ヒープを作らないこと。

## 0.2 式評価中の一時値と例外脱出（GCH-010）

自作OSの有無に関わらず破綻するGCの穴を1件修正した。生成Cは二項演算を
`(p2c_binop_begin(left), p2c_binop_finish(p2c_obj_add, right))` の形で出すため、
`left` は `p2c_binop_finish()` が取り出すまでTLSのbinopスタックにしか無く、
`right` の評価中に自動収集が走ると回収され、解放済みポインタが演算関数へ渡っていた。
処理中の例外（`p2c_active_exception`）も同じ期間があり、f-stringのビルダは
ネイティブ確保のままTLSに置かれていた。

| # | 症状（修正前） | 原因 | 修正 |
|---|---|---|---|
| 1 | GCしきい値を小さくすると `[1, 2] + boom()` が静かなuse-after-freeになる | 二項演算の左オペランドと処理中の例外がTLSにしか無い期間、保守的スタックスキャンから見えない | マークフェーズでTLS一時値を明示的にルート化（`gc_mark_tls_temporaries()`）。個数は `p2c_gc_last_temp_roots()` で観測できる |
| 2 | `try` の反復で65回目に「binary expression nesting limit exceeded」(RuntimeError)、f-stringは33回目で同様になり、`except ValueError` を素通りして異常終了する | 式の途中で例外が脱出する（longjmpする）と `g_binop_depth`/`g_fstr_depth` が戻らず、f-stringビルダは解放されずリークしていた | 生成Cの `try` が開始時の `p2c_binop_depth()`/`p2c_fstr_depth()` を保存し、例外ハンドラ到達時と未処理の再送出直前に `p2c_binop_rewind()`/`p2c_fstr_rewind()` で戻す。`p2c_runtime_shutdown()` も両方を空にする |

> カーネルが `P2C_LONGJMP` を直接使って脱出する場合は、例外フレームへ到達した
> 時点で保存した深さへ戻す契約になる（`docs/spec/HOBBY_OS_EMBEDDING_ALPHA0.6.md` §4）。

追加ゲート:

- `make test-gc-temp-roots` — 式の途中の自動GCで左オペランドが壊れないこと、処理中の例外が回収されないこと、200回例外が脱出しても深さが漏れずその後の式が正しいこと、停止後に深さが0へ戻ること（LeakSanitizer付きのリンクを含む）。
- `tests/audit_regression.sh` の GCH-010 ケース — `tests/gc_temp_roots_alpha06.py` がCPythonと同一出力になること。

検証（欠陥注入）:

- マークフェーズのTLSルート化（`gc_mark_tls_temporaries()` の呼び出し）を外すと
  `make test-gc-temp-roots` が失敗する（`p2c_gc_last_temp_roots()` が0になり、
  左オペランドの検算も通らない）。`p2c_binop_rewind()`/`p2c_fstr_rewind()` を
  無効化すると、同テストに加えて `tests/gc_temp_roots_alpha06.py` の生成C実行が
  `RuntimeError: f-string nesting limit exceeded` で失敗する。修正が実際に
  欠陥を塞いでいることはこの2方向の注入で確認した。

サニタイザゲートの修正（GCH-010の検証中に見つかった既存不具合）:

- `make test-sanitizers` が `src/tools/python_code_to_c_httpd.c` の
  `strncpy(dst, src, sizeof(dst) - 1)` で `-Werror=stringop-truncation` により
  失敗していた（ASan/UBSan併用の `-O2` でのみ出るGCCの誤検出）。長さを明示した
  有界コピー `copy_bounded()` に置き換え、切り詰めとNUL終端を1箇所にまとめた
  （変換器本体・生成Cの挙動は変わらない）。

## 1. 自作OS組み込みの強化

### 1.1 カーネル呼び出し可能なエントリ生成 `--embed-entry NAME`

- 生成物は `P2C_Object *NAME(void)` のみで、`int main()` を生成しない。
- 名前はC識別子のみ受理し、不正なら変換時に診断する（生成Cへ直接埋め込むため）。
- ランタイム初期化・GC初期化・shutdown はカーネルの責務になり、生成エントリは
  モジュールグローバルのルート登録と本体実行だけを行う。
- APIからは `P2C_TranspileOptions.embed_entry` で指定できる。
- 回帰: `make test-embed-generated`（カーネル相当ドライバで実行しCPythonと差分比較）、
  `make test-embed-compile`（不正名の拒否も検証）。

### 1.2 統合ファサード `p2c_embed`

`include/platform/python_code_to_c_embed.h` / `src/platform/python_code_to_c_embed.c`

| 追加 | 内容 |
|---|---|
| `P2C_EmbedHeap` | 境界タグ + アドレス順空きリスト + 隣接合体の組込みヒープ。libc非依存、16バイト整列、破損検出（`p2c_embed_heap_check`）、使用量/ピーク/失敗回数の統計 |
| `P2C_EmbedConfig` + `p2c_embed_start/run_program/stop` | シンク・時計・入力・ヒープ・スタック区間・OOM方針を一度に設定し、`P2C_Platform` 登録、`p2c_runtime_init`、GC初期化とスタック境界宣言まで行う |
| `P2C_EMBED_PROVIDE_LIBC_HEAP` | `malloc/calloc/realloc/free` を組込みヒープへ委譲。GCの解放が再利用へ繋がる |
| `P2C_EMBED_PROVIDE_PLATFORM_COMPAT` | `p2c_platform_write/write_n/read_line/abort/init/shutdown` も提供し、カーネルの追加ファイルを1つにする |
| 未処理例外の処理 | `p2c_embed_run_program()` が診断をシンクへ出してNULLを返す（カーネルを落とさない） |
| コンソール控え | 出力をUARTへ送りつつ固定バッファへ複製し、直前のログを検査できる |

実装中に見つけた不具合も同時に修正した。組込みヒープの「隣接空きと合体して拡張」
経路で `used` を二重に減算しており、`realloc` 拡張後に使用量がアンダーフローして
いた（テストが検出）。

### 1.3 GCの安全側設計とヒープ統計

- `p2c_gc_set_stack_bounds(lo, hi)` / `P2C_GC_ENTER_TASK(lo, hi)` / `p2c_gc_stack_scan_available()`。
- スタック境界が不明な間は、しきい値による自動収集と `p2c_gc_collect()` を
  **実行しない**（生存オブジェクトを誤って解放しないため）。一度だけ対処方法を診断する。
  従来は境界不明でも回収していたため、非Linuxのホストや非pthread構成では
  ローカル変数からのみ到達可能なオブジェクトを解放しうる潜在不具合があった。
- 非Linuxホスト向けに `-DP2C_GC_STACK_HINT_RANGE=<bytes>` を用意（明示オプトイン）。
- スタックスキャンは収集ごとに構築するアドレス索引で判定し、従来の線形探索より高速化。
- `p2c_runtime_heap_size/used/peak()`、`p2c_runtime_alloc_failures()`、`p2c_embed_stats()`。

### 1.4 確保失敗（OOM）の通知

- `p2c_runtime_set_oom_handler()` / `p2c_runtime_notify_oom()`。
- 標準ハンドラ `p2c_oom_raise_memory_error()` は、`runtime_init` で事前確保した
  `MemoryError` を使うため枯渇中に再確保しない。例外フレームが無ければシンクへ
  診断して `p2c_platform_abort()` へ進む。
- 既定（ハンドラ未設定）は従来どおりNULLを返すだけで、既存の「確保失敗時は
  NULLを返してロールバックする」契約は変えていない。
- 主要な確保経路（オブジェクト、文字列、list/tuple/dict/set成長、呼出しバッファ、
  sorted/reversed のコピー等、32箇所）を共通の通知経路へ通した。

### 1.5 カーネル提供アロケータとfreestanding例外

- `PYTHON_CODE_TO_C_NO_LIBC_STUBS` を定義すると、ランタイムは
  `malloc/realloc/calloc/free` と `setjmp/longjmp` を定義せずカーネル実装へ委譲する。

### 1.6 freestanding 既定アロケータの不具合修正（重要）

`p2c_default_allocator()` は `PYTHON_CODE_TO_C_NO_STDLIB` で **NULLを返していた**。
その結果、`p2c_alloc(NULL, ...)`（文字列ビルダ `p2c_str_new(NULL)`、f-string構築、
例外の文字列化、そして変換器コアの既定アロケータ）が未定義動作になり、最適化された
組込みビルドではGCCが到達不能と判断してトラップ命令を生成していた。実測では
`str(exc)` を含むNO_STDLIBプログラムが実行時にクラッシュしていた。

- 組込みでも既定アロケータが `malloc/free/realloc` へ委譲するようにした
  （実体はランタイムの線形ヒープ、カーネルのアロケータ、または `p2c_embed` のヒープ）。
- これにより **OS内でのPython→C変換（オンデバイス変換）** が実際に動作する。
  回帰 `make test-embed-compile` がカーネル相当環境で変換器コアを実行する。
- その後、プラットフォームアロケータを登録した場合は**共通層の共有ヒープ
  （`p2c_heap_*`）**へ一本化し、変換器コアとランタイムが同じヒープを使うようにした
  （§0-4）。既定アロケータが無い構成のフォールバック（64KiB静的バッファ）は
  呼び出しごとに解放され、同じプロセスで何度でも変換できる。

### 1.7 CRLF/CR ソースの無言の誤変換を修正（重要）

Windowsで保存したCRLFのPythonソースでは、行末の `\r` が未知トークンとして扱われ、
インデント計算とclass本体のメソッド検出が壊れていた。実測では
「classのメソッドがクラスに登録されず、実行時に `AttributeError: read`」という
無言の誤変換を起こしていた（LFソースでは正常）。

- 字句解析器が入力ソースの改行を正規化する（`\r\n` と孤立 `\r` を `\n` へ）。
  正規化が必要な場合だけ内部複製し、通常のLFソースは複製しない。
- 回帰: `make test-crlf`（LF/CRLF/CRで生成Cが完全一致すること、メソッドがクラス所属
  であることを検査）。

### 1.8 文字列エスケープの修正（重要）

- `\uXXXX` / `\UXXXXXXXX` は未対応で、`"caf\u00e9"` が `"cafu00e9"` に化けていた
  （黙って内容が変わる）。UTF-8エンコードして格納するようにした。
- `\xHH`、8進 `\ooo`、行継続 `\` + 改行 を実装し、未知のエスケープは
  CPythonと同じくバックスラッシュごと保持する。
- `\N{...}` はUnicode名前表を持たないため、無言の破損ではなく明示診断にした。
- f-string も同じ共通ヘルパで処理する（従来は同じ誤りが重複していた）。

### 1.9 生成Cの警告品質

- 内包表記の反復長 `size_t n = p2c_len(...)` に明示キャスト（`-Wsign-conversion`）。
- `except ... as NAME` の変数を `volatile` 宣言（setjmp/longjmp をまたぐ自動変数の
  C11 要件、`-Wclobbered` の指摘）。
- 式文を `(void)(...)` で評価し、`...` やリテラルだけの文が
  `-Wunused-value` にならないようにした。
- `callable(名前)` は静的に判定できる場合 `&P2C_True` へ畳み込み、関数ポインタや
  ビルトイン名を値として評価して壊れたCを生成しないようにした。

## 2. 対応構文・機能の追加

| 追加 | 内容 |
|---|---|
| `divmod(a, b)` | intはPythonのfloor除算/剰余、floatはCPythonのfloat_divmod補正つき。タプルを返す |
| `pow(base, exp, mod)` | モジュラべき乗。負の指数は法における逆元（存在しなければ `ValueError`）、法0は `ValueError`、結果の符号は法に従う。64ビット範囲を超える法は `OverflowError` |
| `format(value, spec)` | f-string/`str.format` と同じ書式エンジン。加えて `,`/`_` の桁区切りと `%` 型を実装 |
| `callable(obj)` | 関数・クラス・`__call__` を持つインスタンスを真とする |
| `str.isascii()` / `str.isprintable()` | ASCII近似（空文字列はTrue） |
| `...`（Ellipsis）と `Ellipsis` | 単一値。`def f(): ...` のスタブ本体、`is`/`==`、`repr`、`type()`、コンテナ要素として動作 |
| for文のタプル/starredターゲット | 意味解析が全ての単純名を束縛するよう修正（`for k, v in ...` のループ本体で参照すると「undefined name」で誤って失敗していた） |
| ネストしたクラス定義 | `class Outer: class Inner:`。生成CではC名を外側クラス名で前置して衝突を避け（`Outer__Inner`）、外側クラスの`__classobj()`が**上から下への評価順**で属性として登録する。外部からは`Outer.Inner`で参照・構築でき、クラス本体の式からはその名前で直接参照できる。同じ単純名を持つ別々の外側クラス、二段以上のネスト、ネストクラス版`__str__`、インスタンス経由の`self.Inner`、クラス本体の`alias = Inner`・`[First, Second]`も動作する。クラスオブジェクトは既存の`p2c_class_new`/`p2c_setattr`だけで作るため、新しいランタイムAPIもGCルートも追加していない（`NO_STDLIB`/freestandingでも同じコードが動く） |
| `isinstance(x, <class object>)` | 第2引数がbare nameでない場合（`Outer.Inner`のような属性経由や、変数に保持したclass object）も判定できるようにした。以前は変換自体は成功するが常に`False`を返していた（`p2c_isinstance_of_object`を呼ばない空白経路）。併せてクラスオブジェクトの同一性を先に見るようにし、名前ベース判定はフォールバックとして維持 |
| 関数本体内の`class`定義の診断 | 以前は受理されてCの入れ子関数定義を含む不正なCを生成していた（コンパイルエラーになるまで原因が分からない）。意味解析で明示診断する |
| Cライブラリ名と衝突する識別子 | `index`/`rindex`/`round`/`abs`/`pow`/`sqrt`/`log`/`main`等をパラメータ・ローカル・モジュール関数名に使うと、パラメータ宣言が`p2c_user_<name>`へマングルされる一方で**クラスメソッドの`(void)`キャストだけ生名**になっていた。前方宣言・パラメータ宣言・`(void)`キャストをすべて同じマングル名へ統一（`examples/baremetal/baremetal_hello.py`のネストクラス例が`'index' undeclared`として露呈した） |

これらは変換時に受理されるだけでなく、生成Cが必ずコンパイルできること
（`divmod` や3引数 `pow` は以前「受理されるが未定義のC関数呼び出しを生成する」
状態だった）を回帰で固定している。

## 3. テストとゲートの追加

| ゲート | 検証内容 |
|---|---|
| `make test-embed-runtime` | 組込みヒープ、ライフサイクル、タスク再起動、GC安全側停止と有効化、OOM通知、MemoryError化、panic経路 |
| `make test-freestanding-setjmp` | カーネル提供setjmp/longjmpでのraise/except、入れ子フレーム、深いフレームからのlongjmp |
| `make test-embed-compile` | カーネル相当環境での変換器コア実行（オンデバイス変換） |
| `make test-embed-generated` | `--embed-entry` 生成モジュールの実行とCPython差分 |
| `make test-baremetal-exceptions` | 自作libcスタブ構成（libcなし）での`raise`/`except`、スタック上の生存ローカル保護、プラットフォームアロケータの使用 |
| `make test-gc-stack-scan-scope` | 保守的スタックスキャンの走査範囲（`p2c_gc_last_stack_words()`）と生存ローカルの保護 |
| `make test-hobby-os-template` | 組み込みテンプレートのコンパイル（警告即エラー） |
| `make test-crlf` | 改行コード差で生成Cが完全一致すること |
| CPython差分 C403–C460 | `divmod`/`pow(...,mod)`/`format`/`callable`/文字列判定、Ellipsis、文字列エスケープ（計58 assertion） |
| CPython差分 C461–C479 | ネストしたクラス定義：`Outer.Inner`経由の構築・メソッド呼出し、クラス本体での名前参照と上から下への評価順、ネストクラスの継承、`isinstance(x, Outer.Inner)`、同名ネストクラスの衝突回避、二段ネスト（計19 assertion） |
| CPython差分 C480–C486 | Cのライブラリ名と衝突する識別子（`index`/`round`/`abs`/`pow`/`sqrt`/`log`/`main`）をパラメータ・ローカル・モジュール関数名に使う回帰（計7 assertion） |
| `make test-decorator-diagnostics`（拡張） | 関数本体内の`class`定義と、メソッド本体からのネストクラス名参照が明示診断になり、黙って壊れたCを出さないこと |
| `make test-baremetal-generated` / `make test-embed-generated`（拡張） | 組込み例（`examples/baremetal/baremetal_hello.py`、`examples/embed/embed_boot.py`）にネストしたクラス定義を含め、freestandingコンパイルとカーネル相当ドライバ実行＋CPython差分で確認 |

## 4. 追加されたファイル

| ファイル | 内容 |
|---|---|
| `include/platform/python_code_to_c_embed.h` / `src/platform/python_code_to_c_embed.c` | 統合ファサード |
| `examples/embed/{embed_boot.py,embed_driver.c,host_stack_bounds.c,host_stack_bounds.h,x86_64_setjmp.c}` | 実行例（カーネル相当ドライバ、スタック境界、setjmp参考実装） |
| `templates/hobby_os/{README.md,hobby_os.mk,hobby_os_api.h,embed/hobby_os_embed.c,platform/python_code_to_c_platform_user.c}` | 組み込みテンプレート（推奨構成とフルコントロール構成） |
| `tests/{test_embed_runner.c,test_freestanding_setjmp.c,test_embed_transpile.c}` | 組込み回帰 |
| `tests/{test_baremetal_exceptions.c,test_gc_stack_scan_scope.c}` | ベアメタル例外・共有ヒープ・スタックスキャン範囲の回帰 |
| `tests/{builtin_gap_alpha06.py,ellipsis_alpha06.py,string_escapes_alpha06.py,crlf_source_probe.py,crlf_source_regression.sh}` | 新規構文の回帰 |
| `tests/nested_class_alpha06.py` | ネストしたクラス定義のCPython差分回帰（C461–C479） |
| `tests/c_identifier_collision_alpha06.py` | Cライブラリ名と衝突する識別子のCPython差分回帰（C480–C486） |
| `tests/{class_in_function_rejection_alpha06.py,nested_class_method_visibility_rejection_alpha06.py}` | ネストしたクラス定義に伴う明示診断（関数本体内の`class`、メソッド本体からのクラススコープ参照） |
| `docs/spec/HOBBY_OS_EMBEDDING_ALPHA0.6.md` | 組み込みガイド |
| `tools/convert_alloc_sites.py` | 確保経路のOOM通知化に使った移行スクリプト（変更の追跡用） |

## 5. 既知の制限（変更なし）

- 文字列長・添字はUTF-8**バイト**単位（CPythonのコードポイント単位とは異なる）。
  非ASCII文字列の `len()` はCPythonと異なる値を返す。
- `\N{...}`（Unicode名前エスケープ）は名前表を持たないため明示診断。
- 埋め込まれたNULバイトを含む文字列リテラルの長さは保持しない（C文字列として扱う）。
- suspension（`async def`/generator）内の `try` 文、`finally` 内の `return`/`break`/
  `continue`、bytes/bytearray、複素数、多重継承、複数for節のgenerator expression は
  引き続き明示診断または非対応。
- ネストしたクラス定義は**モジュール直下とクラス本体の直下**のみ（関数本体の中の
  `class` 定義はCの入れ子関数定義になり得ないため明示診断）。Python仕様どおり
  クラススコープの名前はメソッド本体から見えないため、その位置での参照も明示診断する
  （`Outer.Inner` を使う）。
- **基底クラスが持つクラス属性は、サブクラスのインスタンスからは見えない**
  （`class Base: kind = "base"` のとき `Derived("x").kind` は `AttributeError`。
  CPythonは `"base"` を返す）。継承で引き継がれるのはメソッドと `__init__` であり、
  これは本変更前からの既知の制限（ネストしたクラス定義の継承でも同じ）。
- クラスオブジェクトの `__name__` は取得できない（`type(obj).__name__` は
  `AttributeError`）。`type(obj)` はクラスオブジェクトを返すが、その `__name__`
  属性は未実装。


- `examples/embed/x86_64_setjmp.c` に、`void *buf[16]` の `jmp_buf` 契約に合わせた
  x86-64 の setjmp/longjmp 参考実装を追加（カーネルはこれを置き換える）。

## 8. 追加構文・C3線形化MRO・厳格ビルド (2026-09-28)

「対応構文をさらに増やす」「全コードの品質を上げる」「ビルドオプションをさらに
厳しくする」を同時に進めた第2ラウンド。CPython差分は486→**549アサーション**へ増え、
すべて一致している。

### 8-1. 追加した構文と意味論

| 機能 | 以前 | 現在 |
|---|---|---|
| 多重継承 `class D(B, C)` | 受理するが、基底を左から深さ優先で辿る近似のため**ダイヤモンド継承でCPythonと違うメソッドを選ぶ**（静かな誤動作） | Pythonと同じ**C3線形化**でMROを計算しキャッシュ（`v_class.mro`）。`isinstance`・例外捕捉もMROで判定 |
| 基底クラスのクラス属性 | サブクラスのインスタンスから見えない（`AttributeError`） | MRO順に探索し、Python同様見える。クラスオブジェクト経由でも同様 |
| メソッドを値として取り出す `m = obj.method` | `AttributeError`（呼び出し形 `obj.method()` のみ対応） | **束縛メソッド**（環境辞書 `self`+名前を持つクロージャ）を返す。`sorted(key=...)`・`map()`・コールバックへ渡せる。`hasattr(obj, "method")` も真 |
| `finally`内の`return`/`break`/`continue` | 意味解析エラー | 受理。`finally`本体を実行し、保留中の例外・`return`を上書きする（クリーンアップフレームに`in_finalbody`を持たせ、実行中のfinallyを再実行せず例外フレームだけ復元） |
| ジェネレータ式の複数for節・タプルターゲット | 1節・単純名のみ（診断エラー） | 任意段（〜8節）の`for`節、タプル/リストのアンパック、`if`フィルタ。最も外側のiterableだけを生成時に評価するPythonの規則にも従う（レベル変数＋switchの状態機械） |
| ジェネレータ式内の式 | 単純な式のみ（組込み呼び出しやタプル表示は未宣言のCを出力） | `cg->genexpr_locals` を介して `gen_expr` の完全なディスパッチ（`len`/`range`/`sum`/`sorted`/タプル・辞書表示/添字/f-string）をそのまま利用可能 |

### 8-2. 品質修正（静かな誤動作の除去）

| 症状（修正前） | 原因 | 修正 |
|---|---|---|
| `sorted([(2, 1), (1, 1)])` が**未ソートのまま**返る | タプルを整数0として比較 | タプル/リストの辞書式比較を実装。順序を持たない型同士はCPython同様`TypeError` |
| 大きな`sorted()`がO(n²) | 挿入ソート | 安定なマージソート（O(n log n)）へ置換。`reverse=`/`key=`の安定性は維持 |
| `map(f, xs)`/`filter(f, xs)` にクロージャや束縛メソッドを渡すと**黙って空/無フィルタ** | `v_function.func` を直接呼び、`NULL`ならスキップ | 必ず `p2c_call` 経由（関数・クロージャ・束縛メソッド・クラスを同じ規則で扱う） |
| 2段以上先の基底例外を`except`で捕捉できない | `p2c_exc_name_match` が直接の基底名しか見ない | MRO上の名前と比較 |
| `type(クラス)` が `<class 'object'>` | OBJ_CLASS/OBJ_MODULE が既定値 | `<class 'type'>`/`<class 'module'>` |
| `super()`がダイヤモンド継承で静的な基底を選ぶ | コード生成時に基底チェーンを辿っていた | 実行時にインスタンスの型のMROで「メソッドを書いたクラスの次」から解決（`p2c_super_call_attr`） |
| MRO計算が67KiBのスタックを使う | 1フレームに64×2個の作業配列 | 上限を明示（名前32・基底8・深さ12・アリーナ3072）し、`merged`コピーを廃止 → 約3.4KiB |
| 例外オブジェクト生成の失敗時にNULL参照（`-fanalyzer`検出） | `p2c_raise(NULL)` の経路 | OOM時は診断を出して安全に停止 |

### 8-3. ビルドの厳格化

- `WARN_CFLAGS` を大幅に拡張（`-Wcast-align=strict`, `-Wlogical-op`,
  `-Wduplicated-cond/-branches`, `-Wstrict-overflow=2`, `-Wformat-overflow=2`,
  `-Wformat-truncation=2`, `-Wformat-signedness`, `-Wstringop-overflow=4`,
  `-Wstringop-truncation`, `-Warray-bounds=2`, `-Wuse-after-free=3`,
  `-Wjump-misses-init`, `-Wnested-externs`, `-Wmissing-declarations`,
  `-Wswitch-default`, `-Wimplicit-fallthrough=5`, `-Wunused-macros`,
  `-Warith-conversion`, `-Wcast-function-type`, `-Wmultistatement-macros`,
  `-Wsizeof-pointer-memaccess`, `-Wsizeof-array-argument`,
  `-Wmissing-parameter-type`, `-Wcalloc-transposed-args`, `-Wpointer-arith`,
  `-Wbad-function-cast`, `-Wrestrict`, `-Wshift-overflow=2`）。追加分で出た
  28件の警告はすべて修正（snprintfの切り詰め検査は長さを明示計算、
  アラインメントキャストは`void*`経由、`%.*f`の精度を上限化、他）。
- 意図的に無効化するフラグと理由を `Makefile` に明記（`-Wswitch-enum`,
  `-Wdeclaration-after-statement`, `-Wc++-compat`, `-Wnull-dereference`）。
- ホスト向けに実行時ハードニング `HOSTED_HARDEN`
  （`-fstack-protector-strong`, `-fstack-clash-protection`,
  `-D_FORTIFY_SOURCE=3`）を既定適用。組込み構成では分離。
- 新ターゲット:
  - `make test-stack-usage` … freestanding全体のフレーム上限（既定4096バイト）を
    `-Wstack-usage` + `-Werror` で検査。`make test` に含める。
  - `make test-analyzer` … GCC `-fanalyzer` による欠陥検査（長時間のため任意実行）。
- `--supported` の機能一覧は、1文字列リテラル4095バイトの上限（ISO C99が保証する
  最低値、`-Woverlength-strings`）を超えないよう3リテラルへ分割し、容量確認つきで
  連結する方式へ変更。表示内容も今回の追加機能に合わせて更新。

### 8-4. テスト

新規フィクスチャとCPython差分ケース:

| ケース | フィクスチャ | 内容 |
|---|---|---|
| C487-C493 | `tests/multiple_inheritance_alpha06.py` | ダイヤモンド継承のC3順序、`isinstance`、基底クラス属性、基底`__init__`の継承 |
| C494-C499 | `tests/bound_method_alpha06.py` | 束縛メソッドの取り出し・再利用・`map`/`filter`への受け渡し |
| C500-C505 | `tests/finally_control_flow_alpha06.py` | `finally`内`return`/`break`/`continue`、入れ子、例外の上書き |
| C506-C515 | `tests/generator_expression_multi_alpha06.py` | 複数for節、タプルターゲット、フィルタ、遅延評価 |
| C516-C525 | `tests/tuple_ordering_alpha06.py` | タプル/リストの比較、安定性、`key=`、`reverse=` |
| C526-C528 | `tests/super_mro_alpha06.py` | `super()`のMRO解決（ダイヤモンド継承で兄弟基底が選ばれること、`super().__init__`） |

## 9. C99/TinyCC対応・ELF生成・メソッドデコレータ (2026-09-30)

`docs/testing/STRICT_BUILD_AND_ANALYSIS_ALPHA0.6.md` に、警告基準・除外理由・
ハードニング・解析・スタック上限の一覧をまとめた（第2ラウンド分）。

第3ラウンド。**対応構文の追加**（メソッドデコレータ、math拡充）、**C99（TinyCC）でも
ビルド可能**、**`make elf` / `make hobbyos elf`** の2種類のELF生成、**品質向上**
（float表示のCPython一致、`--supported`の切り詰めバグなど）を行った。
CPython差分は528→**549アサーション**へ増え、既定の厳格ビルド（C11＋厳格警告）は不変。

### 9-1. 追加した構文

| 機能 | 以前 | 現在 |
|---|---|---|
| `@staticmethod` / `@classmethod` / `@property` | 「decorators are currently supported only on module-level functions」で拒否 | メソッド種別（`P2C_METHOD_*`）として受理。runtimeが種別に従い、staticはself無し・classはクラスオブジェクト・propertyは属性読み出しでゲッターを呼ぶ |
| propertyの意味論 | — | インスタンス属性より優先（data descriptor）、MRO順に継承・オーバーライド、`hasattr`が真、setter未実装への代入は`AttributeError` |
| `import math` | `pi`/`e`/`sqrt`/`sin`/`cos`/`pow` のみ | `tau`/`inf`/`nan`、`floor`/`ceil`/`trunc`/`fabs`/`fmod`/`hypot`/`copysign`/`ldexp`/`degrees`/`radians`/`tan`/`asin`/`acos`/`atan`/`atan2`/`exp`/`expm1`/`log`(1〜2引数)/`log2`/`log10`/`log1p`/`cbrt`/`isnan`/`isinf`/`isfinite`/`fsum`/`prod`/`factorial`/`gcd`/`isqrt`/`comb`/`perm`/`erf`/`erfc`/`gamma`/`lgamma` を追加（整数系はオーバーフロー検査付き） |

### 9-2. C99 / TinyCC 対応

- コアを **C99で妥当** な範囲に保ち、`P2C_THREAD_LOCAL` などの移植マクロでC11差分を吸収。
  `__builtin_setjmp`/`__builtin_longjmp` と `no_sanitize` 属性は GCC/Clang 限定とし、
  **tcc（`__TINYC__`）では libc の setjmp/longjmp またはカーネル実装を使う**。
- freestanding構成で使うlibmの宣言が不足していたため、`common.h` の NO_STDLIB 節へ
  C99数学関数のプロトタイプを追加（`isnan`/`isinf`/`isfinite` はマクロ依存をやめ、
  値の性質から直接判定する実装へ変更）。
- 新ターゲット: `make c99`（`-std=c99 -pedantic -Wall -Wextra -Werror`）、
  `make test-c99`（C99で変換器と生成Cをビルドして差分コーパスを実行）、
  `make tcc` / `make test-tcc`（TinyCC。`check-tcc`が無い場合に理由を表示）。
  既定の `WARN_CFLAGS`（C11の厳格基準）は**変更していない**。

### 9-3. ELF生成

| ターゲット | 生成物 | 内容 |
|---|---|---|
| `make elf` | `build/elf/python-code-to-c` | 完全版（ホスト機能込み）を**静的リンク**した自己完結ELF。`INTERP`が無いこと・実行できることを自動検査し、`file`/`readelf`のマニフェストを残す |
| `make hobbyos elf`（= `hobbyos-elf`） | `build/hobbyos/python-code-to-c-hobbyos.elf` | HobbyOS向けELF。`p2c_hobbyos_entry()` をエントリ、`.text`(R+X)/`.rodata`(R)/`.data`+`.bss`(R+W) の**W^X 3セグメント**、`-nostdlib`。`INTERP`が無く**未定義シンボルも無い**（`nm -u`）ことを自動検査 |
| `make test-hobbyos-libc` | `build/tests/test_hobbyos_libc` | HobbyOS向けELFが要求する**参照libm/libc**（`templates/hobby_os/embed/hobby_os_libc.c`）を、ホストのlibm/libcと比較検証（シンボルは`stub_`前置で衝突回避） |

- `make hobbyos elf` は `MAKECMDGOALS` を見て `hobbyos-elf` へ委譲する（`make elf` は完全版）。
- 参照libcは libm 28関数 + `snprintf`/`vsnprintf`/`strtoll`/`strtod` を自前実装し、
  精度方針（組込み用途で実用的な範囲・境界はPythonの期待に合わせる）を明記。
- リンカスクリプト `templates/hobby_os/hobbyos.ld` を追加（`HOBBYOS_LOAD_ADDR`でロードアドレス変更可）。

### 9-4. 品質修正

| 症状（修正前） | 原因 | 修正 |
|---|---|---|
| `print(100.0)` が `1e+02` になる | `%g` 依存の最短表現選択 | CPythonのrepr規則（指数が-4未満/16以上で指数表記、それ以外は固定小数＋`.0`）で生成 |
| `1e308*10` が `inf.0` と表示される | inf判定が `v > 1e308*10`（最適化でinf同士の比較） | DBL_MAX との比較に変更 |
| `--supported` の後半（クラス／未対応一覧）が**黙って欠落** | 固定バッファ6000バイト < 合計約6.6KB | バッファを8192へ拡大し、容量確認付き追記の意図を明記 |
| freestandingで `isnan`/`isinf` が未宣言 | libcの判定マクロは freestanding で提供されない | ビットパターン判定へ置換（`p2c_double_is_nan`/`_is_inf`） |
| 生成Cのメソッド表で `-Wmissing-field-initializers` | `P2C_MethodDef` へ `kind` を追加したため既存の位置指定初期化子が不足 | pygame/runtimeの全メソッド表と生成Cの番兵へ `P2C_METHOD_INSTANCE` を明示 |
| エディタ経由で追加したファイルがCRLF | 作成ツールの既定 | リポジトリ内の該当11ファイルをLFへ正規化 |

### 9-5. テスト

| ケース | フィクスチャ | 内容 |
|---|---|---|
| C529-C538 | `tests/method_decorators_alpha06.py` | static/class/propertyの呼び出し形態、propertyのMRO継承とオーバーライド、代入時の`AttributeError`、束縛property、classmethodによるクラス属性更新 |
| C539-C549 | `tests/math_module_alpha06.py` | 追加したmath関数と定数のCPython一致 |

### 9-6. TinyCCでのフル機能ビルド検証（実測）

検証環境（Ubuntu 24.04 / GCC 14）にはtccが入っていなかったため、`apt-get download tcc`
＋ `dpkg-deb -x`（root不要）で `/tmp/tccroot` へ展開して **TinyCC 0.9.27 を実際に導入**し、
次を実行して確認した。

| 対象 | コマンド | 結果 |
|---|---|---|
| 変換器本体（CLI，フル機能） | `make tcc TCC=".../tcc -B ..."` | `tcc_build_ok`（`-std=c99 -Wall -Werror`、機能を削る `#ifdef` なし） |
| 生成C＋差分コーパス | `make test-tcc` | CPython差分コーパスを tcc でビルド・実行して一致 |
| 単一ヘッダー（hosted） | `make test-single-header-tcc` | ビルド＋実行（hosted機能がそのまま動く） |
| 単一ヘッダー（freestanding） | 同上 | `NO_STDLIB` でビルドでき、`nm -u` に hosted libc 参照が無い |
| 単一ヘッダー（GCC C99） | `make test-single-header-c99` | `-std=c99 -pedantic-errors -Wall -Wextra -Werror` でビルド＋実行 |

この検証で見つかって直した点:

| 症状 | 原因 | 修正 |
|---|---|---|
| `tcc: error: invalid option -- '-MMD'` | 依存関係生成がGCC前提 | `DEPFLAGS` を変数化し、tcc構成では空にする |
| `_GNU_SOURCE redefined`（tccは再定義をエラーにする） | ソース側の機能マクロが無条件定義 | `src/` と `tests/` の `_GNU_SOURCE`/`_POSIX_C_SOURCE`/`_DEFAULT_SOURCE` 定義を `#ifndef` で保護 |
| `incompatible redefinition of 'int64_t'`（NO_STDLIB + tcc） | tccの`stddef.h`も固定幅型を定義 | `__TINYC__` ではコンパイラ提供の型ヘッダを使う（`PYTHON_CODE_TO_C_USE_COMPILER_TYPES`）。`-nostdinc`環境向けに `PYTHON_CODE_TO_C_NO_COMPILER_HEADERS` も用意 |
| `CC="tcc -B ..."` のような複数語CCが渡せない | テストスクリプトが `"$CC"` と引用 | `$CC`（非引用）で単語分割して実行 |
| `test-sanitizers`/`test-analyzer` が `build/tests` 不在で失敗 | `make clean` 直後や外部要因で消失 | 各ターゲットと `conformance_regression.sh` の先頭で `mkdir -p build/tests` |

### 9-7. カーネル/組込みテストの前提修正

`make test-embed-runtime` が `p2c_embed_start()` 内で停止するようになった。原因は
**組込みモジュール（`math`）の登録で初期化時のメモリ使用量が増えた**ことで、テストが
使っていた 4 KB ヒープでは初期化が OOM になり、panic フック（`longjmp` 先未設定）が
復帰したため `p2c_embed` が停止ループ（カーネルでは halt 相当）へ入っていた。

- 実測用の `tests/measure_embed_baseline.c` と `make test-embed-baseline` を追加し、
  初期化直後の使用量が **約 17.7 KB** であることを確認
- 枯渇テストのヒープを 4 KB → **32 KB**（実測に余裕を持たせた値）へ変更
- `docs/spec/HOBBY_OS_EMBEDDING_ALPHA0.6.md` に「ヒープサイズの見積り（実測）」を追記
  （推奨 32 KB 以上、テンプレート既定 64 KB、HobbyOS向けELF 128 KB）

これにより `make test-embed-runtime` は再び即座に完了する。

### 9-8. 単一ヘッダーのC99/TinyCC検証

`make test-single-header-c99`（GCC `-std=c99 -pedantic-errors -Wall -Wextra -Werror`）と
`make test-single-header-tcc`（TinyCC hosted 実行＋freestanding コンパイル、`nm -u` で
hosted libc 参照なしを確認）を追加し、`make test` / `make test-tcc` に組み込んだ。
`make tcc` と `make c99` は `all gui`（CLI＋GUI）をビルドするようにし、TinyCCでも
通常ビルドと同じ機能が使えることを確認した。

`docs/build/C99_TINYC_AND_ELF_ALPHA0.6.md` にC99/TinyCCビルドとELF生成の使い方・設計・
参照libcの方針をまとめた。

## 10. フォールバック・適応GC・品質強化 (2026-09-30)

第4ラウンド。**未対応構文のフォールバック（`--fallback`）**、**適応GC**、**GC統計API**、
**silent-bug源の除去**、**単一ヘッダー再生成**、**`make help` 全コマンドの実行確認**を行った。
CPython差分は**549アサーション**を維持し、厳格ビルド（C11＋厳格警告）とTinyCCビルドの
どちらでもエラーが出ないことを確認している。

### 10-1. 未対応構文のフォールバック（`--fallback`）

これまで未対応構文は変換エラーで停止していた。`--fallback` を付けると、**未対応の
箇所を「実行時に `NotImplementedError` を送出するスタブ」へ置き換えて変換・ビルドを
続行**する。到達しない行なら通常どおり動くため、大きなコードの一部分だけが未対応でも
ビルドして試せる。

| 対象 | 挙動 |
| --- | --- |
| parser レベル（`bytes` リテラル、複素数リテラル、`\N{...}`） | スタブ式にして解析を継続（`P2C_UNSUPPORTED_NAME_PREFIX` の名前ノード経由） |
| codegen レベル（式・文の既定分岐） | `p2c_fallback_expr` / `p2c_fallback_stmt` を出力 |
| 到達したとき | `NotImplementedError: unsupported <種別> at line N (converted with --fallback)` |
| 到達しないとき | 何も起きない（そのまま実行できる） |

既定（`--fallback` なし）は従来どおり明確な診断で停止する。回帰は `make test-fallback`。

### 10-2. 適応GC（しきい値の自動調整）

固定しきい値（既定 256KiB）だけでは、生存オブジェクトが多いプログラムで
「収集してもほとんど解放されないのに毎回スタックスキャンする」状態になり、収集回数が
そのままコストになっていた。`p2c_gc_collect()` の最後で次を判定するようにした。

- 解放がごく少ない（`freed * 4 < live`）→ しきい値を倍々に伸ばす（上限 4MiB、`growths` を計数）
- よく解放できた（`freed > live / 2`）→ しきい値を基準値へ25%ずつ戻す
- `p2c_gc_set_threshold()` は基準値と現在値を同時に設定し、成長回数をリセット
- `p2c_gc_set_adaptive(false)` で従来どおりの固定しきい値に戻せる

回帰 `make test-gc-adaptive` は、同一のチャーン負荷（生存2万オブジェクト＋一時オブジェクト生成）
を固定しきい値と適応ありで実行して比較する。実測では **収集回数 96 → 9**（約1/10）、
追跡オブジェクト数のピークは上限内（約2.9万）に収まった。

### 10-3. GC統計API

`p2c_gc_stats(P2C_GcStats *)` を追加（`collections` / `objects` / `peak_objects` /
`last_freed` / `threshold` / `base_threshold` / `threshold_growths` / `scanned_words` /
`temp_roots`）。カーネルがメモリ見積りと収集頻度の診断に使える。あわせて
`p2c_gc_is_adaptive()` と追跡オブジェクト数のピーク計測を追加した。

### 10-4. silent-bug源の除去（品質）

| 症状（修正前） | 原因 | 修正 |
| --- | --- | --- |
| 式の既定分岐が `&P2C_None` を出力 | 未対応の式を黙って `None` にしていた | 変換エラーとして報告（`--fallback` ではスタブ式） |
| 文の既定分岐が `/* unimplemented stmt */` | 未対応の文を黙って無視していた | 変換エラーとして報告（`--fallback` ではスタブ文） |
| 既定オプションが位置指定初期化子 | フィールド追加で値がずれる（実際に`--fallback`追加時に発生） | `P2C_DEFAULT_TRANSPILER_OPTIONS` / `P2C_DEFAULT_OPTIONS` を**指定初期化子**へ |

### 10-5. ヘッダーと検証

- `include/python_code_to_c_single.h` を再生成（適応GC・GC統計API・フォールバックを反映）
- `make help` に載っている**全コマンドを実行**し、エラーが出たものは都度修正
  （前ラウンドの `build/tests` 消滅・`test-embed-runtime` 停止・デコレータ診断の
  期待値更新を含む）
- 追加ターゲット: `test-fallback`、`test-gc-adaptive`、`test-embed-baseline`、
  `test-single-header-c99` / `test-single-header-tcc`（前ラウンド）


