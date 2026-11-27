"""Compile the release shaders and record their SHA-256 hashes."""
import ctypes as c
import hashlib
import json
from pathlib import Path
from tempfile import TemporaryDirectory


class Macro(c.Structure):
    _fields_ = [('Name', c.c_char_p), ('Definition', c.c_char_p)]


compile_from_file = c.WinDLL('d3dcompiler_47.dll').D3DCompileFromFile
compile_from_file.argtypes = [
    c.c_wchar_p, c.POINTER(Macro), c.c_void_p, c.c_char_p, c.c_char_p, c.c_uint,
    c.c_uint, c.POINTER(c.c_void_p), c.POINTER(c.c_void_p),
]
compile_from_file.restype = c.c_long


def data(blob):
    vt = c.cast(blob, c.POINTER(c.POINTER(c.c_void_p))).contents
    ptr = c.WINFUNCTYPE(c.c_void_p, c.c_void_p)(vt[3])(blob)
    size = c.WINFUNCTYPE(c.c_size_t, c.c_void_p)(vt[4])(blob)
    return c.string_at(ptr, size)


jobs = [
    ('Bottle', 'BottleVertex', 'VSMain', 'vs_5_0', ['DBL_VERTEX']),
    ('Bottle', 'BottleImmediate', 'PSMain', 'ps_5_0', []),
    ('Bottle', 'BottleOptical', 'PSMain', 'ps_5_0', ['DBL_OPTICAL']),
    ('Bottle', 'BottleForward', 'PSMain', 'ps_5_0', ['DBL_OPTICAL', 'DBL_FORWARD']),
    ('OpticalCompose', 'ComposeVertex', 'VS', 'vs_5_0', []),
    ('OpticalCompose', 'ComposePixel', 'PS', 'ps_5_0', []),
    ('NativeCapture', 'NativeCapture', 'main', 'cs_5_0', []),
    ('Bottle', 'BottleNativeLiquid', 'PSMain', 'ps_5_0', ['DBL_NATIVE_GLASS']),
    ('NativeCapture', 'NativeCaptureCS', 'main', 'cs_5_0', ['DBL_CS_CAPTURE']),
    ('SharedBlur', 'SharedBlurDown', 'main', 'cs_5_0', []),
    ('SharedBlur', 'SharedBlurReverseDown', 'main', 'cs_5_0', ['DBL_REVERSE_DEPTH']),
    ('SharedBlur', 'SharedBlurCameraDown', 'main', 'cs_5_0', ['DBL_CAMERA_DEPTH']),
    ('SharedBlur', 'SharedBlurCameraReverseDown', 'main', 'cs_5_0', ['DBL_CAMERA_DEPTH', 'DBL_REVERSE_DEPTH']),
    ('SharedBlur', 'SharedBlurReduce', 'main', 'cs_5_0', ['DBL_REDUCE']),
    ('TmdShellOptics', 'TmdShellOptics', 'main', 'ps_5_0', []),
    ('TmdShellCoverage', 'TmdShellCoverage', 'main', 'ps_5_0', []),
    ('BottleProjection', 'BottleProjectionVS', 'VS', 'vs_5_0', []),
    ('BottleProjection', 'BottleProjectionPS', 'PS', 'ps_5_0', []),
    ('BottleProjection', 'BottleShadowMaskVS', 'VS', 'vs_5_0', ['PROJECTION_MASK']),
    ('BottleProjection', 'BottleShadowMaskPS', 'PS', 'ps_5_0', ['PROJECTION_MASK']),
    ('BottleProjection', 'BottleTileVS', 'TileVS', 'vs_5_0', []),
    ('BottleProjection', 'BottleCausticTilePS', 'CausticTilePS', 'ps_5_0', []),
    ('BottleProjection', 'BottleTilePS', 'TilePS', 'ps_5_0', []),
]


def compile_job(job, output_dir=None):
    source, name, entry, target, defines = job
    macros = (Macro * (len(defines) + 1))(*[Macro(x.encode(), b'1') for x in defines], Macro(None, None))
    code = c.c_void_p()
    err = c.c_void_p()
    hr = compile_from_file(
        str(Path(source + '.hlsl').resolve()), macros, c.c_void_p(1), entry.encode(),
        target.encode(), 1 << 15, 0, c.byref(code), c.byref(err),
    )
    try:
        if hr < 0:
            message = data(err).decode(errors='replace') if err.value else ''
            raise RuntimeError(f'{name} ({entry}, {target}) failed with {hr & 0xffffffff:#x}: {message}')
        output = Path(name + '.cso')
        if output_dir is not None:
            output = output_dir / output
        output.write_bytes(data(code))
        print(name, 'compiled', flush=True)
    finally:
        for blob in (code, err):
            if blob.value:
                vt = c.cast(blob, c.POINTER(c.POINTER(c.c_void_p))).contents
                c.WINFUNCTYPE(c.c_ulong, c.c_void_p)(vt[2])(blob)

from concurrent.futures import ThreadPoolExecutor


def build():
    sources = dict.fromkeys([job[0] + '.hlsl' for job in jobs] + ['CommunityLighting.hlsli', 'RefractionCamera.hlsli'])
    inputs = {name: hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in sources}
    with TemporaryDirectory(dir='.') as folder:
        staging = Path(folder)
        with ThreadPoolExecutor(max_workers=3) as pool:
            futures = [pool.submit(compile_job, job, staging) for job in jobs]
            for future in futures:
                future.result()
        if any(hashlib.sha256(Path(name).read_bytes()).hexdigest() != digest for name, digest in inputs.items()):
            raise RuntimeError('Shader sources changed during compilation. Rerun the build.')
        outputs = [job[1] + '.cso' for job in jobs]
        manifest = dict(inputs)
        for name in outputs:
            manifest[name] = hashlib.sha256((staging / name).read_bytes()).hexdigest()
        manifest_name = 'shader-build.json'
        (staging / manifest_name).write_text(json.dumps(manifest, indent=2))
        for name in [*outputs, manifest_name]:
            (staging / name).replace(name)


if __name__ == '__main__':
    build()
