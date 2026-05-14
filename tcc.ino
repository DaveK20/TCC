/*
 * Sistema de Irrigação Inteligente - ESP32
 * 3x Sensores Capacitivos de Umidade do Solo
 * + Previsão do Tempo via OpenWeatherMap API
 *
 * Pinos dos sensores:
 *   Sensor 1 -> GPIO 34 (ADC1_CH6)
 *   Sensor 2 -> GPIO 35 (ADC1_CH7)
 *   Sensor 3 -> GPIO 32 (ADC1_CH4)
 *   Relé da bomba -> GPIO 26
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>  // Instale: ArduinoJson by Benoit Blanchon v6+

// ─── CONFIGURAÇÕES ───────────────────────────────────────────────
const char* WIFI_SSID = "SEU_WIFI";
const char* WIFI_PASSWORD = "SUA_SENHA";

const char* OWM_API_KEY = "SUA_CHAVE_OPENWEATHERMAP";
const char* OWM_CITY = "Rio de Janeiro";  // ou use lat/lon abaixo
const float OWM_LAT = -22.9068;
const float OWM_LON = -43.1729;

// ─── PINOS ───────────────────────────────────────────────────────
const int PIN_SENSOR_1 = 34;
const int PIN_SENSOR_2 = 35;
const int PIN_SENSOR_3 = 32;
const int PIN_RELE = 26;  // LOW = liga bomba (relé normalmente aberto)

// ─── CALIBRAÇÃO DOS SENSORES CAPACITIVOS ─────────────────────────
// Meça seu sensor:  no ar (seco) e submerso em água (úmido)
const int SENSOR_SECO = 2800;   // valor ADC no ar
const int SENSOR_UMIDO = 1200;  // valor ADC na água

// ─── LIMIARES DE IRRIGAÇÃO ────────────────────────────────────────
const float UMIDADE_MINIMA = 30.0;  // % — irriga abaixo disto
const float UMIDADE_ALVO = 60.0;    // % — para de irrigar ao atingir

// Se chuva prevista >= este valor (mm) nas próximas horas, não irriga
const float CHUVA_PREVISTA_LIMITE = 5.0;  // mm
// Se probabilidade de chuva >= este valor, não irriga
const float PROB_CHUVA_LIMITE = 0.60;  // 60%
// Se umidade relativa do ar >= este valor, solo provavelmente já está úmido
const float UMIDADE_AR_LIMITE = 85.0;  // %

// ─── INTERVALOS ──────────────────────────────────────────────────
const unsigned long INTERVALO_LEITURA = 10UL * 60 * 1000;  // 10 min
const unsigned long INTERVALO_WEATHER = 30UL * 60 * 1000;  // 30 min
const unsigned long TEMPO_MAX_IRRIGAR = 5UL * 60 * 1000;   // 5 min máximo

// ─── VARIÁVEIS GLOBAIS ────────────────────────────────────────────
struct DadosClimaticos {
  float tempC;
  float umidadeAr;        // %
  float chuvaUltimas3h;   // mm nas últimas 3h (current)
  float chuvaProximas;    // mm previstos nas próximas horas (forecast)
  float probChuva;        // 0.0 – 1.0
  float velocidadeVento;  // m/s
  bool valido = false;
};

DadosClimaticos clima;
unsigned long ultimaLeituraMs = 0;
unsigned long ultimoWeatherMs = 0;
unsigned long inicioIrrigacaoMs = 0;
bool irrigando = false;

// ─── FUNÇÕES AUXILIARES ───────────────────────────────────────────

float adcParaUmidade(int adc) {
  // Mapeia o valor ADC invertido para 0–100%
  int val = constrain(adc, SENSOR_UMIDO, SENSOR_SECO);
  return map(val, SENSOR_SECO, SENSOR_UMIDO, 0, 100);
}

float lerSensor(int pino) {
  // Faz 10 leituras e retorna a média para reduzir ruído
  long soma = 0;
  for (int i = 0; i < 10; i++) {
    soma += analogRead(pino);
    delay(10);
  }

  int media = soma / 10;
  return adcParaUmidade(media);
}

void ligarBomba() {
  if (!irrigando) {
    digitalWrite(PIN_RELE, LOW);  // ativa relé
    irrigando = true;
    inicioIrrigacaoMs = millis();
    Serial.println("🚿 IRRIGAÇÃO INICIADA");
  }
}

void desligarBomba() {
  if (irrigando) {
    digitalWrite(PIN_RELE, HIGH);  // desativa relé
    irrigando = false;
    Serial.println("✅ IRRIGAÇÃO ENCERRADA");
  }
}

// ─── OPENWEATHERMAP ───────────────────────────────────────────────

bool buscarClimaAtual() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = "http://api.openweathermap.org/data/2.5/weather?lat=" + String(OWM_LAT, 4) + "&lon=" + String(OWM_LON, 4) + "&appid=" + String(OWM_API_KEY) + "&units=metric&lang=pt_br";

  HTTPClient http;
  http.begin(url);
  int code = http.GET();

  if (code != 200) {
    Serial.println("Erro ao buscar clima atual: HTTP " + String(code));
    http.end();
    return false;
  }

  DynamicJsonDocument doc(2048);
  deserializeJson(doc, http.getString());
  http.end();

  clima.tempC = doc["main"]["temp"];
  clima.umidadeAr = doc["main"]["humidity"];
  clima.velocidadeVento = doc["wind"]["speed"];

  // Chuva nas últimas 3h (campo opcional na API)
  if (doc.containsKey("rain") && doc["rain"].containsKey("3h")) {
    clima.chuvaUltimas3h = doc["rain"]["3h"];
  } else {
    clima.chuvaUltimas3h = 0.0;
  }

  return true;
}

bool buscarPrevisao() {
  if (WiFi.status() != WL_CONNECTED) return false;

  // Forecast de 5 dias / 3h — pega as próximas 3 entradas (~9h)
  String url = "http://api.openweathermap.org/data/2.5/forecast?lat=" + String(OWM_LAT, 4) + "&lon=" + String(OWM_LON, 4) + "&appid=" + String(OWM_API_KEY) + "&units=metric&cnt=3&lang=pt_br";

  HTTPClient http;
  http.begin(url);
  int code = http.GET();

  if (code != 200) {
    Serial.println("Erro ao buscar previsão: HTTP " + String(code));
    http.end();
    return false;
  }

  DynamicJsonDocument doc(4096);
  deserializeJson(doc, http.getString());
  http.end();

  float totalChuva = 0.0;
  float maxProb = 0.0;
  JsonArray lista = doc["list"];

  for (JsonObject item : lista) {
    float pop = item["pop"] | 0.0;  // probability of precipitation
    if (pop > maxProb) maxProb = pop;

    if (item.containsKey("rain") && item["rain"].containsKey("3h")) {
      totalChuva += (float)item["rain"]["3h"];
    }
  }

  clima.chuvaProximas = totalChuva;
  clima.probChuva = maxProb;
  clima.valido = true;

  return true;
}

void atualizarClima() {
  Serial.println("\n📡 Atualizando dados climáticos...");
  if (buscarClimaAtual() && buscarPrevisao()) {
    Serial.printf("  🌡️  Temperatura:      %.1f °C\n", clima.tempC);
    Serial.printf("  💧 Umidade do ar:    %.0f %%\n", clima.umidadeAr);
    Serial.printf("  🌧️  Chuva últimas 3h: %.1f mm\n", clima.chuvaUltimas3h);
    Serial.printf("  🌦️  Chuva prevista:   %.1f mm\n", clima.chuvaProximas);
    Serial.printf("  📊 Prob. de chuva:   %.0f %%\n", clima.probChuva * 100);
    Serial.printf("  💨 Vento:            %.1f m/s\n", clima.velocidadeVento);
  }
}

// ─── LÓGICA DE DECISÃO ────────────────────────────────────────────

void avaliarIrrigacao(float u1, float u2, float u3) {
  float mediaUmidade = (u1 + u2 + u3) / 3.0;

  Serial.println("\n🌱 Avaliando irrigação...");
  Serial.printf("  Sensor 1: %.1f%%  Sensor 2: %.1f%%  Sensor 3: %.1f%%\n", u1, u2, u3);
  Serial.printf("  Média umidade solo: %.1f%%\n", mediaUmidade);

  // ── Motivos para NÃO irrigar ──────────────────────────────────
  if (mediaUmidade >= UMIDADE_ALVO) {
    Serial.println("  ➡️  Solo já está suficientemente úmido. Sem necessidade.");
    desligarBomba();
    return;
  }

  if (clima.valido) {
    if (clima.chuvaProximas >= CHUVA_PREVISTA_LIMITE) {
      Serial.printf("  ➡️  Chuva prevista de %.1f mm. Irrigação adiada.\n", clima.chuvaProximas);
      desligarBomba();
      return;
    }
    if (clima.probChuva >= PROB_CHUVA_LIMITE) {
      Serial.printf("  ➡️  Probabilidade de chuva de %.0f%%. Irrigação adiada.\n", clima.probChuva * 100);
      desligarBomba();
      return;
    }
    if (clima.chuvaUltimas3h >= CHUVA_PREVISTA_LIMITE) {
      Serial.printf("  ➡️  Choveu %.1f mm nas últimas 3h. Solo pode estar úmido.\n", clima.chuvaUltimas3h);
      desligarBomba();
      return;
    }
    if (clima.umidadeAr >= UMIDADE_AR_LIMITE) {
      Serial.printf("  ➡️  Umidade do ar muito alta (%.0f%%). Evaporação baixa.\n", clima.umidadeAr);
      desligarBomba();
      return;
    }
  }

  // ── Motivo para irrigar ───────────────────────────────────────
  if (mediaUmidade < UMIDADE_MINIMA) {
    Serial.printf("  ✅ Solo seco (%.1f%% < %.1f%%). IRRIGANDO!\n", mediaUmidade, UMIDADE_MINIMA);
    ligarBomba();
    return;
  }

  Serial.println("  ➡️  Umidade aceitável. Aguardando.");
  desligarBomba();
}

// ─── SETUP & LOOP ─────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_RELE, OUTPUT);
  digitalWrite(PIN_RELE, HIGH);  // garante bomba desligada na inicialização

  analogReadResolution(12);  // ESP32: ADC 12 bits (0–4095)

  Serial.println("\n=== Sistema de Irrigação Inteligente ===");

  // Conecta ao WiFi
  Serial.printf("Conectando ao WiFi '%s'", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi conectado! IP: " + WiFi.localIP().toString());
    atualizarClima();
    ultimoWeatherMs = millis();
  } else {
    Serial.println("\n⚠️  WiFi falhou. Operando apenas com sensores.");
  }

  ultimaLeituraMs = millis() - INTERVALO_LEITURA;  // força leitura imediata
}

void loop() {
  unsigned long agora = millis();

  // Atualiza clima a cada 30 min
  if (agora - ultimoWeatherMs >= INTERVALO_WEATHER) {
    if (WiFi.status() == WL_CONNECTED) {
      atualizarClima();
    } else {
      WiFi.reconnect();
    }
    ultimoWeatherMs = agora;
  }

  // Leitura dos sensores e decisão a cada 10 min
  if (agora - ultimaLeituraMs >= INTERVALO_LEITURA) {
    float u1 = lerSensor(PIN_SENSOR_1);
    float u2 = lerSensor(PIN_SENSOR_2);
    float u3 = lerSensor(PIN_SENSOR_3);

    avaliarIrrigacao(u1, u2, u3);
    ultimaLeituraMs = agora;
  }

  // Segurança: desliga bomba após tempo máximo
  if (irrigando && (agora - inicioIrrigacaoMs >= TEMPO_MAX_IRRIGAR)) {
    Serial.println("⏱️  Tempo máximo de irrigação atingido. Desligando.");
    desligarBomba();
  }

  delay(500);
}
