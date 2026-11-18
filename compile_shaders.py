import ctypes as c
from pathlib import Path
class Macro(c.Structure):
    _fields_=[('Name',c.c_char_p),('Definition',c.c_char_p)]
f=c.WinDLL('d3dcompiler_47.dll').D3DCompileFromFile
f.argtypes=[c.c_wchar_p,c.POINTER(Macro),c.c_void_p,c.c_char_p,c.c_char_p,c.c_uint,c.c_uint,c.POINTER(c.c_void_p),c.POINTER(c.c_void_p)]
f.restype=c.c_long
def data(blob):
    vt=c.cast(blob,c.POINTER(c.POINTER(c.c_void_p))).contents
    ptr=c.WINFUNCTYPE(c.c_void_p,c.c_void_p)(vt[3])(blob)
    size=c.WINFUNCTYPE(c.c_size_t,c.c_void_p)(vt[4])(blob)
    return c.string_at(ptr,size)
jobs=[('Bottle','BottleVertex','VSMain','vs_5_0',['DBL_VERTEX']),('Bottle','BottleImmediate','PSMain','ps_5_0',[]),('Bottle','BottleOptical','PSMain','ps_5_0',['DBL_OPTICAL']),('Bottle','BottleForward','PSMain','ps_5_0',['DBL_OPTICAL','DBL_FORWARD']),('OpticalCompose','ComposeVertex','VS','vs_5_0',[]),('OpticalCompose','ComposePixel','PS','ps_5_0',[]),('NativeCapture','NativeCapture','main','cs_5_0',[])]
jobs += [('Bottle','BottleNativeLiquid','PSMain','ps_5_0',['DBL_NATIVE_GLASS'])]
jobs += [('NativeCapture','NativeCaptureCS','main','cs_5_0',['DBL_CS_CAPTURE'])]
jobs += [('SharedBlur','SharedBlurDown','main','cs_5_0',[]),('SharedBlur','SharedBlurReduce','main','cs_5_0',['DBL_REDUCE'])]
def compile_job(job):
    source,name,entry,target,defines=job
    macros=(Macro*(len(defines)+1))(*[Macro(x.encode(),b'1') for x in defines],Macro(None,None))
    code=c.c_void_p();err=c.c_void_p()
    hr=f(str(Path(source+'.hlsl').resolve()),macros,c.c_void_p(1),entry.encode(),target.encode(),1<<15,0,c.byref(code),c.byref(err))
    if hr<0:
        if err.value:print(data(err).decode(errors='replace'))
        raise RuntimeError(hex(hr&0xffffffff))
    Path(name+'.cso').write_bytes(data(code));print(name,'compiled',flush=True)
    for blob in (code,err):
        if blob.value:
            vt=c.cast(blob,c.POINTER(c.POINTER(c.c_void_p))).contents
            c.WINFUNCTYPE(c.c_ulong,c.c_void_p)(vt[2])(blob)
from concurrent.futures import ThreadPoolExecutor
with ThreadPoolExecutor(max_workers=3) as pool:list(pool.map(compile_job,jobs))
import hashlib,json
paths=['Bottle.hlsl','OpticalCompose.hlsl','NativeCapture.hlsl','SharedBlur.hlsl','CommunityLighting.hlsli']+[j[1]+'.cso' for j in jobs]
Path('shader-build.json').write_text(json.dumps({p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in paths},indent=2))
