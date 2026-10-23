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
- SDKの `Actor::getMainHand()` で取得した装備スナップショットからMurderDetectの役割を表示（弓・矢=Sheriff/水色、鉄剣=Murderer/赤）
- MurderDetectはTab表示前から間隔を空けてActorを1件ずつ読み、役割キャッシュを準備（Minecraftオブジェクトのネイティブ読み取りは安全のためゲームスレッド上で実行）
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
Actorの検出・NameTag取得・PlayerList名との照合はBedrockDynamicSDKの `GameHooks` / `GameApi` / `Actor` API経由です。TabList固有のActor HookやRuntimeActorList呼び出しは使いません。SDKが観測したActorのNameTag変更をTabList描画時に反映し、PlayerList名との照合はSDKの `Actor::getName()` が返す正規名で行います。色遷移時のみ `PlayerList name color updated` を記録し、コード・色名・完全一致の有無を表示します。Hive上で実際に紫に描画される `§i` はTabListでも紫として表示します。
`Level::getRuntimeActorList` は実機でSEH例外を起こしたため、現在の経路では呼び出していません。Hookが観測できないActorも含むView系のActor一覧取得は、現状まだ別経路として検証が必要です。
`corner_radius` を `0` にすると角丸を無効にできます。スキンがまだ取得できていない
プレイヤーは、アクセント色の小さな丸で表示します。

`murder_detect` は手持ちの弓/矢を `[Sheriff]`（水色）、鉄剣を `[Murderer]`（赤）で
表示します。Actorごとの手持ち読み出しをtick間に分散し、TabListを開いた瞬間には追加走査しません。
`murder_scan_interval_ticks` は追跡中のActor手持ちを読む間隔（既定1、最小1、最大40 tick）です。
以前の既定値 `20` が残る設定ファイルも `1` として読み込み、その他の明示値は保持します。
Actor一覧は新しいActorのNameTag観測をまとめて反映し、静かな場合のみ5秒ごとに安全更新します。手持ちのNative readは
1 tickにつき最大2人ずつ行い、20 TPS・20人なら約0.5秒で一巡します。確認できたアイテムが前回から変わった時は、debug設定に関係なく
`TabList.log` に `player / item / role / previous` を1行出します。同じアイテムを継続して持っている間は繰り返しません。
アイテムの読み取り失敗や空スロットは「空になった」と断定せず、最後に確認できたアイテムを比較基準として残します。
これにより一時的なNative read失敗で変更ログが欠けたり、同じアイテムを再検知したりするのを防ぎます。
debug=trueではtranslation key等の詳細を追加します。
これによりActor数がスナップショット更新間隔を超えても先頭に戻って後半のActorを取りこぼすことがなく、
TabListを開いた瞬間の一括読み取りも発生しません。間隔を増やすほど負荷は下がりますが、
各Actorの手持ちが再確認されるまでの時間はActor数に応じて長くなります。
一度検知したSheriff／Murdererの表示は、手から武器が離れても最後の検知から5分間維持し、
その間に再検知すればさらに5分延長します。プレイヤーが一時的に抜けても同じ名前の判定は期限まで保持します。
未知のアイテムIDは `TabList.log` に一度だけ候補として記録されます。
`debug` を `true` にするとActor名・表示名・個数・アイテム切替の詳細ログも出ます。

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

