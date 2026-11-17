import numpy as np, sys
from scipy.sparse import csr_matrix
from scipy.sparse.linalg import lsqr
POP=np.array([bin(i).count('1') for i in range(8192)],dtype=np.int64)
def load(n):
    raw=np.fromfile('build/guess/data_%d.bin'%n,dtype=np.uint8).reshape(-1,36)
    h=raw[:,:32].copy().view(np.uint16).reshape(-1,4,4).astype(np.int64)
    return h,raw[:,32].astype(np.int64),raw[:,33].astype(np.int64),raw[:,34].astype(np.int64)
def feats(h):
    N=h.shape[0]
    allc=h[:,0]|h[:,1]|h[:,2]|h[:,3]; ns=h[:,0]|h[:,2]
    pat=np.zeros((N,4),np.int64); k=np.zeros((N,4),np.int64)
    for r in range(12,-1,-1):
        pres=(allc>>r)&1; isns=(ns>>r)&1
        ok=(pres==1)&(k<4)
        pat|=np.where(ok,isns<<k,0); k+=ok
    ln=POP[h]  # N,4 seats,4 suits
    return pat,ln
def cost(t,v,n):
    # number of questions starting at t
    c=np.where(v>=t, np.where(v==n, v-t+1, v-t+2), np.where(v==0, t-v, t-v+1))
    return c
def old_guess(pat,ln,trump,leader,n):
    N=len(trump); side=np.ones((N,4),bool); side[np.arange(N)[trump<4],trump[trump<4]]=False
    c=[ (POP[pat&m]*side).sum(1) for m in (1,3,7)]
    tr=[ (POP[pat&m]*~side).sum(1) for m in (1,3,7)]
    tl=((ln[:,0]+ln[:,2]-ln[:,1]-ln[:,3])*~side).sum(1)
    e=np.where(trump==4, -22-143*(leader&1)+119*c[0]+21*c[1]+12*c[2], 52-55*(leader&1)+76*c[0]+11*c[1]+6*c[2]+87*tr[0]+30*tr[1]+20*tr[2]+45*tl)
    return e/100.0*n/6
def keys(pat,ln):
    lns=np.minimum(np.maximum(ln[:,0],ln[:,2]),4); lew=np.minimum(np.maximum(ln[:,1],ln[:,3]),4)
    return pat+16*(lns+5*lew)   # 400
NK=400
def design(pat,ln,trump):
    N=len(trump); ky=keys(pat,ln); side=np.ones((N,4),bool); idx=np.arange(N); tm=trump<4; side[idx[tm],trump[tm]]=False
    rows=[];cols=[]
    for s in range(4):
        rows.append(idx); cols.append(np.where(side[:,s],ky[:,s],NK+ky[:,s]))
    tl=((ln[:,0]+ln[:,2]-ln[:,1]-ln[:,3])*~side).sum(1)
    rows.append(idx); cols.append(2*NK+tl+14)
    rows=np.concatenate(rows); cols=np.concatenate(cols)
    return csr_matrix((np.ones(len(rows)),(rows,cols)),shape=(N,2*NK+29))
if __name__=='__main__':
    for n in (6,5,4,7):
        h,trump,leader,v=load(n); pat,ln=feats(h)
        N=len(v); tr=np.arange(N)<N*0.8; te=~tr
        eo=old_guess(pat,ln,trump,leader,n)
        to=np.clip(np.floor(eo+0.5),1,n).astype(int)
        print('n',n,'old rule: questions %.4f exact-window %.3f'%(cost(to,v,n)[te].mean(), ((v==to)|(v==to-1))[te].mean()))
        for th in (0.7,0.8,0.9,1.0,1.1):
            t=np.clip(np.floor(eo+th),1,n).astype(int); print('   old model theta %.1f: %.4f'%(th,cost(t,v,n)[te].mean()))
        X=design(pat,ln,trump); est=np.zeros(N)
        for nt in (0,1):
            for ls in (0,1):
                m=((trump==4)==bool(nt))&((leader&1)==ls)
                w=lsqr(X[m&tr],v[m&tr].astype(float),damp=0.5,iter_lim=300)[0]
                est[m]=X[m]@w
        print('   new model rmse %.3f (old %.3f)'%(np.sqrt(((est-v)**2)[te].mean()), np.sqrt(((eo-v)**2)[te].mean())))
        for th in (0.8,0.9,1.0,1.1):
            t=np.clip(np.floor(est+th),1,n).astype(int); print('   new model theta %.1f: %.4f  window %.3f'%(th,cost(t,v,n)[te].mean(), ((v==t)|(v==t-1))[te].mean()))
        print('   floor (perfect):', np.where((v==0)|(v==n),1,2)[te].mean())
