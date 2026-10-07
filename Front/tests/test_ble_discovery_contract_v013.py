from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]



def test_btstack_stdout_dump_has_printf_hexdump_support():
    config = (ROOT / "Codigo" / "node-wifi" / "btstack_config.h").read_text()
    assert "#define ENABLE_PRINTF_HEXDUMP" in config
