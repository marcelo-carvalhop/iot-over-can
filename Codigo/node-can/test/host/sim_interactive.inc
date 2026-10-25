/*
 * Modo interativo da bancada virtual. Incluido em sim_network.cpp, antes do
 * fechamento do namespace anonimo. Nao altera a execucao dos 17 cenarios.
 * Contrato: linhas #CTRL sao controle; linhas @<ms> sao saida da Probe 00.
 */

// Painel em papel eletronico (perfil 0x04): implementado em sim_network.cpp,
// depois deste arquivo. Os testes do controle compilam versoes vazias.
const char* simProfileName(const Sensor& s);
void simPanelAdded(Sensor& s);
bool simPanelSource(Sensor& s, bool usb);
bool simPanelPower(Sensor& s, bool on);
void simPanelControl(std::istringstream& in);

// Fabrica gerada pelo script de build: os Node IDs disponiveis sao definidos
// na compilacao, enquanto os Nodes ativos podem ser adicionados em runtime.
#include "sim_factory_registry.inc"

std::map<uint8_t, std::vector<std::function<::SimInstance*()>>> g_node_pool;
std::map<uint8_t, size_t> g_node_consumed;

void simPrepareFactories() {
  g_node_pool = simGeneratedFactories();
  for (Node& n : g_nodes) {
    if (n.id == 0) continue;
    auto it = g_node_pool.find(n.id);
    if (it == g_node_pool.end()) continue;
    n.generations = it->second;
    g_node_consumed[n.id] = n.nextGeneration;
  }
}

Node* simFindNode(uint8_t id) {
  for (Node& n : g_nodes) if (n.id == id) return &n;
  return nullptr;
}

std::string simUpper(std::string s) {
  for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
  return s;
}

void simControlError(const std::string& why) {
  printf("#CTRL ERROR %s\n", why.c_str());
}

unsigned long long simParseUnsigned(const std::string& token, unsigned long long max) {
  if (token.empty() || token[0] == '-') throw std::runtime_error("inteiro invalido");
  size_t end = 0;
  unsigned long long value = std::stoull(token, &end, 0);
  if (end != token.size() || value > max) throw std::runtime_error("inteiro fora do intervalo");
  return value;
}

int simParseRssi(const std::string& token) {
  size_t end = 0;
  int rssi = std::stoi(token, &end, 10);
  if (end != token.size() || rssi < -127 || rssi > 0)
    throw std::runtime_error("RSSI deve estar entre -127 e 0 dBm");
  return rssi;
}

Sensor* simFindSensor(uint64_t uuid) {
  for (Sensor& s : g_sensors) if (s.uuid == uuid) return &s;
  return nullptr;
}

void simPrintStatus(bool running) {
  printf("#CTRL OK STATUS t_ms=%u running=%u leader=%u nodes=%zu sensors=%zu frames=%u bits=%llu\n",
         nowMs(), running ? 1 : 0, currentLeader(), g_nodes.size(), g_sensors.size(), g_bus_frames,
         static_cast<unsigned long long>(g_bus_bits));
  for (const Node& n : g_nodes) {
    printf("#CTRL NODE id=%u powered=%u state=%d tx_disconnected=%u restarts=%u boots_remaining=%zu budget_pct=%u\n",
           n.id, n.powered ? 1 : 0, n.inst ? n.inst->nodeState() : -1,
           n.txDisconnected ? 1 : 0, n.restarts,
           n.generations.size() - n.nextGeneration, n.cpuBudgetPct);
  }
  for (const Sensor& s : g_sensors) {
    // IOC_USB_HYBRID_STAGE5
    printf("#CTRL SENSOR uuid=0x%016llX profile=%s powered=%u parent=%u child=%u mode=%u links=%zu source=%s usb_age_ms=%u\n",
           static_cast<unsigned long long>(s.uuid),
           simProfileName(s),
           s.poweredOn ? 1 : 0, s.parent, s.child,
           static_cast<unsigned>(s.mode), s.rssi.size(), s.usbSource ? "USB" : "SYNTH",
           s.usbLastRxMs ? nowMs()-s.usbLastRxMs : 0u);
    for (const auto& link : s.rssi) {
      auto loss = g_radio_loss_pct.find({s.uuid,link.first});
      printf("#CTRL LINK uuid=0x%016llX node=%u rssi=%d loss_pct=%u\n",
             static_cast<unsigned long long>(s.uuid), link.first, link.second,
             loss==g_radio_loss_pct.end()?0u:static_cast<unsigned>(loss->second));
    }
  }
  printf("#CTRL END\n");
}

void simPrintHelp() {
  puts("#CTRL OK HELP");
  puts("#CTRL HELP STATUS | PAUSE | RESUME | STEP <ms> | QUIT");
  puts("#CTRL HELP SEED <uint32> (apenas pausado; altera eventos futuros)");
  puts("#CTRL HELP MAX_NODES = limite de compilacao; ADD/REMOVE em runtime");
  puts("#CTRL HELP NODE <id> ADD|REMOVE");
  puts("#CTRL HELP NODE <id> POWER ON|OFF|RESTART");
  puts("#CTRL HELP NODE <id> TX ON|OFF");
  puts("#CTRL HELP NODE <id> BUDGET <1..100>  (percentual de passos do loop)");
  puts("#CTRL HELP SENSOR ADD <uuid> VIBRATION|ACOUSTIC|PANEL");
  puts("#CTRL HELP PANEL PAGE <uuid>  (pagina no papel ou espelho do painel USB)");
  puts("#CTRL HELP PANEL KEY <uuid> <codigo> | PANEL BUTTON <uuid> A|B|C|P|PP | PANEL NOTE <uuid> <texto>");
  puts("#CTRL HELP PANEL BATTERY <uuid> ON|OFF  (painel virtual dorme entre verificacoes)");
  puts("#CTRL HELP PANEL POLL|JOIN|LEAVE|UP <uuid> ...  (ponte USB do PaperColor)");
  puts("#CTRL HELP SENSOR REMOVE <uuid>");
  puts("#CTRL HELP SENSOR SOURCE <uuid> USB|SYNTH");
  puts("#CTRL HELP SENSOR SAMPLE <uuid> KEY=NUMERIC ...  (somente fonte USB)");
  puts("#CTRL HELP SENSOR FFT_QUERY <uuid>  (ponte USB)");
  puts("#CTRL HELP SENSOR SPECTRUM <uuid> RATE_HZ=n FFT_SIZE=n VALUES=f,...  (ponte USB)");
  puts("#CTRL HELP SENSOR POWER <uuid> ON|OFF");
  puts("#CTRL HELP SENSOR RSSI <uuid> <node_id> <-127..0|OFF>");
  puts("#CTRL HELP SENSOR LINK <uuid> <node_id> LOSS <0..100> (perda de pacotes %)");
  puts("#CTRL HELP SENSOR FAULT <uuid> MACHINE ON|OFF  (perfil acustico)");
  puts("#CTRL HELP PROBE <comando textual nativo da Probe 00>");
  puts("#CTRL END");
}

bool simIsNodeId(unsigned long long id) { return id > 0 && g_node_pool.count(static_cast<uint8_t>(id)) != 0; }

void simControl(const std::string& line, bool& running, bool& quit,
                std::chrono::steady_clock::time_point& wallAnchor, uint64_t& simAnchor) {
  std::istringstream in(line);
  std::string action;
  if (!(in >> action)) return;
  action = simUpper(action);
  try {
    if (action == "HELP") {
      simPrintHelp();
      return;
    }
    if (action == "STATUS") {
      simPrintStatus(running);
      return;
    }
    if (action == "QUIT" || action == "EXIT") {
      puts("#CTRL OK QUIT");
      quit = true;
      return;
    }
    if (action == "SEED") {
      std::string token, extra;
      if (!(in >> token) || (in >> extra)) throw std::runtime_error("uso: SEED <0..4294967295>");
      if (running) throw std::runtime_error("PAUSE antes de SEED");
      g_rng = static_cast<uint32_t>(simParseUnsigned(token, UINT32_MAX));
      printf("#CTRL OK SEED value=%u\n", g_rng);
      return;
    }
    if (action == "PAUSE") {
      running = false;
      printf("#CTRL OK PAUSE t_ms=%u\n", nowMs());
      return;
    }
    if (action == "RESUME") {
      simAnchor = g_now_us;
      wallAnchor = std::chrono::steady_clock::now();
      running = true;
      printf("#CTRL OK RESUME t_ms=%u\n", nowMs());
      return;
    }
    if (action == "STEP") {
      std::string msText, extra;
      if (!(in >> msText) || (in >> extra)) throw std::runtime_error("uso: STEP <ms>");
      if (running) throw std::runtime_error("PAUSE antes de STEP");
      const unsigned long long ms = simParseUnsigned(msText, 120000);
      const uint64_t untilUs = g_now_us + ms * 1000u;
      while (g_now_us < untilUs) tick();
      printf("#CTRL OK STEP t_ms=%u\n", nowMs());
      return;
    }
    if (action == "PROBE") {
      const size_t separator = line.find_first_of(" \t");
      const size_t start = separator == std::string::npos ? std::string::npos : line.find_first_not_of(" \t", separator);
      if (start == std::string::npos) throw std::runtime_error("uso: PROBE <comando>");
      if (!probe().powered || !probe().inst) throw std::runtime_error("Probe indisponivel");
      probeCommand(line.substr(start));
      puts("#CTRL OK PROBE");
      return;
    }
    if (action == "NODE") {
      std::string idToken, kind, state, extra;
      if (!(in >> idToken >> kind)) throw std::runtime_error("uso: NODE <id> ADD|REMOVE|POWER|TX");
      const auto id = simParseUnsigned(idToken, 255);
      if (!simIsNodeId(id)) throw std::runtime_error("ID sem instancia compilada; ajuste IOC_SIM_MAX_NODES");
      const uint8_t key = static_cast<uint8_t>(id);
      kind = simUpper(kind);
      Node* existing = simFindNode(key);
      if (kind == "ADD") {
        if (in >> extra) throw std::runtime_error("uso: NODE <id> ADD");
        if (existing) throw std::runtime_error("Node ja presente");
        const auto& factories = g_node_pool.at(key);
        const size_t consumed = g_node_consumed[key];
        if (consumed >= factories.size()) throw std::runtime_error("geracoes esgotadas para esse ID");
        Node created;
        created.id = key;
        created.generations = factories;
        created.nextGeneration = consumed;
        g_nodes.push_back(std::move(created));
        powerOn(g_nodes.back());
        g_node_consumed[key] = g_nodes.back().nextGeneration;
        printf("#CTRL OK NODE id=%u kind=ADD\n", key);
        return;
      }
      if (kind == "REMOVE") {
        if (in >> extra) throw std::runtime_error("uso: NODE <id> REMOVE");
        if (!existing) throw std::runtime_error("Node ausente");
        g_node_consumed[key] = existing->nextGeneration;
        if (existing->powered) powerOff(*existing, false);
        g_nodes.erase(std::remove_if(g_nodes.begin(), g_nodes.end(),
                      [key](const Node& n) { return n.id == key; }), g_nodes.end());
        // As observacoes de alcance persistem no ambiente. O firmware de um
        // sensor perdera a conectividade ate um Node desse ID reaparecer.
        printf("#CTRL OK NODE id=%u kind=REMOVE\n", key);
        return;
      }
      if (!(in >> state) || (in >> extra))
        throw std::runtime_error("uso: NODE <id> POWER ON|OFF|RESTART, TX ON|OFF ou BUDGET <1..100>");
      if (!existing) throw std::runtime_error("Node ausente; use NODE <id> ADD");
      Node& n = *existing;
      state = simUpper(state);
      if (kind == "BUDGET") {
        const auto pct=simParseUnsigned(state,100);
        if (pct==0) throw std::runtime_error("BUDGET deve estar entre 1 e 100");
        n.cpuBudgetPct=static_cast<uint8_t>(pct);
        printf("#CTRL OK NODE id=%u kind=BUDGET pct=%u\n",n.id,n.cpuBudgetPct);
        return;
      }
      if (kind == "TX") {
        if (state != "ON" && state != "OFF") throw std::runtime_error("TX deve ser ON ou OFF");
        n.txDisconnected = state == "OFF";
      } else if (kind == "POWER") {
        if (state == "OFF") {
          if (!n.powered) throw std::runtime_error("Node ja esta desligado");
          powerOff(n, false);
        } else if (state == "ON") {
          if (n.powered) throw std::runtime_error("Node ja esta ligado");
          if (n.nextGeneration >= n.generations.size())
            throw std::runtime_error("limite de boots atingido para esse ID");
          powerOn(n);
          g_node_consumed[key] = n.nextGeneration;
        } else if (state == "RESTART") {
          if (!n.powered) throw std::runtime_error("Node esta desligado");
          if (n.nextGeneration >= n.generations.size())
            throw std::runtime_error("limite de boots atingido para esse ID");
          powerOff(n, true);
        } else throw std::runtime_error("POWER deve ser ON, OFF ou RESTART");
      } else throw std::runtime_error("NODE aceita ADD, REMOVE, POWER ou TX");
      printf("#CTRL OK NODE id=%u kind=%s state=%s\n", n.id, kind.c_str(), state.c_str());
      return;
    }
    if (action == "PANEL") {
      simPanelControl(in);
      return;
    }
    if (action == "SENSOR") {
      std::string op, uuidToken;
      if (!(in >> op >> uuidToken)) throw std::runtime_error("uso: SENSOR <acao> <uuid> ...");
      op = simUpper(op);
      uint64_t uuid = static_cast<uint64_t>(simParseUnsigned(uuidToken, UINT64_MAX));
      Sensor* s = simFindSensor(uuid);
      if (op == "ADD") {
        std::string profile, extra;
        if (!(in >> profile) || (in >> extra)) throw std::runtime_error("uso: SENSOR ADD <uuid> VIBRATION|ACOUSTIC|PANEL");
        profile = simUpper(profile);
        if (s) throw std::runtime_error("UUID ja existente");
        if (profile != "VIBRATION" && profile != "ACOUSTIC" && profile != "PANEL")
          throw std::runtime_error("perfil desconhecido");
        if (uuid == 0 || g_sensors.size() >= 200)
          throw std::runtime_error("UUID zero ou capacidade de sensores excedida");
        // Porta/IP disponiveis: nao reutilizar os de um sensor ainda conectado
        // apos exclusoes e inclusoes na mesma sessao.
        uint16_t port = 4242;
        for (; port < 4492; ++port) {
          bool busy = false;
          for (const Sensor& other : g_sensors) if (other.port == port) busy = true;
          if (!busy) break;
        }
        if (port >= 4492) throw std::runtime_error("sem endereco IP/porta virtual disponivel");
        const uint8_t last = static_cast<uint8_t>(2 + port - 4242);
        Sensor& added = addSensor(uuid, port, last);
        if (profile == "ACOUSTIC") {
          added.profile = PROFILE_ACOUSTIC_MEMS;
          added.telemetryPeriodMs = 1000;
        }
        if (profile == "PANEL") simPanelAdded(added);
        printf("#CTRL OK SENSOR ADD uuid=0x%016llX profile=%s\n",
               static_cast<unsigned long long>(uuid), profile.c_str());
        return;
      }
      if (!s) throw std::runtime_error("sensor inexistente");
/* IOC_USB_HYBRID_STAGE5 */
      // IOC_USB_FFT_STAGE6: poll seguro para ponte USB (nao consome pedido).
      if (op == "FFT_QUERY") {
        std::string extra;
        if (in>>extra) throw std::runtime_error("uso: SENSOR FFT_QUERY <uuid>");
        if (!s || !s->usbSource) throw std::runtime_error("FFT_QUERY exige sensor USB");
        printf("#CTRL OK SENSOR FFT_QUERY uuid=0x%016llX request=%u\n",
          static_cast<unsigned long long>(uuid),s->usbFftRequest);
        return;
      }
      if (op == "SPECTRUM") {
        if (!s || !s->usbSource || !s->poweredOn || s->profile!=PROFILE_ACOUSTIC_MEMS)
          throw std::runtime_error("SPECTRUM exige sensor acustico USB ativo");
        std::string rateText,sizeText,valuesText,extra;
        if (!(in>>rateText>>sizeText>>valuesText) || (in>>extra))
          throw std::runtime_error("uso: SENSOR SPECTRUM <uuid> RATE_HZ=n FFT_SIZE=n VALUES=f,...");
        auto extract=[](const std::string& text,const char* prefix) -> std::string {
          const std::string head(prefix);
          if (text.compare(0,head.size(),head)!=0 || text.size()==head.size())
            throw std::runtime_error("SPECTRUM parametro invalido");
          return text.substr(head.size());
        };
        const std::string rateString=extract(rateText,"RATE_HZ=");
        size_t end=0;
        double rate=std::stod(rateString,&end);
        if(end!=rateString.size() || !std::isfinite(rate) || rate<1000 || rate>100000)
          throw std::runtime_error("RATE_HZ invalido");
        const auto size=simParseUnsigned(extract(sizeText,"FFT_SIZE="),512);
        const std::string valueString=extract(valuesText,"VALUES=");
        std::istringstream csv(valueString);
        std::vector<float> spectrum;
        std::string item;
        while (std::getline(csv,item,',')) {
          if (item.empty() || spectrum.size()==256) throw std::runtime_error("espectro malformado");
          size_t stop=0;
          float value=std::stof(item,&stop);
          if (stop!=item.size() || !std::isfinite(value) || value<0 || value>1e8f)
            throw std::runtime_error("magnitude de espectro invalida");
          spectrum.push_back(value);
        }
        if (spectrum.empty() || spectrum.size()*2!=size)
          throw std::runtime_error("FFT_SIZE deve ser o dobro de BINS");
        if (s->usbFftRequest==s->usbFftInjected)
          throw std::runtime_error("espectro nao solicitado pela Probe");
        s->usbFftInjected=s->usbFftRequest;
        s->usbSpectrum=std::move(spectrum);
        s->usbSpectrumFftSize=static_cast<uint16_t>(size);
        s->usbSpectrumRateHz=static_cast<float>(rate);
        ++s->usbSampleVersion;
        printf("#CTRL OK SENSOR SPECTRUM uuid=0x%016llX bins=%zu\n",
          static_cast<unsigned long long>(uuid),s->usbSpectrum.size());
        return;
      }
      if (op == "SOURCE") {
        std::string value,extra;
        if (!(in>>value) || (in>>extra)) throw std::runtime_error("uso: SENSOR SOURCE <uuid> USB|SYNTH");
        if (!s) throw std::runtime_error("sensor inexistente");
        value=simUpper(value);
        if (value!="USB" && value!="SYNTH") throw std::runtime_error("SOURCE deve ser USB ou SYNTH");
        if (simPanelSource(*s, value=="USB")) {
          printf("#CTRL OK SENSOR SOURCE uuid=0x%016llX source=%s\n",static_cast<unsigned long long>(uuid),value.c_str());
          return;
        }
        s->usbSource=value=="USB";
        s->usbValues.clear();
        s->usbSampleVersion=s->usbForwardedVersion=0;
        s->usbLastRxMs=0;
        printf("#CTRL OK SENSOR SOURCE uuid=0x%016llX source=%s\n",static_cast<unsigned long long>(uuid),value.c_str());
        return;
      }
      if (op == "SAMPLE") {
        if (!s || !s->usbSource || !s->poweredOn) throw std::runtime_error("fonte USB nao configurada ou sensor desligado");
        static const std::vector<std::string> allowed = {
          "MODE","ACQ","WIN","WINDOW_TYPE","FFT_VALID","STA_LTA","CLIP","RATE_HZ","RMS","KURT","CREST", "PEAK_HZ","PEAK_AMP","ENT",
          "PPV_MM_S","BATT_PCT","BATT_MV","DTC","DTC_COUNT","RATE_EFF_HZ","INTERVAL_MS",
          "CPU_PCT","COVERAGE_PCT","LAEQ_DB","LCEQ_DB","LZEQ_DB","LZPEAK_DB","LAF_DB",
          "LAFMAX_DB","LAFMIN_DB","CENTROID_HZ","FLATNESS","LEQ_T_DB","L10_DB","L50_DB","L90_DB",
          "DOSE_NR15_PCT","DOSE_NHO01_PCT","ELAPSED_S","TONAL_DB","F0_HZ","HARMONICS",
          "ANOMALY_Z","BASELINE_PCT","EVENTS","EVENTS_INTERVAL","EVENT_CLASS",
          "EVENT_PEAK_DB","EVENT_SEL_DB","EVENT_DURATION_MS","EVENT_CENTROID_HZ","EVENT_AGE_S",
          "STA_LTA_DB","BACKGROUND_DB","ANOMALY","MIC_FAULT","OVERLOAD","EVENT_ACTIVE",
          "DOSE_LIMIT","BASELINE_READY","BASELINE_LEARNING",
          "OCT_0","OCT_1","OCT_2","OCT_3","OCT_4","OCT_5","OCT_6","OCT_7","OCT_8","OCT_9"
        };
        std::map<std::string,double> values;
        std::string item;
        while (in>>item) {
          const auto idx=item.find('=');
          if (idx==std::string::npos || idx==0 || idx==item.size()-1)
            throw std::runtime_error("SAMPLE requer campos KEY=NUMERO");
          std::string key=simUpper(item.substr(0,idx));
          if (std::find(allowed.begin(),allowed.end(),key)==allowed.end())
            throw std::runtime_error("campo USB desconhecido: "+key);
          const std::string number=item.substr(idx+1);
          size_t end=0;
          const double value=std::stod(number,&end);
          if (end!=number.size() || !std::isfinite(value) || std::fabs(value)>1e8)
            throw std::runtime_error("valor de SAMPLE invalido");
          values[key]=value;
          if (values.size()>64) throw std::runtime_error("excesso de campos SAMPLE");
        }
        if (values.empty()) throw std::runtime_error("SAMPLE exige campos");
        if (s->profile==PROFILE_ACOUSTIC_MEMS) {
          if (!values.count("LAEQ_DB")) throw std::runtime_error("TEL acustico exige LAEQ_DB");
        } else if (!values.count("RMS")) {
          throw std::runtime_error("TEL de vibracao exige RMS");
        }
        s->usbValues=std::move(values);
        s->usbLastRxMs=nowMs();
        ++s->usbSampleVersion;
        printf("#CTRL OK SENSOR SAMPLE uuid=0x%016llX sample=%u\n",
          static_cast<unsigned long long>(uuid),s->usbSampleVersion);
        return;
      }

      if (op == "REMOVE") {
        std::string extra;
        if (in >> extra) throw std::runtime_error("uso: SENSOR REMOVE <uuid>");
        g_sensors.erase(std::remove_if(g_sensors.begin(), g_sensors.end(),
                       [uuid](const Sensor& item) { return item.uuid == uuid; }), g_sensors.end());
        for (auto it=g_radio_loss_pct.begin();it!=g_radio_loss_pct.end();) {
          if (it->first.first==uuid) it=g_radio_loss_pct.erase(it);
          else ++it;
        }
        printf("#CTRL OK SENSOR REMOVE uuid=0x%016llX\n", static_cast<unsigned long long>(uuid));
        return;
      }
      if (op == "POWER") {
        std::string state, extra;
        if (!(in >> state) || (in >> extra)) throw std::runtime_error("uso: SENSOR POWER <uuid> ON|OFF");
        state = simUpper(state);
        if ((state == "ON" || state == "OFF") && simPanelPower(*s, state == "ON")) {
          printf("#CTRL OK SENSOR POWER uuid=0x%016llX state=%s\n", static_cast<unsigned long long>(uuid), state.c_str());
          return;
        }
        if (state == "OFF") s->poweredOn = false;
        else if (state == "ON") {
          if (!s->poweredOn) {
            Sensor fresh;
            fresh.uuid = s->uuid;
            fresh.profile = s->profile;
            fresh.port = s->port;
            memcpy(fresh.ip, s->ip, sizeof(fresh.ip));
            memcpy(fresh.devKey, s->devKey, sizeof(fresh.devKey));
            fresh.rssi = s->rssi;
            fresh.telemetryPeriodMs = s->telemetryPeriodMs;
            fresh.usbSource = s->usbSource;
            *s = std::move(fresh);
          }
        } else throw std::runtime_error("POWER deve ser ON ou OFF");
        printf("#CTRL OK SENSOR POWER uuid=0x%016llX state=%s\n",
               static_cast<unsigned long long>(uuid), state.c_str());
        return;
      }
      if (op == "RSSI") {
        std::string idToken, value, extra;
        if (!(in >> idToken >> value) || (in >> extra))
          throw std::runtime_error("uso: SENSOR RSSI <uuid> <node> <-127..0|OFF>");
        const auto id = simParseUnsigned(idToken, 255);
        if (!simIsNodeId(id) || !simFindNode(static_cast<uint8_t>(id)))
          throw std::runtime_error("Node nao existe na bancada");
        if (simUpper(value) == "OFF") s->rssi.erase(static_cast<uint8_t>(id));
        else s->rssi[static_cast<uint8_t>(id)] = simParseRssi(value);
        printf("#CTRL OK SENSOR RSSI uuid=0x%016llX node=%llu value=%s\n",
               static_cast<unsigned long long>(uuid), id, value.c_str());
        return;
      }
      if (op == "LINK") {
        std::string idToken, metric, value, extra;
        if (!(in >> idToken >> metric >> value) || (in >> extra))
          throw std::runtime_error("uso: SENSOR LINK <uuid> <node> LOSS <0..100>");
        const auto id=simParseUnsigned(idToken,255);
        if (!simIsNodeId(id) || !simFindNode(static_cast<uint8_t>(id)))
          throw std::runtime_error("Node inexistente");
        metric=simUpper(metric);
        if (metric!="LOSS") throw std::runtime_error("Somente LOSS esta implementado");
        const auto pct=simParseUnsigned(value,100);
        g_radio_loss_pct[{uuid,static_cast<uint8_t>(id)}]=static_cast<uint8_t>(pct);
        printf("#CTRL OK SENSOR LINK uuid=0x%016llX node=%llu loss_pct=%llu\n",
               static_cast<unsigned long long>(uuid),id,pct);
        return;
      }
      if (op == "FAULT") {
        std::string kind, state, extra;
        if (!(in >> kind >> state) || (in >> extra))
          throw std::runtime_error("uso: SENSOR FAULT <uuid> MACHINE ON|OFF");
        kind = simUpper(kind);
        state = simUpper(state);
        if (kind != "MACHINE" || s->profile != PROFILE_ACOUSTIC_MEMS ||
            (state != "ON" && state != "OFF"))
          throw std::runtime_error("falha disponivel somente para perfil ACOUSTIC: MACHINE ON|OFF");
        s->machineFault = state == "ON";
        printf("#CTRL OK SENSOR FAULT uuid=0x%016llX kind=MACHINE state=%s\n",
               static_cast<unsigned long long>(uuid), state.c_str());
        return;
      }
      throw std::runtime_error("acao SENSOR desconhecida");
    }
    throw std::runtime_error("comando desconhecido; use HELP");
  } catch (const std::exception& e) {
    simControlError(e.what());
  }
}

int simInteractiveLoop(bool startPaused = false) {
  using Clock = std::chrono::steady_clock;
  bool running = !startPaused;
  bool quit = false;
  auto wallAnchor = Clock::now();
  uint64_t simAnchor = g_now_us;
  std::string buffer;
  const int oldFlags = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (oldFlags < 0 || fcntl(STDIN_FILENO, F_SETFL, oldFlags | O_NONBLOCK) < 0) {
    perror("stdin nao bloqueante");
    return 2;
  }
  puts("#CTRL OK READY mode=INTERACTIVE protocol=1");
  puts("#CTRL INFO Digite HELP; todos os comandos sao processados no mesmo fluxo da simulacao");
  while (!quit) {
    if (running) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - wallAnchor).count();
      const uint64_t target = simAnchor + static_cast<uint64_t>(std::max<int64_t>(elapsed, 0));
      // Limite de trabalho por ciclo para manter o comando interativo responsivo.
      unsigned ticks = 0;
      while (g_now_us < target && ticks < 1000) {
        tick();
        ++ticks;
      }
    }
    pollfd pfd{};
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN | POLLHUP;
    const int rc = poll(&pfd, 1, running ? 5 : 250);
    if (rc < 0) {
      if (errno == EINTR) continue;
      perror("poll");
      break;
    }
    if (rc > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
      char chars[1024];
      for (;;) {
        const ssize_t n = read(STDIN_FILENO, chars, sizeof(chars));
        if (n == 0) { quit = true; break; }
        if (n < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
          perror("read stdin");
          quit = true;
          break;
        }
        buffer.append(chars, static_cast<size_t>(n));
        if (buffer.size() > 16384) {
          buffer.clear();
          simControlError("entrada excedeu 16 KiB");
        }
        for (;;) {
          const size_t end = buffer.find('\n');
          if (end == std::string::npos) break;
          std::string command = buffer.substr(0, end);
          buffer.erase(0, end + 1);
          if (!command.empty() && command.back() == '\r') command.pop_back();
          simControl(command, running, quit, wallAnchor, simAnchor);
          if (quit) break;
        }
        if (quit) break;
      }
    }
  }
  fcntl(STDIN_FILENO, F_SETFL, oldFlags);
  return g_failures ? 1 : 0;
}
