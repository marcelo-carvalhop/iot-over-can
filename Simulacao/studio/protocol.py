"""Contratos de controle da bancada virtual IoT-over-CAN.

Não confunde PROBE (protocolo operacional) com comandos #CTRL (simulação).
"""
from __future__ import annotations

import re
import socket
from dataclasses import dataclass
from pathlib import Path
from typing import Any

UUID_RE = re.compile(r"^(?:0x[0-9a-fA-F]{1,16}|[1-9][0-9]{0,19})$")
STATE_NAMES = {0:'IDLE', 1:'GATEWAY', 2:'JOINING', 3:'ELECTION', 4:'RECOVERING',
               5:'LEADER', 6:'FOLLOWER', 7:'FAULT'}


def parse_fields(line: str) -> dict[str, str]:
    return dict(part.split('=', 1) for part in line.split() if '=' in part)


def parse_status(output: str) -> dict[str, Any]:
    result: dict[str, Any] = {'nodes':[], 'sensors':[], 'links':[]}
    success = False
    for line in output.splitlines():
        if line.startswith('#CTRL ERROR'):
            raise RuntimeError(line.removeprefix('#CTRL ERROR').strip())
        if line.startswith('#CTRL OK STATUS '):
            success=True
            f=parse_fields(line)
            result.update(t_ms=int(f['t_ms']), running=f['running']=='1', leader=int(f['leader']),
                          frames=int(f['frames']), bits=int(f['bits']))
        elif line.startswith('#CTRL NODE '):
            f=parse_fields(line)
            result['nodes'].append(dict(id=int(f['id']), powered=f['powered']=='1',
                state=int(f['state']), state_name=STATE_NAMES.get(int(f['state']), 'UNKNOWN'),
                tx_disconnected=f['tx_disconnected']=='1', restarts=int(f['restarts']),
                boots_remaining=int(f['boots_remaining']), budget_pct=int(f.get('budget_pct','100'))))
        elif line.startswith('#CTRL SENSOR '):
            f=parse_fields(line)
            result['sensors'].append(dict(uuid=f['uuid'].upper().replace('0X','0x'), profile=f['profile'],
                powered=f['powered']=='1', parent=int(f['parent']), child=int(f['child']),
                mode=int(f['mode']), links=int(f['links']),
                source=f.get('source','SYNTH'),usb_age_ms=int(f.get('usb_age_ms','0')))) # IOC_USB_HYBRID_STAGE5
        elif line.startswith('#CTRL LINK '):
            f=parse_fields(line)
            result['links'].append(dict(uuid=f['uuid'].upper().replace('0X','0x'),
                node=int(f['node']), rssi=int(f['rssi']), loss_pct=int(f.get('loss_pct','0'))))
    if not success or '#CTRL END' not in output:
        raise RuntimeError('Resposta STATUS incompleta ou inválida')
    result['nodes'].sort(key=lambda n:n['id'])
    result['sensors'].sort(key=lambda n:n['uuid'])
    return result


def check_uuid(value: Any) -> str:
    value=str(value).strip()
    if not UUID_RE.fullmatch(value):
        raise ValueError('UUID: forneça um inteiro decimal ou 0x hexadecimal')
    number=int(value,0) if value.lower().startswith('0x') else int(value,10)
    if not 0 < number <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError('UUID fora do intervalo de 64 bits')
    return f'0x{number:016X}'


def num(value: Any, low: int, high: int, label: str) -> int:
    if isinstance(value, bool):
        raise ValueError(f'{label}: inteiro requerido')
    try:
        n=int(str(value))
    except ValueError as ex:
        raise ValueError(f'{label}: inteiro requerido') from ex
    if str(n)!=str(value).strip() and not (isinstance(value,int)):
        raise ValueError(f'{label}: inteiro requerido')
    if n < low or n > high:
        raise ValueError(f'{label}: intervalo permitido {low}..{high}')
    return n


def enum(value: Any, values: set[str], label: str) -> str:
    value=str(value).upper().strip()
    if value not in values:
        raise ValueError(f'{label}: escolha {", ".join(sorted(values))}')
    return value


def build_command(payload: dict[str, Any]) -> str:
    """Constrói comandos apenas a partir de operações enumeradas; sem comandos shell."""
    if not isinstance(payload, dict):
        raise ValueError('Ação JSON deve ser um objeto')
    op=payload.get('type')
    if op in ('sim.pause','sim.resume','sim.status'):
        return op.split('.')[1].upper()
    if op=='sim.step':
        return f'STEP {num(payload.get("ms"),0,120000,"Tempo")}'
    if isinstance(op,str) and op.startswith('node.'):
        node=num(payload.get('id'),1,31,'Node ID')
        verb=op[5:]
        if verb in ('add','remove'):
            return f'NODE {node} {verb.upper()}'
        if verb=='power':
            return f'NODE {node} POWER {enum(payload.get("state"),{"ON","OFF","RESTART"},"Energia")}'
        if verb=='tx':
            return f'NODE {node} TX {enum(payload.get("state"),{"ON","OFF"},"Transmissão")}'
        if verb=='budget':
            return f'NODE {node} BUDGET {num(payload.get("pct"),1,100,"Orçamento")}'
    if isinstance(op,str) and op.startswith('sensor.'):
        verb=op[7:]
        uuid=check_uuid(payload.get('uuid'))
        if verb=='add':
            return f'SENSOR ADD {uuid} {enum(payload.get("profile"),{"VIBRATION","ACOUSTIC"},"Perfil")}'
        if verb=='remove':
            return f'SENSOR REMOVE {uuid}'
        if verb=='power':
            return f'SENSOR POWER {uuid} {enum(payload.get("state"),{"ON","OFF"},"Energia")}'
        if verb=='rssi':
            node=num(payload.get('node'),1,31,'Node ID')
            raw=str(payload.get('rssi')).strip().upper()
            level='OFF' if raw=='OFF' else str(num(payload.get('rssi'),-127,0,'RSSI'))
            return f'SENSOR RSSI {uuid} {node} {level}'
        if verb=='loss':
            node=num(payload.get('node'),1,31,'Node ID')
            return f'SENSOR LINK {uuid} {node} LOSS {num(payload.get("pct"),0,100,"Perda")}'
        if verb=='machine':
            return f'SENSOR FAULT {uuid} MACHINE {enum(payload.get("state"),{"ON","OFF"},"Falha")}'
    if op in ('probe.bind','probe.unbind','probe.policy'):
        uuid=check_uuid(payload.get('uuid'))
        node=num(payload.get('node'),1,31,'Node ID')
        if op=='probe.bind':
            policy=enum(payload.get('policy','MANUAL'), {'AUTO','MANUAL'}, 'Política')
            return f'PROBE WIRELESS BIND {node} {uuid}'+(' AUTO' if policy=='AUTO' else '')
        if op=='probe.unbind':
            return f'PROBE WIRELESS UNBIND {node} {uuid}'
        return f'PROBE WIRELESS POLICY {node} {uuid} '+enum(payload.get('policy'),{'AUTO','MANUAL'},'Política')
    raise ValueError('Operação não permitida nesta versão do Studio')


def validate_native_command(command: str) -> str:
    """Exige que experimentos usem a mesma lista autorizada da interface."""
    if not isinstance(command,str) or not command or len(command)>512 or any(c in command for c in '\r\n\0'):
        raise ValueError('Comando vazio, muito longo ou contendo quebra de linha')
    p=command.split()
    upper=[item.upper() for item in p]
    if upper in (["PAUSE"],["RESUME"],["STATUS"]):
        return ' '.join(upper)
    if len(p)==2 and upper[0]=='STEP':
        return build_command({'type':'sim.step','ms':p[1]})
    if len(p)>=3 and upper[0]=='NODE':
        base={'id':p[1]}
        if len(p)==3 and upper[2] in ('ADD','REMOVE'):
            return build_command({**base,'type':'node.'+p[2].lower()})
        if len(p)==4 and upper[2] in ('POWER','TX','BUDGET'):
            op={'POWER':'node.power','TX':'node.tx','BUDGET':'node.budget'}[upper[2]]
            key='pct' if upper[2]=='BUDGET' else 'state'
            return build_command({**base,'type':op,key:p[3]})
    if upper[0]=='SENSOR':
        if len(p)==4 and upper[1]=='ADD':
            return build_command({'type':'sensor.add','uuid':p[2],'profile':p[3]})
        if len(p)==3 and upper[1]=='REMOVE':
            return build_command({'type':'sensor.remove','uuid':p[2]})
        if len(p)==4 and upper[1]=='POWER':
            return build_command({'type':'sensor.power','uuid':p[2],'state':p[3]})
        if len(p)==5 and upper[1]=='RSSI':
            return build_command({'type':'sensor.rssi','uuid':p[2],'node':p[3],'rssi':p[4]})
        if len(p)==6 and upper[1]=='LINK' and upper[4]=='LOSS':
            return build_command({'type':'sensor.loss','uuid':p[2],'node':p[3],'pct':p[5]})
        if len(p)==5 and upper[1]=='FAULT' and upper[3]=='MACHINE':
            return build_command({'type':'sensor.machine','uuid':p[2],'state':p[4]})
    if len(p) in (5,6) and upper[:3]==['PROBE','WIRELESS','BIND']:
        if len(p)==6 and upper[5]!='AUTO': raise ValueError('Política inválida')
        return build_command({'type':'probe.bind','node':p[3],'uuid':p[4],
                            'policy':'AUTO' if len(p)==6 else 'MANUAL'})
    if len(p)==5 and upper[:3]==['PROBE','WIRELESS','UNBIND']:
        return build_command({'type':'probe.unbind','node':p[3],'uuid':p[4]})
    if len(p)==6 and upper[:3]==['PROBE','WIRELESS','POLICY']:
        return build_command({'type':'probe.policy','node':p[3],'uuid':p[4],'policy':p[5]})
    raise ValueError('Comando fora do conjunto permitido em experimentos')


def request(socket_path: str|Path, command: str, *, timeout: float=15.0) -> str:
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(str(socket_path))
        client.sendall((command+'\n').encode('utf-8'))
        client.shutdown(socket.SHUT_WR)
        chunks=[]
        total=0
        while True:
            block=client.recv(8192)
            if not block: break
            total+=len(block)
            if total>1_000_000: raise RuntimeError('Resposta acima do limite')
            chunks.append(block)
    result=b''.join(chunks).decode('utf-8', errors='replace')
    if '#CTRL ERROR' in result:
        raise RuntimeError(result.strip())
    if not result.startswith('#CTRL OK'):
        raise RuntimeError('Resposta de controle ausente ou inválida')
    return result
