// Browser integration check with a simulated API; no serial port or real light is accessed.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const {chromium}=require('playwright');
const root=path.resolve(__dirname,'..');
const values={on:true,level:127,kelvin:2700,hue:25,saturation:40};
const lights=[{id:'001788010203abcd',name:'Čtecí lampa',online:true,reported:true,on:true,level:127,colorKnown:true,color:'#ffc981',wanted:{...values}},
  {id:'001788010203abce',name:'Police',online:true,reported:true,on:false,level:60,colorKnown:true,color:'#b8a5ff',wanted:{...values,on:false}},
  {id:'display',name:'Displej / lampička',online:true,reported:true,on:false,level:127,colorKnown:true,color:'#ffe0a3',wanted:{...values}}];
const state={connected:true,channel:15,joinSeconds:0,apSsid:'HueLamp-DEMO',apEnabled:false,apActive:false,networkCount:2,staSsid:'',wifiError:'',apIp:'192.168.4.1',staIp:'',staConnected:false,clients:1,lastError:'',lights,groups:[],display:{awake:false,bootOn:false,mode:'status',timeout:120}};
const settings={bootOn:false,timeout:120,statusBrightness:40,mode:'status',apSsid:'HueLamp-DEMO',apPassword:'demo-pass-123',networks:[{ssid:'Kolej',password:'kolej-demo-123'},{ssid:'Doma',password:'doma-demo-123'}],presetTarget:'all',preset:{...values}};
const irNames=['Jas +','Jas -','OFF','ON','R','G','B','W','R1','G1','B1','FLASH','R2','G2','B2','STROBE','R3','G3','B3','FADE','R4','G4','B4','SMOOTH'];
settings.irEnabled=true;settings.irTarget='all';settings.irKeys=irNames;
state.ir={lastCode:'',lastKey:-1,age:-1,learningKey:-1,learningSeconds:0,effect:0,codes:Array(24).fill('00F700FF')};
const commands=[],errors=[];
state.alias='Z-lamp';state.hostname='z-lamp';state.aliasPending=false;settings.alias='Z-lamp';
state.deviceApi=3;
lights.forEach(l=>Object.assign(l,{transition:3,legacyXy:false,capabilities:31,manufacturer:'Philips',model:'Hue test',minKelvin:2000,maxKelvin:6500}));
const sceneModes=['Duha plynule','Barevné kroky','Barevný dech','Barevný flash','Disko','Party rytmus','Modrá a růžová','Západ slunce','Polární záře','Oceán','Oheň','Barevný běžec','Vlastní paleta'].map(name=>({name,description:name+' bez bílé.',hold:5,fade:20}));
const sceneData={whiteKelvin:2700,whiteLevel:127,brightnessSteps:10,customBrightness:false,brightnessLevels:[1,2,4,7,12,20,32,50,75,100],irMembers:[],buttons:['s1','s2','s3','s4'],modes:sceneModes,scenes:[3,5,6,0].map((mode,i)=>({id:'s'+(i+1),name:sceneModes[mode].name,mode,level:127,hold:5,fade:20,useIr:true,stagger:false,hues:[240,320],members:[]}))};
const updateState={busy:false,target:0,received:0,total:0,phase:'',error:'',expectedId:'',gateway:{version:'2.7.0',id:'a'.repeat(64),boot:1,confirmed:true},coordinator:{version:'2.7.0',id:'b'.repeat(64),boot:2,confirmed:true,available:true}};
const bleState={enabled:true,running:true,connected:false,authenticated:false,paused:false,bonds:1,name:'Z-lamp',pairingMode:'always',freeHeap:60000,largestBlock:32768,minimumHeap:45000};
let stateFailures=1,rejectNextUpdate=false;
(async()=>{
  const browser=await chromium.launch({headless:true,...(process.env.BROWSER_EXECUTABLE?{executablePath:process.env.BROWSER_EXECUTABLE}:{channel:'msedge'})});
  try {
    const page=await browser.newPage({viewport:{width:1280,height:1050}});
    page.on('pageerror',e=>errors.push(e.message));
    const save=async name=>Promise.all([page.waitForResponse(r=>r.url().endsWith('/api/settings')&&r.request().method()==='POST'),page.getByRole('button',{name,exact:true}).click()]);
    await page.route('http://lamp.test/**',async route=>{
      const req=route.request(),url=new URL(req.url());
      if(url.pathname.startsWith('/api/')) {
        assert.equal(req.headers().authorization,undefined,'Web must not send a login credential');
        const op=url.pathname.slice(5),body=req.headers()['content-type']?.startsWith('application/json')?req.postDataJSON():null;
        if(op==='ble') {
          if(body){commands.push({op,body});if('enabled' in body){bleState.enabled=body.enabled;bleState.running=body.enabled;}if(body.forgetAll)bleState.bonds=0;}
          return route.fulfill({json:body?{ok:true}:bleState});
        }
        if(op==='update/status')return route.fulfill({json:updateState});
        if(op==='update/gateway'||op==='update/coordinator') {
          if(rejectNextUpdate){rejectNextUpdate=false;return route.fulfill({status:400,json:{error:'update_partition'}});}
          const target=op.slice(7),data=req.postDataBuffer();assert.equal(data.subarray(0,8).toString(),'ZLAMPOTA');
          commands.push({op,bytes:data.length});updateState.error='';updateState.busy=true;updateState.target=target==='gateway'?1:2;updateState.total=data.length-144;updateState.received=updateState.total;updateState.phase='verify_boot';updateState.expectedId=data.subarray(112,144).toString('hex');
          setTimeout(()=>{updateState[target].id=updateState.expectedId;updateState[target].boot++;updateState[target].confirmed=true;updateState.phase='done';updateState.busy=false;},500);
          return route.fulfill({json:{ok:true}});
        }
        if(op==='update/url'){commands.push({op,body});Object.assign(updateState,{busy:true,phase:'download',error:'',expectedId:'',total:0,received:0,target:body.target==='gateway'?1:2});return route.fulfill({json:{ok:true}});}
        if(op==='update/cancel'){updateState.busy=false;updateState.phase='error';updateState.error='update_cancelled';return route.fulfill({json:{ok:true}});}
        if(op==='state'&&stateFailures>0){stateFailures--;return route.fulfill({status:503,json:{error:'offline'}});}
        if(body)commands.push({op,body});
        if(op==='reboot'){stateFailures=1;state.alias=settings.alias;state.hostname=settings.alias.toLowerCase();state.aliasPending=false;}
        if(op==='pair')state.joinSeconds=body.seconds;
        if(op==='timer') {
          const ids=body.target==='all'?lights.filter(l=>l.id!=='display').map(l=>l.id):state.groups.find(g=>g.id===body.target)?.members||[body.target];
          state.timers=(state.timers||[]).filter(t=>!ids.includes(t.id));
          if(body.seconds)state.timers.push(...ids.filter(id=>id!=='display').map(id=>({id,seconds:body.seconds,explicit:true})));
        }
        if(op==='rename')lights.find(l=>l.id===body.id).name=body.name;
        if(op==='device')Object.assign(lights.find(l=>l.id===body.id),body);
        if(op==='startup') {const l=lights.find(l=>l.id===body.id);l.startup={...l.startup,...body};if(body.powerMode!==undefined)l.powerStatus=body.powerMode?2:0;}
        if(op==='control')for(const l of lights)if(l.id===body.target || body.target==='all'&&l.id!=='display')Object.assign(l.wanted,body.patch);
        if(op==='remove') {
          const l=lights.find(l=>l.id===body.id);
          if(body.force){lights.splice(lights.indexOf(l),1);for(const g of state.groups)g.members=g.members.filter(id=>id!==body.id);}
          else {l.removing=true;setTimeout(()=>{l.removing=false;l.removeError='leave_timeout';},100);}
        }
        if(op==='ir/learn'){state.ir.learningKey=body.key;state.ir.learningSeconds=20;}
        if(op==='ir/cancel'){state.ir.learningKey=-1;state.ir.learningSeconds=0;}
        if(op==='ir/reset'){state.ir.learningKey=-1;state.ir.codes=Array(24).fill('00F700FF');}
        if(op==='settings'&&body){Object.assign(settings,body);state.aliasPending=settings.alias!==state.alias;}
        if(op==='remote')Object.assign(sceneData,body);
        if(op==='scenes'&&body) {
          if(body.delete){sceneData.scenes=sceneData.scenes.filter(s=>s.id!==body.id);sceneData.buttons=sceneData.buttons.map(id=>id===body.id?null:id);}
          else {const id=body.id||Array.from({length:8},(_,i)=>'s'+(i+1)).find(id=>!sceneData.scenes.some(s=>s.id===id));sceneData.scenes=sceneData.scenes.filter(s=>s.id!==id);sceneData.scenes.push({...body,id});}
        }
        if(op==='scene/run'){const s=sceneData.scenes.find(s=>s.id===body.id);state.ir.sceneName=s.name;state.ir.sceneLevel=s.level;state.ir.sceneIndex=Number(s.id.slice(1))-1;state.ir.effect=4;}
        if(op==='scene/stop'){state.ir.sceneName='';state.ir.effect=0;}
        if(op==='group') {
          if(body.delete)state.groups=state.groups.filter(g=>g.id!==body.id);
          else {const g={...body,id:body.id||'g1'};state.groups=state.groups.filter(old=>old.id!==g.id);state.groups.push(g);}
        }
        return route.fulfill({json:op==='state'?state:op==='settings'&&!body?settings:op==='scenes'&&!body?sceneData:{ok:true}});
      }
      const file={'/':'index.html','/app.js':'app.js','/style.css':'style.css'}[url.pathname];
      if(!file)return route.fulfill({status:404,body:''});
      return route.fulfill({body:fs.readFileSync(path.join(root,'gateway/main/web',file)),contentType:file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':'text/html',headers:{'Content-Security-Policy':"default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'"}});
    });
    await page.goto('http://lamp.test/');
    await page.locator('#connection').filter({hasText:'Spojení přerušeno'}).waitFor();
    assert.equal(await page.locator('#workspace').evaluate(el=>el.inert),true);
    assert.equal(await page.locator('#login, #login-form, #password, #logout').count(),0);
    await page.waitForFunction(()=>document.querySelectorAll('.light-card').length===3);
    assert.equal(await page.locator('#workspace').evaluate(el=>el.inert),false);
    await page.locator('#target').selectOption(lights[0].id);
    await page.locator('#saturation').fill('65');await page.locator('#saturation').dispatchEvent('change');
    await page.waitForResponse(r=>r.url().endsWith('/api/state'));
    assert.deepEqual(commands.find(c=>c.op==='control').body,{target:lights[0].id,patch:{saturation:65}});
    await page.locator('#pair-open').click();await page.locator('#pair-start').click();
    await page.locator('#pair-title').filter({hasText:'180 s'}).waitFor();
    assert(commands.some(c=>c.op==='pair'&&c.body.seconds===180));
    await page.getByRole('button',{name:'Skupiny',exact:true}).click();await page.locator('#new-group').click();
    await page.locator('#group-name').fill('Večer');await page.locator('#group-members input').first().check();await page.locator('#group-members input[value="display"]').check();
    await page.getByRole('button',{name:'Uložit skupinu'}).click();await page.locator('.group-card h2').filter({hasText:'Večer'}).waitFor();
    assert.deepEqual(state.groups[0].members,[lights[0].id,'display']);
    await page.getByRole('button',{name:'Ovládat',exact:true}).click();await Promise.all([page.waitForResponse(r=>r.url().endsWith('/api/control')),page.locator('#power-off').click()]);
    assert(commands.some(c=>c.op==='control'&&c.body.target==='g1'&&c.body.patch.on===false));
    await page.getByRole('button',{name:'Nastavení',exact:true}).click();await page.locator('#timeout').filter({visible:true}).waitFor();
    await page.waitForFunction(()=>document.getElementById('timeout').value==='120');
    assert.equal(await page.locator('#alias').inputValue(),'Z-lamp');
    assert.equal(await page.locator('#device-name').textContent(),'Z-lamp');
    assert((await page.locator('#alias-status').textContent()).includes('http://z-lamp.local'));
    await page.locator('#ble-status').filter({hasText:'uložené telefony 1/4'}).waitFor();
    await page.locator('#ble-code').filter({hasText:'dostupné vždy'}).waitFor();
    assert.equal(await page.locator('#ble-pair, #ble-cancel, #ble-rssi').count(),0);
    await page.locator('#ble-toggle').click();await page.locator('#ble-status').filter({hasText:'Vypnuté'}).waitFor();
    assert.equal(settings.apEnabled,undefined);assert.equal(state.apActive,false);
    await page.locator('#ble-toggle').click();await page.locator('#ble-status').filter({hasText:'Připravené'}).waitFor();
    page.once('dialog',d=>d.accept());await page.locator('#ble-forget').click();await page.locator('#ble-status').filter({hasText:'uložené telefony 0/4'}).waitFor();
    await page.getByRole('button',{name:'Světla',exact:true}).click();
    const oldLightValues=JSON.stringify(lights.map(l=>l.wanted));
    await page.locator('#timer-minutes').fill('60');await page.getByRole('button',{name:'Nastavit časovač',exact:true}).click();
    await page.locator('#timer-list').filter({hasText:'60 min'}).waitFor();
    assert(state.timers.length>0);assert.equal(JSON.stringify(lights.map(l=>l.wanted)),oldLightValues);
    await page.reload();await page.locator('#timer-list').filter({hasText:'60 min'}).waitFor();
    await page.locator('#timer-cancel').click();await page.locator('#timer-list').filter({hasText:'Žádný časovač'}).waitFor();
    await page.getByRole('button',{name:'Nastavení',exact:true}).click();
    const beforeAlias=commands.length;
    await page.locator('#alias').fill('-bad');await page.getByRole('button',{name:'Uložit alias',exact:true}).click();
    assert.equal(await page.locator('#alias').evaluate(el=>el.checkValidity()),false);assert.equal(commands.length,beforeAlias);
    await page.locator('#alias').fill('Nocni-lampa');await save('Uložit alias');
    await page.locator('#alias-status').filter({hasText:'čeká na restart'}).waitFor();
    assert.equal(settings.alias,'Nocni-lampa');assert.equal(state.alias,'Z-lamp');
    assert.equal(settings.apSsid,'HueLamp-DEMO');
    assert.equal(await page.locator('#bootOn').isChecked(),false);
    await page.locator('#timeout').fill('0');await page.locator('#mode').selectOption('lamp');await save('Uložit displej');
    assert.equal(settings.timeout,0);assert.equal(settings.mode,'lamp');assert.equal(settings.bootOn,false);
    assert.equal(await page.locator('#pots-form').count(),0);
    await page.locator('#presetTarget').selectOption('g1');await page.locator('#presetLevel').fill('25');
    await page.locator('#presetKelvin').fill('2200');await page.locator('#presetHue').fill('240');await page.locator('#presetSaturation').fill('60');
    await save('Uložit preset');
    assert.equal(settings.presetTarget,'g1');assert.deepEqual(settings.preset,{on:true,level:64,kelvin:2200,hue:240,saturation:60});
    await page.locator('#network-list fieldset').nth(1).getByRole('button',{name:'Nahoru'}).click();
    await page.locator('#add-network').click();
    await page.locator('#network-list fieldset').nth(2).locator('[data-field="ssid"]').fill('Dilna');
    await page.locator('#network-list fieldset').nth(2).locator('[data-field="password"]').fill('dilna-demo-123');
    await save('Uložit pořadí sítí');
    assert.deepEqual(settings.networks.map(n=>n.ssid),['Doma','Kolej','Dilna']);
    assert.equal(settings.networks[0].password,'doma-demo-123');
    assert((await page.locator('#ap-status').textContent()).includes('Hotspot vypnutý'));
    assert(!commands.some(c=>c.op==='settings'&&'apEnabled' in c.body));
    await page.locator('#network-list fieldset').nth(1).getByRole('button',{name:'Odebrat'}).click();
    await save('Uložit pořadí sítí');
    assert.deepEqual(settings.networks.map(n=>n.ssid),['Doma','Dilna']);
    await page.getByRole('button',{name:'Ovladač',exact:true}).click();
    await page.locator('#irTarget').selectOption('display');await save('Uložit IR ovladač');
    assert.equal(settings.irTarget,'display');assert.equal(settings.irEnabled,true);
    await page.locator('#ir-form summary').click();
    assert.equal(await page.locator('#ir-keys button').count(),24);
    const controlsBefore=commands.filter(c=>c.op==='control').length;
    await page.getByRole('button',{name:'Naučit R',exact:true}).click();
    await page.locator('#ir-status').filter({hasText:'Stiskni R'}).waitFor();
    assert(commands.some(c=>c.op==='ir/learn'&&c.body.key===4));
    state.ir.learningKey=-1;state.ir.lastCode='12AB34CB';state.ir.lastKey=4;state.ir.age=0;state.ir.codes[4]='12AB34CB';
    await page.locator('#ir-last').filter({hasText:'12AB34CB'}).waitFor();
    assert.equal(await page.getByRole('button',{name:'Naučit R',exact:true}).locator('small').textContent(),'12AB34CB');
    assert.equal(commands.filter(c=>c.op==='control').length,controlsBefore);
    await page.getByRole('button',{name:'Naučit W',exact:true}).click();await page.locator('#ir-status').filter({hasText:'Stiskni W'}).waitFor();
    await page.locator('#ir-cancel').click();await page.locator('#ir-status').filter({hasText:'Efekt: vypnutý'}).waitFor();
    await page.getByRole('button',{name:'Nastavení',exact:true}).click();
    assert.equal(await page.getByLabel('Heslo hotspotu',{exact:true}).inputValue(),'demo-pass-123');
    page.once('dialog',dialog=>dialog.accept());
    await page.locator('#reboot').click();
    await page.locator('#connection').filter({hasText:'Spojení přerušeno'}).waitFor();
    await page.locator('#connection').filter({hasText:'Zigbee připojeno'}).waitFor();
    assert(commands.some(c=>c.op==='reboot'));
    assert.equal(await page.locator('#device-name').textContent(),'Nocni-lampa');
    assert((await page.locator('#alias-status').textContent()).includes('http://nocni-lampa.local'));
    assert.equal(await page.title(),'Nocni-lampa · ovládání světla');
    await page.setViewportSize({width:390,height:844});
    assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
    await page.screenshot({path:path.join(root,'.cache/web-settings-mobile.png'),fullPage:true});
    await page.setViewportSize({width:1280,height:1050});
    await page.getByRole('button',{name:'Světla',exact:true}).click();
    fs.mkdirSync(path.join(root,'.cache'),{recursive:true});await page.screenshot({path:path.join(root,'.cache/web-desktop.png'),fullPage:true});
    await page.setViewportSize({width:390,height:844});
    assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
    await page.screenshot({path:path.join(root,'.cache/web-mobile.png'),fullPage:true});
    // Untrusted light/group labels must stay plain text, never interpreted as markup.
    lights[0].name='<img src=x onerror=alert(1)>';
    await page.waitForFunction(()=>document.querySelector('.light-card strong')?.textContent.includes('<img'));
    assert.equal(await page.locator('.light-card img').count(),0);
    state.alias='W'.repeat(32);state.hostname='w'.repeat(32);
    await page.waitForFunction(()=>document.getElementById('device-name').textContent==='W'.repeat(32));
    await page.getByRole('button',{name:'Nastavení',exact:true}).click();
    assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'Long alias must fit mobile layout');
    state.alias=settings.alias;state.hostname=settings.alias.toLowerCase();
    await page.reload();
    await page.waitForFunction(()=>document.querySelectorAll('.light-card').length===3);
    assert.equal(await page.locator('#login, #logout').count(),0);
    await page.getByRole('button',{name:'Nastavení',exact:true}).click();
    await page.waitForFunction(()=>document.getElementById('alias').value==='Nocni-lampa');
    await page.getByRole('button',{name:'Scény',exact:true}).click();
    await page.locator('#new-scene').click();await page.locator('#scene-name').fill('Modrá a růžová spolu');await page.locator('#scene-mode').selectOption('12');
    await page.locator('#scene-hold').fill('1');await page.locator('#scene-fade').fill('3');await page.locator('#scene-scope').selectOption('members');
    await page.locator('#scene-all-lights').click();assert.equal(await page.locator('.scene-member').count(),2);
    assert.equal(await page.locator('.scene-member input').nth(1).inputValue(),'2');
    await page.locator('#scene-palette input').first().fill('#0000ff');await page.locator('#scene-palette input').first().dispatchEvent('change');
    await page.locator('#scene-palette input').nth(1).fill('#ff00aa');await page.locator('#scene-palette input').nth(1).dispatchEvent('change');
    assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
    await page.screenshot({path:path.join(root,'.cache/web-scenes-mobile.png'),fullPage:true});
    await Promise.all([page.waitForResponse(r=>r.url().endsWith('/api/scenes')&&r.request().method()==='POST'),page.getByRole('button',{name:'Uložit scénu',exact:true}).click()]);
    await page.locator('#scene-form').filter({visible:false}).count();
    const custom=sceneData.scenes.find(s=>s.name==='Modrá a růžová spolu');assert.deepEqual(custom.hues,[240,320]);assert.equal(custom.fade,30);assert.equal(custom.hold,10);assert.deepEqual(custom.members,[[lights[0].id,0],[lights[1].id,1]]);
    const card=page.locator('.scene-card').filter({has:page.getByRole('heading',{name:'Modrá a růžová spolu',exact:true})});await card.getByRole('button',{name:'Spustit',exact:true}).click();
    await page.locator('#scene-running').filter({hasText:'Právě běží: Modrá a růžová spolu'}).waitFor();
    await page.locator('#scene-stop').click();await page.locator('#scene-running').filter({hasText:'Žádná scéna'}).waitFor();
    await page.getByRole('button',{name:'Ovladač',exact:true}).click();await page.locator('#white-kelvin').fill('2400');await page.locator('#white-level').fill('20');await page.locator('#brightness-steps').fill('16');await page.locator('#brightness-steps').dispatchEvent('change');
    assert.equal((await page.locator('#brightness-scale').textContent()).split('→').length,16);
    await page.locator('#remote-button-0').selectOption(custom.id);
    await Promise.all([page.waitForResponse(r=>r.url().endsWith('/api/remote')),page.getByRole('button',{name:'Uložit tlačítka',exact:true}).click()]);
    assert.equal(sceneData.whiteKelvin,2400);assert.equal(sceneData.whiteLevel,51);assert.equal(sceneData.brightnessSteps,16);assert.equal(sceneData.buttons[0],custom.id);
    await page.reload();await page.waitForFunction(()=>document.querySelectorAll('.light-card').length===3);await page.getByRole('button',{name:'Ovladač',exact:true}).click();
    await page.waitForFunction(()=>document.getElementById('brightness-steps').value==='16');assert.equal(await page.locator('#white-kelvin').inputValue(),'2400');assert.equal(await page.locator('#remote-button-0').inputValue(),custom.id);
    await page.locator('#brightness-steps').fill('4');await page.locator('#brightness-steps').dispatchEvent('change');await page.locator('#brightness-mode').selectOption('custom');
    for(const [i,v] of [1,2,4,20].entries())await page.locator('#brightness-levels input').nth(i).fill(String(v));
    assert.deepEqual(await page.locator('#remote-form :invalid').evaluateAll(nodes=>nodes.map(n=>({id:n.id,value:n.value,message:n.validationMessage}))),[],'Remote fields must be valid before saving');
    const savesBefore=commands.filter(c=>c.op==='remote').length;
    await page.locator('#brightness-levels input').nth(1).fill('1');await page.getByRole('button',{name:'Uložit tlačítka',exact:true}).click();
    await page.locator('#toast').filter({hasText:'rostoucí celé jasy'}).waitFor();assert.equal(commands.filter(c=>c.op==='remote').length,savesBefore);
    await page.locator('#brightness-levels input').nth(1).fill('2');
    await Promise.all([page.waitForResponse(r=>r.url().endsWith('/api/remote')),page.getByRole('button',{name:'Uložit tlačítka',exact:true}).click()]);
    assert.deepEqual(sceneData.brightnessLevels,[1,2,4,20]);assert(sceneData.customBrightness);
    await page.locator('#irTarget').selectOption('remote');await page.locator('#ir-members input').first().check();await save('Uložit IR ovladač');
    assert.equal(settings.irTarget,'remote');assert.deepEqual(sceneData.irMembers,[lights[0].id]);
    await page.screenshot({path:path.join(root,'.cache/web-remote-mobile.png'),fullPage:true});
    assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
    await page.reload();await page.waitForFunction(()=>document.querySelectorAll('.light-card').length===3);await page.getByRole('button',{name:'Ovladač',exact:true}).click();
    await page.waitForFunction(()=>document.getElementById('brightness-mode').value==='custom');assert.equal(await page.locator('#irTarget').inputValue(),'remote');assert.equal(await page.locator('#brightness-levels input').nth(2).inputValue(),'4');
    await page.getByRole('button',{name:'Aktualizace',exact:true}).click();await page.locator('#update-versions').filter({hasText:'2.7.0'}).waitFor();
    await page.locator('#update-file').setInputFiles(path.join(root,'build/coordinator/hue_lamp.zota'));
    await page.getByRole('button',{name:'Instalovat firmware',exact:true}).click();await page.locator('#toast').filter({hasText:'jiné desce'}).waitFor();
    assert(!commands.some(c=>c.op==='update/gateway'));
    await page.locator('#update-target').selectOption('coordinator');await page.getByRole('button',{name:'Instalovat firmware',exact:true}).click();
    await page.locator('#update-phase').filter({hasText:'Aktualizace dokončena'}).waitFor();assert(commands.some(c=>c.op==='update/coordinator'));
    await page.locator('#update-source').selectOption('url');await page.locator('#update-url').fill('https://firmware.example/test.zota');await page.getByRole('button',{name:'Instalovat firmware',exact:true}).click();
    await page.locator('#update-cancel').filter({visible:true}).waitFor();await page.waitForFunction(()=>!document.getElementById('update-cancel').disabled);await page.locator('#update-cancel').click();
    await page.locator('#update-phase').filter({hasText:'Přenos zrušen'}).waitFor();
    await page.waitForFunction(()=>document.getElementById('update-cancel').disabled);
    await page.screenshot({path:path.join(root,'.cache/web-updates-mobile.png'),fullPage:true});
    await page.locator('#update-source').selectOption('file');await page.locator('#update-target').selectOption('gateway');
    await page.locator('#update-file').setInputFiles(path.join(root,'build/gateway/lamp_gateway.zota'));rejectNextUpdate=true;
    await page.getByRole('button',{name:'Instalovat firmware',exact:true}).click();await page.locator('#toast').filter({hasText:'Chybí OTA oddíl'}).waitFor();
    assert.equal(await page.evaluate(()=>localStorage.getItem('lamp-update')),null);
    await page.getByRole('button',{name:'Instalovat firmware',exact:true}).click();
    await page.locator('#update-phase').filter({hasText:'Aktualizace dokončena'}).waitFor();assert(commands.some(c=>c.op==='update/gateway'));
    await page.getByRole('button',{name:'Světla',exact:true}).click();
    await page.locator('#target').selectOption(lights[1].id);
    await page.locator('#rename').click();
    assert(await page.locator('#device-dialog').evaluate(d=>d.open));
    await page.locator('#device-label').fill('Noční světlo');
    await page.getByRole('button',{name:'Uložit název',exact:true}).click();
    await page.locator('#device-title').filter({hasText:'Noční světlo'}).waitFor();
    await page.locator('#device-transition').fill('1.7');await page.locator('#device-color').selectOption('native');
    await Promise.all([page.waitForResponse(r=>r.url().endsWith('/api/device')),page.getByRole('button',{name:'Uložit chování',exact:true}).click()]);
    assert.equal(lights[1].transition,17);
    await page.locator('#device-saturation').fill('100');await page.locator('#device-on').check();
    await Promise.all([page.waitForResponse(r=>r.url().endsWith('/api/control')),page.getByRole('button',{name:'Použít hodnoty světla',exact:true}).click()]);
    assert.equal(lights[1].wanted.saturation,100);
    await page.locator('#wake-mode').selectOption('1');await page.locator('#wake-level').fill('8');await page.locator('#wake-kelvin').fill('2300');
    await Promise.all([page.waitForResponse(r=>r.url().endsWith('/api/startup')),page.getByRole('button',{name:'Uložit běžné zapnutí',exact:true}).click()]);
    assert.equal(lights[1].startup.wakeMode,1);assert.equal(lights[1].startup.wake.level,20);assert.equal(lights[1].startup.wake.kelvin,2300);
    await page.locator('#power-mode').selectOption('4');await page.locator('#power-level').fill('12');await page.locator('#power-hue').fill('320');await page.locator('#power-saturation').fill('100');
    await Promise.all([page.waitForResponse(r=>r.url().endsWith('/api/startup')),page.getByRole('button',{name:'Uložit do žárovky',exact:true}).click()]);
    assert.equal(lights[1].startup.powerMode,4);assert.equal(lights[1].startup.power.hue,320);
    await page.locator('#power-status').filter({hasText:'Potvrzeno'}).waitFor();
    await page.locator('#power-form').scrollIntoViewIfNeeded();await page.screenshot({path:path.join(root,'.cache/web-startup-mobile.png')});
    await page.locator('#device-close').click();await page.locator('#rename').click();
    assert.equal(await page.locator('#wake-level').inputValue(),'8');assert.equal(await page.locator('#power-hue').inputValue(),'320');
    await page.locator('#power-mode').selectOption('1');assert(await page.locator('#power-values').isHidden());
    await Promise.all([page.waitForResponse(r=>r.url().endsWith('/api/startup')),page.getByRole('button',{name:'Uložit do žárovky',exact:true}).click()]);
    assert.equal(lights[1].startup.powerMode,1);
    await Promise.all([page.waitForResponse(r=>r.url().endsWith('/api/identify')),page.locator('#identify').click()]);
    await page.screenshot({path:path.join(root,'.cache/web-device-mobile.png')});
    await page.locator('#device-close').click();
    const wheel=page.locator('#color-wheel');await wheel.scrollIntoViewIfNeeded();const box=await wheel.boundingBox();
    const beforeWheel=commands.filter(c=>c.op==='control').length;
    const touch=await page.context().newCDPSession(page);
    const point=(x,y)=>[{x,y,id:0}];
    await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:point(box.x+box.width/2,box.y+box.height*.04)});
    for(let i=0;i<30;i++)await touch.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:point(box.x+box.width*(.5+i/60),box.y+box.height*.5)});
    await touch.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:point(box.x+box.width,box.y+box.height*.5)});
    await touch.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});await touch.detach();
    await page.waitForFunction(()=>document.getElementById('control-status').textContent.includes('Příkaz přijat'));
    await page.waitForTimeout(400);
    const wheelCommands=commands.filter(c=>c.op==='control').slice(beforeWheel);
    assert(wheelCommands.length<15,'Touch movement must coalesce commands');
    assert.equal(wheelCommands.at(-1).body.patch.saturation,100);assert.equal(wheelCommands.at(-1).body.patch.hue,90);
    // Changing selection before debounce must not send the previous gesture to another bulb.
    const firstId=lights[0].id,secondId=lights[1].id;
    await page.locator('#saturation').fill('81');await page.locator('#target').selectOption(firstId);
    await page.waitForTimeout(400);
    assert(commands.some(c=>c.op==='control'&&c.body.target===secondId&&c.body.patch.saturation===81));
    await page.locator('#target').selectOption(secondId);await page.locator('#rename').click();
    await page.locator('#device-removal summary').click();
    const beforeRemove=commands.filter(c=>c.op==='remove').length;
    page.once('dialog',d=>d.dismiss());await page.locator('#force-remove').click();
    assert.equal(commands.filter(c=>c.op==='remove').length,beforeRemove);
    page.once('dialog',d=>d.accept());await page.locator('#remove-device').click();
    await page.locator('#device-status').filter({hasText:'nepotvrdila'}).waitFor();assert.equal(lights.length,3);
    page.once('dialog',d=>d.accept());await page.locator('#force-remove').click();
    await page.waitForFunction(()=>!document.getElementById('device-dialog').open);assert.equal(lights.length,2);
    await page.locator('#target').selectOption('display');await page.locator('#rename').click();
    assert(await page.locator('#device-removal').isHidden());assert(await page.locator('#device-config-form').isHidden());
    await page.locator('#device-close').click();
    await page.goto('http://lamp.test/#bluetooth');
    stateFailures=1;
    await page.reload();
    await page.locator('#ble-panel').waitFor({state:'visible'});
    assert((await page.locator('#ble-instructions').textContent()).includes('aplikaci Z-lamp'));
    await page.locator('#ble-code').filter({hasText:'dostupné vždy'}).waitFor();
    await page.locator('#ble-panel').screenshot({path:path.join(root,'.cache/web-ble-pairing-mobile.png')});
    assert(!commands.some(c=>c.op==='ble' && ('pair' in c.body || 'pairRssi' in c.body)));
    assert.equal(state.apActive,false,'Pairing must not enable the hotspot');
    assert.deepEqual(errors,[]);console.log('Browser checks passed: BLE always-available pairing help without window or RSSI controls, custom IR steps/scope and persistence, both OTA targets, wrong board/server rejection, URL/cancel, scenes/phases, wake/power presets, device settings/removal, touch wheel, alias, unauthenticated access, Wi-Fi and mobile layout.');
  }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
