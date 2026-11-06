/*
 * SEMANA 13 + DASHBOARD "Monitoreo Aula" - Adafruit IO + IFTTT
 * PUBLICA cada 10 s : temperatura, humedad (DHT11) y luminosidad (LDR)
 * ESCUCHA           : led-rojo ("ON"/"1" = encender, "OFF"/"0" = apagar)
 * ALERTA            : luminosidad < 100 -> Webhook de IFTTT -> notificacion push
 *                     (UNA sola alerta por cada oscuridad, se rearma al volver la luz)
 *
 * Hardware : ESP32 + DHT11 (DATA -> GPIO 27) + modulo LDR (DO -> GPIO 34)
 *            + LED rojo con resistencia 220 ohm (GPIO 26)
 * Librerias: PubSubClient (Nick O'Leary) y DHT sensor library (Adafruit)
 * Credenciales en config.h (NO se sube a GitHub).

 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <DHT.h>
#include "config.h"

#define DHTPIN       27   // Dato del DHT11
#define DHTTYPE      DHT11
#define LDR_PIN      34   // Salida del modulo LDR (solo entrada)
#define LED_ROJO_PIN 26   // LED rojo (con resistencia de 220 ohm)

// Con este modulo: MAS luz = numero BAJO, oscuridad = numero ALTO.
// Con true se invierte para que "oscuro" de un numero BAJO (< 100).
#define INVERTIR_LUZ true

const unsigned long INTERVALO_ENVIO     = 10000;  // 10 s
const unsigned long INTERVALO_REINTENTO = 5000;

// --- Alerta por webhook de IFTTT ---
const char*   IFTTT_EVENT          = "luminosidad_baja";  // mismo nombre que en el applet
const int     UMBRAL_LUZ           = 100;
const int     LECTURAS_PARA_ALERTA = 2;       // lecturas seguidas bajo el umbral

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);

char topicTemp[80];
char topicHum[80];
char topicLuz[80];
char topicLed[80];
unsigned long ultimoEnvio = 0;
unsigned long ultimoIntentoMQTT = 0;
bool alertaActiva = false;   // true = ya se aviso en esta oscuridad
int lecturasOscuro = 0;

// Llega un comando desde el dashboard (interruptor led-rojo)
void callback(char* topic, byte* payload, unsigned int length) {
  String mensaje = "";
  for (unsigned int i = 0; i < length; i++) {
    mensaje += (char)payload[i];
  }
  mensaje.trim();
  mensaje.toUpperCase();

  Serial.print("Comando recibido [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(mensaje);

  bool encender = (mensaje == "ON" || mensaje == "1");

  if (strcmp(topic, topicLed) == 0) {
    digitalWrite(LED_ROJO_PIN, encender ? HIGH : LOW);
    Serial.println(encender ? "-> LED rojo ENCENDIDO" : "-> LED rojo APAGADO");
  }
}

// Promedia 20 lecturas (0-4095); oscuro = valor bajo
int leerLuminosidad() {
  long suma = 0;
  for (int i = 0; i < 20; i++) {
    suma += analogRead(LDR_PIN);
    delay(5);
  }
  int valor = suma / 20;
  if (INVERTIR_LUZ) valor = 4095 - valor;
  return valor;
}

void conectarWiFi() {
  Serial.print("Conectando a WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(500);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[OK] WiFi conectado. IP: " + WiFi.localIP().toString());
  } else {
    Serial.print("\n[ERROR] Sin WiFi. Estado: ");
    Serial.println(WiFi.status());
  }
}

// reconnect(): mantiene viva la conexion MQTT sin bloquear el loop
void reconnect() {
  if (WiFi.status() != WL_CONNECTED) {
    conectarWiFi();
    return;
  }
  if (!client.connected() && millis() - ultimoIntentoMQTT >= INTERVALO_REINTENTO) {
    ultimoIntentoMQTT = millis();
    Serial.print("Conectando a Adafruit IO...");
    if (client.connect(MQTT_CLIENT_ID, AIO_USERNAME, AIO_KEY)) {
      Serial.println("OK");
      client.subscribe(topicLed);                 // escuchar el interruptor del LED
      ultimoEnvio = millis() - INTERVALO_ENVIO;   // publica de inmediato
    } else {
      Serial.print("Fallo, rc=");
      Serial.println(client.state());   // 4 = usuario/clave mal, 5 = no autorizado
    }
  }
}

// Llama al webhook de IFTTT mandando el valor como value1
void enviarAlertaIFTTT(int valor) {
  WiFiClientSecure seguro;
  seguro.setInsecure();   // sin verificar certificado (suficiente para el laboratorio)
  HTTPClient http;
  http.setTimeout(8000);

  String url = String("https://maker.ifttt.com/trigger/") + IFTTT_EVENT +
               "/with/key/" + IFTTT_KEY + "?value1=" + String(valor);

  if (http.begin(seguro, url)) {
    int codigo = http.GET();
    Serial.print(">>> ALERTA IFTTT enviada, codigo HTTP: ");
    Serial.println(codigo);   // 200 = IFTTT la recibio
    http.end();
  } else {
    Serial.println(">>> No se pudo iniciar la peticion a IFTTT");
  }
}

void publicarDatos() {
  float temperatura = dht.readTemperature();
  float humedad = dht.readHumidity();
  int luz = leerLuminosidad();

  Serial.println("--- Publicando en Adafruit IO ---");

  if (isnan(temperatura) || isnan(humedad)) {
    Serial.println("[ERROR] Fallo lectura DHT11, revisa el cableado");
  } else {
    client.publish(topicTemp, String(temperatura, 1).c_str());
    client.publish(topicHum, String(humedad, 1).c_str());
    Serial.print("Temperatura: "); Serial.print(temperatura, 1); Serial.println(" C");
    Serial.print("Humedad:     "); Serial.print(humedad, 1); Serial.println(" %");
  }

  client.publish(topicLuz, String(luz).c_str());
  Serial.print("Luminosidad: "); Serial.println(luz);

  // Alerta: UNA sola vez por cada episodio de oscuridad
  if (luz < UMBRAL_LUZ) {
    lecturasOscuro++;
  } else {
    lecturasOscuro = 0;
    if (luz > 500) alertaActiva = false;   // se rearma cuando vuelve la luz
  }

  if (lecturasOscuro >= LECTURAS_PARA_ALERTA && !alertaActiva) {
    enviarAlertaIFTTT(luz);
    alertaActiva = true;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(LDR_PIN, INPUT);
  pinMode(LED_ROJO_PIN, OUTPUT);
  digitalWrite(LED_ROJO_PIN, LOW);
  dht.begin();

  snprintf(topicTemp, sizeof(topicTemp), "%s/feeds/temperatura", AIO_USERNAME);
  snprintf(topicHum,  sizeof(topicHum),  "%s/feeds/humedad",     AIO_USERNAME);
  snprintf(topicLuz,  sizeof(topicLuz),  "%s/feeds/luminosidad", AIO_USERNAME);
  snprintf(topicLed,  sizeof(topicLed),  "%s/feeds/led-rojo",    AIO_USERNAME);

  client.setServer(AIO_SERVER, AIO_PORT);
  client.setCallback(callback);
  ultimoIntentoMQTT = millis() - INTERVALO_REINTENTO;
  conectarWiFi();
}

void loop() {
  reconnect();

  if (client.connected()) {
    client.loop();

    if (millis() - ultimoEnvio >= INTERVALO_ENVIO) {
      ultimoEnvio = millis();
      publicarDatos();
    }
  }
}