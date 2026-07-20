// birb_tiny.js — synth player for 4K demos (~600b brotli)
// b=birb(songArrayBuffer, audioCtx) → {play(), stop(), row, pat, samples}
//<REV>
// Reverb-send code is fenced in //<REV> … //</REV> markers (an optional
// //<REV-else> gives the lean alternative). This checked-in file IS the
// with-reverb source and runs as-is; strip_reverb.py deletes the REV blocks
// (and uncomments any REV-else) to emit birb_tiny.norev.js, a lean no-reverb
// build. Marker lines must be exactly //<REV>, //<REV-else>, //</REV>.
//</REV>
function birb(B,X){
var d=new Uint8Array(B),p=8,S=44100,N=4,F=65536,
bpm=d[4],tpr=d[5]||6,ni=d[6],np=d[7],
ol=d[p++],O=[],I=[],pn=[],pi=[],pf=[],pp=[],pl=[],
bf=[24,26,27,29,31,32,34,36,38,41,43,46],
dv=[8192,16384,32768,49152],
nf=n=>(n=n<0?0:n>95?95:n,bf[n%12]<<(n/12)),
i,c,r,nr
for(i=0;i<ol;i++){O[i]=[];for(c=0;c<N;c++)O[i][c]=d[p++]}
for(i=0;i<ni;i++)I[i]={w:d[p],u:d[p+1],a:d[p+2],d:d[p+3],s:d[p+4],r:d[p+5],e:d[p+6]>127?d[p+6]-256:d[p+6],l:d[p+7],x:d[p+8],y:d[p+9]},p+=12
for(i=0;i<np;i++){nr=d[p++];pl[i]=nr;pn[i]=[];pi[i]=[];pf[i]=[];pp[i]=[]
for(c=0;c<N;c++){pn[i][c]=[];for(r=0;r<nr;r++)pn[i][c][r]=d[p++]}
for(c=0;c<N;c++){pi[i][c]=[];for(r=0;r<nr;r++)pi[i][c][r]=d[p++]}
for(c=0;c<N;c++){pf[i][c]=[];for(r=0;r<nr;r++)pf[i][c][r]=d[p++]}
for(c=0;c<N;c++){pp[i][c]=[];for(r=0;r<nr;r++)pp[i][c][r]=d[p++]}}
//<REV>
// optional REVB section (sits right after pattern data): 'REVB',size,damp,wet,count,count×[inst,send]
var rSize=0,rDamp=0,rWet=0,IS=[],RV=mkRev()
if(d[p]==82&&d[p+1]==69&&d[p+2]==86&&d[p+3]==66){p+=4
rSize=d[p++]/255;rDamp=d[p++]/255;rWet=d[p++]/255
for(var rn=d[p++],rk=0;rk<rn;rk++){IS[d[p]]=d[p+1];p+=2}}
function mkRev(){return{cb:[new Float32Array(1116),new Float32Array(1188),new Float32Array(1277),new Float32Array(1356)],cp:[0,0,0,0],cl:[0,0,0,0],ab:[new Float32Array(556),new Float32Array(441)],ap:[0,0],
tick:function(x,size,damp,wet){var fb=0.7+0.28*size,dc=0.4*damp,o=0,k,b,q,y,ot
for(k=0;k<4;k++){b=this.cb[k];q=this.cp[k];y=b[q];this.cl[k]=y*(1-dc)+this.cl[k]*dc;b[q]=x+this.cl[k]*fb;this.cp[k]=q+1<b.length?q+1:0;o+=y}
o*=(1-fb)*5.5
for(k=0;k<2;k++){b=this.ab[k];q=this.ap[k];y=b[q];ot=-o+y;b[q]=o+y*0.5;this.ap[k]=q+1<b.length?q+1:0;o=ot}
return o*wet}}}
//</REV>
var spt=S*5/((bpm||125)*2)|0,
mx=pl.reduce((a,b)=>a>b?a:b,16),
T=ol*mx*tpr*spt,
out=new Float32Array(T),
ch=[],ct=-1,cr=0,op=0,tc=0,sr=0,sp=0
for(c=0;c<N;c++)ch[c]={p:0,f:0,b:0,w:0,n:0,u:F/2,e:0,t:0,a:0,d:0,s:0,r:0,q:0,g:0,x:0,y:0,k:0,l:0,h:0x7FFF,j:16,m:0,i:0}
function R(){for(c=0;c<N;c++){var q=O[op][c];if(q>=np)continue
var n=pn[q][c][cr],ii=pi[q][c][cr],fx=pf[q][c][cr],pm=pp[q][c][cr],C=ch[c]
if(n==1)C.t=4;else if(n>=2){if(ii==255)ii=C.i;if(ii<ni){C.i=ii;var s=n-2,j=I[ii]
C.n=s;C.b=nf(s);C.f=C.b;C.p=0;C.w=j.w;C.u=dv[j.u&3]
C.a=j.a;C.d=j.d;C.s=j.s;C.r=j.r;C.t=1;C.e=0
C.q=j.e;C.g=j.l;C.x=j.x;C.y=j.y;C.k=0;C.l=0
//<REV>
C.rs=IS[ii]||0
//</REV>
if(j.w>=3){C.h=0x7FFF;C.m=0;C.j=256>>(s/12)||1}}}
if(fx==1){C.x=pm>>4;C.y=pm&15;C.k=0}
else if(fx==2)C.l=pm<<2;else if(fx==3)C.l=-(pm<<2);else if(fx==6)C.e=F*pm/255}}
function K(){ct++;if(ct>=tpr){ct=0;cr++;var v=pl[O[op][0]]||32
if(cr>=v){cr=0;op++;if(op>=ol)op=0}R();sr=cr;sp=op}
for(c=0;c<N;c++){var C=ch[c]
if(C.g){C.b+=C.q<<2;if(C.b<1)C.b=1;C.g--}
if(C.l){C.b+=C.l;if(C.b<1)C.b=1}
if(C.x|C.y){var n=C.n,t=C.k%3;if(t==1)n+=C.x;else if(t==2)n+=C.y;C.f=nf(n);C.k++}else C.f=C.b
var e=C.t;if(e==1){C.e+=F/(C.a+1);if(C.e>=F){C.e=F;C.t=2}}
else if(e==2){var g=F*C.s/255;C.e-=(F-g)/(C.d+1);if(C.e<=g){C.e=g;C.t=3}}
else if(e==4){C.e-=C.e/(C.r+1);if(C.e<64){C.e=0;C.t=0}}}}
for(i=0;i<T;i++){if(tc<=0){K();tc=spt}tc--
var v=0
//<REV>
var revIn=0
//</REV>
for(c=0;c<N;c++){var C=ch[c];if(!C.t&&!C.e)continue
var h=C.p,s;if(C.w==0)s=h<C.u?.5:-.5
else if(C.w==1)s=h<F/2?(h*4-F)/F:(F*3-h*4)/F
else if(C.w==2)s=(h*2-F)/F
else{C.m++;if(C.m>=C.j){C.m=0;var z=(C.h^(C.h>>1))&1;C.h=(C.h>>1)|(z<<14)}s=(C.h&1)?.5:-.5}
var cv=s*C.e/F;v+=cv
//<REV>
if(C.rs)revIn+=cv*C.rs/255
//</REV>
C.p=(C.p+C.f)%F}
//<REV>
v+=RV.tick(revIn,rSize,rDamp,rWet)
out[i]=Math.tanh(v)
//<REV-else>
//out[i]=v>1?1:v<-1?-1:v
//</REV>
}
X=X||new AudioContext({sampleRate:S})
var ab=X.createBuffer(1,T,S);ab.getChannelData(0).set(out)
var st={r:0,p:0,g:0}
return{get row(){return st.r},get pat(){return st.p},samples:out,ctx:X,spt:spt,
play(){var s=X.createBufferSource();s.buffer=ab;s.connect(X.destination);s.loop=1;s.start()
st.g=1;var t0=X.currentTime;!function u(){if(!st.g)return
var e=((X.currentTime-t0)*S)|0,k=e%T,tk=(k/spt|0)%(ol*mx*tpr)
var ro=0,po=0,tt=-1,rr=0,oo=0
for(var i=0;i<=tk;i++){tt++;if(tt>=tpr){tt=0;rr++;var v=pl[O[oo][0]]||32;if(rr>=v){rr=0;oo++;if(oo>=ol)oo=0}}}
st.r=rr;st.p=oo;requestAnimationFrame(u)}();st.s=s;return s},
stop(){st.g=0;if(st.s)try{st.s.stop()}catch(e){};X.close()}}}
