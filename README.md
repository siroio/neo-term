# neo-term

Windows版Emacsのバッファ内でWindowsのCLIを操作する端末パッケージです。Emacsに読み込んだC++のDLLがConPTYと独自の端末エンジンを制御し、Emacs Lispが画面と入力を扱います。端末エンジンにlibvtermを使用しません。

## 導入

Windows x64用の `build/neo-term-module.dll` を同梱しています。仲介EXEは配布・ビルド・起動しません。動的モジュールとSVGに対応したGUI Emacs 29.1以降、Windows 10 1809以降のx64が必要です。動作確認はEmacs 31.1 / Windows build 26300です。利用時にコンパイラーやPythonは不要です。

```elisp
(use-package neo-term
  :vc (:url "https://github.com/siroio/neo-term" :rev :newest)
  :commands neo-term)
```

手動でcloneした場合:

```elisp
(add-to-list 'load-path "C:/path/to/neo-term")
(require 'neo-term)
```

`M-x neo-term` で起動します。pwshがあればpwsh、なければWindows PowerShellを使い、起動時の `default-directory` と環境変数を引き継ぎます。`M-x neo-term-other-window`、`neo-term-cmd`、`neo-term-git-bash` も使用できます。`neo-term-kill-buffer-on-exit` を `t` にするとCLIの終了時にバッファを閉じます。

## ConPTYランタイム

`neo-term-backend` は `auto`（既定）、`bundled-conpty`、`system-conpty` から選べます。`auto` は同梱ConPTY、Windows標準ConPTYの順で初期化します。CLI起動後に別方式へ再起動しません。DLLの読み込みや初期化が失敗しても仲介EXEへ切り替えません。従来Console APIの `classic` と `neo-term-no-conpty` は使用できません。

`neo-term-transport` は `dll` が既定です。既存設定の `auto` もDLLのみを使い、`exe` はエラーになります。バックエンド制御・入力・画面取得はDLL内のワーカースレッドで処理します。入力は非同期キュー、画面通知はトークン認証付きの127.0.0.1接続を使います。バッファ破棄はCLIの終了処理を待ちません。

Microsoft公式ConPTY **1.25.260930003** は次のコマンドで導入できます。

```powershell
.\install-runtime.ps1
```

SHA256を照合し `build/runtime/conpty.dll` と `OpenConsole.exe` を配置します。OpenConsole.exe / Windowsのconhost.exeとCLI自身のプロセスは引き続き必要です。通常利用時に外部ネットワークへ接続しません。ランタイムの再配布にはMicrosoftのMITライセンスも添付してください。

画面クリア `C-l` はConPTYの消去APIを使います。そのAPIがない標準ConPTYでは、CLIを維持したまま操作をエラーにします。同梱ConPTYでは画面クリア・履歴保持・その後の入力を確認済みです。`M-x neo-term-describe-session` / `C-x C-d` で使用したランタイムと初期化失敗理由を確認できます。

## 操作

| 操作 | キー |
| --- | --- |
| 文字・矢印・Enter・Tab・Backspace・Delete・F1〜F24を送る | 通常どおり |
| CLIを中断 | `C-c C-c` |
| 現在の画面をクリアする | `C-l` |
| スクロールバック履歴を消す | `C-c C-l` または `C-c M-l` |
| キルリングを貼り付け | `C-y`、`S-insert` または `C-c C-v` |
| 履歴を選択・コピーするモードへ切り替え | `C-c C-t`、`C-x C-t` または `C-x C-q` |
| コピーモードで選択範囲をコピーする | `M-w` |
| 前／次のプロンプトの入力開始位置へ移動する | `C-c C-p` / `C-c C-n` |
| 選択範囲またはカーソル位置のファイル参照を開く | `C-c C-f` |
| コピーモードで入力開始位置／論理行の先頭へ移動する | `C-a` |
| 選択範囲または現在行をコピーして端末へ戻る | コピーモードで `RET` |
| コピーモードから端末へ戻る | `q` または切り替えキー |
| セッション情報 | `C-c C-d` または `C-x C-d` |
| セッションを閉じる | `C-c C-k`、`C-x C-k` またはバッファ破棄 |
| Emacs用に予約したキーをCLIへ送る | `C-q` の後に対象キー |

`neo-term-mode` は端末の表示を保護します。BackspaceとDeleteはCLIへ送信し、Emacsのバッファから文字を削除しません。読み取り専用を解除しても、表示文字の読み取り専用属性と変更フックが通常の直接編集を防ぎ、各コマンドの終了時に保護を再設定します。`C-x C-q` は編集可能にする操作ではなく、コピーモードの切り替えです。

`C-c` は端末操作のプレフィックス、`C-x` はEmacs操作、`C-g` はEmacsの中断です。CLIへ直接Ctrl+Cを送る場合は `C-c C-c`、予約キーをそのまま送る場合は `C-q` を使います。コピーモード中は画面の描画を止め、`M-w` でコピーでき、戻ると最新画面へ追いつきます。履歴上限は `neo-term-scrollback-lines`（既定2000行）です。

`C-c C-l` はvtermに合わせて履歴消去へ変更しました。画面消去には `C-l` を使います。`neo-term-clear-scrollback-when-clearing` を `t` にすると `C-l` で両方を消去し、`C-u C-l` はその設定を反転します。

画面クリアはC++側からWindowsのコンソールを消去し、CLIを再起動したり `cls` を入力したりしません。入力途中のCLI文字列とスクロールバックは保持します。TUIはアプリ自身が再描画する場合があります。`C-c M-l` は履歴だけを消し、現在の画面は保持します。クリア・貼り付け・CLI中断はコピーモードから入力モードへ戻って実行します。

## シェル連携とコピー

通常のPowerShell/pwsh起動では、付属の初期化コードを `-EncodedCommand` で渡します。neo-term内のセッションのコンソール入出力とネイティブプログラムへのパイプ出力をBOMなしUTF-8に設定し、Windows PowerShellで絵文字が `?` に変換されることを防ぎます。既存のprompt関数を呼び出し、その表示を維持しながら、OSC 133でプロンプト開始・入力開始を通知します。名前付きパイプへ直接コンソール座標を通知します。作業ディレクトリも名前付きパイプで送ります。実行ポリシーやプロファイルファイルは変更しません。`-Command`、`-File`、独自の起動引数がある場合は起動内容を変更しません。

cmd.exeは起動用スクリプトで既存のPROMPTを包み、Git Bashは既存のPS1・PROMPT_COMMANDを維持して通知を追加します。Git Bashは既存の `.bashrc` を読み込みますが変更しません。通常の対話起動だけが自動連携の対象です。ConPTYでは3シェルともプロンプト位置を通知し、その位置を色付き履歴とリサイズ後のセルへ引き継ぎます。PowerShellとcmdは名前付きパイプ、Git BashはBash標準のTCP接続で通知します。通知用EXEは起動しません。

各シェルで `neo-open README.md 12 3` を実行すると、Emacsの別ウィンドウでファイルを開き、12行目・3列目へ移動します。行・列は1始まりで省略できます。`C-c C-f` は選択範囲やカーソル位置の `src/main.cpp:12:3` などを開きます。相対パスはシェルの作業ディレクトリを基準に解決します。シェルからのファイル要求を無効にするには `neo-term-enable-file-requests` を `nil` にします。

ローカルドライブの既存ディレクトリを `default-directory` に反映します。UNC、WSL、TRAMPのパスは現在の追跡対象外です。`neo-term-shell-integration` を `nil` にすると自動連携を無効にできます。端末から受け取ったデータをLispコードとして実行する機能はありません。

コピーモードで `RET` を押すと、選択範囲があればその範囲、なければ現在行をコピーして入力モードへ戻ります。現在行のコピーでは、追跡済みのプロンプトと末尾の空白を除きます。`neo-term-copy-exclude-prompt` を `nil` にするとプロンプトも含め、`C-u RET` はその設定を反転します。正確な通知を受けたセッションではセルの位置情報で判定します。位置通知のない既存連携では文字列の先頭一致を使います。

ConPTYでは画面幅による折り返しと明示的な改行を区別します。`RET` の現在行コピーは、カーソルが折り返しの途中にあっても論理行全体をコピーします。`RET` の範囲コピーと `M-w` も折り返しだけの改行を除き、明示的な改行と実際の空白は保持します。`neo-term-copy-remove-soft-newlines` を `nil` にすると画面上の改行もコピーします。

履歴にはセルの色・背景・太字・下線などを保持します。ウィンドウ幅の変更時は履歴の論理行を新しい幅で再配置し、日本語・絵文字・結合文字のセルを分割しません。ConPTYの現在画面は独自エンジンで再配置します。コピーモード中は表示を固定し、戻るとリサイズと出力に追いつきます。履歴上限は保存した元の行数なので、幅を狭めると表示上の行数は増えます。

`C-c C-p` / `C-c C-n` はコピーモードへ入り、追跡済みの前／次のプロンプトの入力開始位置へ移動します。数値引数で移動数を指定できます。コピーモードの `C-a` は入力開始位置へ移動し、そこでもう一度押すと論理行の先頭へ移動します。幅によってプロンプトが折り返されても追跡します。

ConPTYが全角文字を折り返す際、空きセルを実際のSPACEとして転送する場合があります。このSPACEはCLIが出した空白と区別できないため、コピーに残ることがあります。空きセルとして識別できるものだけを除き、本来の空白を推測で削除しません。位置通知のない既存連携では、プロンプトと同じ文字列で始まる通常出力を誤認する場合があります。

ConPTYではCLIが指定したブロック・下線・バーのカーソル形状と点滅を、バッファごとに反映します。コピーモードではEmacs操作用のバーになります。

タイトルをバッファ名に反映する場合:

```elisp
(setq neo-term-buffer-name-string "*neo-term: %s*")
```

ConPTYではOSC 52によるコピーにも対応します。既定は無効です。有効にするとCLIが送った文字列をキルリングとクリップボードへコピーします。問い合わせによるクリップボードの読み取りは実装していません。1回64KiB、1画面イベント8件の上限があります。

```elisp
(setq neo-term-enable-osc52 t)
```

マウス報告を要求するConPTYアプリにはクリック・ボタン解放・ホイール入力を送ります。`neo-term-enable-mouse` を `nil` にすると無効になります。コピーモードではEmacsの選択・コピー操作になります。この環境の同梱ConPTYで動作を確認しました。標準ConPTYではマウスのモード要求が転送されず、使えない場合があります。

ウィンドウサイズ変更をCLIへ通知します。バッファを破棄すると、そのCLIとJob Object内の子孫プロセスを終了します。

## 検証と制限

ネイティブ契約・独自エンジンの再配置/分割VTテスト、実際のEmacs経由のCLI統合、ERT、DLL統合、警告なしのbyte compile、GUIを検証します。CLIテストは実際のEmacsにDLLを読み込ませるテスト用Lispから制御し、仲介EXEを使いません。出力が詰まった状態の非同期終了はDLL統合テストで確認します。

日本語・絵文字・結合文字、反復リサイズ、3000行の履歴末尾、Ctrl+C、子孫プロセスの終了、30回の起動/GC、セッション別cwd/環境、DLL不足/初期化失敗、画面クリア、PowerShell/cmd/Git Bashの通知を検証します。GUIではEmacsのキー操作による入力とコピー、色・代替画面・プロンプト・カーソル・ファイル要求・同梱ConPTYのマウスを確認します。Vimを起動するテストはありません。

開発時はVisual StudioのC++ツールを使ってビルドします。テストにはPython 3、動的モジュール対応Emacs、同梱ConPTYが必要です。

```powershell
.\build.ps1 -Test
.\test.ps1 -Gui -Python 'C:/path/to/python.exe' -Emacs 'C:/path/to/emacs.exe'
```

ネイティブ変更時は `build/neo-term-module.dll` も更新します。テスト用のCLI実行ファイルはGit管理・配布の対象外です。Gitからの導入確認は `emacs -Q --batch -l tests/check-package.el` で行えます。

DLL化時の同じEmacsによるバッチ測定では、入力から描画までの中央値は約1.9ms、起動から3000行出力・最終描画・終了まで約1.87秒でした。GUIの実測値ではありません。再測定は `emacs -Q --batch -l tests/benchmark-module.el` で行えます。描画処理の測定は `tests/benchmark-render.el` です。

変更行と履歴の増減だけを描画し、カーソルだけの更新では文字を書き換えません。ConPTYの出力を専用スレッドで待ち、VT解析時に変更行を記録します。通知を作る際はその行だけを取得・JSON化し、差分検出のための全セル走査は行いません。出力・入力・シェル通知・プロセス終了のイベントでワーカーを起こし、アイドル中の画面取得は行いません。通知後の固定8ms待ちとEmacs側の固定20ms描画待ちはありません。行内の文字はまとめてバッファへ挿入し、フォント情報は描画ごとに取得します。未送信の履歴が64行以上ある間は待たずに送信します。親の終了後はJob Object内の子孫を終了させ、出力のEOFと残った履歴を確認してから終了を通知します。

通常のGUIタイマー経路を通す入力測定は `emacs -Q -l tests/benchmark-input.el` で実行できます。`NEO_TERM_BENCH_SHELL=powershell.exe` を設定すると通常のシェル入力を測定します。結果は `build/input-benchmark.log` へ保存します。Emacsのキーマップを通した入力からredisplayまでの測定で、物理キーの配送と画面走査は含みません。

Unicode 16.0.0の文字幅・結合文字表を使用します。1セルに保持する文字は基底文字を含め16コードポイントまでで、超過する結合文字は読み飛ばします。複数コードポイントからなる絵文字の全組み合わせ、画像、任意のTUIの完全互換は保証していません。未対応のDCS画像・端末機能問い合わせは読み飛ばします。リサイズでは現在画面のカーソルを再配置しますが、別途保存したカーソル位置は復元時に画面内へ制限します。プロンプト位置は出力と順序が一致するOSC 133を使用し、別チャネルで届くコンソール座標からの推測は行いません。標準ConPTYでは反復リサイズで入力末尾を上書きするケースがあり、同梱ConPTYで末尾保持を検証します。

2026-10-09の表示最適化では、変更セルだけを置換し、未変更のプロンプト属性を保持します。通常のセルには値のない表示属性を付けません。履歴・表示構成が変わらない入力では、プロンプト検索を現在画面と必要な前の文脈に限定します。GUI EmacsとPowerShellによるbyte compile済みの最終比較では、通常入力の中央値が約7.6〜7.9msから約6.0〜6.7msに短縮しました。2000件のプロンプト履歴を生成した負荷では、検索範囲の限定により約12.3〜13.7msから約8.1msへ短縮しました。通常入力のバッファ更新は約0.09〜0.10msですが、GUI再表示にはなお約5msかかり、全体0.2msには達していません。物理キーの配送と画面走査は含みません。

切り分け用の `tests/profile-display.el` はGUI版Emacsで、`tests/profile-prompts.el` はバッチ版Emacsで実行します。`tests/benchmark-input.el` に `NEO_TERM_BENCH_HISTORY=2000` を設定すると、PowerShell経由でOSC 133付きのプロンプト履歴を生成した後に入力を測定できます。詳細は `docs/superpowers/plans/2026-10-09-input-latency-profile.md` にあります。

## 構成とライセンス

`native/module.cpp` はEmacsモジュールとセッションの寿命、`native/conpty.cpp` はConPTY/libvterm、`neo-term.el` は表示と操作です。画面通知は4MiB上限の長さ付きUTF-8 JSONです。Emacs APIはEmacsから呼ばれたモジュール関数内だけで使用します。通知をLispコードとして実行しません。

`native/shell.hpp` はセッション固有の名前付きパイプと127.0.0.1限定のTCP通知を扱います。通知は16KiBを上限とし、TCPでは256bitのセッショントークンを照合します。接続途中の通知には2秒の期限があります。PowerShellは.NET、cmdはシェルのリダイレクト、Git Bashは標準のTCP機能から直接送信します。

neo-termはGPL-3.0-or-laterです。Emacsモジュールヘッダーは `vendor/emacs-module.h` です。独自エンジンは `native/terminal.cpp`、Unicodeの表とライセンスは `native/unicode-width.hpp` と `native/unicode-data/` にあります。

独自エンジンのUTF-8/VT分割受信、色・カーソル・モード・履歴・全角セルの再配置、入力符号化を `tests/terminal-test.cpp` で検証します。

Unicodeの表は公式の [UnicodeData.txt](https://www.unicode.org/Public/16.0.0/ucd/UnicodeData.txt) と [EastAsianWidth.txt](https://www.unicode.org/Public/16.0.0/ucd/EastAsianWidth.txt) から生成し、Unicode License V3を同梱しています。再生成は `python tools/generate-unicode.py`、生成内容の照合は `python tools/generate-unicode.py --check` です。
