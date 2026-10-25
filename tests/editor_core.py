"""Numerical and serialization contracts for the Studio/Core extension.
Also executes the legacy integration suite to guard the v1 processing path."""
from integration import *
import struct,zlib
class Curves(C.Structure):
    _fields_=[('count',C.c_uint32*4),('mode',C.c_uint32*4),('points',((C.c_float*2)*32)*4),('lut',(C.c_float*1024)*4)]
class Recipe(C.Structure):
    _fields_=[('target',Target),('curves',Curves)]
build_curves=proto('IBP_BuildCurves',[C.POINTER(Curves)])
default_curves=proto('IBP_DefaultCurves',[C.POINTER(Curves)],None)
copy_image=proto('IBP_CopyImage',[C.c_void_p,C.c_uint64,C.c_uint64])
display=proto('IBP_EncodeDisplay',[C.c_void_p,C.c_uint64,C.c_uint64])
process_curves=proto('IBP_ProcessBatchCurves',[C.c_void_p,C.POINTER(C.c_uint64),C.POINTER(Params),C.POINTER(Curves),C.c_uint32])
save_recipe=proto('IBP_SaveRecipe',[C.c_char_p,C.POINTER(Recipe)])
load_recipe=proto('IBP_LoadRecipe',[C.c_char_p,C.POINTER(Recipe)])
default_target=proto('IBP_DefaultTarget',[C.POINTER(Target)],None)
ctx=C.c_void_p();ck(create(None,C.byref(ctx)))
handles=[]
try:
    a=np.ones((19,31,4),np.float32);a[:,:,:3]=.18
    for _ in range(3):
        h=host(a);ident=C.c_uint64();ck(upload(ctx,C.byref(h),C.byref(ident)));handles.append(ident)
    original,work,view=handles
    p=params();p.v[0]=.75
    curves=Curves();default_curves(C.byref(curves))
    curves.count[0]=3;curves.points[0][1][:]=[.5,.65];curves.points[0][2][:]=[1,1];ck(build_curves(C.byref(curves)))
    lut=np.array(curves.lut[0]);assert np.all(np.diff(lut)>=0) and lut.min()==0 and lut.max()==1
    expected_linear=.18*2**.75
    encoded=float(srgb(np.array(expected_linear)))
    expected_curve=np.interp(encoded,np.linspace(0,1,1024),lut)
    expected=((expected_curve+.055)/1.055)**2.4
    snapshots=[]
    for repeat in range(4):
        ck(copy_image(ctx,original,work))
        ck(process_curves(ctx,C.byref(work),C.byref(p),C.byref(curves),1))
        out=Host();ck(download(ctx,work,C.byref(out)))
        snapshots.append(np.ctypeslib.as_array(out.pixels,shape=(19*31*4,)).copy());free(C.byref(out))
        np.testing.assert_allclose(snapshots[-1].reshape(-1,4)[:,:3],expected,atol=1e-5)
    for image in snapshots[1:]:np.testing.assert_array_equal(image,snapshots[0])
    ck(display(ctx,work,view));out=Host();ck(download(ctx,view,C.byref(out)))
    np.testing.assert_allclose(np.ctypeslib.as_array(out.pixels,shape=(19*31*4,)).reshape(-1,4)[:,:3],expected_curve,atol=1e-5);free(C.byref(out))
    out=Host();ck(download(ctx,original,C.byref(out)));np.testing.assert_array_equal(np.ctypeslib.as_array(out.pixels,shape=(19*31*4,)),a.ravel());free(C.byref(out))
    assert copy_image(ctx,original,original)!=0
    with tempfile.TemporaryDirectory() as temp:
        path=Path(temp)/'曲线.imgarg';r=Recipe();default_target(C.byref(r.target));r.target.params=p;r.curves=curves
        ck(save_recipe(str(path).encode(),C.byref(r)));data=path.read_bytes();assert len(data)==22848
        assert struct.unpack('<6I',data[8:32])==(2,0x01020304,32,22816,zlib.crc32(data[32:]),1)
        loaded=Recipe();ck(load_recipe(str(path).encode(),C.byref(loaded)));assert bytes(r)==bytes(loaded)
        r.target.ranges[0]=float('nan');assert save_recipe(str(path).encode(),C.byref(r))!=0
        invalid=bytearray(data);invalid[6000]^=1;path.write_bytes(invalid);assert load_recipe(str(path).encode(),C.byref(loaded))!=0
        ck(load_recipe(str(ROOT/'natural.imgarg').encode(),C.byref(loaded)));assert loaded.curves.count[0]==17
    curves.points[0][1][0]=0;assert build_curves(C.byref(curves))!=0
    curves.lut[0][12]=float('nan');assert process_curves(ctx,C.byref(work),C.byref(p),C.byref(curves),1)!=0
    # Different LUTs and exposures in one batch must use their own push-constant offset.
    batch_params=(Params*3)();batch_curves=(Curves*3)();expected_batch=[]
    for i,ident in enumerate(handles):
        # Reset every input by uploading a fresh handle, keeping the original contract separate.
        h=host(a);fresh=C.c_uint64();ck(upload(ctx,C.byref(h),C.byref(fresh)))
        ck(copy_image(ctx,fresh,ident));release(ctx,fresh)
        batch_params[i]=params();batch_params[i].v[0]=i*.25
        default_curves(C.byref(batch_curves[i]));batch_curves[i].lut[0][:]=np.linspace(0,1,1024)**(1+i*.2)
        encoded=float(srgb(np.array(.18*2**(i*.25))))
        value=np.interp(encoded,np.linspace(0,1,1024),batch_curves[i].lut[0])
        expected_batch.append(((value+.055)/1.055)**2.4)
    ids=(C.c_uint64*3)(*(h.value for h in handles))
    ck(process_curves(ctx,ids,batch_params,batch_curves,3))
    for ident,value in zip(handles,expected_batch):
        out=Host();ck(download(ctx,ident,C.byref(out)))
        np.testing.assert_allclose(np.ctypeslib.as_array(out.pixels,shape=(19*31*4,)).reshape(-1,4)[:,:3],value,atol=1e-5);free(C.byref(out))
    print('PASS: resident reset/no accumulation, immutable original, 1024 LUT CPU/GPU agreement, display encoding, IMGARG v2 CRC/roundtrip/v1 import and invalid curves')
finally:
    for handle in handles: release(ctx,handle)
    destroy(ctx)
