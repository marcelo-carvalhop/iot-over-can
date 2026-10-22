#!/usr/bin/env python3
"""Integra Pico W/Pico 2 W real via USB CDC na bancada CAN virtual.

As leituras sao reais. BLE, Wi-Fi/UDP e autenticacao de sessao na topologia
continuam sendo executados pelo modelo simulado. Esta ponte NAO liga o Pico
fisico ao AP do Node virtual nem substitui a seguranca do protocolo real.
"""
from __future__ import annotations

import argparse
import math
import os
import re
import stat
import sys
import time
from pathlib import Path

from sim_ctl import discover_socket
from studio.protocol import parse_status, request

FIELDS_RE = re.compile(r"([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)")
UUID_RE = re.compile(r"^0x[0-9a-fA-F]{1,16}$")
VIB_MODES = {'IDLE':0,'ROTATING':1,'STRUCTURAL':2,'SEISMIC':3}
ACQ_MODES = {'IDLE':4,'DRDY':1,'POLLING':2,'SIM':3,'UNKNOWN':0}
AUDIO_MODES = {'IDLE':0,'LEVEL':1,'MACHINE':2,'EVENTS':3}
WINDOW_TYPES = {'RECT':0,'RECTANGULAR':0,'HANN':1,'HAMMING':2,'FLATTOP':3,'BLACKMAN':4}
VIB_FIELDS = {'WIN','RMS','KURT','CREST','PEAK_HZ','PEAK_AMP','ENT','PPV_MM_S','BATT_PCT','BATT_MV','DTC','DTC_COUNT'}
AUDIO_FIELDS = {'WIN','RATE_EFF_HZ','INTERVAL_MS','CPU_PCT','COVERAGE_PCT','LAEQ_DB','LCEQ_DB','LZEQ_DB','LZPEAK_DB','LAF_DB','LAFMAX_DB','LAFMIN_DB','KURT','CREST','PEAK_HZ','CENTROID_HZ','FLATNESS', 'LEQ_T_DB','L10_DB','L50_DB','L90_DB','BATT_PCT','BATT_MV','DTC','DTC_COUNT',
    'DOSE_NR15_PCT','DOSE_NHO01_PCT','ELAPSED_S','TONAL_DB','F0_HZ','HARMONICS',
    'ANOMALY_Z','BASELINE_PCT','EVENTS','EVENTS_INTERVAL','EVENT_PEAK_DB','EVENT_SEL_DB',
    'EVENT_DURATION_MS','EVENT_CENTROID_HZ','EVENT_AGE_S','STA_LTA_DB','BACKGROUND_DB'}


def fields(line: str) -> dict[str,str]:
    return dict(FIELDS_RE.findall(line))


def clean_line(raw: bytes) -> str:
    # O console ecoa os comandos e imprime prompts de forma assíncrona.
    decoded = raw.decode('utf-8','replace').replace('\x00','').strip()
    return decoded.rsplit('EDGE>',1)[-1].strip() if 'EDGE>' in decoded else decoded


def numeric(value: str) -> float | None:
    try:
        num=float(int(value,16)) if value.lower().startswith('0x') else float(value)
    except (ValueError,OverflowError):
        return None
    return num if math.isfinite(num) and abs(num) <= 1e8 else None


def convert_tel(line: str, profile: str, rate_hz: float | None = None, window: str | None = None) -> str | None:
    if not line.startswith('TEL '):
        return None
    values=fields(line)
    if profile == 'ACOUSTIC':
        if numeric(values.get('LAEQ_DB','')) is None:
            return None
        result={'MODE':AUDIO_MODES.get(values.get('MODE','').upper(),1)}
        if window is not None and window.upper() in WINDOW_TYPES:
            result['WINDOW_TYPE']=WINDOW_TYPES[window.upper()]
        for key in ('ANOMALY','MIC_FAULT','OVERLOAD','EVENT_ACTIVE','DOSE_LIMIT'):
            result[key]=1 if values.get(key)=='YES' else 0
        # IOC_USB_ACOUSTIC_FIDELITY: SPECTRUM_VALID do Pico segue como
        # FFT_VALID (chave aceita tambem por simuladores compilados antes).
        # Sem ele, a TUI ocultava frequencia dominante, centroide e planura.
        result['FFT_VALID']=1 if values.get('SPECTRUM_VALID')=='YES' else 0
        result['BASELINE_READY']=1 if values.get('BASELINE')=='READY' else 0
        result['BASELINE_LEARNING']=1 if values.get('BASELINE')=='LEARNING' else 0
        result['EVENT_CLASS']={'IMPACT':1,'CRACK':2,'TONAL':3,'TRANSIENT':4}.get(values.get('EVENT_CLASS',''),0)
        octaves=values.get('OCT_DB','').split(',')
        for index,value in enumerate(octaves[:10]):
            db=numeric(value)
            if db is not None:result[f'OCT_{index}']=db
        keys=AUDIO_FIELDS
    else:
        if numeric(values.get('RMS','')) is None:
            return None
        result={'MODE':VIB_MODES.get(values.get('MODE','').upper(),1),
                'ACQ':ACQ_MODES.get(values.get('ACQ','').upper(),1),
                'FFT_VALID':1 if values.get('FFT_VALID')=='YES' else 0,
                'STA_LTA':1 if values.get('STA_LTA')=='YES' else 0,
                'CLIP':1 if values.get('CLIP')=='YES' else 0}
        if rate_hz is not None and math.isfinite(rate_hz) and 0 < rate_hz <= 100000:
            result['RATE_HZ']=rate_hz
        keys=VIB_FIELDS
    for key in sorted(keys):
        if key in values:
            number=numeric(values[key])
            if number is not None:
                result[key]=number
    return ' '.join(f'{key}={value:g}' if isinstance(value,float) else f'{key}={value}'
                    for key,value in result.items())


# IOC_USB_FFT_ECHO_FIX: o console do Pico ecoa o que a ponte digita. O eco
# "FFT ONCE" tambem comeca com "FFT ", mas nao e resultado. Somente uma
# linha "FFT VALID=YES|NO" e resposta do firmware; o resto e eco ou ACK.
FFT_RESULT_RE = re.compile(r'^FFT\s+VALID=(YES|NO)(?=\s|$)')
FFT_BRIDGE_BUDGET_S = 4.5   # o Node CAN encerra o pedido em 5 s; sobra margem para CAN/Probe
FFT_MAX_ATTEMPTS = 3
FFT_DEFAULT_REPORT_S = 1.0  # o DSP acustico fecha um relatorio por segundo


def fft_result_state(line: str) -> str | None:
    """'YES' ou 'NO' para a resposta do firmware; None para eco, ACK ou ruido."""
    match=FFT_RESULT_RE.match(line)
    return match.group(1) if match else None


# IOC_USB_FFT_STAGE6: espectro completo do Pico, nao dados fabricados.
def parse_fft(line: str) -> tuple[float, int, list[float]] | None:
    if fft_result_state(line)!='YES':
        return None
    payload=fields(line)
    try:
        bins=int(payload['BINS'])
        rate=float(payload['RATE_HZ'])
        fft_size=int(payload['FFT_SIZE'])
        values=[float(part) for part in payload['VALUES'].split(',')]
    except (KeyError,ValueError,OverflowError) as exc:
        raise ValueError('FFT do Pico com campos invalidos') from exc
    if not (1<=bins<=256 and len(values)==bins and 1000<=rate<=100000
            and bins*2==fft_size and all(math.isfinite(x) and x>=0 and x<1e8 for x in values)):
        raise ValueError('FFT do Pico com tamanho, frequencia ou magnitude invalidos')
    return rate,fft_size,values


def deliver_fft(socket_path: str, uuid: str, result: tuple[float,int,list[float]]) -> None:
    rate,fft_size,values=result
    command=(f'SENSOR SPECTRUM {uuid} RATE_HZ={rate:.8g} FFT_SIZE={fft_size} '
             + 'VALUES=' + ','.join(f'{x:.9g}' for x in values))
    request(socket_path,command,timeout=6)


def fft_request_count(socket_path:str,uuid:str)->int:
    response=request(socket_path,f'SENSOR FFT_QUERY {uuid}',timeout=4)
    result=fields(response)
    if 'request' not in result:
        raise RuntimeError('FFT_QUERY sem campo request')
    return int(result['request'])


def get_complete_serial_line(ser, pending: bytearray) -> str:
    """Preserva linhas FFT longas mesmo se USB CDC retornar leituras parciais."""
    chunk=ser.readline()
    if chunk:
        pending.extend(chunk)
    if len(pending)>16384:
        pending.clear()
        raise ValueError('Linha USB excedeu 16 KiB')
    if b'\n' not in pending:
        return ''
    end=pending.index(b'\n')
    raw=bytes(pending[:end+1])
    del pending[:end+1]
    return clean_line(raw)


def authorize_fft(ser, token_file: Path | None, pending: bytearray | None = None) -> bool:
    """Solicita AUTH UNLOCK apenas quando ha credencial provisionada.

    Com ``pending``, le pelo mesmo buffer do streaming: uma linha TEL ou FFT
    parcialmente recebida nao e cortada nem colada a resposta de AUTH.
    O eco "AUTH UNLOCK <token>" e descartado e nunca impresso.
    """
    if token_file is None:
        return False
    secret=read_admin_token(token_file)
    ser.write(f'AUTH UNLOCK {secret}\n'.encode('ascii'))
    ser.flush()
    deadline=time.monotonic()+3
    while time.monotonic()<deadline:
        response=(get_complete_serial_line(ser,pending) if pending is not None
                  else clean_line(ser.readline()))
        if response.startswith('OK AUTH_UNLOCKED'):
            return True
        if response.startswith('ERR AUTH_'):
            raise RuntimeError('Autenticacao USB rejeitada: '+response)
    raise RuntimeError('Sem confirmacao de AUTH_UNLOCKED')


def read_identity(ser, timeout: float = 10.0) -> tuple[str, str, dict[str,str]]:
    """Exige VERSION com UUID64; nao cria sensor com ID inventado."""
    deadline=time.monotonic()+timeout
    found_uuid=None
    found_profile=None
    found_status={}
    last_sent=0.0
    while time.monotonic()<deadline:
        if time.monotonic()-last_sent >= 1.5:
            ser.write(b'VERSION\nSTATUS\n')
            ser.flush()
            last_sent=time.monotonic()
        line=clean_line(ser.readline())
        if line.startswith('VERSION '):
            data=fields(line)
            if data.get('NODE_UUID') and UUID_RE.fullmatch(data['NODE_UUID']):
                found_uuid=f'0x{int(data["NODE_UUID"],16):016X}'
            if data.get('PROFILE'):
                found_profile=data['PROFILE'].upper()
        if line.startswith('STATUS '):
            found_status=fields(line)
            if found_status.get('PROFILE'):
                found_profile=found_status['PROFILE'].upper()
        if found_uuid and found_status:
            profile=found_profile or 'VIBRATION'
            if profile not in {'VIBRATION','ACOUSTIC'}:
                raise RuntimeError(f'Perfil nao suportado: {profile}')
            return found_uuid,profile,found_status
    raise RuntimeError('VERSION/STATUS nao retornaram identidade valida. Confira a porta e o firmware.')


def read_admin_token(path: Path) -> str:
    info=path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid!=os.getuid() or (info.st_mode & 0o077):
        raise ValueError('Arquivo de token deve pertencer ao usuario e ter permissao 0600')
    token=path.read_text(encoding='utf-8').strip()
    if not re.fullmatch(r'(?:0[xX][0-9a-fA-F]+|[0-9]+)',token):
        raise ValueError('Token administrativo deve ser um inteiro decimal ou hexadecimal')
    return token


def setup_sensor(socket_path: str, uuid: str, profile: str, node: int, rssi: int) -> None:
    present=parse_status(request(socket_path,'STATUS',timeout=8))
    if not any(n['id']==node and n['powered'] for n in present['nodes']):
        raise RuntimeError(f'Node {node} ausente ou desligado na simulacao')
    sensor=next((s for s in present['sensors'] if s['uuid'].lower()==uuid.lower()),None)
    if sensor and sensor['profile']!=profile:
        raise RuntimeError('UUID ja existe na bancada com outro perfil')
    if sensor and sensor.get('source')=='USB' and sensor['powered']:
        raise RuntimeError('Sensor ja pertence a uma ponte USB ativa; desligue-o no Studio antes de retomar')
    if not sensor:
        request(socket_path,f'SENSOR ADD {uuid} {profile}',timeout=8)
    elif not sensor['powered']:
        request(socket_path,f'SENSOR POWER {uuid} ON',timeout=8)
    request(socket_path,f'SENSOR RSSI {uuid} {node} {rssi}',timeout=8)
    request(socket_path,f'SENSOR SOURCE {uuid} USB',timeout=8)


def stop_sensor(socket_path:str,uuid:str)->None:
    # Desconectar o dispositivo na USB o torna fisicamente ausente no modelo.
    # Nao desassociar logicamente por fora do protocolo do Node.
    try: request(socket_path,f'SENSOR POWER {uuid} OFF',timeout=3)
    except (OSError,RuntimeError,TimeoutError): pass


def stream(ser, socket_path:str, uuid:str, profile:str, status:dict[str,str],
           token_file:Path|None, passive:bool=False, period_ms:int=1000, fft_enabled:bool=False)->int:
    if passive and status.get('TELEMETRY')!='ON':
        raise RuntimeError('--passive exige TELEMETRY=ON no STATUS da USB')
    # IOC_USB_RATE_FIX: nao conservar um streaming antigo em SLOW (5000 ms).
    # Atualizar tambem a cadencia quando TELEMETRY ja estiver ON.
    reported_period = status.get('PERIOD_MS','')
    current_period = int(reported_period) if reported_period.isdecimal() else None
    configure = not passive and (status.get('TELEMETRY')!='ON' or current_period != period_ms)
    if configure:
        if status.get('AUTH')!='UNLOCKED':
            if token_file is None:
                raise RuntimeError('Sensor AUTH=LOCKED e TELEMETRY=OFF. Forneca --token-file (0600) ou habilite TELEMETRY ON no proprio sensor. Nao e possivel contornar a autenticacao do firmware.')
            token=read_admin_token(token_file)
            ser.write(f'AUTH UNLOCK {token}\n'.encode('ascii'))
            ser.flush()
            deadline=time.monotonic()+3
            unlocked=False
            while time.monotonic()<deadline:
                line=clean_line(ser.readline())
                if 'OK AUTH_UNLOCKED' in line: unlocked=True;break
                if 'ERR AUTH_' in line:raise RuntimeError('Falha na autenticacao USB: '+line)
            if not unlocked:raise RuntimeError('Sensor nao confirmou AUTH_UNLOCKED')
        ser.write(f'TELEMETRY PERIOD {period_ms}\nTELEMETRY ON\n'.encode('ascii'))
        ser.flush()
        print(f'[usb] cadencia USB solicitada: {period_ms} ms ({profile})',flush=True)
    rate=numeric(status.get('RATE_HZ',''))
    delivered=0
    pending=bytearray()
    fft_poll_at=0.0
    # Consolidação: não reproduzir pedidos pendentes de uma execução anterior.
    fft_seen=fft_request_count(socket_path,uuid) if fft_enabled else 0
    fft_waiting=False
    fft_deadline=0.0
    fft_started=0.0
    # "OK FFT_ONCE" confirma que o Pico aceitou o pedido; o espectro so sai
    # no proximo relatorio do DSP (ate ~1 s depois), nunca junto com o ACK.
    fft_acked=False
    # O firmware permite uma autorizacao por 300 s. Renova somente se houver
    # pedido de FFT e um token valido. Nao altera a autenticacao fisica.
    fft_auth_at=time.monotonic() if status.get('AUTH')=='UNLOCKED' or configure else 0.0
    fft_retried_auth=False
    # Autenticar antes do pedido da Probe, sem consumir os 5 s de timeout CAN.
    if fft_enabled and token_file is not None and not configure and status.get('AUTH')!='UNLOCKED':
        authorize_fft(ser,token_file,pending)
        fft_auth_at=time.monotonic()
        print('[usb] autenticacao FFT USB pronta',flush=True)
    # IOC_USB_FFT_NO_SPECTRUM_RETRY: retry bounded; never synthesize a spectrum.
    fft_attempts=0
    last_tel_info={}

    def report_period_s() -> float:
        # INTERVAL_MS do TEL mede a janela do relatorio; sem TEL, 1 s.
        interval=numeric(last_tel_info.get('INTERVAL_MS',''))
        return min(max(interval/1000.0,0.1),5.0) if interval else FFT_DEFAULT_REPORT_S

    def send_fft_once() -> None:
        nonlocal fft_acked
        fft_acked=False
        # Espelha a sequência da TUI direta, já validada com o Pico físico.
        ser.write(b'FFT ONCE\n')
        ser.flush()
        time.sleep(0.04)
        ser.write(b'TELEMETRY ONCE\n')
        ser.flush()

    def fft_context() -> str:
        detail=' '.join(f'{key}={last_tel_info[key]}' for key in
                        ('MODE','CPU_PCT','COVERAGE_PCT','SPECTRUM_VALID','MIC_FAULT','OVERLOAD')
                        if key in last_tel_info)
        return f'{detail or "sem indicadores TEL"}; FFT_ONCE_ACK={"YES" if fft_acked else "NO"}'

    def retry_or_give_up(reason: str) -> None:
        # Nova tentativa so se o proximo relatorio do DSP couber no prazo.
        nonlocal fft_attempts,fft_waiting
        if fft_attempts<FFT_MAX_ATTEMPTS and time.monotonic()+report_period_s()+0.25<fft_deadline:
            fft_attempts+=1
            send_fft_once()
            print(f'[usb] {reason}; nova tentativa {fft_attempts}/{FFT_MAX_ATTEMPTS}',file=sys.stderr,flush=True)
            return
        fft_waiting=False
        print(f'[usb] FFT indisponivel apos {fft_attempts} tentativa(s) ({reason}); '
              f'{fft_context()}. Node recebera timeout real.',file=sys.stderr,flush=True)

    print(f'[usb] captura iniciada uuid={uuid} perfil={profile}; aguarde descoberta na TUI (F7)',flush=True)
    while True:
        now=time.monotonic()
        # O pedido parte da TUI, atravessa a Probe, e so entao chega ao Pico.
        if fft_enabled and now>=fft_poll_at:
            fft_poll_at=now+0.25
            sequence=fft_request_count(socket_path,uuid)
            if sequence>fft_seen:
                fft_seen=sequence
                if fft_waiting:
                    print('[usb] novo FFT enquanto o anterior esta pendente; atendendo o mais recente',flush=True)
                fft_retried_auth=False
                fft_attempts=1
                fft_started=now
                if now-fft_auth_at>255 and token_file is not None:
                    authorize_fft(ser,token_file,pending)
                    fft_auth_at=time.monotonic()
                send_fft_once()
                fft_waiting=True
                fft_deadline=fft_started+FFT_BRIDGE_BUDGET_S
                print('[usb] pedido FFT da Probe encaminhado ao Pico',flush=True)
        if fft_waiting and time.monotonic()>fft_deadline:
            fft_waiting=False
            print(f'[usb] FFT fisica nao chegou em {FFT_BRIDGE_BUDGET_S:g} s; {fft_context()}. '
                  'Node devera rejeitar por timeout',file=sys.stderr,flush=True)
        line=get_complete_serial_line(ser,pending)
        if not line:continue
        if line.startswith('OK FFT_ONCE'):
            fft_acked=True
            continue
        if line.startswith('ERR '):
            print('[usb] firmware: '+line,file=sys.stderr,flush=True)
            if fft_waiting and 'AUTH_REQUIRED' in line and not fft_retried_auth and token_file is not None:
                fft_retried_auth=True
                authorize_fft(ser,token_file,pending)
                fft_auth_at=time.monotonic()
                send_fft_once()  # o prazo do Node conta desde o pedido; nao e estendido
                fft_attempts+=1
                continue
            if fft_waiting and ('AUTH_' in line or 'FFT' in line):
                fft_waiting=False
                continue
            if 'TELEMETRY' in line:
                raise RuntimeError('Firmware recusou streaming: '+line)
        if line.startswith('FFT '):
            state=fft_result_state(line)
            if state is None:
                continue  # eco do comando "FFT ONCE" digitado pela ponte
            if not fft_waiting:
                continue  # nao enviar espectro que nao foi solicitado na rede
            if state=='NO':
                # O Pico respondeu de fato VALID=NO: a janela nao tinha
                # espectro. Pedir o proximo relatorio, sem espectro antigo
                # ou sintetico.
                retry_or_give_up('FFT VALID=NO')
                continue
            try:
                result=parse_fft(line)
            except ValueError as exc:
                # Linha truncada/corrompida na USB nao derruba a ponte.
                retry_or_give_up(f'FFT do Pico rejeitada: {exc}')
                continue
            if result is None:
                continue
            fft_waiting=False
            try:
                deliver_fft(socket_path,uuid,result)
            except (TimeoutError,RuntimeError,OSError) as exc:
                print(f'[usb] simulador recusou o espectro: {exc}',file=sys.stderr,flush=True)
                continue
            print(f'[usb] FFT fisica entregue: {len(result[2])} bins, {result[0]:g} Hz '
                  f'em {time.monotonic()-fft_started:.2f} s',flush=True)
            continue
        if line.startswith('TEL '):
            last_tel_info=fields(line)
        sample=convert_tel(line,profile,rate,status.get('WINDOW'))
        if not sample:continue
        try:
            request(socket_path,f'SENSOR SAMPLE {uuid} {sample}',timeout=6)
        except (TimeoutError,RuntimeError,OSError) as e:
            raise RuntimeError(f'Falha ao entregar amostra ao simulador: {e}') from e
        delivered+=1
        if delivered<=3 or delivered%20==0:
            print(f'[usb] amostras aceitas={delivered}',flush=True)


def main(argv:list[str]|None=None)->int:
    ap=argparse.ArgumentParser(description='Ponte entre sensor Pico W/Pico 2 W USB e Nodes CAN simulados')
    ap.add_argument('--port',required=True,help='Porta USB CDC real (ex.: /dev/ttyACM0)')
    ap.add_argument('--socket',help='Socket CONTROL; omitir se houver uma unica simulacao ativa')
    ap.add_argument('--node',type=int,default=1,help='Node virtual que observa o sensor (padrao: 1)')
    ap.add_argument('--rssi',type=int,default=-55,help='RSSI virtual simulado (padrao: -55 dBm)')
    ap.add_argument('--baud',type=int,default=115200,help='USB CDC (baud nominal)')
    ap.add_argument('--token-file',type=Path,help='Token administrativo em arquivo 0600, nunca na linha de comando')
    ap.add_argument('--passive',action='store_true',help='Nao modificar o streaming existente (ignora --period-ms)')
    ap.add_argument('--period-ms',type=int,default=1000,help='Periodo solicitado da telemetria USB (padrao: 1000 ms)')
    args=ap.parse_args(argv)
    if not 1<=args.node<=31:ap.error('--node deve estar entre 1 e 31')
    if not -127<=args.rssi<=0:ap.error('--rssi deve estar entre -127 e 0')
    try:
        import serial
    except ImportError as e:
        ap.error('pyserial ausente; use Front/.venv/bin/python ou instale as dependencias da TUI')
    socket_path=args.socket or discover_socket()
    uuid=None
    owned=False
    try:
        with serial.Serial(args.port,args.baud,timeout=0.25,write_timeout=2,exclusive=True) as ser:
            uuid,profile,status=read_identity(ser)
            print(f'[usb] dispositivo {uuid}, perfil={profile}, firmware NET={status.get("NET","?")}',flush=True)
            if status.get('NET')=='BOUND':
                raise RuntimeError('Sensor ja associado por wireless fisico: nao use simultaneamente a ponte USB.')
            # Antes de criar entidades no simulador, verifica capacidade de ativar a aquisicao USB.
            # IOC_USB_RATE_FIX: alterar PERIOD exige o mesmo nivel de autorizacao que TELEMETRY ON.
            current_period = status.get('PERIOD_MS','')
            needs_config = not args.passive and (status.get('TELEMETRY')!='ON' or current_period != str(args.period_ms))
            if needs_config and status.get('AUTH')!='UNLOCKED' and args.token_file is None:
                raise RuntimeError('TELEMETRY necessita ativacao ou ajuste de periodo, mas AUTH=LOCKED. Forneca --token-file ou use --passive.')
            setup_sensor(socket_path,uuid,profile,args.node,args.rssi)
            owned=True
            if (profile=='ACOUSTIC' and not 1000<=args.period_ms<=60000) or (profile=='VIBRATION' and not 100<=args.period_ms<=10000):
                raise ValueError('--period-ms fora do intervalo do firmware (ACOUSTIC: 1000..60000; VIBRATION: 100..10000)')
            stream(ser,socket_path,uuid,profile,status,args.token_file,args.passive,args.period_ms,
                   fft_enabled=(profile=='ACOUSTIC'))
    except KeyboardInterrupt:
        print('\n[usb] ponte interrompida pelo operador',flush=True)
    except (OSError,RuntimeError,ValueError,serial.SerialException) as e:
        print('[usb] ERRO: '+str(e),file=sys.stderr)
        return 1
    finally:
        if owned and uuid:stop_sensor(socket_path,uuid)
    return 0


if __name__=='__main__':
    raise SystemExit(main())
