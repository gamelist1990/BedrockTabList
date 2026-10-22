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
`sort_by_name_color` を `true` にすると、Tabを押している間、ゲームtick上でLevelの
runtime Actor一覧を250ms間隔で読み、各Actorの`getNameTag()`から取得した最初の
`§0`〜`§f`色コードを装飾コード除去後のPlayerList名と部分一致で照合します。
一致したプレイヤーをMinecraftの色コード順にグループ化し、同色内は名前順に並べます。
一致するActor名や色コードがない名前は最後に並びます。`§l` などの装飾コードは色判定に使いません。
対象ビルドでruntime Actor一覧の関数が解決できない、または呼び出しに失敗した場合は色ソートを停止します。Actor一覧関数はSolstice 1.21.111由来の候補シグネチャを使って解決するため、SDKプロファイルは実行ファイルの完全一致時だけ候補を許可し、対象ビルドでのシグネチャヒットと呼び出し成功をログで確認してください。
Nametag描画フックや推定vtable slotには依存せず、ワールドのruntime Actor一覧関数を呼んで各Actorの `getNameTag()` を直接読み取ります。ログには解決方式、初回一覧の件数、取得したNameTagのサンプル（最大64件）を `World Actor NameTag: §aPlayerName` の形式で記録します。色ソートへの適用は§色コードがある名前だけです。
PlayerList名とActor名の一致位置で有効な色コードを判定するため、名前より後ろにあるレベル表示やランクの `§` 色はプレイヤー名色として誤採用しません。最初に一致した色の適用は `PlayerList color matched: player=... actor=... order=...` に記録します。
`corner_radius` を `0` にすると角丸を無効にできます。スキンがまだ取得できていない
プレイヤーは、アクセント色の小さな丸で表示します。

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

