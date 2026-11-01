'use strict';
const $ = id => document.getElementById(id);
const defaults = {on:false,level:127,kelvin:2700,hue:0,saturation:0};
const fields = ['level','kelvin','hue','saturation'];
let savedNetworks = [], irNames = [];
let state = null, selected = 'all', groupId = null, toastTimer, settingsLoaded = false;
let deviceId=null, wheelPointer=null, sending=false, sendTimer;
let sceneData=null,sceneEditId=null,sceneHues=[240,320],sceneMembers=[];
const pendingControls=new Map();
const errors = {coordinator_offline:'XIAO neodpovídá. Zkontroluj napájení a propojení TX/RX.',coordinator_rejected_or_timeout:'XIAO příkaz nepotvrdilo. Zkontroluj spojení a zkus to znovu.',invalid_settings:'Zkontroluj hodnoty. Heslo Wi‑Fi musí mít 8–63 znaků; názvy nejvýše 32 bajtů.',invalid_name:'Název musí mít 1–32 bajtů (česká písmena mohou zabírat dva).',invalid_group:'Zadej název a vyber alespoň jedno světlo.',group_limit:'Můžeš vytvořit nejvýše 8 skupin.',empty_group:'Nejsou vybrané žádné žárovky.',unknown_target:'Tento cíl už neexistuje.',unknown_light:'Žárovka už není v síti.'};
async function api(path, body) {
  const response = await fetch('/api/'+path,{method:body===undefined?'GET':'POST',headers:body===undefined?{}:{'Content-Type':'application/json'},body:body===undefined?undefined:JSON.stringify(body),signal:AbortSignal.timeout(8000)});
  const data = await response.json();
  if (!response.ok) throw new Error(data.error==='invalid_alias'?'Alias musí mít 1–32 písmen bez diakritiky, číslic nebo pomlček; pomlčka nesmí být první ani poslední.':errors[data.error] || data.error || 'Příkaz se nepodařil.');
  return data;
}
function toast(message, bad=false) { clearTimeout(toastTimer); $('toast').textContent=message; $('toast').classList.toggle('error',bad); $('toast').hidden=false; toastTimer=setTimeout(()=>{$('toast').hidden=true;},5000); }
async function action(button, job) { if (button) button.disabled=true; try { await job(); } catch(e) { toast(e.message,true); } finally { if(button)button.disabled=false; } }
function node(tag, text, className) { const el=document.createElement(tag); if(text!==undefined)el.textContent=text; if(className)el.className=className; return el; }
function targetOptions() { return [{id:'all',name:'Všechny žárovky'},...(state?.groups||[]),...(state?.lights||[])]; }
function fillTargets(element, value) { element.replaceChildren(...targetOptions().map(t=>{const o=node('option',t.name);o.value=t.id;return o;})); if(element.id==='irTarget'){const custom=node('option','Vlastní výběr žárovek');custom.value='remote';element.prepend(custom);}element.value=value; if(!element.value)element.value='all'; }
function lightName(id) { return state.lights.find(l=>l.id===id)?.name || id; }
function showTab(tab) { ['lights','groups','scenes','remote','settings','updates'].forEach(t=>{$('tab-'+t).hidden=t!==tab;}); document.querySelectorAll('[data-tab]').forEach(b=>b.classList.toggle('active',b.dataset.tab===tab)); if((tab==='settings'||tab==='remote')&&!settingsLoaded)action(null,loadSettings);if(tab==='scenes'&&!sceneData)action(null,loadScenes);if(tab==='updates')action(null,refreshUpdate);if(tab==='settings')action(null,refreshBle); }
Object.assign(errors,{coordinator_update_required:'Tato funkce vyžaduje nový firmware v XIAO C6.',device_command_failed:'Žárovka příkaz nepřijala. Podrobnost je ve stavu spojení.',leave_timeout:'Žárovka nepotvrdila odpárování. Zůstává v seznamu. Zapni její napájení a zkus to znovu, nebo použij Force remove.',leave_rejected:'Žárovka odmítla odpárování. Zůstává v seznamu.'});
function labels() {
  const level=Number($('level').value)*100/254,h=Number($('hue').value),s=Number($('saturation').value);
  $('level-value').textContent=Math.round(level)+' %';$('kelvin-value').textContent=$('kelvin').value+' K';$('hue-value').textContent=h+'°';$('saturation-value').textContent=s+' %';
  $('level').style.background=`linear-gradient(90deg,#dfecac ${level}%,#39433c ${level}%)`;
  $('saturation').style.background=`linear-gradient(90deg,${whiteColor()},hsl(${h} 100% 50%))`;
  const angle=h*Math.PI/180;
  $('color-cursor').style.left=(50+Math.sin(angle)*s*.46)+'%';$('color-cursor').style.top=(50-Math.cos(angle)*s*.46)+'%';
  $('white-mode').classList.toggle('active',s===0);$('full-color').classList.toggle('active',s===100);
  drawWheel();
}
function whiteRgb() {
  const t=Number($('kelvin').value)/100;
  return [255,Math.min(255,Math.max(0,99.4708*Math.log(t)-161.1196)),t<=19?0:Math.min(255,Math.max(0,138.5177*Math.log(t-10)-305.0448))];
}
function whiteColor() { return `rgb(${whiteRgb().map(Math.round).join(' ')})`; }
let wheelTemperature;
function drawWheel() {
  if(wheelTemperature===$('kelvin').value)return;wheelTemperature=$('kelvin').value;
  const canvas=$('color-wheel').querySelector('canvas'),ctx=canvas.getContext('2d'),size=canvas.width,im=ctx.createImageData(size,size),w=whiteRgb();
  for(let y=0;y<size;y++)for(let x=0;x<size;x++) {
    const dx=x-size/2,dy=y-size/2,r=Math.hypot(dx,dy)/(size*.46);if(r>1)continue;
    const h=((Math.atan2(dx,-dy)/(Math.PI*2)+1)%1)*6,f=h-Math.floor(h);
    const rgb=[[1,f,0],[1-f,1,0],[0,1,f],[0,1-f,1],[f,0,1],[1,0,1-f]][Math.floor(h)%6],i=(y*size+x)*4;
    for(let c=0;c<3;c++)im.data[i+c]=(1-r)*w[c]+r*rgb[c]*255;
    im.data[i+3]=255;
  }
  ctx.putImageData(im,0,0);
}
function selectTarget(id) {
  selected=id; fillTargets($('target'),id); selected=$('target').value;
  const light=state.lights.find(l=>l.id===selected); const values=light?.wanted||defaults;
  fields.forEach(f=>{$(f).value=values[f];}); labels();
  $('editor-name').textContent=targetOptions().find(t=>t.id===selected)?.name||selected;
  $('rename').hidden=!light;
  const lo=Math.max(2000,light?.minKelvin||2000),hi=Math.min(6500,light?.maxKelvin||6500);
  $('kelvin').min=lo;$('kelvin').max=Math.max(lo,hi);$('kelvin').step=1;
  if(light?.wanted)$('kelvin').value=light.wanted.kelvin;
  labels();
  $('selection-note').textContent=selected==='display'?'Barevná plocha displeje je dekorativní lampička. Je-li displej uspaný, probuď jej bočním tlačítkem.':light?'Posuvníky ukazují poslední požadované hodnoty. Karta světla ukazuje stav načtený ze žárovky.':'Posuvníky nejsou průměrem skupiny. Změna odešle pouze daný parametr všem vybraným členům.';
  renderLights();
}
function renderLights() {
  const list=$('light-list'); list.replaceChildren();
  state.lights.forEach(l=>{
    const b=node('button',undefined,'light-card'+(selected===l.id?' selected':'')); b.type='button'; b.addEventListener('click',()=>selectTarget(l.id));
    const swatch=node('span',undefined,'swatch'); swatch.style.background=l.colorKnown&&l.reported&&l.on?l.color:'#333b49';
    const title=node('span');title.append(node('strong',l.name),node('small',l.id==='display'?(state.display.awake?'Obrazovka je vzhůru':'Obrazovka spí · probuď tlačítkem'):(l.online?'Zigbee · '+l.id:'Nedostupná · '+l.id)));
    const status=l.removing?'Odpárovávám…':l.removeError?'Odpárování selhalo':!l.online?'Offline':!l.reported?'Čeká na stav':l.id==='display'&&!state.display.awake?'Spí':l.id==='display'&&state.display.mode==='status'?'Stavový režim':l.on?'Svítí · '+Math.round(l.level*100/254)+' %':'Zhasnuto';
    b.append(swatch,title,node('span',status,'state'));
    const row=node('div',undefined,'device-card-row'),settings=node('button','⚙','device-settings');settings.type='button';settings.setAttribute('aria-label','Nastavení: '+l.name);
    settings.addEventListener('click',()=>{selectTarget(l.id);openDevice(l.id);});row.append(b,settings);list.append(row);
  });
}
function renderGroups() {
  $('group-list').replaceChildren();
  if(!state.groups.length)$('group-list').append(node('p','Ještě žádné skupiny. Vytvoř si třeba „Večer“ nebo „Pracovna“.','muted'));
  state.groups.forEach(g=>{
    const card=node('section',undefined,'panel group-card');card.append(node('h2',g.name),node('p',g.members.map(lightName).join(' · ')));
    const actions=node('div',undefined,'actions'),control=node('button','Ovládat','primary'),edit=node('button','Upravit');
    control.addEventListener('click',()=>{showTab('lights');selectTarget(g.id);});edit.addEventListener('click',()=>editGroup(g));actions.append(control,edit);card.append(actions);$('group-list').append(card);
  });
}
function render() {
  $('device-name').textContent=state.alias;document.title=state.alias+' · ovládání světla';
  $('alias-status').textContent=`Aktivní: ${state.alias} · http://${state.hostname}.local${state.aliasPending?' · Nový alias čeká na restart.':''}`;
  const physical=state.lights.filter(l=>l.id!=='display'),lit=physical.filter(l=>l.online&&l.reported&&l.on).length,unknown=physical.filter(l=>!l.online||!l.reported).length;
  $('connection').textContent=state.connected?'Zigbee připojeno':'XIAO je nedostupné';
  $('summary').textContent=`${lit} z ${physical.length} žárovek svítí${unknown?' · '+unknown+' bez aktuálního stavu':''}`;
  $('empty').hidden=physical.length!==0;
  $('pair-title').textContent=state.joinSeconds>0?`Síť je otevřená · zbývá ${state.joinSeconds} s`:'Připojení nové žárovky';
  if(state.joinSeconds>0)$('pairing').hidden=false;
  $('pair-start').disabled=!state.connected||state.joinSeconds>0;$('pair-stop').disabled=!state.connected||state.joinSeconds===0;
  const available=targetOptions(); if(!available.some(t=>t.id===selected))selectTarget('all');
  fillTargets($('target'),selected); const presetTarget=$('presetTarget').value;fillTargets($('presetTarget'),presetTarget); const irTarget=$('irTarget').value;fillTargets($('irTarget'),irTarget);
  $('editor-name').textContent=available.find(t=>t.id===selected)?.name||selected;
  renderLights();renderGroups();renderIrStatus();renderTimers();
  renderDeviceStatus();
  $('network-summary').textContent=state.apActive?`Hotspot: ${state.apSsid} · ${state.apIp}`:state.staConnected?`Wi‑Fi: ${state.staSsid} · ${state.staIp}`:`Hotspot vypnutý · ${state.networkCount?'hledám známé Wi‑Fi':'žádné uložené Wi‑Fi'}`;
  $('ap-status').textContent=state.apEnabled?(state.apActive?'Hotspot zapnutý':'Hotspot se zapíná'):'Hotspot vypnutý. Bez automatického zapínání.';
  $('diagnostics').textContent=`XIAO: ${state.connected?'připojeno, Zigbee kanál '+state.channel:'bez spojení'}. Zařízení na hotspotu: ${state.clients}.${state.lastError?' Poslední chyba: '+state.lastError:''}${state.wifiError?' Wi‑Fi: '+state.wifiError:''}`;
}
async function refresh() { const first=state===null;state=await api('state');render();if(first){selectTarget('all');openBluetoothLink();}$('workspace').inert=false; }
async function poll() { try {await refresh();}catch(e){$('connection').textContent='Spojení přerušeno';if(!state)$('summary').textContent='Čekám na připojení…';}finally{setTimeout(poll,2000);} }
function queueChange(patch) {
  pendingControls.set(selected,{...pendingControls.get(selected),...patch});
  $('control-status').textContent='Odesílám změnu…';
  if(!sending && !sendTimer)sendTimer=setTimeout(sendChanges,140);
}
async function sendChanges() {
  sendTimer=null;if(sending || !pendingControls.size)return;sending=true;
  const [target,patch]=pendingControls.entries().next().value;pendingControls.delete(target);
  try {await api('control',{target,patch});$('control-status').textContent='Příkaz přijat · čekám na stav žárovky';}
  catch(e){toast(e.message,true);$('control-status').textContent='Změnu se nepodařilo odeslat';}
  finally {sending=false;if(pendingControls.size)sendTimer=setTimeout(sendChanges,140);else action(null,refresh);}
}
async function change(patch) { queueChange(patch); }
async function flushControls() {
  clearTimeout(sendTimer);sendTimer=null;
  while(sending || pendingControls.size) {
    if(!sending)await sendChanges();else await new Promise(resolve=>setTimeout(resolve,30));
  }
}
function wheelMove(e) {
  const box=$('color-wheel').getBoundingClientRect(),dx=e.clientX-box.left-box.width/2,dy=e.clientY-box.top-box.height/2;
  const saturation=Math.min(100,Math.round(Math.hypot(dx,dy)/(box.width*.46)*100));
  if(saturation>0)$('hue').value=Math.round((Math.atan2(dx,-dy)*180/Math.PI+360)%360);
  $('saturation').value=saturation;labels();queueChange({hue:Number($('hue').value),saturation});
}
$('color-wheel').addEventListener('pointerdown',e=>{if(wheelPointer!==null || e.button!==0)return;wheelPointer=e.pointerId;$('color-wheel').setPointerCapture(e.pointerId);wheelMove(e);});
$('color-wheel').addEventListener('pointermove',e=>{if(e.pointerId===wheelPointer)wheelMove(e);});
$('color-wheel').addEventListener('pointerup',e=>{if(e.pointerId===wheelPointer){wheelMove(e);wheelPointer=null;}});
$('color-wheel').addEventListener('pointercancel',()=>{wheelPointer=null;});
$('white-mode').addEventListener('click',()=>{$('saturation').value=0;labels();queueChange({saturation:0});});
$('full-color').addEventListener('click',()=>{$('saturation').value=100;labels();queueChange({hue:Number($('hue').value),saturation:100});});
for(const kind of ['wake','power']) {
  for(const [key,title,min,max] of [['level','Jas (%)',1,100],['kelvin','Teplota bílé (K)',2000,6500],['hue','Odstín (°)',0,360],['saturation','Sytost (%)',0,100]]) {
    const label=node('label',title),input=node('input');input.id=kind+'-'+key;input.type='number';input.min=min;input.max=max;input.required=true;label.append(input);$(kind+'-values').append(label);
  }
  $(kind+'-mode').addEventListener('change',()=>startupFields(kind));
  $(kind+'-copy').addEventListener('click',()=>fillStartup(kind,state.lights.find(l=>l.id===deviceId)?.wanted||defaults));
  $(kind+'-form').addEventListener('submit',e=>{e.preventDefault();const id=deviceId,mode=Number($(kind+'-mode').value),v={on:true};
    ['level','kelvin','hue','saturation'].forEach(key=>{v[key]=Number($(kind+'-'+key).value);});v.level=Math.max(1,Math.round(v.level*254/100));
    action(e.submitter,async()=>{await api('startup',{id,[kind+'Mode']:mode,[kind]:v});await refresh();toast(kind==='wake'?'Běžné zapnutí uložené.':mode?'Požadavek přijat. Sleduj potvrzení žárovky.':'Nastavení žárovky ponecháno.');});
  });
}
function fillStartup(kind,v) {
  ['kelvin','hue','saturation'].forEach(key=>{$(kind+'-'+key).value=v[key];});$(kind+'-level').value=Math.max(1,Math.round(v.level*100/254));
}
function startupFields(kind) {
  const preset=Number($(kind+'-mode').value)===(kind==='wake'?1:4);
  $(kind+'-values').hidden=!preset;$(kind+'-copy').hidden=!preset;
  $(kind+'-values').querySelectorAll('input').forEach(input=>{input.disabled=!preset;});
}
function openDevice(id) {
  const l=state.lights.find(l=>l.id===id);if(!l)return;deviceId=id;
  $('device-title').textContent=l.name;$('device-label').value=l.name;
  const v=l.wanted||defaults;$('device-on').checked=v.on;
  $('device-level').value=Math.max(1,Math.round(v.level*100/254));
  ['kelvin','hue','saturation'].forEach(f=>{$('device-'+f).value=v[f];});
  $('device-kelvin').min=Math.max(2000,l.minKelvin||2000);$('device-kelvin').max=Math.min(6500,l.maxKelvin||6500);
  $('device-kelvin').value=Math.min(Number($('device-kelvin').max),Math.max(Number($('device-kelvin').min),v.kelvin));
  $('device-transition').value=(l.transition??3)/10;$('device-color').value=l.legacyXy?'xy':'native';
  $('device-config-form').hidden=id==='display';$('device-removal').hidden=id==='display';$('device-removal').open=false;
  $('display-device-note').hidden=id!=='display';
  $('device-startup').hidden=id==='display';
  for(const kind of ['wake','power']) { $(kind+'-mode').value=l.startup?.[kind+'Mode']||0;fillStartup(kind,l.startup?.[kind]||{...defaults,on:true});startupFields(kind); }
  renderDeviceStatus();$('device-dialog').showModal();
}
function renderDeviceStatus() {
  if(!deviceId)return;const l=state.lights.find(l=>l.id===deviceId);
  if(!l){$('device-dialog').close();deviceId=null;settingsLoaded=false;toast('Zařízení bylo odebráno i ze skupin.');return;}
  $('device-info').textContent=l.id==='display'?'Vestavěná obrazovka T‑Display':`${l.manufacturer||'Výrobce zatím nenačten'} · ${l.model||'model zatím nenačten'} · IEEE ${l.id}`;
  $('device-status').textContent=l.removing?'Čekám na potvrzení odpárování (nejvýše 15 s)…':l.removeError?(errors[l.removeError]||l.removeError):l.online?'Zařízení je dostupné':'Zařízení je offline. Poslední známé hodnoty mohou být zastaralé.';
  const supported=state.deviceApi>=1 && state.connected;
  ['remove-device','identify'].forEach(id=>{$(id).disabled=!supported||l.removing||!l.online;});
  $('force-remove').disabled=!supported;
  $('device-config-form').querySelector('button').disabled=!supported||l.removing;
  if(l.id!=='display' && !state.deviceApi)$('device-status').textContent=errors.coordinator_update_required;
  const startSupported=supported && state.deviceApi>=3 && !l.removing;
  $('wake-form').querySelector('.primary').disabled=!startSupported;
  $('power-form').querySelector('.primary').disabled=!startSupported || !l.online || l.powerStatus===1;
  const powerStates=['Nastavení po napájení se tímto ovladačem nemění.','Čekám na zápis a zpětné ověření…','Potvrzeno: nastavení přečtené ze žárovky odpovídá.','Nastavení není potvrzené. Některé hodnoty se mohly zapsat; po vyřešení chyby ulož znovu.','Uložená volba čeká na ověření po připojení žárovky.'];
  $('power-status').textContent=!startSupported?errors.coordinator_update_required:(powerStates[l.powerStatus||0]+(l.powerError?' '+(errors[l.powerError]||l.powerError):''));
}
$('device-close').addEventListener('click',()=>$('device-dialog').close());
$('device-dialog').addEventListener('close',()=>{deviceId=null;});
$('display-settings').addEventListener('click',()=>{$('device-dialog').close();showTab('settings');$('screen-form').scrollIntoView({behavior:'smooth'});});
$('device-name-form').addEventListener('submit',e=>{e.preventDefault();const id=deviceId;action(e.submitter,async()=>{await api('rename',{id,name:$('device-label').value.trim()});await refresh();$('device-title').textContent=lightName(id);toast('Název uložený.');});});
$('device-values-form').addEventListener('submit',e=>{e.preventDefault();const target=deviceId;const patch={on:$('device-on').checked,level:Math.max(1,Math.round(Number($('device-level').value)*254/100))};['kelvin','hue','saturation'].forEach(f=>{patch[f]=Number($('device-'+f).value);});action(e.submitter,async()=>{await flushControls();await api('control',{target,patch});await refresh();if(selected===target){fields.forEach(f=>{$(f).value=patch[f];});labels();}toast('Hodnoty odeslané.');});});
$('device-config-form').addEventListener('submit',e=>{e.preventDefault();action(e.submitter,async()=>{await api('device',{id:deviceId,transition:Math.round(Number($('device-transition').value)*10),legacyXy:$('device-color').value==='xy'});toast('Chování žárovky uložené.');await refresh();});});
$('identify').addEventListener('click',()=>action($('identify'),async()=>{await api('identify',{id:deviceId});toast('Žárovka dostala požadavek na identifikaci na 3 sekundy.');}));
for(const [button,force] of [['remove-device',false],['force-remove',true]])$(button).addEventListener('click',()=>{
  const id=deviceId,name=lightName(id),message=force?`Vynuceně odstranit „${name}“ z ovladače a skupin? Žárovka se tím nemusí odpárovat; před dalším párováním může potřebovat reset.`:`Odpárovat „${name}“? Po potvrzení žárovky se odstraní z ovladače a skupin.`;
  if(confirm(message))action($(button),async()=>{await flushControls();await api('remove',{id,force});settingsLoaded=false;toast(force?'Odstranění přijato. Čekám na seznam zařízení.':'Požadavek odeslán. Čekám na potvrzení žárovky.');await refresh();});
});
async function loadSettings() {
  const settings=await api('settings');
  ['bootOn','irEnabled'].forEach(k=>{$(k).checked=settings[k];});
  ['alias','timeout','statusBrightness','mode','apSsid','apPassword'].forEach(k=>{$(k).value=settings[k];});
  fillTargets($('presetTarget'),settings.presetTarget); fillTargets($('irTarget'),settings.irTarget);
  irNames=settings.irKeys;renderIrKeys();
  $('presetLevel').value=Math.round(settings.preset.level*100/254);
  ['Kelvin','Hue','Saturation'].forEach(k=>{$('preset'+k).value=settings.preset[k.toLowerCase()];});
  savedNetworks=settings.networks.map(n=>({...n}));renderNetworks();settingsLoaded=true;
  await loadScenes();
}
function renderIrStatus() {
  const ir=state?.ir;if(!ir)return;
  $('ir-status').textContent=ir.learningKey>=0?`Stiskni ${irNames[ir.learningKey]||'vybrané tlačítko'} na ovladači · ${ir.learningSeconds} s`:ir.sceneName?`Scéna: ${ir.sceneName} · jas ${Math.round(ir.sceneLevel*100/254)} %`:'Efekt: vypnutý';
  $('scene-running').textContent=ir.sceneName?`Právě běží: ${ir.sceneName} · jas ${Math.round(ir.sceneLevel*100/254)} %`:'Žádná scéna neběží.';$('scene-stop').disabled=!ir.sceneName;
  $('ir-last').textContent=ir.lastCode?`Poslední příjem: ${ir.lastCode} · ${irNames[ir.lastKey]||'nenamapováno'} · před ${Math.floor(ir.age)} s`:'Zatím žádný platný IR kód.';
  $('ir-cancel').disabled=ir.learningKey<0;
  document.querySelectorAll('#ir-keys button').forEach((button,i)=>{
    button.classList.toggle('active',i===ir.learningKey);button.querySelector('small').textContent=ir.codes[i]||'bez kódu';
  });
}
function renderIrKeys() {
  $('ir-keys').replaceChildren(...irNames.map((name,key)=>{
    const button=node('button');button.type='button';button.setAttribute('aria-label','Naučit '+name);
    button.append(node('span',name),node('small',''));button.addEventListener('click',()=>action(button,async()=>{await api('ir/learn',{key});await refresh();}));return button;
  }));renderIrStatus();
}
$('ir-cancel').addEventListener('click',()=>action($('ir-cancel'),async()=>{await api('ir/cancel',{});await refresh();}));
$('ir-reset').addEventListener('click',()=>{if(confirm('Obnovit výchozí mapu 24 tlačítek? Naučené kódy se přepíšou.'))action($('ir-reset'),async()=>{await api('ir/reset',{});await refresh();});});
function editGroup(group) {
  groupId=group?.id||null;$('group-name').value=group?.name||'';$('group-heading').textContent=group?'Upravit skupinu':'Nová skupina';$('delete-group').hidden=!group;
  $('group-members').replaceChildren();state.lights.forEach(l=>{const label=node('label',undefined,'check'),box=document.createElement('input');box.type='checkbox';box.value=l.id;box.checked=group?.members.includes(l.id)||false;label.append(box,document.createTextNode(l.name));$('group-members').append(label);});$('group-form').hidden=false;$('group-name').focus();
}
document.querySelectorAll('[data-tab]').forEach(b=>b.addEventListener('click',()=>showTab(b.dataset.tab)));
$('target').addEventListener('change',()=>selectTarget($('target').value));
fields.forEach(f=>{for(const event of ['input','change'])$(f).addEventListener(event,()=>{labels();queueChange({[f]:Number($(f).value)});});});
$('power-on').addEventListener('click',()=>action($('power-on'),()=>change({on:true})));$('power-off').addEventListener('click',()=>action($('power-off'),()=>change({on:false})));
$('pair-open').addEventListener('click',()=>{$('pairing').hidden=!$('pairing').hidden;});
$('pair-start').addEventListener('click',()=>action($('pair-start'),async()=>{await api('pair',{seconds:180});toast('Síť se otevírá pro párování.');await refresh();}));
$('pair-stop').addEventListener('click',()=>action($('pair-stop'),async()=>{await api('pair',{seconds:0});await refresh();}));
$('rename').addEventListener('click',()=>openDevice(selected));
$('new-group').addEventListener('click',()=>editGroup(null));$('cancel-group').addEventListener('click',()=>{$('group-form').hidden=true;});
$('group-form').addEventListener('submit',e=>{e.preventDefault();action(e.submitter,async()=>{
  const members=[...$('group-members').querySelectorAll('input:checked')].map(box=>box.value);
  await api('group',{...(groupId?{id:groupId}:{}),name:$('group-name').value,members});$('group-form').hidden=true;toast('Skupina uložená.');await refresh();settingsLoaded=false;
});});
$('delete-group').addEventListener('click',()=>{if(confirm('Smazat tuto skupinu? Žárovky zůstanou spárované.'))action($('delete-group'),async()=>{await api('group',{id:groupId,delete:true});$('group-form').hidden=true;settingsLoaded=false;await refresh();});});
function settingsForm(id, values, message) { $(id).addEventListener('submit',e=>{e.preventDefault();action(e.submitter,async()=>{await api('settings',values());toast(message);await refresh();});}); }
$('irTarget').addEventListener('change',()=>{$('ir-members-box').hidden=$('irTarget').value!=='remote';});
$('ir-form').addEventListener('submit',e=>{e.preventDefault();action(e.submitter,async()=>{
  const members=[...$('ir-members').querySelectorAll('input:checked')].map(b=>b.value);
  if($('irTarget').value==='remote' && !members.length)throw new Error('Vyber alespoň jedno světlo pro ovladač.');
  await api('remote',{irMembers:members});await api('settings',{irEnabled:$('irEnabled').checked,irTarget:$('irTarget').value});toast('Světla ovladače uložená.');await refresh();
});});
settingsForm('alias-form',()=>({alias:$('alias').value}),'Alias uložený. V síti a na displeji se použije po restartu T‑Display.');
settingsForm('screen-form',()=>({bootOn:$('bootOn').checked,timeout:Number($('timeout').value),statusBrightness:Number($('statusBrightness').value),mode:$('mode').value}),'Nastavení displeje uložené.');
settingsForm('wifi-form',()=>({apSsid:$('apSsid').value,apPassword:$('apPassword').value}),'Hotspot uložený. Název a heslo použiješ restartem T‑Display.');
settingsForm('networks-form',()=>({networks:savedNetworks}),'Pořadí sítí uložené. Při vypnutém hotspotu začíná nové hledání; spojení se může přerušit.');
settingsForm('preset-form',()=>({presetTarget:$('presetTarget').value,preset:{on:true,level:Math.max(1,Math.round(Number($('presetLevel').value)*254/100)),kelvin:Number($('presetKelvin').value),hue:Number($('presetHue').value),saturation:Number($('presetSaturation').value)}}),'Preset tlačítka uložený.');
function renderNetworks() {
  $('network-list').replaceChildren();
  savedNetworks.forEach((network,i)=>{
    const box=node('fieldset');box.append(node('legend',`Priorita ${i+1}`));
    for(const [key,title] of [['ssid','Název sítě'],['password','Heslo']]) {
      const label=node('label',title),input=node('input');input.value=network[key];input.required=true;input.autocomplete='off';input.dataset.field=key;
      input.type=key==='password'?'password':'text';input.maxLength=key==='ssid'?32:63;if(key==='password')input.minLength=8;
      input.addEventListener('input',()=>{network[key]=input.value;});label.append(input);box.append(label);
    }
    const actions=node('div',undefined,'actions');
    for(const [title,delta] of [['Nahoru',-1],['Dolů',1],['Odebrat',0]]) {
      const button=node('button',title);button.type='button';button.disabled=delta<0?i===0:delta>0?i===savedNetworks.length-1:false;
      button.addEventListener('click',()=>{if(delta)[savedNetworks[i],savedNetworks[i+delta]]=[savedNetworks[i+delta],savedNetworks[i]];else savedNetworks.splice(i,1);renderNetworks();});actions.append(button);
    }
    box.append(actions);$('network-list').append(box);
  });
  $('add-network').disabled=savedNetworks.length>=5;
}
$('add-network').addEventListener('click',()=>{if(savedNetworks.length<5){savedNetworks.push({ssid:'',password:''});renderNetworks();}});
$('reboot').addEventListener('click',()=>{if(confirm('Restartovat T‑Display a použít uložené nastavení Wi‑Fi?'))action($('reboot'),async()=>{await api('reboot',{});toast('Restart za dvě sekundy. Hotspot zůstane v posledním ručně zvoleném stavu.');});});
Object.assign(errors,{invalid_remote:'Zkontroluj bílou, počet stupňů a přiřazené scény.',invalid_scene:'Zkontroluj název, časování, paletu a světla scény. Jeden krok musí trvat alespoň 0,5 s.',empty_scene:'Scéna nemá žádné světlo. Uprav její seznam.',unknown_scene:'Tato scéna už neexistuje.',scene_missing_device:'Některé světlo scény už není spárované. Uprav seznam světel.',scene_limit:'Můžeš uložit nejvýše 8 scén.'});
Object.assign(errors,{scene_changed:'Seznam světel se během startu změnil. Spusť scénu znovu.',invalid_startup:'Neplatné výchozí nastavení.',startup_info_missing:'Počkej na načtení vlastností žárovky.',startup_color_unsupported:'Tato žárovka nepodporuje barevný start Hue.',startup_busy:'Předchozí zápis ještě probíhá.',startup_attribute_unsupported:'Žárovka tento atribut nepodporuje.',startup_write_rejected:'Žárovka zápis odmítla.',startup_readback_mismatch:'Přečtené hodnoty nesouhlasí.',startup_readback_invalid:'Žárovka vrátila neplatnou hodnotu.',startup_timeout:'Žárovka včas neodpověděla.'});
function hueHex(h) {
  const f=h/60%1,rgb=[[1,f,0],[1-f,1,0],[0,1,f],[0,1-f,1],[f,0,1],[1,0,1-f]][Math.floor(h/60)%6];
  return '#'+rgb.map(v=>Math.round(v*255).toString(16).padStart(2,'0')).join('');
}
function hexHue(hex) {
  const [r,g,b]=[1,3,5].map(i=>parseInt(hex.slice(i,i+2),16)/255),hi=Math.max(r,g,b),lo=Math.min(r,g,b),d=hi-lo;
  if(d<.03)return null;
  const h=hi===r?(g-b)/d:hi===g?2+(b-r)/d:4+(r-g)/d;return (Math.round(h*60)+360)%360;
}
function recommendedScale(count) {
  if(count===10)return [1,2,4,7,12,20,32,50,75,100];
  let previous=0;return Array.from({length:count},(_,i)=>{previous=Math.min(100-(count-1-i),Math.max(previous+1,Math.round(100**(i/(count-1)))));return previous;});
}
function brightnessValues() {return [...$('brightness-levels').querySelectorAll('input')].map(input=>Number(input.value));}
function brightnessScale(reset=true) {
  const count=Math.min(32,Math.max(2,Math.floor(Number($('brightness-steps').value)||10))),custom=$('brightness-mode').value==='custom';
  if(reset) {
    const values=recommendedScale(count);$('brightness-levels').replaceChildren(...values.map((v,i)=>{const label=node('label',`Krok ${i+1} (%)`),input=node('input');input.type='number';input.min=1;input.max=100;input.required=true;input.value=v;input.addEventListener('input',()=>brightnessScale(false));label.append(input);return label;}));
  }
  $('brightness-levels').hidden=!custom;$('brightness-levels').querySelectorAll('input').forEach(i=>{i.disabled=!custom;});
  const values=custom?brightnessValues():recommendedScale(count);$('brightness-scale').textContent=values.map(v=>v+' %').join(' → ');
}
async function loadScenes() {
  sceneData=await api('scenes');renderSceneList();renderRemote();
}
function renderRemote() {
  $('white-kelvin').value=sceneData.whiteKelvin;$('white-level').value=Math.max(1,Math.round(sceneData.whiteLevel*100/254));
  $('brightness-steps').value=sceneData.brightnessSteps;$('brightness-mode').value=sceneData.customBrightness?'custom':'auto';brightnessScale();
  if(sceneData.customBrightness){[...$('brightness-levels').querySelectorAll('input')].forEach((input,i)=>{input.value=sceneData.brightnessLevels[i];});brightnessScale(false);}
  $('ir-members').replaceChildren(...state.lights.map(l=>{const label=node('label',undefined,'check'),box=node('input');box.type='checkbox';box.value=l.id;box.checked=sceneData.irMembers?.includes(l.id)||false;label.append(box,document.createTextNode(l.name));return label;}));$('ir-members-box').hidden=$('irTarget').value!=='remote';
  $('remote-buttons').replaceChildren(...['FLASH','STROBE','FADE / MODE','SMOOTH'].map((name,i)=>{
    const label=node('label',name),select=node('select');select.id='remote-button-'+i;
    const off=node('option','Bez akce');off.value='';select.append(off);
    sceneData.scenes.forEach(s=>{const option=node('option',s.name);option.value=s.id;select.append(option);});select.value=sceneData.buttons[i]||'';label.append(select);return label;
  }));
}
function renderSceneList() {
  $('scene-list').replaceChildren(...sceneData.scenes.map(s=>{
    const card=node('section',undefined,'panel scene-card');card.append(node('h2',s.name),node('p',sceneData.modes[s.mode].description));
    card.append(node('p',`${Math.round(s.level*100/254)} % · výdrž ${s.hold/10} s · přechod ${s.fade/10} s`,'hint'));
    card.append(node('p',s.useIr?'Světla podle cíle IR ovladače':s.members.map(m=>lightName(m[0])).join(' · ')||'Chybí světla','hint'));
    const actions=node('div',undefined,'actions'),run=node('button','Spustit','primary'),edit=node('button','Upravit scénu');
    run.addEventListener('click',()=>action(run,async()=>{await flushControls();await api('scene/run',{id:s.id});await refresh();}));
    edit.addEventListener('click',()=>editScene(s));actions.append(run,edit);card.append(actions);return card;
  }));$('new-scene').disabled=sceneData.scenes.length>=8;
}
function editScene(s) {
  sceneEditId=s?.id||null;$('scene-heading').textContent=s?'Upravit scénu':'Nová scéna';$('scene-name').value=s?.name||'';
  $('scene-mode').replaceChildren(...sceneData.modes.map((mode,i)=>{const o=node('option',mode.name);o.value=i;return o;}));
  $('scene-mode').value=s?.mode??0;$('scene-level').value=Math.max(1,Math.round((s?.level??127)*100/254));
  $('scene-hold').value=(s?.hold??0)/10;$('scene-fade').value=(s?.fade??20)/10;
  $('scene-scope').value=s?.useIr===false?'members':'ir';$('scene-stagger').checked=s?.stagger||false;
  sceneHues=[...(s?.hues||[240,320])];sceneMembers=(s?.members||[]).map(m=>[...m]);
  $('scene-delete').hidden=!s;$('scene-form').hidden=false;sceneMode();sceneScope();renderPalette();renderSceneMembers();$('scene-name').focus();
}
function sceneMode() {
  const mode=Number($('scene-mode').value);$('scene-description').textContent=sceneData.modes[mode].description;$('scene-palette-box').hidden=mode!==12;
}
function sceneScope() { const explicit=$('scene-scope').value==='members';$('scene-members-box').hidden=!explicit;$('scene-stagger-label').hidden=explicit; }
function renderPalette() {
  $('scene-palette').replaceChildren(...sceneHues.map((h,i)=>{
    const row=node('div',undefined,'palette-row'),label=node('label',`Barva ${i+1}`),picker=node('input');picker.type='color';picker.value=hueHex(h);picker.setAttribute('aria-label',`Barva ${i+1}`);
    picker.addEventListener('change',()=>{const hue=hexHue(picker.value);if(hue===null){toast('Vyber barevný odstín. Bílá a šedá do cyklu nepatří.',true);picker.value=hueHex(sceneHues[i]);return;}sceneHues[i]=hue;picker.value=hueHex(hue);});label.append(picker);row.append(label);
    for(const [text,delta] of [['↑',-1],['↓',1],['Odebrat',0]]) {
      const b=node('button',text);b.type='button';b.setAttribute('aria-label',`${text} barvu ${i+1}`);b.disabled=delta<0?i===0:delta>0?i===sceneHues.length-1:sceneHues.length<=2;
      b.addEventListener('click',()=>{if(delta)[sceneHues[i],sceneHues[i+delta]]=[sceneHues[i+delta],sceneHues[i]];else sceneHues.splice(i,1);renderPalette();});row.append(b);
    }
    return row;
  }));$('scene-add-color').disabled=sceneHues.length>=8;
}
function renderSceneMembers() {
  const available=state.lights.filter(l=>!sceneMembers.some(m=>m[0]===l.id));
  $('scene-add-member').replaceChildren(...available.map(l=>{const o=node('option',l.name);o.value=l.id;return o;}));$('scene-add-light').disabled=!available.length;
  $('scene-members').replaceChildren(...sceneMembers.map((m,i)=>{
    const row=node('div',undefined,'scene-member'),label=node('label',`${i+1}. ${lightName(m[0])}`),offset=node('input');
    offset.type='number';offset.min=1;offset.max=32;offset.value=m[1]+1;offset.required=true;offset.setAttribute('aria-label','Počáteční krok: '+lightName(m[0]));
    offset.addEventListener('input',()=>{m[1]=Number(offset.value)-1;});label.append(node('small','Počáteční krok'),offset);row.append(label);
    const actions=node('div',undefined,'actions');for(const [text,delta] of [['↑',-1],['↓',1],['Odebrat',0]]) {
      const b=node('button',text);b.type='button';b.setAttribute('aria-label',`${text} světlo ${i+1}`);b.disabled=delta<0?i===0:delta>0?i===sceneMembers.length-1:false;
      b.addEventListener('click',()=>{if(delta)[sceneMembers[i],sceneMembers[i+delta]]=[sceneMembers[i+delta],sceneMembers[i]];else sceneMembers.splice(i,1);renderSceneMembers();});actions.append(b);
    }row.append(actions);return row;
  }));
}
$('new-scene').addEventListener('click',()=>editScene(null));$('scene-cancel').addEventListener('click',()=>{$('scene-form').hidden=true;});
$('scene-mode').addEventListener('change',()=>{const mode=sceneData.modes[Number($('scene-mode').value)];$('scene-hold').value=mode.hold/10;$('scene-fade').value=mode.fade/10;sceneMode();});
$('scene-scope').addEventListener('change',sceneScope);
$('scene-add-color').addEventListener('click',()=>{if(sceneHues.length<8){sceneHues.push((sceneHues.at(-1)+60)%360);renderPalette();}});
$('scene-add-light').addEventListener('click',()=>{const id=$('scene-add-member').value;if(id && !sceneMembers.some(m=>m[0]===id)){sceneMembers.push([id,sceneMembers.length]);renderSceneMembers();}});
$('scene-all-lights').addEventListener('click',()=>{state.lights.filter(l=>l.id!=='display').forEach(l=>{if(!sceneMembers.some(m=>m[0]===l.id))sceneMembers.push([l.id,sceneMembers.length]);});renderSceneMembers();});
$('scene-same').addEventListener('click',()=>{sceneMembers.forEach(m=>{m[1]=0;});renderSceneMembers();});
$('scene-stagger-all').addEventListener('click',()=>{sceneMembers.forEach((m,i)=>{m[1]=i;});renderSceneMembers();});
$('scene-form').addEventListener('submit',e=>{e.preventDefault();action(e.submitter,async()=>{
  const hold=Math.round(Number($('scene-hold').value)*10),fade=Math.round(Number($('scene-fade').value)*10),useIr=$('scene-scope').value==='ir';
  if(hold+fade<5)throw new Error('Výdrž a přechod musí dohromady trvat alespoň 0,5 s.');if(!useIr && !sceneMembers.length)throw new Error('Vyber alespoň jedno světlo.');
  await api('scenes',{...(sceneEditId?{id:sceneEditId}:{}),name:$('scene-name').value.trim(),mode:Number($('scene-mode').value),level:Math.max(1,Math.round(Number($('scene-level').value)*254/100)),hold,fade,useIr,stagger:$('scene-stagger').checked,hues:sceneHues,members:useIr?[]:sceneMembers});
  $('scene-form').hidden=true;await loadScenes();await refresh();toast('Scéna uložená. Tlačítku ji přiřadíš na kartě Ovladač.');
});});
$('scene-delete').addEventListener('click',()=>{if(confirm('Smazat tuto scénu? Její tlačítka zůstanou bez přiřazení.'))action($('scene-delete'),async()=>{await api('scenes',{id:sceneEditId,delete:true});$('scene-form').hidden=true;await loadScenes();await refresh();});});
$('scene-stop').addEventListener('click',()=>action($('scene-stop'),async()=>{await api('scene/stop',{});await refresh();}));
$('brightness-steps').addEventListener('change',()=>{if(Number($('brightness-steps').value)!==brightnessValues().length)brightnessScale();});$('brightness-mode').addEventListener('change',()=>brightnessScale(false));
$('remote-form').addEventListener('submit',e=>{e.preventDefault();action(e.submitter,async()=>{
  const levels=brightnessValues();
  if($('brightness-mode').value==='custom' && levels.some((v,i)=>!Number.isInteger(v) || v<1 || v>100 || i>0 && v<=levels[i-1]))throw new Error('Vlastní kroky musí mít rostoucí celé jasy od 1 do 100 %.');
  await api('remote',{whiteKelvin:Number($('white-kelvin').value),whiteLevel:Math.max(1,Math.round(Number($('white-level').value)*254/100)),brightnessSteps:Number($('brightness-steps').value),customBrightness:$('brightness-mode').value==='custom',brightnessLevels:brightnessValues(),buttons:[0,1,2,3].map(i=>$('remote-button-'+i).value||null)});
  await loadScenes();toast('Bílá, stupně jasu a tlačítka scén uložené.');
});});
labels();poll();

const updatePhases={clock:'Ověřuji čas pro zabezpečené spojení…',download:'Stahuji firmware přes HTTPS…',transfer:'Přenáším firmware…',validate:'Ověřuji celý obraz firmwaru…',restart:'Deska se restartuje…',verify_boot:'Čekám na potvrzení nového firmwaru C6…',done:'Aktualizace dokončena a nový firmware potvrdil start.',error:'Aktualizace se nedokončila.'};
Object.assign(errors,{update_busy:'Právě probíhá aktualizace.',update_target:'Vyber cílovou desku.',update_wrong_board:'Balíček patří jiné desce.',update_header:'Neplatný balíček .zota.',update_size:'Velikost balíčku nesouhlasí.',update_https_url:'Zadej platnou HTTPS adresu balíčku.',update_https_failed:'Stažení selhalo. Ověř přímou HTTPS adresu, přístup k internetu a certifikát serveru.',update_clock:'Nepodařilo se ověřit čas pro HTTPS. Zkus soubor nebo dostupnou internetovou síť.',update_cancelled:'Přenos zrušen. Původní firmware zůstává aktivní.',update_interrupted:'Přenos byl přerušen. Původní firmware zůstává aktivní.',update_checksum:'Kontrolní součet nesouhlasí. Firmware se neaktivoval.',update_invalid_image:'Obraz firmwaru není platný.',update_boot_unconfirmed:'Nejdřív počkej 30 sekund na ověření právě spuštěného firmwaru.',update_partition:'Chybí OTA oddíl nebo se firmware nevejde. Je potřeba kabelová příprava desky.',update_boot_failed:'Nový firmware C6 nepotvrdil start. Zkontroluj spojení a zobrazenou verzi; mohl se vrátit předchozí firmware.',update_commit_unknown:'C6 nepotvrdila přepnutí. Neodpojuj ji; zkontroluj verzi po restartu.',update_restarting:'Firmware už byl předán ke spuštění. Přenos už nelze zrušit.'});
let updateInfo=null,updatePolling=false,updatePending=null;
try{updatePending=JSON.parse(localStorage.getItem('lamp-update')||'null');}catch{}
function rememberUpdate(value){updatePending=value;if(value)localStorage.setItem('lamp-update',JSON.stringify(value));else localStorage.removeItem('lamp-update');}
async function refreshUpdate() {
  if(updatePolling)return;updatePolling=true;
  try {
    const info=await api('update/status');updateInfo=info;
    $('update-versions').textContent=`T‑Display: ${info.gateway.version}${info.gateway.confirmed?'':' · ověřuje start'}\nXIAO C6: ${info.coordinator.version||'verze nezjištěna'}${info.coordinator.available?'':' · OTA nedostupné'}`;
    let message=info.error?(errors[info.error]||info.error):(updatePhases[info.phase]||'Připraveno.');
    if(updatePending) {
      const target=info[updatePending.target];
      if(info.expectedId && info.target===(updatePending.target==='gateway'?1:2) && (info.busy || info.phase==='done')) {updatePending.id=info.expectedId;rememberUpdate(updatePending);}
      if(target.boot!==updatePending.boot && target.confirmed && updatePending.id && target.id===updatePending.id) {message='Aktualizace dokončena. Nový firmware potvrdil úspěšný start.';rememberUpdate(null);}
      else if(info.error && !info.busy)rememberUpdate(null);
      else if(Date.now()-updatePending.started>600000) {message='Ověření vypršelo. Zkontroluj uvedenou verzi a spojení desek.';rememberUpdate(null);}
      else if(!info.busy)message='Čekám na restart a potvrzení nové verze…';
    }
    $('update-phase').textContent=message;$('update-progress').value=info.total?info.received*100/info.total:0;
    $('update-bytes').textContent=info.total?`${Math.round(info.received/1024)} / ${Math.round(info.total/1024)} KiB potvrzeno`:'';
    $('update-cancel').disabled=!info.busy || ['restart','verify_boot','done'].includes(info.phase);
    $('update-form').querySelector('.primary').disabled=info.busy || !!updatePending;
  } catch(e) {if(updatePending)$('update-phase').textContent='Čekám na opětovné spojení po restartu…';else $('update-phase').textContent=e.message;}
  finally {updatePolling=false;}
}
$('update-source').addEventListener('change',()=>{const url=$('update-source').value==='url';$('update-file-label').hidden=url;$('update-file').disabled=url;$('update-url-label').hidden=!url;$('update-url').disabled=!url;$('update-url').required=url;});
$('update-cancel').addEventListener('click',async()=>{await action($('update-cancel'),async()=>{await api('update/cancel',{});});await refreshUpdate();});
$('update-form').addEventListener('submit',async e=>{e.preventDefault();await action(e.submitter,async()=>{
  await refreshUpdate();if(!updateInfo || updateInfo.busy)throw new Error('Aktualizace není připravená.');
  const target=$('update-target').value,pending={target,boot:updateInfo[target].boot,id:'',started:Date.now()};
  if(target==='coordinator'&&!updateInfo.coordinator.available)throw new Error(errors.coordinator_update_required);
  if($('update-source').value==='url') {await api('update/url',{target,url:$('update-url').value});rememberUpdate(pending);}
  else {
    const file=$('update-file').files[0];if(!file || file.size<432)throw new Error(errors.update_header);
    const header=new Uint8Array(await file.slice(0,144).arrayBuffer()),view=new DataView(header.buffer);
    if(new TextDecoder().decode(header.slice(0,8))!=='ZLAMPOTA' || header[8]!==1 || view.getUint32(12,true)+144!==file.size)throw new Error(errors.update_header);
    if(header[9]!==({gateway:1,coordinator:2})[target])throw new Error(errors.update_wrong_board);
    pending.id=[...header.slice(112,144)].map(v=>v.toString(16).padStart(2,'0')).join('');rememberUpdate(pending);
    try {
      const response=await fetch('/api/update/'+target,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:file});
      const result=await response.json();if(!response.ok){rememberUpdate(null);throw new Error(errors[result.error]||result.error);}
    } catch(e) {await refreshUpdate();if(updateInfo?.error)rememberUpdate(null);throw e;}
  }
  await refreshUpdate();
});await refreshUpdate();});
setInterval(()=>{if(!$('tab-updates').hidden || updatePending)refreshUpdate();},1500);

Object.assign(errors,{ble_disabled:'Nejdřív zapni Bluetooth.',ble_storage:'Nepodařilo se uložit nastavení Bluetooth.',ble_paused:'Bluetooth je dočasně pozastavené.',update_ble_busy:'Bluetooth se nepodařilo zastavit. Firmware se nenainstaloval; zkus restart T‑Display.'});
updatePhases.pause_ble='Pozastavuji Bluetooth před aktualizací…';
let bleInfo=null,blePolling=false;
async function refreshBle() {
  if(blePolling)return;blePolling=true;
  try {
    const b=await api('ble');bleInfo=b;
    const status=b.paused?'Pozastavené během aktualizace':!b.enabled?'Vypnuté':b.authenticated?'Telefon připojen':b.connected?'Ověřuji telefon':b.running?'Připravené k připojení':'Spouštím Bluetooth';
    $('ble-status').textContent=`${b.name} · ${status} · uložené telefony ${b.bonds}/4${b.error?' · '+b.error:''}`;
    $('ble-code').textContent=b.enabled?'Párování bez PINu je dostupné vždy. V aplikaci stiskni Párovat a vyber lampu.':'Bluetooth je vypnuté.';
    $('ble-toggle').textContent=b.enabled?'Vypnout Bluetooth':'Zapnout Bluetooth';
    $('ble-toggle').disabled=b.paused;
    $('ble-forget').disabled=!b.running || b.paused || b.forgetPending || !b.bonds;
    $('ble-memory').textContent=`Volná RAM: ${Math.round(b.freeHeap/1024)} KiB · největší blok: ${Math.round(b.largestBlock/1024)} KiB · minimum od startu: ${Math.round(b.minimumHeap/1024)} KiB`;
  } catch(e) {$('ble-status').textContent=e.message;}finally{blePolling=false;}
}
$('ble-toggle').onclick=()=>action($('ble-toggle'),async()=>{if(!bleInfo)return;await api('ble',{enabled:!bleInfo.enabled});await refreshBle();});
$('ble-forget').onclick=()=>action($('ble-forget'),async()=>{if(confirm('Zapomenout všechna párování telefonů? V telefonu pak odeber starý záznam Z-lamp a spáruj ho znovu. Žárovky zůstanou spárované.')){await api('ble',{forgetAll:true});await refreshBle();}});
setInterval(()=>{if(!$('tab-settings').hidden)refreshBle();},2000);
function openBluetoothLink() { if(state && location.hash==='#bluetooth') { showTab('settings');$('ble-panel').scrollIntoView(); } }
addEventListener('hashchange',openBluetoothLink);
openBluetoothLink();

function renderTimers() {
  const select=$('timer-target'),previous=select.value;
  select.replaceChildren(...targetOptions().filter(t=>t.id!=='display').map(t=>new Option(t.name,t.id)));
  select.value=[...select.options].some(o=>o.value===previous)?previous:'all';
  const list=$('timer-list');list.replaceChildren();
  for(const t of state.timers||[])list.append(node('p',`${lightName(t.id)} · ${t.seconds?`${Math.floor(t.seconds/60)} min ${t.seconds%60} s`:'Čeká na potvrzení vypnutí'}`));
  if(!state.timers?.length)list.append(node('p','Žádný časovač neběží.','hint'));
}
$('timer-form').onsubmit=e=>{e.preventDefault();const minutes=Number($('timer-minutes').value);if(!Number.isInteger(minutes)||minutes<1||minutes>1440)return toast('Zadej 1–1440 minut.');action(e.submitter,async()=>{await api('timer',{target:$('timer-target').value,seconds:minutes*60});toast('Časovač uložen v lampě.');await refresh();});};
$('timer-cancel').onclick=()=>action($('timer-cancel'),async()=>{await api('timer',{target:$('timer-target').value,seconds:0});await refresh();});
Object.assign(errors,{invalid_timer:'Zadej dobu nejvýše 24 hodin.',timer_requires_bulb:'Vyber Zigbee žárovky. Displej má vlastní uspávání.',arrival_storage_failed:'Časovač se nepodařilo uložit do lampy.'});
