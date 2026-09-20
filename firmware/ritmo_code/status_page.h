// Pannello web servito dal dispositivo su "/" (tema Terminale, stesse pagine del display).
// Dati da /api/status (ogni 15 s) e /api/pc (all'intervallo di lettura del PC, pagine pc e home).
#pragma once
#include <Arduino.h>

static const char STATUS_PAGE[] PROGMEM = R"HTML(<!doctype html><html lang=it><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Ritmo Code</title>
<link rel=preconnect href="https://fonts.googleapis.com">
<link rel=stylesheet href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;500;800&display=swap">
<style>
:root{--bg:#141413;--s:#1B1A18;--tr:#2C2A27;--bd:#3A3834;--tx:#E8E6DF;--mu:#8E8B82;--fa:#5C5A55;
--ac:#D97757;--ok:#9BC08A;--wa:#E0B25A;--bad:#E06C5A;--bl:#7DB9D6}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);font:14px/1.45 "JetBrains Mono",ui-monospace,Menlo,Consolas,monospace;padding:18px 16px 40px}
main{max-width:820px;margin:0 auto;display:flex;flex-direction:column;gap:22px}
a{color:var(--ac);text-decoration:none}a:hover{text-decoration:underline}
.hdr{display:flex;flex-wrap:wrap;align-items:baseline;gap:4px 12px;border-bottom:1px solid var(--bd);padding-bottom:10px}
.hdr b{color:var(--ac);font-weight:500}.hdr .st{margin-left:auto;color:var(--mu)}
.tabs{display:flex;flex-wrap:wrap;gap:2px 4px;margin-top:-10px}
.tabs button{font:inherit;background:none;border:0;color:var(--fa);padding:4px 8px;cursor:pointer}
.tabs button.on{color:var(--tx)}.tabs button:hover{color:var(--mu)}
.tabs button:focus-visible,.seg button:focus-visible{outline:2px solid var(--ac);outline-offset:1px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:22px}
.box{position:relative;border:1px solid var(--bd);border-radius:4px;padding:18px 16px 14px;min-width:0}
.lg{position:absolute;top:-10px;left:10px;background:var(--bg);padding:0 6px;color:var(--mu);font-size:12px;max-width:calc(100% - 20px);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.seg{display:flex;flex-wrap:wrap;gap:6px;justify-content:flex-end;margin:-4px 0 12px}
.seg button{font:inherit;font-size:12px;background:none;color:var(--mu);border:1px solid var(--bd);border-radius:4px;padding:4px 12px;cursor:pointer;min-width:52px}
.seg button.on{color:var(--ac);border-color:var(--ac);background:#D977571f}
.pct{font-size:56px;font-weight:800;line-height:1;font-variant-numeric:tabular-nums}.pct small{font-size:22px;color:var(--mu);font-weight:400}
.blk{position:relative;display:flex;gap:2px;margin:14px 0 10px;height:18px}.blk i{flex:1;background:var(--tr)}
.mark{position:absolute;top:-4px;bottom:-4px;width:2px;background:var(--tx)}
.row{display:flex;justify-content:space-between;gap:10px;color:var(--mu);font-size:12px}
.cd{font-size:22px;font-weight:500;margin-top:4px;font-variant-numeric:tabular-nums}
.prompt{color:var(--mu)}.prompt:before{content:"> ";color:var(--ac)}
.k{color:var(--mu)}.f{color:var(--fa)}.big{font-size:20px;color:var(--tx)}
svg{display:block;width:100%;height:auto}
table{width:100%;border-collapse:collapse;font-size:13px}td{padding:7px 4px;border-bottom:1px solid var(--tr)}
td.r{text-align:right;white-space:nowrap}td.id{color:var(--fa);word-break:break-all}
.bars{display:flex;align-items:flex-end;gap:3px;height:130px;border-bottom:1px solid var(--bd)}
.bars i{flex:1;min-height:2px;background:var(--ac);position:relative}
.bars i span{position:absolute;top:-18px;left:-10px;right:-10px;text-align:center;font-size:11px;font-style:normal;color:var(--mu)}
.hrs{display:flex;justify-content:space-between;color:var(--fa);font-size:11px;margin-top:4px}
.clock{font-size:clamp(56px,14vw,96px);font-weight:500;line-height:1;letter-spacing:-2px;font-variant-numeric:tabular-nums}
.wx{display:flex;gap:14px;align-items:center}.wx svg{width:52px;flex:0 0 52px}
.temp{font-size:48px;font-weight:800;line-height:1}
.lines div{padding:3px 0}.mini{display:grid;grid-template-columns:auto 1fr auto;gap:6px 12px;align-items:center}
.mini .blk{margin:0;height:14px}
.spark{height:60px;margin-top:10px}
.pre{white-space:pre;overflow-x:auto;color:var(--mu)}
.foot{display:flex;flex-wrap:wrap;gap:8px 20px;font-size:13px;border-top:1px solid var(--bd);padding-top:12px}
.menu{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px;border-top:1px solid var(--bd);padding-top:14px;margin-top:6px}
.mg{display:flex;flex-direction:column;gap:4px}
.mt{font-size:12px;color:var(--mu);text-transform:lowercase}
.mg a{font-size:13px}
.err{color:var(--bad)}[hidden]{display:none!important}
</style></head><body><main>
<div class=hdr><b>&#10043; ritmo-code</b><span id=path>/home</span><span id=acct class=k></span><span class=st id=upd>connessione...</span></div>
<nav class=tabs id=tabs></nav>

<section data-t=home>
 <div class=grid>
  <div class=box><span class=lg id=hmcity>ora</span><div class=clock id=hmclock>--:--</div><div class=k id=hmdate style="margin-top:8px"></div></div>
  <div class=box><span class=lg>meteo</span><div class=wx><span id=wxicon></span><div class=temp id=wxtemp>--</div></div>
   <div class=lines style="margin-top:10px"><div id=wxtoday></div><div id=wxtom class=k></div><div id=wxrain></div><div id=wxsun class=f></div></div></div>
 </div>
 <div class=box style="margin-top:22px"><span class=lg>claude</span><div class=mini id=hmclaude></div></div>
 <div class=box style="margin-top:22px"><span class=lg id=hmpclg>pc</span><div id=hmpc class=k></div></div>
</section>

<section data-t=ora hidden>
 <div class=grid>
  <div class=box><span class=lg>finestra 5h</span><div class=pct id=p5>--</div><div class=blk id=b5></div>
   <div class=row><span>reset tra</span><span id=a5></span></div><div class=cd id=c5>--</div></div>
  <div class=box><span class=lg>settimana</span><div class=pct id=p7>--</div><div class=blk id=b7></div>
   <div class=row><span>reset tra</span><span id=a7></span></div><div class=cd id=c7>--</div></div>
 </div>
 <div class=prompt id=line style="margin-top:18px">stato --</div>
</section>

<section data-t=modelli hidden>
 <div class=box><span class=lg>modelli sondati</span><div class=prompt id=msum style="margin-bottom:8px"></div><table id=mt></table></div>
</section>

<section data-t=5h hidden>
 <div class=box><span class=lg>andamento</span><div class=seg id=rg></div><svg id=ch viewBox="0 0 720 200" role=img aria-label="andamento utilizzo"></svg>
  <div class=row style="justify-content:flex-start;gap:16px;margin-top:6px"><span style="color:var(--ac)">&#9472; 5h</span><span>&#9476; settimana</span><span id=span class=f></span></div></div>
</section>

<section data-t=ritmo hidden>
 <div class=box><span class=lg>ritmo orario</span><div class=seg id=hm></div><div class=bars id=bars></div>
  <div class=hrs><span>0h</span><span>6h</span><span>12h</span><span>18h</span><span>23h</span></div>
  <div class=prompt style="margin-top:10px">quota 5h consumata per ora locale</div></div>
</section>

<section data-t=settimane hidden>
 <div class=box><span class=lg>picco settimanale &middot; ultime 8</span><div class=bars id=wk style="height:150px;gap:10px;margin-top:18px"></div>
  <div class=hrs id=wkd style="gap:10px"></div><div class=prompt id=wkc style="margin-top:10px"></div></div>
</section>

<section data-t=pc hidden>
 <div class=grid>
  <div class=box><span class=lg id=cpulg>cpu</span><div class=big id=cpuv>--</div><div class=k id=cpus></div><svg class=spark id=sp0 viewBox="0 0 300 60" preserveAspectRatio=none></svg></div>
  <div class=box><span class=lg id=gpulg>gpu</span><div class=big id=gpuv>--</div><div class=k id=gpus></div><svg class=spark id=sp1 viewBox="0 0 300 60" preserveAspectRatio=none></svg></div>
  <div class=box><span class=lg id=ramlg>ram</span><div class=big id=ramv>--</div><svg class=spark id=sp2 viewBox="0 0 300 60" preserveAspectRatio=none></svg></div>
  <div class=box><span class=lg>rete &middot; disco</span><div class=big id=netv>--</div><div class=k id=diskv></div></div>
 </div>
 <div class=box style="margin-top:22px"><span class=lg>sistema</span><div id=sysv class=k>--</div></div>
</section>

<div class=menu>
 <div class=mg><span class=mt>claude</span><a href="/models">modelli &middot; modifica ID</a></div>
 <div class=mg><span class=mt>home e meteo</span><a href="/home#citta">citta' del meteo</a></div>
 <div class=mg><span class=mt>calendario</span><a href="/home#cal">link iCal (segreto)</a></div>
 <div class=mg><span class=mt>rete e pc</span><a href="/home#pc">indirizzo del pc</a><a href="/home#kwh">prezzo dell'energia</a></div>
 <div class=mg><span class=mt>sistema</span><a href="/update">aggiorna firmware</a><span id=fw class=f></span></div>
</div>
</main>
<script>
var D=null,PC=null,off=0,NB=19,lastOk=0,failing=false;
var $=function(i){return document.getElementById(i)};
function pref(k,d){try{var v=localStorage.getItem(k);return v===null?d:v}catch(e){return d}}
function save(k,v){try{localStorage.setItem(k,v)}catch(e){}}
var TABS=[['home','/home'],['ora','/usage'],['modelli','/models'],['5h','/window'],['ritmo','/rhythm'],['settimane','/weeks'],['pc','/pc']];
var TAB=pref('tab','home'),RANGE=+pref('rg',2),HM=+pref('hm',-1);
var RG=[['6h',21600],['24h',86400],['tutto',0]],HMN=['oggi','7g','30g','tutto'];
function esc(s){return String(s==null?'':s).replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function lvl(p){return p<50?'var(--ok)':p<80?'var(--ac)':'var(--bad)'}
function nowS(){return Date.now()/1000+off}
function eta(e){if(!e)return'--';var s=e-nowS();if(s<=0)return'ora';var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60),x=Math.floor(s%60);
 return d?d+'g '+h+'h '+m+'m':h?h+'h '+('0'+m).slice(-2)+'m':m+'m '+('0'+x).slice(-2)+'s'}
function hm(e){return e?new Date(e*1000).toLocaleTimeString('it-IT',{hour:'2-digit',minute:'2-digit'}):'--:--'}
function clk(e){if(!e)return'';var t=new Date(e*1000);return t.toLocaleDateString('it-IT',{weekday:'short'})+' '+hm(e)}
function blocks(el,p,mk,n){n=n||NB;var f=Math.round(p/100*n);if(p>0.5&&!f)f=1;var h='';for(var i=0;i<n;i++)h+='<i'+(i<f?' style="background:'+lvl(p)+'"':'')+'></i>';
 if(mk>=0)h+='<span class=mark style="left:calc('+(mk*100).toFixed(2)+'% - 1px)"></span>';el.innerHTML=h}
function seg(id,names,cur,fn){$(id).innerHTML=names.map(function(n,i){return '<button'+(i===cur?' class=on':'')+' data-i='+i+'>'+n+'</button>'}).join('');
 $(id).onclick=function(e){var i=e.target.getAttribute('data-i');if(i!==null)fn(+i)}}
function rate(b){return b>=1048576?(b/1048576).toFixed(1)+' MB/s':b>=1024?Math.round(b/1024)+' KB/s':Math.round(b)+' B/s'}
function shortName(s){s=String(s||'').replace(/ Processor|\(R\)|\(TM\)|NVIDIA GeForce |NVIDIA |AMD Radeon | Graphics/g,'');return s.replace(/ \d+-Core.*$/,'').trim().toLowerCase()}

// ---- schede ----
function tabs(){$('tabs').innerHTML=TABS.map(function(t){return '<button data-t="'+t[0]+'"'+(t[0]===TAB?' class=on':'')+'>'+(t[0]===TAB?'['+t[0]+']':t[0])+'</button>'}).join('');
 document.querySelectorAll('section').forEach(function(s){s.hidden=s.getAttribute('data-t')!==TAB});
 $('path').textContent=(TABS.filter(function(t){return t[0]===TAB})[0]||TABS[0])[1]}
$('tabs').onclick=function(e){var t=e.target.getAttribute('data-t');if(!t)return;TAB=t;save('tab',t);tabs();render();loadPc()};

// ---- meteo ----
function wxGroup(c){return c===0?0:c<=2?1:c===3?2:(c===45||c===48)?3:((c>=71&&c<=77)||c===85||c===86)?5:c>=95?6:c>=51?4:2}
function wxDesc(c){var g=wxGroup(c);return['sereno','poco nuvoloso','nuvoloso','nebbia',c>=80?'rovesci':c<60?'pioviggine':'pioggia','neve','temporale'][g]}
function wxSvg(c,day){var g=wxGroup(c),s='<svg viewBox="0 0 44 44">',R=function(x,y,w,h,r,col){return '<rect x='+x+' y='+y+' width='+w+' height='+h+' rx='+r+' fill="'+col+'"/>'},
 cloud=function(x,y,col){return R(x+2,y+12,34,14,7,col)+R(x+8,y+3,17,17,8,col)+R(x+19,y+7,14,14,7,col)},W='#E0B25A',T='#E8E6DF',M='#8E8B82',F='#5C5A55',B='#141413';
 if(g===0)s+=day?R(12,10,20,20,10,W)+R(21,1,2,6,0,W)+R(21,33,2,6,0,W)+R(3,19,6,2,0,W)+R(35,19,6,2,0,W):R(11,8,24,24,12,T)+R(19,3,22,22,11,B);
 else if(g===1)s+=(day?R(2,0,18,18,9,W):R(2,0,18,18,9,T)+R(8,-3,16,16,8,B))+cloud(6,8,M);
 else if(g===3)s+=R(4,10,36,3,1,M)+R(8,18,32,3,1,M)+R(4,26,36,3,1,M);
 else{s+=cloud(3,0,g===2?M:F);if(g===4)s+=R(11,30,2,8,1,'#7DB9D6')+R(20,32,2,8,1,'#7DB9D6')+R(29,30,2,8,1,'#7DB9D6');
  if(g===5)s+=R(10,32,4,4,2,T)+R(20,35,4,4,2,T)+R(30,32,4,4,2,T);if(g===6)s+='<polyline points="20,26 14,34 22,34 16,42" fill=none stroke="'+W+'" stroke-width=3 />'}
 return s+'</svg>'}

// ---- render ----
function week(){var re=D.d7_reset;if(!re)return -1;var used=D.d7;if(re<=nowS())used=0;while(re<=nowS())re+=604800;
 return {f:Math.min(1,Math.max(0,1-(re-nowS())/604800)),used:used}}
function forecast(w){if(w===-1||w.f<=0.02)return null;var hi=w.f*168,hl=(1-w.f)*168,rt=w.used/hi;
 if(w.used>=99.5)return['quota settimanale esaurita','var(--bad)'];
 if(rt>0&&(100-w.used)/rt<hl)return['a questo ritmo finisce '+clk(nowS()+(100-w.used)/rt*3600),'var(--bad)'];
 var at=Math.min(100,Math.round(w.used+rt*hl));return['al reset arrivi al ~'+at+'%',at>=90?'var(--wa)':'var(--ok)']}
function home(){var d=D,w=d.wx||{};
 var t=new Date(nowS()*1000);$('hmclock').textContent=('0'+t.getHours()).slice(-2)+':'+('0'+t.getMinutes()).slice(-2);
 $('hmdate').textContent=t.toLocaleDateString('it-IT',{weekday:'long',day:'numeric',month:'long'});
 $('hmcity').textContent=(w.city||'ora').toLowerCase();
 if(w.ok){$('wxicon').innerHTML=wxSvg(w.code,w.day);$('wxtemp').textContent=Math.round(w.temp)+'°';
  $('wxtoday').textContent=wxDesc(w.code)+' · '+Math.round(w.tmax)+'°/'+Math.round(w.tmin)+'°';
  $('wxtom').textContent='domani '+Math.round(w.tmax2)+'°/'+Math.round(w.tmin2)+'° '+wxDesc(w.code2);
  var f=-1,b=0,i;for(i=0;i<12;i++){if(w.rain[i]>b)b=w.rain[i];if(f<0&&w.rain[i]>=40)f=i}
  $('wxrain').innerHTML=f>=0?'<span style="color:var(--bl)">pioggia '+w.rain[f]+'% alle '+('0'+((w.rain_h0+f)%24)).slice(-2)+'</span>':b>=20?'pioggia possibile '+b+'%':'niente pioggia nelle 12h';
  $('wxsun').textContent='sole '+w.sunrise+'-'+w.sunset}
 else $('wxtoday').textContent='meteo in arrivo...';
 var st={allowed:['ok','var(--ok)'],allowed_warning:['attenzione','var(--wa)'],rejected:['bloccato','var(--bad)']}[d.status]||['--','var(--mu)'];
 var ok=d.models.filter(function(m){return m.mood===1}).length,fc=forecast(week());
 $('hmclaude').innerHTML=[['5h',d.h5,'reset '+hm(d.h5_reset)],['sett.',d.d7,'reset tra '+eta(d.d7_reset)]].map(function(r){
  return '<span class=k>'+r[0]+'</span><span class=blk data-p="'+r[1]+'"></span><span><b style="color:'+lvl(r[1])+';font-weight:500">'+Math.round(r[1])+'%</b> <span class=k>'+r[2]+'</span></span>'}).join('')+
  '<span></span><span class=k>stato <span style="color:'+st[1]+'">'+st[0]+'</span> · modelli '+ok+'/'+d.models.length+' ok'+(d.paused?' · <span style="color:var(--wa)">richieste in pausa</span>':'')+'</span><span>'+(fc?'<span style="color:'+fc[1]+'">'+fc[0]+'</span>':'')+'</span>';
 $('hmclaude').querySelectorAll('.blk').forEach(function(e){blocks(e,+e.getAttribute('data-p'),-1,24)});
 var p=PC||d.pc||{};$('hmpclg').textContent='pc · '+(p.host||'non collegato');
 $('hmpc').innerHTML=p.ok?'cpu <b class=big style="font-size:15px">'+Math.round(p.cpu)+'%</b>'+(p.cpu_t?' '+Math.round(p.cpu_t)+'°':'')+
  ' &nbsp; ram <b class=big style="font-size:15px">'+Math.round(p.ram)+'%</b> &nbsp; gpu <b class=big style="font-size:15px">'+Math.round(p.gpu)+'%</b>'+(p.gpu_t?' '+Math.round(p.gpu_t)+'°':'')+
  ' &nbsp; disco <b class=big style="font-size:15px">'+Math.round(p.disk)+'%</b>':'<span class=f>pc spento o Ritmo Code PC Monitor non attivo</span>'}
function ora(){var d=D;
 [['5',d.h5],['7',d.d7]].forEach(function(x){var p=Math.round(x[1]);$('p'+x[0]).innerHTML=(d.ok?p:'--')+'<small>%</small>';$('p'+x[0]).style.color=lvl(p)});
 var w=week();blocks($('b5'),d.h5,-1);blocks($('b7'),w===-1?d.d7:w.used,w===-1?-1:w.f);
 $('a5').textContent=clk(d.h5_reset);$('a7').textContent=clk(d.d7_reset);
 var st={allowed:['ok','var(--ok)'],allowed_warning:['attenzione','var(--wa)'],rejected:['bloccato','var(--bad)']}[d.status]||['--','var(--mu)'];
 var ln='stato <span style="color:'+st[1]+'">'+st[0]+'</span>';
 if(w!==-1){var df=Math.round(w.used-w.f*100),c=df<=0?'var(--ok)':df<=15?'var(--wa)':'var(--bad)';
  ln+=' &nbsp;·&nbsp; ritmo sett. <span style="color:'+c+'">'+(df===0?'in linea':(df>0?'+':'')+df+'%')+'</span>';
  var fc=forecast(w);if(fc)ln+=' &nbsp;·&nbsp; <span style="color:'+fc[1]+'">'+fc[0]+'</span>'}
 $('line').innerHTML=ln;tick()}
function modelli(){var MS={0:['--','var(--fa)'],1:['ok','var(--ok)'],2:['429','var(--wa)'],3:['errore','var(--bad)'],4:['id?','var(--wa)']};
 var ok=D.models.filter(function(m){return m.mood===1}).length,lim=D.models.filter(function(m){return m.mood===2}).length;
 $('msum').textContent=ok+' disponibili · '+lim+' limitati';
 $('mt').innerHTML=D.models.map(function(m){var s=MS[m.mood],c=m.code;var lab=m.mood==3&&c>0?String(c):m.mood==3&&c<0?'rete':m.mood==3&&!m.up?'down':s[0];
  var age=m.age<0?'':m.age<60?'ora':Math.floor(m.age/60)+'m fa';
  return '<tr><td>'+esc(m.name.toLowerCase())+'</td><td class=id>'+esc(m.id)+'</td><td class=r style="color:'+s[1]+'">'+lab+'</td><td class=r>'+(m.ms?(m.ms/1000).toFixed(1)+'s':'')+'</td><td class="r f">'+age+'</td></tr>'}).join('')}
function chart(){var all=D.hist,h=all;seg('rg',RG.map(function(r){return r[0]}),RANGE,function(i){RANGE=i;save('rg',i);chart()});
 if(RG[RANGE][1]&&all.length){var lim=all[all.length-1][0]-RG[RANGE][1];h=all.filter(function(r){return r[0]>=lim})}
 var W=720,H=200,L=34,B=22,T=8,s='';
 for(var g=0;g<=100;g+=50){var y=T+(H-T-B)*(1-g/100);s+='<line x1='+L+' x2='+W+' y1='+y+' y2='+y+' stroke="#2C2A27"/><text x='+(L-6)+' y='+(y+4)+' fill="#5C5A55" font-size=11 text-anchor=end>'+g+'%</text>'}
 if(h.length>1){var t0=h[0][0],t1=h[h.length-1][0]||t0+1,sp=Math.max(1,t1-t0);
  var pt=function(k){return h.map(function(r){return (L+(W-L)*(r[0]-t0)/sp).toFixed(1)+','+(T+(H-T-B)*(1-r[k]/100)).toFixed(1)}).join(' ')};
  s+='<polyline fill=none stroke="#8E8B82" stroke-width=1.5 stroke-dasharray="4 4" points="'+pt(2)+'"/><polyline fill=none stroke="#D97757" stroke-width=2 points="'+pt(1)+'"/>';
  var f=function(e){return new Date(e*1000).toLocaleString('it-IT',{day:'2-digit',month:'2-digit',hour:'2-digit',minute:'2-digit'})};
  s+='<text x='+L+' y='+(H-4)+' fill="#5C5A55" font-size=11>'+f(t0)+'</text><text x='+W+' y='+(H-4)+' fill="#5C5A55" font-size=11 text-anchor=end>'+f(t1)+'</text>';
  $('span').textContent=h.length+' campioni'}
 else{s+='<text x='+(W/2)+' y='+(H/2)+' fill="#5C5A55" font-size=13 text-anchor=middle>nessun dato nel periodo</text>';$('span').textContent=''}
 $('ch').innerHTML=s}
function ritmo(){var m=HM<0?D.heat_mode:HM;seg('hm',HMN,m,function(i){HM=i;save('hm',i);ritmo()});
 var v=D.heat[m],mx=Math.max.apply(null,v.concat([1])),hr=new Date(nowS()*1000).getHours();
 $('bars').innerHTML=v.map(function(x,i){var r=x/mx;return '<i title="'+i+':00 · '+x.toFixed(1)+'%" style="height:'+(2+r*98).toFixed(1)+'%;'+
  (i===hr?'background:var(--tx)':'opacity:'+(0.3+r*0.7).toFixed(2))+'"></i>'}).join('')}
function settimane(){var w=(D.weeks||[]).slice(-8),s=0,m=0,h='',dd='';
 w.forEach(function(r,i){var cur=i===w.length-1,t=new Date((r[0]-604800)*1000);
  h+='<i style="height:'+(2+r[1]*0.98)+'%;background:'+lvl(r[1])+';opacity:'+(cur?1:.7)+'"><span style="color:var(--'+(cur?'tx':'mu')+')">'+r[1]+'%</span></i>';
  dd+='<span style="flex:1;text-align:center;'+(cur?'color:var(--tx)':'')+'">'+(cur?'ora':('0'+t.getDate()).slice(-2)+'/'+('0'+(t.getMonth()+1)).slice(-2))+'</span>';
  if(!cur){s+=r[1];m=Math.max(m,r[1])}});
 $('wk').innerHTML=h;$('wkd').innerHTML=dd;
 $('wkc').textContent=w.length<2?'raccolta dati: ogni settimana conclusa aggiunge una barra':'media '+Math.round(s/(w.length-1))+'% · massimo '+m+'% (settimane concluse)'}
function spark(id,arr,col){var n=arr.length;if(!n){$(id).innerHTML='';return}
 var pts=arr.map(function(v,k){return (300-(n-1-k)*300/239).toFixed(1)+','+(60-v*0.6).toFixed(1)}).join(' ');
 $(id).innerHTML='<line x1=0 x2=300 y1=59.5 y2=59.5 stroke="#2C2A27"/><polyline points="'+pts+'" fill=none stroke="'+col+'" stroke-width=2 vector-effect=non-scaling-stroke />'}
function pc(){var p=PC;if(!p)return;var ok=p.ok,h=p.hist||[[],[],[]];
 $('cpulg').textContent='cpu'+(p.cpu_name?' · '+shortName(p.cpu_name):'');
 $('cpuv').textContent=ok?Math.round(p.cpu)+'%  '+(p.cpu_mhz/1000).toFixed(2)+' GHz'+(p.cpu_t?'  '+Math.round(p.cpu_t)+'°':'')+(p.cpu_w?'  '+Math.round(p.cpu_w)+'W':''):'--';
 $('cpus').textContent=ok&&p.core_max>=0?'core max '+Math.round(p.core_max)+'%'+(p.volt?' · '+p.volt.toFixed(2)+' V':''):'';
 $('gpulg').textContent='gpu'+(p.gpu_name?' · '+shortName(p.gpu_name):'');
 $('gpuv').textContent=ok?Math.round(p.gpu)+'%'+(p.gpu_t?'  '+Math.round(p.gpu_t)+'°':'')+(p.gpu_w?'  '+Math.round(p.gpu_w)+'W':''):'--';
 $('gpus').textContent=ok&&p.vram_total_gb?'vram '+p.vram_used_gb.toFixed(1)+' / '+Math.round(p.vram_total_gb)+' GB'+(p.gpu_hot?' · hot '+Math.round(p.gpu_hot)+'°':''):'';
 var rt=Math.max.apply(null,(p.ram_t||[]).concat([0]));
 $('ramlg').textContent='ram'+(ok&&p.ram_total_gb?' · '+p.ram_used_gb.toFixed(1)+' / '+Math.round(p.ram_total_gb)+' GB':'')+(ok&&rt?' · '+Math.round(rt)+'°':'');
 $('ramv').textContent=ok?Math.round(p.ram)+'%':'--';
 $('netv').innerHTML=ok?'↓ '+rate(p.net_down)+' &nbsp; ↑ '+rate(p.net_up):'--';
 $('diskv').textContent=ok?'disco r '+rate(p.disk_r)+' · w '+rate(p.disk_w):'';
 spark('sp0',h[0],'#D97757');spark('sp1',h[1],'#9BC08A');spark('sp2',h[2],'#7DB9D6');
 if(!ok){$('sysv').innerHTML='<span class=f>'+(p.host?'pc spento o Ritmo Code PC Monitor non attivo':'nessun pc collegato: usa Ritmo Code PC Monitor')+'</span>';return}
 var up=p.uptime,upt=up>=86400?Math.floor(up/86400)+'g '+Math.floor(up%86400/3600)+'h':Math.floor(up/3600)+'h '+Math.floor(up%3600/60)+'m';
 var watt=(p.cpu_w||0)+(p.gpu_w||0),eur=(watt*24/1000*p.kwh).toFixed(2).replace('.',',');
 var dt=0,life=101;(p.drives||[]).forEach(function(d){if(d[1]>dt)dt=d[1];if(d[2]>=0&&d[2]<life)life=d[2]});
 var gb=function(v){return v>=1000?(v/1024).toFixed(1)+' TB':Math.round(v)+' GB'};
 $('sysv').innerHTML='acceso '+upt+(p.claude>=0?' · claude code '+p.claude:'')+(watt?' · '+Math.round(watt)+' W ~'+eur+' €/giorno':'')+
  (dt?' · dischi '+Math.round(dt)+'°':'')+(life<=100?' vita '+Math.round(life)+'%':'')+
  '<div style="margin-top:8px">'+(p.disks||[]).map(function(d){return d[0]+': <span style="color:'+lvl(d[1])+'">'+Math.round(d[1])+'%</span> <span class=f>'+gb(d[2])+' liberi</span>'}).join(' &nbsp; ')+'</div>'+
  '<div class=f style="margin-top:6px">'+esc(p.host)+' · lettura ogni '+(p.int>=60?'1 min':p.int+' s')+'</div>'}
function tick(){if(D){$('c5').textContent=eta(D.h5_reset);$('c7').textContent=eta(D.d7_reset);
  if(TAB==='home'){var t=new Date(nowS()*1000);$('hmclock').textContent=('0'+t.getHours()).slice(-2)+':'+('0'+t.getMinutes()).slice(-2)}}
 var ago=Math.round((Date.now()-lastOk)/1000);
 if(failing){$('upd').innerHTML='<span class=err>non raggiungibile'+(lastOk?' · dati di '+ago+'s fa':'')+' · riprovo</span>';return}
 if(!D)return;var u=Math.round(nowS()-D.updated);
 $('upd').textContent=D.paused?(D.pause_until?'pausa fino '+hm(D.pause_until):'richieste in pausa'):D.night?'notte · in pausa':D.refreshing?'aggiornamento...':!D.updated?'--':u<60?'aggiornato ora':'aggiornato '+Math.floor(u/60)+'m fa'}
function render(){if(!D)return;
 $('acct').textContent=D.account?'@'+D.account:'';$('fw').textContent='v'+D.fw;
 var f={home:home,ora:ora,modelli:modelli,'5h':chart,ritmo:ritmo,settimane:settimane,pc:pc}[TAB];
 try{f&&f()}catch(e){console.error(e)}tick()}

// ---- dati: timeout, riprova rapida in caso di errore, dati precedenti mantenuti ----
function getJSON(url){var c=window.AbortController?new AbortController():null,t=c&&setTimeout(function(){c.abort()},15000);
 return fetch(url,{cache:'no-store',signal:c&&c.signal}).then(function(r){if(t)clearTimeout(t);if(!r.ok)throw new Error(r.status);return r.json()})}
var stT=0;function loadStatus(){clearTimeout(stT);
 getJSON('/api/status').then(function(j){D=j;off=j.now-Date.now()/1000;lastOk=Date.now();failing=false;render();stT=setTimeout(loadStatus,15000)})
 .catch(function(){failing=true;tick();stT=setTimeout(loadStatus,4000)})}
var pcT=0;function loadPc(){clearTimeout(pcT);var need=TAB==='pc'||TAB==='home';
 if(!need){pcT=setTimeout(loadPc,5000);return}
 getJSON('/api/pc').then(function(j){PC=j;lastOk=Date.now();render();pcT=setTimeout(loadPc,Math.max(2000,(j.int||2)*1000))})
 .catch(function(){pcT=setTimeout(loadPc,4000)})}
tabs();loadStatus();loadPc();setInterval(tick,1000);
</script></body></html>)HTML";
