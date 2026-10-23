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
- SDKの `Actor::getMainHand()` で取得した装備スナップショットからMurderDetectの役割を表示（弓・矢・虫眼鏡=Sheriff/水色、`hivemurder:murder_weapon_*`・光線銃・剣系ID=Murderer/赤）
- 手持ちアイテムの表示名をプレイヤー名の横に表示
- `gamemode_check` が有効ならSpectatorを灰色にし `[Spectator]` を表示
- MurderDetectはLevelのRuntimeActorListを順送りし、手持ちのネイティブ読み取りを最大1 Actor/tickに制限（読み取りはゲームスレッド上で実行）。名前が未解決のActorもアイテム確認対象にし、アイテム保持時にだけSDK identityを解決
- Sheriff／Murdererは最後の検知から5分間保持し、期間内に再検知した場合は期限を延長
- Actorの装備コンポーネント／手持ち枠の読み取りで例外が出た対象はスキップし、後続Actorの走査を継続
- TabListの行・色・役割はゲームスレッドでコピーした値だけをワーカーが最大8行ずつ処理し、完成後にまとめてキャッシュを交換。顔画像はtickごとに1人ずつ追加。Tab表示時はキャッシュだけを描画
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
  "show_held_items": true,
  "gamemode_check": true,
  "murder_scan_interval_ms": 400,
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

`murder_detect` は手持ちの弓・矢・`hivemurder:magnifying_glass_item` を `[Sheriff]`（水色）、IDが `hivemurder:murder_weapon_*` または `hivemurder:zapper_ray_gun` のアイテム、バニラの剣・メイスを `[Murderer]`（赤）で
表示します。`show_held_items` は手持ち名をTabListに表示する設定です。`false` にすると表示名の取得・キャッシュ・描画を止め、MurderDetectはItem IDだけを使って継続します。
Actorごとの手持ち読み出しを実時間間隔で分散し、TabListを開いた瞬間には追加走査しません。
`murder_scan_interval_ms` は追跡中のActor手持ちを読む間隔（既定400ms、最小250ms、最大2000ms）です。1回に読むActorは最大1人です。読み取り時刻は前回処理が遅れても追いつき実行せず、次の間隔を現在時刻から数え直します。以前のtick設定 `3` と `20` は400msに、その他のtick設定は50ms単位に変換して範囲内へ収めます。短時間だけ表示されるアイテムを見落とす可能性は上がりますが、ゲームスレッド上の読み取り頻度を下げ、安定性を優先しています。
Actor一覧はLevelのRuntimeActorListから取得し、取得開始の間隔を最短2秒に制限します。一覧の取得と各Actorの検証は別の更新に分け、RuntimeIDの確認を1更新につき最大1件ずつ進めます。処理中は前の完成済み一覧で追跡を続けます。`RuntimeIDComponent` はSDKのバージョン別レイアウトを使うアダプターで読み、名前キャッシュとラウンドロビン順序のキーに使います。これはEnTTのentity IDやActorUniqueIDとは別のBedrock RuntimeIDです。共有Hook runtimeでコピーしたNameTagは最初のActorとPlayerList名の照合に使います。NameTagのgetter呼び出しごとにSDKのActor再検証・全体キャッシュ更新を行わず、変更された短い文字列だけを保存します。キャッシュが使用中ならgetter側は待たず、次の観測に任せます。一度一致した後はActorポインタが変わってもRuntimeIDで名前対応を保持します。RuntimeIDを読めないActorはポインタを予備キーとして使い、同じRuntimeIDの重複Actorは除外します。手持ちの読み取り対象はPlayerList名と照合できたActorに絞り、その他のエンティティの装備を読みません。NameTagが一度も観測されずPlayerList名と結び付いていないActorは対象外です。役割判定は表示名でなくItem IDで行います。手持ちのNative readは
1回の走査につき最大1 Actorに制限し、読み取りはゲームスレッドで行います。新しいアイテムIDの表示名をNative解決する処理も最大500msに1回に制限し、同じIDの名前はキャッシュを使います。Item IDと表示名がコピーされた後の役割判定・差分管理・ログ出力は上限256件のワーカーキューで処理します。キューが満杯なら古いサンプルを破棄し、キューが使用中の場合もゲームスレッドは待ちません。ワーカーを利用できない場合に、役割判定や行コピーをゲームスレッドへ戻す処理も行いません。確認できたアイテムが前回から変わった時は、`TabList.log` に `player` / `name` / `id` / `role` / `previous` を1行出します。同じアイテムを継続して持っている間は繰り返しません。`show_held_items=false` の場合は表示名のNative解決を省き、ログの`name`欄はItem IDになります。
アイテムの読み取り失敗では最後に確認できた表示を保ちます。getterが正常に空を返すかスタック数0を確認できた場合は、素手として前の表示を消します。
一時的なNative read失敗と本当の空スロットを分け、表示の残留と一時的なちらつきを防ぎます。
アイテム名には、1.26.52.3の検証済み `ItemStackBase::getHoverName` から取得した表示名を使い、Item IDごとにキャッシュします。取得できない場合のみItem IDを名前欄に表示します。Minecraftの装飾コードと `[Use]` などの操作ヒントを除いた名前だけを表示・記録します。アイテム名はNative read後にワーカーから行キャッシュへ反映するため、次の全行更新を待たずに表示されます。色遷移の詳細ログは `debug=true` の場合に限ります。MurderDetectのrosterログはプレイヤー人数変更時と10秒ごとに制限し、Actor数の小刻みな変動では毎秒出力しません。

ワーカーはコピー済みの名前・アイテム情報・顔ピクセルを加工し、観測したActorのアドレスは照合キーとしてのみ扱います。Actor/EnTTをワーカーから参照することはありません。MinecraftがActor/EnTTを更新・破棄するタイミングと競合するため、Native readはゲームスレッドで分散して行います。手持ちを読む前にもRuntimeIDの一致を確認し、破棄・再利用されたハンドルの読み取りを避けます。装備・PlayerList・Actor確認・顔画像の取得はスケジューラーが1コールバックにつき最大1種類だけ実行し、仕事の実行間隔を最低25ms空けます。遅れた更新をまとめて取り返す処理はしません。顔画像は最短200ms間隔で1人ずつ取得し、PlayerListからコピーしたUUIDで対象エントリを照合します。顔を探す際に全員分の名前文字列や装飾を繰り返し読み直す処理と、ゲームスレッドでの全行コピーを廃止しました。ゲーム側のUUIDハッシュ方式を推測してネイティブのハッシュ検索を呼ぶこともありません。RuntimeID component hashは1.26.52.3の実行ファイルで確認済みですが、値を64-bit/offset 0として読む配置は候補で、実機での一致確認が必要です。RuntimeIDとPlayerList entry IDの直接照合は行わず、NameTagで得たプレイヤー名をRuntimeIDに関連付けます。1.26.52.3のNative item layoutも候補値のため、実機での装備ID読み取り確認が必要です。
これによりActor数がスナップショット更新間隔を超えても先頭に戻って後半のActorを取りこぼすことがなく、
TabListを開いた瞬間の一括読み取りも発生しません。間隔を増やすほど負荷は下がりますが、
各Actorの手持ちが再確認されるまでの時間はActor数に応じて長くなります。
一度検知したSheriff／Murdererの表示は、手から武器が離れても最後の検知から5分間維持し、
その間に再検知すればさらに5分延長します。プレイヤーが一時的に抜けても同じ名前の判定は期限まで保持します。
Levelが切り替わると、Actor/NameTag観測、プレイヤー一覧キャッシュ、手持ち表示、役割判定、Spectator状態と未処理のワーカーサンプルを破棄します。前のワールドの遅延サンプルは世代番号で無視します。
手持ちアイテムが切り替わった時だけ、表示名とItem ID、個数、役割を `TabList.log` に記録します。色遷移ログなどの詳細は `debug=true` で有効になります。

行の組み立て・役割や顔の引き継ぎ・行スナップショットの公開は常駐ワーカーが行います。毎秒スレッドを作成・joinする処理を廃止し、処理待ちは最新のPlayerList入力1件だけを保持します。ネイティブItemオブジェクトの識別子もワールド内でキャッシュし、同じItemのID文字列を毎回読み直しません。ワーカーは通常より低い優先度で実行します。

ログは上限512行の待機キューへ送り、専用ワーカーが最大250msごとにまとめてファイルへ書き込みます。ゲーム／描画コールバックでファイルのflushやログ書き込み待ちは発生しません。`debug=true` では10秒ごとに `TabList native max us: item=... players=... roster=... face=...` を記録します。値は各取得処理の最大所要時間（マイクロ秒）です。2ms以上かかった取得がある場合はdebug無効でも同じ形式を最大10秒に1回記録し、残る引っかかりの箇所を追跡できます。ゲーム固有のネイティブ関数1回の処理を途中で中断する機能ではありません。

GUIはWindowsにインストールされたYu Gothic、Meiryo、MS Gothicの順で日本語グリフを追加します。該当フォントがOSにない場合は英語フォントへフォールバックします。

PlayerListの各エントリにはActorUniqueIDがありますが、現在のSDK `getEntityId()` が返すECS entity IDとは別の値です。Minecraft 1.26.52.3でActor側のActorUniqueIDを安全に読む方法を確定するまでは、この2種類を比較して名前を推測しません。そのため、Nametagが一度も観測されなかったActorは引き続き`unmatched`になり、該当PlayerList行への役割表示は保留されます。

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

