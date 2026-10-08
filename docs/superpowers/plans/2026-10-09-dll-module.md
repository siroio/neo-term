# Emacs DLL module implementation plan

Goal: neo-termの通常のConPTYセッションをEmacs用DLLで動かし、入力・Unicode・リサイズ・大量出力・終了とシェル連携を維持する。

Architecture: 既存のConPTYバックエンドと画面イベント形式を共有する。DLLのワーカースレッドでセッションを動かし、認証した127.0.0.1接続からEmacsへ画面を通知する。Emacs APIはEmacsから呼ばれたモジュール関数内だけで使う。入力は非同期キューへ渡す。DLL非対応環境と従来APIには既存EXE経路を残す。

Constraints: コメントはWhy notのみ。上流へ送信しない。既存の未コミット修正を維持する。各セッションの環境とcwdは子プロセスへ明示し、Emacsの環境・コンソール状態を変更しない。ビルド済みDLLを同梱する。

- [x] DLL経由の起動・Unicode・終了をテストにし、未実装の失敗を確認する。
- [x] 画面エンコーダーを共有し、DLLの非同期セッションとモジュールAPIを実装する。
- [x] バッファごとのDLL接続と終了をEmacsへ追加し、通常起動でDLLを選ぶ。
- [x] 複数セッションの環境・cwd・シェル連携とコンソール状態を検証する。
- [x] 大量出力・Ctrl+C・反復リサイズ・停止・GUIを検証し、ビルドと全テストを実行する。
- [x] 独立レビュー、性能測定、README更新を行う。

回帰検証: セッション固有PATH/cwdの探索と正常終了時のバッファ破棄は、失敗するテストを追加して修正した。レビューで判明したConPTYスレッドのキャンセル競合は、完了確認までCancelSynchronousIoを繰り返すことで修正した。

Windows版EmacsのCRTパイプ管理と外部モジュールによるFD破棄の組み合わせは30回の起動/GC後に切断を生じたため、公開ネットワークprocess APIを利用するイベント接続へ変更した。接続は127.0.0.1のみで、BCryptの256bitトークンを照合してからセッションを引き継ぐ。終了時はshutdownで送信を解除し、workerがsocketとバックエンドを破棄する。

画面クリアはGUIと独立したDLLテストで失敗を再現し、ConPTYの消去通知とlibvterm内の画面消去を組み合わせた。[公式の信号ハンドラー](https://github.com/microsoft/terminal/blob/main/src/host/PtySignalInputThread.cpp)はConPTYのバッファとカーソルを変更するため、ホスト側の画面状態も更新する。履歴は消去しない。追加レビューで、入力parserにESC列を挿入すると受信途中のCSI/OSCを破壊する問題を発見した。分割された色指定を壊す失敗をネイティブテストで再現し、parserに触れないvterm_state_clear_screen APIへ変更した。色指定・タイトルの残りが正しく解釈されることを検証する。

性能: 同じEmacsバッチ描画でDLL/EXEを3回ずつ測定。入力応答の中央値はEXE 9.3ms / DLL 1.9ms、3000行出力と最終描画までの中央値はEXE 1.38秒 / DLL 1.87秒。read-process-output-max拡大とadaptive読み取り無効化でも大量出力は改善しなかったため、Emacsの全体設定を変更しない。EXE経路で標準ConPTYのVim保存が一度タイムアウトしたが、同じVimサブケースの単独再実行は全バックエンドで成功した。

最終検証: MSVC /W4 /WXビルド、ネイティブ契約とVT再配置/分割CSI/OSC、EXE CLI43件、ERT46件、DLL統合16件、GUI、byte compile、diff checkが成功。クリア履歴テストは画面と履歴が別フレームで届くことを踏まえ、テスト開始時点の履歴が保持されていることを検証する。全CLI/ERTはbuild/dll-final-verified.log、最終DLL16件はbuild/module-final-green.log、GUIはbuild/gui-check.logに記録した。追加レビューで新APIのparser継続状態保持を確認した。
