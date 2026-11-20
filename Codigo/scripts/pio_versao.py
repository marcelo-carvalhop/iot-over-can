# Script extra do PlatformIO (pre:): define IOT_FW_VERSION e IOT_FW_COMMIT a partir do
# arquivo VERSION e do git. Use em qualquer platformio.ini com:
#   extra_scripts = pre:../scripts/pio_versao.py
# O firmware imprime os dois valores no boot e no comando VERSION (ioc_version.h).
Import("env")  # noqa: F821  (fornecido pelo PlatformIO)
import sys
from pathlib import Path

_scripts = Path(env["PROJECT_DIR"]).resolve().parent / "scripts"  # noqa: F821
sys.path.insert(0, str(_scripts))
import versao  # noqa: E402

for _name, _value in versao.defines():
    env.Append(CPPDEFINES=[(_name, env.StringifyMacro(_value))])  # noqa: F821
print(f"iot-over-can: firmware {versao.read_version()} commit {versao.git_commit()}")
