# Python Code to C Alpha0.6 — Alpha0.8 メソッド引数機能拡張

## 追加した構文対応

| 機能 | 対応内容 |
|---|---|
| メソッド `*args` | 余剰位置引数をtupleへ束縛 |
| メソッド `**kwargs` | 未使用の名前付き引数をdictへ束縛 |
| メソッド名前付き引数 | 通常引数、デフォルト引数、キーワード専用引数へ正しく束縛 |
| メソッド `**mapping` | dictの文字列キーを展開してキーワード引数として配送 |
| キーワード専用引数 | `def method(self, *, flag=False)`の位置引数拒否と名前付き束縛 |

## 内部設計

`P2C_MethodDef`へキーワード対応adapterを追加し、従来の位置引数adapterとの後方互換性を保った。ユーザー定義クラスのメソッドは`p2c_call_attr_kw`を経由し、通常のキーワードと`**mapping`を平坦化した後でadapterに渡す。ネイティブpygame互換メソッドはキーワードadapterをNULLとして登録し、従来どおり名前付き引数を明示的に拒否する。

## 検証

`tests/method_varargs_kwargs_alpha07.py`で、位置引数、`*args`、キーワード専用引数、`**kwargs`、`**mapping`展開をCPython出力と比較した。既存のスライス、辞書内包表記、継承・`super()`回帰も比較した。さらにGC lifecycle、GC allocation failure、single-header、strict C11、freestanding single-headerをGCCで検証した。

## 既知の境界

ユーザー定義メソッドの直接的な名前付き引数と`**dict`展開を対象とする。呼び出し側の`*iterable`による動的な位置引数展開、メソッドデコレータ、`classmethod`/`staticmethod`、多重継承MRO、bytes/bytearray、複素数、複数for節のジェネレータ式は引き続き未対応である。
