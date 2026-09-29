# Python Code to C Alpha0.6 拡張改修記録

## 今回の追加対応

- `list[start:stop:step] = iterable` のスライス代入を追加。
- 通常スライスでは代入によりリスト長が増減する処理を追加。
- 拡張スライスでは選択要素数と右辺の要素数が一致することを検査。
- 正のstep・負のstep・省略境界・負の境界値・空のlist/tupleによる置換に対応。
- 右辺はlistまたはtupleに限定し、非listへのスライス代入は明確なTypeErrorを発生させる。
- READMEと`--supported`表示を更新。
- `tests/slice_assignment_alpha07.py`を追加。

## 検証結果

- GCCによるホストCLIのビルドに成功。
- 生成CをC11でコンパイル・実行し、CPythonの出力と一致。
- コンテナファジング生成スクリプトの変換に成功。
- single-header通常版、strict C11版、freestanding版の検査に成功。

## 参考アーカイブ

付属のMicroPythonソースは、処理系の一般的な構造と対応構文の比較材料として確認した。既存Py2Cの層構造を保つ必要があるため、MicroPythonのVM/compilerコードを直接取り込む変更は行っていない。
