"""Independent numerical checks against the real Vulkan backend, including file ABI."""
import ctypes as C
import json
import math
import tempfile
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
dll = C.CDLL(str(ROOT/'build/bin/Release/ImageBatchProcessorCore.dll'))
class Host(C.Structure):
    _fields_ = [('width', C.c_uint32), ('height', C.c_uint32), ('pixels', C.POINTER(C.c_float))]
class Params(C.Structure):
    _fields_ = [('v', C.c_float*256)]
class Analysis(C.Structure):
    _fields_ = [('metrics', C.c_double*23), ('samples', C.c_uint64), ('hist', C.c_uint32*1024)]
class Target(C.Structure):
    _fields_ = [('params', Params), ('ranges', C.c_double*8), ('analysis', Analysis)]
def proto(name, args, result=C.c_int):
    f = getattr(dll, name)
    f.argtypes = args
    f.restype = result
    return f
create = proto('IBP_Create',[C.c_char_p,C.POINTER(C.c_void_p)])
destroy = proto('IBP_Destroy',[C.c_void_p],None)
last = proto('IBP_LastError',[],C.c_char_p)
default = proto('IBP_DefaultParams',[C.POINTER(Params)],None)
upload = proto('UploadImage',[C.c_void_p,C.POINTER(Host),C.POINTER(C.c_uint64)])
process = proto('IBP_ProcessBatch',[C.c_void_p,C.POINTER(C.c_uint64),C.POINTER(Params),C.c_uint32])
download = proto('DownloadImage',[C.c_void_p,C.c_uint64,C.POINTER(Host)])
free = proto('IBP_FreeHostImage',[C.POINTER(Host)],None)
release = proto('IBP_ReleaseImage',[C.c_void_p,C.c_uint64])
analyze = proto('IBP_Analyze',[C.c_void_p,C.c_uint64,C.POINTER(Analysis)])
load = proto('IBP_LoadImage',[C.c_char_p,C.c_int,C.POINTER(Host)])
save = proto('IBP_SaveImage',[C.c_char_p,C.POINTER(Host),C.c_int])
def ck(value):
    assert value==0, last().decode()
def params():
    p=Params(); default(C.byref(p)); return p
def host(a):
    return Host(a.shape[1],a.shape[0],a.ctypes.data_as(C.POINTER(C.c_float)))
def srgb(a):
    return np.where(a<=.0031308,12.92*a,1.055*np.maximum(a,0)**(1/2.4)-.055)
ctx=C.c_void_p();ck(create(None,C.byref(ctx)))
rng=np.random.default_rng(42)
a=np.ones((37,53,4),dtype=np.float32);a[:,:,:3]=rng.uniform(.02,.42,a[:,:,:3].shape)
def render(a,p):
    h=host(a);handle=C.c_uint64();out=Host()
    ck(upload(ctx,C.byref(h),C.byref(handle)))
    try:
        ck(process(ctx,C.byref(handle),C.byref(p),1)); ck(download(ctx,handle,C.byref(out)))
        s=Analysis();ck(analyze(ctx,handle,C.byref(s)))
        return np.ctypeslib.as_array(out.pixels,shape=(out.width*out.height*4,)).reshape(out.height,out.width,4).copy(),s
    finally:
        free(C.byref(out));ck(release(ctx,handle))
try:
    identity,s=render(a,params());np.testing.assert_allclose(identity,a,atol=3e-6)
    expected=srgb(a[:,:,:3]);y=expected@np.array([.2126,.7152,.0722])
    assert abs(s.metrics[0]-y.mean())<1e-6
    assert abs(s.metrics[6]-y.std())<1e-6
    rg=expected[:,:,0]-expected[:,:,1];yb=(expected[:,:,0]+expected[:,:,1])/2-expected[:,:,2]
    color=math.hypot(rg.std(),yb.std())+.3*math.hypot(rg.mean(),yb.mean())
    assert abs(s.metrics[13]-color)<1e-6
    assert all(sum(s.hist[c*256:(c+1)*256])==a.shape[0]*a.shape[1] for c in range(4))
    p=params();p.v[0]=.5
    bright,_=render(a,p);np.testing.assert_allclose(bright[:,:,:3],a[:,:,:3]*2**.5,atol=4e-6)
    p=params();p.v[6]=0
    mono,_=render(a,p);np.testing.assert_allclose(mono[:,:,0],mono[:,:,1],atol=1e-6)
    red=np.zeros((11,19,4),np.float32);red[:,:,0]=.25;red[:,:,3]=1
    p=params();p.v[8]=120
    green,_=render(red,p);np.testing.assert_allclose(green[:,:,1],.25,atol=2e-6);assert green[:,:,0].max()<1e-6
    p=params();p.v[9]=1
    warm,_=render(a,p);np.testing.assert_allclose(warm[:,:,:3],a[:,:,:3]*[2**.2,1,2**(-.2)],atol=4e-6)
    flat=np.full((67,39,4),.2,np.float32);flat[:,:,3]=1
    p=params();p.v[13]=.6;p.v[12]=.8;p.v[14]=.5
    smooth,_=render(flat,p);np.testing.assert_allclose(smooth,flat,atol=3e-6)
    p=params();p.v[17]=.5;p.v[18]=110
    warped,_=render(a,p);assert np.isfinite(warped).all();assert np.mean(abs(warped-a))>.005
    p=params();p.v[30]=1;p.v[51]=.3;p.v[52]=.1;p.v[54]=1
    local,_=render(flat,p)
    np.testing.assert_allclose(local[33,19,:3],.4,atol=4e-6)
    np.testing.assert_allclose(local[0,0,:3],.2,atol=4e-6)
    p.v[30]=2;p.v[53]=0
    graduated,_=render(flat,p)
    np.testing.assert_allclose(graduated[33,0,:3],.2,atol=4e-6)
    np.testing.assert_allclose(graduated[33,-1,:3],.4,atol=4e-6)
    p.v[30]=3;p.v[57]=.4;p.v[58]=.6;p.v[52]=.04
    luma_mask,_=render(flat,p);np.testing.assert_allclose(luma_mask[:,:,:3],.4,atol=4e-6)
    p=params();p.v[0]=float('nan');h=host(a);handle=C.c_uint64();ck(upload(ctx,C.byref(h),C.byref(handle)))
    assert process(ctx,C.byref(handle),C.byref(p),1)!=0;ck(release(ctx,handle))
    with tempfile.TemporaryDirectory(prefix='imgbp_unicode_测试_') as directory:
        for ext in ['tif','png','jpg']:
            f=Path(directory)/('颜色.'+ext);source=host(a);ck(save(str(f).encode(),C.byref(source),95))
            assert save(str(f).encode(),C.byref(source),95)!=0
            decoded=Host();ck(load(str(f).encode(),0,C.byref(decoded)))
            try:
                assert (decoded.width,decoded.height)==(53,37)
                if ext!='jpg':
                    out=np.ctypeslib.as_array(decoded.pixels,shape=(53*37*4,)).reshape(37,53,4)
                    np.testing.assert_allclose(out,a,atol=2e-5)
            finally: free(C.byref(decoded))
    print('PASS: independent CPU/GPU mean, RMS, Hasler colourfulness, hue, WB, exposure, denoise invariance, Panini, NaN rejection, Unicode and JPEG/16-bit TIFF/PNG roundtrips')
finally:
    destroy(ctx)
