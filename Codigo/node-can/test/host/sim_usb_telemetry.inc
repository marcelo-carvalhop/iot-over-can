/* IOC_USB_FFT_STAGE6: suporte a FFT fisica. */
/* Telemetria medida por dispositivo USB: executada dentro de Sensor.
 * A fonte USB substitui APENAS a aquisicao sintetica; a associacao BLE/Wi-Fi
 * continua sendo emulada pelo firmware Node CAN do host.
 * As amostras nao sao repetidas: cada TEL fisico e encaminhado no maximo uma vez.
 */
void usbTelemetry() {
  if (!usbSource || (usbSampleVersion==usbForwardedVersion && usbSpectrum.empty())) return;
  if (!usbLastRxMs || nowMs() - usbLastRxMs > 10000u) return;
  const auto val = [this](const char* key, double fallback = 0.) {
    const auto it=usbValues.find(key);
    return it==usbValues.end() ? fallback : it->second;
  };
  const auto u8 = [&val](const char* key, double fallback = 0.) -> uint8_t {
    return static_cast<uint8_t>(std::max(0.,std::min(255.,val(key,fallback))));
  };
  const auto u16 = [&val](const char* key, double fallback = 0.) -> uint16_t {
    return static_cast<uint16_t>(std::max(0.,std::min(65535.,val(key,fallback))));
  };
  const auto centibel = [&val](const char* key) -> int16_t {
    const double value=val(key,-327.68)*100.;
    return static_cast<int16_t>(std::max(-32768.,std::min(32767.,std::round(value))));
  };
  if (profile==PROFILE_ACOUSTIC_MEMS) {
    Payload_AcousticStream a{};
    a.magic_header=NET_MAGIC_HEADER;
    a.cmd_type=CMD_ACOUSTIC_STREAM;
    a.seq_num=seq++;
    a.mode=u8("MODE", ACOUSTIC_MODE_LEVEL);
    a.flags=0; // nenhuma FFT fisica foi transportada na amostra textual
    a.window_type=u8("WINDOW_TYPE");
    a.fft_size=u16("WIN",1024);
    a.sample_rate_req_hz=static_cast<float>(val("RATE_EFF_HZ",0));
    a.sample_rate_eff_hz=a.sample_rate_req_hz;
    a.interval_ms=u16("INTERVAL_MS",1000);
    a.coverage_pct=u8("COVERAGE_PCT",0);
    a.cpu_pct=u8("CPU_PCT",0);
    a.laeq_cb=centibel("LAEQ_DB");
    a.lceq_cb=centibel("LCEQ_DB");
    a.lzeq_cb=centibel("LZEQ_DB");
    a.lzpeak_cb=centibel("LZPEAK_DB");
    a.laf_cb=centibel("LAF_DB");
    a.lafmax_cb=centibel("LAFMAX_DB");
    a.lafmin_cb=centibel("LAFMIN_DB");
    a.crest_x100=centibel("CREST");
    a.kurtosis_x100=centibel("KURT");
    a.peak_hz=static_cast<float>(val("PEAK_HZ"));
    a.centroid_hz=u16("CENTROID_HZ");
    a.flatness_x10000=static_cast<uint16_t>(std::max(0.,std::min(10000.,val("FLATNESS")*10000.)));
    a.leq_total_cb=centibel("LEQ_T_DB");
    a.l10_cb=centibel("L10_DB");
    a.l50_cb=centibel("L50_DB");
    a.l90_cb=centibel("L90_DB");
    a.dose_nr15_x10=static_cast<uint16_t>(std::max(0.,std::min(65535.,val("DOSE_NR15_PCT")*10.)));
    a.dose_nho01_x10=static_cast<uint16_t>(std::max(0.,std::min(65535.,val("DOSE_NHO01_PCT")*10.)));
    a.elapsed_s=static_cast<uint32_t>(std::max(0.,val("ELAPSED_S")));
    a.tonal_cb=centibel("TONAL_DB");
    a.f0_hz=static_cast<float>(val("F0_HZ"));
    a.harmonics=u8("HARMONICS");
    a.anomaly_x100=centibel("ANOMALY_Z");
    a.baseline_pct=u8("BASELINE_PCT");
    a.event_count=u16("EVENTS");
    a.events_interval=u8("EVENTS_INTERVAL");
    a.event_class=u8("EVENT_CLASS");
    a.event_peak_cb=centibel("EVENT_PEAK_DB");
    a.event_sel_cb=centibel("EVENT_SEL_DB");
    a.event_duration_ms=u16("EVENT_DURATION_MS");
    a.event_centroid_hz=u16("EVENT_CENTROID_HZ");
    a.event_age_s=u16("EVENT_AGE_S", ACOUSTIC_AGE_NONE);
    a.sta_lta_cb=centibel("STA_LTA_DB");
    a.background_cb=centibel("BACKGROUND_DB");
    if (u8("ANOMALY")) a.flags|=ACOUSTIC_FLAG_ANOMALY;
    if (u8("MIC_FAULT")) a.flags|=ACOUSTIC_FLAG_MIC_FAULT;
    if (u8("OVERLOAD")) a.flags|=ACOUSTIC_FLAG_OVERLOAD;
    if (u8("EVENT_ACTIVE")) a.flags|=ACOUSTIC_FLAG_EVENT_ACTIVE;
    if (u8("DOSE_LIMIT")) a.flags|=ACOUSTIC_FLAG_DOSE_LIMIT;
    if (u8("BASELINE_READY")) a.flags|=ACOUSTIC_FLAG_BASELINE_READY;
    if (u8("BASELINE_LEARNING")) a.flags|=ACOUSTIC_FLAG_BASELINE_LEARNING;
    for (unsigned b=0;b<ACOUSTIC_OCTAVE_BANDS;++b) {
      const std::string name="OCT_"+std::to_string(b);
      const auto it=usbValues.find(name);
      const double db=it==usbValues.end()?-327.68:it->second;
      a.octave_cb[b]=static_cast<int16_t>(std::max(-32768.,std::min(32767.,std::round(db*100.))));
    }
    a.battery_pct=u8("BATT_PCT",255);
    a.battery_mv=u16("BATT_MV",65535);
    a.dtc_count=u8("DTC_COUNT");
    a.dtc_active_code=u16("DTC");
    a.spectrum_bins=static_cast<uint16_t>(usbSpectrum.size());
    if (a.spectrum_bins>0) {
      a.flags|=ACOUSTIC_FLAG_SPECTRUM_VALID;
      a.sample_rate_req_hz=usbSpectrumRateHz;
      a.sample_rate_eff_hz=usbSpectrumRateHz;
      a.fft_size=usbSpectrumFftSize;
      std::vector<uint8_t> payload(sizeof(a)+usbSpectrum.size()*sizeof(float));
      memcpy(payload.data(),&a,sizeof(a));
      memcpy(payload.data()+sizeof(a),usbSpectrum.data(),usbSpectrum.size()*sizeof(float));
      sendData(payload.data(),payload.size());
      usbSpectrum.clear();
    } else {
      sendData(&a,sizeof(a));
    }
  } else {
    Payload_TelemetryStream t{};
    t.magic_header=NET_MAGIC_HEADER;
    t.cmd_type=CMD_TELEMETRY_STREAM;
    t.seq_num=seq++;
    t.fsm_mode=u8("MODE",FSM_MODE_ROTATING_MACH);
    t.acquisition_mode=u8("ACQ",ACQ_MODE_POLLING);
    t.axis_mask=AXIS_MASK_VECTOR;
    t.fft_valid=u8("FFT_VALID");
    t.window_size=u16("WIN",512);
    t.sample_rate_req_hz=static_cast<float>(val("RATE_HZ",0));
    t.sample_rate_eff_hz=t.sample_rate_req_hz;
    t.rms_ac=static_cast<float>(val("RMS"));
    t.kurtosis=static_cast<float>(val("KURT"));
    t.crest_factor=static_cast<float>(val("CREST"));
    t.peak_freq_hz=static_cast<float>(val("PEAK_HZ"));
    t.peak_amplitude=static_cast<float>(val("PEAK_AMP"));
    t.entropy=static_cast<float>(val("ENT"));
    t.ppv_max_mm_s=static_cast<float>(val("PPV_MM_S"));
    t.seismic_triggered=u8("STA_LTA");
    t.clipping_detected=u8("CLIP");
    t.dtc_count=u8("DTC_COUNT");
    t.dtc_active_code=u16("DTC");
    t.fft_bins_count=0; // um TEL textual nao traz as faixas FFT
    t.battery_pct=u8("BATT_PCT",255);
    t.battery_mv=u16("BATT_MV",65535);
    sendData(&t,sizeof(t));
  }
  usbForwardedVersion=usbSampleVersion;
}
