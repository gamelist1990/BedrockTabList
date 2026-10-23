# BedrockTabList

Windows配布物は `BedrockTabList.dll` 単体です。SDK hook runtimeとMinHookはDLL内に静的同封され、他のSDK製Modとはプロセス内でHook所有権を共有します。
SDK共有ランタイムが他のBedrockDynamicSDK対応ModとMinecraft hookを共用します。

[BedrockDynamicSDK](https://github.com/gamelist1990/BedrockDynamicSDK) 用の、
Minecraft Bedrock 1.26.52.3（Windows x64）向け公開TabListサンプルです。

## 動作

- ワールド内で `Tab` を押している間だけプレイヤー一覧を表示
- 画面上部中央に、半透明の背景・人数・プレイヤー名を表示
- Minecraftのスキンデータからプレイヤーの顔を表示（設定でOFF可能）
- `Actor::getNameTag` のMinecraft `§` 色コードをPlayerListのRawNameへ照合し、設定で色順ソート可能
- SDKの `Actor::getMainHand()` で取得した装備スナップショットからMurderDetectの役割を表示（弓=Sheriff/水色、`hivemurder:` ID=Murderer/赤）
- `gamemode_check` が有効ならSpectatorを灰色にし `[Spectator]` を表示
- MurderDetectはTab表示前からtickごとに最大2人ずつ読み、役割キャッシュを準備（Minecraftオブジェクトのネイティブ読み取りはゲームスレッド上で実行）
- Sheriff／Murdererは最後の検知から5分間保持し、期間内に再検知した場合は期限を延長
- Actorの装備コンポーネント／手持ち枠の読み取りで例外が出た対象はスキップし、後続Actorの走査を継続
- TabListの行・色・役割はゲーム更新中に1秒間隔でキャッシュを作り直し、顔画像はtickごとに1人ずつ追加。Tab表示時はキャッシュだけを描画
- 16人を超える場合は自動的に複数列へ分割
- メニュー、チャット、インベントリでは自動的に非表示
- Minecraftの `Keyboard::feed(InputKey, down)` を直接使用
- WinAPIのキーポーリングやキーボードフックは不使用
- プレイヤー一覧はMinecraftのLevelが保持するネイティブ一覧を読み取り
- `MultiPlayerLevel::_subTick` から現在のLevelを直接取得

見た目と基本的な並べ方はLatite ClientのPlayer Listを参考にしつつ、
シンプルにまとめたexampleです。

## 設定

初回起動時に次のファイルを作成し、保存後はゲーム中に自動反映します。

```text
C:\Users\<ユーザー名>\Documents\PEXData\TabList\config.json
```

```json
{
  "enabled": true,
  "show_faces": true,
  "sort_by_name_color": false,
  "murder_detect": true,
  "gamemode_check": true,
  "murder_scan_interval_ticks": 1,
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

色は `#RRGGBB` または透明度を含む `#RRGGBBAA` 形式です。
`sort_by_name_color` を `true` にすると、装飾コードを除いたPlayerList名を、Actor::getNameTag Hookで観測したActor名に部分一致させ、名前部分に適用されている色順に並べます。同色内は名前順です。
MurderDetectとGamemodeCheckのActor一覧は、SDKアダプターから `Level::getRuntimeActorList` だけで取得します。SDKのNameTag観測キャッシュなど別経路のActorを走査一覧へ混ぜません。関数の完全一致プロファイルがない場合、検知一覧は空のままにして他のActor取得経路へ切り替えません。PlayerList名との照合にはコピー済みNameTagを使います。
対象ビルドでRuntimeActorListのslotとABIを検証し、実際のvtable entryが一致した場合だけActor一覧を取得します。native Actorの読み取りはゲームスレッドで行い、コピーした値の判定だけをワーカーへ送ります。
`corner_radius` を `0` にすると角丸を無効にできます。スキンがまだ取得できていない
プレイヤーは、アクセント色の小さな丸で表示します。

`murder_detect` は手持ちの弓を `[Sheriff]`（水色）、アイテムIDが `hivemurder:` で始まるアイテムを `[Murderer]`（赤）で
表示します。Actorごとの手持ち読み出しをtick間に分散し、TabListを開いた瞬間には追加走査しません。
`murder_scan_interval_ticks` は追跡中のActor手持ちを読む間隔（既定1、最小1、最大40 tick）です。
以前の既定値 `20` が残る設定ファイルも `1` として読み込み、その他の明示値は保持します。
Actor一覧はLevelのRuntimeActorListから1秒ごとに取得します。手持ちのNative readは
1 tickにつき最大2人ずつ行い、読み取り・識別・表示名のコピーはゲームスレッドで行います。Item IDと表示名がコピーされた後の役割判定・差分管理・ログ出力は上限256件のワーカーキューで処理します。キューが満杯なら古いサンプルを破棄します。確認できたアイテムが前回から変わった時は、`TabList.log` に `player / name / id / role / previous` を1行出します。同じアイテムを継続して持っている間は繰り返しません。
アイテムの読み取り失敗や空スロットは「空になった」と断定せず、最後に確認できたアイテムを比較基準として残します。
これにより一時的なNative read失敗で変更ログが欠けたり、同じアイテムを再検知したりするのを防ぎます。
アイテム名には、1.26.52.3の検証済み `ItemStackBase::getHoverName` から取得した表示名を使い、Item IDごとにキャッシュします。取得できない場合のみItem IDを名前欄に表示します。色遷移の詳細ログは `debug=true` の場合に限ります。MurderDetectのActor rosterはワールド参加ごとに一度だけ記録します。

ワーカースレッドへActorポインタやSDKハンドルは渡しません。MinecraftがActor/EnTTを更新・破棄するタイミングと競合するため、Native read自体を別スレッドへ移すことはしていません。1.26.52.3のNative item layoutも現在は候補値のため、実機での装備ID読み取り確認が必要です。
これによりActor数がスナップショット更新間隔を超えても先頭に戻って後半のActorを取りこぼすことがなく、
TabListを開いた瞬間の一括読み取りも発生しません。間隔を増やすほど負荷は下がりますが、
各Actorの手持ちが再確認されるまでの時間はActor数に応じて長くなります。
一度検知したSheriff／Murdererの表示は、手から武器が離れても最後の検知から5分間維持し、
その間に再検知すればさらに5分延長します。プレイヤーが一時的に抜けても同じ名前の判定は期限まで保持します。
手持ちアイテムが切り替わった時だけ、表示名とItem ID、個数、役割を `TabList.log` に記録します。色遷移ログなどの詳細は `debug=true` で有効になります。

`gamemode_check` はActorの `ActorGameTypeComponent` を読み、Spectator (GameType 3) を灰色で表示します。
死亡・不可視フラグの読み取りは対象バージョンでまだ確定できていないため、この設定では判定しません。

同じフォルダの `TabList.log` には、起動結果、Level取得、Tab遷移、プレイヤー一覧の
取得結果だけをコンパクトに記録します。メインメニューでinjectした場合も、Level tickと
ClientInstance経路を継続的に再確認し、ワールド参加後に自動で有効になります。

## ビルド

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

成果物は `build\Release\BedrockTabList.dll` です。Minecraft起動後に使用者の
DLLローダーからロードしてください。SDK依存はデフォルトで `main` を使用するため、
通常ビルドではBedrockDynamicSDKの最新版を取得します。

起動時にはSDKのGitHub `main` からmanifestと実行中Minecraftに対応するバージョン別JSONを
取得します。対象Minecraft実行ファイルのハッシュがプロファイルと一致しない場合、ゲーム
内部のレイアウトを読み取らず安全に起動を中止します。

