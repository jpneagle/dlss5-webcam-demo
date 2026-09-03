# DLSSCamDemo

Webカメラの映像を D3D12 + NVIDIA NGX DLSS に通し、実験的な DLSS 5 Neural Rendering (NR)
ランタイムによる人物の質感再構築を A/B 比較するための研究用デモ。

[DLSS Video Player](https://gitlab.com/JessicaNataliaMods/dlss-5-video-player) (MIT) をベースに、
入力を動画ファイルから Webカメラへ置き換えたもの。DLSS/NGX の契約、時間ガイド生成、D3D12 レンダラは
そちら由来で、カメラ入力・人物セグメンテーション・A/B 比較表示・ニューラル出力の色補正を追加している。

**このリポジトリにバイナリは一切含まれない。** NVIDIA のランタイム、ReShade、実験的な Neural
Rendering アドオン、セグメンテーションモデルはすべて利用者が個別に用意する。

## 構成

```text
Webcam (Media Foundation capture)
  -> BGRA フレーム
  -> TemporalGuideGenerator      : 実写フレームから MV / 疑似 Depth / bias マスクを推定
  -> D3D12                       : linear FP16 color + RG16F MV + R32 depth + R8 bias
  -> NGX DLSS SR (raw CreateFeature / EvaluateFeature_C)
       ^ ReShade + renodx-dlss5 addon がここに割り込み Feature 18 (NR) を適用
  -> マスク合成 + A/B split
  -> swapchain
```

DLSS の入出力は既定で **1280x720 入力 -> 2560x1440 出力**。

## 前提

- Windows x64 / Visual Studio 2022 (C++ ワークロード + CMake)
- NVIDIA RTX GPU
- Webカメラ 1台以上
- ONNX Runtime (win-x64) を `external/onnxruntime/` に展開（人物セグメンテーション用）
- 人物マット用モデルを `models/rvm_mobilenetv3_fp32.onnx` に配置

```powershell
# ONNX Runtime
Invoke-WebRequest https://github.com/microsoft/onnxruntime/releases/download/v1.29.0/onnxruntime-win-x64-1.29.0.zip -OutFile ort.zip
Expand-Archive ort.zip .; Move-Item .\onnxruntime-win-x64-1.29.0 .\external\onnxruntime

# Robust Video Matting (mobilenetv3)
Invoke-WebRequest https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3_fp32.onnx -OutFile .\models
vm_mobilenetv3_fp32.onnx
```

## ビルド

```bat
build_windows.bat
```

`external/DLSS` と `external/ffmpeg` は `../dlss5video/player/external/` へのジャンクション。
単独で使う場合は `build_windows.bat` が公式 NVIDIA/DLSS SDK を自動で clone する。

## DLSS 5 ランタイムの配置

実験的な NR ランタイムはこのリポジトリに含めない。ユーザーが自分で用意したものを
`runtime/` へ置くと、ビルド時に exe の隣へ配置される。

```text
runtime/
  nvngx_dlssnr.dll          # Ada 対応の実験ランタイム
  renodx-dlss5.addon64      # ReShade add-on
  sl.*.dll                  # パッケージが要求する場合
```

`runtime/` は `.gitignore` 済み。NVIDIA のプロプライエタリバイナリおよびコミュニティ改変版を
コミット・再配布しないこと。詳細は `../dlss5video/DLSS5_VIDEO_RESEARCH.md` の 20 章を参照。

## 実行

1. ReShade (add-on サポート版) を `DLSSCamDemo.exe` に対して DirectX 12 でインストール
2. `DLSSCamDemo.exe` を起動しカメラを選択（2台以上あれば選択ダイアログ。選択は ini に保存）
3. Home キーで ReShade を開き、RenoDX add-on が有効か確認
4. F6 で NGX feature を作り直すと RenoDX が再フックする
5. A/B split（既定 ON）で左＝DLSS 入力そのもの、右＝DLSS + NR 出力を比較する

### 操作

| キー | 動作 |
|---|---|
| `1`〜`6` | Final / DLSS 入力 / モーションベクトル / Depth / Bias マスク / 合成マスク |
| `S` | A/B split の on/off。境界はマウスドラッグで移動 |
| `K` | 楕円マスク on/off。ホイールでサイズ、Shift+ホイールで強度、Ctrl+ホイールでフェザー、右ドラッグで中心移動 |
| `G` | 人物セグメンテーション on/off（初回のみモデルをロード） |
| `R` | 取り込み解像度モードを巡回（Capture メニューでも選択可）|
| `M` | ミラー反転（表示のみ。DLSS 入力は反転しない） |
| `D` | DLSS on/off |
| `F6` | NGX feature を再作成 |
| `F9` | 静止画一式を `captures/` に保存（`_A_input` / `_B_dlss` / `_C_screen` / `_D_matte`）|
| `F11` / ダブルクリック | フルスクリーン |

ReShade が入力を占有している間は `Ctrl+Alt+D` / `Ctrl+Alt+F6` / `Ctrl+Alt+F7` /
`Ctrl+Alt+F9` / `Ctrl+Alt+K` / `Ctrl+Alt+G` / `Ctrl+Alt+R` を使う。

### 取り込み解像度モード

| モード | DLSS 入力 | 用途 |
|---|---|---|
| 1280x720 native | 1280x720 | そのまま。再構築の余地が小さく変化は控えめ |
| 960x540 | 1280x720 | |
| **640x360 reconstruction demo** | 1280x720 | **デモ向け。差が最も分かりやすい** |
| 320x180 extreme | 1280x720 | カメラが対応していなければ最寄りのモードにフォールバック |

低い解像度ほど入力と出力の隔たりが大きくなり、DLSS 5 が何を再構築しているかが見える。
ゲームで劇的に見えるのは入力が合成画像で隔たりが大きいからで、素の Webカメラ映像は
既に写真なので NR が足せるものが少ない。調査ドキュメント17章が推奨する
「意図的に劣化させて元を Ground Truth にする」評価方法と同じ考え方。

NGX の入力解像度は出力 2560x1440 に対する下限 1280x720 でクランプされるため、
640x360 のときは 1280x720 へ引き伸ばしてから DLSS が再構築する。

```text
NGX input policy: source=640x360 optimal=1707x960 range=1280x720..2560x1440
                  selected=1280x720 output=2560x1440
```

モードを切り替えるとカメラ・D3D12・NGX をすべて作り直すため、数秒間フレームレートが
乱れて `Camera stall detected` が出ることがある。定常状態に入れば 30fps に戻る。
実測（640x360、70秒連続）:

```text
30.0 fps | latency 5 ms | capture drop 40 (起動時のみ) | guide 3.0 ms | render 1.5 ms | resets 1
```

### コマンドライン

```text
--device N          カメラ番号を指定して選択ダイアログを飛ばす
--capture 1280x720  希望する取り込み解像度
--fps 30            希望するフレームレート
--output 2560x1440  DLSS 出力解像度
--dlaa              入力解像度のまま処理（超解像なし）
--performance       DLSS Performance プリセット
```

ログは `DLSSCamDemo.log`（exe と同じ場所）、NGX ログは `ngx_logs/`。
5秒ごとに fps / レイテンシ / ガイド生成時間 / reset 回数 / 露出変動が記録される。

## 実測（RTX 4090 / Driver 616.56, c922 @1280x720 NV12 -> 2560x1440）

DLSS 5 NR + 人物セグメンテーションを両方有効にした状態で 5.5 分連続:

```text
開始   32.2 fps | latency 7 ms | guide 4.5 ms | render 2.0 ms | matte 14 ms
終了   30.0 fps | latency 6 ms | guide 3.9 ms | render 1.5 ms | matte  8 ms
       captured 9765 | capture drop 87 | NGX eval 9619 | matte 9417 | resets 2
```

- 30fps 維持、エラー・再接続なし
- `resets` は起動時と 1 回のみ。自動露出でシーンカットを誤検出していない
- ワーキングセット 519 MB -> 528.7 MB（**+9.7 MB / 5.5分**）。完全にフラットではないので
  長時間運用では監視が要る。調査ドキュメント4章が挙げる DLSS5 のメモリリーク報告に注意

`latency` はフレーム受信から描画サブミットまでのアプリ内時間で、カメラの露光・USB 転送・
ディスプレイ表示は含まない。

## マスク（加工領域の制御）

`final = lerp(original, neural, mask)` を present シェーダで行う。mask は 2 つの積:

- **楕円マスク**（`K`）: 画面空間の固定楕円。フェザー付き。ミラーしても位置は動かない
- **人物マット**（`G`）: Robust Video Matting (mobilenetv3) の alpha。カメラ空間なので
  ミラー時は色と同じ uv で参照する

RVM は動画向けモデルで 4 つの再帰状態をフレーム間で持ち越すため、**時間的に安定した
マットが自前の平滑化なしで得られる**。フレーム単位のセグメンテーションだと境界がちらつき、
それが時間的ニューラルレンダラの履歴を引きずるため、ここは重要。

推論は専用スレッドで 512x288 / `downsample_ratio=0.5`（実測 **約8ms/frame**、CPU 4スレッド）。
レンダループは待たず最新のマットを使うので、推論が遅れてもフレームレートではなく
マットの遅延として劣化する。セグメンテーション有効時も **30fps 維持**。

### 既知のトレードオフ

RenoDX は NR を DLSS の evaluate 内部で適用するため、「NR なしの DLSS 出力」は取り出せない。
したがってマスク外の領域は **原画（720p のバイキュービック拡大）** になり、DLSS の超解像も
効かない。マスク外も超解像を効かせるには Feature 18 の直接呼び出し（下記）が必要。

## NR の効き方について

RenoDX のスライダーは実測で以下が上限で、既定でその値に張り付いている:

```text
NR Intensity             2.00   (上限)
Local Tone Strength      2.00   (上限)
Local Structure Strength 2.00   (上限)
Skin Structure Strength  2.00   (上限)
Color Strength           1.00   (上限)
HDR Transfer Strength    1.00   (上限)
```

`ReShade.ini` に 99 を書いても RenoDX 側でクランプされる。**強度方向に伸びしろはない。**
差を大きくしたい場合は取り込み解像度を下げて再構築の距離を稼ぐ（上記のモード）。

まだスイープしていない、効き方の character を変えうる項目:

- `NR Preset`: Default / #1 / #2 / #3（`DLSSNR.Hint.Render.Preset`、おそらく別モデル）
- `NR Style`: Default / Natural / Cinematic
- `Automatic Mask`: NR 自身がどこに適用するかを決める内部マスク。off にすると顔への
  適用が増える可能性がある
- ガイドの質: 現状 Depth は proxy、MV はほぼ 0。Depth Anything V2 等で実測 Depth を
  入れると NR の判断材料が変わる（調査ドキュメント11章フェーズ2）

なお DLSS 5 NR は生成結果を入力へ拘束する設計で、人物の identity を変えるものではない。
調査ドキュメント4章・13章では顔つきの変化は「守るべき対象」「報告されている破綻」として
扱われている。

## DLSS 5 Neural Rendering の経路

NR には 2 つの経路があり、**動作するのは注入経路**。

### 1. ReShade + RenoDX 注入（動作確認済み）

`runtime/` に `dxgi.dll`(ReShade 6.8 add-on 版) と `renodx-dlss5.addon64` を置くと、
ビルド時に exe の隣へ配置される。インストーラは不要で、この exe だけに閉じた配置になる。

```text
runtime/
  dxgi.dll                  # ReShade 6.8 (add-on サポート版)
  renodx-dlss5.addon64      # DLSS 5 Neural Rendering add-on
  nvngx_dlssnr.dll          # NR ランタイム本体
  ReShade.ini               # 初回のみ配置される。以降は上書きしない
```

起動すると RenoDX が NGX を横取りして NR を適用する。確認できるログ:

```text
[DLSS 5 Neural Rendering] DLSS5 Generic: signed DLSSNR 310.8.0 D3D12 runtime initialized
[DLSS 5 Neural Rendering] DLSS5 Generic: feature 18 created via the signed snippet
                          after DLSS/DLAA for NR input 1280x720 -> output 2560x1440
[DLSS 5 Neural Rendering] DLSS5 Generic: inline feature 18 evaluation succeeded (count=60, ...)
```

NR 有効時も 30fps を維持する。`ReShade.log` に
`Failed to find NVSDK_NGX_D3D12_EvaluateFeature_C` が出るが実害はない。RenoDX は
`EvaluateFeature` 側のフックと inline capture で contract を捕まえている。

**Style / Intensity / Local Tone / Local Structure / Skin Structure / Color Strength /
Auto Mask のスライダーは ReShade オーバーレイ（Home キー）の RenoDX パネル**にある。
値は `ReShade.ini` の `[RenoDX.DLSS5]` に保存されるので、起動前に書き換えてもよい。

```ini
[RenoDX.DLSS5]
NRStyle=1          ; 0=Default 1=Natural 2=Cinematic
NRIntensity=2
NRLocalTone=2
NRLocalStructure=2
NRSkinStructure=2
NRColorStrength=2
NRAutoMask=1
```

#### 静止画 A/B の撮り方

同一フレームで比較するには **1 セッション内**で切り替える。オーバーレイの
"Enable DLSS Neural Rendering" を off にして `F9`、on に戻して `F9`。
add-on を出し入れして 2 回起動する方法だと、その間に被写体が動いてしまい厳密な比較にならない。

ライブでの比較は A/B split（既定 ON）で、左＝DLSS 入力そのもの、右＝DLSS + NR。

### 2. Feature 18 の直接呼び出し（現状ブロック、`--nr-direct` で再現可能）

`DLSSNRBackend` が `nvngx_dlssnr.dll` を直接ロードして NGX feature 18 を叩く実装。
エクスポートは揃っているが、`NVSDK_NGX_D3D12_Init_Ext` がアプリID・APIバージョンの
全組み合わせで `0xBAD00002` (`FAIL_PlatformError`) を返して拒否する。

```text
DLSSNR: runtime loaded, 165840496 bytes, snippet version 0x1360800
DLSSNR: the staged runtime refused direct initialisation:
        [customAppId api1.5]=0xbad00002 [appId0 api1.5]=0xbad00002 ...
```

スニペットは NGX コア (`_nvngx.dll`) から駆動される前提で、直接呼び出しには
コミュニティ実装が持つ互換シムが必要。RenoDX が同じ DLL を「signed snippet」として
使えているのは、NGX コア経由で呼んでいるため。正式な DLSS 5 SDK が公開されるまで
この経路は使えない。実装は残してあり、初期化さえ通れば動く。

パラメータ名は `renodx-dlss5.addon64` のバイナリから確認済みで `DLSSNRBackend.cpp` に
記録してある（`DLSSNR.ColorSubrectWidth` のように Subrect の前にドットが入らない点、
調査ドキュメントに無い `DLSSNR.SkinStructureStrength` の存在を含む）。

## ライセンス

コード部分は `LICENSE` を参照。第三者コンポーネントは `THIRD_PARTY.md`。
