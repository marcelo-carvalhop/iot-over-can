from pico_tui.dtc_catalog import dtc_description, dtc_severity
from pico_tui.core.models import Severity


def test_known_dtc_names_are_specific():
    assert "MPU6050" in dtc_description(0x1001)
    assert "I2C0" in dtc_description(0x1002)
    assert "gravidade" in dtc_description(0x2001)
    assert "clipping" in dtc_description(0x2002).lower()
    assert "DSP" in dtc_description(0x3001)
    assert "alimentação" in dtc_description(0x4001)
    assert "UDP" in dtc_description(0x4002)
    assert "Vínculo rejeitado" in dtc_description(0x4003)


def test_unknown_dtc_uses_category_fallback_with_raw_code():
    description = dtc_description(0x2009)
    assert "sensor" in description
    assert "0x2009" in description


def test_unknown_dtc_severity_is_warning_by_default():
    assert dtc_severity(0x9999) == Severity.WARNING
