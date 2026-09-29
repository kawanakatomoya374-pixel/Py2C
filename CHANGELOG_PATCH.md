# Python Code to C Alpha0.6 改修記録

## 対応内容

- 括弧・角括弧・波括弧の内部で、物理改行と行頭インデントを論理構文へ混入させないlexer処理を追加。
- Pythonの隣接文字列リテラル連結（通常文字列同士、通常文字列とf-string、f-string同士）をparserで実装。
- f-string書式展開に使用する`p2c_format_fixed`と`p2c_fstr_fmt`を意味解析の組み込みシンボルへ登録。
- `__name__`をモジュールコードで利用できる組み込みシンボルとして登録。
- `--supported`の表示を実装実態と既知の制限に同期。
- `tests/adjacent_string_literals_alpha06.py`を追加し、CPython出力との差分回帰を確認。

## 検証

- GCC 13.3.0でホストCLIをビルド。
- 隣接文字列・複数行f-stringを変換、C11コンパイル、実行し、CPython出力と一致。
- `tests/generate_container_fuzz.py`の変換に成功。
- single-header通常版、strict C11版、freestandingオブジェクト版の検査を実行。
