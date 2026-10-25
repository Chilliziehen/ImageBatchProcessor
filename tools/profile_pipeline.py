import sys, time, tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tests'))
from integration import *
ctx=C.c_void_p();ck(create(None,C.byref(ctx)))
source=Host();result=Host();handle=C.c_uint64()
def timed(label, call):
    t=time.perf_counter();v=call();print(label,round(time.perf_counter()-t,3),flush=True);return v
try:
    timed('decode',lambda:ck(load(sys.argv[1].encode(),0,C.byref(source))))
    timed('upload',lambda:ck(upload(ctx,C.byref(source),C.byref(handle))))
    s=Analysis();timed('analysis',lambda:ck(analyze(ctx,handle,C.byref(s))))
    p=params();p.v[0]=1;p.v[28]=1;p.v[13]=.1;p.v[12]=.18
    timed('compute',lambda:ck(process(ctx,C.byref(handle),C.byref(p),1)))
    timed('download',lambda:ck(download(ctx,handle,C.byref(result))))
    with tempfile.TemporaryDirectory() as d:
        timed('encode',lambda:ck(save(str(Path(d)/'profile.jpg').encode(),C.byref(result),95)))
finally:
    free(C.byref(source));free(C.byref(result));release(ctx,handle);destroy(ctx)
