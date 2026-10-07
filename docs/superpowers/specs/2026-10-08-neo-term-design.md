# neo-term 設計案

## 目的と合意事項

Windows GUI版Emacsのバッファ内で、Windowsのシェルと対話型CLIを操作する独立パッケージを作る。配置先は `E:\neo-emacs\neo-term`。C++の仲介プロセスが、セッション起動時に利用可能なバックエンドを選ぶ。

ConPTYを候補に含めることはユーザー確認済み。winptyは使用しない。既存の `emacs.d`、`neo-ime`、`neo-git` の実装は初期開発では変更しない。

初版はWindows 10 1809以降・x64、現在のWindows版Emacs 31.1で検証する。ほかのアーキテクチャへの配布は初版の対象外。最初の検証対象はcmdとPowerShellで、任意のCLIの完全互換を約束しない。

## 構成

```text
neo-term.el
    ↕ パイプ通信
neo-term-host.exe（セッションごとに1プロセス）
    ├─ 同梱ConPTYランタイム
    ├─ Windows標準ConPTY
    └─ 従来Console API
            ↕
         シェル・CLI
```

Emacs Lispはバッファ表示、キーバインド、コピー、ウィンドウサイズ通知を担当する。C++はCLIの起動、入出力、画面状態、リサイズ、終了処理を担当する。Emacs本体へのパッチは加えない。

仲介EXEとの通信はEmacs標準の非同期プロセス通信を使う。Emacs用DLLと共有メモリは初版では追加しない。ConPTY接続側の同期I/Oは専用スレッドで扱い、Emacsやヘルパーの制御処理を待たせない。

## バックエンド選択

利用者は `auto`、`bundled-conpty`、`system-conpty`、`classic` を指定できる。ConPTYを自動選択から除外する設定も用意する。

`auto` の優先順は、同梱ConPTY、システムConPTY、従来Console API。同梱版がない場合は次へ進む。バックエンド指定時は指定した方式だけを試し、失敗を明示する。

CPUアーキテクチャ、必要なDLLとAPIの存在、バックエンド初期化の成功を確認する。OSのバージョンだけでは選ばない。DLLは同梱の決められた場所またはWindowsのシステムディレクトリから読み込み、カレントディレクトリや任意のPATHを検索しない。

バックエンド初期化は対象CLIの起動前に行う。失敗時は作成したリソースを解放してから次へ進む。CLIの起動を試みた後には自動フォールバックしない。実行中の切り替えや自動再起動もしない。

選択したバックエンド、ランタイム情報、選択理由と初期化エラーをEmacsから確認できる。可用性の検出はCLI互換性の保証ではなく、手動指定で問題のある方式を避けられるようにする。

## 画面と入力

ConPTYのUTF-8・VT出力は既存のlibvtermで解釈する。端末パーサーを独自実装しない。従来Console APIでは非表示コンソールの文字セル、属性、カーソル、表示領域を取得する。

両方式の結果を共通の画面更新形式にしてEmacsへ送る。行・セルの位置、文字、表示幅、前景色・背景色、文字属性、カーソル位置と可視状態を含める。ConPTY側では端末応答や入力モードもlibvtermの機能に接続する。

Emacsは文字入力と修飾キーを含むキー操作を仲介EXEへ送る。ConPTY側では端末の状態に対応する入力シーケンスへ、従来方式ではWindowsの入力イベントへ変換する。日本語はEmacs側で確定した文字列を送る。Ctrl+Cは通常の文字入力と区別し、バックエンドとコンソールの入力モードに対応させる。

初版で扱うのは文字入力、Enter、Backspace、Tab、Escape、矢印、Home/End、Delete、PageUp/PageDown、主要な修飾キー、Ctrl+C、貼り付け、リサイズ、画面履歴のコピー。マウス入力、画像プロトコル、シェル連携による作業ディレクトリ追跡は対象外。

従来方式で取得できないTrue Color等の属性は復元しない。ポーリングの間に消えた出力を含め、完全な履歴保存は保証しない。ConPTYと従来方式の機能差は利用者が確認できるようにする。

実機検証で従来Console APIが非BMP文字をU+FFFDへ変換することを確認した。APIから失われた絵文字の復元は初版では保証せず、日本語BMP文字とConPTY側の絵文字を検証する。

## 通信と終了処理

通信はバージョンと長さを持つフレームにし、途中まで届いたデータと複数フレームの連結を正しく扱う。画面データと診断出力を混ぜず、サイズ上限を検証する。制御入力、端末応答、貼り付けの書き込み順を保持する。

出力の受信とEmacs描画を分離し、画面差分はまとめて描画する。待ち行列に上限を設け、過負荷時もメモリ使用量を無制限に増やさない。フレームを部分的に破棄せず、再同期が必要なら完全な画面状態を送る。

起動は実行ファイルと引数を区別して `CreateProcessW` で行う。必要のないシェルラッパーは挟まない。既定はpwshがあればpwsh、なければWindows PowerShell。cmdも明示選択できる。

バッファ破棄、CLI終了、通信断、Emacs終了では、ブロック中のI/Oを解除し、所有するプロセスとハンドルを解放する。Windows Job Objectによる子プロセス管理を使い、管理できない場合は起動失敗として理由を示す。終了直前の出力を可能な範囲で回収し、終了コードを表示する。

## 検証と完了条件

- バックエンドの自動選択、明示指定、ConPTY禁止、初期化失敗を検証する。
- 実際のcmdとPowerShellで起動、終了、非ASCIIのパスと日本語入出力を確認する。
- 画面の上書き、色、カーソル移動、全角文字、代替画面、リサイズを確認する。
- キー入力、Ctrl+C、貼り付けを端末モード別に検証する。
- 分割された通信フレーム、大量出力、通信断、バッファ破棄で破損・停止・プロセス残留がないことを確認する。
- GUI Emacsでシェル操作と最低1つのWindows用TUIを実機確認する。検証用TUIには自己完結した小さなプログラムを用意する。
- 各バックエンドの検証結果と既知の制限を記録する。未検証の方式を動作確認済みと表示しない。

テストはWhat、実装はHow、コミットログはWhyを記述する。コードコメントは必要なWhy notだけとし、挙動を説明するコメントを追加しない。

## 参照

- [ConPTYの作成と同期I/Oの制約](https://learn.microsoft.com/en-us/windows/console/createpseudoconsole)
- [実行時のDLL読み込み](https://learn.microsoft.com/en-us/windows/win32/dlls/using-run-time-dynamic-linking)
- [従来コンソールの画面取得](https://learn.microsoft.com/en-us/windows/console/readconsoleoutput)
- [libvterm](https://www.leonerd.org.uk/code/libvterm/)
- [GhostelのWindows対応](https://github.com/dakra/ghostel)
- [EmacsへのConPTY導入時の互換性議論](https://lists.gnu.org/archive/html/bug-gnu-emacs/2024-06/msg00811.html)
