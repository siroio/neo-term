# vterm usability implementation plan

**Goal:** WindowsのCLIを維持して、vtermに近い描画・コピー・シェル連携を提供する。

**Architecture:** C++ helperと既存バックエンドを維持する。Emacsは行のマーカーを使って変更行と履歴の増減だけを書き換える。PowerShellのprompt関数はコンソールタイトルに固定JSONデータを載せ、helperがscreenイベントで転送する。端末の文字列からLispコードを実行しない。

**Constraints:** Windows x64、Emacs29.1以降、追加の製品依存なし。ConPTY禁止時のclassicを維持。人が読める改行と名前、コードコメントはWhy notのみ。

- [x] 描画: カーソルだけのイベントは文字を書き換えない。変更行、履歴追加・上限による削除、代替画面、リサイズ、コピー中の更新をテストする。既存実装と同一条件でベンチマークを測る。
- [x] コピー: RETで選択範囲または現在行をコピーして端末へ戻る。プロンプト除外は設定可能。検索・履歴閲覧はコピー中に行う。C-c C-lをvterm同様の履歴消去へ合わせ、画面クリアはC-lとする。
- [x] シェル連携: PowerShell/pwshの通常起動に付属コードをEncodedCommandで適用。独自Command/File起動は改変しない。cwd/prompt/titleを固定データで送り、ローカルの絶対パスだけdefault-directoryへ反映。元のpromptが読む$?とLASTEXITCODEを維持する。
- [x] 入力とコピー連携: OSC52は64KiB/8件の上限と既定無効の設定を設ける。マウスは要求の届いたConPTYだけに送信する。標準ConPTYの要求転送とclassicの未対応は制限として記録する。
- [x] 検証: native契約、CLI統合20件、ERT28件、bytecompile、GUI、差分書式を確認。実際のVimで3バックエンドの入力・保存・終了を確認。独立レビューの3件を修正し、READMEと同梱EXEを更新。

**Review focus:** コピー中に積み上がる出力、履歴上限0、同じ幅でも文字数が異なる日本語、代替画面の復帰、独自PowerShell起動引数と既存prompt関数。

**Review results:** 独立レビューの3件を再現テストで修正。row0再描画で移動するscreen-startマーカーを補正。マウス解放にはevent-endを使用。既存promptをラッパーの最初に呼び出して$?を維持。
