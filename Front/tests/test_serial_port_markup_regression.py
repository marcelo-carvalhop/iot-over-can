from pathlib import Path


def test_port_option_list_disables_markup():
    source = (Path(__file__).resolve().parents[1] / "pico_tui" / "screens.py").read_text()
    assert 'OptionList(id="port-list", markup=False)' in source


def test_serial_port_label_keeps_hwid_visible_as_literal_text():
    source = (Path(__file__).resolve().parents[1] / "pico_tui" / "serial_client.py").read_text()
    assert 'hwid = f" [{self.hwid}]" if self.hwid else ""' in source
    assert 'return f"{self.device}{detail}{hwid}"' in source
