# SDD ledger — plan: docs/superpowers/plans/2026-10-08-neo-term.md

ユーザーは「どんどん進めていって」と指示。計画承認の追加停止を省き、現在のチャットで継続実行する。

Workspace: ユーザー指定の新規neo-termディレクトリに独立Gitリポジトリ・developmentブランチを作成。兄弟リポジトリと共有状態なし。

Pre-flight: Task 1の通信契約をTask 2とTask 3が使用する。入力/出力のJSONスキーマはTask 1で固定し、両側で異常長を検証する。
Pre-flight: Task 2のscreen出力をTask 3が表示する。セル幅・カーソル・色はnative側を正とし、Emacs側でCLI文字列を再解釈しない。
