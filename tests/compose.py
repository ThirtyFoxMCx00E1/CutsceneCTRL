"""Composite a UI frame written by tests/imgui (premultiplied RGBA, 1600x720) over a backdrop and save a PNG.
usage: compose.py ui.rgba out.png [backdrop.png]     (no backdrop = a dark neutral gradient)"""
import sys
from PIL import Image
import numpy as np

def comp(rgba_path, out, bg_path=None):
    ui = np.fromfile(rgba_path, dtype=np.uint8).reshape(720, 1600, 4).astype(np.float32)
    if bg_path:
        bg = np.asarray(Image.open(bg_path).convert('RGB').resize((1600, 720))).astype(np.float32)
    else:
        y = np.linspace(0, 1, 720, dtype=np.float32)[:, None, None]
        bg = np.repeat(np.repeat(30 + 40 * y, 1600, axis=1), 3, axis=2)
    a = ui[..., 3:4] / 255.0
    o = ui[..., :3] + bg * (1.0 - a)          # the test rasteriser writes premultiplied colour
    Image.fromarray(np.clip(o, 0, 255).astype(np.uint8)).save(out)

if __name__ == '__main__':
    comp(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
