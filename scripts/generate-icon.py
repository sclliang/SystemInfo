"""Build the application ICO from the approved Desktop Pulse PNG master.

Pillow is needed only when regenerating the icon; normal release builds use
the committed PNG and ICO resources directly.
"""
from pathlib import Path
import struct
from PIL import Image


def main():
    resources = Path(__file__).resolve().parent.parent / "resources"
    with Image.open(resources / "SystemInfo.png") as source:
        image = source.convert("RGBA")
        if image.size != (256, 256):
            raise ValueError("The approved application icon master must be 256 x 256")
        target = resources / "SystemInfo.ico"
        image.save(target, format="ICO",
                   sizes=[(size, size) for size in (16, 20, 24, 32, 40, 48, 64, 256)])
    # Match the Win32 resource compiler's canonical one-plane directory. Pillow
    # emits zero for PNG-backed entries even though their images are 32-bit RGBA.
    data = bytearray(target.read_bytes())
    for index in range(struct.unpack_from("<H", data, 4)[0]):
        struct.pack_into("<H", data, 6 + index * 16 + 4, 1)
    target.write_bytes(data)
    print("Generated resources/SystemInfo.ico (8 sizes, 16-256 px)")


if __name__ == "__main__":
    main()
