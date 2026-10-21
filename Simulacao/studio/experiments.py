"""Ensaios reproduzíveis: processo isolado, relógio pausado e comandos no tempo virtual.

Gera spec.json, trace.log, bus_stats.csv, events.jsonl e summary.json por repetição.
Não usa o relógio de parede para acionar passos científicos.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import queue
import re
import statistics
import subprocess
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

try:
    from .protocol import parse_fields, parse_status, validate_native_command
except ImportError:
    from protocol import parse_fields, parse_status, validate_native_command

STAMP=re.compile(r'^@(\d+) (.*)$')
RELEVANT=('WIRELESS_FAILOVER','WIRELESS_ASSOC','FAULT_EVENT','HEARTBEAT','WIRELESS_LINK','DTC')


def validate_spec(spec: dict[str,Any]) -> dict[str,Any]:
    if not isinstance(spec,dict): raise ValueError('Especificação deve ser um objeto JSON')
    name=spec.get('name','ensaio')
    if not isinstance(name,str) or not (1<=len(name)<=80) or any(ord(c)<32 for c in name):
        raise ValueError('Nome inválido')
    duration=spec.get('duration_ms')
    if type(duration) is not int or not 1000<=duration<=300000:
        raise ValueError('duration_ms: inteiro entre 1000 e 300000')
    reps=spec.get('repetitions',1)
    if type(reps) is not int or not 1<=reps<=10:
        raise ValueError('repetitions: inteiro entre 1 e 10')
    steps=spec.get('steps',[])
    if not isinstance(steps,list) or len(steps)>200:
        raise ValueError('steps: vetor de até 200 entradas')
    clean=[]
    previous=0
    for i,step in enumerate(steps):
        if not isinstance(step,dict): raise ValueError(f'Passo {i}: objeto esperado')
        at=step.get('at_ms')
        if type(at) is not int or not 0<=at<=duration or at<previous:
            raise ValueError(f'Passo {i}: at_ms fora do intervalo ou não ordenado')
        previous=at
        cmd=validate_native_command(step.get('command'))
        if cmd.startswith(('STEP','PAUSE','RESUME','STATUS')):
            raise ValueError('Comandos temporais são gerenciados pelo runner')
        clean.append({'at_ms':at,'command':cmd})
    seed=spec.get('seed', 0x01234567)
    if type(seed) is not int or not 0<=seed<=0xFFFFFFFF:
        raise ValueError('seed: inteiro entre 0 e 4294967295')
    seed_mode=spec.get('seed_mode','fixed')
    if seed_mode not in ('fixed','increment'):
        raise ValueError('seed_mode deve ser fixed ou increment')
    description=spec.get('description','')
    if not isinstance(description,str) or len(description)>1000:
        raise ValueError('Descrição inválida')
    return {'name':name,'description':description, 'duration_ms':duration,
            'repetitions':reps,'steps':clean,'seed':seed,'seed_mode':seed_mode}


class ControlledProcess:
    def __init__(self,binary:Path,log_path:Path,events_path:Path,bus_path:Path):
        self.binary=binary
        self.log_path=log_path
        self.events_path=events_path
        self.bus_path=bus_path
        self.proc:subprocess.Popen[str]|None=None
        self.responses:queue.Queue[str]=queue.Queue()
        self.reader:threading.Thread|None=None
        self.stderr:threading.Thread|None=None
        self.stderr_lines:list[str]=[]
        self.stats:list[dict[str,Any]]=[]
        self.events:list[dict[str,Any]]=[]
        self._log_file=None
        self._events_file=None
        self._bus_file=None
        self._bus_writer=None

    def __enter__(self):
        self._log_file=self.log_path.open('w',encoding='utf-8',buffering=1)
        self._events_file=self.events_path.open('w',encoding='utf-8',buffering=1)
        self._bus_file=self.bus_path.open('w',encoding='utf-8',newline='')
        self._bus_writer=csv.writer(self._bus_file)
        self._bus_writer.writerow(['t_ms','load_pct_probe','peak_pct_probe','frames','bits','rx_err','tx_err'])
        self.proc=subprocess.Popen([str(self.binary),'--interactive-paused'],
            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
            text=True,encoding='utf-8',bufsize=1)
        self.reader=threading.Thread(target=self._read_stdout,daemon=True)
        self.reader.start()
        self.stderr=threading.Thread(target=self._read_stderr,daemon=True)
        self.stderr.start()
        self._wait_ready()
        return self

    def _read_stdout(self):
        assert self.proc and self.proc.stdout
        for raw in self.proc.stdout:
            line=raw.rstrip('\r\n')
            if line.startswith('#CTRL'):
                self.responses.put(line)
                continue
            m=STAMP.match(line)
            if not m: continue
            assert self._log_file and self._events_file and self._bus_writer
            self._log_file.write(line+'\n')
            at=int(m.group(1)); msg=m.group(2)
            if 'BUS_STATS ' in msg:
                f=parse_fields(msg)
                try:
                    entry={'t_ms':at,'load':float(f['load']),'peak':float(f['peak']),
                           'frames':int(f['frames']),'bits':int(f['bits']),
                           'rx_err':int(f.get('rx_err',0)),'tx_err':int(f.get('tx_err',0))}
                    self.stats.append(entry)
                    self._bus_writer.writerow([entry[k] for k in ('t_ms','load','peak','frames','bits','rx_err','tx_err')])
                except (KeyError,ValueError): pass
            if any(fragment in msg for fragment in RELEVANT):
                event={'t_ms':at,'message':msg}
                self.events.append(event)
                self._events_file.write(json.dumps(event,ensure_ascii=False)+'\n')

    def _read_stderr(self):
        assert self.proc and self.proc.stderr
        for raw in self.proc.stderr:
            if len(self.stderr_lines)<50:self.stderr_lines.append(raw.rstrip())

    def _wait_ready(self,timeout:float=20):
        deadline=time.monotonic()+timeout
        while True:
            try: line=self.responses.get(timeout=max(0.01,deadline-time.monotonic()))
            except queue.Empty:raise TimeoutError('Simulador não sinalizou READY')
            if line.startswith('#CTRL OK READY'):return
            if line.startswith('#CTRL ERROR'):raise RuntimeError(line)
            if time.monotonic()>deadline:raise TimeoutError('Aguardando READY')

    def send(self,command:str,timeout:float=20)->str:
        assert self.proc and self.proc.stdin
        self.proc.stdin.write(command+'\n');self.proc.stdin.flush()
        lines=[]
        deadline=time.monotonic()+timeout
        multi=command=='STATUS'
        while True:
            try: line=self.responses.get(timeout=max(.01,deadline-time.monotonic()))
            except queue.Empty:raise TimeoutError(f'Sem resposta de {command}')
            if line.startswith('#CTRL ERROR'):
                raise RuntimeError(f'{command}: {line}')
            if line.startswith('#CTRL INFO'):continue
            lines.append(line)
            if (multi and line=='#CTRL END') or (not multi and line.startswith('#CTRL OK')):
                return '\n'.join(lines)+'\n'
            if time.monotonic()>deadline:raise TimeoutError(f'Aguardando conclusão de {command}')

    def __exit__(self,typ,value,tb):
        if self.proc:
            if self.proc.poll() is None:
                try: self.send('QUIT',timeout=2)
                except (OSError,RuntimeError,TimeoutError,BrokenPipeError):pass
            try:self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill();self.proc.wait(timeout=3)
        for thread in (self.reader,self.stderr):
            if thread:thread.join(timeout=3)
        if self.proc:
            for stream in (self.proc.stdin,self.proc.stdout,self.proc.stderr):
                if stream:stream.close()
        for stream in (self._log_file,self._events_file,self._bus_file):
            if stream:stream.close()


def run_experiment(binary:Path,spec:dict[str,Any],out_root:Path,
                   progress:Callable[[dict[str,Any]],None]|None=None)->dict[str,Any]:
    spec=validate_spec(spec)
    binary=binary.expanduser().resolve()
    if not binary.is_file():raise FileNotFoundError(binary)
    out_root=out_root.resolve();out_root.mkdir(parents=True,exist_ok=True)
    run_id=uuid.uuid4().hex
    root=out_root/run_id
    root.mkdir(mode=0o700)
    (root/'spec.json').write_text(json.dumps(spec,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    executable_hash=hashlib.sha256(binary.read_bytes()).hexdigest()
    overview={'id':run_id,'name':spec['name'],'created_at':datetime.now(timezone.utc).isoformat(),
              'engine_sha256':executable_hash,'model':'Native host simulator; 500 kbit/s; fixed seed 0x01234567',
              'time_basis':'virtual time, initial state after formNetwork()',
              'warning':'Sem calibração física; BUS_STATS da Probe não conta todos os erros ou retransmissões.',
              'repetitions':[]}
    try:
        for repeat in range(spec['repetitions']):
            rep_dir=root/f'run-{repeat+1:02d}'
            rep_dir.mkdir()
            if progress:progress({'status':'running','id':run_id,'repeat':repeat+1,'total':spec['repetitions']})
            commands=[]
            with ControlledProcess(binary,rep_dir/'trace.log',rep_dir/'events.jsonl',rep_dir/'bus_stats.csv') as engine:
                first=parse_status(engine.send('STATUS'))
                if first['running']:raise RuntimeError('O executável não iniciou pausado; recompilação necessária')
                seed=(spec['seed'] + (repeat if spec['seed_mode']=='increment' else 0)) & 0xFFFFFFFF
                engine.send(f'SEED {seed}')
                def advance_virtual(delta: int) -> None:
                    while delta:
                        part=min(delta,120000)  # limite do comando STEP da bancada
                        engine.send(f'STEP {part}',timeout=90)
                        delta-=part

                offset=0
                for step in spec['steps']:
                    advance=step['at_ms']-offset
                    if advance:
                        advance_virtual(advance)
                    offset=step['at_ms']
                    response=engine.send(step['command'],timeout=15)
                    commands.append({'at_ms':offset,'command':step['command'],'result':response.strip()})
                remaining=spec['duration_ms']-offset
                if remaining:advance_virtual(remaining)
                last=parse_status(engine.send('STATUS'))
                if last['t_ms']-first['t_ms']!=spec['duration_ms']:
                    raise RuntimeError('Relógio de simulação não corresponde à duração pedida')
                stats=list(engine.stats)
                events=list(engine.events)
            metrics={'index':repeat+1,'seed':seed,'initial_t_ms':first['t_ms'],'final_t_ms':last['t_ms'],
                     'frames_delta':last['frames']-first['frames'],
                     'bits_confirmed_delta':last['bits']-first['bits'],
                     'utilization_confirmed_pct':round(100*(last['bits']-first['bits']) /
                                       (500_000*(spec['duration_ms']/1000)),4),
                     'mean_probe_load_pct':round(sum(x['load'] for x in stats)/len(stats),4) if stats else None,
                     'probe_samples':len(stats),
                     'failovers':sum('WIRELESS_FAILOVER' in x['message'] for x in events),
                     'events':len(events),'leader_final':last['leader'],
                     'node_count_final':len(last['nodes']),'sensor_count_final':len(last['sensors']),
                     'commands':commands,'replay':str((rep_dir/'trace.log').relative_to(root))}
            (rep_dir/'metrics.json').write_text(json.dumps(metrics,indent=2,ensure_ascii=False)+'\n')
            overview['repetitions'].append(metrics)
        metrics_keys=('frames_delta','bits_confirmed_delta','utilization_confirmed_pct',
                      'mean_probe_load_pct','failovers')
        aggregate={}
        for key in metrics_keys:
            numbers=[float(row[key]) for row in overview['repetitions'] if row.get(key) is not None]
            if numbers:
                aggregate[key]={'mean':round(statistics.mean(numbers),5),
                                'min':min(numbers),'max':max(numbers),
                                'stddev_sample':round(statistics.stdev(numbers),5) if len(numbers)>1 else None}
        overview['aggregate']=aggregate
        overview['warning_repetitions']='Sementes fixas produzem repetições determinísticas, não amostras estatisticamente independentes.' if spec['seed_mode']=='fixed' else 'Sementes incrementais variam sequências futuras do PRNG, mas a formação inicial permanece fixa e os ensaios requerem validação física.'
        with (root/'comparison.csv').open('w',newline='',encoding='utf-8') as stream:
            writer=csv.writer(stream)
            writer.writerow(['repetition','seed',*metrics_keys])
            for row in overview['repetitions']:
                writer.writerow([row['index'],row['seed'],*[row.get(key) for key in metrics_keys]])
        (root/'comparison.json').write_text(json.dumps(aggregate,ensure_ascii=False,indent=2)+'\n')
        overview['status']='completed'
    except Exception as exc:
        overview['status']='failed'
        overview['error']=str(exc)
        (root/'summary.json').write_text(json.dumps(overview,ensure_ascii=False,indent=2)+'\n')
        raise
    (root/'summary.json').write_text(json.dumps(overview,ensure_ascii=False,indent=2)+'\n')
    if progress:progress({'status':'completed','id':run_id,'result':overview})
    return overview


def main():
    ap=argparse.ArgumentParser(description='Executa ensaios de tempo virtual reproduzíveis')
    ap.add_argument('--binary',type=Path,default=Path('/tmp/ioc_sim_network'))
    ap.add_argument('--spec',type=Path,required=True)
    ap.add_argument('--out',type=Path,default=Path('Simulacao/results'))
    args=ap.parse_args()
    spec=json.loads(args.spec.read_text(encoding='utf-8'))
    result=run_experiment(args.binary,spec,args.out)
    print(json.dumps({'id':result['id'],'status':result['status'],'output':str(args.out/result['id'])},ensure_ascii=False))


if __name__=='__main__':main()
