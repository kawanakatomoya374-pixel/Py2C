# Python Code to C Alpha0.6 構文・GC・生成C改善記録

## 構文監査

実装中の明示的な未対応診断と、変換を受理するが生成Cで失敗する構文を調査した。辞書内包表記、単一継承・多段継承、`super()`は実行結果をCPythonと比較して確認した。一方、`del list[a:b:c]`は受理されるもののスライスが内部的に呼び出し式として表現されるため、従来は不正なC代入式を出力する問題を確認した。

## 今回の実装

- `del list[a:b:c]`を通常スライス、拡張スライス、逆順stepで実装。
- `list[a:b:c] += iterable`を実装。ターゲットと境界式、右辺を一度ずつ評価する。
- `list[a:b] = list`の自己参照代入で、再配置やメモリ移動による右辺破壊を防ぐスナップショットを追加。
- 連続スライス削除は補助ビットマップを使わず、`memmove`で処理するよう最適化。
- `--supported`とREADMEを更新。

## GC

ランタイムは、明示的ルート、ランタイムレジストリ、ネイティブ側pin、保守的スタックスキャンを根として、オブジェクトグラフを走査するstop-the-world mark-and-sweep GCを採用している。list、tuple、dict、set、function closure、instance、class、module、iterator、exception、cellをtraverse対象としている。循環参照、allocation failure、runtime再初期化の既存検証を実行した。

## 検証

- 新規スライス代入・削除・複合代入・自己参照代入を、生成Cの実行結果とCPythonの出力比較で検証。
- 辞書内包表記と継承・`super()`をCPython比較で検証。
- GCCによるHosted CLIビルド、GC lifecycle、GC allocation failure、単一ヘッダー、strict C11、freestanding単一ヘッダー検証に成功。

## 残る主要な未対応領域

- bytes/bytearray・複素数
- 多重継承とMRO
- ネストしたクラス定義
- メソッドの`*args`/`**kwargs`とメソッドデコレータ
- 複数for節のジェネレータ式、ネストクロージャ内のジェネレータ式捕捉
- async forの完全な状態機械
- finally/with内部で保留中の制御フローを上書きするreturn/break/continue
