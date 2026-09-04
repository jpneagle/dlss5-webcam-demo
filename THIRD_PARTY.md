# Third-party components

This repository contains project source code under the MIT License, but it interoperates with third-party software that has separate licenses and terms.

## NVIDIA DLSS / NGX

The one-click build clones the official NVIDIA DLSS repository into `external/DLSS`. NVIDIA files are not relicensed by this project. Review NVIDIA's license in that checkout before redistributing NVIDIA binaries.

## ReShade

ReShade is optional for native DLSS SR but required for the experimental RenoDX DLSS 5 workflow described in the documentation. ReShade is a separate project.

## Upstream project

The application is derived from the MIT-licensed DLSS Video Player
(<https://gitlab.com/JessicaNataliaMods/dlss-5-video-player>). Its copyright notice is retained in
`LICENSE`.

## Experimental DLSS 5 Neural Rendering runtimes

None of the following are included in this repository. Users obtain and use them separately under
the terms applicable to those files:

- `nvngx_dlssnr.dll` — NVIDIA proprietary. The builds circulating for RTX 40-series hardware are
  community-modified and do not match NVIDIA's original Authenticode hash.
- `renodx-dlss5.addon64` — RenoDX experimental DLSS 5 add-on.
- `deep-fried-chicken.addon64`, `deep-fried-chicken-nvngx.dll` — Deep Fried Chicken, distributed by
  its author as its own archive. Its licence asks that the author's official release link be shared
  rather than the archive rehosted.
- `dxgi.dll` (ReShade with add-on support) — ReShade is a separate project.

The demo only ever loads such files from a local `runtime/` directory that the user populates; that
directory is excluded from version control.

## ONNX Runtime

Microsoft ONNX Runtime (MIT) is used for the person matte. It is downloaded by the user into
`external/onnxruntime/` and is not redistributed here.

## Robust Video Matting

The person matte uses the RVM mobilenetv3 model from
<https://github.com/PeterL1n/RobustVideoMatting> (GPL-3.0). The model file is downloaded by the user
into `models/` and is not redistributed here. Check that project's licence before shipping anything
that bundles the weights.
