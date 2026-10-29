#!/usr/bin/env python3
"""Makes the Android resources of Wipeout Pure VR in packaging/quest/res: the app name and the launcher icon
layers (an adaptive icon: background colour, foreground, monochrome) from packaging/icon.png, the project's own
WP emblem (packaging/make_icon.py; no game art). Needs Pillow. Re-run after changing the emblem."""
import os

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
RES = os.path.join(HERE, "res")
BACKGROUND = "#141826"  # the emblem's body colour at its top

emblem = Image.open(os.path.join(ROOT, "packaging", "icon.png")).convert("RGBA")
os.makedirs(os.path.join(RES, "values"), exist_ok=True)
with open(os.path.join(RES, "values", "strings.xml"), "w") as f:
    f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
            '    <string name="app_name">Wipeout Pure VR</string>\n</resources>\n')
with open(os.path.join(RES, "values", "ic_launcher_background.xml"), "w") as f:
    f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
            f'    <color name="ic_launcher_background">{BACKGROUND}</color>\n</resources>\n')

# Adaptive icon layers are 108 dp with the visible part in the middle 72 dp; xxxhdpi is 4x.
for density, scale in (("mdpi", 1), ("hdpi", 1.5), ("xhdpi", 2), ("xxhdpi", 3), ("xxxhdpi", 4)):
    size = int(108 * scale)
    inner = int(72 * scale)
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    art = emblem.resize((inner, inner), Image.Resampling.LANCZOS)
    layer.paste(art, ((size - inner) // 2, (size - inner) // 2), art)
    folder = os.path.join(RES, f"mipmap-{density}")
    os.makedirs(folder, exist_ok=True)
    layer.save(os.path.join(folder, "ic_launcher_foreground.png"))
    mono = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    mono.putalpha(layer.getchannel("A"))
    mono.save(os.path.join(folder, "ic_launcher_monochrome.png"))
    # The plain icon for launchers without adaptive icons (and the Quest library's 2D tile).
    plain = emblem.resize((int(48 * scale), int(48 * scale)), Image.Resampling.LANCZOS)
    plain.save(os.path.join(folder, "ic_launcher.png"))
    plain.save(os.path.join(folder, "ic_launcher_round.png"))
print("wrote", RES)
