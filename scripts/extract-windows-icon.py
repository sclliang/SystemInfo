"""Extract the Windows Documents folder ICO without resampling its images."""
import ctypes
from ctypes import wintypes as w
from pathlib import Path
import struct


def main():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.LoadLibraryExW.argtypes = [w.LPCWSTR, w.HANDLE, w.DWORD]
    kernel.LoadLibraryExW.restype = w.HMODULE
    kernel.FindResourceW.argtypes = [w.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
    kernel.FindResourceW.restype = w.HANDLE
    kernel.LoadResource.argtypes = [w.HMODULE, w.HANDLE]
    kernel.LoadResource.restype = w.HANDLE
    kernel.LockResource.argtypes = [w.HANDLE]
    kernel.LockResource.restype = ctypes.c_void_p
    kernel.SizeofResource.argtypes = [w.HMODULE, w.HANDLE]
    kernel.SizeofResource.restype = w.DWORD
    kernel.FreeLibrary.argtypes = [w.HMODULE]
    # Documents/desktop.ini: IconResource=%SystemRoot%/system32/imageres.dll,-112
    system = ctypes.create_unicode_buffer(32768)
    kernel.GetSystemDirectoryW(system, len(system))
    source = Path(system.value) / "imageres.dll"
    icon_id = 112
    candidates = [source, source.parent.parent / "SystemResources" / (source.name + ".mun")]
    for candidate in candidates:
        module = kernel.LoadLibraryExW(str(candidate), None, 0x22)
        if not module:
            continue
        try:
            def resource(resource_id, kind):
                handle = kernel.FindResourceW(module, resource_id, kind)
                if not handle:
                    raise ctypes.WinError(ctypes.get_last_error())
                return ctypes.string_at(kernel.LockResource(kernel.LoadResource(module, handle)),
                                        kernel.SizeofResource(module, handle))

            try:
                group = resource(icon_id, 14)
            except OSError:
                continue
            count = struct.unpack_from("<H", group, 4)[0]
            entries, images = [], []
            offset = 6 + 16 * count
            for index in range(count):
                entry = group[6 + index * 14:6 + (index + 1) * 14]
                image = resource(struct.unpack_from("<H", entry, 12)[0], 3)
                entries.append(entry[:8] + struct.pack("<II", len(image), offset))
                images.append(image)
                offset += len(image)
            target = Path(__file__).resolve().parent.parent / "resources" / "SystemInfo.ico"
            target.write_bytes(struct.pack("<HHH", 0, 1, count) + b"".join(entries + images))
            print(f"Extracted {count} original sizes from {candidate}, group {icon_id} to {target}")
            return
        finally:
            kernel.FreeLibrary(module)
    raise RuntimeError("Windows stock application icon resource was not found")


if __name__ == "__main__":
    main()
