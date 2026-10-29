(() => {
  'use strict';
  const token = document.querySelector('meta[name="ioc-token"]').content;
  const $ = id => document.getElementById(id);
  const svgNS='http://www.w3.org/2000/svg';
  let state=null, templates=[], selected=null, lastJobs='', lastResults='', activeView='topology';
  let toastTimer=null, requestBusy=false, dragging=null;
  const clock = (ms) => !Number.isFinite(ms) ? '—' : (ms/1000).toFixed(1)+' s';
  const number = value => Number.isFinite(value) ? new Intl.NumberFormat('pt-BR',{maximumFractionDigits:2}).format(value) : '—';
  const pct = value => Number.isFinite(value) ? number(value)+'%' : '—';

  async function api(path, body) {
    const options={method:body===undefined?'GET':'POST',headers:{'X-IOC-Token':token}};
    if(body!==undefined){options.headers['Content-Type']='application/json';options.body=JSON.stringify(body);}
    const resp=await fetch(path,options);
    const data=await resp.json();
    if(!resp.ok)throw new Error(data.error||`HTTP ${resp.status}`);
    return data;
  }
  function notify(message,error=false) {
    const element=$('toast'); element.textContent=message; element.classList.toggle('error',error);
    element.classList.add('visible');clearTimeout(toastTimer);toastTimer=setTimeout(()=>element.classList.remove('visible'),4700);
  }
  async function command(payload) {
    if(requestBusy)return;
    requestBusy=true;
    try {
      const reply=await api('/api/action',payload);
      const dispatched=reply.result.includes('DISPATCHED');
      notify(dispatched?'Pedido enviado à Probe. Aguarde confirmação do firmware (ACK / eventos).':
        'Alteração aplicada: '+reply.result.replace(/#CTRL OK ?/,'').trim().slice(0,135));
      await refresh();
    } catch(error) {notify(error.message,true);}
    finally {requestBusy=false;}
  }
  function setView(view) {
    activeView=view;
    document.querySelectorAll('.nav-item').forEach(el=>el.classList.toggle('active',el.dataset.view===view));
    document.querySelectorAll('.view').forEach(el=>el.classList.toggle('active',el.id==='view-'+view));
    $('page-title').textContent=({topology:'Topologia interativa',metrics:'Métricas e eventos',experiments:'Ensaios reprodutíveis'})[view];
    if(view==='experiments')refreshExperiments();
    if(view==='metrics'&&state)renderMetrics(state);
  }
  document.querySelectorAll('[data-view]').forEach(el=>el.addEventListener('click',()=>setView(el.dataset.view)));
  $('form-node').addEventListener('submit',event=>{event.preventDefault();command({type:'node.add',id:$('new-node').value});});
  // IOC_WIRELESS_FLOW_HOTFIX_V1
  $('form-sensor').addEventListener('submit',event=>{
    event.preventDefault();
    const node=$('new-sensor-node').value;
    command({type:'sensor.create',uuid:$('new-uuid').value,profile:$('new-profile').value,
      node:node ? Number(node) : null,rssi:$('new-sensor-rssi').value});
  });
  function updateSensorNodeChoices(snapshot) {
    const control=$('new-sensor-node');
    const nodes=snapshot.nodes.filter(n=>n.id!==0&&n.powered).map(n=>n.id).sort((a,b)=>a-b);
    const key=nodes.join(',');
    if(control.dataset.ids===key)return;
    const previous=control.value;
    control.replaceChildren();
    control.append(elt('option',{value:''},'Sem enlace (invisível para os Nodes)'));
    for(const id of nodes)control.append(elt('option',{value:id},`Node ${String(id).padStart(2,'0')} · RSSI configurável`));
    control.value=nodes.map(String).includes(previous)?previous:(nodes.length?String(nodes[0]):'');
    control.dataset.ids=key;
  }
  $('pause-btn').addEventListener('click',()=>{
    const running=state?.snapshot?.running;
    command({type:running?'sim.pause':'sim.resume'});
  });
  $('step-btn').addEventListener('click',()=>command({type:'sim.step',ms:1000}));
  $('refresh-btn').addEventListener('click',refresh);

  function elt(name, attributes={}, content='') {
    const item=document.createElement(name);
    for(const [key,val] of Object.entries(attributes))item.setAttribute(key,String(val));
    if(content!==undefined)item.textContent=content;
    return item;
  }
  function svgEl(name,attributes={},value) {
    const item=document.createElementNS(svgNS,name);
    for(const [key,val] of Object.entries(attributes))item.setAttribute(key,String(val));
    if(value!==undefined)item.textContent=String(value);
    return item;
  }
  function putSVG(parent,name,attrs,value){const e=svgEl(name,attrs,value);parent.appendChild(e);return e;}
  function linkLine(svg,x1,y1,x2,y2,associated,rssi) {
    putSVG(svg,'line',{x1,y1,x2,y2,stroke:associated?'#7bc7ac':'#6fa9d5',
      'stroke-width':associated?2.9:1.4,'stroke-opacity':associated?0.9:0.5,
      'stroke-dasharray':associated?'none':'5 5',class:'legend-edge'});
    putSVG(svg,'text',{x:(x1+x2)/2+5,y:(y1+y2)/2-5,class:'map-edge-label'},`${rssi} dBm`);
  }
  function renderTopology(model) {
    const svg=$('topology');svg.replaceChildren();
    const nodes=model.nodes.filter(n=>n.id!==0);
    const sensors=model.sensors;
    const cols=5,nRows=Math.max(1,Math.ceil(nodes.length/cols));
    const sensorStart=260+nRows*87;
    const sRows=Math.max(1,Math.ceil(sensors.length/4));
    const height=Math.max(465,sensorStart+140*sRows);
    svg.setAttribute('viewBox',`0 0 1000 ${height}`);
    const grid=putSVG(svg,'g',{'opacity':0.16});
    for(let x=24;x<1000;x+=34)putSVG(grid,'line',{x1:x,y1:20,x2:x,y2:height-20,stroke:'#5c8a9b','stroke-width':0.45});
    for(let y=20;y<height;y+=34)putSVG(grid,'line',{x1:20,y1:y,x2:980,y2:y,stroke:'#5c8a9b','stroke-width':0.45});
    const probeGroup=putSVG(svg,'g',{class:'map-node','data-node-id':0});
    putSVG(probeGroup,'rect',{x:440,y:22,width:120,height:55,rx:9,fill:'#25475b',stroke:'#6fa9d5','stroke-width':1.8});
    putSVG(probeGroup,'text',{x:500,y:46,'text-anchor':'middle',class:'map-label'},'PROBE 00');
    putSVG(probeGroup,'text',{x:500,y:62,'text-anchor':'middle',class:'map-sub'},'Instrumentação');
    putSVG(svg,'line',{x1:500,y1:77,x2:500,y2:119,stroke:'#6fa9d5','stroke-width':2});
    putSVG(svg,'line',{x1:70,y1:119,x2:930,y2:119,stroke:'#718c9f','stroke-width':5,'stroke-linecap':'round'});
    putSVG(svg,'text',{x:77,y:102,fill:'#b9cad5','font-size':11},'CAN 2.0 · 500 kbit/s');
    const nodePos=new Map();
    nodes.forEach((node,i)=>{
      const col=i%cols,row=Math.floor(i/cols),x=105+col*197,y=160+row*87;
      nodePos.set(node.id,{x:x+55,y:y+28});
      putSVG(svg,'line',{x1:x+55,y1:119,x2:x+55,y2:y,stroke:'#718c9f','stroke-width':2,opacity:0.65});
      const group=putSVG(svg,'g',{class:'map-node'+(selected?.kind==='node'&&selected.id===node.id?' map-selected':''),'data-node-id':node.id,tabindex:0,role:'button','aria-label':`Node ${node.id}, ${node.state_name}`});
      const fill=!node.powered?'#303741':node.state_name==='LEADER'?'#315e50':'#263d49';
      const border=!node.powered?'#71808d':node.state_name==='LEADER'?'#7bc7ac':'#6f98ad';
      putSVG(group,'rect',{x,y,width:110,height:56,rx:8,fill,stroke:border,'stroke-width':selected?.kind==='node'&&selected.id===node.id?2.5:1.4});
      putSVG(group,'circle',{cx:x+12,cy:y+16,r:4,fill:node.powered?'#7bc7ac':'#ec8e90'});
      putSVG(group,'text',{x:x+58,y:y+24,'text-anchor':'middle',class:'map-label'},'NODE '+String(node.id).padStart(2,'0'));
      putSVG(group,'text',{x:x+58,y:y+41,'text-anchor':'middle',class:'map-sub'},node.powered?node.state_name:'DESLIGADO');
      group.addEventListener('click',()=>select({kind:'node',id:node.id}));
      group.addEventListener('keydown',event=>{if(event.key==='Enter')select({kind:'node',id:node.id});});
    });
    const sensorPositions=new Map();
    sensors.forEach((sensor,i)=>{
      const x=135+(i%4)*245,y=sensorStart+Math.floor(i/4)*130;
      sensorPositions.set(sensor.uuid,{x,y});
      model.links.filter(link=>link.uuid===sensor.uuid).forEach(link=>{
        const target=nodePos.get(link.node);
        if(target)linkLine(svg,x+60,y,target.x,target.y+29,sensor.parent===link.node,link.rssi);
      });
    });
    sensors.forEach(sensor=>{
      const position=sensorPositions.get(sensor.uuid),{x,y}=position;
      const group=putSVG(svg,'g',{class:'map-sensor'+(selected?.kind==='sensor'&&selected.uuid===sensor.uuid?' map-selected':''),
        'data-sensor-uuid':sensor.uuid,tabindex:0,role:'button',
        'aria-label':`Sensor ${sensor.uuid} ${sensor.profile}`});
      putSVG(group,'rect',{x:x-12,y:y-7,width:145,height:65,rx:9,fill:sensor.powered?'#283f55':'#363942',stroke:sensor.powered?'#6fa9d5':'#7d8188','stroke-width':1.5});
      putSVG(group,'circle',{cx:x+7,cy:y+12,r:4,fill:sensor.powered?'#6fa9d5':'#ec8e90'});
      putSVG(group,'text',{x:x+62,y:y+17,'text-anchor':'middle',class:'map-label'},sensor.profile==='ACOUSTIC'?'SENSOR ACÚSTICO':sensor.profile==='PANEL'?'PAINEL PAPERCOLOR':'SENSOR VIBRAÇÃO');
      putSVG(group,'text',{x:x+62,y:y+35,'text-anchor':'middle',class:'map-sub'},sensor.uuid.slice(-8));
      putSVG(group,'text',{x:x+62,y:y+48,'text-anchor':'middle',class:'map-sub'},sensor.parent?`Pai lógico: Node ${sensor.parent}`:'Não associado');
      group.addEventListener('click',()=>select({kind:'sensor',uuid:sensor.uuid}));
      group.addEventListener('keydown',event=>{if(event.key==='Enter')select({kind:'sensor',uuid:sensor.uuid});});
      group.addEventListener('pointerdown',event=>startDragging(event,sensor.uuid));
    });
    if(sensors.length===0)putSVG(svg,'text',{x:500,y:sensorStart+42,'text-anchor':'middle',fill:'#6e91a4','font-size':13},'Adicione um sensor para configurar observações BLE');
  }

  function startDragging(event,uuid){
    if(event.button!==0)return;
    const startX=event.clientX,startY=event.clientY;
    let ghost=null,moved=false;
    function move(e){
      if(Math.hypot(e.clientX-startX,e.clientY-startY)<8&&!moved)return;
      moved=true;
      if(!ghost){ghost=elt('div',{class:'drag-ghost'},'Conectar rádio de '+uuid.slice(-8));document.body.appendChild(ghost);}
      ghost.style.left=(e.clientX+12)+'px';ghost.style.top=(e.clientY+12)+'px';
    }
    async function up(e){
      window.removeEventListener('pointermove',move);window.removeEventListener('pointerup',up);
      if(ghost)ghost.remove();
      if(!moved)return;
      const target=document.elementFromPoint(e.clientX,e.clientY)?.closest('[data-node-id]');
      if(!target)return;
      const node=Number(target.dataset.nodeId);
      if(node===0)return;
      const rssi=window.prompt(`RSSI observado no Node ${node} (−127 a 0 dBm):`,'-55');
      if(rssi!==null)await command({type:'sensor.rssi',uuid,node,rssi});
    }
    window.addEventListener('pointermove',move);
    window.addEventListener('pointerup',up,{once:true});
  }
  function select(choice){selected=choice;if(state?.snapshot){renderTopology(state.snapshot);renderInspector(state.snapshot);}}
  function row(parent,label,value){
    const el=elt('div',{class:'kv'});el.append(elt('span',{},label),elt('span',{},String(value)));parent.append(el);
  }
  function button(parent,label,payload,kind='btn-secondary'){
    const item=elt('button',{type:'button',class:'btn btn-small '+kind},label);
    item.addEventListener('click',()=>command(payload));parent.append(item);return item;
  }
  function field(parent,label,type,value){
    const wrapper=elt('div');const title=elt('label',{},label);const input=elt('input',{type,value});
    wrapper.append(title,input);parent.append(wrapper);return input;
  }
  function selectField(parent,label,options,initial){
    const wrapper=elt('div');wrapper.append(elt('label',{},label));const drop=elt('select');
    options.forEach(value=>{const opt=elt('option',{value},value);drop.append(opt)});drop.value=initial;wrapper.append(drop);parent.append(wrapper);return drop;
  }
  function actionsBox(parent){const b=elt('div',{class:'editor-actions'});parent.append(b);return b;}
  function heading(parent,text){parent.append(elt('h3',{},text));}
  function renderInspector(model){
    const root=$('inspector-body');root.replaceChildren();root.className='';
    if(!selected){root.className='empty-state';root.textContent='Selecione um dispositivo na topologia.';return;}
    if(selected.kind==='node'){
      const n=model.nodes.find(v=>v.id===selected.id);
      if(!n){root.textContent='Node removido da bancada.';return;}
      root.append(elt('div',{class:'inspector-title'},n.id===0?'Probe 00':`Node CAN ${String(n.id).padStart(2,'0')}`));
      row(root,'Estado',n.state_name);row(root,'Energia',n.powered?'Ligado':'Desligado');
      row(root,'Transmissão',n.tx_disconnected?'Interrompida':'Ativa');
      row(root,'Ciclos do loop (modelo)',n.budget_pct+'%');row(root,'Boots disponíveis',n.boots_remaining);
      if(n.id===0){root.append(elt('p',{},'A Probe é instrumentação: não participa da eleição ou da associação como gateway.'));return;}
      const section=elt('section',{class:'editor-section'});root.append(section);heading(section,'Operações físicas simuladas');
      let actions=actionsBox(section);
      button(actions,n.powered?'Desligar':'Ligar',{type:'node.power',id:n.id,state:n.powered?'OFF':'ON'},n.powered?'btn-danger':'btn-secondary');
      if(n.powered)button(actions,'Reiniciar',{type:'node.power',id:n.id,state:'RESTART'});
      button(actions,n.tx_disconnected?'Restaurar TX':'Interromper TX',{type:'node.tx',id:n.id,state:n.tx_disconnected?'ON':'OFF'});
      button(actions,'Remover Node',{type:'node.remove',id:n.id},'btn-danger');
      heading(section,'Restrição de processamento');
      const inline=elt('div',{class:'inline-fields'});section.append(inline);
      const budget=field(inline,'Orçamento de loop (%)','number',n.budget_pct);budget.min='1';budget.max='100';
      const b=elt('button',{class:'btn btn-secondary grid-button',type:'button'},'Aplicar budget');
      b.addEventListener('click',()=>command({type:'node.budget',id:n.id,pct:budget.value}));inline.append(b);
      return;
    }
    const s=model.sensors.find(v=>v.uuid===selected.uuid);
    if(!s){root.textContent='Sensor removido da bancada.';return;}
    root.append(elt('div',{class:'inspector-title'},s.profile==='ACOUSTIC'?'Sensor acústico':s.profile==='PANEL'?'Painel em papel eletrônico':'Sensor de vibração'));
    if(s.profile==='PANEL')renderPanelSection(root,s);
    row(root,'UUID',s.uuid);row(root,'Energia',s.powered?'Ligado':'Desligado');
    row(root,'Responsável lógico',s.parent?`Node ${s.parent}`:'Sem associação');
    // IOC_USB_HYBRID_STAGE5
    row(root,'Origem da aquisição',s.source==='USB'?'Sensor físico (USB CDC)':'Modelo sintético');
    if(s.source==='USB')row(root,'Idade da última amostra USB',s.usb_age_ms+' ms');
    row(root,'Identificador filho',s.child||'—');row(root,'Enlaces físicos modelados',s.links);
    if(!s.links)root.append(elt('p',{class:'wireless-hint'},
      'Sem alcance BLE: configure RSSI com um Node. Enquanto não houver enlace, este sensor não poderá aparecer na TUI.'));
    else if(!s.parent)root.append(elt('p',{class:'wireless-hint'},
      'Enlace BLE configurado. Com o simulador em execução, aguarde alguns segundos, abra Sensores sem fio (F7) na TUI e solicite a associação.'));
    else root.append(elt('p',{class:'wireless-hint'},'Vínculo lógico registrado; confirme a sessão e a telemetria na TUI.'));
    let section=elt('section',{class:'editor-section'});root.append(section);
    heading(section,'Estado do dispositivo');let acts=actionsBox(section);
    button(acts,s.powered?'Desligar':'Ligar',{type:'sensor.power',uuid:s.uuid,state:s.powered?'OFF':'ON'},s.powered?'btn-danger':'btn-secondary');
    button(acts,'Remover sensor',{type:'sensor.remove',uuid:s.uuid},'btn-danger');
    if(s.profile==='ACOUSTIC'){
      heading(section,'Anomalia acústica sintética');let extra=actionsBox(section);
      button(extra,'Ativar falha',{type:'sensor.machine',uuid:s.uuid,state:'ON'});
      button(extra,'Eliminar falha',{type:'sensor.machine',uuid:s.uuid,state:'OFF'});
    }
    section=elt('section',{class:'editor-section'});root.append(section);heading(section,'Configuração de enlace BLE / Wi-Fi');
    if(model.nodes.filter(v=>v.id>0).length===0){section.append(elt('p',{},'Adicione um Node antes de criar um enlace.'));return;}
    let grid=elt('div',{class:'inline-fields'});section.append(grid);
    const nodes=model.nodes.filter(v=>v.id>0).map(v=>String(v.id));
    const chosen=selectField(grid,'Node observador',nodes,String(s.parent||Number(nodes[0])));
    const rssi=field(grid,'RSSI (dBm)','number','-55');rssi.min='-127';rssi.max='0';
    const loss=field(grid,'Perda (%)','number','0');loss.min='0';loss.max='100';
    acts=actionsBox(section);
    const rssiBtn=elt('button',{type:'button',class:'btn btn-small btn-secondary'},'Aplicar RSSI');
    rssiBtn.addEventListener('click',()=>command({type:'sensor.rssi',uuid:s.uuid,node:chosen.value,rssi:rssi.value}));acts.append(rssiBtn);
    const lossBtn=elt('button',{type:'button',class:'btn btn-small btn-secondary'},'Aplicar perda');
    lossBtn.addEventListener('click',()=>command({type:'sensor.loss',uuid:s.uuid,node:chosen.value,pct:loss.value}));acts.append(lossBtn);
    const disconnect=elt('button',{type:'button',class:'btn btn-small btn-danger'},'Retirar alcance');
    disconnect.addEventListener('click',()=>command({type:'sensor.rssi',uuid:s.uuid,node:chosen.value,rssi:'OFF'}));acts.append(disconnect);
    for(const link of model.links.filter(v=>v.uuid===s.uuid)){
      const line=elt('div',{class:'kv'});line.append(elt('span',{},`Node ${link.node}`),elt('span',{},`${link.rssi} dBm · perdas ${link.loss_pct}%`));section.append(line);
    }
    section=elt('section',{class:'editor-section'});root.append(section);
    heading(section,'Associação lógica pelo firmware');
    section.append(elt('p',{},'Criar um enlace não associa automaticamente o sensor. A vinculação passa pelo protocolo da Probe 00.'));
    grid=elt('div',{class:'inline-fields'});section.append(grid);
    const parent=selectField(grid,'Node responsável',nodes,String(s.parent||Number(nodes[0])));
    const policy=selectField(grid,'Política de reassociação',['MANUAL','AUTO'],'MANUAL');
    acts=actionsBox(section);
    const bind=elt('button',{type:'button',class:'btn btn-small btn-primary'},'Associar');
    bind.addEventListener('click',()=>{
      if(!model.links.some(link=>link.uuid===s.uuid&&String(link.node)===parent.value)){
        notify('Configure primeiro o alcance BLE (RSSI) para o Node escolhido.',true);return;
      }
      if(!model.running){notify('Retome o relógio virtual e aguarde os anúncios BLE antes de associar.',true);return;}
      command({type:'probe.bind',uuid:s.uuid,node:parent.value,policy:policy.value});
    });acts.append(bind);
    const unbind=elt('button',{type:'button',class:'btn btn-small btn-danger'},'Desassociar');
    unbind.addEventListener('click',()=>command({type:'probe.unbind',uuid:s.uuid,node:parent.value}));acts.append(unbind);
    const pol=elt('button',{type:'button',class:'btn btn-small btn-secondary'},'Alterar política');
    pol.addEventListener('click',()=>command({type:'probe.policy',uuid:s.uuid,node:parent.value,policy:policy.value}));acts.append(pol);
  }


  // Painel em papel eletrônico: a página que está no papel (painel virtual) ou o
  // espelho do PaperColor ligado pela USB, desenhada a partir da lista de desenho.
  const PAPER_INK=['#1c1c1c','#f4f1ea','#e8c33a','#b8312f','#4a3fd0','#3f7d3f'];
  const panelCache=new Map();
  async function fetchPanel(uuid){
    const cached=panelCache.get(uuid);
    if(cached&&Date.now()-cached.at<1800)return cached.data;
    try{const data=await api('/api/panel?uuid='+encodeURIComponent(uuid));panelCache.set(uuid,{at:Date.now(),data});return data;}
    catch(error){return cached?cached.data:null;}
  }
  function drawPaper(canvas,page){
    const ctx=canvas.getContext('2d'),scale=canvas.width/400;
    ctx.setTransform(scale,0,0,scale,0,0);
    ctx.fillStyle=PAPER_INK[1];ctx.fillRect(0,0,400,600);
    if(!page||!page.items.length){ctx.fillStyle='#888';ctx.font='16px sans-serif';ctx.fillText('Sem página desenhada',110,300);return;}
    for(const it of page.items){
      const ink=PAPER_INK[it.ink]||PAPER_INK[0];ctx.fillStyle=ink;ctx.strokeStyle=ink;ctx.lineWidth=1;
      if(it.op===1)ctx.fillRect(it.x,it.y,it.w,it.h);
      else if(it.op===2)ctx.strokeRect(it.x+0.5,it.y+0.5,it.w-1,it.h-1);
      else if(it.op===4){ctx.beginPath();ctx.moveTo(it.x,it.y+0.5);ctx.lineTo(it.x+it.w,it.y+it.h+0.5);ctx.stroke();}
      else if(it.op===5){
        const z=it.size,x=it.x,y=it.y;ctx.lineWidth=3;ctx.beginPath();
        if(it.w===0){ctx.arc(x+z/2,y+z/2,z/2-1,0,Math.PI*2);ctx.fill();}
        else if(it.w===1){ctx.moveTo(x+z/2,y);ctx.lineTo(x,y+z);ctx.lineTo(x+z,y+z);ctx.closePath();ctx.fill();}
        else if(it.w===2){ctx.moveTo(x+2,y+2);ctx.lineTo(x+z-2,y+z-2);ctx.moveTo(x+z-2,y+2);ctx.lineTo(x+2,y+z-2);ctx.stroke();}
        else if(it.w===3){ctx.moveTo(x+1,y+z/2);ctx.lineTo(x+z/3,y+z-3);ctx.lineTo(x+z-2,y+2);ctx.stroke();}
        else{ctx.moveTo(x+z/2,y);ctx.lineTo(x,y+z*0.75);ctx.lineTo(x+z,y+z*0.75);ctx.closePath();ctx.fill();
             ctx.beginPath();ctx.moveTo(x+z/2,y+z);ctx.lineTo(x,y+z*0.25);ctx.lineTo(x+z,y+z*0.25);ctx.closePath();ctx.fill();}
      } else if(it.op===3){
        // Avanço fixo de meia altura, como a fonte efont do firmware.
        ctx.font=`${it.size}px monospace`;ctx.textBaseline='top';
        const chars=[...it.text].length||1,natural=ctx.measureText(it.text).width||1,target=chars*it.size/2;
        ctx.save();ctx.translate(it.x,it.y+1);ctx.scale(target/natural,1);ctx.fillText(it.text,0,0);ctx.restore();
      }
    }
  }
  function renderPanelSection(root,s){
    const section=elt('section',{class:'editor-section'});root.append(section);
    heading(section,'Página no papel');
    const info=elt('p',{class:'wireless-hint'},'Carregando a página…');section.append(info);
    // Os dois LEDs do topo do PaperColor (a cor é ampliada para ficar visível na tela).
    const ledRow=elt('div',{style:'display:flex;gap:10px;align-items:center;margin:4px 0 8px'});
    const ledDots=[0,1].map(()=>elt('span',{style:'width:18px;height:18px;border-radius:50%;background:#222;border:1px solid #556;display:inline-block'}));
    const ledText=elt('span',{class:'wireless-hint'},'LEDs: rede · painel');
    ledRow.append(...ledDots,ledText);section.append(ledRow);
    const showLeds=page=>{
      (page?page.leds:['000000','000000']).forEach((hex,i)=>{
        const v=[0,2,4].map(k=>parseInt(hex.slice(k,k+2),16)||0),m=Math.max(...v);
        ledDots[i].style.background=m?`rgb(${v.map(c=>Math.round(c*255/m)).join(',')})`:'#222';
      });
      if(page)ledText.textContent=page.off?'Desligando: LEDs vermelhos até a tela final.':
        page.unacked?`Alerta sem ciência (${page.alerts} anúncios) — qualquer botão dá ciência.`:
        `LEDs: rede · painel. ${page.acks} ciências registradas.`;
    };
    const canvas=elt('canvas',{width:300,height:450,style:'width:100%;max-width:300px;border-radius:6px;border:1px solid #445'});
    section.append(canvas);
    const cached=panelCache.get(s.uuid);
    if(cached){drawPaper(canvas,cached.data);showLeds(cached.data);}
    fetchPanel(s.uuid).then(page=>{
      drawPaper(canvas,page);showLeds(page);
      if(!page){info.textContent='Página indisponível.';return;}
      const levels=['rede normal','a rede está tratando','atenção','requer você'];
      info.textContent=(page.source==='MIRROR'?'Espelho do PaperColor físico (USB). ':page.source==='VIRTUAL'?'Painel virtual (firmware na bancada). ':
        page.source==='PAPER'?'Painel desligado: o papel guarda a última tela. ':'Painel desligado. ')+
        `Nível: ${levels[page.level]||'?'} · sessão ${page.session?'ativa com o Node '+page.node:'inativa'} · ${page.renders} desenhos · ${page.beeps} alertas sonoros.`;
    });
    const acts=actionsBox(section);
    for(const [label,btn] of [['A: marca','A'],['B: página','B'],['C: atualiza','C'],['⏻ toque','P'],['⏻ segurar (desliga)','H']])
      button(acts,label,{type:'panel.button',uuid:s.uuid,button:btn},btn==='H'?'btn-danger':'btn-secondary');
    const grid=elt('div',{class:'inline-fields'});section.append(grid);
    const note=field(grid,'Marca/nota (CardKB, #etiquetas)','text','');note.maxLength=96;
    const send=elt('button',{class:'btn btn-secondary grid-button',type:'button'},'Enviar nota');
    send.addEventListener('click',()=>{if(note.value.trim())command({type:'panel.note',uuid:s.uuid,text:note.value.trim()});});grid.append(send);
    if(s.source!=='USB'){
      const bat=actionsBox(section);
      button(bat,'Bateria (dorme)',{type:'panel.battery',uuid:s.uuid,state:'ON'});
      button(bat,'USB (acordado)',{type:'panel.battery',uuid:s.uuid,state:'OFF'});
    }
  }

  function drawChart(canvas,values,maxValue,titleColor){
    if(!canvas)return;
    const ratio=Math.max(1,window.devicePixelRatio||1),rect=canvas.getBoundingClientRect();
    const w=Math.max(240,rect.width),h=220;
    canvas.width=Math.floor(w*ratio);canvas.height=h*ratio;
    const ctx=canvas.getContext('2d');ctx.scale(ratio,ratio);
    ctx.clearRect(0,0,w,h);const pad={l:45,r:14,t:19,b:31};
    const innerW=w-pad.l-pad.r,innerH=h-pad.t-pad.b;
    const mx=Math.max(maxValue,1,...values);
    ctx.lineWidth=1;ctx.strokeStyle='#314956';ctx.fillStyle='#98b2c0';ctx.font='11px system-ui';
    for(let i=0;i<=4;i++){
      const y=pad.t+innerH*i/4;
      ctx.beginPath();ctx.moveTo(pad.l,y);ctx.lineTo(w-pad.r,y);ctx.stroke();
      ctx.fillText((mx*(1-i/4)).toFixed(mx>10?0:1),4,y+4);
    }
    if(!values.length){ctx.fillStyle='#76909c';ctx.fillText('Aguardando amostras...',pad.l+10,h/2);return;}
    ctx.strokeStyle=titleColor;ctx.lineWidth=2.25;ctx.beginPath();
    values.forEach((v,i)=>{const x=pad.l+innerW*(values.length===1?.5:i/(values.length-1)),y=pad.t+innerH*(1-v/mx);if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);});ctx.stroke();
    ctx.fillStyle='#7997a7';ctx.fillText('−'+values.length+' amostras',pad.l,h-7);ctx.fillText('atual',w-pad.r-27,h-7);
  }
  function renderMetrics(payload) {
    const snapshots=payload.series||[];
    drawChart($('chart-load'),snapshots.map(x=>x.can_load_confirmed_pct||0),100,'#7bc7ac');
    drawChart($('chart-frames'),snapshots.map(x=>x.frames_per_sec||0),1,'#6fa9d5');
    const table=$('node-table');table.replaceChildren();
    for(const n of payload.snapshot?.nodes||[]){
      const row=elt('tr');[String(n.id).padStart(2,'0'),n.state_name,n.powered?'Ligado':'Desligado',
        n.tx_disconnected?'OFF':'ON',n.budget_pct+'%',String(n.boots_remaining)].forEach(value=>row.append(elt('td',{},value)));
      table.append(row);
    }
    const events=$('event-list');events.replaceChildren();
    if(payload.events.length===0){events.append(elt('div',{class:'empty-state'},payload.log_available?'Ainda não há eventos registrados.':'Sem log: use --record ao anexar-se à TUI.'));}
    for(const event of payload.events){
      const line=elt('div',{class:'event'});line.append(elt('b',{},clock(event.t_ms)),document.createTextNode(event.message));events.append(line);
    }
  }
  async function refresh(){
    try{
      const data=await api('/api/state');state=data;
      const s=data.snapshot;if(!s)return;
      updateSensorNodeChoices(s);
      $('connection-label').textContent=data.connected?'Bancada conectada':'Sem resposta';
      $('connection-subtitle').textContent=data.connected?'Motor de simulação ativo':String(data.error||'Erro de controle').slice(0,65);
      document.querySelector('.status-dot').classList.toggle('offline',!data.connected);
      $('clock-text').textContent=clock(s.t_ms);
      $('pause-btn').textContent=s.running?'Pausar':'Retomar';
      $('step-btn').disabled=!!s.running;
      $('metric-load').textContent=pct(data.rates?.can_load_confirmed_pct);
      $('metric-frames').textContent=number(data.rates?.frames_per_sec)+' q/s';
      $('metric-devices').textContent=s.nodes.filter(x=>x.powered&&x.id>0).length+' / '+s.sensors.length;
      $('metric-leader').textContent=s.leader?String(s.leader).padStart(2,'0'):'—';
      if(activeView==='topology'){
        renderTopology(s);
        if(!document.querySelector('#inspector-body input:focus, #inspector-body select:focus'))renderInspector(s);
      }
      if(activeView==='metrics')renderMetrics(data);
    }catch(error){$('connection-label').textContent='Falha de comunicação';document.querySelector('.status-dot').classList.add('offline');}
  }
  async function initTemplates(){
    try{
      templates=await api('/api/experiments/templates');
      const selector=$('template-select');selector.replaceChildren();
      for(const item of templates)selector.append(elt('option',{value:item.file},item.spec.name));
      selector.addEventListener('change',()=>{
        const value=templates.find(t=>t.file===selector.value);
        if(value)$('experiment-spec').value=JSON.stringify(value.spec,null,2);
      });
      if(templates.length){selector.value=templates[0].file;selector.dispatchEvent(new Event('change'));}
    }catch(err){notify(err.message,true);}
  }
  $('run-experiment').addEventListener('click',async()=>{
    try{
      const spec=JSON.parse($('experiment-spec').value);
      const result=await api('/api/experiments/run',spec);
      notify('Ensaio agendado no laboratório: '+result.job.job_id);
      setView('experiments');await refreshExperiments();
    }catch(err){notify(err.message,true);}
  });
  async function downloadRun(id){
    try{
      const response=await fetch(`/api/experiments/export/${encodeURIComponent(id)}`,{headers:{'X-IOC-Token':token}});
      if(!response.ok){const error=await response.json();throw new Error(error.error||'Falha ao exportar');}
      const blob=await response.blob();const url=URL.createObjectURL(blob);
      const anchor=elt('a',{href:url,download:`iot-over-can-${id}.zip`});
      document.body.append(anchor);anchor.click();anchor.remove();setTimeout(()=>URL.revokeObjectURL(url),1000);
    }catch(ex){notify(ex.message,true);}
  }
  async function refreshExperiments(){
    if(activeView!=='experiments')return;
    try{
      const [jobs,results]=await Promise.all([api('/api/experiments/jobs'),api('/api/experiments/results')]);
      const newJobs=JSON.stringify(jobs),newResults=JSON.stringify(results);
      if(newJobs!==lastJobs){
        lastJobs=newJobs;const host=$('jobs-list');host.replaceChildren();host.className='jobs-list';
        if(!jobs.length)host.append(elt('div',{class:'empty-state'},'Nenhum ensaio recente.'));
        jobs.forEach(job=>{
          const item=elt('div',{class:'job'});item.append(elt('strong',{},job.name),elt('small',{},job.status==='running'?
            `Executando repetição ${job.progress.repeat||1}/${job.progress.total||1}`:
            job.status==='completed'?`Concluído · ${job.result_id}`:`Falhou · ${job.error||''}`));host.append(item);
        });
      }
      if(newResults!==lastResults){
        lastResults=newResults;const host=$('results-list');host.replaceChildren();host.className='results-list';
        if(!results.length)host.append(elt('div',{class:'empty-state'},'Nenhum resultado salvo.'));
        results.forEach(result=>{
          const item=elt('div',{class:'job'});item.append(elt('strong',{},result.name),elt('small',{},`${result.status} · ${result.runs} repetição(ões) · ${result.id.slice(0,8)}`));
          const button=elt('button',{type:'button',class:'btn btn-small btn-secondary'},'Exportar ZIP');
          button.addEventListener('click',()=>downloadRun(result.id));item.append(button);host.append(item);
        });
      }
    }catch(ex){notify(ex.message,true);}
  }
  window.addEventListener('resize',()=>{if(activeView==='metrics'&&state)renderMetrics(state);});
  initTemplates().then(()=>{});refresh();
  setInterval(()=>{refresh();refreshExperiments();},1700);
})();
