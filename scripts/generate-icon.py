"""Regenerate the code-drawn application icon; Pillow is only needed for this script."""
from pathlib import Path
from PIL import Image, ImageDraw

size = 256
icon = Image.new("RGBA", (size, size))
draw = ImageDraw.Draw(icon)
draw.rounded_rectangle((8, 8, 248, 248), radius=52, fill="#1268CE")
draw.rounded_rectangle((48, 55, 208, 170), radius=16, fill="#FFFFFF")
draw.rounded_rectangle((59, 66, 197, 148), radius=7, fill="#DBEAFE")
draw.rounded_rectangle((84, 91, 116, 123), radius=5, fill="#1268CE")
draw.rounded_rectangle((130, 92, 174, 100), radius=4, fill="#5190DE")
draw.rounded_rectangle((130, 114, 162, 122), radius=4, fill="#5190DE")
draw.rectangle((116, 170, 140, 194), fill="#FFFFFF")
draw.rounded_rectangle((91, 191, 165, 202), radius=5, fill="#FFFFFF")
destination = Path(__file__).resolve().parents[1] / "resources" / "SystemInfo.ico"
icon.save(destination, sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
