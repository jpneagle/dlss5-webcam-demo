> [!IMPORTANT]
> Due to the weakening Japanese yen, it has become increasingly difficult to
> afford not only the equipment needed to continue development, but even basic
> daily necessities. If you find this project useful, please consider supporting
> its development. Your help would mean a great deal.
>
> [Support this project on Buy Me a Coffee](https://buymeacoffee.com/eaglejp2b)

# DLSSCamDemo

Webカメラの映像を D3D12 + NVIDIA NGX DLSS に通し、実験的な DLSS 5 Neural Rendering (NR)
ランタイムによる人物の質感再構築を A/B 比較するための研究用デモ。

**NR を動かすことが目的のデモ**であり、ReShade と NR ランタイムは必須。
それらが無くても DLSS 超解像だけで起動はするが、それは依存が欠けたときの縮退動作にすぎない。

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
       ^ ReShade + NR add-on がここに割り込み Feature 18 (NR) を適用（任意）
  -> マスク合成 + A/B split
  -> swapchain
```

DLSS の入出力は既定で **1280x720 入力 -> 2560x1440 出力**。

## 必要なもの

このリポジトリはソースのみ。バイナリ・モデル・ランタイムは一切含まれないので、
以下は利用者が別途用意する。**合計で約 1.4 GB。**

### 必須

DLSS 5 Neural Rendering を動かすことがこのデモの目的なので、NR 側も含めてすべて必須。

| 置き場所 | 中身 | サイズ | 入手 |
|---|---|---|---|
| — | Windows x64 / Visual Studio 2022（C++ ワークロード + CMake）| — | Microsoft |
| — | NVIDIA RTX GPU と Webカメラ | — | — |
| `external/DLSS/` | NVIDIA DLSS SDK。ヘッダ・`nvsdk_ngx_d.lib`・`nvngx_dlss.dll` | 約760MB | `build_windows.bat` が自動 clone |
| `external/onnxruntime/` | ONNX Runtime win-x64。ヘッダ・`.lib`・`onnxruntime.dll` | 展開後 約410MB | 下記コマンド（**手動**）|
| `runtime/dxgi.dll` | ReShade 6.8 **add-on サポート版**。exe の隣に置くプロキシ方式でインストーラ不要 | 5.6MB | [reshade.me](https://reshade.me/) |
| `runtime/nvngx_dlssnr.dll` | NVIDIA プロプライエタリの NR ランタイム。RTX 40 系で動くのはコミュニティ改変版で、NVIDIA の Authenticode ハッシュとは一致しない | **166MB** | 非公式配布 |
| `runtime/` | NR コンシューマ（下記のいずれか一方）| — | Discord |

`external/onnxruntime` が無いと **CMake が構成段階で失敗する**。

```powershell
Invoke-WebRequest https://github.com/microsoft/onnxruntime/releases/download/v1.29.0/onnxruntime-win-x64-1.29.0.zip -OutFile ort.zip
Expand-Archive ort.zip .
Move-Item .\onnxruntime-win-x64-1.29.0 .\external\onnxruntime
```

#### NR コンシューマ

NGX を横取りして feature 18 を実行する ReShade add-on。**どちらか一方だけ**を置く。
2つ同時に置くと競合し、Deep Fried Chicken 側は明示的にもう一方の除去を要求する。

- **Deep Fried Chicken**（推奨・本デモの検証環境）— `deep-fried-chicken.addon64` /
  `deep-fried-chicken-nvngx.dll` / `deep-fried-chicken.cfg` の3点。作者が Discord で配布。
  DLSS SR の後に NR を 1〜30 パス重ねられる。検証は v1.4.8-alpha
- **RenoDX `#DLSS5` build** — `renodx-dlss5.addon64`。RenoDX Discord の `#DLSS5` チャンネル

`runtime/` に置くと、ビルド時に exe の隣へ自動配置される。

### 任意

| 置き場所 | 中身 | サイズ | ライセンス |
|---|---|---|---|
| `models/rvm_mobilenetv3_fp32.onnx` | Robust Video Matting の人物マットモデル | 14.3MB | **GPL-3.0** |

```powershell
Invoke-WebRequest https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3_fp32.onnx -OutFile .\models
vm_mobilenetv3_fp32.onnx
```

無い場合は `G` キーが効かず、マスクは楕円のみになる。それ以外は通常どおり動作する。

### NR が無い場合の縮退動作

ReShade と NR ランタイムが無くてもアプリは起動し、DLSS Super Resolution
（1280x720 → 2560x1440）、A/B 比較、人物マスク、静止画保存は 30fps で動作する。
ただし **Neural Rendering は一切走らない**ので、デモとしては成立しない。
依存が欠けたときに落ちずに縮退するというだけで、想定する使い方ではない。

### 再配布について

`runtime/` と `models/` は `.gitignore` 済み。NVIDIA のプロプライエタリバイナリ、
コミュニティ改変版、Deep Fried Chicken のアーカイブはいずれも再配布しないこと。
Deep Fried Chicken のライセンスは、アーカイブの再ホストではなく作者の公式リンクの共有を求めている。

## ビルド

```bat
build_windows.bat
```

`build_windows.bat` は NVIDIA/DLSS SDK を `external/DLSS` へ自動 clone する。
ONNX Runtime は上記の手順で手動配置しておくこと。

## 実行

1. `runtime/` に ReShade・NR ランタイム・NR コンシューマを置いてビルド（exe の隣へ自動配置される）
2. `config/ReShade.ini.template` がビルド時に `ReShade.ini` として exe の隣に配置される
   （Deep Fried Chicken に必要な `LoadFromDllMain` が入っている。ビルド時に自動配置もされる）
3. `DLSSCamDemo.exe` を起動
4. `DLSSCamDemo.log` に `RAW NGX EvaluateFeature_C SUCCESS`、
   `deep-fried-chicken.log` に `standalone neural frame succeeded` が出れば NR が乗っている
5. Home キーで ReShade オーバーレイを開くと、パス数と各パスのパラメータを実行中に変更できる
6. `S` を押して並置モードにすると、左＝適用前 / 右＝適用後を同一フレームで比較できる

カメラは 2台以上あれば起動時に選択ダイアログが出る（選択は ini に保存）。

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

### 1. ReShade + NR コンシューマの注入（動作確認済み）

`runtime/` に置いたものがビルド時に exe の隣へ配置される。インストーラは不要で、
この exe だけに閉じた配置になる。

```text
runtime/
  dxgi.dll                        # ReShade 6.8 (add-on サポート版)
  nvngx_dlssnr.dll                # NR ランタイム本体
  deep-fried-chicken.addon64      # NR コンシューマ
  deep-fried-chicken-nvngx.dll    #   ブリッジ
  deep-fried-chicken.cfg          #   設定。初回のみ配置され以降は上書きしない
```

`config/ReShade.ini.template` が `ReShade.ini` として配置される。Deep Fried Chicken は
DllMain からロードされる必要があるため `LoadFromDllMain` が入っている。

#### Deep Fried Chicken（推奨）

このアプリは NGX を直接叩いてネイティブ DLSS SR を持つので、Chicken の **standalone 経路**が
そのまま使える。DLSS5-Feeder のような外部のガイド供給は不要。

```text
standalone direct DLSSNR initialized through isolated bridge:
  runtime=<exe dir>/nvngx_dlssnr.dll  bridge=<exe dir>/deep-fried-chicken-nvngx.dll
create #1 feature 18: A=0x00000001 Success; MODE=TWO_PASSES requested=2
standalone feature 18 created: game=1280x720->2560x1440 neural=2560x1440->2560x1440
evaluate #1 pass 1 handle=... Color=game       Depth=game MVec=game
evaluate #1 pass 2 handle=... Color=previous-pass Depth=game MVec=game
standalone neural frame succeeded: count=3
```

`Depth=game MVec=game` は、本アプリが生成した推定 Depth とモーションベクトルが
実際に使われていることを示す。

**`deep-fried-chicken.cfg` の `layers` が最大の効きどころ**（1〜30）。DLSS SR の後に NR を
数珠つなぎで重ね、各パスの入力が前パスの出力になる。RTX 4090 / 2560x1440 での実測:

| layers | fps | 単純拡大比の構造量 |
|---|---|---|
| 1 | 30.0 | 1.29x |
| **2** | **30.0** | **1.41x** |
| 4 | 1.2（実用外）| 未測定 |

主なパラメータ:

```ini
layers=2
preserve_native_tone_color=0   ; 1 にすると色調を原画に固定するが、clean_fry と併用が前提
clean_fry_enabled=0            ; 多段用のクリーンアップ。色転びは消えるが構造も削れる
clean_fry_cleanup_strength=0.650
neural_work_percent=100
layer_N_nr_preset / nr_style / intensity / local_tone / local_structure / skin_structure
```

パス数と各パスのパラメータは **ReShade オーバーレイ（Home キー）の Chicken タブで実行中に
変更できる**。同一フレームでの A/B はここでパス数を切り替えて `F9` を2回押すのが確実。

`preserve_native_tone_color=1` を `clean_fry_enabled=0` で使うと 15fps 前後まで落ちて
不安定になる（2回とも再現）。この2つはセットで使う前提と思われる。

#### RenoDX `#DLSS5` build（代替）

`renodx-dlss5.addon64` を単独で置く。Chicken とは併用不可。
Style / Intensity / Local Tone / Local Structure / Skin Structure / Color Strength /
Auto Mask のスライダーが ReShade オーバーレイの RenoDX パネルに出る。値は
`ReShade.ini` の `[RenoDX.DLSS5]` に保存される。

実測で確認した上限（`ReShade.ini` に大きい値を書いてもクランプされる）:

```text
NR Intensity / Local Tone / Local Structure / Skin Structure : 2.00
Color Strength / HDR Transfer Strength                       : 1.00
```

`ReShade.log` に `Failed to find NVSDK_NGX_D3D12_EvaluateFeature_C` が出るが実害はない。
RenoDX は `EvaluateFeature` 側のフックと inline capture で contract を捕まえている。

#### 色転びについて

NR は出力の色温度を寒色側へ、輝度を上へずらす。実測値（入力との差）:

| 条件 | R−B | 輝度 |
|---|---|---|
| 1パス | −14.7 | +5.4 |
| 2パス | −15.9 | +10.1 |

これは NR 固有の性質でパス数のせいではない。本アプリは present パス（NR より後段）で
ニューラル側だけに per-channel ゲインをかけて補正する。`[` `]` で色温度、`0` でリセット。
`DLSSCamDemo.ini` の `[neural] warmth` / `exposure` に保存される。
**A/B 比較の参照側にはこの補正がかからない**ので、比較が歪まない。

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
