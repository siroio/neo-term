# SDD ledger — plan: docs/superpowers/plans/2026-10-08-neo-term.md

ユーザーは「どんどん進めていって」と指示。計画承認の追加停止を省き、現在のチャットで継続実行する。

Workspace: ユーザー指定の新規neo-termディレクトリに独立Gitリポジトリ・developmentブランチを作成。兄弟リポジトリと共有状態なし。

Pre-flight: Task 1の通信契約をTask 2とTask 3が使用する。入力/出力のJSONスキーマはTask 1で固定し、両側で異常長を検証する。
Pre-flight: Task 2のscreen出力をTask 3が表示する。セル幅・カーソル・色はnative側を正とし、Emacs側でCLI文字列を再解釈しない。

Task 1: Ruling: 入力は固定種別の長さ付きフレーム、出力はJSONにする — C++側にJSONパーサーを新設せず同じ検証と機能を保てる — 形式変更時は両側の修正が必要。
Task 1 RED: native-testが「backend selection and communication contracts are absent」で正常に失敗。
Task 1 GREEN: build.ps1 -Test → NATIVE_TESTS=PASS。
Task 2 RED: Python統合テスト7件がhelper未実装で失敗。
Task 2: Ruling: Backendのメソッド名はinitialize/launch/snapshot/command/stopに統一 — 初期化とCLI起動を分離して二重起動を防ぐ — 名前変更の影響はnative内部のみ。
Task 2: Ruling: classicの非BMP文字はWindowsが置換文字に変換する場合がある — ReadConsoleOutputWとReadConsoleOutputCharacterW両方でU+FFFDになることを実機確認し、仕様・テスト・制限に記録 — classicでは絵文字を完全に復元できない。
Task 3 RED: ERT6件が未実装関数で失敗。GREEN: ERT6/6、警告をエラー扱いしたbyte compile成功。
Task 2 GREEN: 同梱ConPTY 1.25.260930003・system・classicでCLI統合テスト9/9。通信断でCLI終了、閉鎖時の孫プロセス終了もハンドル待機で確認。
Task 3 GREEN: test.ps1 -Gui → native PASS、host 9/9、ERT 6/6、byte compile警告なし、3バックエンドGUI PASS。
