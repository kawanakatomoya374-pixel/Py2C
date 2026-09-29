# Python Code to C Alpha0.6

Alpha0.6では、製品・ファイル・CLI・マクロの名称を統一し、OS依存処理を `P2C_Platform` 契約へ集約しました。Hostedビルドとfreestandingコアライブラリを分離し、自作OS側からコアAPIを直接利用できるようにしています。既存の広い構文対応を維持しながら、診断と移植手順を整理しました。
