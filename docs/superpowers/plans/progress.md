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

vterm parity: 差分描画、RETでコピーして復帰、プロンプト除外、PowerShell cwd/prompt/title、別ウィンドウ、任意の終了時バッファ破棄、OSC52の既定無効コピー、要求されたマウス入力を追加。
vterm parity RED/GREEN: 変更行・履歴位置、コピー、メタデータ、キー、OSC52、マウスに再現テストを追加。PowerShellの実行ポリシーは変更せず、EncodedCommandで付属コードを起動する。
vterm parity review: 独立レビューの3件（screen-start移動、ドラッグ終了座標、PowerShellの$?）を再現して修正。プロンプトは最初に呼び出し、LASTEXITCODEにも余分な代入をしない。
vterm parity performance: 同一条件の200更新、履歴2000行/80列24行。カーソルのみ5.152→0.030秒、1変更行5.484→0.211秒。vtermとの直接比較ではない。
vterm parity limits: 標準ConPTYでマウスモードが転送されない場合があり、このPCでは同梱版で確認。classicマウス、カラー履歴、折り返し行の結合、UNC/WSL/TRAMP追跡は未対応としてREADMEへ記録。
vterm parity verification: native PASS、CLI20/20（実機Vimを含む）、ERT28/28、警告なしbytecompile、3バックエンドGUI・同梱版マウス PASS。PowerShellテストのstderrはバイトで収集して混在する文字コードによる検証スレッドの例外を解消。書式チェック通過。

History/reflow: v0.3.0。セル属性付き履歴、soft wrap追跡と論理行コピー、履歴・主画面の幅変更、前後プロンプトへの移動を実装。C-c C-p/C-n、コピーのC-a、RET/M-wを追加・更新。
History/reflow RED/GREEN: カラー・wide/combiningセル・履歴/画面境界・マーカー・プロンプト消去/折り返しを再現。libvtermの先頭継続行、セル分割、リサイズ後の入力上書き、狭い画面での履歴欠落を再配置処理で修正。
History/reflow review: 独立レビューでclassicのrows2履歴追跡、境界カーソルによる実改行の誤結合を修正。実WinConsoleカーソル座標によって標準ConPTY側の反復リサイズ不具合を切り分け、アプリのカーソル管理と競合する強制補正は採用しない。
History/reflow limits: classicは折り返し情報を返さず、元の各行を別論理行として扱う。ConPTYが転送済みSPACEへ変換した空きセルは識別できず保持する。同梱版の末尾入力保持を検証し、標準版の既知の入力上書きをREADMEへ記録。
History/reflow verification: native/libvterm PASS、CLI26/26（実機Vim含む）、ERT40/40、警告なしbytecompile、3バックエンドGUI PASS。クリア検証は入力全体の表示を待って競合を解消。描画200回/履歴2000行でcursor 0.049秒、1変更行0.235秒。clang-format/Black/git diff --check通過。

Shell notifications: v0.4.0。OSC 133とセル内の開始・入力位置によって通常出力とのプロンプト誤認を防ぎ、履歴・reflow後も位置を保持する。classicのPowerShellはコンソール座標と実表示の確認を使う。
Shell notifications: バッファごとのカーソル形状・点滅、cmd.exe・Git Bashの自動連携、ローカル名前付きパイプ、neo-openとC-c C-fによるファイル・行・列への移動を追加。PowerShellの実行結果・元prompt、BashのPS1・PROMPT_COMMANDを保持する。
Shell notifications review: classicのリサイズ・固定バッファスクロールでの位置移動、wide/surrogateセル参照、引用符付きパス、空白セルの入力境界消失を修正。PowerShell 5.1のネイティブ引数引用によるJSON破損は、Windows Crypt32によるBase64通知で解消。
Shell notifications limits: classicの正確な位置通知はPowerShellのみ。cmd.exe・Git Bashはcwdとファイル要求に対応。classicのカーソルはサイズとWindowsの点滅設定から推定し、個別のVT形状・点滅は取得できない。
Shell notifications verification: build.ps1 -Testとtest.ps1 -Gui → native/libvterm PASS、CLI31/31（3バックエンド・Git Bash・実機Vim含む）、ERT46/46、警告なしbytecompile、GUIカーソル・ファイル表示 PASS。描画200回/履歴2000行でcursor 0.051秒、1変更行0.231秒。
Shell notifications final review: classic最下行の長いpromptで描画中スクロールにより開始座標が移動することを追加実機テストで再現。下端を超える通知のみ、明示座標から有限範囲の移動候補を完全表示・終了カーソルで照合して修正。追加テストRED→GREEN。
Shell notifications final verification: CLI32/32、ERT46/46、native/libvterm・bytecompile・GUI PASS。探索上限へ開始列を加えた最終ビルドでも、プロンプト境界・位置通知・クリア・classicリサイズの追加回帰4件 PASS。clang-format・Black・git diff --check通過。ローカルGitのauthor/committerをsiroioへ設定。
