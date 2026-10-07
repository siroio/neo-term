# neo-term

Windows版Emacsのバッファ内でWindowsのCLIを操作する端末パッケージです。C++の仲介EXEが起動時にバックエンドを選び、Emacs Lispが画面と入力を扱います。winptyは使用しません。

動作確認環境はWindows x64（OS build 26300）、GUI版Emacs 31.1、MSVCです。必要環境はWindows 10 1809以降のx64とSVG対応のGUI Emacs 29.1以降です。ほかのバージョンは未検証です。

## 導入

Windows x64用のビルド済み `build/neo-term-host.exe` をGitに同梱しています。利用者側でのビルド、C++コンパイラー、Python、Emacsの動的モジュール対応は不要です。標準のバックエンドにはWindows付属のConPTYを使います。

`use-package :vc` が使えるEmacsでは、次の設定でGitHubから導入できます。

```elisp
(use-package neo-term
  :vc (:url "https://github.com/siroio/neo-term" :rev :newest)
  :commands neo-term)
```

Emacs 29では `package-vc-install` でも導入できます。

```elisp
(package-vc-install "https://github.com/siroio/neo-term")
```

手動でcloneする場合:

```powershell
git clone https://github.com/siroio/neo-term.git
```

```elisp
(add-to-list 'load-path "C:/path/to/neo-term")
(require 'neo-term)
```

`M-x neo-term` で新しいセッションを開きます。既定はpwshがあればpwsh、なければWindows PowerShellです。起動時の `default-directory` を引き継ぎます。

cmdを使用する場合:

```elisp
(setq neo-term-shell "cmd.exe"
      neo-term-shell-arguments '("/Q"))
```

## バックエンド

`neo-term-backend` は `auto`、`bundled-conpty`、`system-conpty`、`classic` から選べます。`auto` は同梱ConPTY → Windows標準ConPTY → 従来Console APIの順に初期化を試します。明示指定時はその方式だけを使います。CLI起動後の切り替え・再起動は行いません。

ConPTYを一切使いたくない場合:

```elisp
(setq neo-term-no-conpty t
      neo-term-backend 'auto)
```

`M-x neo-term-describe-session` または `C-x C-d` で、選択結果・ランタイムパス・初期化失敗理由を確認できます。APIの可用性を調べる方式なので、すべてのCLIとの互換性を判定するものではありません。

同梱版は任意です。Microsoft公式NuGetパッケージの固定版 **1.25.260930003** を導入する場合:

```powershell
.\install-runtime.ps1
```

パッケージをダウンロードし、SHA256を照合して `build/runtime/conpty.dll` と `OpenConsole.exe` を取り出します。通常起動時にはネットワークへ接続しません。EXEを移動する場合は同じディレクトリの `runtime/` へランタイムを配置します。再配布時はMicrosoftのMITライセンスも添付してください。

## 操作

| 操作 | キー |
| --- | --- |
| 文字・矢印・Enter・Tab・Backspace・Delete・F1〜F24を送る | 通常どおり |
| CLIを中断 | `C-c C-c` |
| 現在の画面をクリアする | `C-l` または `C-c C-l` |
| スクロールバック履歴を消す | `C-c M-l` |
| キルリングを貼り付け | `C-y`、`S-insert` または `C-c C-v` |
| 履歴を選択・コピーするモードへ切り替え | `C-c C-t`、`C-x C-t` または `C-x C-q` |
| コピーモードで選択範囲をコピーする | `M-w` |
| コピーモードから端末へ戻る | `q` または切り替えキー |
| セッション情報 | `C-c C-d` または `C-x C-d` |
| セッションを閉じる | `C-c C-k`、`C-x C-k` またはバッファ破棄 |
| Emacs用に予約したキーをCLIへ送る | `C-q` の後に対象キー |

`neo-term-mode` は端末の表示を保護します。BackspaceとDeleteはCLIへ送信し、Emacsのバッファから文字を削除しません。読み取り専用を解除しても、表示文字の読み取り専用属性と変更フックが通常の直接編集を防ぎ、各コマンドの終了時に保護を再設定します。`C-x C-q` は編集可能にする操作ではなく、コピーモードの切り替えです。

`C-c` は端末操作のプレフィックス、`C-x` はEmacs操作、`C-g` はEmacsの中断です。CLIへ直接Ctrl+Cを送る場合は `C-c C-c`、予約キーをそのまま送る場合は `C-q` を使います。コピーモード中は画面の描画を止め、`M-w` でコピーでき、戻ると最新画面へ追いつきます。履歴上限は `neo-term-scrollback-lines`（既定2000行）です。

画面クリアはC++側からWindowsのコンソールを消去し、CLIを再起動したり `cls` を入力したりしません。入力途中のCLI文字列とスクロールバックは保持します。TUIはアプリ自身が再描画する場合があります。`C-c M-l` は履歴だけを消し、現在の画面は保持します。クリア・貼り付け・CLI中断はコピーモードから入力モードへ戻って実行します。

ウィンドウサイズ変更をCLIへ通知します。バッファを破棄すると、そのCLIとJob Object内の子孫プロセスを終了します。

## 検証と制限

| 項目 | 同梱ConPTY | 標準ConPTY | 従来API |
| --- | --- | --- | --- |
| cmd / Windows PowerShellの対話操作 | 確認済み | 確認済み | 確認済み |
| 日本語・引数・キー・Ctrl+C・リサイズ | 確認済み | 確認済み | 確認済み |
| 絵文字の出力 | 確認済み | 確認済み | 置換文字になる場合あり |
| True Color・代替画面・大量出力 | 確認済み | 確認済み | 16色・代替画面識別なし |
| 通信断・バッファ破棄による終了 | 確認済み | 確認済み | 確認済み |

ネイティブ契約テスト、CLI統合テスト15件（バックエンド別のサブケースを含む）、ERT16件、警告なしのbyte compile、GUI確認が通過しています。GUIでは3バックエンドで入力・リサイズ・中断・コピー中の編集防止・矢印・Enter/Tab・Backspace/Delete・画面クリア後の入力、ConPTYの全画面・色・代替画面と字形のピクセル幅を確認しました。

Unicodeの幅定義やフォントの幅が一致しない字形は、SVGの表示プロパティでセル幅へ合わせます。バッファ内の文字列はそのままなので、日本語や絵文字をコピーできます。

従来APIは表示領域をポーリングします。短時間に上書きされた文字や急速なスクロールの履歴は取りこぼす場合があります。`ReadConsoleOutputW` は非BMP文字をU+FFFDに変換する場合があり、失われた絵文字を復元できません。ConPTY系も完全なvterm互換ではありません。初版はマウス、画像、シェル連携、カラー付き履歴、任意のTUIの完全互換を扱いません。

テストにはPython 3とEmacsが必要です。

```powershell
.\test.ps1
.\test.ps1 -Gui -Emacs 'C:\path\to\Emacs\bin\emacs.exe'
```

Pythonの場所は `-Python 'C:\path\to\python.exe'` で指定できます。GUI確認では実体のEmacs EXEを指定してください。自己完結した検証CLIで画面・入力・終了を確認します。

開発時にEXEと検証用プログラムを再ビルドする場合は、Visual Studioの「C++によるデスクトップ開発」またはBuild Toolsを導入して実行します。

```powershell
.\build.ps1 -Test
```

ネイティブコードを変更した場合は、再ビルドした `build/neo-term-host.exe` も一緒にコミットします。その他のビルド成果物はGitの管理対象外です。`test.ps1` はビルド後に実行してください。

Gitからの導入確認だけを行う場合は、次のコマンドが最新コミットを一時ディレクトリへ `use-package :vc` で導入し、同梱EXEでcmdの出力まで確認します。

```powershell
emacs -Q --batch -l tests/check-package.el
```

## 構成とライセンス

`native/` はWin32・ConPTY・libvtermによるCLI制御、`neo-term.el` はEmacs画面、`tests/` は仕様と実機テストです。通信は4MiB上限の長さ付きフレームを使い、入力は固定種別、出力はUTF-8 JSONです。ConPTYのUTF-8/VT変換はC++側で解釈します。

neo-termはGPL-3.0-or-laterです。libvterm **0.3.3** はMITライセンスで、ソースとライセンスを `vendor/libvterm/` に同梱しています。tarballのSHA256は `09156F43DD2128BD347CBEEBE50D9A571D32C64E0CF18D211197946AFF7226E0` です。

設計の背景: [ConPTYの同期I/O](https://learn.microsoft.com/en-us/windows/console/createpseudoconsole)、[Emacsの互換性議論](https://lists.gnu.org/archive/html/bug-gnu-emacs/2024-06/msg00811.html)、[従来APIの非BMP制限](https://github.com/microsoft/terminal/issues/10810)、[公式ConPTYパッケージ](https://www.nuget.org/packages/Microsoft.Windows.Console.ConPTY/1.25.260930003)。
