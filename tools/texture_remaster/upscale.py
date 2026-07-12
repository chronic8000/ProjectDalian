#!/usr/bin/env python3
"""GPU / CPU image upscalers for the remaster pipeline."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Optional

import numpy as np
from PIL import Image


class Upscaler:
    def __init__(self, scale: int = 2, backend: str = "auto") -> None:
        self.scale = scale
        self.backend = backend
        self._impl = None
        self._name = "none"
        self._init_backend()

    @property
    def name(self) -> str:
        return self._name

    def _init_backend(self) -> None:
        want = self.backend
        if want in ("auto", "realesrgan"):
            if self._try_torch_realesrgan():
                return
        if want in ("auto", "ncnn"):
            if self._try_ncnn():
                return
        if want in ("auto", "lanczos"):
            self._name = "lanczos"
            self._impl = "lanczos"
            return
        raise RuntimeError(f"no upscaler backend available (wanted={want})")

    def _try_torch_realesrgan(self) -> bool:
        try:
            import torch
            from basicsr.archs.rrdbnet_arch import RRDBNet
            from realesrgan import RealESRGANer
        except Exception:
            return False
        if not torch.cuda.is_available():
            # Still usable on CPU but very slow — only take if explicitly realesrgan
            if self.backend != "realesrgan":
                return False
        model = RRDBNet(num_in_ch=3, num_out_ch=3, num_feat=64, num_block=23, num_grow_ch=32, scale=4)
        half = torch.cuda.is_available()
        # Official x4plus weights (downloaded once into cwd / cache).
        weight_url = (
            "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth"
        )
        weight_path = Path(__file__).resolve().parent / "weights" / "RealESRGAN_x4plus.pth"
        if not weight_path.is_file():
            try:
                import urllib.request

                weight_path.parent.mkdir(parents=True, exist_ok=True)
                print(f"Downloading RealESRGAN weights -> {weight_path}")
                urllib.request.urlretrieve(weight_url, weight_path)
            except Exception:
                return False
        try:
            upsampler = RealESRGANer(
                scale=4,
                model_path=str(weight_path),
                model=model,
                tile=400,
                tile_pad=10,
                pre_pad=0,
                half=half,
            )
        except Exception:
            return False
        self._impl = ("torch", upsampler)
        self._name = f"realesrgan-torch({'cuda' if half else 'cpu'})"
        return True

    def _try_ncnn(self) -> bool:
        exe = shutil.which("realesrgan-ncnn-vulkan") or shutil.which("realesrgan-ncnn-vulkan.exe")
        if not exe:
            local = Path(__file__).resolve().parent / "realesrgan-ncnn-vulkan.exe"
            if local.is_file():
                exe = str(local)
        if not exe:
            return False
        self._impl = ("ncnn", exe)
        self._name = f"realesrgan-ncnn ({exe})"
        return True

    def upscale_rgba(self, rgba: np.ndarray) -> np.ndarray:
        if self._impl == "lanczos":
            return self._lanczos(rgba, self.scale)
        kind = self._impl[0]
        if kind == "torch":
            return self._torch_up(rgba)
        if kind == "ncnn":
            return self._ncnn_up(rgba)
        raise RuntimeError("upscaler not initialized")

    def _lanczos(self, rgba: np.ndarray, scale: int) -> np.ndarray:
        img = Image.fromarray(rgba, "RGBA")
        w, h = img.size
        out = img.resize((w * scale, h * scale), Image.Resampling.LANCZOS)
        return np.array(out)

    def _torch_up(self, rgba: np.ndarray) -> np.ndarray:
        upsampler = self._impl[1]
        # RealESRGAN expects BGR uint8
        bgr = rgba[:, :, 2::-1].copy()
        try:
            output, _ = upsampler.enhance(bgr, outscale=self.scale)
        except Exception:
            # Alpha: upscale RGB, nearest alpha
            rgb = self._lanczos(rgba, self.scale)
            return rgb
        out = np.zeros((output.shape[0], output.shape[1], 4), dtype=np.uint8)
        out[:, :, 2::-1] = output
        if rgba.shape[2] == 4:
            a = Image.fromarray(rgba[:, :, 3], "L").resize(
                (out.shape[1], out.shape[0]), Image.Resampling.LANCZOS
            )
            out[:, :, 3] = np.array(a)
        else:
            out[:, :, 3] = 255
        return out

    def _ncnn_up(self, rgba: np.ndarray) -> np.ndarray:
        exe = self._impl[1]
        models_dir = str(Path(exe).resolve().parent / "models")
        with tempfile.TemporaryDirectory(prefix="bf2_up_") as td:
            td_path = Path(td)
            src = td_path / "in.png"
            dst = td_path / "out.png"
            Image.fromarray(rgba, "RGBA").save(src)
            cmd = [
                exe,
                "-i",
                str(src),
                "-o",
                str(dst),
                "-m",
                models_dir,
                "-n",
                "realesrgan-x4plus",
                "-s",
                str(self.scale),
                "-f",
                "png",
            ]
            subprocess.run(cmd, check=True, capture_output=True)
            return np.array(Image.open(dst).convert("RGBA"))
