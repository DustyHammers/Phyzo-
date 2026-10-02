import re,sys,json
ALU={0x00:'ADD',0x01:'ADDV',0x02:'ADDC',0x0E:'AMDF',0x08:'AND',0x0F:'AS',0x11:'ASDH',0x12:'ASDL',0x0D:'AVG',0x19:'BIOZ',0x15:'BREV',0x16:'DREV',0x18:'HOST',0x1C:'Jcc',0x1D:'JScc',0x17:'LIM',0x10:'LS',0x13:'LSDH',0x14:'LSDL',0x06:'MAX',0x07:'MIN',0x0B:'MOV',0x1B:'MOVcc',0x09:'OR',0x0C:'RECT',0x1F:'REPT',0x1E:'RScc',0x03:'SUB',0x05:'SUBB',0x1A:'SUBREV',0x04:'SUBV',0x0A:'XOR'}
AGEN=['RD','WR','RD+BASE','WR+BASE','RD+1','WR+1','NOP','BASE']
class R:
    def __init__(s,d,p): s.d=d;s.p=p
    def b(s): v=s.d[s.p]; s.p+=1; return v
    def num(s):
        c=s.b()
        if c&0x80:
            n=c&0x7f; v=int.from_bytes(s.d[s.p:s.p+n],'big'); s.p+=n; return v,n
        return c,0
    def n(s): return s.num()[0]
    def name(s): c=s.b(); t=s.d[s.p:s.p+c].decode('latin1'); s.p+=c; return t
def fields(w):
    x=int.from_bytes(w,'big')
    g=lambda hi,lo:(x>>lo)&((1<<(hi-lo+1))-1)
    return dict(A=g(95,86),B=g(85,76),C=g(75,66),ALU=g(65,61),ALUs=g(60,60),D=g(59,50),E=g(49,40),F=g(39,30),MAC=g(29,25),SH=g(24,21),MACs=g(20,20),G=g(19,11)|0x200,AG=g(10,8),RGN=g(7,5),DL=g(4,1),AGs=g(0,0))
def parse(d,s):
    r=R(d,s); assert d[s:s+2]==b'E2'; r.p+=2
    hsize=r.n(); ver=r.n(); offs={}
    end=s+2+1+hsize if False else None
    hend=s+3+hsize
    while r.p<hend:
        t=r.b(); offs[t]=r.n()
    o={'addr':s,'version':ver,'offsets':offs}
    def part(tag):
        if not offs.get(tag): return None
        r.p=s+offs[tag]; assert r.b()==tag; size=r.n(); return r.p,size
    q,sz=part(0); r.p=q; r.b(); o['family']=r.n(); o['member']=r.n(); o['name']=r.name()
    q,sz=part(1); r.p=q; r.b()
    o['res']=dict(zip(['num_inst','num_gprs','num_aors','num_regions','dil_bitmap','dol_bitmap','tbl_mem_size','ddl_mem_size','exc_time'],[r.n() for _ in range(9)]))
    # init part
    if part(3):
        q,sz=part(3); r.p=q; inits=[]
        while r.p<q+sz:
            t=r.b()
            if t==0:
                n=r.n(); a=r.n(); inits+= [(a+i,r.n()) for i in range(n)]
            elif t==1:
                n=r.n(); inits+=[(r.n(),r.n()) for i in range(n)]
            elif t==2:
                n=r.n(); o.setdefault('reg_arrays',[]).extend([(r.n(),r.n(),r.n()) for i in range(n)])
            elif t==3:
                n=r.n(); o.setdefault('int_tables',[]).extend([(r.n(),r.n(),r.n()) for i in range(n)])
            elif t==4:
                n=r.n(); o.setdefault('ext_tables',[]).extend([(r.n(),r.n(),r.name()) for i in range(n)])
            elif t==5:
                n=r.n(); o.setdefault('ram_inits',[]).extend([(r.n(),r.n(),r.n()) for i in range(n)])
            else:
                o['init_unknown_tag']=t; break
        o['reg_inits']=inits
    q,sz=part(5); r.p=q; assert r.b()==0; n=r.n()
    o['uinst']=[d[r.p+12*i:r.p+12*i+12] for i in range(n)]
    q,sz=part(7); r.p=q; assert r.b()==0; o['checksum']=int.from_bytes(d[r.p:r.p+2],'big'); o['cksum_span_end']=r.p; r.p+=2
    o['end']=r.p+1 if d[r.p]==0xff else None
    return o
def find(d): return [m.start() for m in re.finditer(rb'E2[\x00-\x7f][\x00-\x7f]\x00',d) ]
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read()
