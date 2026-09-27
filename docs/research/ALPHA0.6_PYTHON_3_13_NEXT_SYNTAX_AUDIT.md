# Python Code to C Alpha0.6 — 次期Python 3.13構文監査

## 公式仕様から確認した事項

Python 3.13の完全文法は、function/class definitionに任意個のdecoratorを許可し、decoratorの構文を`'@' named_expression NEWLINE`として定義する。[1] 同じ文法は`except*`、`async for`を含むcomprehension、`async with`の複数item・star targetを定義している。[1]

公式の式仕様では、list/set/dict displayは明示要素列またはcomprehensionで構築される。comprehensionは少なくとも一つのfor節を持ち、left-to-rightの入れ子として評価される。[2] async def内のcomprehensionは`async for`と`await`を含められるが、停止可能なimplicit scopeになる。[2]

公式の複合文仕様では、`except*`はExceptionGroupをmatching/non-matching subgroupに分割し、各handler実行後に未処理部分と新規例外をmergeする。[3] これは専用の例外群object、split/merge、複数handler制御を必要とするため、既存の単一例外frameだけで安全に部分実装してはならない。[3]

## 優先候補

| 優先度 | 構文 | 判断 | 理由 |
|---|---|---|---|
| P0 | decorator | **module-level function/classで実装済み** | parser、AST、semantic、codegen、GC root、single-headerへ接続し、上から下への評価・下から上への適用をC366–C372で検証。nested/method/vararg/keyword-callは明示診断 |
| P1 | multi-context `async with` | 次の優先候補 | 通常withのnesting設計とasync with state machineを結合する必要があり、exception cleanup順の状態数が増える |
| P2 | async comprehension | 後続対象 | implicit scope、async iterator、await、複数停止点を同時に実装する必要がある |
| P3 | `except*` / ExceptionGroup | 設計先行 | subgroup split/mergeとtraceback意味論を持つruntimeが必要 |
| P4 | bytes / complex / ellipsis / `@` | 値モデル先行 | lexer/parser受理だけでは不十分で、object typeと演算意味論が必要 |

> decoratorはmodule-level function/classの安全な範囲で実装し、Clang/GCC full-build、ASan/UBSan、strict C11、freestanding、single-header、C01–C372のCPython差分を通過した。次の候補は、複数contextを持つ`async with`である。いずれの候補もparser受理だけでは完了とせず、AST、semantic、codegen、runtime/GC、strict diagnostics、CPython差分を一貫して実装できない場合は理由付きdiagnosticを維持する。

## References

[1]: https://docs.python.org/3.13/reference/grammar.html "Python 3.13 Full Grammar specification"
[2]: https://docs.python.org/3.13/reference/expressions.html#displays-for-lists-sets-and-dictionaries "Python 3.13 Language Reference — Displays for lists, sets and dictionaries"
[3]: https://docs.python.org/3.13/reference/compound_stmts.html#except-star "Python 3.13 Language Reference — The except* clause"

## 2026-08-22 追加監査: 複数context manager async with

Python 3.13のcompound statement grammarは`async_with_stmt`を独立したcompound statementとして定義し、`async with`のsuiteを持つ。[2] 非同期context managerは`__aenter__()`と`__aexit__()`をawaitするプロトコルであり、PEP 492はenter・exitの非同期呼出しをasync withの目的として定義している。[3] 複数itemは単一の文として受理できるが、実装上は外側から内側へenterし、body終了時は内側から外側へexitする入れ子相当のcleanup順を維持しなければならない。[2]

| 対象 | 今回の判断 | 安全境界 |
|---|---|---|
| `async with A() as a, B() as b:` | 次の実装対象 | Aのenter完了後にBをenterし、Bのexit完了後にAをexitする状態列へlowerする |
| `async with A(), B():` | 同時実装対象 | enter結果は破棄するが、各protocol awaitと例外cleanupは保持する |
| 複合`as` target | 複数itemの後段 | tuple/list/star targetは通常withのtarget binderを再利用し、未対応形は明示診断する |
| enter途中の例外 | 必須回帰 | 既に成功した内側/外側contextだけを正しい逆順でexitする必要がある |
| body例外 | 必須回帰 | 全てのexitへ例外情報を渡し、各exitのtruthy結果に応じて抑止または再送出する |
| exit途中の例外 | 必須回帰 | 新しいexit例外が元例外を置換するPython規則を状態機械で保持する |
| `@asynccontextmanager` | 後続対象 | async generator、yield境界、再利用可能なdecoratorを同時に要するため別設計とする。[4] |

現行Alpha0.6は単一context managerのasync withを実装済みであり、今回の拡張ではASTのitem vectorと既存`P2C_AstWith.is_async`を保持したまま、context object、enter result、pending exception、exit resultをitemごとのgenerator localへ分離する。本文に複雑なawaitやreturnを混在させる既存制約は維持し、状態機械で安全に表現できない入力を暗黙変換しない。

## References

[2]: https://docs.python.org/3.13/reference/compound_stmts.html "Python 3.13 Language Reference — Compound statements"
[3]: https://peps.python.org/pep-0492/ "PEP 492 — Coroutines with async and await syntax"
[4]: https://docs.python.org/3.13/library/contextlib.html "Python 3.13 Library Reference — contextlib"
