from __future__ import annotations
import importlib.util
import fcntl
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / 'iniciar_laboratorio.py'
spec = importlib.util.spec_from_file_location('ioc_lab_launcher', SCRIPT)
lab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lab)


class LauncherTests(unittest.TestCase):
    def test_port_and_control_detection(self):
        self.assertEqual(lab.parse_studio_line('PORT=/dev/pts/12\n'), ('port', '/dev/pts/12'))
        self.assertEqual(lab.parse_studio_line('CONTROL=/tmp/ioc-sim-123/control.sock\n'), ('control', '/tmp/ioc-sim-123/control.sock'))
        self.assertIsNone(lab.parse_studio_line('[studio] Interface: http://127.0.0.1:8765'))
        self.assertIsNone(lab.parse_studio_line('PORT=/tmp/evil'))

    def test_supported_terminals_never_use_shell_eval(self):
        cmd=['python3','my file.py','test']
        for terminal in ('mate-terminal','gnome-terminal','xfce4-terminal','xterm','konsole'):
            output=lab.terminal_command(terminal,'Studio; touch /tmp/evil',cmd)
            self.assertEqual(output[0],terminal)
            self.assertIn('Studio; touch /tmp/evil',' '.join(output))

    def test_dry_run(self):
        p=subprocess.run([sys.executable,str(SCRIPT),'--dry-run'],capture_output=True,text=True)
        self.assertEqual(p.returncode,0,p.stderr)
        self.assertIn('Porta PTY e socket',p.stdout)

    def test_second_launcher_rejects_existing_startup_lock(self):
        with tempfile.TemporaryDirectory(prefix='ioc_lock_') as base:
            directory = pathlib.Path(base) / 'iot-over-can'
            directory.mkdir(parents=True)
            lock_path = directory / (lab.SESSION.name + '.lock')
            env = os.environ.copy()
            env['XDG_STATE_HOME'] = base
            env['DISPLAY'] = ':99'
            with lock_path.open('w') as lock:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                process = subprocess.run(
                    [sys.executable, str(SCRIPT), '--no-usb', '--no-browser'],
                    env=env, capture_output=True, text=True, timeout=10,
                )
            self.assertNotEqual(process.returncode, 0)
            self.assertIn('já está sendo iniciada', process.stderr)

    def test_complete_orchestration_with_fake_studio_and_usb(self):
        with tempfile.TemporaryDirectory(prefix='ioclauncher_') as temp:
            base=pathlib.Path(temp)
            repo=base/'repo'
            script=repo/'Simulacao'/'iniciar_laboratorio.py'
            script.parent.mkdir(parents=True)
            shutil.copy2(SCRIPT,script)
            for folder in ('Simulacao/studio','Front/.venv/bin','Front/pico_tui', 'Codigo/scripts'):
                (repo/folder).mkdir(parents=True,exist_ok=True)
            (repo/'Front/.venv/bin/python').symlink_to(sys.executable)
            (repo/'Front/pico_tui/__main__.py').write_text('''import os,time,sys
open(os.environ['IOC_SMOKE_TUI_ARGS'],'w').write(' '.join(sys.argv))
while True: time.sleep(1)
''')
            (repo/'Simulacao/usb_sensor_bridge.py').write_text('''import os,time,sys
open(os.environ['IOC_SMOKE_USB_ARGS'],'w').write(' '.join(sys.argv))
while True: time.sleep(1)
''')
            (repo/'Codigo/scripts/build_network_sim.sh').write_text('''#!/bin/sh
printf '#!/bin/sh\\nexit 0\\n' > "$1"
chmod +x "$1"
''')
            fake_studio=repo/'Simulacao/studio/server.py'
            fake_studio.write_text('''import os,pty,socket,signal,tempfile,time,sys
m,s=pty.openpty()
folder=tempfile.mkdtemp(prefix='ioc-fake-',dir='/tmp')
sock=socket.socket(socket.AF_UNIX)
address=folder+'/control.sock'
sock.bind(address)
sock.listen(1)
print('PORT='+os.ttyname(s),flush=True)
print('CONTROL='+address,flush=True)
print('[studio] Interface: http://127.0.0.1:8765',flush=True)
try:
    while True: time.sleep(0.1)
except KeyboardInterrupt:
    pass
finally:
    sock.close()
    os.close(s)
    os.close(m)
''')
            fakedir=base/'bin';fakedir.mkdir()
            fake_term=fakedir/'xterm';fake_term.write_text('''#!/bin/sh
while [ "$1" != "-e" ]; do shift; done
shift
exec "$@"
''');fake_term.chmod(0o755)
            token=base/'token';token.write_text('0xABCDEF1234567890');token.chmod(0o600)
            device=base/'usbdev';device.touch()
            binary=base/'simfake'
            env=os.environ.copy()
            env.update({'XDG_STATE_HOME':str(base/'state'), 'DISPLAY':':99',
                        'PATH':str(fakedir)+':'+env['PATH'],
                        'PYTHONPATH':str(repo/'Front'),
                        'IOC_SMOKE_TUI_ARGS':str(base/'tuiargs'),
                        'IOC_SMOKE_USB_ARGS':str(base/'usbargs')})
            def call(*args):
                return subprocess.run([sys.executable,str(script),*args],text=True,env=env,
                                      capture_output=True,timeout=20)
            try:
                start=call('--no-browser','--terminal','xterm','--device',str(device),
                           '--token-file',str(token),'--binary',str(binary))
                self.assertEqual(start.returncode,0,start.stderr+'\n'+start.stdout)
                self.assertIn('TUI conectará em /dev/pts/',start.stdout)
                self.assertIn('Terminais abertos',start.stdout)
                deadline=time.monotonic()+8
                while time.monotonic()<deadline and not ((base/'tuiargs').exists() and (base/'usbargs').exists()):
                    time.sleep(.1)
                self.assertTrue((base/'tuiargs').exists(),'TUI não iniciou')
                self.assertTrue((base/'usbargs').exists(),'ponte não iniciou')
                self.assertIn('--mode probe',(base/'tuiargs').read_text())
                self.assertIn('--socket',(base/'usbargs').read_text())
                self.assertIn(str(token),(base/'usbargs').read_text())
                status=call('status')
                for component in ('studio','tui','usb'):
                    self.assertIn(component+' ',status.stdout)
                    self.assertRegex(status.stdout,rf'{component} +ativo')
                again=call('--no-browser','--terminal','xterm','--device',str(device),
                           '--token-file',str(token),'--binary',str(binary))
                self.assertNotEqual(again.returncode,0)
                self.assertIn('já está ativa',again.stderr)
            finally:
                call('stop')
                time.sleep(.3)
            result=call('status')
            self.assertIn('studio   desligado', result.stdout)
            self.assertIn('tui      desligado', result.stdout)
            self.assertIn('usb      desligado', result.stdout)

if __name__=='__main__':
    unittest.main()
