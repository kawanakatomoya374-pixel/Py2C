# Python Code to C Alpha0.6 — try/finally脱出の意味論修正と厳格警告基線

## 修正した意味論

`try`本体・`except`節・`else`節から`return`/`break`/`continue`で脱出するとき、生成Cは`finally`本体を実行せずに脱出していたため、CPythonと異なる出力を無言で生成していた。

| ケース | 修正前 | 修正後 |
|---|---|---|
| `try: return 1` + `finally: print("cleanup")` | `1` | `cleanup` → `1` |
| ループ内`try`で`x == 2`のとき`break` + `finally: print("cleanup", x)` | `cleanup 1` | `cleanup 1` → `cleanup 2` |
| `except`ハンドラ内`return` + `finally` | `finally`が消える | `finally`を実行してから戻る |
| 戻り値式の副作用（`return f()` + `finally`） | 順序不定 | 戻り値評価 → `finally` → `return` |

脱出時に例外フレームが関数を抜けたスタックを指したままになる問題も同時に解消した。`with`本体の`return`/`break`/`continue`は従来どおり明示診断である。

## 内部設計

コード生成中のCスタックへクリーンアップフレーム（`try_id`、`finally`本体、ループ入れ子数）をLIFOで連結し、脱出時に内側から外側へ

1. `finally`本体
2. `p2c_exc_stack = _p2c_ef_<try_id>.prev;`による例外フレームの復元

を生成する。`return`は全てのフレームを、`break`/`continue`は脱出先のループより内側のフレームだけを処理する。`finally`本体の生成中は自分自身のフレームを外すため、入れ子`try/finally`は外側のみを再実行する。入れ子関数・メソッド本体・closure entryは必ずフレームとループ深さをリセットし、外側の`try`へ脱出しない。`finally`内の`return`/`break`/`continue`は保留中の制御を上書きする順序の曖昧さを残さないよう、引き続き明示診断する。

## 厳格警告基線

`WARN_CFLAGS`へ`-Wold-style-definition`、`-Wredundant-decls`、`-Wundef`、`-Wconversion`、`-Wsign-conversion`、`-Wcast-qual`、`-Wwrite-strings`、`-Wdouble-promotion`、`-Wvla`、`-Wfloat-equal`を追加し、`-Werror`のまま0警告で通した。`-Wswitch-enum`はASTノード種別の`switch`が`default:`節で未対応種別を診断する設計であり、`-Wnull-dereference`は単一ヘッダー構成で関数間解析が効くため未チェック確保と誤検出が混在した約300件を報告するため、除外理由を`Makefile`へ明記した。

あわせて、追加引数を持たないメソッドの`__kwadapter`と、外側変数を読まない入れ子関数・lambdaのclosure entryが、参照しないパラメータの`(void)`キャストを欠いていたため、生成Cが`-Wunused-parameter`でコンパイルできなかった既存不具合を修正した（変更前のビルドでも再現する）。

## 検証

`tests/finally_unwind_alpha06.py`をCPython差分コーパスへC384–C402として登録し、戻り値評価順、`break`/`continue`とfor/whileの`else`、`except`ハンドラ内`return`、例外伝播、入れ子`def`、メソッド、モジュールレベルループを比較した。`make test`のスモーク、GC・GUI、整数境界、async/generator、ベアメタル、単一ヘッダー（Hosted・strict C11・freestanding）、コンフォーマンス402件、監査回帰、ファジング、移植性ビルドはGCCで通した。freestandingコアとベアメタル例も追加フラグ付きで0警告でビルドする。

## 既知の境界

suspension（`async def`・generator）経路は`try`文をstep関数へlowerする際に`NotImplementedError: suspension inside this statement is not yet supported`を実行時に送出し、`try/finally`と`return`を併用した`async def`は現状出力を生成しない。同期関数・モジュールレベルの`try/finally`のみ対応済みである。多重継承、bytes/bytearray、複素数、複数for節のジェネレータ式、`finally`内の`return`/`break`/`continue`、ネストしたクラス定義は引き続き明示診断または未対応である。
