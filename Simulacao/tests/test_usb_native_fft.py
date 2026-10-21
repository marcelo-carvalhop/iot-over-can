"""Compila a rotina de injecao nativa com estruturas binárias equivalentes.
Nao substitui a compilacao com edge_protocol_definitions.h no checkout real.
"""
from __future__ import annotations
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]

@unittest.skipUnless(shutil.which('c++'), 'Compilador C++ indisponivel')
class UsbNativeTest(unittest.TestCase):
    def test_stale_once_and_payload(self):
        source=r'''
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <cstdio>
#include <cstring>
#include <vector>
#define NET_MAGIC_HEADER 0xABCD
#define CMD_ACOUSTIC_STREAM 0x21
#define CMD_TELEMETRY_STREAM 0x20
#define ACOUSTIC_MODE_LEVEL 1
#define ACOUSTIC_AGE_NONE 65535
#define PROFILE_ACOUSTIC_MEMS 2
#define FSM_MODE_ROTATING_MACH 1
#define ACQ_MODE_POLLING 2
#define AXIS_MASK_VECTOR 7
#define ACOUSTIC_OCTAVE_BANDS 10
#define ACOUSTIC_FLAG_SPECTRUM_VALID 0x01
#define ACOUSTIC_FLAG_ANOMALY 0x20
#define ACOUSTIC_FLAG_MIC_FAULT 0x80
#define ACOUSTIC_FLAG_OVERLOAD 0x02
#define ACOUSTIC_FLAG_EVENT_ACTIVE 0x04
#define ACOUSTIC_FLAG_DOSE_LIMIT 0x40
#define ACOUSTIC_FLAG_BASELINE_READY 0x08
#define ACOUSTIC_FLAG_BASELINE_LEARNING 0x10
struct Payload_AcousticStream {
 uint16_t magic_header=0; uint8_t cmd_type=0; uint16_t seq_num=0;
 uint8_t mode=0,flags=0,window_type=0; uint16_t fft_size=0;
 float sample_rate_req_hz=0,sample_rate_eff_hz=0; uint16_t interval_ms=0;
 uint8_t coverage_pct=0,cpu_pct=0;
 int16_t laeq_cb=0,lceq_cb=0,lzeq_cb=0,lzpeak_cb=0,laf_cb=0,lafmax_cb=0,lafmin_cb=0;
 int16_t crest_x100=0,kurtosis_x100=0; float peak_hz=0;
 uint16_t centroid_hz=0,flatness_x10000=0;
 int16_t leq_total_cb=0,l10_cb=0,l50_cb=0,l90_cb=0;
 uint16_t dose_nr15_x10=0,dose_nho01_x10=0; uint32_t elapsed_s=0;
 int16_t tonal_cb=0; float f0_hz=0; uint8_t harmonics=0;
 int16_t anomaly_x100=0; uint8_t baseline_pct=0;
 uint16_t event_count=0; uint8_t events_interval=0,event_class=0;
 int16_t event_peak_cb=0,event_sel_cb=0; uint16_t event_duration_ms=0,event_centroid_hz=0,event_age_s=0;
 int16_t sta_lta_cb=0,background_cb=0; int16_t octave_cb[10]={};
 uint8_t battery_pct=0; uint16_t battery_mv=0; uint8_t dtc_count=0;
 uint16_t dtc_active_code=0,spectrum_bins=0;
};
struct Payload_TelemetryStream {
 uint16_t magic_header=0; uint8_t cmd_type=0; uint16_t seq_num=0;
 uint8_t fsm_mode=0,acquisition_mode=0,axis_mask=0,fft_valid=0;
 uint16_t window_size=0; float sample_rate_req_hz=0,sample_rate_eff_hz=0;
 float rms_ac=0,kurtosis=0,crest_factor=0,peak_freq_hz=0,peak_amplitude=0,entropy=0,ppv_max_mm_s=0;
 uint8_t seismic_triggered=0,clipping_detected=0,battery_pct=0;
 uint16_t battery_mv=0; uint8_t dtc_count=0; uint16_t dtc_active_code=0,fft_bins_count=0;
};
uint32_t clock_ms=10000;
uint32_t nowMs(){return clock_ms;}
struct Sensor {
  bool usbSource=true; uint32_t usbLastRxMs=9950;
  uint32_t usbSampleVersion=1,usbForwardedVersion=0;
  std::map<std::string,double> usbValues;
  uint32_t usbFftRequest=0;
  std::vector<float> usbSpectrum;
  float usbSpectrumRateHz=0;
  uint16_t usbSpectrumFftSize=0;

  uint8_t profile=1; uint16_t seq=0; unsigned count=0;
  Payload_TelemetryStream last_t{}; Payload_AcousticStream last_a{};
  uint16_t lastSpectrumBins=0;
  void sendData(const void* value,size_t size){
    if (size>=sizeof(last_a)) {
      auto *a=reinterpret_cast<const Payload_AcousticStream*>(value);
      if(a->cmd_type==CMD_ACOUSTIC_STREAM) lastSpectrumBins=a->spectrum_bins;
    }
    ++count;
    if(size==sizeof(last_t))memcpy(&last_t,value,size);
    if(size==sizeof(last_a))memcpy(&last_a,value,size);
  }
#include "sim_usb_telemetry.inc"
};
int main(){
  Sensor v;v.usbValues={{"RMS",0.27},{"MODE",1},{"DTC",4096},{"ACQ",2}};
  v.usbTelemetry();v.usbTelemetry();
  if(v.count!=1||v.last_t.rms_ac<0.26||v.last_t.dtc_active_code!=4096)return 1;
  ++v.usbSampleVersion;
  v.usbLastRxMs=0;
  v.usbTelemetry();if(v.count!=1)return 2;
  Sensor a;a.profile=2;a.usbValues={{"LAEQ_DB",82.5},{"FLATNESS",0.4},{"OCT_1",51.0}};
  a.usbTelemetry();
  if(a.count!=1||a.last_a.laeq_cb!=8250||a.last_a.flatness_x10000!=4000||a.last_a.octave_cb[1]!=5100)return 3;
  if(a.last_a.octave_cb[0]!=-32768||a.last_a.spectrum_bins!=0)return 4;
  a.usbSpectrum={0.1f,0.2f};a.usbSpectrumRateHz=16000;a.usbSpectrumFftSize=4;
  a.usbTelemetry();
  if(a.lastSpectrumBins!=2 || !a.usbSpectrum.empty()) return 5;
  a.usbTelemetry();
  if(a.lastSpectrumBins!=2) return 6;
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)
            (p/'main.cpp').write_text(source,encoding='utf-8')
            result=subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',
                '-I',str(ROOT/'Codigo/node-can/test/host'),str(p/'main.cpp'),'-o',str(p/'run')],
                capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stderr)
            run=subprocess.run([str(p/'run')],capture_output=True,text=True,timeout=5)
            self.assertEqual(run.returncode,0,run.stderr)
