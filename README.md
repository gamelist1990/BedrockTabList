# BedrockTabList

Minecraft Bedrock Windows x64向けのSDKサンプルです。1.26.60.5向けの配布物は `BedrockTabList.dll` です。共有Hook runtimeはDLLへ静的に同封します。

revision=6で、プロフィール選択、入力・ゲーム状態・LevelTick・NameTagイベント、プレイヤー一覧、Actor・装備・インベントリの読み取りをSDK APIへ統合しました。Mod側にはゲームのオフセット、関数ABI、EnTT操作、直接のHook登録を残していません。

## 機能

- ワールド内でTabを押している間、名前・人数・顔画像を表示。16人を超えると複数列に分割します。
- メニュー、チャット、インベントリでは自動的に非表示にします。
- NameTagの色コードをプレイヤー名へ照合し、色順に並べ替えできます。
- 手持ちアイテム名、Spectator表示、MurderDetectの役割表示を維持します。
- Sheriff／Murdererは最後の検知から5分間保持します。金・ダイヤの剣とメイスは手持ちだけでのMurderer判定から除外します。

1.26.60.5のプレイヤー一覧・顔画像はSDKのPlayerListパケットサービスを使います。DLL読み込み後にサーバーへ参加してください。受信の開始に失敗した場合は、空一覧のまま準備完了とせず起動失敗をログへ残します。古いキャッシュはSDKのプロフィール選択で検査し、対象EXEと一致する内蔵定義へ切り替えます。

## SDKとの分担

`NativeGameAccess` がゲームスレッドのLevelTick中に装備・RuntimeID・ゲームモードを読み取り、DLL所有の値へコピーします。Modのワーカーと描画処理はコピー済み情報のみを扱います。失敗した読み取りと空スロットを区別し、一時的な読み取り失敗で表示を消しません。ワールド切替時はキャッシュと遅延サンプルを破棄します。

アイテム表示名の戻り値もSDKでコピーし、ゲーム側のメモリ解放関数を使って解放します。ModのC++ランタイムでゲーム所有文字列を破棄しません。名前はIDごとにキャッシュし、新規の名前解決は最短500ms間隔です。表示名を無効にしてもItem IDでの役割判定は継続します。

装備走査は既定150msを一巡の目標とし、1 tickあたり最大12人・750µsの協調的な予算で分散します。人数・負荷によって遅れる場合があり、短時間の持ち替え検知は保証しません。NameTagと一覧を照合できないActorは対象外です。役割判定・行生成は常駐ワーカー、描画は完成済みキャッシュを使います。

## 設定とログ

`Documents/PEXData/TabList/config.json` を自動作成します。保存するとゲーム中に再読込します。ログは同じフォルダーの `TabList.log` です。起動行に `roster revision=6` と記録します。

```json
{
  "enabled": true,
  "show_faces": true,
  "sort_by_name_color": false,
  "murder_detect": true,
  "show_held_items": true,
  "gamemode_check": true,
  "murder_scan_interval_ms": 150,
  "debug": false,
  "background_color": "#0A0D14CC",
  "text_color": "#F2F5FBFF",
  "accent_color": "#64DC9BFF",
  "border_color": "#6E91DCAA",
  "corner_radius": 7.0,
  "row_height": 24.0,
  "header_height": 32.0,
  "face_size": 18.0,
  "top_offset": 22.0,
  "column_gap": 10.0,
  "minimum_column_width": 150.0,
  "maximum_rows": 16,
  "reload_interval_ms": 250
}
```

色は `#RRGGBB` / `#RRGGBBAA` です。`show_held_items=false` で表示名の取得と描画を止めます。`debug=true` で詳細ログを有効にします。

## ビルドと確認範囲

Windows x64 MSVC Releaseでビルドします。SDKの `BEDROCK_SDK_ENABLE_NATIVE_CLIENT` を有効にし、`bedrock::client` と型付き `GameHooks` を使用します。EXEが一致しない場合は起動を中止します。

1.26.52.3ではSDK内の旧PlayerList読み取り経路を使用します。この版にはゲーム側の文字列解放定義をまだ追加していないため、アイテム表示名はIDにフォールバックします。

revision=6のゲーム内表示と他Mod併用時の実機確認は未完了です。プロフィール内でABIが候補の操作は、今回の移行だけで検証済みには変更していません。
