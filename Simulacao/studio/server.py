#!/usr/bin/env python3
"""Simulation Studio: web local para topologia e ensaios do IoT-over-CAN.

Somente loopback, zero dependências externas, sem HTTP externo.
"""
from __future__ import annotations

import argparse
import io
import json
import os
import secrets
import sys
import threading
import time
import zipfile
from collections import deque
from datetime import datetime,timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse
from typing import Any

ROOT=Path(__file__).resolve().parent
SIM_DIR=ROOT.parent
sys.path.insert(0,str(SIM_DIR))
try:
    from .protocol import request, parse_status, parse_fields, build_command
    from .experiments import run_experiment,validate_spec
except ImportError:
    from protocol import request, parse_status, parse_fields, build_command
    from experiments import run_experiment,validate_spec
from sim_bridge import SimBridge
from sim_ctl import discover_socket

STATIC=ROOT/'static'
TEMPLATES=ROOT/'experiments'
# IOC_WIRELESS_FLOW_HOTFIX_V1
ALERTS=('WIRELESS_CANDIDATE','WIRELESS_FAILOVER','WIRELESS_ASSOC','FAULT_EVENT','DTC','HEARTBEAT','WIRELESS_LINK', 'ACK ')


class StudioState:
    def __init__(self,binary:Path,socket_path:str|None,record:Path|None,results:Path):
        self.binary=binary.expanduser().resolve()
        self.socket=socket_path
        self.record=record
        self.results=results.expanduser().resolve()
        self.results.mkdir(parents=True,exist_ok=True)
        self.token=secrets.token_urlsafe(32)
        self.bridge:SimBridge|None=None
        self.bridge_thread:threading.Thread|None=None
        self.stop=threading.Event()
        self.lock=threading.RLock()
        self.command_lock=threading.Lock()
        self.history:deque[dict[str,Any]]=deque(maxlen=180)
        self.events:deque[dict[str,Any]]=deque(maxlen=120)
        self.last_snapshot:dict[str,Any]|None=None
        self.status_error:str|None=None
        self.last_status_at=0.0
        self.log_offset=0
        self.latest_probe:dict[str,Any]|None=None
        self.last_absolute:dict[str,Any]|None=None
        self.jobs:dict[str,dict[str,Any]]={}
        self.jobs_lock=threading.Lock()
        self.poll_thread:threading.Thread|None=None

    def start(self):
        if self.socket is None:
            if not self.binary.is_file() or not os.access(self.binary,os.X_OK):
                raise FileNotFoundError(f'Compile antes o motor: {self.binary}')
            if self.record is None:
                self.record=self.results/'studio-live.log'
            self.record.parent.mkdir(parents=True,exist_ok=True)
            self.bridge=SimBridge(self.binary,self.record)
            errors=[]
            def bridge_worker():
                try:self.bridge.serve()
                except Exception as ex:
                    errors.append(ex)
                    self.bridge.ready.set()
            self.bridge_thread=threading.Thread(target=bridge_worker,daemon=True,name='studio-bridge')
            self.bridge_thread.start()
            if not self.bridge.ready.wait(15) or errors:
                raise RuntimeError(f'Falha ao iniciar ponte: {errors}')
            if self.bridge.proc is None or self.bridge.proc.poll() is not None:
                raise RuntimeError('O processo de simulação não iniciou')
            assert self.bridge.socket_path
            self.socket=str(self.bridge.socket_path)
        else:
            # Na conexão externa, não apropriamos o ciclo de vida da TUI existente.
            request(self.socket,'STATUS',timeout=4)
        self.refresh()
        self.poll_thread=threading.Thread(target=self._poll,daemon=True,name='studio-poll')
        self.poll_thread.start()

    def close(self):
        self.stop.set()
        if self.bridge:self.bridge.stop.set()
        if self.poll_thread:self.poll_thread.join(timeout=3)
        if self.bridge_thread:self.bridge_thread.join(timeout=6)

    def _tail_log(self):
        path=self.record
        if not path or not path.is_file():return
        size=path.stat().st_size
        if size<self.log_offset:self.log_offset=0
        with path.open('r',encoding='utf-8',errors='replace') as stream:
            stream.seek(self.log_offset)
            while True:
                raw=stream.readline()
                if not raw or not raw.endswith('\n'):break
                self.log_offset=stream.tell()
                if not raw.startswith('@'):continue
                head,_,message=raw[1:].partition(' ')
                if not head.isdigit():continue
                t_ms=int(head)
                message=message.strip()
                if 'BUS_STATS ' in message:
                    fields=parse_fields(message)
                    try:self.latest_probe={'t_ms':t_ms,'load_pct':float(fields['load']),
                                           'peak_pct':float(fields.get('peak',fields['load']))}
                    except (KeyError,ValueError):pass
                if any(tag in message for tag in ALERTS):
                    self.events.append({'t_ms':t_ms,'message':message[:400]})

    def _poll(self):
        while not self.stop.wait(1.0):
            try:self.refresh()
            except Exception as ex:
                with self.lock:self.status_error=str(ex)

    def refresh(self):
        assert self.socket
        with self.command_lock:
            raw=request(self.socket,'STATUS',timeout=6)
        snapshot=parse_status(raw)
        with self.lock:
            previous=self.last_absolute
            self._tail_log()
            sample={'t_ms':snapshot['t_ms'],'frames':snapshot['frames'],'bits':snapshot['bits'],
                    'can_load_confirmed_pct':0.,'frames_per_sec':0.}
            if previous and snapshot['t_ms']>previous['t_ms']:
                elapsed=(snapshot['t_ms']-previous['t_ms'])/1000
                delta_bits=max(0,snapshot['bits']-previous['bits'])
                delta_frames=max(0,snapshot['frames']-previous['frames'])
                sample['can_load_confirmed_pct']=round(100*delta_bits/(500000*elapsed),4)
                sample['frames_per_sec']=round(delta_frames/elapsed,2)
            self.last_absolute={k:snapshot[k] for k in ('t_ms','frames','bits')}
            if not self.history or snapshot['t_ms']!=self.history[-1]['t_ms']:
                self.history.append(sample)
            self.last_snapshot=snapshot
            self.status_error=None
            self.last_status_at=time.monotonic()

    def get_state(self)->dict[str,Any]:
        with self.lock:
            last=self.history[-1] if self.history else {}
            return {'snapshot':self.last_snapshot,'series':list(self.history),
                    'events':list(self.events)[-60:][::-1],
                    'probe':self.latest_probe,'rates':last,
                    'connected':self.status_error is None,'error':self.status_error,
                    'log_available':self.record is not None,
                    'source':'Tráfego CAN modelado; telemetria de sensores USB pode ser física',
                    'record':str(self.record) if self.record else None}

    def action(self,payload:dict[str,Any])->str:
        if not isinstance(payload,dict):
            raise ValueError('Ação deve ser um objeto JSON')
        if payload.get('type')=='sensor.create':
            # Primeiro o dispositivo, depois a sua observacao de radio.
            # A Probe continua responsavel pela associacao logica.
            add=build_command({'type':'sensor.add','uuid':payload.get('uuid'),
                               'profile':payload.get('profile')})
            uuid=add.split()[2]
            target=payload.get('node')
            link=None
            if target not in (None,''):
                link=build_command({'type':'sensor.rssi','uuid':uuid,'node':target,
                                    'rssi':payload.get('rssi')})
            assert self.socket
            with self.command_lock:
                if link:
                    snapshot=parse_status(request(self.socket,'STATUS',timeout=8))
                    target_id=int(link.split()[3])
                    if not any(n['id']==target_id and n['powered'] for n in snapshot['nodes']):
                        raise ValueError('Node selecionado ausente ou desligado: não foi criado sensor')
                created=request(self.socket,add,timeout=20)
                if not link:
                    return created.rstrip()+"\n#CTRL INFO SENSOR SEM ENLACE: configure RSSI para permitir descoberta BLE\n"
                try:
                    request(self.socket,link,timeout=20)
                except Exception as exc:
                    # Compensacao: caso o segundo comando falhe, restaura a
                    # topologia original em vez de deixar um sensor invisivel.
                    try:request(self.socket,'SENSOR REMOVE '+uuid,timeout=10)
                    except Exception as cleanup:
                        raise RuntimeError(f'RSSI falhou ({exc}); limpeza tambem falhou ({cleanup})') from exc
                    raise RuntimeError(f'Falha ao configurar alcance; inclusão desfeita: {exc}') from exc
                return f'#CTRL OK SENSOR CREATED_AND_LINKED uuid={uuid} node={target_id} rssi={link.split()[-1]}\n'
        command=build_command(payload)
        if command=='STATUS':return 'Consulte GET /api/state'
        assert self.socket
        with self.command_lock:
            return request(self.socket,command,timeout=20)

    def templates(self)->list[dict[str,Any]]:
        output=[]
        for path in sorted(TEMPLATES.glob('*.json')):
            try:
                content=validate_spec(json.loads(path.read_text(encoding='utf-8')))
                output.append({'file':path.name,'spec':content})
            except (OSError,ValueError,json.JSONDecodeError):pass
        return output

    def launch_experiment(self,spec:dict[str,Any])->dict[str,Any]:
        if not self.binary.is_file() or not os.access(self.binary,os.X_OK):
            raise RuntimeError('Executável necessário para experimentos isolados: --binary')
        validated=validate_spec(spec)
        with self.jobs_lock:
            if any(x['status']=='running' for x in self.jobs.values()):
                raise RuntimeError('Já existe um experimento em execução')
            job_id=secrets.token_hex(8)
            self.jobs[job_id]={'job_id':job_id,'status':'running','name':validated['name'],
                               'progress':{},'created_at':datetime.now(timezone.utc).isoformat()}
        def work():
            try:
                def on_progress(info):
                    with self.jobs_lock:self.jobs[job_id]['progress']=info
                result=run_experiment(self.binary,validated,self.results,on_progress)
                with self.jobs_lock:
                    self.jobs[job_id].update(status='completed',result_id=result['id'],progress={})
            except Exception as ex:
                with self.jobs_lock:self.jobs[job_id].update(status='failed',error=str(ex))
        threading.Thread(target=work,daemon=True,name='studio-experiment').start()
        return dict(self.jobs[job_id])

    def get_jobs(self):
        with self.jobs_lock:return list(self.jobs.values())[::-1]

    def list_results(self):
        found=[]
        for path in sorted(self.results.glob('*/summary.json'),key=lambda p:p.stat().st_mtime,reverse=True)[:50]:
            try:
                meta=json.loads(path.read_text(encoding='utf-8'))
                found.append({'id':meta['id'],'name':meta['name'],'status':meta['status'],
                              'created_at':meta['created_at'],'runs':len(meta.get('repetitions',[]))})
            except (KeyError,OSError,json.JSONDecodeError):continue
        return found

    def export(self,run_id:str)->bytes:
        if not isinstance(run_id,str) or len(run_id)!=32 or any(c not in '0123456789abcdef' for c in run_id):
            raise ValueError('ID de resultado inválido')
        root=(self.results/run_id).resolve()
        if root.parent!=self.results or not (root/'summary.json').is_file():
            raise FileNotFoundError('Experimento não encontrado')
        memory=io.BytesIO()
        with zipfile.ZipFile(memory,'w',compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(root.rglob('*')):
                if path.is_file() and not path.is_symlink():
                    archive.write(path,path.relative_to(root))
        return memory.getvalue()


class Handler(BaseHTTPRequestHandler):
    server_version='IOC-Studio/0.4'
    def log_message(self,fmt,*args):
        print('[http] '+fmt%args,file=sys.stderr)

    @property
    def state(self)->StudioState:return self.server.studio  # type: ignore[attr-defined]

    def headers_common(self,typ:str,length:int):
        self.send_header('Content-Type',typ)
        self.send_header('Content-Length',str(length))
        self.send_header('Cache-Control','no-store')
        self.send_header('X-Content-Type-Options','nosniff')
        self.send_header('Referrer-Policy','no-referrer')
        self.send_header('X-Frame-Options','DENY')
        self.send_header('Content-Security-Policy',"default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'")

    def send_bytes(self,status:int,content:bytes,typ='application/json; charset=utf-8',extra=None):
        self.send_response(status)
        self.headers_common(typ,len(content))
        if extra:
            for k,v in extra.items():self.send_header(k,v)
        self.end_headers()
        try:self.wfile.write(content)
        except (BrokenPipeError,ConnectionResetError):pass

    def send_json(self,status:int,obj:Any):
        self.send_bytes(status,json.dumps(obj,ensure_ascii=False,allow_nan=False).encode('utf-8'))

    def is_local_host(self):
        host=self.headers.get('Host','')
        expected=f'127.0.0.1:{self.server.server_port}'
        alternate=f'localhost:{self.server.server_port}'
        return host in (expected,alternate)

    def is_authorized(self):
        return secrets.compare_digest(self.headers.get('X-IOC-Token',''),self.state.token)

    def is_same_origin(self):
        origin=self.headers.get('Origin')
        host=self.headers.get('Host','')
        return not origin or origin==f'http://{host}'

    def do_GET(self):
        if not self.is_local_host():
            self.send_json(403,{'error':'Acesso permitido somente pelo endereço local'});return
        path=urlparse(self.path).path
        if path=='/':
            raw=(STATIC/'index.html').read_text(encoding='utf-8')
            raw=raw.replace('__IOC_TOKEN__',self.state.token)
            self.send_bytes(200,raw.encode('utf-8'),'text/html; charset=utf-8')
            return
        files={'/static/app.js':('app.js','text/javascript; charset=utf-8'),
               '/static/style.css':('style.css','text/css; charset=utf-8')}
        if path in files:
            name,typ=files[path]
            self.send_bytes(200,(STATIC/name).read_bytes(),typ)
            return
        if path.startswith('/api/'):
            if not self.is_authorized():
                self.send_json(403,{'error':'Token local ausente ou inválido'})
                return
            try:
                if path=='/api/state':result=self.state.get_state()
                elif path=='/api/experiments/templates':result=self.state.templates()
                elif path=='/api/experiments/jobs':result=self.state.get_jobs()
                elif path=='/api/experiments/results':result=self.state.list_results()
                elif path.startswith('/api/experiments/export/'):
                    run_id=path.rsplit('/',1)[-1]
                    self.send_bytes(200,self.state.export(run_id),'application/zip',
                                    {'Content-Disposition':f'attachment; filename="ioc-experiment-{run_id}.zip"'})
                    return
                else:
                    self.send_json(404,{'error':'Rota desconhecida'});return
                self.send_json(200,result)
            except (ValueError,RuntimeError,FileNotFoundError,OSError) as ex:
                self.send_json(400,{'error':str(ex)})
            return
        self.send_json(404,{'error':'Página não encontrada'})

    def do_POST(self):
        path=urlparse(self.path).path
        if not self.is_local_host() or not self.is_authorized() or not self.is_same_origin():
            self.send_json(403,{'error':'Origem ou token inválido'});return
        length=self.headers.get('Content-Length','')
        if not length.isdecimal() or not 0<int(length)<=60000 or self.headers.get('Content-Type','').split(';')[0]!='application/json':
            self.send_json(400,{'error':'Corpo JSON inválido ou excede 60 kB'});return
        try:
            data=json.loads(self.rfile.read(int(length)))
            if path=='/api/action':
                result=self.state.action(data)
                self.send_json(200,{'ok':True,'result':result})
                return
            if path=='/api/experiments/run':
                result=self.state.launch_experiment(data)
                self.send_json(202,{'ok':True,'job':result})
                return
            self.send_json(404,{'error':'Rota desconhecida'})
        except (ValueError,TypeError,RuntimeError,OSError,json.JSONDecodeError) as ex:
            self.send_json(400,{'error':str(ex)})


class LocalServer(ThreadingHTTPServer):
    daemon_threads=True
    def __init__(self,address,studio):
        super().__init__(address,Handler)
        self.studio=studio


def main():
    ap=argparse.ArgumentParser(description='Simulation Studio IoT-over-CAN (somente acesso local)')
    ap.add_argument('--binary',type=Path,default=Path('/tmp/ioc_sim_network'))
    ap.add_argument('--socket',help='Anexa-se a uma TUI --sim existente; não encerra a sessão anexa')
    ap.add_argument('--attach',action='store_true',help='Conecta à única sessão --sim ativa')
    ap.add_argument('--record',type=Path,help='Arquivo de replay com telemetria da Probe')
    ap.add_argument('--results',type=Path,default=SIM_DIR/'results')
    ap.add_argument('--port',type=int,default=8765)
    ap.add_argument('--browser',action='store_true',help='Abre navegador local')
    args=ap.parse_args()
    if not 1024<=args.port<=65535:ap.error('Porta entre 1024 e 65535')
    socket_path=args.socket
    if args.attach and socket_path is None:
        socket_path=discover_socket()
    state=StudioState(args.binary,socket_path,args.record,args.results)
    try:
        state.start()
        server=LocalServer(('127.0.0.1',args.port),state)
        url=f'http://127.0.0.1:{server.server_address[1]}'
        print(f'[studio] Interface: {url}',flush=True)
        print(f'[studio] CONTROLE: {state.socket}',flush=True)
        if args.browser:
            import webbrowser
            webbrowser.open(url)
        try:server.serve_forever(poll_interval=.5)
        except KeyboardInterrupt:pass
        finally:server.server_close()
    finally:state.close()


if __name__=='__main__':main()
