> [!IMPORTANT]
> Due to the weakening Japanese yen, it has become increasingly difficult to
> afford not only the equipment needed to continue development, but even basic
> daily necessities. If you find this project useful, please consider supporting
> its development. Your help would mean a great deal.
>
> [Support this project on Buy Me a Coffee](https://buymeacoffee.com/eaglejp2b)

# DLSSCamDemo

**English** | [日本語](#日本語)

A research demo that feeds a webcam through D3D12 + NVIDIA NGX DLSS and compares, side by
side, what an experimental DLSS 5 Neural Rendering (NR) runtime does to a person's skin,
hair and fabric.

**Running NR is the point of this demo**, so ReShade and an NR runtime are required. The
app still starts without them and gives you plain DLSS Super Resolution, but that is what
happens when a dependency is missing, not the intended use.

Derived from [DLSS Video Player](https://gitlab.com/JessicaNataliaMods/dlss-5-video-player)
(MIT). The DLSS/NGX contract, the temporal guide generation and the D3D12 renderer come
from there. This project replaces the file decoder with a live camera and adds person
segmentation, the A/B comparison views, and colour correction of the neural output.

**No binaries are included in this repository.** The NVIDIA runtime, ReShade, the
experimental Neural Rendering add-ons and the segmentation model are all obtained
separately by the user.

## How it works

```text
Webcam (Media Foundation capture)
  -> BGRA frames
  -> TemporalGuideGenerator   : estimates motion vectors, a depth proxy and a bias
                                mask from the camera frames themselves
  -> D3D12                    : linear FP16 colour + RG16F MV + R32 depth + R8 bias
  -> NGX DLSS SR (raw CreateFeature / EvaluateFeature_C)
       ^ ReShade + an NR add-on hooks in here and runs Feature 18
  -> mask composite + A/B comparison
  -> swapchain
```

A normal camera frame carries no motion vectors and no depth buffer, so the demo
reconstructs both from image history before handing them to DLSS. The NR consumer's log
confirms they are actually used: `Depth=game MVec=game`.

DLSS runs **1280x720 in -> 2560x1440 out** by default.

## What you need to supply

This repository is source only. Nothing below is included, so roughly **1.4 GB** has to be
obtained separately.

### Required

| Location | What | Size | Where |
|---|---|---|---|
| — | Windows x64, Visual Studio 2022 (C++ workload + CMake) | — | Microsoft |
| — | An NVIDIA RTX GPU and a webcam | — | — |
| `external/DLSS/` | NVIDIA DLSS SDK: headers, `nvsdk_ngx_d.lib`, `nvngx_dlss.dll` | ~760 MB | cloned automatically by `build_windows.bat` |
| `external/onnxruntime/` | ONNX Runtime win-x64: headers, `.lib`, `onnxruntime.dll` | ~410 MB extracted | **manual**, see below |
| `runtime/dxgi.dll` | ReShade 6.8 **with add-on support**. Proxy DLL beside the exe; no installer | 5.6 MB | [reshade.me](https://reshade.me/) |
| `runtime/nvngx_dlssnr.dll` | NVIDIA's proprietary NR runtime. The builds that work on RTX 40-series are community-modified and do not match NVIDIA's Authenticode hash | **166 MB** | unofficial distribution |
| `runtime/` | An NR consumer, one of the two below | — | Discord |

**CMake fails at configure time** if `external/onnxruntime` is missing.

```powershell
Invoke-WebRequest https://github.com/microsoft/onnxruntime/releases/download/v1.29.0/onnxruntime-win-x64-1.29.0.zip -OutFile ort.zip
Expand-Archive ort.zip .
Move-Item ./onnxruntime-win-x64-1.29.0 ./external/onnxruntime
```

#### NR consumers

A ReShade add-on that intercepts NGX and runs Feature 18. Install **only one**. Two at
once conflict, and Deep Fried Chicken explicitly asks you to remove the other.

- **Deep Fried Chicken** (recommended; what this demo was verified against) —
  `deep-fried-chicken.addon64`, `deep-fried-chicken-nvngx.dll` and
  `deep-fried-chicken.cfg`. Distributed by its author through Discord. Stacks 1 to 30 NR
  passes after DLSS SR. Verified here with v1.4.8-alpha.
- **RenoDX `#DLSS5` build** — `renodx-dlss5.addon64`, from the `#DLSS5` channel of the
  RenoDX Discord.

Anything placed in `runtime/` is copied next to the executable at build time.

### Optional

| Location | What | Size | Licence |
|---|---|---|---|
| `models/rvm_mobilenetv3_fp32.onnx` | Robust Video Matting person matte model | 14.3 MB | **GPL-3.0** |

```powershell
Invoke-WebRequest https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3_fp32.onnx -OutFile ./models/rvm_mobilenetv3_fp32.onnx
```

Without it the `G` key does nothing and the mask falls back to the fixed oval. Everything
else works normally.

### Without an NR runtime

The app still starts and runs DLSS Super Resolution (1280x720 -> 2560x1440), the A/B
comparison, the person mask and still capture, all at 30 fps. But **no Neural Rendering
happens at all**, so it does not demonstrate anything. This is graceful degradation when a
dependency is missing, not a supported configuration.

### Redistribution

`runtime/` and `models/` are gitignored. Do not redistribute NVIDIA's proprietary
binaries, the community-modified builds, or the Deep Fried Chicken archive. Deep Fried
Chicken's licence asks that you share the author's official release link rather than
rehost the archive.

## Building

```bat
build_windows.bat
```

It clones the NVIDIA/DLSS SDK into `external/DLSS`. Place ONNX Runtime manually first.

## Running

1. Put ReShade, the NR runtime and one NR consumer in `runtime/`, then build. They are
   copied next to the executable.
2. `config/ReShade.ini.template` is staged as `ReShade.ini` at build time. It carries the
   `LoadFromDllMain` entry Deep Fried Chicken needs. An existing file is never overwritten.
3. Launch `DLSSCamDemo.exe`.
4. NR is running when `DLSSCamDemo.log` shows `RAW NGX EvaluateFeature_C SUCCESS` and
   `deep-fried-chicken.log` shows `standalone neural frame succeeded`.
5. Press Home for the ReShade overlay: pass count and per-pass parameters can be changed
   while it runs.
6. Press `S` twice for side-by-side: before on the left, after on the right, same frame.

If more than one camera is present, a picker appears at startup. The choice is saved.

### Controls

| Key | Action |
|---|---|
| `1`–`6` | Final / DLSS input / motion vectors / depth / bias mask / combined mask |
| `S` | Cycle A/B comparison: wipe -> side-by-side -> off. Drag to move the wipe boundary |
| `K` | Oval mask on/off. Wheel resizes, Shift+wheel sets strength, Ctrl+wheel softens the edge, right-drag moves the centre |
| `G` | Person segmentation on/off (loads the model on first use) |
| `R` | Cycle capture resolution (also in the Capture menu) |
| `[` `]` | Neural white balance, cooler / warmer. `0` resets |
| `M` | Mirror the view only; the DLSS input is never flipped |
| `D` | DLSS on/off |
| `F6` | Recreate the NGX feature |
| `F9` | Save a still set to `captures/`: `_A_input`, `_B_dlss`, `_C_screen`, `_D_matte` |
| `F11` / double click | Fullscreen |

While ReShade owns the input, use the `Ctrl+Alt+` variants: `D`, `F6`, `F7`, `F9`, `K`,
`G`, `R`, `[`, `]`.

### Comparison views

- **Wipe** — one image, left half untouched, right half reconstructed. Best for spotting
  that something changed.
- **Side-by-side** — the whole frame twice, each half a full-height centred crop so the
  pair fills the window with no letterboxing. The only mode where the same feature can be
  compared before and after at the same instant.

The reference half never receives the neural white balance, so the comparison is not
skewed by a correction applied to one side only.

### Capture resolution modes

| Mode | DLSS input | Purpose |
|---|---|---|
| 1280x720 native | 1280x720 | As-is. Little room to reconstruct, so the change is subtle |
| 960x540 | 1280x720 | |
| **640x360 reconstruction demo** | 1280x720 | **Best for demos; the difference is clearest** |
| 320x180 extreme | 1280x720 | Falls back to the nearest mode the camera actually offers |

The lower the capture resolution, the wider the gap between input and output, and the more
visible what DLSS 5 is reconstructing. The reason it looks dramatic in games is that the
input is synthetic and that gap is enormous; a raw webcam frame is already a photograph, so
there is far less for NR to add.

NGX clamps its input to the 1280x720 lower bound of the dynamic range for a 2560x1440
output, so at 640x360 the frame is stretched to 1280x720 first and DLSS reconstructs from
there.

```text
NGX input policy: source=640x360 optimal=1707x960 range=1280x720..2560x1440
                  selected=1280x720 output=2560x1440
```

Switching modes rebuilds the camera, D3D12 and NGX, so the frame rate is disturbed for a
few seconds and `Camera stall detected` may appear. Steady state returns to 30 fps.
Measured over 70 s at 640x360:

```text
30.0 fps | latency 5 ms | capture drop 40 (startup only) | guide 3.0 ms | render 1.5 ms | resets 1
```

### Command line

```text
--device N          pick a camera by index and skip the picker
--capture 1280x720  requested capture resolution
--fps 30            requested frame rate
--output 2560x1440  DLSS output resolution
--dlaa              process at input resolution, no upscaling
--performance       DLSS Performance preset
--nr-direct         probe the unsupported direct Feature 18 path
```

`DLSSCamDemo.log` sits next to the executable, NGX logs go to `ngx_logs/`. A stats line is
written every 5 s with frame rate, latency, guide time, reset count and exposure drift.

## Measurements (RTX 4090, driver 616.56, c922 at 1280x720 NV12 -> 2560x1440)

5.5 minutes with both DLSS 5 NR and person segmentation enabled:

```text
start  32.2 fps | latency 7 ms | guide 4.5 ms | render 2.0 ms | matte 14 ms
end    30.0 fps | latency 6 ms | guide 3.9 ms | render 1.5 ms | matte  8 ms
       captured 9765 | capture drop 87 | NGX eval 9619 | matte 9417 | resets 2
```

- 30 fps held, no errors and no reconnects.
- Only two `resets`, one of them at startup. The camera's auto exposure never tripped the
  scene-cut detector.
- Working set 519 MB -> 528.7 MB, so **+9.7 MB over 5.5 minutes**. Not flat; worth
  watching over long sessions given the memory-leak reports around DLSS 5.

`latency` is time inside the app from frame arrival to render submit. It excludes camera
exposure, USB transfer and display.

## The mask

`final = lerp(original, neural, mask)` in the presentation shader, where the mask is the
product of two terms:

- **Oval** (`K`) — a feathered ellipse in screen space. Mirroring does not move it.
- **Person matte** (`G`) — the alpha from Robust Video Matting (mobilenetv3). It lives in
  camera space, so it is sampled with the same uv as the colour when the view is mirrored.

RVM is a video model carrying four recurrent states between frames, so **the matte is
temporally stable without any smoothing of our own**. That matters here: a per-frame
segmentation would flicker at the boundary, and a temporal neural renderer drags that
flicker into its history.

Inference runs on its own thread at 512x288 with `downsample_ratio=0.5`, measured at
**about 8 ms per frame** on 4 CPU threads. The render loop never waits for it and uses the
newest matte available, so a slow model degrades mask latency rather than frame rate.
30 fps is held with segmentation on.

### Known trade-off

The NR consumer applies Neural Rendering inside the DLSS evaluate, so there is no way to
obtain "the DLSS output without NR". Outside the mask you therefore get the **original
frame bicubically upscaled**, without DLSS super resolution. Fixing that needs the direct
Feature 18 path described below.

## Getting a bigger difference

Strength is already at its ceiling. Writing 99 into RenoDX's sliders clamps to:

```text
NR Intensity / Local Tone / Local Structure / Skin Structure : 2.00
Color Strength / HDR Transfer Strength                       : 1.00
```

Two things actually move the needle:

1. **Lower the capture resolution** so there is more to reconstruct (see the modes above).
2. **Stack NR passes** with Deep Fried Chicken's `layers`. Each pass takes the previous
   pass's output as its input. Measured on an RTX 4090 at 2560x1440:

| layers | fps | structure vs plain bicubic upscale |
|---|---|---|
| 1 | 30.0 | 1.29x |
| **2** | **30.0** | **1.41x** |
| 4 | 1.2 (unusable) | not measured |

Still unswept, and likely to change the character rather than the amount: `NR Preset`
(Default / #1 / #2 / #3, i.e. `DLSSNR.Hint.Render.Preset`), `NR Style`
(Default / Natural / Cinematic), and `Automatic Mask`, which is NR's own decision about
where to apply itself.

Note that DLSS 5 NR is designed to constrain its output to the input. It does not change a
person's identity; facial change is a failure mode to guard against, not a feature.

### Colour cast

NR shifts the output cooler and brighter. Measured against the input:

| Condition | R−B | Luminance |
|---|---|---|
| 1 pass | −14.7 | +5.4 |
| 2 passes | −15.9 | +10.1 |

This is inherent to NR, not a consequence of stacking. The demo corrects it with a
per-channel gain in the presentation pass, **after** DLSS, so nothing the neural renderer
sees is touched and temporal history is unaffected. `[` and `]` adjust it, `0` resets, and
the values are saved to `[neural] warmth` / `exposure` in `DLSSCamDemo.ini`. The reference
side of an A/B view never receives it.

The correction costs no detail: applying the same gain numerically to an uncorrected
capture left the structure metric unchanged (1.30x -> 1.32x).

## The two NR paths

### 1. ReShade + an NR consumer (works)

```text
runtime/
  dxgi.dll                        # ReShade 6.8 with add-on support
  nvngx_dlssnr.dll                # the NR runtime itself
  deep-fried-chicken.addon64      # NR consumer
  deep-fried-chicken-nvngx.dll    #   bridge
  deep-fried-chicken.cfg          #   settings; staged once, never overwritten
```

Because this app calls NGX directly and has native DLSS SR, Deep Fried Chicken's
**standalone path** applies. No external guide feeder such as DLSS5-Feeder is needed.

```text
standalone direct DLSSNR initialized through isolated bridge:
  runtime=<exe dir>/nvngx_dlssnr.dll  bridge=<exe dir>/deep-fried-chicken-nvngx.dll
create #1 feature 18: A=0x00000001 Success; MODE=TWO_PASSES requested=2
standalone feature 18 created: game=1280x720->2560x1440 neural=2560x1440->2560x1440
evaluate #1 pass 1 handle=... Color=game          Depth=game MVec=game
evaluate #1 pass 2 handle=... Color=previous-pass Depth=game MVec=game
standalone neural frame succeeded: count=3
```

Main settings in `deep-fried-chicken.cfg`:

```ini
layers=2
preserve_native_tone_color=0   ; 1 pins tone to the original, but expects clean_fry too
clean_fry_enabled=0            ; multi-pass cleanup; kills the colour cast but also detail
clean_fry_cleanup_strength=0.650
neural_work_percent=100
layer_N_nr_preset / nr_style / intensity / local_tone / local_structure / skin_structure
```

Pass count and per-pass parameters can be changed live in the Chicken tab of the ReShade
overlay. That is the reliable way to get a same-frame A/B: change the pass count and press
`F9` twice.

`preserve_native_tone_color=1` with `clean_fry_enabled=0` drops to around 15 fps and
becomes unstable; reproduced twice. The two appear to be meant as a pair.

With RenoDX instead, `ReShade.log` reports
`Failed to find NVSDK_NGX_D3D12_EvaluateFeature_C`. It is harmless: RenoDX catches the
contract through its `EvaluateFeature` hook and inline capture.

### 2. Calling Feature 18 directly (blocked; reproduce with `--nr-direct`)

`DLSSNRBackend` loads `nvngx_dlssnr.dll` itself and calls NGX feature 18. All the exports
are present, but `NVSDK_NGX_D3D12_Init_Ext` rejects every combination of application id and
API version with `0xBAD00002` (`FAIL_PlatformError`).

```text
DLSSNR: runtime loaded, 165840496 bytes, snippet version 0x1360800
DLSSNR: the staged runtime refused direct initialisation:
        [customAppId api1.5]=0xbad00002 [appId0 api1.5]=0xbad00002 ...
```

The snippet expects to be driven by the NGX core (`_nvngx.dll`); calling it directly needs
the compatibility shim community implementations carry. That is why the injection path
works with the same DLL — it goes through the core. The implementation is kept and would
work if initialisation ever succeeds.

Parameter names were read out of `renodx-dlss5.addon64` and recorded in
`DLSSNRBackend.cpp`, including the detail that subrect keys carry no dot
(`DLSSNR.ColorSubrectWidth`) and the existence of `DLSSNR.SkinStructureStrength`.

## Licence

Project code is MIT; see `LICENSE`. Third-party components are listed in `THIRD_PARTY.md`.

---

# 日本語

[English](#dlsscamdemo)

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
       ^ ReShade + NR add-on がここに割り込み Feature 18 (NR) を適用
  -> マスク合成 + A/B 比較
  -> swapchain
```

通常のカメラ映像はモーションベクトルも Z-buffer も持たないので、本アプリが画像履歴から
両方を推定して DLSS へ渡す。NR コンシューマ側のログ `Depth=game MVec=game` が、
それが実際に使われている証拠になる。

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
Move-Item ./onnxruntime-win-x64-1.29.0 ./external/onnxruntime
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
Invoke-WebRequest https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3_fp32.onnx -OutFile ./models/rvm_mobilenetv3_fp32.onnx
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
2. `config/ReShade.ini.template` がビルド時に `ReShade.ini` として配置される
   （Deep Fried Chicken に必要な `LoadFromDllMain` が入っている。既存ファイルは上書きしない）
3. `DLSSCamDemo.exe` を起動
4. `DLSSCamDemo.log` に `RAW NGX EvaluateFeature_C SUCCESS`、
   `deep-fried-chicken.log` に `standalone neural frame succeeded` が出れば NR が乗っている
5. Home キーで ReShade オーバーレイを開くと、パス数と各パスのパラメータを実行中に変更できる
6. `S` を2回押して並置モードにすると、左＝適用前 / 右＝適用後を同一フレームで比較できる

カメラは 2台以上あれば起動時に選択ダイアログが出る（選択は ini に保存）。

### 操作

| キー | 動作 |
|---|---|
| `1`〜`6` | Final / DLSS 入力 / モーションベクトル / Depth / Bias マスク / 合成マスク |
| `S` | A/B 比較の巡回: wipe → 並置 → off。wipe の境界はマウスドラッグで移動 |
| `K` | 楕円マスク on/off。ホイールでサイズ、Shift+ホイールで強度、Ctrl+ホイールでフェザー、右ドラッグで中心移動 |
| `G` | 人物セグメンテーション on/off（初回のみモデルをロード） |
| `R` | 取り込み解像度モードを巡回（Capture メニューでも選択可）|
| `[` `]` | ニューラル側のホワイトバランス。`0` でリセット |
| `M` | ミラー反転（表示のみ。DLSS 入力は反転しない） |
| `D` | DLSS on/off |
| `F6` | NGX feature を再作成 |
| `F9` | 静止画一式を `captures/` に保存（`_A_input` / `_B_dlss` / `_C_screen` / `_D_matte`）|
| `F11` / ダブルクリック | フルスクリーン |

ReShade が入力を占有している間は `Ctrl+Alt+` 版（`D` / `F6` / `F7` / `F9` / `K` / `G` /
`R` / `[` / `]`）を使う。

### 比較表示

- **wipe** — 1枚の画像の左半分が適用前、右半分が適用後。変化の有無を見るのに向く
- **並置** — 全体を2枚並べる。各半分はフル高さの中央クロップなので黒帯なしで画面を埋める。
  同じ箇所を同時刻の適用前後で比べられる唯一のモード

参照側（適用前）にはニューラル用ホワイトバランス補正がかからないので、片側だけ補正した
状態で比較してしまうことがない。

### 取り込み解像度モード

| モード | DLSS 入力 | 用途 |
|---|---|---|
| 1280x720 native | 1280x720 | そのまま。再構築の余地が小さく変化は控えめ |
| 960x540 | 1280x720 | |
| **640x360 reconstruction demo** | 1280x720 | **デモ向け。差が最も分かりやすい** |
| 320x180 extreme | 1280x720 | カメラが対応していなければ最寄りのモードにフォールバック |

低い解像度ほど入力と出力の隔たりが大きくなり、DLSS 5 が何を再構築しているかが見える。
ゲームで劇的に見えるのは入力が合成画像で隔たりが大きいからで、素の Webカメラ映像は
既に写真なので NR が足せるものが少ない。

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
--nr-direct         非対応の直接 Feature 18 経路を試す
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
  長時間運用では監視が要る

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

NR コンシューマは DLSS の evaluate 内部で NR を適用するため、「NR なしの DLSS 出力」は
取り出せない。したがってマスク外の領域は **原画のバイキュービック拡大** になり、DLSS の
超解像も効かない。マスク外も超解像を効かせるには Feature 18 の直接呼び出しが必要。

## 差を大きくするには

強度は既に上限に張り付いている。RenoDX のスライダーに 99 を書いてもクランプされる:

```text
NR Intensity / Local Tone / Local Structure / Skin Structure : 2.00
Color Strength / HDR Transfer Strength                       : 1.00
```

実際に効くのは2つ。

1. **取り込み解像度を下げて**再構築の距離を稼ぐ（上記のモード）
2. **NR パスを重ねる** — Deep Fried Chicken の `layers`。各パスの入力が前パスの出力になる。
   RTX 4090 / 2560x1440 での実測:

| layers | fps | 単純拡大比の構造量 |
|---|---|---|
| 1 | 30.0 | 1.29x |
| **2** | **30.0** | **1.41x** |
| 4 | 1.2（実用外）| 未測定 |

まだスイープしていない、量ではなく character を変えうる項目: `NR Preset`
（Default / #1 / #2 / #3 = `DLSSNR.Hint.Render.Preset`）、`NR Style`
（Default / Natural / Cinematic）、`Automatic Mask`（NR 自身がどこに適用するかを決める内部マスク）。

なお DLSS 5 NR は生成結果を入力へ拘束する設計で、人物の identity を変えるものではない。
顔つきの変化は「守るべき対象」「報告されている破綻」として扱うべきもので、機能ではない。

### 色転びについて

NR は出力の色温度を寒色側へ、輝度を上へずらす。実測値（入力との差）:

| 条件 | R−B | 輝度 |
|---|---|---|
| 1パス | −14.7 | +5.4 |
| 2パス | −15.9 | +10.1 |

これは NR 固有の性質でパス数のせいではない。本アプリは present パス（NR より後段）で
ニューラル側だけに per-channel ゲインをかけて補正する。`[` `]` で色温度、`0` でリセット。
`DLSSCamDemo.ini` の `[neural] warmth` / `exposure` に保存される。
**A/B 比較の参照側にはこの補正がかからない**ので、比較が歪まない。

補正がディテールを損なわないことは検証済み（無補正の撮影に同じゲインを数値的に適用しても
構造量は 1.30x → 1.32x で不変）。

## DLSS 5 Neural Rendering の経路

### 1. ReShade + NR コンシューマの注入（動作確認済み）

```text
runtime/
  dxgi.dll                        # ReShade 6.8 (add-on サポート版)
  nvngx_dlssnr.dll                # NR ランタイム本体
  deep-fried-chicken.addon64      # NR コンシューマ
  deep-fried-chicken-nvngx.dll    #   ブリッジ
  deep-fried-chicken.cfg          #   設定。初回のみ配置され以降は上書きしない
```

このアプリは NGX を直接叩いてネイティブ DLSS SR を持つので、Chicken の **standalone 経路**が
そのまま使える。DLSS5-Feeder のような外部のガイド供給は不要。

```text
standalone direct DLSSNR initialized through isolated bridge:
  runtime=<exe dir>/nvngx_dlssnr.dll  bridge=<exe dir>/deep-fried-chicken-nvngx.dll
create #1 feature 18: A=0x00000001 Success; MODE=TWO_PASSES requested=2
standalone feature 18 created: game=1280x720->2560x1440 neural=2560x1440->2560x1440
evaluate #1 pass 1 handle=... Color=game          Depth=game MVec=game
evaluate #1 pass 2 handle=... Color=previous-pass Depth=game MVec=game
standalone neural frame succeeded: count=3
```

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

RenoDX を使う場合、`ReShade.log` に `Failed to find NVSDK_NGX_D3D12_EvaluateFeature_C` が
出るが実害はない。RenoDX は `EvaluateFeature` 側のフックと inline capture で contract を
捕まえている。

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
コミュニティ実装が持つ互換シムが必要。注入経路が同じ DLL で動くのは NGX コア経由だから。
実装は残してあり、初期化さえ通れば動く。

パラメータ名は `renodx-dlss5.addon64` のバイナリから確認済みで `DLSSNRBackend.cpp` に
記録してある（`DLSSNR.ColorSubrectWidth` のように Subrect の前にドットが入らない点、
`DLSSNR.SkinStructureStrength` の存在を含む）。

## ライセンス

コード部分は MIT。`LICENSE` を参照。第三者コンポーネントは `THIRD_PARTY.md`。
