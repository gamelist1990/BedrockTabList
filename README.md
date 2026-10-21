# BedrockTabList

[BedrockDynamicSDK](https://github.com/gamelist1990/BedrockDynamicSDK) 用の、
Minecraft Bedrock 1.26.52.3（Windows x64）向け公開TabListサンプルです。

## 動作

- ワールド内で `Tab` を押している間だけプレイヤー一覧を表示
- 画面上部中央に、半透明の背景・人数・プレイヤー名を表示
- 16人を超える場合は自動的に複数列へ分割
- メニュー、チャット、インベントリでは自動的に非表示
- Minecraftの `Keyboard::feed(InputKey, down)` を直接使用
- WinAPIのキーポーリングやキーボードフックは不使用
- プレイヤー一覧はMinecraftのLevelが保持するネイティブ一覧を読み取り

見た目と基本的な並べ方はLatite ClientのPlayer Listを参考にしつつ、
プレイヤーヘッドや複雑な設定を省いたシンプルなexampleです。

## ビルド

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

成果物は `build\Release\BedrockTabList.dll` です。Minecraft起動後に使用者の
DLLローダーからロードしてください。SDK依存はデフォルトで `main` を使用するため、
通常ビルドではBedrockDynamicSDKの最新版を取得します。

対象Minecraft実行ファイルのハッシュがSDKプロファイルと一致しない場合、ゲーム内部の
レイアウトを読み取らず安全に起動を中止します。

