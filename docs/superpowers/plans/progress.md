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
Task 1: complete (commits 48e76b0..adef705, tests: build.ps1 -Test → NATIVE_TESTS=PASS)
Task 2: complete (commits adef705..707f053, tests: Python unittest test_host → 9/9 PASS)
Task 3: review pending (commit d7db366, tests: test.ps1 -Gui → native PASS / host 9/9 / ERT 6/6 / GUI PASS)
Final review: fresh-context reviewer gpt-6-astra、Criticalなし、Important2件、Minorなし。
Final: fixed 未読stdoutと終了要求の競合 — test_close_releases_helper_when_screen_output_is_not_read RED→GREEN、main/input双方を完了まで繰り返しキャンセルする。
Final: fixed nativeセル幅とEmacs幅の不一致 — neo-term-native-cell-width-overrides-emacs-unicode-width RED→GREEN、GUIピクセル比較もRED→GREEN。バッファ固有の幅テーブルと必要時だけSVG表示を使い元の文字列を保持する。
Final: Ruling: classicでの非BMP復元・完全履歴・代替画面識別は初版制限を維持 — APIが返さない情報の復元を約束しない — classicで絵文字や一部TUI・履歴が欠ける。
Final: Ruling: マウス・画像プロトコル・シェル連携・カラー付き履歴は初版対象外を維持 — 文字端末の初期範囲を完成させる — 対応するアプリは一部機能を使えない。
Final: Ruling: Windows旧ビルドとEmacs29は最低要件として記載し未検証と明示 — 実測済みのbuild26300/Emacs31.1だけを確認済みとする — 古い環境では追加修正が必要になる可能性がある。
Final verification: test.ps1 -Gui → native PASS、host 12/12、ERT 7/7、byte compile警告なし、GUI Unicodeピクセル幅/TUI/3バックエンド PASS。git diff --check成功。Deferred minors: none。
Task 3: complete (tests: test.ps1 -Gui → 12/12 host, 7/7 ERT, native/bytecompile/GUI PASS)
Finish: 新規独立リポジトリで分岐元・remoteがないため、既存ブランチへのmergeや公開は行わずdevelopmentを保持。ユーザーの「どんどん進めていって」に従い不要な統合確認で停止しない。

Follow-up readability: ユーザーの順序指定に従い、コード全体の改行・インデント・関数抽出を先に完了。commit 0e3e047。既存のnative/host12/ERT7/bytecompile/GUI検証を維持。
Follow-up mode RED: 表示編集、Delete、C-cプレフィックス、履歴消去の追加ERT5件が失敗。矢印などの特殊キーはEmacsコマンドへ解決され、ASCII EnterもCtrl+Mとして送られることを追加テストで再現。
Follow-up mode: 既存のneo-term-modeを強化。表示文字の読み取り専用属性と変更フック、コマンド終了時・描画時の再保護。C-cを端末操作プレフィックス、C-x C-qをコピー切り替えに変更。特殊キーと修飾キーは明示的にCLIへ送信。
Follow-up clear: LはWin32の画面消去、Hは履歴消去。ConPTYはCLIコンソールへ一時接続して消去し、シェル文字列は送らない。Hの完了はscreen.history_clearedで通知して、先行する履歴フレームとの順序を保つ。
Follow-up review: 独立レビューで変更フックの例外による解除を確認。コマンド終了時・描画時の再登録を追加して解決。履歴が溜まった状態の統合テストも追加。他のImportantなし。
Follow-up verification: build.ps1 -Test と test.ps1 -Gui → native PASS、host15/15、ERT16/16、警告なしbytecompile、3バックエンドGUI PASS。cmd/PowerShellで入力途中の文字列を保持したクリアも確認。clang-format/Black/git diff --check通過。
