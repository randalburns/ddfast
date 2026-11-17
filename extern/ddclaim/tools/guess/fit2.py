import numpy as np, sys
sys.path.insert(0,__import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from fit import *
def featsC(h,leader,cap):
    N=h.shape[0]; idx=np.arange(N)
    hr=np.stack([h[idx,(leader+i)&3] for i in range(4)],1)   # leader-relative seats
    allc=hr[:,0]|hr[:,1]|hr[:,2]|hr[:,3]; ls=hr[:,0]|hr[:,2]
    pat=np.zeros((N,4),np.int64); k=np.zeros((N,4),np.int64)
    for r in range(12,-1,-1):
        pres=(allc>>r)&1; b=(ls>>r)&1; ok=(pres==1)&(k<4)
        pat|=np.where(ok,b<<k,0); k+=ok
    ln=POP[hr]
    c=np.minimum(ln,cap)
    key=pat+16*(c[:,0]+(cap+1)*(c[:,1]+(cap+1)*(c[:,2]+(cap+1)*c[:,3])))
    return key,ln
def designC(key,ln,trump,NK):
    N=len(trump); side=np.ones((N,4),bool); idx=np.arange(N); tm=trump<4; side[idx[tm],trump[tm]]=False
    rows=[];cols=[]
    for s in range(4):
        rows.append(idx); cols.append(np.where(side[:,s],key[:,s],NK+key[:,s]))
    tl=((ln[:,0]+ln[:,2]-ln[:,1]-ln[:,3])*~side).sum(1)
    rows.append(idx); cols.append(2*NK+tl+14)
    return csr_matrix((np.ones(N*5),(np.concatenate(rows),np.concatenate(cols))),shape=(N,2*NK+29))
if __name__=='__main__':
    for n in (6,):
        h,trump,leader,v=load(n); N=len(v); tr=np.arange(N)<N*0.8; te=~tr
        vl=np.where(leader&1,n-v,v)
        for cap in (3,4):
            NK=16*(cap+1)**4
            key,ln=featsC(h,leader,cap); X=designC(key,ln,trump,NK); est=np.zeros(N)
            for nt in (0,1):
                m=((trump==4)==bool(nt))
                w=lsqr(X[m&tr],vl[m&tr].astype(float),damp=1.0,iter_lim=400)[0]
                est[m]=X[m]@w
            ens=np.where(leader&1,n-est,est)
            print('C cap',cap,'rmse %.3f'%np.sqrt(((ens-v)**2)[te].mean()))
            for th in (0.9,1.0,1.1):
                t=np.clip(np.floor(ens+th),1,n).astype(int); print('   theta %.1f: %.4f window %.3f'%(th,cost(t,v,n)[te].mean(),((v==t)|(v==t-1))[te].mean()))
