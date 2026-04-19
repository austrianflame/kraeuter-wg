#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h> 
#include <time.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <ArduinoOTA.h> 
#include <Preferences.h> 
#include <esp_now.h> // NEU für die Funkverbindung
#include "PCF8574.h"
#include <ESPmDNS.h>
#include <Update.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h> // NEU: Für HomeAssistant
#include "LittleFS.h"

// --- WIFI ---
unsigned long lastWifiCheck = 0;
unsigned long wifiCheckInterval = 300000; // Start mit 5 Minuten (300.000 ms)
int wifiFailCount = 0;

// --- MQTT (HOMEASSISTANT) ---
WiFiClient espClient;
PubSubClient mqttClient(espClient);

bool USE_MQTT = false;
unsigned long letzterMqttVersuch = 0;
unsigned long mqttOfflineSeit = 0; // Merkt sich, wann wir die Verbindung verloren haben
bool mqttWarVerbunden = false;     // Status-Tracker für den Wechsel

bool siemBlockWatering = false; // NEU: Der globale SIEM-Schild
bool systemPausiert = false; // NEU: Der globale System-Pause-Schalter
bool pinGesperrt[110] = {false}; // NEU: Globales Schloss-System für jedes einzelne Relais
String mqttServer = "";
int mqttPort = 1883;
String mqttUser = "";
String mqttPass = "";
String mqttTopic = "kraeuterwg"; // Basis-Ordner im MQTT

// Virtuelle Sensoren für das SIEM (Werden von HomeAssistant per MQTT gefüttert!)
float mqttSensorWert[2] = {0.0, 0.0};

bool soakTimeAktiv[6] = {false};

// --- I2C EXPANDER (PCF8574) ---
bool USE_PCF = false;
PCF8574 pcf(0x20); // 0x20 ist die Standard-Adresse. Manche Module haben 0x27 oder 0x38.

// Das Datenpaket, das in die Luft geschickt wird
typedef struct struct_message {
    char name[20];        // Name des Systems
    int feuchte[6];       // Feuchtigkeit aller 6 Töpfe
    bool tank;            // Wassertank OK?
    bool tank2;           // NEU: Status von Tank 2
    bool hasKreis2;       // NEU: Ist Kreislauf 2 überhaupt aktiv?
    float temp[3];        // GEÄNDERT: Array für alle 3 Zonen
    float luft[3];        // NEU: Luftfeuchtigkeit für alle 3 Zonen
    int phase;            // Aktuelle Phase (1 oder 2)
    int minuten;          // Minuten im Check
    int anzToepfe;        // NEU: Wie viele Töpfe sind aktiv?
    int anzDht;           // NEU: Damit das Display weiß, wie viele DHTs da sind
    char zeit[6];         // NEU: Die aktuelle Uhrzeit ("12:34")
    char tName[6][12];    // <--- NEU: Speicher für 6 kurze Namen!
    // --- NEU FÜR DIE TOUCH-DETAILS ---
    char letztesGiessen[6][15]; // <--- GEÄNDERT von [6][6] auf [6][15]
    int topfStatus[6];         // 0=OK, 1=Trocken, 2=DeepCheck, 3=Gesperrt/Fehler
    bool isPaused;        // <--- NEU: Der Pause-Status für das CYD!
} struct_message;

struct_message myData;    // Speicher für das aktuelle Paket

// ============================================================
// SYSTEM LIVE-LOG (Ring-Puffer im RAM)
// ============================================================
#define MAX_LOG_LINES 15
String sysLogs[MAX_LOG_LINES];

void addLog(String msg) {
  // Zeitstempel generieren
  struct tm tinfo;
  String tStr = "";
  if(getLocalTime(&tinfo)){
     char tBuff[12];
     strftime(tBuff, sizeof(tBuff), "%H:%M:%S", &tinfo);
     tStr = "[" + String(tBuff) + "] ";
  } else {
     tStr = "[??:??:??] ";
  }
  
  String fullMsg = tStr + msg;

  // Alle alten Logs um 1 nach oben schieben
  for(int i=0; i<MAX_LOG_LINES-1; i++){
    sysLogs[i] = sysLogs[i+1];
  }
  // Neuestes Log ganz unten einfügen
  sysLogs[MAX_LOG_LINES-1] = fullMsg;
  
  // Auch über den normalen Serial-Monitor ausgeben!
  Serial.println("🪵 LOG: " + msg);
}

// ============================================================
// WEBSERVER KONFIGURATION
// ============================================================
WebServer server(80);
Preferences preferences;
File fsUploadFile; // Speichert den Datei-Upload temporär

// ============================================================
// HARDWARE PINS (ESP32)
// ============================================================
#define SCREEN_WIDTH 128 
#define SCREEN_HEIGHT 64 
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

//const int PIN_TEMP = 4;
#define DHTTYPE DHT11           


int PIN_SCHWIMMER = 5;   
int PIN_PUMPE = 27;      

// --- ESP-NOW SETTINGS ---
bool ESP_NOW_ACTIVE = false;
String DISPLAY_MAC = "FF:FF:FF:FF:FF:FF"; 

#define MAX_DISPLAYS 3 // Maximal 3 Displays gleichzeitig
uint8_t broadcastAddresses[MAX_DISPLAYS][6]; 
int registeredDisplays = 0; // Zählt, wie viele gültige MACs gefunden wurden



// --- EXTRA HARDWARE (FREIE SENSOREN) ---
int extraSensorPin[2] = {-1, -1}; // -1 = Deaktiviert
String extraSensorName[2] = {"Extra Sensor 1", "Extra Sensor 2"};
int extraSensorWert[2] = {0, 0}; // Hier speichert die loop() später den aktuellen Messwert

// --- NEU: MASTER-SYSTEM-EINSTELLUNGEN ---
int ANZAHL_TOEPFE = 3;          // Dynamisch 1 bis 6
bool ZWEITER_KREISLAUF = false; 
bool USE_DISPLAY = true;

// --- AUTH & SICHERHEIT ---
bool USE_AUTH = false;
String authUser = "admin";
String authPass = "flame";

// --- DÜNGER-PLANER (0=Täglich, 1-7=Mo-So, 8=Alle 3 Tage, 9=14 Tage, 10=21 Tage, 11=1. im Monat) ---
int mosfetMode[3] = {0, 0, 0};

int PIN_PUMPE2 = 16;            // <--- NEU: Pin für 2. Pumpe
int PIN_SCHWIMMER2 = 15;        // <--- NEU: Pin für 2. Tank-Sensor 

// --- KLIMA-SENSOREN (MULTI-ZONE) ---
#define DHT_TYPE DHT11 // (Oder DHT22)
int ANZAHL_DHT = 1;    // 0 bis 3
int PIN_DHT[3] = {4, 18, 19}; // Standard-Startwerte
float aktuelleTemp[3] = {0.0, 0.0, 0.0};
float aktuelleLuft[3] = {0.0, 0.0, 0.0};
DHT* dhtSensors[3]; // Genialer Trick: Dynamische Pointer für die Sensoren


// --- AUSTRIAN FLAME: SPRACH-ENGINE & BRANDING ---
bool isEnglish = false; // Standard ist 🇦🇹

String systemName = "kraeuter-wg"; // Standard-Name

String timeZone = "CET-1CEST,M3.5.0,M10.5.0/3"; // Standard: Österreich

String t(String de, String en) {
  return isEnglish ? en : de;
}

// Baut die Navbar für jede Seite automatisch (JETZT MIT DARK MODE HACK!)
String getNavbar() {
  String h = "<script>";
  // Das smarte Javascript für den Browser-Speicher
  h += "function toggleDark(){ ";
  h += "  document.body.classList.toggle('dark-theme'); ";
  h += "  let isDark = document.body.classList.contains('dark-theme'); ";
  h += "  localStorage.setItem('kraeuterDark', isDark ? '1' : '0'); ";
  h += "  document.getElementById('darkBtn').innerText = isDark ? '☀️' : '🌙'; ";
  h += "} ";
  h += "document.addEventListener('DOMContentLoaded', () => { ";
  h += "  if(localStorage.getItem('kraeuterDark') === '1') { ";
  h += "    document.body.classList.add('dark-theme'); ";
  h += "    let btn = document.getElementById('darkBtn'); if(btn) btn.innerText = '☀️'; ";
  h += "  } ";
  h += "});";
  h += "</script>";

  // Der CSS Overdrive (Überschreibt alle weißen Farben brutal mit dunklen!)
  h += "<style>";
  h += "body.dark-theme { background-color: #121212 !important; color: #e0e0e0 !important; } ";
  h += ".dark-theme .card { background-color: #1e1e1e !important; box-shadow: 0 4px 6px rgba(0,0,0,0.8) !important; border-color:#333 !important; } ";
  h += ".dark-theme h1, .dark-theme h2, .dark-theme h3, .dark-theme h4, .dark-theme label, .dark-theme p { color: #ecf0f1 !important; } ";
  h += ".dark-theme input, .dark-theme select { background: #2c3e50 !important; color: #fff !important; border: 1px solid #555 !important; } ";
  h += ".dark-theme .rule-box { background: #2c3e50 !important; border-color: #555 !important; } ";
  h += ".dark-theme table th { background-color: #2c3e50 !important; color: white !important; } ";
  h += ".dark-theme table td, .dark-theme table tr { background-color: #1e1e1e !important; border-color: #444 !important; color: #e0e0e0 !important;} ";
  // Hacker-Trick: Alle hellen "Pastell-Boxen" (die inline Styles haben) abdunkeln
  h += ".dark-theme div[style*='background:#f'], .dark-theme div[style*='background:#e'] { background-color: #2c3e50 !important; color: #ecf0f1 !important; border-color: #444 !important; } ";
  h += "</style>";

  // Die eigentliche sichtbare Navbar
  h += "<div style='display:flex; justify-content:space-between; align-items:center; background:#2c3e50; padding:10px 15px; color:white; border-radius:5px; margin-bottom:15px;'>";
  h += "<div style='font-size:14px; font-weight:bold; letter-spacing:1px;'>🔥 AUSTRIAN FLAME</div>";
  h += "<div style='font-size:22px; display:flex; gap:10px; align-items:center;'>";
  
  // Der neue Dark-Mode Button
  h += "<span id='darkBtn' onclick='toggleDark()' style='cursor:pointer; text-decoration:none; font-size:20px;'>🌙</span>"; 
  h += "<span style='border-left:1px solid #7f8c8d; height:20px; margin:0 2px;'></span>"; // Optischer Trennstrich
  
  // JS-Funktion für einen sicheren POST-Sprachwechsel
  h += "<script>function setLang(en) { fetch('/setlang?en=' + en, {method: 'POST'}).then(() => location.reload()); }</script>";
  
  // Die neuen, sicheren Flaggen-Buttons
  h += "<span onclick='setLang(0)' style='cursor:pointer; text-decoration:none; opacity:" + String(isEnglish ? "0.4" : "1.0") + ";'>🇦🇹</span>";
  h += "<span onclick='setLang(1)' style='cursor:pointer; text-decoration:none; opacity:" + String(isEnglish ? "1.0" : "0.4") + ";'>🇬🇧</span>";
  h += "</div></div>";
  
  return h;
}

// --- NEU: ARRAYS AUF MAXIMAL 6 TÖPFE ERWEITERT ---
#define MAX_TOEPFE 6
int PIN_ERDE[MAX_TOEPFE];   
int PIN_VENTIL[MAX_TOEPFE]; 
int ZIEL_FEUCHTIGKEIT[MAX_TOEPFE]; 
String NAME_TOPF[MAX_TOEPFE];
int GIESS_DAUER[MAX_TOEPFE]; 
int PUMPEN_WAHL[MAX_TOEPFE]; // 0 = Pumpe 1, 1 = Pumpe 2

// Die sicheren Pins für das Dropdown-Menü im Web
const int SAFE_ADC_PINS[] = {32, 33, 34, 35, 36, 39};
//const int SAFE_OUT_PINS[] = {4, 13, 14, 16, 17, 18, 19, 23, 25, 26, 27};
// 12 absolut sichere Pins für Relais und digitale Sensoren
const int SAFE_OUT_PINS[12] = {5, 13, 14, 15, 16, 25, 26, 27, 18, 19, 23, 4};

// --- INDIVIDUELLE SENSOR KALIBRIERUNG (Für 6 Töpfe) ---
int SENSOR_TROCKEN[MAX_TOEPFE] = {2760, 2760, 2760, 2760, 2760, 2760};  
int SENSOR_NASS[MAX_TOEPFE]    = {1130, 1130, 1130, 1130, 1130, 1130};

const long MINUTE_IN_MS = 60000;  
//Test safte Moed
//const long MINUTE_IN_MS = 1000;  // 1 Sekunde simuliert 1 Minute!

// ============================================================
// GLOBALE VARIABLEN (SPEICHER FÜR 6 TÖPFE)
// ============================================================
int aktuelleErde[MAX_TOEPFE];
int gedaechtnisWert[MAX_TOEPFE]; 
int fehlversuche[MAX_TOEPFE] = {0}; // Zählt, wie oft gegossen wurde, ohne dass es feuchter wurde
String letztesGiessen[MAX_TOEPFE];
bool pumpenSperre[MAX_TOEPFE]; 
bool imDeepCheck[MAX_TOEPFE]; 


bool tankVoll = true;
bool tankVoll2 = true; // <--- NEU
String aktuelleZeit = "00:00";
String aktuellesDatum = "00.00.0000";
String aktuelleIP = "Verbinde...";

int aktuellePhase = 1; 
int minutenInPhase = 0;

// ============================================================
// AUSTRIAN FLAME AUTOMATOR (Smart Rules)
// ============================================================
#define MAX_RULES 8

// --- NEUE PRIORITÄTEN (1 = Höchste, 11 = Niedrigste) ---
int rulePrio[MAX_RULES] = {1, 2, 3, 4, 5, 6, 7, 8}; 
int timerPrio[3] = {9, 10, 11};
int timerLink[3] = {0, 0, 0}; // NEU: 0 = Immer an, 1-8 = Nur wenn Regel 1-8 WAHR ist

// Arrays für die 8 Regeln (werden im EEPROM/Preferences gespeichert)
bool ruleActive[MAX_RULES] = {false, false, false, false, false, false, false, false};

// Welcher Sensor löst aus? (0=Aus, 1=Temp1, 2=Temp2, 10=Topf1... etc.)
int ruleTrigger[MAX_RULES] = {0}; 

// Bedingung: 0 = Kleiner als (<), 1 = Größer als (>)
int ruleCondition[MAX_RULES] = {0};

// Der Schwellenwert (z.B. 30 Grad oder 40% Feuchtigkeit)
int ruleValue[MAX_RULES] = {0};

// Was soll passieren? (-1 = Nichts, echte Pins, oder 100+ für PCF)
int ruleAction1[MAX_RULES] = {-1, -1, -1, -1, -1, -1, -1, -1};
int ruleAction2[MAX_RULES] = {-1, -1, -1, -1, -1, -1, -1, -1}; // Das "UND" Feld!
int ruleMode[MAX_RULES] = {0}; // NEU: 0 = Einschalten (LOW), 1 = Sperren/Aus (HIGH)

int ruleDuration[MAX_RULES] = {0}; 
int rulePause[MAX_RULES] = {0}; // NEU: Die Cooldown-Zeit!
unsigned long ruleTimer[MAX_RULES] = {0};

// Zeitschaltuhr für MOSFETS (USB Lampen etc.)
int mosfetPin[3] = {-1, -1, -1}; // Bis zu 3 Zeit-Kanäle
String mosfetStart[3] = {"08:00", "08:00", "08:00"};
String mosfetStop[3] = {"20:00", "20:00", "20:00"};


// ============================================================
// FESTPLATTE LADEN
// ============================================================
void loadPreferences() {
  preferences.begin("kraeuter", false); 

  isEnglish = preferences.getBool("langEn", false);

  USE_PCF = preferences.getBool("usePcf", false);

  USE_AUTH = preferences.getBool("useAuth", false);
  authUser = preferences.getString("authUser", "admin");
  authPass = preferences.getString("authPass", "flame");

  PIN_PUMPE = preferences.getInt("pinP1", 27);
  PIN_SCHWIMMER = preferences.getInt("pinS1", 5);

  USE_MQTT = preferences.getBool("useMqtt", false);
  mqttServer = preferences.getString("mqSrv", "");
  mqttPort = preferences.getInt("mqPrt", 1883);
  mqttUser = preferences.getString("mqUsr", "");
  mqttPass = preferences.getString("mqPwd", "");
  mqttTopic = preferences.getString("mqTop", "kraeuterwg");

  // --- NEU: Den mDNS Namen direkt beim Systemstart in den RAM laden ---
  systemName = preferences.getString("sysName", "kraeuter-wg");

  ANZAHL_TOEPFE = preferences.getInt("anzToepfe", 3);
  ZWEITER_KREISLAUF = preferences.getBool("zweiKreis", false);
  USE_DISPLAY = preferences.getBool("useDisp", true);

  systemPausiert = preferences.getBool("sysPause", false);

  timeZone = preferences.getString("tz", "CET-1CEST,M3.5.0,M10.5.0/3");

  for(int i=0; i<3; i++) {
      timerPrio[i] = preferences.getInt(("tPri" + String(i)).c_str(), 9+i);
      timerLink[i] = preferences.getInt(("tLnk" + String(i)).c_str(), 0); // <--- DIESE ZEILE NEU!
  }
  for(int i=0; i<MAX_RULES; i++) rulePrio[i] = preferences.getInt(("rPri" + String(i)).c_str(), 1+i);

  for(int i=0; i<2; i++) {
    extraSensorName[i] = preferences.getString(("esName"+String(i)).c_str(), "Extra Sensor " + String(i+1));
    extraSensorPin[i] = preferences.getInt(("esPin"+String(i)).c_str(), -1);
  }

  ANZAHL_DHT = preferences.getInt("anzDht", 1);
  for(int i=0; i<3; i++) {
    // NEU: Nimmt die Pins von GANZ HINTEN aus dem 11er-Array (Index 10, 9, 8)
    PIN_DHT[i] = preferences.getInt(("pinDht"+String(i)).c_str(), SAFE_OUT_PINS[10 - i]);
  }

  ESP_NOW_ACTIVE = preferences.getBool("useNow", false);
  DISPLAY_MAC = preferences.getString("dispmac", "FF:FF:FF:FF:FF:FF");

  // Pins für den zweiten Kreislauf laden (16 ist jetzt SAFE_OUT_PINS[3])
  PIN_PUMPE2 = preferences.getInt("pinP2", 16);
  PIN_SCHWIMMER2 = preferences.getInt("pinS2", 15);

  for(int i=0; i<MAX_TOEPFE; i++) {
    NAME_TOPF[i] = preferences.getString(("name"+String(i)).c_str(), "Topf " + String(i+1));
    ZIEL_FEUCHTIGKEIT[i] = preferences.getInt(("ziel"+String(i)).c_str(), 50);
    GIESS_DAUER[i] = preferences.getInt(("dauer"+String(i)).c_str(), 3000);
    PUMPEN_WAHL[i] = preferences.getInt(("pWahl"+String(i)).c_str(), 0); 
    
    // Die Erdsensoren bleiben in ihrem sicheren ADC-Revier
    PIN_ERDE[i] = preferences.getInt(("pinE"+String(i)).c_str(), SAFE_ADC_PINS[i % 6]);
    
    // NEU: Modulo 6 zwingt die Ventile, nur die vordersten Pins (Index 0-5) zu nehmen!
    PIN_VENTIL[i] = preferences.getInt(("pinV"+String(i)).c_str(), SAFE_OUT_PINS[i % 6]);
    
    // RAM-Variablen für den Live-Betrieb nullen
    aktuelleErde[i] = 0; gedaechtnisWert[i] = -1;
    pumpenSperre[i] = false; imDeepCheck[i] = false;

    // 💾 NEU: Historie laden! (Standardwert ist "Neustart", falls der ESP32 komplett leer ist)
    letztesGiessen[i] = preferences.getString(("lGi" + String(i)).c_str(), "Neustart");

    // Individuelle Kalibrierung laden
    SENSOR_TROCKEN[i] = preferences.getInt(("trocken"+String(i)).c_str(), 2760);
    SENSOR_NASS[i] = preferences.getInt(("nass"+String(i)).c_str(), 1130);
  }

  // --- AUTOMATISIERUNG LADEN ---
  for(int i=0; i<3; i++) {
    mosfetPin[i] = preferences.getInt(("mPin" + String(i)).c_str(), -1);
    mosfetStart[i] = preferences.getString(("mSta" + String(i)).c_str(), "08:00");
    mosfetStop[i] = preferences.getString(("mSto" + String(i)).c_str(), "20:00");
  }

  for(int i=0; i<MAX_RULES; i++) {
    ruleActive[i] = preferences.getBool(("rAct" + String(i)).c_str(), false);
    ruleTrigger[i] = preferences.getInt(("rTri" + String(i)).c_str(), 0);
    ruleCondition[i] = preferences.getInt(("rCon" + String(i)).c_str(), 0);
    ruleValue[i] = preferences.getInt(("rVal" + String(i)).c_str(), 0);
    ruleAction1[i] = preferences.getInt(("rA1_" + String(i)).c_str(), -1);
    ruleAction2[i] = preferences.getInt(("rA2_" + String(i)).c_str(), -1);
    ruleMode[i] = preferences.getInt(("rMod" + String(i)).c_str(), 0); // <-- DAS IST NEU
    ruleDuration[i] = preferences.getInt(("rDur" + String(i)).c_str(), 0);

    ruleDuration[i] = preferences.getInt(("rDur" + String(i)).c_str(), 0);
    rulePause[i] = preferences.getInt(("rPau" + String(i)).c_str(), 0); // NEU
  }

  preferences.end();
}

// ============================================================
// WEBSERVER HTML-SEITE (DAS MODERNE DASHBOARD)
// ============================================================
void handleRoot() {
  
  // --- DER TÜRSTEHER ---
  if (USE_AUTH && !server.authenticate(authUser.c_str(), authPass.c_str())) {
    return server.requestAuthentication();
  }

  // --- RAM OPTIMIERUNG ---
  String html;
  html.reserve(8000); 
  html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><meta http-equiv='refresh' content='60'>";
  html += getNavbar();
  

  html += "<title>Kraeuter-WG</title>";
  html += "<style>body{font-family:'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color:#eef2f3; padding:15px; text-align:center;} ";
  html += ".card{background:#fff; padding:20px; border-radius:15px; box-shadow:0 10px 20px rgba(0,0,0,0.05); margin-bottom:20px;} ";
  html += ".val{font-size:26px; font-weight:bold; color:#27ae60;} .warn{color:#e74c3c;} ";
  html += "a.btn{display:inline-block; background-color:#8e44ad; color:white; padding:12px 25px; text-decoration:none; border-radius:8px; font-weight:bold; margin-top:10px;}</style>";
  html += "</head><body>";
  
  // --- NEUES BRANDING & UHRZEIT ---
  html += "<h1>🌿 Meine Kräuter-WG</h1>";
  // Zeigt die NTP-Uhrzeit dezent unter dem Titel an
  // --- UHRZEIT & BUTTONS ---
  html += "<p style='text-align:center; color:#7f8c8d; font-size:14px; margin-top:-10px; margin-bottom:20px;'>🕒 " + t("Aktuelle Systemzeit:", "Current System Time:") + " <b>" + aktuellesDatum + " - " + aktuelleZeit + t(" Uhr", "") + "</b></p>";
 
 // --- DIE VIER HAUPT-BUTTONS (ECHTES 2x2 GRID) ---
 html += "<div style='display:grid; grid-template-columns: 1fr 1fr; gap:10px; margin-bottom:15px;'>";
 html += "<a href='/sys_settings' class='btn' style='background-color:#7f8c8d; margin:0; display:flex; align-items:center; justify-content:center; text-align:center;'>⚙️ " + t("System", "System") + "</a>";
 html += "<a href='/plant_settings' class='btn' style='background-color:#27ae60; margin:0; display:flex; align-items:center; justify-content:center; text-align:center;'>🪴 " + t("Pflanzen", "Plants") + "</a>";
 html += "<a href='/automation' class='btn' style='background-color:#e67e22; margin:0; display:flex; align-items:center; justify-content:center; text-align:center;'>🤖 " + t("Smart Rules", "Smart Rules") + "</a>";
 html += "<a href='/help' class='btn' style='background-color:#3498db; margin:0; display:flex; align-items:center; justify-content:center; text-align:center;'>ℹ️ " + t("Hilfe", "Help") + "</a>";
 html += "</div>";

 // --- NEU: DER FETTE PAUSE-BUTTON ---
 String pauseColor = systemPausiert ? "#c0392b" : "#27ae60"; // Rot = Pausiert, Grün = Aktiv
 String pauseText = systemPausiert ? t("⏸️ STOP AKTIV (Klicken für Start)", "⏸️ STOP ACTIVE (Click to Start)") : t("▶️ SYSTEM AKTIV (Klicken für Pause)", "▶️ SYSTEM ACTIVE (Click to Pause)");
 html += "<a href='/toggle_pause' class='btn' style='background-color:" + pauseColor + "; margin:0 0 20px 0; display:block; text-align:center; width:100%; box-sizing:border-box;'>" + pauseText + "</a>";

 // Wenn pausiert, zeige ganz oben einen fetten Warnbanner!
 if (systemPausiert) {
     html += "<div style='background:#c0392b; color:white; padding:15px; border-radius:8px; margin-bottom:20px; font-weight:bold; font-size:18px;'>⚠️ " + t("SYSTEM IST PAUSIERT! Automatische Bewässerung ist deaktiviert.", "SYSTEM IS PAUSED! Auto-watering is disabled.") + "</div>";
 }


  // --- KLIMA & TANK ---
  html += "<div class='card'><h3>🌤️ " + t("Klima & Tank", "Climate & Tank") + "</h3>";
  
  if(ANZAHL_DHT > 0) {
    for(int i=0; i<ANZAHL_DHT; i++) {
      html += "<h4>" + t("Zone ", "Zone ") + String(i+1) + "</h4>";
      html += "<p style='margin:0;'>" + t("Temp", "Temp") + ": <b>" + String(aktuelleTemp[i], 1) + " &deg;C</b> | " + t("Luft", "Hum") + ": <b>" + String(aktuelleLuft[i], 1) + " %</b></p>";
    }
  } else {
    html += "<p style='color:#7f8c8d;'><i>" + t("Keine Klima-Sensoren aktiv", "No climate sensors active") + "</i></p>";
  }

  // Wassertank Status übersetzen
  // --- NEU: Dynamische Tank-Anzeige ---
  String tankStatus = tankVoll ? "OK" : t("LEER!", "EMPTY!");
  html += "<p>" + t("Haupt-Tank", "Main Tank") + ": <span class='val " + String(tankVoll ? "" : "warn") + "'>" + tankStatus + "</span></p>";

  // Wenn Kreislauf 2 aktiv ist, blenden wir einfach eine zweite Zeile ein!
  if (ZWEITER_KREISLAUF) {
    String tankStatus2 = tankVoll2 ? "OK" : t("LEER!", "EMPTY!");
    html += "<p style='margin-top:-10px;'>" + t("Tank 2", "Tank 2") + ": <span class='val " + String(tankVoll2 ? "" : "warn") + "'>" + tankStatus2 + "</span></p>";
  }

  html += "</div>";

  for(int i=0; i<ANZAHL_TOEPFE; i++) {
    // NEU: Smarte Namens-Erkennung!
    String dName = NAME_TOPF[i];
    if (dName == "Topf " + String(i+1) || dName == "") dName = t("Topf ", "Pot ") + String(i+1);
    
    html += "<div class='card'><h3>🪴 " + dName + "</h3>"; // Nur noch der saubere Name!
    html += "<p>" + t("Feuchtigkeit: ", "Humidity: ") + "<span class='val'>" + String(aktuelleErde[i]) + " %</span> " + t("(Ziel: ", "(Target: ") + String(ZIEL_FEUCHTIGKEIT[i]) + "%)</p>";
    
    html += "<div style='background:#f8f9fa; padding:10px; border-radius:8px; margin:10px 0; text-align:left; border-left:4px solid ";
    
    if(pumpenSperre[i]) {
    // Unterscheiden zwischen Kabelbruch (<3%) und Wasser-Flow-Fehler (>= 3 Versuche)
      if (fehlversuche[i] >= 2) {
     html += "#e67e22;'><b style='color:#e67e22;'>🏜️ " + t("WASSER-ALARM", "WATER ALARM") + "</b><br><small>" + t("3x gegossen, aber Erde wird nicht feuchter! Schlauch ab? Sensor verrutscht?", "Watered 3x, but soil is not getting wetter! Hose disconnected? Sensor moved?") + "</small>";
      } else {
     html += "#e74c3c;'><b style='color:#e74c3c;'>⚠️ " + t("SENSOR FEHLER", "SENSOR ERROR") + "</b><br><small>" + t("Sensor meldet unter 3%. Gießen blockiert! Bitte prüfen und neustarten.", "Sensor reports under 3%. Watering blocked! Please check and restart.") + "</small>";
      }
      } else if (systemPausiert) { 
      // --- NEU: WENN PAUSE GEDRÜCKT IST ---
      // Das überschreibt den Countdown und zeigt direkt die Pause an!
      html += "#7f8c8d;'><b style='color:#7f8c8d;'>⏸️ " + t("SYSTEM PAUSIERT", "SYSTEM PAUSED") + "</b><br><small>" + t("Gießen manuell blockiert.", "Watering manually blocked.") + "</small>";
    } else if (aktuellePhase == 2 && imDeepCheck[i]) {
      int restMin = 30 - minutenInPhase; int stufe = 1; if(restMin <= 20) stufe = 2; if(restMin <= 10) stufe = 3;
      html += "#f39c12;'><b style='color:#f39c12;'>⏳ " + t("Sicherheits-Check ", "Safety Check ") + String(stufe) + "/3</b><br><small>" + t("Pumpe startet in ca. ", "Pump starts in approx. ") + String(restMin) + t(" Minuten", " minutes") + "</small>";
    } else {
       // --- NEU: DIE OPTIMIERTE STATUS-LOGIK FÜR DAS WEB-DASHBOARD ---
       if (aktuelleErde[i] <= (ZIEL_FEUCHTIGKEIT[i] - 5)) {
         html += "#3498db;'><b style='color:#3498db;'>🔍 " + t("Prüfe Messwerte...", "Checking values...") + "</b><br><small>" + t("Erde trocken, warte auf Bestätigung", "Soil dry, awaiting confirmation") + "</small>";
       } else if (aktuelleErde[i] > (ZIEL_FEUCHTIGKEIT[i] + 15)) {
         html += "#2980b9;'><b style='color:#2980b9;'>🌊 " + t("Sehr Nass", "Very Wet") + "</b><br><small>" + t("Erde ist weit über dem Zielwert (+15%)", "Soil is well above target (+15%)") + "</small>";
       } else {
         if (aktuellePhase == 2) {
           html += "#27ae60;'><b style='color:#27ae60;'>🟢 " + t("System überwacht...", "System monitoring...") + "</b><br><small>" + t("Parallel-Check läuft im Hintergrund", "Parallel check running in background") + "</small>";
         } else {
           html += "#27ae60;'><b style='color:#27ae60;'>🟢 " + t("System überwacht...", "System monitoring...") + "</b><br><small>" + t("Alles im optimalen Bereich", "Everything in optimal range") + "</small>";
         }
       }
    }
    
    // Kleiner Trick: Das harte deutsche Wort "Neustart" aus dem Array on-the-fly übersetzen
    String lastWaterText = (letztesGiessen[i] == "Neustart") ? t("Neustart", "System restart") : letztesGiessen[i];
    
    html += "</div><p><small>" + t("Letztes Gießen: ", "Last watered: ") + lastWaterText + "</small></p></div>";
  }
  
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ============================================================
// HELPER 1: RADAR FÜR BELEGTE HARDWARE-PINS
// ============================================================
bool isPinBelegt(int p) {
  if (p == -1) return false;
  if (p == PIN_PUMPE) return true;
  if (ZWEITER_KREISLAUF && p == PIN_PUMPE2) return true;
  for(int i=0; i<ANZAHL_TOEPFE; i++) {
    if (p == PIN_VENTIL[i]) return true;
  }
  return false;
}

// ============================================================
// HELPER: SMART DROPDOWN (Erkennt Pumpen, Ventile & sperrt Sensoren)
// ============================================================
String getPinDropdown(int selectedPin) {
  String opts = "<option value='-1'>" + t("Keines", "None") + "</option>";
  int outPins[] = {2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 23, 25, 26, 27, 32, 33};
  
  for(int i=0; i<17; i++) {
    int pin = outPins[i];
    String info = "";
    bool isSensor = false;

    // 1. Ist es eine Pumpe oder ein Ventil? (Bekommt einen Namen, bleibt aber KLICKBAR!)
    if (pin == PIN_PUMPE) info = " (" + t("Pumpe 1", "Pump 1") + ")";
    else if (ZWEITER_KREISLAUF && pin == PIN_PUMPE2) info = " (" + t("Pumpe 2", "Pump 2") + ")";
    else {
      for(int j=0; j<ANZAHL_TOEPFE; j++) {
        if (pin == PIN_VENTIL[j]) info = " (" + t("Ventil", "Valve") + " " + String(j+1) + ")";
        if (pin == PIN_ERDE[j]) { info = " (" + t("Erdsensor", "Soil Sensor") + ")"; isSensor = true; }
      }
      for(int j=0; j<ANZAHL_DHT; j++) {
        if (pin == PIN_DHT[j]) { info = " (" + t("Klima", "Climate") + ")"; isSensor = true; }
      }
      for(int j=0; j<2; j++) {
        if (pin == extraSensorPin[j]) { info = " (" + extraSensorName[j] + ")"; isSensor = true; }
      }
    }

    String sel = (selectedPin == pin) ? "selected" : "";
    // 2. WICHTIG: Wir sperren NUR die Sensoren, damit nichts kaputt geht!
    String dis = isSensor ? "disabled" : ""; 
    String text = "Pin D" + String(pin) + info;
    opts += "<option value='" + String(pin) + "' " + sel + " " + dis + ">" + text + "</option>";
  }
  
  // 3. PCF-Erweiterungspins
  if (USE_PCF) {
    for(int p=0; p<8; p++) {
      int vPin = 100 + p;
      String info = "";
      // Auch PCF Pins beschriften, falls sie für Ventile genutzt werden
      if (vPin == PIN_PUMPE) info = " (" + t("Pumpe 1", "Pump 1") + ")";
      else if (ZWEITER_KREISLAUF && vPin == PIN_PUMPE2) info = " (" + t("Pumpe 2", "Pump 2") + ")";
      else {
        for(int j=0; j<ANZAHL_TOEPFE; j++) {
          if (vPin == PIN_VENTIL[j]) info = " (" + t("Ventil", "Valve") + " " + String(j+1) + ")";
        }
      }
      String sel = (selectedPin == vPin) ? "selected" : "";
      opts += "<option value='" + String(vPin) + "' " + sel + ">PCF P" + String(p) + info + "</option>";
    }
  }
  // --- NEU: SYSTEM BEFEHLE FÜR DAS SIEM ---
  String selSys = (selectedPin == 999) ? "selected" : "";
  opts += "<option value='999' " + selSys + " style='color:red; font-weight:bold;'>🛑 " + t("SYSTEM: Alle Pumpen sperren", "SYSTEM: Block all Pumps") + "</option>";
  
  return opts;
}

// ============================================================
// HELPER 3: TRIGGER DROPDOWN (Die vermisste Funktion!)
// ============================================================
String getTriggerDropdown(int selectedTrig) {
  String opts = "<option value='0'>" + t("--- Auswählen ---", "--- Select ---") + "</option>";
  // 1. Die Klima-Sensoren (IDs 1-3 = Temp, 4-6 = Luftfeuchte)
  for(int i=0; i<ANZAHL_DHT; i++) {
    opts += "<option value='" + String(1 + i) + "' " + (selectedTrig == 1+i ? "selected":"") + ">" + t("Temp ", "Temp ") + String(i+1) + "</option>";
    opts += "<option value='" + String(4 + i) + "' " + (selectedTrig == 4+i ? "selected":"") + ">" + t("Luftfeuchte ", "Humidity ") + String(i+1) + "</option>";
  }
  // 2. Die Erd-Sensoren (IDs 10-15)
  for(int i=0; i<ANZAHL_TOEPFE; i++) {
    opts += "<option value='" + String(10 + i) + "' " + (selectedTrig == 10+i ? "selected":"") + ">" + NAME_TOPF[i] + " (" + t("Erde", "Soil") + ")</option>";
  }
  // 3. Deine neuen "Freien Sensoren" / Regensensor etc. (IDs 20-21)
  for(int i=0; i<2; i++) {
    if(extraSensorPin[i] != -1) {
      opts += "<option value='" + String(20 + i) + "' " + (selectedTrig == 20+i ? "selected":"") + ">" + extraSensorName[i] + "</option>";
    }
  }
  // 4. NEU: Die virtuellen MQTT-Sensoren (IDs 30 & 31)
  if (USE_MQTT) {
    opts += "<option value='30' " + String(selectedTrig == 30 ? "selected":"") + ">🌐 MQTT " + t("Wert 1", "Value 1") + "</option>";
    opts += "<option value='31' " + String(selectedTrig == 31 ? "selected":"") + ">🌐 MQTT " + t("Wert 2", "Value 2") + "</option>";
  }
  return opts;
}

// ============================================================
// 1. SYSTEM SETTINGS SEITE (NETZWERK, ARCHITEKTUR, BACKUP)
// ============================================================
void handleSysSettings() {

  // --- DER TÜRSTEHER ---
  if (USE_AUTH && !server.authenticate(authUser.c_str(), authPass.c_str())) {
    return server.requestAuthentication();
  }

  // --- RAM OPTIMIERUNG ---
  String html;
  html.reserve(12000); 
  html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += getNavbar();


  html += "<title>System Setup</title>";
  html += "<style>body{font-family:'Segoe UI', Tahoma, Verdana, sans-serif; background-color:#eef2f3; padding:15px; line-height:1.6;} ";
  html += ".card{background:#fff; padding:20px; border-radius:10px; margin-bottom:20px; box-shadow:0 4px 6px rgba(0,0,0,0.05);} ";
  html += "h1, h2, h3{color:#2c3e50; margin-top:0;} ";
  html += "input, select{width:100%; padding:10px; margin:5px 0 15px 0; border:1px solid #ccc; border-radius:5px; box-sizing:border-box;} ";
  html += "input[type=submit]{background:#2c3e50; color:white; font-size:18px; font-weight:bold; cursor:pointer; padding:15px; border:none; border-radius:8px;} ";
  html += ".toggle-label{display:flex; justify-content:space-between; align-items:center; font-weight:bold; margin-bottom:10px;} ";
  html += "input[type=checkbox]{width:25px; height:25px; margin:0;} ";
  html += ".btn-back{background-color:#7f8c8d; color:white; padding:12px; text-decoration:none; border-radius:8px; font-weight:bold; display:block; text-align:center; margin-bottom:20px;}";
  html += "option:disabled { background-color: #ecf0f1; color: #e74c3c; font-weight:bold; } ";
  html += "</style>";

  html += "<script>";
  html += "function updateUI() {";
  html += "  var anzDht = document.getElementById('anzDht').value;";
  html += "  for(var i=1; i<=3; i++) {";
  html += "    var dbox = document.getElementById('dht_box_' + i);";
  html += "    if(dbox) dbox.style.display = (i <= anzDht) ? 'block' : 'none';";
  html += "  }";
  html += "  document.getElementById('kreis2_box').style.display = document.getElementById('zweiKreis').checked ? 'block' : 'none';";
  html += "  document.getElementById('now_box').style.display = document.getElementById('useNow').checked ? 'block' : 'none';";
  html += "  updatePins();"; // Pin-Check triggern!
  html += "}";

  // --- DER CROSS-PAGE JAVASCRIPT HACK ---
  html += "function updatePins() {";
  html += "  var selects = document.querySelectorAll('.pin-select');";
  html += "  var otherPins = [27, 5"; // Feste System-Pins (Pumpe 1 & Schwimmer 1)
  // Wir holen uns die Pins von der PFLANZEN-Seite aus dem C++ Speicher
  for(int i=0; i<ANZAHL_TOEPFE; i++) { html += "," + String(PIN_ERDE[i]) + "," + String(PIN_VENTIL[i]); }
  html += "  ];";
  html += "  var used = [...otherPins];";
  html += "  selects.forEach(s => { var p = s.closest('div'); if(p && p.style.display !== 'none' && s.value !== '-1') used.push(parseInt(s.value)); });";
  html += "  selects.forEach(s => {";
  html += "    Array.from(s.options).forEach(opt => {";
  html += "      var v = parseInt(opt.value); if(isNaN(v) || v === -1) return;";
  html += "      var isUsed = used.includes(v) && v !== parseInt(s.value);";
  html += "      opt.disabled = isUsed;";
  html += "      if(isUsed && !opt.text.includes('⛔')) opt.text = '⛔ ' + opt.text;";
  html += "      if(!isUsed) opt.text = opt.text.replace('⛔ ', '');";
  html += "    });";
  html += "  });";
  html += "}";
  html += "window.onload = function() { updateUI(); };"; 
  html += "</script></head><body>";
  
  html += "<a href='/' class='btn-back'>🔙 " + t("Zurück zum Dashboard", "Back to Dashboard") + "</a>";
  html += "<h2 style='text-align:center; color:#2c3e50;'>⚙️ " + t("System & Hardware", "System & Hardware") + "</h2>";
  html += "<form action='/save_sys' method='POST'>";

  // --- ZEITZONEN & NETZWERK (Vollständig!) ---
  html += "<div class='card' style='border: 2px solid #34495e;'><h3 style='color:#34495e; margin-top:0;'>🌍 " + t("Lokale Zeitzone", "Local Timezone") + "</h3>";
  html += "<select name='tz'>";
  html += "<option value='CET-1CEST,M3.5.0,M10.5.0/3' " + String(timeZone == "CET-1CEST,M3.5.0,M10.5.0/3" ? "selected" : "") + ">" + t("Europa / Berlin, Wien (CET)", "Europe / Berlin, Vienna (CET)") + "</option>";
  html += "<option value='EET-2EEST,M3.5.0/3,M10.5.0/4' " + String(timeZone == "EET-2EEST,M3.5.0/3,M10.5.0/4" ? "selected" : "") + ">" + t("Europa / Helsinki, Athen (EET)", "Europe / Helsinki, Athens (EET)") + "</option>";
  html += "<option value='GMT0BST,M3.5.0/1,M10.5.0' " + String(timeZone == "GMT0BST,M3.5.0/1,M10.5.0" ? "selected" : "") + ">" + t("Europa / London (GMT)", "Europe / London (GMT)") + "</option>";
  html += "<option value='EST5EDT,M3.2.0,M11.1.0' " + String(timeZone == "EST5EDT,M3.2.0,M11.1.0" ? "selected" : "") + ">" + t("USA / New York (EST)", "USA / New York (EST)") + "</option>";
  html += "<option value='CST6CDT,M3.2.0,M11.1.0' " + String(timeZone == "CST6CDT,M3.2.0,M11.1.0" ? "selected" : "") + ">" + t("USA / Chicago (CST)", "USA / Chicago (CST)") + "</option>";
  html += "<option value='MST7MDT,M3.2.0,M11.1.0' " + String(timeZone == "MST7MDT,M3.2.0,M11.1.0" ? "selected" : "") + ">" + t("USA / Denver (MST)", "USA / Denver (MST)") + "</option>";
  html += "<option value='PST8PDT,M3.2.0,M11.1.0' " + String(timeZone == "PST8PDT,M3.2.0,M11.1.0" ? "selected" : "") + ">" + t("USA / Los Angeles (PST)", "USA / Los Angeles (PST)") + "</option>";
  html += "<option value='BRT3' " + String(timeZone == "BRT3" ? "selected" : "") + ">" + t("Brasilien / São Paulo (BRT)", "Brazil / São Paulo (BRT)") + "</option>";
  html += "<option value='SAST-2' " + String(timeZone == "SAST-2" ? "selected" : "") + ">" + t("Afrika / Johannesburg (SAST)", "Africa / Johannesburg (SAST)") + "</option>";
  html += "<option value='GST-4' " + String(timeZone == "GST-4" ? "selected" : "") + ">" + t("Dubai / Abu Dhabi (GST)", "Dubai / Abu Dhabi (GST)") + "</option>";
  html += "<option value='IST-5:30' " + String(timeZone == "IST-5:30" ? "selected" : "") + ">" + t("Asien / Indien (IST)", "Asia / India (IST)") + "</option>";
  html += "<option value='ICT-7' " + String(timeZone == "ICT-7" ? "selected" : "") + ">" + t("Asien / Bangkok (ICT)", "Asia / Bangkok (ICT)") + "</option>";
  html += "<option value='SGT-8' " + String(timeZone == "SGT-8" ? "selected" : "") + ">" + t("Asien / Singapur (SGT)", "Asia / Singapore (SGT)") + "</option>";
  html += "<option value='CST-8' " + String(timeZone == "CST-8" ? "selected" : "") + ">" + t("Asien / Peking, Shanghai (CST)", "Asia / Beijing, Shanghai (CST)") + "</option>";
  html += "<option value='JST-9' " + String(timeZone == "JST-9" ? "selected" : "") + ">" + t("Asien / Tokio (JST)", "Asia / Tokyo (JST)") + "</option>";
  html += "<option value='AEST-10AEDT,M10.1.0,M4.1.0/3' " + String(timeZone == "AEST-10AEDT,M10.1.0,M4.1.0/3" ? "selected" : "") + ">" + t("Australien / Sydney (AEST)", "Australia / Sydney (AEST)") + "</option>";
  html += "<option value='NZST-12NZDT,M9.5.0,M4.1.0/3' " + String(timeZone == "NZST-12NZDT,M9.5.0,M4.1.0/3" ? "selected" : "") + ">" + t("Neuseeland / Auckland (NZST)", "New Zealand / Auckland (NZST)") + "</option>";
  html += "</select>";
  
  html += "<label>" + t("System-Name (mDNS):", "System Name (mDNS):") + "</label>";
  html += "<input type='text' name='sysName' value='" + systemName + "' maxlength='20'></div>";

// --- SICHERHEIT (AUTH) ---
  html += "<div class='card' style='border: 2px solid #e74c3c;'><h3 style='color:#e74c3c; margin-top:0;'>🔒 " + t("Sicherheit", "Security") + "</h3>";
  html += "<label class='toggle-label'><b>" + t("Passwort-Schutz aktivieren?", "Enable Password Protection?") + "</b> <input type='checkbox' name='useAuth' value='1' " + String(USE_AUTH ? "checked" : "") + "></label>";
  html += "<label>" + t("Benutzername:", "Username:") + "</label><input type='text' name='authUser' value='" + authUser + "'>";
  html += "<label>" + t("Passwort:", "Password:") + "</label>";
  
  // DAS NEUE PASSWORT-FELD MIT AUGEN-BUTTON
  html += "<div style='display:flex; align-items:center; gap:10px;'>";
  html += "<input type='password' id='authPass' name='authPass' value='" + authPass + "' style='margin:0; flex-grow:1;'>";
  html += "<button type='button' onclick='let p=document.getElementById(\"authPass\"); p.type=(p.type===\"password\")?\"text\":\"password\"; this.innerText=(p.type===\"password\")?\"👁️\":\"🙈\";' style='padding:10px 15px; border-radius:5px; border:1px solid #ccc; background:#fff; cursor:pointer; font-size:18px;'>👁️</button>";
  html += "</div></div>";

  // --- MQTT SETTINGS (HomeAssistant) ---
  html += "<div class='card' style='border: 2px solid #16a085;'><h3 style='color:#16a085; margin-top:0;'>📡 " + t("MQTT / HomeAssistant", "MQTT / HomeAssistant") + "</h3>";
  html += "<label class='toggle-label'><b>" + t("MQTT aktivieren?", "Enable MQTT?") + "</b> <input type='checkbox' name='useMqtt' value='1' " + String(USE_MQTT ? "checked" : "") + "></label>";
  html += "<div style='display:flex; gap:10px;'>";
  html += "<div style='flex:3;'><label>" + t("Server IP:", "Server IP:") + "</label><input type='text' name='mqttServer' value='" + mqttServer + "' placeholder='192.168.1.100'></div>";
  html += "<div style='flex:1;'><label>" + t("Port:", "Port:") + "</label><input type='number' name='mqttPort' value='" + String(mqttPort) + "'></div>";
  html += "</div>";
  html += "<div style='display:flex; gap:10px;'>";
  html += "<div style='flex:1;'><label>" + t("Benutzer:", "User:") + "</label><input type='text' name='mqttUser' value='" + mqttUser + "'></div>";
  html += "<div style='flex:1;'><label>" + t("Passwort:", "Password:") + "</label><input type='password' name='mqttPass' value='" + mqttPass + "'></div>";
  html += "</div>";
  html += "<label>" + t("Basis-Topic:", "Base Topic:") + "</label><input type='text' name='mqttTopic' value='" + mqttTopic + "' placeholder='kraeuterwg'>";
  html += "<p style='font-size:12px; color:#7f8c8d; margin-top:-5px;'><i>" + t("Verbindet das System mit HomeAssistant oder Node-RED für Graphen und externe Smart Rules Auslöser.", "Connects the system to HomeAssistant or Node-RED for graphs and external Smart Rules triggers.") + "</i></p></div>";
 
 // --- HAUPT-PINS (DAU-SICHER) ---
  html += "<div class='card' style='border: 2px solid #3498db;'><h3 style='color:#3498db; margin-top:0;'>💧 " + t("Haupt-Wasserversorgung (Kreislauf 1)", "Main Water Supply (Circuit 1)") + "</h3>";
  
  // Pumpe 1 (Alle normalen Pins + PCF)
  html += "<label>" + t("Pin Haupt-Pumpe 1:", "Pin Main Pump 1:") + "</label><select class='pin-select' onchange='updatePins()' name='pinP1'>";
  for(int j=0; j<12; j++) { String sel = (PIN_PUMPE == SAFE_OUT_PINS[j]) ? "selected" : ""; html += "<option value='" + String(SAFE_OUT_PINS[j]) + "' " + sel + ">Pin D" + String(SAFE_OUT_PINS[j]) + "</option>"; }
  if (USE_PCF) { for(int p=0; p<8; p++) { int vPin = 100 + p; String sel = (PIN_PUMPE == vPin) ? "selected" : ""; html += "<option value='" + String(vPin) + "' " + sel + ">PCF P" + String(p) + "</option>"; } }
  html += "</select>";
  
  // Schwimmer 1 (NUR noch normale Pins mit internem Pull-Up!)
  html += "<label>" + t("Pin Wassertank (Schwimmer):", "Pin Water Tank (Float):") + "</label><select class='pin-select' onchange='updatePins()' name='pinS1'>";
  html += "<option value='-1' " + String(PIN_SCHWIMMER == -1 ? "selected" : "") + ">" + t("Keiner (Festwasser)", "None (Direct Line)") + "</option>";
  for(int j=0; j<12; j++) { String sel = (PIN_SCHWIMMER == SAFE_OUT_PINS[j]) ? "selected" : ""; html += "<option value='" + String(SAFE_OUT_PINS[j]) + "' " + sel + ">Pin D" + String(SAFE_OUT_PINS[j]) + "</option>"; }
  html += "</select></div>";

  html += "<div class='card' style='border: 2px solid #8e44ad;'><h3 style='color:#8e44ad; margin-top:0;'>🏗️ " + t("Architektur", "Architecture") + "</h3>";
  html += "<label>" + t("Anzahl der Töpfe (1-6):", "Number of pots (1-6):") + "</label><select name='anzToepfe'>";
  for(int j=1; j<=6; j++) { String sel = (ANZAHL_TOEPFE == j) ? "selected" : ""; html += "<option value='" + String(j) + "' " + sel + ">" + String(j) + t(" Töpfe", " Pots") + "</option>"; }
  html += "</select>";
  
  html += "<label>" + t("Anzahl Klima-Sensoren (0-3):", "Number of Climate Sensors (0-3):") + "</label><select name='anzDht' id='anzDht' onchange='updateUI()'>";
  for(int j=0; j<=3; j++) { String sel = (ANZAHL_DHT == j) ? "selected" : ""; html += "<option value='" + String(j) + "' " + sel + ">" + String(j) + t(" Sensoren", " Sensors") + "</option>"; }
  html += "</select>";

  for(int i=0; i<3; i++) {
    html += "<div id='dht_box_" + String(i+1) + "' style='margin-bottom:10px; background:#f9ebea; padding:10px; border-radius:5px;'>";
    html += "<label>Pin Klima-Zone " + String(i+1) + ":</label><select class='pin-select' onchange='updatePins()' name='pinDht" + String(i) + "'>";
    for(int j=0; j<12; j++) { String sel = (PIN_DHT[i] == SAFE_OUT_PINS[j]) ? "selected" : ""; html += "<option value='" + String(SAFE_OUT_PINS[j]) + "' " + sel + ">Pin D" + String(SAFE_OUT_PINS[j]) + "</option>"; }
    html += "</select></div>";
  }

  html += "<label class='toggle-label'>" + t("Extra Kreislauf?", "Extra Circuit?") + " <input type='checkbox' id='zweiKreis' name='zweiKreis' value='1' onchange='updateUI()' " + String(ZWEITER_KREISLAUF ? "checked" : "") + "></label>";
  html += "<label class='toggle-label'>" + t("OLED Display?", "OLED Display?") + " <input type='checkbox' name='useDisp' value='1' " + String(USE_DISPLAY ? "checked" : "") + "></label>";
  html += "<label class='toggle-label'>" + t("PCF8574 (I2C Expander)?", "PCF8574 (I2C Expander)?") + " <input type='checkbox' id='usePcf' name='usePcf' value='1' onchange='updateUI()' " + String(USE_PCF ? "checked" : "") + "></label>";
  html += "<label class='toggle-label'><b>" + t("ESP-NOW Funk aktivieren?", "Enable ESP-NOW Wireless?") + "</b> <input type='checkbox' id='useNow' name='useNow' value='1' onchange='updateUI()' " + String(ESP_NOW_ACTIVE ? "checked" : "") + "></label>";
  
  html += "<div id='now_box' style='background:#fcf3cf; padding:15px; border-radius:8px; border: 1px dashed #f39c12; margin-top:10px; display:" + String(ESP_NOW_ACTIVE ? "block" : "none") + ";'>";
  html += "<label>" + t("MAC-Adresse(n) der Displays:", "MAC Address(es) of Displays:") + "</label>";
  html += "<input type='text' name='dispmac' value='" + DISPLAY_MAC + "' placeholder='AA:BB:CC:11:22:33; FF:EE...'>";
  html += "<p style='margin-top:2px; font-size:12px; color:#d35400;'><i>" + t("💡 Mehrere Displays? Einfach mit einem Strichpunkt ( ; ) trennen! (Max. 3 Displays)", "💡 Multiple displays? Just separate them with a semicolon ( ; )! (Max. 3 displays)") + "</i></p>";
  html += "<a href='/hub-code' target='_blank' style='background:#f39c12; color:white; padding:8px 12px; text-decoration:none; border-radius:5px; display:inline-block; font-size:13px; font-weight:bold; margin-top:5px;'>📄 " + t("Display-Code generieren", "Generate Display Code") + "</a>";
  html += "</div>";


  // --- KREISLAUF 2 ---
  html += "<div id='kreis2_box' style='background:#f0f3f4; padding:15px; border-radius:8px; margin-top:15px; border-left: 4px solid #3498db;'>";
  html += "<h4>💧 " + t("Pins für Kreislauf 2", "Pins for Circuit 2") + "</h4>";
  
  html += "<label>" + t("Pin 2. Pumpe:", "Pin 2nd Pump:") + "</label><select class='pin-select' onchange='updatePins()' name='pinP2'>";
  for(int j=0; j<12; j++) { String sel = (PIN_PUMPE2 == SAFE_OUT_PINS[j]) ? "selected" : ""; html += "<option value='" + String(SAFE_OUT_PINS[j]) + "' " + sel + ">Pin D" + String(SAFE_OUT_PINS[j]) + "</option>"; }
  if (USE_PCF) {
    for(int p=0; p<8; p++) {
      int vPin = 100 + p;
      String sel = (PIN_PUMPE2 == vPin) ? "selected" : "";
      html += "<option value='" + String(vPin) + "' " + sel + ">PCF P" + String(p) + "</option>";
    }
  }
  html += "</select>";
  
  // SCHWIMMER 2 (NUR NOCH SAFE_OUT_PINS)
  html += "<label>" + t("Pin 2. Tank-Sensor:", "Pin 2nd Tank Sensor:") + "</label><select class='pin-select' onchange='updatePins()' name='pinS2'>";
  html += "<option value='-1' " + String(PIN_SCHWIMMER2 == -1 ? "selected" : "") + ">" + t("Keiner (Festwasser)", "None (Direct Line)") + "</option>";
  for(int j=0; j<12; j++) { String sel = (PIN_SCHWIMMER2 == SAFE_OUT_PINS[j]) ? "selected" : ""; html += "<option value='" + String(SAFE_OUT_PINS[j]) + "' " + sel + ">Pin D" + String(SAFE_OUT_PINS[j]) + "</option>"; }
  html += "</select></div></div>";

  html += "<div class='card' style='border: 2px solid #8e44ad;'><h3 style='color:#8e44ad; margin-top:0;'>🔌 " + t("Freie Sensoren", "Custom Sensors") + "</h3>";
  for(int i=0; i<2; i++) {
    html += "<input type='text' name='esName" + String(i) + "' value='" + extraSensorName[i] + "' maxlength='15'>";
    html += "<select class='pin-select' onchange='updatePins()' name='esPin" + String(i) + "'><option value='-1'>" + t("Deaktiviert", "Disabled") + "</option>";
    int anaPins[] = {32, 33, 34, 35, 36, 39}; 
    for(int j=0; j<6; j++) { String sel = (extraSensorPin[i] == anaPins[j]) ? "selected" : ""; html += "<option value='" + String(anaPins[j]) + "' " + sel + ">Pin D" + String(anaPins[j]) + "</option>"; }
    html += "</select>";
  }
  html += "</div>";

  html += "<input type='submit' value='💾 " + t("System Speichern & Neustart", "Save System & Restart") + "'></form>";

  // --- BACKUP & RESTORE CARD ---
  html += "<div class='card' style='border: 2px solid #27ae60; margin-top:20px; text-align:center;'>";
  html += "<h3 style='color:#27ae60; margin-top:0;'>💾 Backup & Restore</h3>";
  html += "<a href='/backup' style='background:#27ae60; color:white; padding:12px 20px; border-radius:8px; text-decoration:none; display:inline-block; margin-bottom:20px;'>⬇️ Config (.json)</a><hr>";
  html += "<form method='POST' action='/restore' id='restoreForm'>";
  html += "<input type='file' id='fileInput' accept='.json' style='margin-bottom:10px;' required><br>";
  html += "<input type='hidden' name='configData' id='configData'>";
  html += "<button type='button' onclick='uploadConfig()' style='background:#2980b9; color:white; font-size:16px; font-weight:bold; cursor:pointer; padding:12px 20px; border:none; border-radius:8px;'>⬆️ Upload Config</button>";
  html += "</form>";
  html += "<script>function uploadConfig(){ let f = document.getElementById('fileInput').files[0]; if(!f){ return; } let r = new FileReader(); r.onload = function(e){ document.getElementById('configData').value = e.target.result; document.getElementById('restoreForm').submit(); }; r.readAsText(f); }</script></div>";

  // --- UPDATE CARDS ---
  html += "<div class='card' style='border: 2px solid #2980b9; text-align:center;'><h3 style='color:#2980b9; margin-top:0;'>🚀 System Update</h3>";
  html += "<a href='/update' style='background:#2980b9; color:white; padding:12px 20px; border-radius:8px; font-weight:bold; text-decoration:none; display:inline-block;'>🔄 Firmware Update</a></div>";
  
  // --- NEU: DER DEZENTE ADMIN-BUTTON ---
  html += "<a href='/diagnose' style='background-color:#34495e; color:white; padding:15px; text-decoration:none; border-radius:8px; font-weight:bold; display:block; text-align:center; margin-bottom:20px;'>🩺 " + t("System-Diagnose & Logs", "System Diagnostics & Logs") + "</a>";

  // --- RESET CARDS (JETZT ALS SICHERES POST-FORMULAR) ---
  String confirmMsg = t("Bist du absolut sicher? ALLE Regeln, WLAN und Kalibrierungen werden gelöscht!", "Are you absolutely sure? ALL rules, WiFi and calibrations will be deleted!");
  html += "<div class='card' style='border: 2px solid #c0392b; text-align:center;'><h3 style='color:#c0392b; margin-top:0;'>⚠️ " + t("Gefahrenzone", "Danger Zone") + "</h3>";
  
  // Das neue POST-Formular, an dem Nmap abprallt!
  html += "<form action='/factory_reset' method='POST' onsubmit='return confirm(\"" + confirmMsg + "\");' style='margin:0;'>";
  html += "<button type='submit' style='background:#c0392b; color:white; padding:12px 20px; border-radius:8px; font-weight:bold; border:none; cursor:pointer;'>💥 " + t("Werkseinstellungen (Reset)", "Factory Reset") + "</button>";
  html += "</form></div>";
  
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ------------------------------------------------------------
// SAVE SYSTEM
// ------------------------------------------------------------
void handleSaveSys() {
  preferences.begin("kraeuter", false);
  preferences.putString("tz", server.arg("tz"));
  preferences.putString("sysName", server.arg("sysName"));
  preferences.putInt("anzToepfe", server.arg("anzToepfe").toInt());
  preferences.putBool("zweiKreis", server.hasArg("zweiKreis")); 
  preferences.putBool("useDisp", server.hasArg("useDisp"));
  preferences.putBool("usePcf", server.hasArg("usePcf"));
  preferences.putBool("useNow", server.hasArg("useNow"));
  preferences.putString("dispmac", server.arg("dispmac"));
  preferences.putInt("pinP2", server.arg("pinP2").toInt());
  preferences.putInt("pinS2", server.arg("pinS2").toInt());
  preferences.putInt("anzDht", server.arg("anzDht").toInt());
  preferences.putBool("useAuth", server.hasArg("useAuth"));
  preferences.putString("authUser", server.arg("authUser"));
  preferences.putString("authPass", server.arg("authPass"));
  preferences.putInt("pinP1", server.arg("pinP1").toInt());
  preferences.putInt("pinS1", server.arg("pinS1").toInt());

  preferences.putBool("useMqtt", server.hasArg("useMqtt"));
  preferences.putString("mqSrv", server.arg("mqttServer"));
  preferences.putInt("mqPrt", server.arg("mqttPort").toInt());
  preferences.putString("mqUsr", server.arg("mqttUser"));
  preferences.putString("mqPwd", server.arg("mqttPass"));
  preferences.putString("mqTop", server.arg("mqttTopic"));

  for(int i=0; i<3; i++) { preferences.putInt(("pinDht"+String(i)).c_str(), server.arg("pinDht"+String(i)).toInt()); }
  for(int i=0; i<2; i++) {
    preferences.putString(("esName"+String(i)).c_str(), server.arg("esName"+String(i)));
    preferences.putInt(("esPin"+String(i)).c_str(), server.arg("esPin"+String(i)).toInt());
  }
  preferences.end();
  
  String msg = t("System gespeichert! Neustart...", "System saved! Restarting...");
  String html = "<html><meta http-equiv='refresh' content='5; url=/'><body style='text-align:center; padding-top:50px;'><h2 style='color:#27ae60;'>" + msg + "</h2></body></html>";
  server.send(200, "text/html", html);
  delay(1000); ESP.restart();
}

// ============================================================
// 2. PFLANZEN SETTINGS SEITE (MIT LITTLEFS & DYNAMISCHEM FILTER!)
// ============================================================
void handlePlantSettings() {

  if (USE_AUTH && !server.authenticate(authUser.c_str(), authPass.c_str())) {
    return server.requestAuthentication();
  }

  String html;
  html.reserve(10000); 
  html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += getNavbar();

  html += "<title>Pflanzen Setup</title>";
  html += "<style>body{font-family:'Segoe UI', Tahoma, Verdana, sans-serif; background-color:#eef2f3; padding:15px; line-height:1.6;} ";
  html += ".card{background:#fff; padding:20px; border-radius:10px; margin-bottom:20px; box-shadow:0 4px 6px rgba(0,0,0,0.05);} ";
  html += "h1, h2, h3{color:#2c3e50; margin-top:0;} ";
  html += "input, select{width:100%; padding:10px; margin:5px 0 15px 0; border:1px solid #ccc; border-radius:5px; box-sizing:border-box;} ";
  html += "input[type=submit]{background:#27ae60; color:white; font-size:18px; font-weight:bold; cursor:pointer; padding:15px; border:none; border-radius:8px;} ";
  html += ".btn-back{background-color:#7f8c8d; color:white; padding:12px; text-decoration:none; border-radius:8px; font-weight:bold; display:block; text-align:center; margin-bottom:20px;}";
  html += ".chk-label{display:block; margin:8px 0; font-size:16px;} input[type=checkbox]{width:20px; height:20px; margin-right:10px;}";
  html += "option:disabled { background-color: #ecf0f1; color: #e74c3c; font-weight:bold; } ";
  html += "</style>";

  html += "<script>";
  html += "function updatePins() {";
  html += "  var selects = document.querySelectorAll('.pin-select');";
  html += "  var otherPins = [27, 5"; 
  if(ZWEITER_KREISLAUF) html += "," + String(PIN_PUMPE2) + "," + String(PIN_SCHWIMMER2);
  for(int i=0; i<ANZAHL_DHT; i++) html += "," + String(PIN_DHT[i]);
  for(int i=0; i<2; i++) if(extraSensorPin[i] != -1) html += "," + String(extraSensorPin[i]);
  html += "  ];";
  html += "  var used = [...otherPins];";
  html += "  selects.forEach(s => { if(s.value !== '-1') used.push(parseInt(s.value)); });";
  html += "  selects.forEach(s => {";
  html += "    Array.from(s.options).forEach(opt => {";
  html += "      var v = parseInt(opt.value); if(isNaN(v) || v === -1) return;";
  html += "      var isUsed = used.includes(v) && v !== parseInt(s.value);";
  html += "      opt.disabled = isUsed;";
  html += "      if(isUsed && !opt.text.includes('⛔')) opt.text = '⛔ ' + opt.text.replace('⛔ ', '');";
  html += "      if(!isUsed) opt.text = opt.text.replace('⛔ ', '');";
  html += "    });";
  html += "  });";
  html += "}";
  html += "window.onload = function() { updatePins(); };";
  html += "</script>";

  // 🚨 DATENBANK AUS DEM FLASH LADEN 🚨
  html += "<script src='/plants.js'></script>";

  html += "<script>";
  html += "const isEn = " + String(isEnglish ? "true" : "false") + ";";

  html += "function addPlantSlot(potId) {";
  html += "  let cont = document.getElementById('eco_zone_' + potId);";
  html += "  if(cont.children.length >= 10) { alert('" + t("Maximal 10 Pflanzen pro Zone!", "Max 10 plants per zone!") + "'); return; }"; 
  
  html += "  let wrapper = document.createElement('div'); wrapper.style.marginBottom='10px'; wrapper.style.borderBottom='1px dashed #bdc3c7'; wrapper.style.paddingBottom='5px';";
  
  html += "  let topRow = document.createElement('div'); topRow.style.display='flex'; topRow.style.gap='5px'; topRow.style.marginBottom='5px';";
  
  html += "  let catSel = document.createElement('select'); catSel.style.flex='1'; catSel.style.margin='0';";
  html += "  catSel.options.add(new Option('" + t("Alle Kategorien", "All Categories") + "', 'all'));";
  
  html += "  for(let cat in plantDB) {";
  html += "    let dispCat = (isEn && cat.includes(' / ')) ? cat.split(' ')[0] + ' ' + cat.split(' / ')[1] : cat.split(' / ')[0];";
  html += "    catSel.options.add(new Option(dispCat, cat));";
  html += "  }";
  
  html += "  let searchInp = document.createElement('input'); searchInp.type='text'; searchInp.style.flex='1'; searchInp.style.margin='0';";
  html += "  searchInp.placeholder = '🔎 " + t("Suche (Asien, Tomate...)", "Search (Asia, Tomato...)") + "';";
  html += "  searchInp.setAttribute('onkeydown', 'if(event.keyCode===13){event.preventDefault();return false;}');";
  
  html += "  topRow.appendChild(catSel); topRow.appendChild(searchInp);";
  
  html += "  let pltSel = document.createElement('select'); pltSel.className='plt-sel'; pltSel.style.width='100%'; pltSel.style.margin='0';";
  html += "  let infoDiv = document.createElement('div'); infoDiv.style.fontSize='11.5px'; infoDiv.style.color='#34495e'; infoDiv.style.marginTop='4px'; infoDiv.style.display='none';";
  
  html += "  let updateDropdown = function() {";
  html += "    let filterCat = catSel.value; let query = searchInp.value.toLowerCase(); let oldVal = pltSel.value;";
  html += "    pltSel.innerHTML = '<option value=\"\">" + t("Pflanze wählen...", "Select Plant...") + "</option>';";
  html += "    for(let cat in plantDB) {";
  html += "      if(filterCat !== 'all' && cat !== filterCat) continue;"; 
  
  html += "      let dispCat = (isEn && cat.includes(' / ')) ? cat.split(' ')[0] + ' ' + cat.split(' / ')[1] : cat.split(' / ')[0];";
  html += "      let group = document.createElement('optgroup'); group.label = dispCat;";
  
  html += "      for(let plt in plantDB[cat]) {";
  html += "        let data = plantDB[cat][plt];";
  html += "        if(plt.toLowerCase().includes(query) || cat.toLowerCase().includes(query) || data.i.toLowerCase().includes(query)) {";
  
  // ✂️ HIER IST DER NEUE SNIPER FÜR DIE PFLANZEN-NAMEN (BILINGUAL!) ✂️
  html += "          let dispPlt = plt;";
  html += "          if(plt.includes(' / ')) { dispPlt = isEn ? plt.split(' / ')[1].trim() : plt.split(' / ')[0].trim(); }";
  
  html += "          let opt = new Option(dispPlt + ' (' + data.z + '%)', cat + '|' + plt);";
  html += "          opt.dataset.z = data.z;";
  
  html += "          let rawInfo = data.i; let cleanInfo = rawInfo;";
  html += "          if(rawInfo.includes('| EN:')) {";
  html += "            let parts = rawInfo.split('| EN:');";
  html += "            cleanInfo = isEn ? parts[1].trim() : parts[0].replace('DE:', '').trim();";
  html += "          }";
  html += "          opt.dataset.info = cleanInfo;";
  
  html += "          if(opt.value === oldVal) opt.selected = true;";
  html += "          group.appendChild(opt);";
  html += "        }";
  html += "      }";
  html += "      if(group.children.length > 0) pltSel.appendChild(group);";
  html += "    }";
  html += "    calcZone(potId, false);"; 
  html += "  };";
  
  html += "  catSel.onchange = updateDropdown; searchInp.onkeyup = updateDropdown;";
  
  html += "  pltSel.onchange = function() {";
  html += "    if(pltSel.selectedIndex > 0) { infoDiv.innerHTML = '<b>💡 Info:</b> ' + pltSel.options[pltSel.selectedIndex].dataset.info; infoDiv.style.display = 'block'; }";
  html += "    else { infoDiv.style.display = 'none'; }";
  html += "    calcZone(potId, false);"; 
  html += "  };";
  
  // 🚨 HIER WURDE DER GEISTER-BUG BEHOBEN: Zuerst ins DOM laden, DANN updaten und calcZone rufen! 🚨
  html += "  wrapper.appendChild(topRow); wrapper.appendChild(pltSel); wrapper.appendChild(infoDiv); cont.appendChild(wrapper);";
  html += "  updateDropdown();"; 
  html += "}";

  // Die Berechnungs-Logik
  html += "function calcZone(potId, applyVals) {";
  html += "  let selects = document.getElementById('eco_zone_' + potId).querySelectorAll('.plt-sel');";
  html += "  let sumZ = 0, sumT = 0, sumN = 0, valid = 0; let anchorZ = null;"; 
  html += "  selects.forEach(sel => { if(sel.value) { let parts = sel.value.split('|'); let p = plantDB[parts[0]][parts[1]]; if(p) { if(anchorZ === null) anchorZ = p.z; sumZ += p.z; sumT += p.t; sumN += p.n; valid++; } } });";
  
  html += "  selects.forEach(sel => {";
  html += "    Array.from(sel.options).forEach(opt => {";
  html += "      if(!opt.value) return;";
  html += "      let pZ = parseInt(opt.dataset.z);";
  html += "      if(anchorZ !== null && Math.abs(pZ - anchorZ) > 10) { opt.disabled = true; opt.text = '⛔ ' + opt.text.replace('⛔ ', ''); if(sel.value === opt.value) sel.value = ''; }";
  html += "      else { opt.disabled = false; opt.text = opt.text.replace('⛔ ', ''); }";
  html += "    });";
  html += "  });";
  
  html += "  let warnDiv = document.getElementById('warn_' + potId);";
  html += "  if(valid > 0) {";
  html += "  if(applyVals) { document.getElementsByName('ziel'+potId)[0].value = Math.round(sumZ / valid); warnDiv.innerHTML = '<b style=\"color:#27ae60;\">✅ " + t("Werte übernommen!", "Values applied!") + "</b>'; }";
  html += "    else { warnDiv.innerHTML = '<b style=\"color:#3498db;\">💡 " + t("Klicke auf Smart Calc zum Übernehmen.", "Click Smart Calc to apply.") + "</b>'; }";
  html += "  } else { warnDiv.innerHTML = ''; }";
  html += "}";

  html += "function runMultiTest() { var btn = document.getElementById('testBtn'); var oldText = btn.innerHTML; var oldBg = btn.style.background; btn.innerHTML = '⏳ " + t("Test laeuft...", "Testing...") + "'; btn.style.background = '#f39c12'; var url = '/test?'; if(document.getElementById('test_p1') && document.getElementById('test_p1').checked) url += 'p1=1&'; if(document.getElementById('test_p2') && document.getElementById('test_p2').checked) url += 'p2=1&'; for(var i=0; i<" + String(ANZAHL_TOEPFE) + "; i++) { var cb = document.getElementById('test_v'+i); if(cb && cb.checked) url += 'v'+i+'=1&'; } fetch(url, {method: 'POST'}).then(response => { btn.innerHTML = '✅ " + t("Erfolgreich!", "Success!") + "'; btn.style.background = '#27ae60'; setTimeout(() => { btn.innerHTML = oldText; btn.style.background = oldBg; }, 2000); }); }";
  html += "</script></head><body>";
  
  html += "<a href='/' class='btn-back'>🔙 " + t("Zurück zum Dashboard", "Back to Dashboard") + "</a>";
  html += "<h2 style='text-align:center; color:#27ae60;'>🪴 " + t("Pflanzen & Kalibrierung", "Plants & Calibration") + "</h2>";

  html += "<div class='card' style='border: 2px solid #2ecc71;'><h3 style='color:#2ecc71; margin-top:0;'>🎛️ " + t("Live-Rohwerte", "Live Raw Values") + "</h3><div style='background:#f4f6f7; padding:15px; border-radius:5px; font-family:monospace; font-size:16px;'>";
for(int i=0; i<ANZAHL_TOEPFE; i++) { 
      long sum = 0; 
      for(int j=0; j<10; j++) { sum += analogRead(PIN_ERDE[i]); delay(2); } 
      int avg = sum / 10; 
      html += "<div><b>" + NAME_TOPF[i] + ":</b> " + String(avg) + "</div>"; 
  }
  html += "</div><p style='font-size:13px; line-height:1.5; margin-bottom:0; color:#7f8c8d;'><i>" + t("<b>Tipp zur Kalibrierung:</b> Sensor an der trockenen Luft ablesen = 0%. Sensor im Wasserglas = 100%.<br>Da die Elektronik leicht schwankt, trage bei TROCKEN einfach den <b>höchsten</b> und bei NASS den <b>niedrigsten</b> Wert ein, den du siehst!", "<b>Calibration Tip:</b> Read sensor in dry air = 0%. In a glass of water = 100%.<br>Since electronics fluctuate slightly, simply enter the <b>highest</b> value you see for DRY and the <b>lowest</b> for WET!") + "</i></p></div>";
  
  html += "<form action='/save_plants' method='POST'>";

  for(int i=0; i<ANZAHL_TOEPFE; i++) {
    html += "<div class='card'><h3>🪴 " + NAME_TOPF[i] + "</h3>";
    
    html += "<div style='background:#eaf2f8; padding:15px; border-radius:5px; margin-bottom:15px; border-left:4px solid #3498db;'>";
    html += "<h4 style='margin-top:0; color:#2980b9;'>🌱 " + t("Eco-Zonen Rechner", "Eco-Zone Calculator") + "</h4>";
    html += "<p style='font-size:12px; margin-top:-10px; color:#7f8c8d;'>" + t("Füge Pflanzen hinzu, um den perfekten Wasser-Durchschnitt zu berechnen.", "Add plants to calculate the perfect water average.") + "</p>";
    
    html += "<div id='eco_zone_" + String(i) + "'></div>";
    
    html += "<div style='display:flex; gap:10px; margin-top:5px;'>";
    html += "<button type='button' onclick='addPlantSlot(" + String(i) + ")' style='background:#3498db; color:white; border:none; padding:10px; border-radius:5px; cursor:pointer; font-weight:bold; flex:1;'>+ " + t("Pflanze", "Plant") + "</button>";
    html += "<button type='button' onclick='calcZone(" + String(i) + ", true)' style='background:#f39c12; color:white; border:none; padding:10px; border-radius:5px; cursor:pointer; font-weight:bold; flex:1;'>🪄 " + t("Smart Calc", "Smart Calc") + "</button>";
    html += "</div>";
    html += "<div id='warn_" + String(i) + "' style='font-size:13px; margin-top:10px;'></div>";
    
    html += "<p style='font-size:11px; color:#95a5a6; margin-bottom:0; border-top:1px solid #d6eaf8; padding-top:5px; margin-top:10px;'><i>" + t("Info: Dies ist ein Taschenrechner. Um den Speicher des ESP32 zu schonen, werden beim Speichern nur die errechneten Zahlenwerte (%/Rohwerte) übernommen, nicht die ausgewählten Pflanzennamen.", "Info: This is a calculator. To save memory, only the calculated numbers are saved, not the selected plant names.") + "</i></p>";
    html += "</div>";
    
    html += "<label>" + t("Name:", "Name:") + "</label><input type='text' name='name" + String(i) + "' value='" + NAME_TOPF[i] + "'>";
    html += "<label>" + t("Ziel (%):", "Target (%):") + "</label><input type='number' name='ziel" + String(i) + "' value='" + String(ZIEL_FEUCHTIGKEIT[i]) + "'>";
    html += "<label>" + t("Gieß-Dauer (ms):", "Watering Time (ms):") + "</label><input type='number' name='dauer" + String(i) + "' value='" + String(GIESS_DAUER[i]) + "'>";

    
    // 💡 SMART TUNING ADVISOR

    int currentDauer = GIESS_DAUER[i];
    String advisorHtml = "";

    // Szenario 1: Zu oft nachgegossen (Wert zu niedrig)
    if (fehlversuche[i] >= 2) {
        int empfehlung = currentDauer + (currentDauer / 2); // +50% Regel
        advisorHtml = "<div style='background:#fef9e7; border-left:4px solid #f1c40f; padding:10px; margin:5px 0 15px 0; border-radius:4px; font-size:13px; color:#7f8c8d;'>";
        advisorHtml += "💡 <b>" + t("Tuning-Tipp:", "Tuning Tip:") + "</b> " + t("Dieser Topf brauchte beim letzten Mal ", "This pot needed ") + "<b>" + String(fehlversuche[i]) + t(" Anläufe", " attempts") + "</b>" + t(", um feucht zu werden.<br>", " to get wet.<br>");
        advisorHtml += t("Aktuell eingestellt: ", "Currently set: ") + String(currentDauer) + " ms.<br>";
        advisorHtml += "👉 <i>" + t("Empfehlung: Erhöhe den Wert auf ca. ", "Recommendation: Increase value to approx. ") + "<b>" + String(empfehlung) + " ms</b> (+50%).</i></div>";
    } 
    // Szenario 2: Massiv überschwemmt (Wert zu hoch)
    else if (aktuelleErde[i] > (ZIEL_FEUCHTIGKEIT[i] + 15)) {
        int empfehlung = currentDauer / 2; // -50% Regel
        if (empfehlung < 500) empfehlung = 500; // Schutz: Nicht unter 0.5 Sekunden fallen
        advisorHtml = "<div style='background:#fdedec; border-left:4px solid #e74c3c; padding:10px; margin:5px 0 15px 0; border-radius:4px; font-size:13px; color:#7f8c8d;'>";
        advisorHtml += "⚠️ <b>" + t("Tuning-Tipp:", "Tuning Tip:") + "</b> " + t("Der Topf wurde beim letzten Mal überschwemmt (Aktuell: ", "The pot was flooded last time (Current: ") + String(aktuelleErde[i]) + "%).<br>";
        advisorHtml += t("Aktuell eingestellt: ", "Currently set: ") + String(currentDauer) + " ms.<br>";
        advisorHtml += "👉 <i>" + t("Empfehlung: Reduziere den Wert auf ca. ", "Recommendation: Reduce value to approx. ") + "<b>" + String(empfehlung) + " ms</b> (-50%).</i></div>";
    }

    // Den generierten Tipp ins HTML einfügen
    html += advisorHtml;
    // ==========================================

    if (ZWEITER_KREISLAUF) {
      html += "<label>" + t("Wasserquelle:", "Water Source:") + "</label><select name='pWahl" + String(i) + "'>";
      html += "<option value='0' " + String(PUMPEN_WAHL[i] == 0 ? "selected" : "") + ">" + t("Haupt-Pumpe 1", "Main Pump 1") + "</option>";
      html += "<option value='1' " + String(PUMPEN_WAHL[i] == 1 ? "selected" : "") + ">" + t("Pumpe 2 (Kreislauf 2)", "Pump 2 (Circuit 2)") + "</option>";
      html += "<option value='2' " + String(PUMPEN_WAHL[i] == 2 ? "selected" : "") + ">" + t("Auto-Fallback (1 -> 2)", "Auto-Fallback (1 -> 2)") + "</option>";
      html += "</select>";
    } else {
      html += "<input type='hidden' name='pWahl" + String(i) + "' value='0'>"; 
    }
    
    html += "<div style='background:#fdfefe; border:1px solid #bdc3c7; padding:10px; margin:10px 0; border-radius:5px;'>";
    html += "<label style='color:#e67e22;'>" + t("TROCKEN (0%):", "DRY (0%):") + "</label><input type='number' name='trocken" + String(i) + "' value='" + String(SENSOR_TROCKEN[i]) + "'>";
    html += "<label style='color:#3498db;'>" + t("NASS (100%):", "WET (100%):") + "</label><input type='number' name='nass" + String(i) + "' value='" + String(SENSOR_NASS[i]) + "'>";
    html += "</div>";

    html += "<label>" + t("Erdsensor Pin:", "Soil Sensor Pin:") + "</label><select class='pin-select' onchange='updatePins()' name='pinE" + String(i) + "'>";
    for(int j=0; j<6; j++) { String sel = (PIN_ERDE[i] == SAFE_ADC_PINS[j]) ? "selected" : ""; html += "<option value='" + String(SAFE_ADC_PINS[j]) + "' " + sel + ">Pin D" + String(SAFE_ADC_PINS[j]) + "</option>"; }
    html += "</select><label>" + t("Relais Pin (Ventil):", "Relay Pin (Valve):") + "</label><select class='pin-select' onchange='updatePins()' name='pinV" + String(i) + "'>";
    for(int j=0; j<12; j++) { String sel = (PIN_VENTIL[i] == SAFE_OUT_PINS[j]) ? "selected" : ""; html += "<option value='" + String(SAFE_OUT_PINS[j]) + "' " + sel + ">Pin D" + String(SAFE_OUT_PINS[j]) + "</option>"; }
    if (USE_PCF) {
      for(int p=0; p<8; p++) { int vPin = 100 + p; String sel = (PIN_VENTIL[i] == vPin) ? "selected" : ""; html += "<option value='" + String(vPin) + "' " + sel + ">PCF P" + String(p) + "</option>"; }
    }
    html += "</select></div>";
  }
  
  html += "<input type='submit' value='💾 " + t("Pflanzen Speichern & Neustart", "Save Plants & Restart") + "'></form>";

  html += "<div class='card' style='border: 2px solid #e67e22; margin-top:20px;'><h3 style='color:#e67e22; text-align:center;'>🛠️ " + t("Manueller Hardware-Test", "Manual Hardware Test") + "</h3>";
  html += "<div style='margin-left:10%;'><label class='chk-label'><input type='checkbox' id='test_p1'> 💧 " + t("Pumpe 1", "Pump 1") + "</label>";
  if(ZWEITER_KREISLAUF) html += "<label class='chk-label'><input type='checkbox' id='test_p2'> 💧 " + t("Pumpe 2", "Pump 2") + "</label>";
  for(int i=0; i<ANZAHL_TOEPFE; i++) { html += "<label class='chk-label'><input type='checkbox' id='test_v" + String(i) + "'> 🪴 " + t("Ventil: ", "Valve: ") + NAME_TOPF[i] + "</label>"; }
  html += "</div><br><button type='button' id='testBtn' onclick='runMultiTest()' style='background:#e67e22; color:white; padding:12px; border:none; border-radius:8px; width:100%; font-size:16px; font-weight:bold; cursor:pointer;'>" + t("Ausgewählte testen", "Test Selected") + "</button></div>";
  
  // --- NEU: DER DATENBANK UPLOAD BUTTON ---
  html += "<div class='card' style='border: 2px solid #8e44ad; margin-top:20px; text-align:center;'>";
  html += "<h3 style='color:#8e44ad; margin-top:0;'>📚 " + t("Pflanzen-Datenbank Update", "Plant Database Update") + "</h3>";
  html += "<p style='font-size:14px; color:#7f8c8d;'>" + t("Lade hier eine neue oder angepasste <b>plants.js</b> hoch.", "Upload a new or custom <b>plants.js</b> here.") + "</p>";
  html += "<form method='POST' action='/upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='f' accept='.js' style='margin-bottom:10px;' required><br>";
  html += "<button type='submit' style='background:#8e44ad; color:white; padding:12px 20px; border:none; border-radius:8px; width:100%; font-size:16px; font-weight:bold; cursor:pointer;'>⬆️ " + t("Datenbank Hochladen", "Upload Database") + "</button>";
  html += "</form></div>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ------------------------------------------------------------
// SAVE PLANTS
// ------------------------------------------------------------
void handleSavePlants() {
  preferences.begin("kraeuter", false);
  for(int i=0; i<ANZAHL_TOEPFE; i++) {
    preferences.putString(("name"+String(i)).c_str(), server.arg("name"+String(i)));
    preferences.putInt(("ziel"+String(i)).c_str(), server.arg("ziel"+String(i)).toInt());
    preferences.putInt(("dauer"+String(i)).c_str(), server.arg("dauer"+String(i)).toInt());
    preferences.putInt(("pWahl"+String(i)).c_str(), server.arg("pWahl"+String(i)).toInt()); 
    preferences.putInt(("pinE"+String(i)).c_str(), server.arg("pinE"+String(i)).toInt());
    preferences.putInt(("pinV"+String(i)).c_str(), server.arg("pinV"+String(i)).toInt());
    preferences.putInt(("trocken"+String(i)).c_str(), server.arg("trocken"+String(i)).toInt());
    preferences.putInt(("nass"+String(i)).c_str(), server.arg("nass"+String(i)).toInt());
  }
  preferences.end();
  
  String msg = t("Pflanzen gespeichert! Neustart...", "Plants saved! Restarting...");
  String html = "<html><meta http-equiv='refresh' content='5; url=/'><body style='text-align:center; padding-top:50px;'><h2 style='color:#27ae60;'>" + msg + "</h2></body></html>";
  server.send(200, "text/html", html);
  delay(1000); ESP.restart();
}

// ============================================================
// Smart Rules TESTLAUF (Alle aktiven Regeln kurz auslösen)
// ============================================================
void handleTestRules() {
  for(int i=0; i<MAX_RULES; i++) {
    if(ruleActive[i]) {
      int p1 = ruleAction1[i];
      int p2 = ruleAction2[i];
      
      // 1. Relais EINSCHALTEN (LOW)
      if(p1 >= 0) schaltePin(p1, LOW); 
      if(p2 >= 0) schaltePin(p2, LOW);
      
      delay(1000); // 1 Sekunde klackern lassen
      
      // 2. Relais AUSSCHALTEN (HIGH)
      if(p1 >= 0) schaltePin(p1, HIGH);
      if(p2 >= 0) schaltePin(p2, HIGH);
      
      delay(500); // Kurz warten, bevor die nächste Regel getestet wird
    }
  }
  server.send(200, "text/plain", "OK");
}

// ============================================================
// DIE AUTOMATISIERUNGS-SEITE (Smart Rules & TIMER)
// ============================================================
void handleAutomation() {

  // --- DER TÜRSTEHER ---
  if (USE_AUTH && !server.authenticate(authUser.c_str(), authPass.c_str())) {
    return server.requestAuthentication();
  }

  // --- RAM OPTIMIERUNG ---
  String html;
  html.reserve(15000); 
  html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += getNavbar();

  html += "<title>Austrian Flame Automation</title>";
  html += "<style>body{font-family:'Segoe UI', Tahoma, Verdana, sans-serif; background-color:#eef2f3; padding:15px; line-height:1.6;} ";
  html += ".card{background:#fff; padding:20px; border-radius:10px; margin-bottom:20px; box-shadow:0 4px 6px rgba(0,0,0,0.05);} ";
  html += "h1, h2, h3{color:#2c3e50; margin-top:0;} ";
  html += "input, select{padding:8px; margin:5px 2px; border:1px solid #ccc; border-radius:5px;} ";
  html += ".btn{background:#e67e22; color:white; padding:15px; border:none; width:100%; border-radius:5px; font-size:18px; font-weight:bold; cursor:pointer;} ";
  html += ".btn-back{background-color:#7f8c8d; color:white; padding:12px; text-decoration:none; border-radius:8px; font-weight:bold; display:block; text-align:center; margin-bottom:20px;} ";
  html += ".rule-box{border-left: 5px solid #3498db; padding: 10px; margin-bottom: 20px; background:#f8f9fa; border-radius:0 5px 5px 0;} ";
  html += ".flex-row{display:flex; flex-wrap:wrap; align-items:center; gap:5px; margin-top:10px;} ";
  html += ".prio-badge{background:#e74c3c; color:white; padding:5px 10px; border-radius:5px; font-weight:bold; margin-right:10px;}";
  html += "</style>";

  // --- JAVASCRIPT FÜR DEN AUTO-SWAP DER PRIORITÄTEN ---
  html += "<script>";
  html += "document.addEventListener('DOMContentLoaded', function() {";
  html += "  let selects = document.querySelectorAll('.prio-select');";
  html += "  let oldVals = {};";
  html += "  selects.forEach(s => {";
  html += "    oldVals[s.name] = s.value;";
  html += "    s.addEventListener('focus', function() { oldVals[this.name] = this.value; });";
  html += "    s.addEventListener('change', function() {";
  html += "      let newVal = this.value; let oldVal = oldVals[this.name];";
  html += "      selects.forEach(other => {";
  html += "        if (other !== this && other.value === newVal) {";
  html += "          other.value = oldVal; oldVals[other.name] = oldVal;";
  html += "        }";
  html += "      });";
  html += "      oldVals[this.name] = newVal;";
  html += "    });";
  html += "  });";
  html += "});";
  html += "</script>";
  html += "</head><body>";

  html += "<a href='/' class='btn-back'>🔙 " + t("Zurück zum Dashboard", "Back to Dashboard") + "</a>";
  html += "<h2 style='text-align:center;'>🤖 " + t("Automatisierung & Logik", "Automation & Logic") + "</h2>";
  html += "<form action='/save_auto' method='POST'>";

  // Hilfsfunktion für die Prio-Dropdowns
  auto getPrioOpts = [](int currentPrio) {
    String out = "";
    for(int i=1; i<=11; i++) {
      out += "<option value='" + String(i) + "' " + (currentPrio == i ? "selected" : "") + ">" + t("Platz ", "Rank ") + String(i) + "</option>";
    }
    return out;
  };

    
html += "<div class='card'><h3 style='color:#f39c12; margin-top:0;'>⏰ " + t("Zeitschaltuhren (Licht / Dünger)", "Timers (Light / Fertilizer)") + "</h3>";
  for(int i=0; i<3; i++) {
    html += "<div class='rule-box' style='border-color:#f39c12;'>";
    
    // --- ZEILE 1: Prio-Badge und Titel (Exakt wie beim SIEM!) ---
    html += "<span class='prio-badge' style='background:#f39c12;'>🏆 <select name='tPrio" + String(i) + "' class='prio-select' style='background:transparent; color:white; border:none; font-weight:bold; outline:none;'>" + getPrioOpts(timerPrio[i]) + "</select></span>";
    html += "<label style='font-size:18px;'><b>" + t("Timer ", "Timer ") + String(i+1) + "</b></label><br>";
    
    // --- ZEILE 2: Alle Dropdowns von links nach rechts aufgefädelt ---
    html += "<div class='flex-row'>";
    
    // 1. Was wird geschaltet?
    html += "<select name='mosPin" + String(i) + "'>" + getPinDropdown(mosfetPin[i]) + "</select>";
    
    // 2. Wann wird geschaltet (Tage)?
    html += "<select name='mosMode" + String(i) + "'>";
    html += "<option value='0' " + String(mosfetMode[i]==0?"selected":"") + ">" + t("Jeden Tag", "Every Day") + "</option>";
    html += "<option value='1' " + String(mosfetMode[i]==1?"selected":"") + ">" + t("Nur Montags", "Mondays Only") + "</option>";
    html += "<option value='2' " + String(mosfetMode[i]==2?"selected":"") + ">" + t("Nur Dienstags", "Tuesdays Only") + "</option>";
    html += "<option value='3' " + String(mosfetMode[i]==3?"selected":"") + ">" + t("Nur Mittwochs", "Wednesdays Only") + "</option>";
    html += "<option value='4' " + String(mosfetMode[i]==4?"selected":"") + ">" + t("Nur Donnerstags", "Thursdays Only") + "</option>";
    html += "<option value='5' " + String(mosfetMode[i]==5?"selected":"") + ">" + t("Nur Freitags", "Fridays Only") + "</option>";
    html += "<option value='6' " + String(mosfetMode[i]==6?"selected":"") + ">" + t("Nur Samstags", "Saturdays Only") + "</option>";
    html += "<option value='7' " + String(mosfetMode[i]==7?"selected":"") + ">" + t("Nur Sonntags", "Sundays Only") + "</option>";
    html += "<option value='8' " + String(mosfetMode[i]==8?"selected":"") + ">" + t("Alle 3 Tage", "Every 3 Days") + "</option>";
    html += "<option value='9' " + String(mosfetMode[i]==9?"selected":"") + ">" + t("Alle 2 Wochen", "Every 14 Days") + "</option>";
    html += "<option value='10' " + String(mosfetMode[i]==10?"selected":"") + ">" + t("Alle 3 Wochen", "Every 21 Days") + "</option>";
    html += "<option value='11' " + String(mosfetMode[i]==11?"selected":"") + ">" + t("1. im Monat", "1st of Month") + "</option>";
    html += "</select>";
    
    // 3. Von und Bis (Uhrzeit)
    html += "<b>" + t("VON", "ON") + "</b> <input type='time' step='1' name='mosStart" + String(i) + "' value='" + mosfetStart[i] + "' style='width:auto;'>";
    html += "<b>" + t("BIS", "TO") + "</b> <input type='time' step='1' name='mosStop" + String(i) + "' value='" + mosfetStop[i] + "' style='width:auto;'>";
    // 4. NEU: UX-Bedingungs-Verknüpfung
    html += "<b style='margin-left:10px;'>" + t("BEDINGUNG", "CONDITION") + "</b> <select name='tLnk" + String(i) + "'>";
    html += "<option value='0' " + String(timerLink[i]==0 ? "selected":"") + ">🟢 " + t("Immer ausführen", "Always run") + "</option>";
    for(int r=1; r<=8; r++) {
        html += "<option value='" + String(r) + "' " + String(timerLink[i]==r ? "selected":"") + ">🔗 " + t("Nur wenn Regel ", "Only if Rule ") + String(r) + t(" zutrifft", " is TRUE") + "</option>";
    }
    html += "</select>";
    
    html += "</div></div>";
  }
  html += "</div>";

  html += "<div class='card'><h3 style='color:#3498db; margin-top:0;'>🧠 " + t("Smart Rules", "Smart Rules") + "</h3>";
  for(int i=0; i<MAX_RULES; i++) {
    html += "<div class='rule-box'>";
    html += "<span class='prio-badge' style='background:#3498db;'>🏆 <select name='rPrio" + String(i) + "' class='prio-select' style='background:transparent; color:white; border:none; font-weight:bold; outline:none;'>" + getPrioOpts(rulePrio[i]) + "</select></span>";
    html += "<label style='font-size:18px;'><b>" + t("Regel ", "Rule ") + String(i+1) + "</b></label>";
    html += " | <label>" + t("Aktiv?", "Active?") + " <input type='checkbox' name='rAct" + String(i) + "' value='1' " + (ruleActive[i] ? "checked" : "") + "></label><br>";

    html += "<div class='flex-row'>";
    html += "<b>" + t("WENN", "IF") + "</b> <select name='rTrig" + String(i) + "'>" + getTriggerDropdown(ruleTrigger[i]) + "</select>";
    html += "<b>" + t("IST", "IS") + "</b> <select name='rCond" + String(i) + "'>";
    html += "<option value='0' " + String(ruleCondition[i]==0 ? "selected" : "") + ">&lt; (" + t("kleiner", "less") + ")</option>";
    html += "<option value='1' " + String(ruleCondition[i]==1 ? "selected" : "") + ">&gt; (" + t("größer", "greater") + ")</option>";
    html += "</select>";
    html += "<input type='number' name='rVal" + String(i) + "' value='" + String(ruleValue[i]) + "' style='width:80px;' placeholder='" + t("Wert", "Value") + "'>";
    html += "<b>" + t("DANN", "THEN") + "</b> <select name='rA1_" + String(i) + "'>" + getPinDropdown(ruleAction1[i]) + "</select>";
    html += "<b>" + t("UND", "AND") + "</b> <select name='rA2_" + String(i) + "'>" + getPinDropdown(ruleAction2[i]) + "</select>";
    // --- NEU: DER MODUS! ---
    html += "<b>" + t("AKTION", "ACTION") + "</b> <select name='rMod" + String(i) + "'>";
    html += "<option value='0' " + String(ruleMode[i]==0 ? "selected" : "") + ">🟢 " + t("Einschalten", "Turn ON") + "</option>";
    html += "<option value='1' " + String(ruleMode[i]==1 ? "selected" : "") + ">🛑 " + t("Sperren (Aus)", "Block (OFF)") + "</option>";
    html += "</select>";

    html += "<b>" + t("FÜR", "FOR") + "</b> <input type='number' min='0' name='rDur" + String(i) + "' value='" + String(ruleDuration[i]) + "' style='width:90px;' placeholder='" + t("Sek.", "Sec.") + "'>";
    // --- NEU: DAS PAUSE FELD ---
    html += "<b>" + t("PAUSE", "PAUSE") + "</b> <input type='number' min='0' name='rPau" + String(i) + "' value='" + String(rulePause[i]) + "' style='width:70px;' placeholder='" + t("Sek.", "Sec.") + "'>";
    html += "</div></div>";
  }
  
  // --- DER NEUE Smart Rules TEST-BUTTON ---
  html += "<div class='card' style='border: 2px dashed #e74c3c; text-align:center; margin-top:20px;'>";
  html += "<h3 style='color:#e74c3c; margin-top:0;'>🛠️ Smart Rules-Testlauf (Hardware-Check)</h3>";
  html += "<p style='font-size:14px; color:#7f8c8d;'>" + t("Spielt alle aktiven Regeln nacheinander ab. Die Relais schalten für 1 Sekunde, um die Verkabelung zu testen.", "Plays all active rules sequentially. Relays will click for 1 second to test wiring.") + "</p>";
  html += "<button type='button' class='btn' style='background:#e74c3c; width:auto; padding:10px 20px;' onclick='this.innerHTML=\"" + t("⏳ Läuft...", "⏳ Running...") + "\"; fetch(\"/test_rules\", {method: \"POST\"}).then(()=>this.innerHTML=\"" + t("✅ Test beendet!", "✅ Test finished!") + "\")'>🚀 " + t("Aktive Regeln testen", "Test Active Rules") + "</button>";
  html += "</div>";

  html += "<input type='submit' class='btn' value='💾 " + t("Regeln Speichern & Neustart", "Save & Restart") + "'>";
  html += "</form></div></body></html>";
  server.send(200, "text/html", html);
}

// ============================================================
// SPEICHER-LOGIK
// ============================================================
void handleSave() {
  preferences.begin("kraeuter", false);
  
  preferences.putString("tz", server.arg("tz"));

  preferences.putString("sysName", server.arg("sysName"));
  
  

  for(int i=0; i<2; i++) {
    preferences.putString(("esName"+String(i)).c_str(), server.arg("esName"+String(i)));
    preferences.putInt(("esPin"+String(i)).c_str(), server.arg("esPin"+String(i)).toInt());
  }

  int neueAnzahl = server.arg("anzToepfe").toInt();
  preferences.putInt("anzToepfe", neueAnzahl);
  preferences.putBool("zweiKreis", server.hasArg("zweiKreis")); 
  preferences.putBool("useDisp", server.hasArg("useDisp"));
  
  preferences.putBool("usePcf", server.hasArg("usePcf"));

  // NEU: Funk-Einstellungen speichern
  preferences.putBool("useNow", server.hasArg("useNow"));
  preferences.putString("dispmac", server.arg("dispmac"));
 
  // Pins Kreislauf 2
  preferences.putInt("pinP2", server.arg("pinP2").toInt());
  preferences.putInt("pinS2", server.arg("pinS2").toInt());

  // Klima-Sensoren (Multi-Zone)
  preferences.putInt("anzDht", server.arg("anzDht").toInt());
  for(int i=0; i<3; i++) {
    preferences.putInt(("pinDht"+String(i)).c_str(), server.arg("pinDht"+String(i)).toInt());
  }

  // Töpfe speichern
  for(int i=0; i<neueAnzahl; i++) {
    preferences.putString(("name"+String(i)).c_str(), server.arg("name"+String(i)));
    preferences.putInt(("ziel"+String(i)).c_str(), server.arg("ziel"+String(i)).toInt());
    preferences.putInt(("dauer"+String(i)).c_str(), server.arg("dauer"+String(i)).toInt());
    preferences.putInt(("pWahl"+String(i)).c_str(), server.arg("pWahl"+String(i)).toInt()); 
    preferences.putInt(("pinE"+String(i)).c_str(), server.arg("pinE"+String(i)).toInt());
    preferences.putInt(("pinV"+String(i)).c_str(), server.arg("pinV"+String(i)).toInt());

    // Individuelle Kalibrierung speichern
    preferences.putInt(("trocken"+String(i)).c_str(), server.arg("trocken"+String(i)).toInt());
    preferences.putInt(("nass"+String(i)).c_str(), server.arg("nass"+String(i)).toInt());
  }
  preferences.end();
  
  // --- ÜBERSETZTE ERFOLGSMELDUNG ---
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><meta http-equiv='refresh' content='5; url=/'></head>";
  html += "<body style='text-align:center; padding-top:50px; font-family:Arial;'><h2 style='color:#27ae60;'>";
  html += t("Speichern erfolgreich!", "Saved successfully!") + "</h2><p>";
  html += t("Der ESP32 startet jetzt neu und wendet die Konfiguration an.", "The ESP32 is rebooting and applying the configuration.") + "<br>";
  html += t("Du wirst in 5 Sekunden weitergeleitet...", "You will be redirected in 5 seconds...") + "</p></body></html>";
  
  server.send(200, "text/html", html);
  
  delay(1000); ESP.restart(); 
}

// ============================================================
// AUTOMATISIERUNG SPEICHERN & NEUSTART (/save_auto)
// ============================================================
void handleSaveAuto() {
  preferences.begin("kraeuter", false);

  for(int i=0; i<3; i++) {

    // --- HIER MUSS ES REIN (Innerhalb der Schleife!) ---
    mosfetMode[i] = server.arg("mosMode" + String(i)).toInt();
    preferences.putInt(("mMod" + String(i)).c_str(), mosfetMode[i]);

    timerPrio[i] = server.arg("tPrio" + String(i)).toInt();
    timerLink[i] = server.arg("tLnk" + String(i)).toInt();
    preferences.putInt(("tLnk" + String(i)).c_str(), timerLink[i]);
    mosfetPin[i] = server.arg("mosPin" + String(i)).toInt();
    mosfetStart[i] = server.arg("mosStart" + String(i));
    mosfetStop[i] = server.arg("mosStop" + String(i));


    preferences.putInt(("tPri" + String(i)).c_str(), timerPrio[i]);
    preferences.putInt(("mPin" + String(i)).c_str(), mosfetPin[i]);
    preferences.putString(("mSta" + String(i)).c_str(), mosfetStart[i]);
    preferences.putString(("mSto" + String(i)).c_str(), mosfetStop[i]);
  }

  for(int i=0; i<MAX_RULES; i++) {
    rulePrio[i] = server.arg("rPrio" + String(i)).toInt();
    ruleActive[i] = server.hasArg("rAct" + String(i)); 
    ruleTrigger[i] = server.arg("rTrig" + String(i)).toInt();
    ruleCondition[i] = server.arg("rCond" + String(i)).toInt();
    ruleValue[i] = server.arg("rVal" + String(i)).toInt();
    ruleAction1[i] = server.arg("rA1_" + String(i)).toInt();
    ruleAction2[i] = server.arg("rA2_" + String(i)).toInt();
    ruleMode[i] = server.arg("rMod" + String(i)).toInt(); // <-- DAS IST NEU

    // Bei den server.arg Abfragen hinzufügen:
    ruleDuration[i] = server.arg("rDur" + String(i)).toInt();
    rulePause[i] = server.arg("rPau" + String(i)).toInt(); // NEU
    

    preferences.putInt(("rPri" + String(i)).c_str(), rulePrio[i]);
    preferences.putBool(("rAct" + String(i)).c_str(), ruleActive[i]);
    preferences.putInt(("rTri" + String(i)).c_str(), ruleTrigger[i]);
    preferences.putInt(("rCon" + String(i)).c_str(), ruleCondition[i]);
    preferences.putInt(("rVal" + String(i)).c_str(), ruleValue[i]);
    preferences.putInt(("rA1_" + String(i)).c_str(), ruleAction1[i]);
    preferences.putInt(("rA2_" + String(i)).c_str(), ruleAction2[i]);
    preferences.putInt(("rMod" + String(i)).c_str(), ruleMode[i]); // <-- DAS IST NEU
    preferences.putInt(("rDur" + String(i)).c_str(), ruleDuration[i]);
    preferences.putInt(("rPau" + String(i)).c_str(), rulePause[i]); // NEU
  }

    

  preferences.end();

  // Wunderschöne Lade-Seite für den Neustart
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta http-equiv='refresh' content='5; url=/automation'>";
  html += "<style>body{font-family:Arial; background:#eef2f3; text-align:center; padding-top:50px;}</style></head><body>";
  html += "<h2 style='color:#27ae60;'>💾 " + t("Gespeichert! System startet neu...", "Saved! System is restarting...") + "</h2>";
  html += "<p style='color:#7f8c8d;'>" + t("Die neuen Regeln werden geladen. Bitte warten...", "Loading new rules. Please wait...") + "</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);

  delay(1000);
  ESP.restart(); // BAM! Das System startet neu und alle Variablen sind frisch!
}

// ============================================================
// HARDWARE-TEST ROUTE (SIMULTAN-TEST)
// ============================================================
void handleTest() {
  int activePins[10]; 
  int count = 0;

  if (server.hasArg("p1")) activePins[count++] = PIN_PUMPE;
  if (ZWEITER_KREISLAUF && server.hasArg("p2")) activePins[count++] = PIN_PUMPE2;

  for(int i=0; i<ANZAHL_TOEPFE; i++) {
    if (server.hasArg("v" + String(i))) {
      activePins[count++] = PIN_VENTIL[i];
    }
  }

  // HIER IST DAS UPGRADE: schaltePin statt digitalWrite!
  for(int i=0; i<count; i++) schaltePin(activePins[i], LOW);
  if(count > 0) delay(2000); 
  for(int i=0; i<count; i++) schaltePin(activePins[i], HIGH);

  server.send(200, "text/plain", "OK");
}

// ============================================================
// HILFS-FUNKTION: MEDIAN (STABILER WERT)
// ============================================================
int berechneStabilenWert(int werte[], int anzahl) {
  for (int i = 0; i < anzahl - 1; i++) {
    for (int k = 0; k < anzahl - i - 1; k++) {
      if (werte[k] > werte[k + 1]) {
        int temp = werte[k]; werte[k] = werte[k + 1]; werte[k + 1] = temp;
      }
    }
  }
  int kandidat = werte[anzahl / 2]; 
  int treffer = 0;
  for(int i = 0; i < anzahl; i++) {
    if (abs(werte[i] - kandidat) <= 5) treffer++;
  }
  if (treffer < (anzahl / 2)) return -999; 
  return kandidat; 
}

// ============================================================
// SENSOREN & ZEIT AUSLESEN
// ============================================================
void leseSensoren() {
  for(int i=0; i<ANZAHL_TOEPFE; i++) {  
    
    // --- NEU: OVERSAMPLING (Hardware-Spikes glätten!) ---
    long summeRoh = 0;
    for(int m = 0; m < 10; m++) {
        summeRoh += analogRead(PIN_ERDE[i]);
        delay(2); // 2 Millisekunden dem ADC Zeit geben
    }
    int roh = summeRoh / 10; // Den perfekten Durchschnitt bilden
    
    aktuelleErde[i] = map(roh, SENSOR_TROCKEN[i], SENSOR_NASS[i], 0, 100);
    aktuelleErde[i] = constrain(aktuelleErde[i], 0, 100);

    // --- NEU: VIRTUELLER FLOW-SENSOR RESET ---
    if (aktuelleErde[i] >= ZIEL_FEUCHTIGKEIT[i]) {
      fehlversuche[i] = 0; // Pflanze ist nass genug, Counter auf Null!
      soakTimeAktiv[i] = false; // Sicker-Pause ebenfalls zurücksetzen!
    }

    }
  

  
  // NEU: Multi-Zone Logik!
  if(ANZAHL_DHT > 0) { 
    for(int i=0; i<ANZAHL_DHT; i++) {
      if(dhtSensors[i] != nullptr) {
        float t = dhtSensors[i]->readTemperature();
        float h = dhtSensors[i]->readHumidity();
        if(!isnan(t) && !isnan(h)) {
          aktuelleTemp[i] = t;
          aktuelleLuft[i] = h;
        }
      }
    }
  }
  
  // NEU: Tank 1 unabhängig auslesen (mit Festwasser-Logik!)
  if (PIN_SCHWIMMER == -1) {
    tankVoll = true; // Auto-OK für den Gartenschlauch an Kreislauf 1!
  } else {
    tankVoll = (digitalRead(PIN_SCHWIMMER) == HIGH); // (Oder LOW, je nach Schwimmer)
  }

  // NEU: Tank 2 unabhängig auslesen!
  if (ZWEITER_KREISLAUF) {
    if (PIN_SCHWIMMER2 == -1) {
      tankVoll2 = true; // Auto-OK für den Gartenschlauch!
    } else {
      tankVoll2 = (digitalRead(PIN_SCHWIMMER2) == HIGH);
    }
  }
  
  struct tm timeinfo;
  if(getLocalTime(&timeinfo)){
    char timeStringBuff[10]; 
    strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M", &timeinfo);
    aktuelleZeit = String(timeStringBuff);
    
    // NEU: Das Datum für das Web-Dashboard extrahieren
    char dateBuff[15]; 
    strftime(dateBuff, sizeof(dateBuff), "%d.%m.%Y", &timeinfo);
    aktuellesDatum = String(dateBuff);
  }
}

// ============================================================
// DISPLAY ROTATION (DYNAMISCH & VORLAGE 1 STYLE - SWEET SPOT)
// ============================================================
void zeigeDisplay(String phaseText, int seite) {
  if(!USE_DISPLAY) return; 

  display.clearDisplay(); 
  display.setTextColor(SSD1306_WHITE);
  
  // DYNAMISCHE SEITENBERECHNUNG
  int numStatusPages = (ANZAHL_TOEPFE > 3) ? 2 : 1;
  int seiteStatusStart = 2; 
  int seitePotsStart = seiteStatusStart + numStatusPages; 
  int seiteDht = seitePotsStart + ANZAHL_TOEPFE;
  int seiteTank = seiteDht + (ANZAHL_DHT > 0 ? 1 : 0);
  
  // --- INHALTS-SEITEN ---
  if (seite == 0) {
    display.setCursor(20, 32); display.setTextSize(2); 
    display.print(aktuelleZeit); 
    display.setCursor(0, 54); display.setTextSize(1); 
    
    if (systemPausiert) {
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.print(t(" SYSTEM PAUSIERT! ", " SYSTEM PAUSED! "));
        display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    } else {
        display.print(phaseText); 
    }
    
  } else if (seite == 1) {
    display.setCursor(0, 32); display.setTextSize(1);
    
    if (WiFi.status() == WL_CONNECTED) {
      display.println(t("WLAN verbunden:", "WiFi connected:")); 
      display.setCursor(0, 44); 
      display.print(systemName + ".local");
      display.setCursor(0, 54); 
      display.print("IP: "); display.print(aktuelleIP);
    } else {
      display.setTextColor(SSD1306_WHITE);
      display.println(t("WLAN OFFLINE!", "WIFI OFFLINE!")); 
      display.setCursor(0, 44); 
      display.print(t("Suche Router...", "Searching router..."));
    }
    
  // --- NEU: DYNAMISCHE STATUS-SEITEN (Max 3 pro Seite mit Abstand) ---
  } else if (seite >= seiteStatusStart && seite < seitePotsStart) {
    display.setTextSize(1);
    int pageOffset = seite - seiteStatusStart; 
    int startPot = pageOffset * 3;
    int endPot = startPot + 3;
    if (endPot > ANZAHL_TOEPFE) endPot = ANZAHL_TOEPFE;

    for(int i = startPot; i < endPot; i++) {
        String dName = NAME_TOPF[i];
        if (dName == "Topf " + String(i+1) || dName == "") dName = t("Topf ", "Pot ") + String(i+1);
        if(dName.length() > 9) dName = dName.substring(0, 7) + ".."; 
        
        int zeile = i - startPot;
        display.setCursor(0, 26 + (zeile * 12)); 
        display.print(dName); display.print(": ");
        if(pumpenSperre[i]) display.print(t("ALARM", "ERROR"));
        else if(imDeepCheck[i]) display.print("CHECK");
        else if(aktuelleErde[i] > (ZIEL_FEUCHTIGKEIT[i] + 15)) display.print(t("NASS!", "WET!"));
        else display.print("OK");
    }

  } else if (seite >= seitePotsStart && seite < seiteDht) { 
    int topf = seite - seitePotsStart;
    display.setCursor(0, 32); display.setTextSize(1); 
    
    String dName = NAME_TOPF[topf];
    if (dName == "Topf " + String(topf+1) || dName == "") dName = t("Topf ", "Pot ") + String(topf+1);
    if (dName.length() > 11) dName = dName.substring(0, 11) + "...";
    
    display.print(dName); 
    display.print(t(" (Ziel: ", " (Target: ")); 
    display.print(ZIEL_FEUCHTIGKEIT[topf]); 
    display.println("%)");

    display.setCursor(30, 45); display.setTextSize(2); 
    if(pumpenSperre[topf]) display.print(t("ALARM", "ERROR")); 
    else { display.print(aktuelleErde[topf]); display.print("%"); }
 
  } else if (seite == seiteDht && ANZAHL_DHT > 0) { 
    display.setTextSize(1); 
    for(int i=0; i<ANZAHL_DHT; i++) {
      display.setCursor(0, 32 + (i * 11)); 
      display.print("Z"); display.print(i+1); display.print(": "); 
      display.print(aktuelleTemp[i], 1); display.print("C | ");
      display.print(aktuelleLuft[i], 0); display.print("%");
    }
    
  } else if (seite == seiteTank) { 
    display.setCursor(0, 32); display.setTextSize(1); 
    display.print("TANK 1: "); display.print(tankVoll ? "OK" : t("LEER!", "EMPTY!"));
    if(ZWEITER_KREISLAUF) {
       display.setCursor(0, 44);
       display.print("TANK 2: "); display.print(tankVoll2 ? "OK" : t("LEER!", "EMPTY!"));
    }
  }
  
  display.display();
}

// ============================================================
// HILFS-FUNKTION: SERIELLER TEST-MODUS
// ============================================================
void checkSerialCommands() {
  if (Serial.available() > 0) {
    char cmd = Serial.read(); 
    if (cmd == '1') { schaltePin(PIN_VENTIL[0], LOW); delay(2000); schaltePin(PIN_VENTIL[0], HIGH); } 
    else if (cmd == '2') { schaltePin(PIN_VENTIL[1], LOW); delay(2000); schaltePin(PIN_VENTIL[1], HIGH); } 
    else if (cmd == '3') { schaltePin(PIN_VENTIL[2], LOW); delay(2000); schaltePin(PIN_VENTIL[2], HIGH); } 
    else if (cmd == 'p' || cmd == 'P') { schaltePin(PIN_PUMPE, LOW); delay(2000); schaltePin(PIN_PUMPE, HIGH); }
  }
}

// ============================================================
// SMART DELAY (NON-BLOCKING GIESSEN MIT FEEDBACK & WATCHDOG!)
// ============================================================
bool smartDelay(long dauer_ms, int pumpe, int topfIndex = -1) {
  unsigned long start = millis();
  static unsigned long lastAutoCheck = 0;

  while(millis() - start < dauer_ms) {
    server.handleClient(); 
    ArduinoOTA.handle(); 
    checkSerialCommands();

    if (USE_MQTT) {
      if (!mqttClient.connected()) reconnectMQTT();
      mqttClient.loop();
    }

    if (millis() - lastAutoCheck > 1000) {
      runAutomation();
      lastAutoCheck = millis();
    }

    // 🚨 NEU: DER HARDWARE-WATCHDOG FÜR DEN AKTUELLEN TOPF
    if (topfIndex != -1) {
        int roh = analogRead(PIN_ERDE[topfIndex]);
        int liveFeuchte = map(roh, SENSOR_TROCKEN[topfIndex], SENSOR_NASS[topfIndex], 0, 100);
        
        // Not-Aus, wenn Feuchtigkeit +10% über dem Zielwert ist!
        if (liveFeuchte >= (ZIEL_FEUCHTIGKEIT[topfIndex] + 10)) {
            addLog("🚨 " + t("NOT-AUS: Topf extrem nass!", "EMERGENCY STOP: Pot extremely wet!"));
            pumpenSperre[topfIndex] = true; // Sperrt den Topf zur Sicherheit hart
            return false; // ❌ PUMPE SOFORT ABBRECHEN!
        }
    }

    // 🚨 NOTFALL-ABBRUCH (SIEM, Manuelle Pause)
    if (siemBlockWatering || pinGesperrt[pumpe] || systemPausiert) {
        addLog("🚨 " + t("NOT-HALT: Gießen abgebrochen!", "EMERGENCY STOP: Watering aborted!"));
        return false; // ❌ ABBRUCH!
    }

    delay(10); 
  }
  return true; // ✅ Alles sicher abgelaufen
}

// ============================================================
// INTELLIGENTES WARTEN (DYNAMISCHE SEITENZAHL + HERZSCHLAG!)
// ============================================================
void warteUndRotiere(long dauer_ms, String phasenName) {
  long startZeit = millis(); 
  long letzterBildwechsel = 0; 
  int seite = 0; 
  bool erstesBild = true;
  static unsigned long lastAutoCheck = 0; // NEU FÜR Smart Rules
  static unsigned long lastMqttSend = 0; // <--- NEU

  int numStatusPages = (ANZAHL_TOEPFE > 3) ? 2 : 1;
  int maxSeite = 2 + numStatusPages + ANZAHL_TOEPFE + (ANZAHL_DHT > 0 ? 1 : 0);

  while (millis() - startZeit < dauer_ms) {
    server.handleClient(); 
    checkSerialCommands(); 
    ArduinoOTA.handle();

    // --- DER INTELLIGENTE WLAN-RETTER (Backoff-Strategie) ---
    if (WiFi.status() != WL_CONNECTED) {
      // Prüfen, ob unser Timer abgelaufen ist (Start: 5 Min, später: 30 Min)
      if (millis() - lastWifiCheck > wifiCheckInterval) {
        lastWifiCheck = millis();
        wifiFailCount++;
        
        addLog(t("WLAN offline. Versuch " + String(wifiFailCount) + "...", "WiFi offline. Attempt " + String(wifiFailCount) + "..."));
        
        // Den WLAN-Chip kurz "aufwecken", bevor wir reconnecten
        WiFi.disconnect();
        WiFi.reconnect();
        
        // Wenn er 3 Mal (also 15 Minuten lang) erfolglos war, schalten wir auf 30 Minuten um!
        if (wifiFailCount >= 3 && wifiCheckInterval != 1800000) {
          wifiCheckInterval = 1800000; // 30 Minuten in Millisekunden
          addLog(t("Router tot? Prüfe WLAN ab jetzt nur noch alle 30 Min.", "Router dead? Checking WiFi every 30 mins now."));
        }
      }
    } else {
      // WLAN ist verbunden! Wir setzen unsere Notfall-Variablen wieder zurück.
      if (wifiFailCount > 0) {
        addLog(t("WLAN erfolgreich wiederhergestellt!", "WiFi successfully restored!"));
        wifiFailCount = 0;
        wifiCheckInterval = 300000; // Wieder scharfschalten auf 5 Minuten für den nächsten Ausfall
      }
    }
    
    // --- DER LEBENSRETTER FÜR MQTT & Smart Rules ---
    if (USE_MQTT) {
      if (!mqttClient.connected()) reconnectMQTT();
      mqttClient.loop(); // Der Herzschlag für den Docker-Server!
      
      // NEU: Daten alle 30 Sekunden schicken, EGAL in welcher Phase wir sind!
      if (millis() - lastMqttSend > 30000) { 
        sendeMQTTDaten();
        lastMqttSend = millis();
      }
    }
    
    // Das SIEM prüft jede Sekunde, auch wenn wir in Phase 1 oder 2 hängen!
    if (millis() - lastAutoCheck > 1000) {
      runAutomation(); 
      lastAutoCheck = millis();
    }
    // ----------------------------------------
    
    if (erstesBild || millis() - letzterBildwechsel > 4000) { 
      letzterBildwechsel = millis(); 
      erstesBild = false; 
      leseSensoren(); 
      zeigeDisplay(phasenName, seite);
      
      seite++; 
      if (seite > maxSeite) seite = 0; 
    }
    delay(10); 
  }
}

// ============================================================
// Factory Reset (WLAN, NVS & Force Defaults)
// ============================================================
void handleFactoryReset() {
  // Türsteher: Nur mit Passwort darf die Bombe gezündet werden!
  if (USE_AUTH && !server.authenticate(authUser.c_str(), authPass.c_str())) return server.requestAuthentication();

  // 1. WLAN-GEDÄCHTNIS LÖSCHEN
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  WiFi.disconnect(true, true); // Schmeißt alle Router-Verbindungen brutal raus

  // 2. FESTPLATTE LÖSCHEN
  preferences.begin("kraeuter", false);
  preferences.clear(); // Die absolute Atombombe für den NVS!
  
  // 3. SICHERE DEFAULTS ERZWINGEN
  // Wir schreiben "Alles AUS" aktiv in den Speicher, damit die C++ Defaults überschrieben werden!
  preferences.putBool("useDisp", false);     // Display AUS
  preferences.putInt("anzDht", 0);           // Klima-Sensoren AUS
  preferences.putBool("zweiKreis", false);   // Zweiter Kreislauf AUS
  preferences.putBool("useNow", false);      // ESP-NOW Funk AUS
  preferences.putBool("usePcf", false);      // PCF8574 Modul AUS
  preferences.putBool("useMqtt", false);     // MQTT AUS
  preferences.putInt("anzToepfe", 1);        // Nur noch 1 Topf aktiv
  preferences.end();
  
  // 4. ABSCHIEDS-SEITE
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta http-equiv='refresh' content='15; url=/'>";
  html += "<style>body{font-family:Arial; background:#e74c3c; color:white; text-align:center; padding-top:50px;}</style></head><body>";
  html += "<h2>💥 " + t("SYSTEM RESET AUSGEFÜHRT", "SYSTEM RESET EXECUTED") + " 💥</h2>";
  html += "<p>" + t("Alle Daten und das WLAN-Passwort wurden vernichtet. Der ESP32 startet jetzt als Neugerät...", "All data and WiFi password destroyed. The ESP32 is rebooting as a new device...") + "</p>";
  html += "<p style='font-size:12px; margin-top:20px;'>" + t("Suche gleich mit deinem Handy nach dem WLAN 'Kraeuter-WG'.", "Search for the WiFi 'Kraeuter-WG' with your phone.") + "</p>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);

  delay(2000);
  ESP.restart(); // Neustart ins nackte Leben
}

// ============================================================
// SPRACHE UMSCHALTEN & SPEICHERN
// ============================================================
void handleSetLang() {
  if (server.hasArg("en")) {
    isEnglish = (server.arg("en") == "1"); // 1 = Englisch, 0 = Deutsch
    
    // Direkt auf die Festplatte speichern, damit es nach einem Neustart bleibt
    preferences.begin("kraeuter", false);
    preferences.putBool("langEn", isEnglish);
    preferences.end();
  }
  
  // Leitet den Browser sofort zurück zur Startseite!
  server.sendHeader("Location", "/"); 
  server.send(303);
}

// --- ESP-NOW MULTI-PEER PARSER ---
void setupEspNowPeers() {
  if (!ESP_NOW_ACTIVE) return;
  
  registeredDisplays = 0;
  String macList = DISPLAY_MAC + ";"; // Dummy-Zeichen am Ende für den perfekten Schnitt
  int startIndex = 0;
  
  for(int i = 0; i < macList.length(); i++) {
    if(macList.charAt(i) == ';') {
      String macStr = macList.substring(startIndex, i);
      macStr.trim(); // Leerzeichen davor/danach killen!
      
      // Ist es eine gültige MAC (17 Zeichen) und haben wir noch Platz?
      if(macStr.length() == 17 && registeredDisplays < MAX_DISPLAYS) {
        int values[6];
        // Liest den String als Hexadezimal-Zahlen aus
        if(6 == sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5])) {
          
          esp_now_peer_info_t peerInfo = {};
          for(int b = 0; b < 6; b++) {
            broadcastAddresses[registeredDisplays][b] = (uint8_t) values[b];
            peerInfo.peer_addr[b] = (uint8_t) values[b];
          }
          
          peerInfo.channel = 0;  
          peerInfo.encrypt = false;
          
          // Dem ESP-NOW System den neuen Empfänger vorstellen
          if (esp_now_add_peer(&peerInfo) == ESP_OK) {
            registeredDisplays++;
          }
        }
      }
      startIndex = i + 1; // Nächsten Block starten
    }
  }
}

// ============================================================
// CONFIG BACKUP (DOWNLOAD ALS .JSON)
// ============================================================
void handleBackup(){
  if (USE_AUTH && !server.authenticate(authUser.c_str(), authPass.c_str())) {
    return server.requestAuthentication();
  }
 
  preferences.begin("kraeuter", true);
  
  // Wir erstellen ein dynamisches JSON-Dokument
  JsonDocument doc;
  
  // --- Globale & Netzwerk Settings ---
  doc["sysName"] = preferences.getString("sysName", "kraeuter-wg");
  doc["tz"] = preferences.getString("tz", "CET-1CEST,M3.5.0,M10.5.0/3");
  doc["langEn"] = preferences.getBool("langEn", 0);
  
  // --- Security ---
  doc["useAuth"] = preferences.getBool("useAuth", 0);
  doc["authUser"] = preferences.getString("authUser", "admin");
  doc["authPass"] = preferences.getString("authPass", "flame");
  
  // --- Hardware Architektur ---
  doc["usePcf"] = preferences.getBool("usePcf", 0);
  doc["anzToepfe"] = preferences.getInt("anzToepfe", 3);
  doc["zweiKreis"] = preferences.getBool("zweiKreis", 0);
  doc["useDisp"] = preferences.getBool("useDisp", 1);
  doc["anzDht"] = preferences.getInt("anzDht", 1);
  
  // --- Haupt-Pins ---
  doc["pinP1"] = preferences.getInt("pinP1", 27);
  doc["pinS1"] = preferences.getInt("pinS1", 5);
  
  // --- Kreislauf 2 ---
  doc["pinP2"] = preferences.getInt("pinP2", 16);
  doc["pinS2"] = preferences.getInt("pinS2", 35);
  
  // --- ESP-NOW ---
  doc["useNow"] = preferences.getBool("useNow", 0);
  doc["dispmac"] = preferences.getString("dispmac", "FF:FF:FF:FF:FF:FF");

  // --- Zeitschaltuhren & Dünger-Modus ---
  for(int i=0; i<3; i++) {
    doc["pinDht" + String(i)] = preferences.getInt(("pinDht"+String(i)).c_str(), -1);
    doc["tPri" + String(i)] = preferences.getInt(("tPri"+String(i)).c_str(), 9+i);
    doc["mPin" + String(i)] = preferences.getInt(("mPin"+String(i)).c_str(), -1);
    doc["mSta" + String(i)] = preferences.getString(("mSta"+String(i)).c_str(), "08:00");
    doc["mSto" + String(i)] = preferences.getString(("mSto"+String(i)).c_str(), "20:00");
    doc["mMod" + String(i)] = preferences.getInt(("mMod"+String(i)).c_str(), 0); 
    doc["tLnk" + String(i)] = preferences.getInt(("tLnk"+String(i)).c_str(), 0); 
  }
  
  // --- Extra Sensoren ---
  for(int i=0; i<2; i++) {
    doc["esName" + String(i)] = preferences.getString(("esName"+String(i)).c_str(), "Sensor");
    doc["esPin" + String(i)] = preferences.getInt(("esPin"+String(i)).c_str(), -1);
  }
  
  // --- Töpfe & Pflanzen ---
  for(int i=0; i<6; i++) {
    doc["name" + String(i)] = preferences.getString(("name"+String(i)).c_str(), "Topf "+String(i+1));
    doc["ziel" + String(i)] = preferences.getInt(("ziel"+String(i)).c_str(), 50);
    doc["dauer" + String(i)] = preferences.getInt(("dauer"+String(i)).c_str(), 3000);
    doc["pWahl" + String(i)] = preferences.getInt(("pWahl"+String(i)).c_str(), 0);
    doc["pinE" + String(i)] = preferences.getInt(("pinE"+String(i)).c_str(), -1);
    doc["pinV" + String(i)] = preferences.getInt(("pinV"+String(i)).c_str(), -1);
    doc["trocken" + String(i)] = preferences.getInt(("trocken"+String(i)).c_str(), 2760);
    doc["nass" + String(i)] = preferences.getInt(("nass"+String(i)).c_str(), 1130);

    // 💾 NEU: Die Gieß-Historie mit ins Backup-File packen!
    doc["lGi" + String(i)] = letztesGiessen[i];
  }
  
  // --- Smart Rules ---
  for(int i=0; i<8; i++) {
    doc["rPri" + String(i)] = preferences.getInt(("rPri"+String(i)).c_str(), i+1);
    doc["rAct" + String(i)] = preferences.getBool(("rAct"+String(i)).c_str(), 0);
    doc["rTri" + String(i)] = preferences.getInt(("rTri"+String(i)).c_str(), 0);
    doc["rCon" + String(i)] = preferences.getInt(("rCon"+String(i)).c_str(), 0);
    doc["rVal" + String(i)] = preferences.getInt(("rVal"+String(i)).c_str(), 0);
    doc["rA1_" + String(i)] = preferences.getInt(("rA1_"+String(i)).c_str(), -1);
    doc["rA2_" + String(i)] = preferences.getInt(("rA2_"+String(i)).c_str(), -1);
    doc["rDur" + String(i)] = preferences.getInt(("rDur"+String(i)).c_str(), 0);
    doc["rPau" + String(i)] = preferences.getInt(("rPau"+String(i)).c_str(), 0);
  }
  preferences.end();

  // JSON in einen String umwandeln
  String jsonOutput;
  serializeJson(doc, jsonOutput);

  // Schickt die Daten als saubere .json Datei an den Browser
  server.sendHeader("Content-Disposition", "attachment; filename=AustrianFlame_Config.json");
  server.send(200, "application/json", jsonOutput);
}

// ============================================================
// CONFIG RESTORE (UPLOAD VON .JSON)
// ============================================================
void handleRestore() {
  String jsonInput = server.arg("configData");
  
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonInput);

  if (error) {
     server.send(400, "text/plain", t("Fehler: Ungültige JSON Datei!", "Error: Invalid JSON file!"));
     return;
  }

  preferences.begin("kraeuter", false);
  
  // Smarter Iterate-Hack: Geht automatisch durch alle JSON-Einträge
  JsonObject root = doc.as<JsonObject>();
  for (JsonPair kv : root) {
      String key = kv.key().c_str();
      JsonVariant val = kv.value();
      
      // Erkennt den Datentyp automatisch und speichert ihn passend
      if (val.is<bool>()) preferences.putBool(key.c_str(), val.as<bool>());
      else if (val.is<int>()) preferences.putInt(key.c_str(), val.as<int>());
      else if (val.is<const char*>()) preferences.putString(key.c_str(), val.as<const char*>());
  }
  
  preferences.end();
  
  // Erfolgsmeldung & Neustart
  String html = "<html><body style='font-family:sans-serif; text-align:center; padding-top:50px;'>";
  html += "<h2 style='color:#27ae60;'>" + t("JSON-Backup erfolgreich geladen!", "JSON Backup successfully loaded!") + "</h2>";
  html += "<p>" + t("Der ESP32 startet jetzt neu und wendet alle Regeln an...", "The ESP32 is restarting and applying all rules...") + "</p>";
  html += "<script>setTimeout(function(){ window.location.href='/'; }, 6000);</script></body></html>";
  server.send(200, "text/html", html);
  
  delay(1000); ESP.restart();
}

// ============================================================
// MQTT ENGINE (HOMEASSISTANT) - TWO WAY SYNC!
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  
  // HIER IST DER FIX: Wir nennen die Variable topicStr, damit t() frei bleibt für die Übersetzung!
  String topicStr = String(topic);
  
  Serial.println("📬 MQTT PAKET: " + topicStr + " -> " + msg);

  if (topicStr == mqttTopic + "/smartrules/1") { mqttSensorWert[0] = msg.toFloat(); }
  if (topicStr == mqttTopic + "/smartrules/2") { mqttSensorWert[1] = msg.toFloat(); }

  // --- DER NEUE COMMAND-PARSER ---
  if (topicStr.startsWith(mqttTopic + "/cmd/ziel/")) {
     int potIndex = topicStr.substring(topicStr.lastIndexOf('/') + 1).toInt();
     
     if (potIndex >= 0 && potIndex < ANZAHL_TOEPFE) {
         int neuesZiel = msg.toInt(); 
         
         // 🛡️ HIER IST DER FIX: Input Sanitizing!
         neuesZiel = constrain(neuesZiel, 0, 100); 
         
         ZIEL_FEUCHTIGKEIT[potIndex] = neuesZiel; 
         
         preferences.begin("kraeuter", false);
         preferences.putInt(("ziel"+String(potIndex)).c_str(), neuesZiel);
         preferences.end();
         
         addLog("🎯 " + t("Ziel ", "Target ") + NAME_TOPF[potIndex] + " -> " + String(neuesZiel) + "%");
         sendeMQTTDaten(); 
     }
  }
}

// --- DIE VISITENKARTE (JETZT SAUBER DRAUSSEN) ---
void sendeDiscovery() {
  if (!USE_MQTT || !mqttClient.connected()) return;

  String deviceJson = ",\"dev\":{\"ids\":[\"" + WiFi.macAddress() + "\"],\"name\":\"" + systemName + "\",\"mf\":\"Austrian Flame\",\"mdl\":\"Garden Control v2\"}";

  // 1. DISCOVERY TÖPFE
  for (int i = 0; i < ANZAHL_TOEPFE; i++) {
    JsonDocument doc;
    String id = "pot_" + String(i + 1);
    doc["name"] = NAME_TOPF[i] + " Feuchtigkeit";
    doc["stat_t"] = mqttTopic + "/state";
    doc["val_tpl"] = "{{ value_json.pflanzen.topf_" + String(i + 1) + ".feuchte }}";
    doc["unit_of_meas"] = "%";
    doc["dev_cla"] = "humidity";
    doc["uniq_id"] = systemName + "_" + id;

    // --- NEU: DISCOVERY FÜR DEN PFLANZEN-STATUS (OK/ALARM) ---
    JsonDocument docStat;
    String idStat = "stat_" + String(i + 1);
    docStat["name"] = NAME_TOPF[i] + " Status";
    docStat["stat_t"] = mqttTopic + "/state";
    docStat["val_tpl"] = "{{ value_json.pflanzen.topf_" + String(i + 1) + ".status }}";
    docStat["icon"] = "mdi:alert-circle-check"; // Geiles Status-Icon
    docStat["uniq_id"] = systemName + "_" + idStat;
    
    String outStat; serializeJson(docStat, outStat);
    outStat.remove(outStat.length()-1); outStat += deviceJson + "}";
    // WICHTIG: Das wird als normaler "sensor" angelegt!
    mqttClient.publish(("homeassistant/sensor/" + systemName + "/" + idStat + "/config").c_str(), outStat.c_str(), true);
  }

  // 2. DISCOVERY KLIMA
  for (int i = 0; i < ANZAHL_DHT; i++) {
    // TEMPERATUR
    JsonDocument docT;
    String idT = "temp_" + String(i + 1);
    docT["name"] = "Zone " + String(i+1) + " Temperatur";
    docT["stat_t"] = mqttTopic + "/state";
    docT["val_tpl"] = "{{ value_json.klima['" + String(i+1) + "'].temp }}";
    docT["unit_of_meas"] = "°C";
    docT["dev_cla"] = "temperature";
    docT["uniq_id"] = systemName + "_" + idT;
    String outT; serializeJson(docT, outT);
    outT.remove(outT.length()-1); outT += deviceJson + "}";
    mqttClient.publish(("homeassistant/sensor/" + systemName + "/" + idT + "/config").c_str(), outT.c_str(), true);

    // LUFTFEUCHTIGKEIT (HIER WAR DER FEHLER!)
    JsonDocument docH;
    String idH = "hum_" + String(i + 1);
    docH["name"] = "Zone " + String(i+1) + " Luftfeuchte";
    docH["stat_t"] = mqttTopic + "/state";
    docH["val_tpl"] = "{{ value_json.klima['" + String(i+1) + "'].luft }}";
    docH["unit_of_meas"] = "%";
    docH["dev_cla"] = "humidity";
    docH["uniq_id"] = systemName + "_" + idH;
    String outH; serializeJson(docH, outH);
    outH.remove(outH.length()-1); outH += deviceJson + "}";
    mqttClient.publish(("homeassistant/sensor/" + systemName + "/" + idH + "/config").c_str(), outH.c_str(), true);
  }

  // 3. DISCOVERY FÜR DEN WASSERTANK (JETZT ALS TEXT-SENSOR!)
  JsonDocument docTank;
  docTank["name"] = "Wassertank Status";
  docTank["stat_t"] = mqttTopic + "/state";
  docTank["val_tpl"] = "{{ value_json.tank }}";
  docTank["icon"] = "mdi:water-pump"; // Gibt dem Tank ein cooles Icon in HA!
  docTank["uniq_id"] = systemName + "_tank";
  
  String outTank; serializeJson(docTank, outTank);
  outTank.remove(outTank.length()-1); outTank += deviceJson + "}";
  // WICHTIG: Hier steht jetzt "sensor" statt "binary_sensor"!
  mqttClient.publish(("homeassistant/sensor/" + systemName + "/tank/config").c_str(), outTank.c_str(), true);

  // 4. DISCOVERY FÜR SYSTEM-PAUSE STATUS
  JsonDocument docSysStat;
  String idSysStat = "sys_status";
  docSysStat["name"] = "System Modus";
  docSysStat["stat_t"] = mqttTopic + "/state";
  docSysStat["val_tpl"] = "{{ value_json.system_status }}";
  docSysStat["icon"] = "mdi:play-pause";
  docSysStat["uniq_id"] = systemName + "_" + idSysStat;
  
  String outSysStat; serializeJson(docSysStat, outSysStat);
  outSysStat.remove(outSysStat.length()-1); outSysStat += deviceJson + "}";
  mqttClient.publish(("homeassistant/sensor/" + systemName + "/" + idSysStat + "/config").c_str(), outSysStat.c_str(), true);

  Serial.println("📡 Discovery-Visitenkarten verschickt!");
}

void reconnectMQTT() {
  if (!USE_MQTT || mqttServer == "") return;
  
  if (!mqttClient.connected()) {
    
    // 1. Wenn wir GERADE ERST die Verbindung verloren haben, merke dir die Uhrzeit!
    if (mqttWarVerbunden) {
      mqttOfflineSeit = millis();
      mqttWarVerbunden = false;
    }

    // 2. Wie lange sind wir schon offline?
    unsigned long offlineDauer = millis() - mqttOfflineSeit;
    
    // 3. Den Backoff-Algorithmus berechnen (Wartezeit)
    unsigned long warteZeit = 5000; // Standard: 5 Sekunden
    
    if (offlineDauer > 60000) { // Nach 1 Minute Ausfall:
      warteZeit = 60000; // Alle 1 Minute probieren
    }
    if (offlineDauer > 600000) { // Nach 10 Minuten Ausfall:
      warteZeit = 3600000; // Nur noch 1x pro Stunde probieren! (60 * 60 * 1000)
    }

    // 4. Prüfen, ob die Wartezeit schon um ist, dann zuschlagen
    if (millis() - letzterMqttVersuch > warteZeit) {
      letzterMqttVersuch = millis();
      
      Serial.print("Verbinde mit MQTT (Intervall: ");
      Serial.print(warteZeit / 1000);
      Serial.println("s)...");

      String clientId = "AustrianFlame-" + WiFi.macAddress();
      clientId.replace(":", ""); 
      
      mqttClient.setKeepAlive(60); 

      bool connected = false;
      if (mqttUser.length() > 0) connected = mqttClient.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str());
      else connected = mqttClient.connect(clientId.c_str());

      if (connected) {
        Serial.println("🚀 ERFOLGREICH VERBUNDEN!");
        addLog(t("MQTT Server verbunden!", "MQTT Server connected!"));
        mqttClient.subscribe((mqttTopic + "/smartrules/1").c_str());
        mqttClient.subscribe((mqttTopic + "/smartrules/2").c_str());
        
        // NEU: Der ESP32 lauscht jetzt auf JEDEN Befehl, der im Ordner "cmd" ankommt!
        mqttClient.subscribe((mqttTopic + "/cmd/#").c_str());

        sendeDiscovery(); 
        mqttWarVerbunden = true; // Wir sind wieder online, Tracker zurücksetzen!
      } else {
        Serial.print("Fehler, rc=");
        Serial.println(mqttClient.state());
      }
    }
  } else {
    // Falls wir beim Booten sofort online sind
    mqttWarVerbunden = true; 
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200); 
  
  if(!LittleFS.begin(true)){
  Serial.println("LittleFS Fehler beim Mounten!");
} else {
  Serial.println("LittleFS erfolgreich gestartet.");
}

  loadPreferences(); 

  // --- LITTLEFS DATEISYSTEM STARTEN ---
  if (!LittleFS.begin(true)) {
    Serial.println("❌ LittleFS Fehler beim Mounten!");
  } else {
    Serial.println("✅ LittleFS erfolgreich gestartet.");
  }


// WICHTIG: Den I2C-Bus als allererstes manuell starten! (SDA=21, SCL=22)
  Wire.begin(21, 22);
  
  
  // NEU: Hier wecken wir den PCF8574 direkt nach dem I2C-Start auf
  if (USE_PCF) {
    if (pcf.begin()) {
      Serial.println("PCF8574 erfolgreich verbunden!");
    } else {
      Serial.println("Fehler: PCF8574 nicht gefunden. Adresse prüfen!");
    }
  }
  

// WICHTIG: pinMode nur für echte ESP-Pins (< 100)! Schalten dann über unsere neue Funktion.
  if (PIN_PUMPE < 100 && PIN_PUMPE != -1) { pinMode(PIN_PUMPE, OUTPUT); schaltePin(PIN_PUMPE, HIGH); }
  
  // --- NEU: SETUP FÜR KREISLAUF 2 ---
  if (ZWEITER_KREISLAUF) {
      if (PIN_PUMPE2 < 100 && PIN_PUMPE2 != -1) { pinMode(PIN_PUMPE2, OUTPUT); schaltePin(PIN_PUMPE2, HIGH); }
      if (PIN_SCHWIMMER2 != -1) { pinMode(PIN_SCHWIMMER2, INPUT_PULLUP); }
  }

  for(int i=0; i<ANZAHL_DHT; i++) {
    dhtSensors[i] = new DHT(PIN_DHT[i], DHT_TYPE);
    dhtSensors[i]->begin();
  }

  for(int i=0; i<ANZAHL_TOEPFE; i++) { 
    if (PIN_VENTIL[i] < 100 && PIN_VENTIL[i] != -1) pinMode(PIN_VENTIL[i], OUTPUT); 
    schaltePin(PIN_VENTIL[i], HIGH); 
  }
  
  if (PIN_SCHWIMMER != -1) pinMode(PIN_SCHWIMMER, INPUT_PULLUP);
  
  // HIER WIRD ABGEFRAGT OB HARDWARE AN IST:
  // 2. Klima-Zonen auslesen
  for(int i=0; i<ANZAHL_DHT; i++) {
    float t = dhtSensors[i]->readTemperature();
    float h = dhtSensors[i]->readHumidity();
    if(!isnan(t) && !isnan(h)) {
      aktuelleTemp[i] = t;
      aktuelleLuft[i] = h;
    }
  }
  
  if(USE_DISPLAY) {
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
      Serial.println("⚠️ OLED Fehler - ignoriere und boote weiter!");
      USE_DISPLAY = false; // Schaltet die Display-Updates ab, damit das System nicht crasht
    } else {
      display.clearDisplay(); display.setTextSize(1); display.setTextColor(WHITE);
      display.setCursor(0, 15); display.println(t("WLAN Suche...", "Searching WiFi...")); display.display();
    }
  }

  // 1. Namen aus dem Speicher laden (hast du ja schon)
  systemName = preferences.getString("sysName", "kraeuter-wg"); 

  // 2. NEU: Dem Router den Namen beim DHCP-Handshake übergeben!
  WiFi.setHostname(systemName.c_str());

  WiFiManager wifiManager;
  bool res = wifiManager.autoConnect("Kraeuter-WG");
  if(!res) { ESP.restart(); }

  aktuelleIP = WiFi.localIP().toString();
  Serial.println("\nWLAN verbunden! IP: " + aktuelleIP);


  // --- mDNS STARTEN ---
  if (!MDNS.begin(systemName.c_str())) {   
    Serial.println("Fehler beim Starten des mDNS!");
  } else {
    Serial.println("mDNS gestartet! Erreichbar unter: http://" + systemName + ".local");
    MDNS.addService("http", "tcp", 80);
  }

  if (USE_MQTT && mqttServer != "") {
    mqttClient.setServer(mqttServer.c_str(), mqttPort);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(2048); // <--- XXL-PAKETE ERLAUBT! (DAS IST DER FIX)
  }

  if (ESP_NOW_ACTIVE) {
        if (esp_now_init() == ESP_OK) {
            setupEspNowPeers(); // Diese eine Funktion erledigt jetzt das Splitten UND das Registrieren!
        }
    }
 
 // Anstatt des festen Strings nehmen wir unsere Variable
  configTzTime(timeZone.c_str(), "pool.ntp.org", "time.nist.gov");

  server.on("/", handleRoot);
  server.on("/sys_settings", handleSysSettings);
  server.on("/plant_settings", handlePlantSettings);
  server.on("/save_sys", HTTP_POST, handleSaveSys);
  server.on("/save_plants", HTTP_POST, handleSavePlants);        
  
  server.on("/test", HTTP_POST, handleTest);             // 🛡️ SICHER! (Schaltet Hardware)
  server.on("/hub-code", handleHubCode);
  server.on("/help", handleHelp);
  
  server.on("/setlang", HTTP_POST, handleSetLang);       // 🛡️ SICHER! (Schreibt in den NVS Flash)
  server.on("/automation", handleAutomation);
  
  server.on("/save_auto", HTTP_POST, handleSaveAuto);    // 🛡️ SICHER! (Vergaß vorher das HTTP_POST)
  server.on("/test_rules", HTTP_POST, handleTestRules);  // 🛡️ SICHER! (Schaltet Hardware)
  
  server.on("/factory_reset", HTTP_POST, handleFactoryReset);
  server.on("/update", HTTP_GET, handleUpdateGET);
  server.on("/update", HTTP_POST, handleUpdatePOST, handleUpdateUpload);
  server.on("/backup", handleBackup);
  server.on("/restore", HTTP_POST, handleRestore);
  server.on("/diagnose", handleDiagnose);
  
  // 1. Liefert die plants.js an das Dashboard aus
  server.on("/plants.js", []() {
    if (!LittleFS.exists("/plants.js")) return server.send(404, "text/plain", "plants.js nicht gefunden!");
    File file = LittleFS.open("/plants.js", "r");
    server.streamFile(file, "application/javascript");
    file.close();
  });

  // --- PAUSE-KNOPF LOGIK ---
  server.on("/toggle_pause", []() {
    if (USE_AUTH && !server.authenticate(authUser.c_str(), authPass.c_str())) return server.requestAuthentication();
    systemPausiert = !systemPausiert; // Umschalten

    preferences.begin("kraeuter", false);
    preferences.putBool("sysPause", systemPausiert);
    preferences.end();

    addLog(systemPausiert ? t("⏸️ System Manuell PAUSIERT!", "⏸️ System Manually PAUSED!") : t("▶️ System Manuell AKTIVIERT!", "▶️ System Manually ACTIVATED!"));

    // --- NEU: ABBRUCH-LOGIK FÜR DEN LOG-BEWEIS ---
    if (systemPausiert && aktuellePhase == 2) {
    addLog(t("🛑 Gießen abgebrochen (durch Pause)!", "🛑 Watering aborted (by Pause)!"));
    aktuellePhase = 0; // Countdown hart im Hintergrund killen
    // WICHTIG: Setze auch die Pflanzen zurück, die auf Gießen gewartet haben
    for(int i=0; i < ANZAHL_TOEPFE; i++) {
      imDeepCheck[i] = false; 
    }
    }

    // Zurück zur Startseite schicken
    server.sendHeader("Location", "/"); 
    server.send(303);
  });

  // 2. Die geheime Upload-Seite (Gott-Modus)
  server.on("/upload", HTTP_GET, []() {
    if (USE_AUTH && !server.authenticate(authUser.c_str(), authPass.c_str())) return server.requestAuthentication();
    String html = "<html><body style='font-family:Arial; padding:20px;'><h2 style='color:#2980b9;'>LittleFS Uploader</h2>";
    html += "<p>" + t("Lade hier deine <b>plants.js</b> hoch!", "Upload your <b>plants.js</b> here!") + "</p>";
    html += "<form method='POST' action='/upload' enctype='multipart/form-data'><input type='file' name='f' accept='.js' style='margin-bottom:10px;'><br><input type='submit' value='Upload' style='padding:10px; background:#27ae60; color:white; border:none; border-radius:5px;'></form></body></html>";
    server.send(200, "text/html", html);
  });

 // 3. Die Upload-Logik im Hintergrund
  server.on("/upload", HTTP_POST, []() {
    String msg = t("Upload erfolgreich! Lade Pflanzen-Seite neu...", "Upload successful! Reloading plants page...");
    server.send(200, "text/html", "<html><meta http-equiv='refresh' content='3; url=/plant_settings'><body style='text-align:center; padding:50px;'><h2 style='color:#27ae60;'>" + msg + "</h2></body></html>");
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      String filename = upload.filename;
      if (!filename.startsWith("/")) filename = "/" + filename;
      fsUploadFile = LittleFS.open(filename, "w");
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (fsUploadFile) fsUploadFile.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
      if (fsUploadFile) fsUploadFile.close();
    }
    addLog(t("System erfolgreich gestartet!", "System successfully booted!"));
  });


  server.begin();

  ArduinoOTA.setHostname("Kraeuter-WG-ESP32");
  ArduinoOTA.setPassword("Kraeuter-WG!");
  ArduinoOTA.begin();
}

//=============================================================
//ESP-NOW
//=============================================================
void sendeDaten() {
    if (!ESP_NOW_ACTIVE) return;

    // Paket füllen
    strcpy(myData.name, systemName.c_str()); 
    for(int i=0; i<MAX_TOEPFE; i++) myData.feuchte[i] = aktuelleErde[i];
    myData.tank = tankVoll;
    
    myData.phase = aktuellePhase;
    myData.minuten = minutenInPhase;

    myData.anzDht = ANZAHL_DHT;
    for(int i=0; i<3; i++) {
        myData.temp[i] = aktuelleTemp[i];
        myData.luft[i] = aktuelleLuft[i];
    }
    
    myData.tank2 = tankVoll2;
    myData.hasKreis2 = ZWEITER_KREISLAUF;

    myData.anzToepfe = ANZAHL_TOEPFE; 
    strcpy(myData.zeit, aktuelleZeit.c_str());
    myData.isPaused = systemPausiert; 

    // Namen und Status der Töpfe verpacken...
    for(int i=0; i<ANZAHL_TOEPFE; i++) {
        String shortName = NAME_TOPF[i];
        if (shortName == "Topf " + String(i+1) || shortName == "") shortName = t("Topf ", "Pot ") + String(i+1);
        strncpy(myData.tName[i], shortName.c_str(), 11);
        myData.tName[i][11] = '\0'; 

        String lG = (letztesGiessen[i] == "Neustart") ? "--:--" : letztesGiessen[i];
        strncpy(myData.letztesGiessen[i], lG.c_str(), 14);
        myData.letztesGiessen[i][14] = '\0';

        // Status Code für das Display ermitteln (Inklusive "Sehr Nass")
        if (pumpenSperre[i]) myData.topfStatus[i] = 3; 
        else if (aktuellePhase == 2 && imDeepCheck[i]) myData.topfStatus[i] = 2; 
        else if (aktuelleErde[i] < ZIEL_FEUCHTIGKEIT[i]) myData.topfStatus[i] = 1; 
        else if (aktuelleErde[i] > (ZIEL_FEUCHTIGKEIT[i] + 15)) myData.topfStatus[i] = 4; // SEHR NASS
        else myData.topfStatus[i] = 0; 
    }

    // Prüfen, ob überhaupt ein Display registriert wurde
    if (registeredDisplays > 0) {
      for(int i = 0; i < registeredDisplays; i++) {
        esp_now_send(broadcastAddresses[i], (uint8_t *) &myData, sizeof(myData));
      }
    }
}

// ============================================================
// HubCode Seite (DYNAMISCH, TOUCH, AUTO-ROTATION, DOWNLOAD & ZWEISPRACHIG)
// ============================================================
void handleHubCode() {
  String code = "// === " + t("AUSTRIAN FLAME: SMART GROW TOUCH TERMINAL (CYD)", "AUSTRIAN FLAME: SMART GROW TOUCH TERMINAL (CYD)") + " ===\n";
  code += "// " + t("Benötigt", "Requires") + ": TFT_eSPI, WiFiManager, ArduinoOTA, XPT2046_Touchscreen\n\n";
  code += "#include <esp_now.h>\n#include <WiFi.h>\n#include <WiFiManager.h>\n";
  code += "#include <ArduinoOTA.h>\n#include <TFT_eSPI.h>\n#include <SPI.h>\n#include <XPT2046_Touchscreen.h>\n\n";
  
  code += "// --- CYD TOUCH PINS ---\n";
  code += "#define XPT2046_IRQ 36\n#define XPT2046_MOSI 32\n#define XPT2046_MISO 39\n";
  code += "#define XPT2046_CLK 25\n#define XPT2046_CS 33\n\n";
  
  code += "SPIClass touchSPI = SPIClass(HSPI);\n";
  code += "XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);\n";
  code += "TFT_eSPI tft = TFT_eSPI();\n\n";

  // NEU: Das Struct im CYD muss exakt zum ESP32 passen!
  code += "typedef struct struct_message {\n";
  code += "    char name[20]; int feuchte[6]; bool tank; bool tank2; bool hasKreis2;\n";
  code += "    float temp[3]; float luft[3]; int phase; int minuten; int anzToepfe;\n";
  code += "    int anzDht; char zeit[6]; char tName[6][12]; char letztesGiessen[6][15];\n";
  code += "    int topfStatus[6]; bool isPaused;\n";
  code += "} struct_message;\n\nstruct_message incomingData;\n\n";

  code += "bool hasData = false; bool needsRedraw = true; int currentScreen = 0;\n";
  code += "unsigned long lastTouchTime = 0; unsigned long lastAutoRotate = 0;\n\n";

  code += "void OnDataRecv(const uint8_t * mac, const uint8_t *incoming, int len) {\n";
  code += "  memcpy(&incomingData, incoming, sizeof(incomingData));\n";
  code += "  hasData = true; needsRedraw = true;\n}\n\n";

  // --- DASHBOARD (KACHELN) ---
  code += "void drawDashboard() {\n";
  code += "  tft.fillScreen(TFT_BLACK);\n";
  code += "  tft.fillRect(0, 0, 320, 30, tft.color565(44, 62, 80));\n";
  code += "  tft.setTextColor(TFT_WHITE); tft.setTextSize(2);\n";
  code += "  tft.setCursor(10, 8); tft.print(incomingData.name);\n";
  
  // NEU: PAUSE BANNER AUF DEM CYD
  code += "  if(incomingData.isPaused) { tft.setTextColor(TFT_RED); tft.setCursor(200, 8); tft.print(\"PAUSED!\"); }\n";
  code += "  else { tft.setCursor(240, 8); tft.print(incomingData.zeit); }\n";
  
  code += "  int y = 35; int x = 10;\n";
  code += "  for(int i=0; i<incomingData.anzToepfe; i++) {\n";
  code += "    uint16_t boxColor = (incomingData.feuchte[i] < 30 || incomingData.topfStatus[i] == 3) ? TFT_RED : tft.color565(39, 174, 96);\n";
  // Wenn sehr nass, Box leicht blau machen!
  code += "    if(incomingData.topfStatus[i] == 4) boxColor = tft.color565(41, 128, 185);\n"; 
  
  code += "    tft.fillRoundRect(x, y, 145, 50, 5, boxColor);\n";
  code += "    tft.setTextColor(TFT_WHITE); tft.setTextSize(1);\n";
  code += "    tft.setCursor(x + 8, y + 8); tft.print(incomingData.tName[i]);\n";
  code += "    tft.setTextSize(2); tft.setCursor(x + 8, y + 25); tft.printf(\"%d %%\", incomingData.feuchte[i]);\n";
  code += "    x += 155; if(x > 170) { x = 10; y += 56; }\n";
  code += "  }\n";
  code += "  tft.fillRect(0, 205, 320, 35, tft.color565(52, 73, 94));\n";
  code += "  tft.setTextColor(TFT_WHITE); tft.setTextSize(2); tft.setCursor(5, 213);\n";
  code += "  if(incomingData.hasKreis2) {\n";
  code += "    tft.setTextSize(1); tft.printf(\"Temp:%.1f C | T1:%s T2:%s\", incomingData.temp[0], incomingData.tank ? \"OK\" : \"X\", incomingData.tank2 ? \"OK\" : \"X\");\n";
  code += "  } else {\n";
  code += "    tft.printf(\"T:%.1f C | Tank:%s\", incomingData.temp[0], incomingData.tank ? \"OK\" : \"" + t("LEER", "EMPTY") + "\");\n";
  code += "  }\n";
  code += "  tft.drawRoundRect(250, 208, 65, 28, 4, TFT_WHITE); tft.setTextSize(1); tft.setCursor(262, 218); tft.print(\"" + t("KLIMA", "CLIMATE") + "\");\n";
  code += "}\n\n";

  // --- KLIMA SCREEN ---
  code += "void drawClimaScreen() {\n";
  code += "  tft.fillScreen(TFT_BLACK); tft.fillRect(0, 0, 320, 40, tft.color565(52, 152, 219));\n";
  code += "  tft.setTextColor(TFT_WHITE); tft.setTextSize(2); tft.setCursor(15, 12); tft.print(\"< BACK\");\n";
  code += "  tft.setCursor(120, 12); tft.print(\"" + t("KLIMA ZONEN", "CLIMATE ZONES") + "\");\n";
  code += "  if(incomingData.anzDht == 0) { tft.setCursor(50, 100); tft.print(\"" + t("Keine Sensoren aktiv", "No sensors active") + "\"); return; }\n";
  code += "  for(int i=0; i<incomingData.anzDht; i++) {\n";
  code += "    int y = 50 + (i * 60);\n";
  code += "    tft.fillRoundRect(10, y, 300, 50, 5, tft.color565(44, 62, 80));\n";
  code += "    tft.setCursor(25, y+17); tft.printf(\"" + t("Zone", "Zone") + " %d: %.1f C | %.1f %%\", i+1, incomingData.temp[i], incomingData.luft[i]);\n";
  code += "  }\n}\n\n";

  // --- DETAIL SCREEN ---
  code += "void drawDetailScreen(int tIndex) {\n";
  code += "  tft.fillScreen(TFT_BLACK); tft.fillRect(0, 0, 320, 40, tft.color565(230, 126, 34));\n";
  code += "  tft.setTextColor(TFT_WHITE); tft.setTextSize(2); tft.setCursor(15, 12); tft.print(\"< BACK\");\n";
  code += "  tft.setTextSize(3); tft.setTextColor(tft.color565(46, 204, 113)); tft.setCursor(10, 60); tft.print(incomingData.tName[tIndex]);\n";
  code += "  tft.setTextSize(2); tft.setTextColor(TFT_WHITE);\n";
  code += "  tft.setCursor(10, 100); tft.printf(\"" + t("Feuchtigkeit:", "Humidity:") + " %d %%\", incomingData.feuchte[tIndex]);\n";
  code += "  tft.setCursor(10, 130); tft.printf(\"" + t("Zuletzt Wasser:", "Last Watered:") + " %s\", incomingData.letztesGiessen[tIndex]);\n";
  code += "  tft.setCursor(10, 170); tft.print(\"Status: \");\n";
  code += "  int s = incomingData.topfStatus[tIndex];\n";
  code += "  if(s==3) { tft.setTextColor(TFT_RED); tft.print(\"" + t("ALARM / FEHLER", "ALARM / ERROR") + "\"); }\n";
  // --- NEU: SEHR NASS STATUS AUF DEM CYD! ---
  code += "  else if(s==4) { tft.setTextColor(tft.color565(52, 152, 219)); tft.print(\"" + t("SEHR NASS", "TOO WET") + "\"); }\n";
  code += "  else if(s==2) { tft.setTextColor(TFT_ORANGE); tft.print(\"" + t("DEEP-CHECK LAEUFT", "DEEP-CHECK RUNNING") + "\"); }\n";
  code += "  else if(s==1) { tft.setTextColor(TFT_CYAN); tft.print(\"" + t("ERDE TROCKEN", "SOIL DRY") + "\"); }\n";
  code += "  else { tft.setTextColor(TFT_GREEN); tft.print(\"" + t("ALLES OPTIMAL", "ALL OPTIMAL") + "\"); }\n}\n\n";

  // --- TOUCH LOGIK ---
  code += "void handleTouch() {\n";
  code += "  if (ts.touched()) {\n";
  code += "    TS_Point p = ts.getPoint(); if (p.z < 200) return;\n";
  code += "    if (millis() - lastTouchTime > 400) {\n";
  code += "      lastTouchTime = millis(); lastAutoRotate = millis(); \n";
  code += "      int touchX = map(p.x, 300, 3800, 0, 320); int touchY = map(p.y, 300, 3800, 0, 240);\n";
  code += "      if (currentScreen == 0) {\n";
  code += "        if(touchX > 240 && touchY > 200) { currentScreen = 99; needsRedraw = true; return; }\n";
  code += "        int startY = 35; for(int i=0; i<incomingData.anzToepfe; i++) {\n";
  code += "          int xP = (i%2==0)?10:165; int yP = startY+((i/2)*56);\n";
  code += "          if(touchX>xP && touchX<xP+145 && touchY>yP && touchY<yP+50) { currentScreen=i+1; needsRedraw=true; break; }\n";
  code += "        }\n";
  code += "      } else { if(touchX<100 && touchY<50) { currentScreen=0; needsRedraw=true; } }\n";
  code += "    }\n  }\n}\n\n";

  // --- SETUP ---
  code += "void setup() {\n";
  code += "  Serial.begin(115200);\n";
  code += "  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);\n";
  code += "  ts.begin(touchSPI); ts.setRotation(1);\n";
  code += "  tft.init(); tft.setRotation(1); tft.fillScreen(TFT_BLACK);\n";
  code += "  tft.setTextColor(TFT_WHITE); tft.setTextSize(2);\n";
  code += "  tft.setCursor(10, 20); tft.println(\"" + t("Starte System...", "Starting system...") + "\");\n\n";
  
  code += "  WiFiManager wifiManager;\n";
  code += "  if(!wifiManager.autoConnect(\"Kraeuter-WG_Display\")) ESP.restart();\n\n";
  
  code += "  tft.fillScreen(TFT_BLACK);\n";
  code += "  tft.setCursor(10, 20); tft.setTextColor(TFT_GREEN); tft.println(\"" + t("WLAN Verbunden!", "WiFi Connected!") + "\");\n";
  code += "  tft.setTextColor(TFT_WHITE); tft.setCursor(10, 50); tft.print(\"IP: \"); tft.println(WiFi.localIP());\n";
  code += "  tft.setTextColor(TFT_YELLOW); tft.setCursor(10, 80); tft.print(\"MAC: \"); tft.println(WiFi.macAddress());\n";
  code += "  tft.setTextColor(TFT_WHITE); tft.setCursor(10, 130); tft.println(\"" + t("Warte auf Sensordaten...", "Waiting for sensor data...") + "\");\n\n";
  
  code += "  ArduinoOTA.setHostname(\"Kraeuter-WG-Display\");\n";
  code += "  ArduinoOTA.setPassword(\"AutGardener!\");\n";
  code += "  ArduinoOTA.begin();\n\n";
  
  code += "  if (esp_now_init() == ESP_OK) esp_now_register_recv_cb(OnDataRecv);\n";
  code += "  else { tft.setTextColor(TFT_RED); tft.setCursor(10, 160); tft.println(\"" + t("ESP-NOW FEHLER!", "ESP-NOW ERROR!") + "\"); }\n}\n\n";

  // --- LOOP MIT AUTO-ROTATION ---
  code += "void loop() {\n";
  code += "  ArduinoOTA.handle();\n";
  code += "  handleTouch();\n\n";
  code += "  if (millis() - lastTouchTime > 15000) {\n";
  code += "    if (millis() - lastAutoRotate > 5000) {\n";
  code += "      lastAutoRotate = millis();\n";
  code += "      if (currentScreen == 0) currentScreen = 99;\n";
  code += "      else currentScreen = 0;\n";
  code += "      needsRedraw = true;\n";
  code += "    }\n  }\n\n";
  code += "  if (needsRedraw && hasData) {\n";
  code += "    if (currentScreen == 0) drawDashboard();\n";
  code += "    else if (currentScreen == 99) drawClimaScreen();\n";
  code += "    else drawDetailScreen(currentScreen - 1);\n";
  code += "    needsRedraw = false;\n";
  code += "  }\n}\n";

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>" + t("Hub Code generieren", "Generate Hub Code") + "</title>";
  html += "<style>body{font-family:Arial; padding:15px; background:#eef2f3; color:#2c3e50; text-align:center;} ";
  html += "textarea{width:100%; height:400px; font-family:monospace; background:#fff; color:#2c3e50; padding:15px; border-radius:8px; border:2px solid #bdc3c7; box-sizing:border-box;} ";
  html += ".btn{background:#e67e22; color:white; padding:15px 30px; text-decoration:none; border-radius:8px; display:inline-block; margin-top:20px; cursor:pointer; font-weight:bold; border:none; font-size:16px; box-shadow:0 4px 6px rgba(0,0,0,0.1); margin-right:10px;}</style>";
  html += "</head><body>";
  
  html += getNavbar();
  
  html += "<h2 style='color:#e67e22;'>📄 " + t("CYD Touch-Terminal Code", "CYD Touch Terminal Code") + "</h2>";
  html += "<p style='font-size:16px;'>" + t("Hier ist der komplette Code inkl. Touch-Steuerung, Detailseiten und OTA-Updates:", "Here is the complete code incl. touch controls, detail screens, and OTA updates:") + "</p>";
  
  html += "<textarea id='codeBox' readonly spellcheck='false'>" + code + "</textarea><br>";
  
  html += "<button class='btn' onclick='copyCode()'>📋 " + t("Kopieren", "Copy") + "</button>";
  html += "<button class='btn' style='background:#3498db;' onclick='downloadCode()'>💾 " + t(".ino Herunterladen", "Download .ino") + "</button>";
  
  html += "<script>";
  html += "function copyCode() { var cb = document.getElementById(\"codeBox\"); cb.select(); document.execCommand(\"copy\"); alert(\"" + t("Kopiert!", "Copied!") + "\"); } ";
  html += "function downloadCode() { ";
  html += "  var text = document.getElementById('codeBox').value; ";
  html += "  var blob = new Blob([text], {type: 'text/plain'}); ";
  html += "  var a = document.createElement('a'); ";
  html += "  a.download = 'CYD_Touch_Terminal.ino'; "; 
  html += "  a.href = window.URL.createObjectURL(blob); ";
  html += "  document.body.appendChild(a); ";
  html += "  a.click(); ";
  html += "  document.body.removeChild(a); ";
  html += "} ";
  html += "</script>";
  
  html += "</body></html>";
  server.send(200, "text/html", html);
}


// ============================================================
// OTA WEB-UPDATER HANDLER
// ============================================================

// 1. Zeigt das HTML-Formular an
void handleUpdateGET() {

  // --- DER TÜRSTEHER ---
  if (USE_AUTH && !server.authenticate(authUser.c_str(), authPass.c_str())) {
    return server.requestAuthentication();
  }

  server.sendHeader("Connection", "close");
  String html = "<html><body style='font-family:sans-serif; padding:20px;'>";
  html += "<h2>" + t("Austrian Flame - Firmware Update", "Austrian Flame - Firmware Update") + "</h2>";
  html += "<p>" + t("Bitte waehle die neue .bin Datei aus:", "Please select the new .bin file:") + "</p>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='update' accept='.bin' style='margin-bottom:20px;'><br>";
  html += "<input type='submit' value='" + t("Firmware Flashen", "Flash Firmware") + "' style='padding:10px 20px; font-weight:bold;'>";
  html += "</form></body></html>";
  server.send(200, "text/html", html);
}

// 2. Wird ganz am Ende aufgerufen (Erfolgsmeldung & Neustart)
void handleUpdatePOST() {
  server.sendHeader("Connection", "close");
  String msg = Update.hasError() ? t("Update fehlgeschlagen! Bitte neustarten.", "Update failed! Please restart.") : t("Update erfolgreich! System startet neu...", "Update successful! System is restarting...");
  server.send(200, "text/plain", msg);
  delay(1000); ESP.restart();
}

// 3. Kümmert sich im Hintergrund um den echten Datei-Stream (Chunks)
void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("Update gestartet: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { 
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) { 
      Serial.printf("Update erfolgreich! Groesse: %u Bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

// ============================================================
// DIE SYSTEM-ANLEITUNG & DOKUMENTATION 
// ============================================================
void handleHelp() {

  // --- DER TÜRSTEHER ---
  if (USE_AUTH && !server.authenticate(authUser.c_str(), authPass.c_str())) {
    return server.requestAuthentication();
  }

  // --- RAM OPTIMIERUNG ---
  String html;
  html.reserve(32000); // Etwas mehr Platz für das fette Handbuch reservieren!
  html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += getNavbar();

  html += "<title>" + t("Hilfe & Anleitung", "Help & Manual") + "</title>";
  html += "<style>body{font-family:'Segoe UI', Tahoma, Verdana, sans-serif; background-color:#eef2f3; padding:15px; line-height:1.6;} ";
  html += ".card{background:#fff; padding:20px; border-radius:10px; margin-bottom:20px; box-shadow:0 4px 6px rgba(0,0,0,0.05);} ";
  html += "h1, h2, h3{color:#2c3e50; margin-top:0;} ";
  html += ".btn-back{background-color:#7f8c8d; color:white; padding:12px; text-decoration:none; border-radius:8px; font-weight:bold; display:block; text-align:center; margin-bottom:20px;} ";
  html += "table{width:100%; border-collapse:collapse; margin-top:10px;} th, td{border:1px solid #ddd; padding:8px; text-align:left;} th{background-color:#f4f6f7;} ";
  html += "</style></head><body>";
  
  html += "<a href='/' class='btn-back'>🔙 " + t("Zurück zum Dashboard", "Back to Dashboard") + "</a>";
  html += "<h1 style='text-align:center;'>📖 " + t("System-Handbuch", "System Manual") + "</h1>";

  // --- WICHTIGER HINWEIS ZUM NEUSTART ---
  html += "<div class='card' style='border-left: 5px solid #e74c3c; background-color:#fdf2e9;'>";
  html += "<h3 style='color:#e74c3c;'>⚠️ " + t("Wichtig: Speichern & Neustart", "Important: Save & Restart") + "</h3>";
  html += "<p>" + t("Wenn du in den Einstellungen (Setup) grundlegende Dinge wie Hardware-Pins, die Zeitzone oder das PCF8574-Modul änderst, <b>muss das System neu starten</b>. Klicke dazu immer unten auf <i>'Speichern & Neustart'</i>. Danach ist das System für ca. 5 Sekunden offline, wendet die neue Architektur an und verbindet sich neu.", "If you change basic settings like hardware pins, the timezone, or the PCF8574 module, <b>the system must reboot</b>. Always click <i>'Save & Restart'</i>. The system will be offline for about 5 seconds to apply the new architecture and reconnect.") + "</p></div>";

  // --- SYSTEM PAUSE ---
  html += "<div class='card' style='border-left: 5px solid #34495e;'><h2>⏸️ " + t("System Pause (Wartungsmodus)", "System Pause (Maintenance Mode)") + "</h2>";
  html += "<p>" + t("Auf der Hauptseite findest du einen großen Schalter, um das System zu pausieren. Ist die Pause aktiv, werden <b>alle Gießvorgänge blockiert</b>. Sensoren und Smart Rules laufen im Hintergrund weiter. Perfekt, wenn du Sensoren reinigen musst oder das System temporär stoppen willst!", "On the main page you will find a large button to pause the system. If active, <b>all watering is blocked</b>. Sensors and Smart Rules continue to run in the background. Perfect when you need to clean sensors or temporarily stop the system!") + "</p></div>";

  // --- SICHERHEIT ---
  html += "<div class='card' style='border-left: 5px solid #e74c3c;'><h2>🔒 " + t("Sicherheit & Passwort", "Security & Password") + "</h2>";
  html += "<p>" + t("Du kannst dein Dashboard in den System-Einstellungen mit einem Passwort schützen, damit niemand in deinem WLAN unerlaubt die Pumpen aktiviert. <b>Achtung:</b> Wenn du dein Passwort vergisst, musst du den ESP32 über USB neu flashen oder einen Hardware-Reset durchführen!", "You can protect your dashboard with a password in the system settings so nobody in your WiFi can activate the pumps without permission. <b>Warning:</b> If you forget your password, you must re-flash the ESP32 via USB or perform a hardware reset!") + "</p></div>";

  // --- AUTOMATOR & Smart Rules ---
  html += "<div class='card' style='border-left: 5px solid #f39c12;'><h2>🤖 " + t("Automatisierung & Smart Rules", "Automation & Smart Rules") + "</h2>";
  html += "<p>" + t("Das Gehirn deines Systems! Hier kannst du Wenn-Dann-Regeln und Zeitschaltuhren frei programmieren, um Relais, Fenster oder Pumpen zu steuern.", "The brain of your system! Program If-Then rules and timers here to control relays, windows, or pumps.") + "</p>";
  
  html += "<h3>🛡️ " + t("Prioritäten (Die Firewall-Logik)", "Priorities (Firewall Logic)") + "</h3>";
  html += "<p>" + t("Das System nutzt eine industrielle 'Priority-based State Evaluation'. Das bedeutet: Die Wichtigkeit einer Regel legst du über ihren <b>Platz (1 bis 11)</b> fest.", "The system uses industrial 'Priority-based State Evaluation'. This means: You set the importance of a rule via its <b>Rank (1 to 11)</b>.") + "</p>";
  html += "<ul><li><b>" + t("Platz 1 ist der absolute Chef:", "Rank 1 is the absolute Boss:") + "</b> " + t("Regeln auf Platz 1 überstimmen alle darunterliegenden. Wenn Regel 1 sagt 'Fenster ZU', verhängt das System eine Sperre. Regel 2 wird dann komplett ignoriert.", "Rules on Rank 1 overrule all below. If Rule 1 says 'Window CLOSE', the system locks the pin. Rule 2 is then completely ignored.") + "</li>";
  html += "<li><b>" + t("Zeitschaltuhren (Timer):", "Timers:") + "</b> " + t("Timer sollten immer eine niedrige Priorität (z.B. Platz 9-11) haben. So können sie jederzeit von einer Notfall-Sensor-Regel (z.B. Regensensor auf Platz 1) überschrieben werden.", "Timers should always have a low priority (e.g. Rank 9-11). This way they can be overwritten at any time by an emergency sensor rule (e.g. rain sensor on Rank 1).") + "</li></ul>";
  
  // --- DÜNGER-KALENDER ---
  html += "<h3>📅 " + t("Dünger-Logik & Kalender (Timer)", "Fertilizer Logic & Calendar (Timers)") + "</h3>";
  html += "<p>" + t("Die Zeitschaltuhren sind extrem smart. Du kannst im Dropdown-Menü nicht nur Uhrzeiten, sondern auch <b>Wochentage</b> (z.B. 'Nur Montags') oder <b>Intervalle</b> (z.B. 'Alle 14 Tage') auswählen. Das ist perfekt, um automatische Flüssigdünger-Pumpen oder spezielle Belüftungs-Zyklen zu steuern!", "The timers are extremely smart. In the dropdown menu, you can select not only times, but also <b>days of the week</b> (e.g. 'Mondays Only') or <b>intervals</b> (e.g. 'Every 14 Days'). This is perfect for controlling automatic liquid fertilizer pumps or special ventilation cycles!") + "</p>";

  // ---> NEU: DAS GOLD-FEATURE (BEDINGUNGEN) <---
  html += "<h4>🔗 " + t("Bedingungen (Das logische UND)", "Conditions (Logical AND)") + "</h4>";
  html += "<p>" + t("Du kannst jetzt jedem Timer eine Bedingung hinzufügen! Wähle einfach aus dem Dropdown eine Smart Rule aus. Der Timer startet dann NUR zur gewünschten Uhrzeit, WENN die verknüpfte Regel in diesem Moment auch zutrifft (z.B. Düngen nur, wenn die Erde trocken ist). Das spart komplexe Logik!", "You can now add a condition to every timer! Simply select a Smart Rule from the dropdown. The timer will then ONLY start at the scheduled time IF the linked rule is also true at that moment (e.g., fertilize only if the soil is dry). This saves complex logic!") + "</p>";

  html += "<h3>⏳ " + t("Zeiten: FÜR und PAUSE", "Timers: FOR and PAUSE") + "</h3>";
  html += "<p>" + t("Mit diesen zwei Feldern baust du Profi-Schaltungen für verschiedene Hardware:", "With these two fields you build professional circuits for different hardware:") + "</p>";
  html += "<ul><li><b>" + t("FÜR = 0:", "FOR = 0:") + "</b> " + t("Dauerhaft AN, solange die Bedingung wahr ist.", "Permanently ON as long as the condition is true.") + "</li>";
  html += "<li><b>" + t("FÜR > 0, PAUSE = 0 (One-Shot):", "FOR > 0, PAUSE = 0 (One-Shot):") + "</b> " + t("Das Relais zieht EINMAL für X Sekunden an und sperrt sich dann. Perfekt für einen Fenster-Motor! Es löst erst wieder aus, wenn die Temperatur einmal gesunken und wieder gestiegen ist.", "The relay triggers ONCE for X seconds and then locks itself. Perfect for a window motor! It only triggers again when the temperature has dropped and risen again.") + "</li>";
  html += "<li><b>" + t("FÜR > 0, PAUSE > 0 (Intervall / Blinker):", "FOR > 0, PAUSE > 0 (Interval / Blinker):") + "</b> " + t("Das Relais schaltet X Sekunden AN, dann Y Sekunden AUS und wiederholt das endlos. Perfekt für Bewässerungssysteme (z.B. 5s Sprühen, 5s Pause zum Einsickern).", "The relay switches ON for X seconds, then OFF for Y seconds and repeats endlessly. Perfect for watering systems (e.g. 5s spray, 5s pause to soak).") + "</li></ul>";

  html += "<div style='background:#f4f6f7; padding:10px; border-radius:5px;'><b>💡 " + t("Praxis-Beispiel: Gewächshaus", "Practical Example: Greenhouse") + "</b><br>";
  html += "<i>" + t("Timer (Platz 2): Fenster (Pin 18) AUF von 13:00 bis 14:00 Uhr.<br>Regel (Platz 1): WENN Regensensor < 2500, DANN Fenster (Pin 18) AUS (Zufahren).<br><b>Ergebnis:</b> Fängt es um 13:30 an zu regnen, schließt die Anlage das Fenster sofort, egal was der Timer sagt!", "Timer (Rank 2): Window (Pin 18) OPEN from 13:00 to 14:00.<br>Rule (Rank 1): IF Rain Sensor < 2500, THEN Window (Pin 18) OFF.<br><b>Result:</b> If it starts raining at 13:30, the system closes the window immediately, no matter what the timer says!") + "</i></div></div>";

  // --- LOGIK ---
  html += "<div class='card'><h2>🧠 " + t("Wie funktioniert die Gieß-Logik?", "How does the watering logic work?") + "</h2>";
  html += "<p>" + t("Das System nutzt keine simplen Echtzeit-Werte, um zu verhindern, dass die Pumpe bei einem kurzen Messfehler anspringt (z.B. wenn sich die Erde kurz verschiebt).", "The system avoids simple real-time values to prevent the pump from triggering on a brief measurement error (e.g., if the soil shifts slightly).") + "</p>";
  
  html += "<ul>";
  html += "<li><b>" + t("Phase 1 (15 Minuten):", "Phase 1 (15 Minutes):") + "</b> " + t("Das System misst alle Sensoren konstant. Nur wenn der Wert stabil und dauerhaft <i>unter</i> dem Ziel-Wert (%-Wert) bleibt, markiert es den Topf als 'trocken'.", "The system constantly measures all sensors. Only if the value remains stable and permanently <i>below</i> the target percentage, it marks the pot as 'dry'.") + "</li>";
  
  html += "<li><b>" + t("Phase 2 (30 Minuten Deep Check):", "Phase 2 (30 Mins Deep Check):") + "</b> " + t("Ein finaler Sicherheits-Countdown startet. Erst wenn der Topf nach weiteren 30 Minuten immer noch knochentrocken ist, wird gegossen. Das verhindert Überwässerung!", "A final safety countdown starts. Only if the pot is still bone dry after another 30 minutes, it will be watered. This prevents overwatering!") + "</li>";

  // --- NEUER PUNKT: SEHR NASS ---
  html += "<li><b>🌊 " + t("Sehr Nass Warnung:", "Very Wet Warning:") + "</b> " + t("Liegt die Feuchtigkeit mehr als 15% über dem Zielwert (z.B. nach starkem Regen oder manuellem Gießen), warnt dich das Dashboard mit einem blauen 'Sehr Nass' Status.", "If the humidity is more than 15% above the target value (e.g. after heavy rain or manual watering), the dashboard warns you with a blue 'Very Wet' status.") + "</li>";

  // --- NEUER PUNKT: VIRTUELLER FLOW SENSOR ---
  html += "<li><b>🏜️ " + t("Virtueller Flow-Sensor (Wasser-Alarm):", "Virtual Flow Sensor (Water Alarm):") + "</b> " + t("Wenn das System zweimal hintereinander gießt, die Feuchtigkeit in der Erde aber nicht ansteigt, stoppt es sofort. Das verhindert eine Dauer-Überschwemmung, falls der Schlauch abgerutscht ist oder der Sensor in der Luft hängt!", "If the system waters twice in a row but the soil moisture does not increase, it stops immediately. This prevents continuous flooding if the hose slips off or the sensor hangs in the air!") + "</li>";
  
  html += "<li><b>🚨 " + t("Kabelbruch-Schutz (< 3%):", "Broken Cable Protection (< 3%):") + "</b> " + t("Fällt die Feuchtigkeit unter 3%, geht das System von einem defekten Sensor aus. Um eine Überschwemmung zu vermeiden, wird das Gießen blockiert.", "If the humidity drops below 3%, the system assumes a defective sensor. To avoid flooding, watering is immediately blocked.") + "</li>";
  
  html += "<li><b>🛑 " + t("Trockenlauf-Schutz (Killswitch):", "Dry-Run Protection (Killswitch):") + "</b> " + t("Meldet der Schwimmerschalter einen leeren Wassertank, wird die betroffene Pumpe sofort hart gesperrt, damit sie nicht durchbrennt. Deine restlichen Automatisierungs-Regeln laufen aber normal weiter!", "If the float switch reports an empty water tank, the affected pump is immediately hard-locked to prevent it from burning out. Your other automation rules continue to run normally!") + "</li>";
  html += "<li><b>⏱️ " + t("Hardware Watchdog (Not-Aus):", "Hardware Watchdog (Emergency Stop):") + "</b> " + t("Während die Pumpe läuft, überwacht das System den Sensor in Echtzeit. Schießt die Feuchtigkeit plötzlich auf 10 % über den Zielwert (z.B. durch eine extrem lang eingestellte Gieß-Dauer), wird die Pumpe in Millisekunden abgewürgt und der Topf sicherheitshalber gesperrt!", "While the pump is running, the system monitors the sensor in real-time. If the moisture suddenly shoots to 10% above the target value (e.g., due to an extremely long watering time), the pump is killed in milliseconds and the pot is locked for safety!") + "</li>";
  html += "</ul></div>";

  // --- SMART SEARCH & ECO-ZONEN (NEU!!!) ---
  html += "<div class='card' style='border-left: 5px solid #2ecc71;'><h2>🔎 " + t("Smart Search & Eco-Zonen", "Smart Search & Eco-Zones") + "</h2>";
  html += "<p>" + t("Die Austrian Flame Pflanzen-Datenbank umfasst über 300 Arten aus der ganzen Welt! Sie liegt direkt auf dem internen Speicher (LittleFS) des ESP32.", "The Austrian Flame plant database includes over 300 species from all over the world! It resides directly on the ESP32's internal memory (LittleFS).") + "</p>";
  
  html += "<h3>🏷️ " + t("Die magische Suchfunktion (Tags)", "The Magic Search Function (Tags)") + "</h3>";
  html += "<p>" + t("Du musst in der Pflanzen-Suche nicht nur nach exakten Namen suchen. Jede Pflanze hat unsichtbare Tags! Probier mal im Suchfeld folgende Begriffe aus:", "You don't just have to search for exact plant names. Every plant has invisible tags! Try the following terms in the search box:") + "</p>";
  html += "<ul>";
  html += "<li><b>" + t("Kontinente & Länder:", "Continents & Countries:") + "</b> Afrika, Asien, Mediterran, Japan, Mexiko...</li>";
  html += "<li><b>" + t("Klima & Biome:", "Climate & Biomes:") + "</b> Tropen, Sumpf, Wüste, Kalt, Tropics, Swamp, Desert, Cold...</li>";
  html += "<li><b>" + t("Eigenschaften:", "Properties:") + "</b> Heilpflanze (Medicinal), Sukkulente (Succulent), Wasserpflanze (Aquatic)...</li>";
  html += "</ul>";

  html += "<h3>🛡️ " + t("Eco-Zonen Schutz (Das ⛔ Symbol)", "Eco-Zone Protection (The ⛔ Symbol)") + "</h3>";
  html += "<p>" + t("Wenn du in der Pflanzen-Konfiguration mehrere Pflanzen zu einem Topf (Zone) hinzufügst, berechnet das System den perfekten Feuchtigkeits-Durchschnitt. Wählst du z.B. einen Kaktus (15%), werden alle Sumpfpflanzen im Dropdown automatisch mit einem ⛔ gesperrt, da sie im selben Topf sterben würden!", "If you add multiple plants to a pot (zone) in the plant configuration, the system calculates the perfect moisture average. For example, if you select a cactus (15%), all swamp plants in the dropdown are automatically locked with a ⛔ because they would die in the same pot!") + "</p>";

  // --- NEU: ERKLÄRUNG ZUR PLANTS.JS ---
  html += "<h3>📁 " + t("Pflanzen-Datenbank updaten (plants.js)", "Update Plant Database (plants.js)") + "</h3>";
  html += "<p>" + t("Das System nutzt eine externe Datei namens <code>plants.js</code> für die Pflanzen-Datenbank. Warum? Damit du die Liste ganz easy austauschen oder updaten kannst! Wenn die Community eine neue Version veröffentlicht oder du deine eigenen Sorten eintragen willst, lädst du die neue Datei einfach ganz unten in den <b>Pflanzen-Einstellungen</b> hoch. Die alte Liste wird dabei automatisch durch die neue ersetzt.", "The system uses an external file called <code>plants.js</code> for the plant database. Why? So you can easily swap or update the list! If the community releases a new version or you want to add your own varieties, simply upload the new file at the very bottom of the <b>Plant Settings</b>. The old list will automatically be replaced by the new one.") + "</p></div>";

  // --- KALIBRIERUNG ---
  html += "<div class='card'><h2>🎛️ " + t("Sensoren kalibrieren (0% bis 100%)", "Calibrate Sensors (0% to 100%)") + "</h2>";
  html += "<p>" + t("Die kapazitiven Erdsensoren lesen intern keine Prozente, sondern Rohwerte (meist zwischen 1000 und 3000). So stellst du sie in den Settings perfekt ein:", "The capacitive soil sensors do not read percentages internally, but raw values (mostly between 1000 and 3000). Here is how you set them up perfectly in the settings:") + "</p>";
  html += "<ol><li>" + t("Gehe in die Settings zur Box 'Live-Rohwerte'.", "Go to the settings and find the 'Live Sensor Raw Values' box.") + "</li>";
  html += "<li>" + t("Halte den nackten Sensor in die <b>trockene Luft</b>. Notiere den Wert (z.B. 2800) und trage ihn bei <b>TROCKEN (0%)</b> ein.", "Hold the bare sensor in <b>dry air</b>. Note the value (e.g. 2800) and enter it under <b>DRY (0%)</b>.") + "</li>";
  html += "<li>" + t("Tauche den Sensor bis zur Markierung in ein <b>Glas Wasser</b>. Notiere den Wert (z.B. 1200) und trage ihn bei <b>NASS (100%)</b> ein.", "Submerge the sensor up to the mark in a <b>glass of water</b>. Note the value (e.g. 1200) and enter it under <b>WET (100%)</b>.") + "</li></ol>";
  html += "<p><i>" + t("Tipp: Der Trocken-Wert ist fast immer höher als der Nass-Wert! Das System wandelt diese beiden Endpunkte automatisch in saubere Prozente (0-100%) um.", "Tip: The dry value is almost always higher than the wet value! The system automatically converts these two endpoints into clean percentages (0-100%).") + "</i></p></div>";

  // --- HARDWARE ---
  html += "<div class='card'><h2>🔌 " + t("Hardware & Erweiterungen", "Hardware & Expansions") + "</h2>";
  
  html += "<h3>📍 " + t("System-Pins & I2C", "System Pins & I2C") + "</h3>";
  html += "<ul><li><b>💧 " + t("Haupt-Wasserversorgung", "Main Water Supply") + ":</b> " + t("Früher fest verbaut, jetzt kannst du Pumpe 1 und Schwimmer 1 in den System-Settings frei zuweisen!", "Previously hardcoded, you can now freely assign Pump 1 and Float 1 in the system settings!") + "</li>";
  html += "<li><b>🔌 " + t("I2C Bus (SDA/SCL)", "I2C Bus (SDA/SCL)") + ":</b> Pin D21 & D22 <i>(" + t("Fest verdrahtet! Das PCF-Erweiterungsmodul und ein lokales OLED-Display teilen sich diese beiden Pins.", "Hardwired! The PCF expansion module and a local OLED display share these two pins.") + ")</i></li></ul>";
  html += "<p style='font-size:13px; color:#7f8c8d; border-left:3px solid #bdc3c7; padding-left:10px;'>" + t("Tipp zum Mikrocontroller: Das System ist exakt für den Standard 38-Pin ESP32 (WROOM) maßgeschneidert. Kleinere Boards (wie der D1 Mini / ESP8266) haben zu wenig Pins, und noch größere Boards (wie der ESP32-S3 oder Mega) sind für dieses Projekt reine Verschwendung.", "Microcontroller Tip: The system is tailor-made for the standard 38-pin ESP32 (WROOM). Smaller boards (like the D1 Mini / ESP8266) lack pins, and even larger boards (like ESP32-S3 or Mega) are a pure waste for this project.") + "</p>";

  html += "<h3>📺 " + t("Lokales OLED-Display (0.96 Zoll)", "Local OLED Display (0.96 Inch)") + "</h3>";
  html += "<p>" + t("Das System unterstützt ein klassisches 0.96 Zoll OLED-Display (SSD1306, 128x64 Pixel). Es wird an den I2C-Bus angeschlossen (SDA=21, SCL=22). Da das Hauptsystem für maximale Stabilität und Performance optimiert ist, werden lokal keine großen Farb-Displays (TFT/SPI) unterstützt. Wenn du ein großes Display möchtest, nutze die ESP-NOW Touch-Display (CYD) Funktion!", "The system supports a classic 0.96 inch OLED display (SSD1306, 128x64 pixels). It connects to the I2C bus (SDA=21, SCL=22). Because the main system is optimized for maximum stability and performance, large color displays (TFT/SPI) are not supported locally. If you want a large display, use the ESP-NOW Touch Display (CYD) feature!") + "</p>";

  html += "<h3>📟 " + t("PCF8574 (I2C Pin-Erweiterung)", "PCF8574 (I2C Pin Expander)") + "</h3>";
  html += "<p>" + t("Gehen dir die Relais-Pins am ESP32 aus? Aktiviere dieses Modul in den Settings, und du erhältst 8 zusätzliche digitale Ausgänge (PCF P0 bis P7). <b>Wichtig:</b> Schwimmer-Sensoren müssen immer an die normalen ESP-Pins, Pumpen und Ventile können an den PCF!", "Running out of relay pins on the ESP32? Activate this module in the settings, and you get 8 additional digital outputs (PCF P0 to P7). <b>Important:</b> Float sensors must always connect to normal ESP pins, pumps and valves can go to the PCF!") + "</p>";

  html += "<h3>🌦️ " + t("Freie Sensoren (Extra Hardware)", "Custom Sensors (Extra Hardware)") + "</h3>";
  html += "<p>" + t("In den Settings kannst du bis zu 2 freie Sensoren (z.B. Regensensor, Lichtsensor) benennen und einem Analog-Pin zuweisen. Diese Sensoren bewässern keine Pflanzen, tauchen aber in deinem Smart Rules als Auslöser für eigene Regeln auf!", "In the settings you can name up to 2 custom sensors (e.g. rain sensor, light sensor) and assign an analog pin. These sensors do not water plants, but appear in your Smart Rules as triggers for custom rules!") + "</p>";

  html += "<h3>" + t("Relais & Magnetventile", "Relays & Solenoid Valves") + "</h3>";
  html += "<p>" + t("Bitte versorge Pumpen und Ventile <b>nicht</b> über den 5V-Pin des ESP32, da dieser sonst durchbrennt!", "Please do <b>not</b> power pumps and valves via the 5V pin of the ESP32, otherwise it will burn out!") + "</p>";
  html += "<ul><li><b>" + t("Stromversorgung:", "Power Supply:") + "</b> " + t("Nutze ein externes 5V Netzteil (mind. 2-3A).", "Use an external 5V power supply (min. 2-3A).") + "</li>";
  html += "<li><b>" + t("WICHTIG (GND):", "IMPORTANT (GND):") + "</b> " + t("Du musst das schwarze GND-Kabel vom externen Netzteil zwingend mit einem GND-Pin am ESP32 verbinden!", "You must connect the black GND wire from the external power supply to a GND pin on the ESP32!") + "</li></ul>";
  
  html += "<h3>📡 " + t("ESP-NOW Touch-Display (CYD Hub)", "ESP-NOW Touch Display (CYD Hub)") + "</h3>";
  html += "<p>" + t("Das System funkt Live-Daten direkt an ein oder bis zu drei <b>CYD (Cheap Yellow Display)</b> Touch-Screens. Aktiviere in den Settings 'ESP-NOW' und trage die MAC-Adresse(n) ein.", "The system broadcasts live data directly to one or three <b>CYD (Cheap Yellow Display)</b> touch screens. Enable 'ESP-NOW' in the settings and enter the MAC address(es).") + "<br>";
  html += "<b>" + t("Mehrere Displays:", "Multiple Displays:") + "</b> " + t("Trenne die MAC-Adressen einfach mit einem Strichpunkt (;).", "Simply separate the MAC addresses with a semicolon (;).") + "<br>";
  html += "<i style='color:#7f8c8d;'>" + t("Beispiel: ", "Example: ") + "AA:BB:CC:DD:EE:FF; 11:22:33:44:55:66</i><br><br>";
  html += t("Dein Hauptsystem funkt ab sofort kabellos alle Live-Daten durch die Wände an alle Displays!", "Your main system will wireless broadcast all live data through the walls to all displays!") + "</p></div>";

  // --- DUAL PUMPEN & FAILOVER ---
  html += "<div class='card'><h2>🔄 " + t("Zweiter Kreislauf & Ausfallsicherheit", "Second Circuit & Failover") + "</h2>";
  html += "<p>" + t("Wenn du den zweiten Kreislauf aktiviert hast, kannst du für jeden Topf einzeln bestimmen, woher das Wasser kommt:", "If you enabled the second circuit, you can determine the water source for each pot individually:") + "</p>";
  
  html += "<ul>";
  html += "<li><b>" + t("Haupt-Pumpe 1:", "Main Pump 1:") + "</b> " + t("Nutzt die primäre Regentonne.", "Uses the primary rain barrel.") + "</li>";
  html += "<li><b>" + t("Pumpe 2 (Kreislauf 2):", "Pump 2 (Circuit 2):") + "</b> " + t("Gießt diesen Topf dauerhaft aus dem zweiten Tank oder dem Festwasseranschluss (perfekt zur Trennung von Pflanzengruppen).", "Waters this pot permanently from the second tank or direct water line (perfect for separating plant groups).") + "</li>";
  html += "<li><b>" + t("Auto-Fallback (1 -> 2):", "Auto-Fallback (1 -> 2):") + "</b> " + t("Der Notfall-Modus! Das System versucht aus Tank 1 zu gießen. Ist dieser leer, schaltet es nahtlos auf Pumpe/Ventil 2 um.", "The emergency mode! The system tries to water from tank 1. If it's empty, it switches seamlessly to pump/valve 2.") + "</li>";
  html += "</ul>";
  
  // Optische Trennung innerhalb der Box
  html += "<hr style='border:1px dashed #ccc; margin:15px 0;'>"; 
  
  html += "<h3>🚰 " + t("Festwasseranschluss (Gartenschlauch)", "Direct Water Line (Garden Hose)") + "</h3>";
  html += "<p>" + t("Möchtest du statt einer zweiten Regentonne direkt die Wasserleitung (mit einem Magnetventil) nutzen? Wähle in den Einstellungen beim <b>Pin 2. Tank-Sensor</b> einfach <i>'Keiner (Festwasser) / -1'</i> aus. Das System weiß dann, dass diese Wasserquelle niemals leer wird, und umgeht den Trockenlaufschutz für Kreislauf 2 ganz automatisch.", "Want to use a direct water line (with a solenoid valve) instead of a second rain barrel? Just select <i>'None (Direct Line) / -1'</i> for the <b>Pin 2nd Tank Sensor</b> in the settings. The system will then know this water source never runs dry and automatically bypasses the dry-run protection for circuit 2.") + "</p></div>";

  // --- DATENBANK (PFLANZEN-RICHTWERTE) ---
  html += "<div class='card'><h2>🪴 " + t("Pflanzen-Datenbank (Richtwerte)", "Plant Database (Guidelines)") + "</h2>";
  html += "<table><tr><th>" + t("Pflanze", "Plant") + "</th><th>" + t("Gießen ab", "Water at") + "</th><th>" + t("Kategorie & Info", "Category & Info") + "</th></tr>";

  // Gruppe 1: Kräuter
  html += "<tr style='background:#e8f8f5;'><td colspan='3'><b>🌿 " + t("Kräuter", "Herbs") + "</b></td></tr>";
  html += "<tr><td>" + t("Basilikum", "Basil") + "</td><td>55% - 65%</td><td>" + t("Darf nie ganz austrocknen.", "Must never dry out completely.") + "</td></tr>";
  html += "<tr><td>" + t("Petersilie", "Parsley") + "</td><td>50% - 60%</td><td>" + t("Mag es gleichmäßig feucht.", "Likes it evenly moist.") + "</td></tr>";
  html += "<tr><td>" + t("Schnittlauch", "Chives") + "</td><td>45% - 55%</td><td>" + t("Staunässe unbedingt vermeiden.", "Avoid waterlogging at all costs.") + "</td></tr>";
  html += "<tr><td>" + t("Oregano / Majoran", "Oregano / Marjoram") + "</td><td>30% - 40%</td><td>" + t("Sehr trockenheitstolerant.", "Very drought tolerant.") + "</td></tr>";
  html += "<tr><td>" + t("Rosmarin / Thymian", "Rosemary / Thyme") + "</td><td>20% - 30%</td><td>" + t("Mediterran! Ertrinken sehr schnell.", "Mediterranean! Drown very quickly.") + "</td></tr>";
  html += "<tr><td>" + t("Minze", "Mint") + "</td><td>70% - 80%</td><td>" + t("Extremer Säufer, breitet sich stark aus.", "Extreme drinker, spreads rapidly.") + "</td></tr>";

  // Gruppe 2: Beeren & Scharfes
  html += "<tr style='background:#fdedec;'><td colspan='3'><b>🍓 " + t("Beeren, Chilis & Gewürze", "Berries, Chilies & Spices") + "</b></td></tr>";
  html += "<tr><td>" + t("Chili / Peperoni", "Chili / Hot Peppers") + "</td><td>40% - 50%</td><td>" + t("Leichter Trockenstress erhöht die Schärfe!", "Slight drought stress increases heat!") + "</td></tr>";
  html += "<tr><td>" + t("Paprika", "Bell Peppers") + "</td><td>50% - 60%</td><td>" + t("Brauchen es etwas feuchter als Chilis.", "Need it slightly moister than chilies.") + "</td></tr>";
  html += "<tr><td>" + t("Erdbeeren", "Strawberries") + "</td><td>60% - 70%</td><td>" + t("Flachwurzler, trocknen an der Oberfläche schnell aus.", "Shallow roots, dry out quickly at the surface.") + "</td></tr>";
  html += "<tr><td>" + t("Himbeeren / Brombeeren", "Raspberries / Blackberries") + "</td><td>55% - 65%</td><td>" + t("Gleichmäßig feucht halten, keine Staunässe.", "Keep evenly moist, no waterlogging.") + "</td></tr>";
  html += "<tr><td>" + t("Pfeffer", "Pepper") + "</td><td>60% - 75%</td><td>" + t("Tropisch. Hohe Luftfeuchtigkeit ideal.", "Tropical. High air humidity ideal.") + "</td></tr>";

  // Gruppe 3: Großes Gemüse
  html += "<tr style='background:#fef9e7;'><td colspan='3'><b>🍅 " + t("Großes Gemüse", "Large Vegetables") + "</b></td></tr>";
  html += "<tr><td>" + t("Tomaten", "Tomatoes") + "</td><td>60% - 70%</td><td>" + t("Konstant feucht halten, sonst platzen die Früchte.", "Keep constantly moist, otherwise fruits split.") + "</td></tr>";
  html += "<tr><td>" + t("Gurken", "Cucumbers") + "</td><td>70% - 80%</td><td>" + t("Bestehen fast nur aus Wasser. Viel gießen!", "Mostly water. Water heavily!") + "</td></tr>";
  html += "<tr><td>" + t("Zucchini", "Zucchini") + "</td><td>60% - 70%</td><td>" + t("Große Blätter verdunsten extrem viel Wasser.", "Large leaves evaporate a lot of water.") + "</td></tr>";
  html += "<tr><td>" + t("Kürbis", "Pumpkin") + "</td><td>50% - 60%</td><td>" + t("Tiefwurzler. Seltener, aber durchdringend gießen.", "Deep roots. Water less often, but thoroughly.") + "</td></tr>";
  html += "<tr><td>" + t("Aubergine", "Eggplant") + "</td><td>65% - 75%</td><td>" + t("Braucht viel Wärme und gleichmäßige Feuchtigkeit.", "Needs a lot of heat and even moisture.") + "</td></tr>";

  // Gruppe 4: Tropisch & Zimmerpflanzen
  html += "<tr style='background:#eaf2f8;'><td colspan='3'><b>🌴 " + t("Tropisch & Zimmerpflanzen", "Tropical & Indoor") + "</b></td></tr>";
  html += "<tr><td>" + t("Monstera / Philodendron", "Monstera / Philodendron") + "</td><td>40% - 50%</td><td>" + t("Erde zwischen dem Gießen gut antrocknen lassen.", "Let soil dry out well between watering.") + "</td></tr>";
  html += "<tr><td>" + t("Calathea / Maranta", "Calathea / Maranta") + "</td><td>65% - 75%</td><td>" + t("Verzeiht absolute Trockenheit fast nie.", "Almost never forgives absolute drought.") + "</td></tr>";
  html += "<tr><td>" + t("Sukkulenten / Kakteen", "Succulents / Cacti") + "</td><td>10% - 20%</td><td>" + t("Wüstenpflanzen. Erde fast komplett austrocknen lassen.", "Desert plants. Let soil dry almost completely.") + "</td></tr>";
  
  html += "</table>";
  html += "<p style='font-size:12px; color:#95a5a6; margin-top:10px;'><i>* " + t("Alle Angaben ohne Gewähr. Die idealen Werte hängen stark von der verwendeten Erde, Topfgröße und dem Standort ab.", "All values without guarantee. Ideal values depend heavily on the soil used, pot size, and location.") + "</i></p></div>";

  // --- NEU: MQTT API DOKUMENTATION FÜR DIE COMMUNITY ---
  html += "<div class='card' style='border-left: 5px solid #8e44ad;'><h2>🕹️ " + t("MQTT API (Two-Way-Sync)", "MQTT API (Two-Way-Sync)") + "</h2>";
  html += "<p>" + t("Für alle Node-RED und Smart-Home Bastler: Das System sendet nicht nur Sensordaten, sondern hört auch auf Befehle (Command Topics)!", "For all Node-RED and smart home tinkerers: The system not only sends sensor data, but also listens for commands (Command Topics)!") + "</p>";
  html += "<ul>";
  html += "<li><b>" + t("Ziel-Feuchtigkeit ändern:", "Change Target Humidity:") + "</b><br>";
  html += t("Sende eine Zahl (0-100) an das Topic ", "Send a number (0-100) to the topic ") + "<code>" + mqttTopic + "/cmd/ziel/X</code><br>";
  html += t("<i>(X = Topf-Nummer von 0 bis 5)</i>. Das System speichert den neuen Wert sofort live im RAM und dauerhaft auf dem Flash-Speicher!", "<i>(X = Pot number from 0 to 5)</i>. The system instantly saves the new value live in RAM and permanently to the flash memory!") + "</li>";
  html += "</ul></div>";

  // --- OPEN SOURCE LIZENZEN & CREDITS ---
  html += "<div class='card' style='border-left: 5px solid #bdc3c7; margin-top:30px;'><h2>📜 " + t("Open Source Lizenzen", "Open Source Licenses") + "</h2>";
  html += "<p style='font-size:13px; color:#7f8c8d;'>" + t("Dieses Projekt ('Austrian Flame') nutzt die folgenden Open-Source-Bibliotheken. Ein großes Dankeschön an die weltweite Entwickler-Community!", "This project ('Austrian Flame') uses the following open-source libraries. A huge thanks to the worldwide developer community!") + "</p>";
  
  html += "<ul style='font-size:12px; color:#34495e; line-height:1.8;'>";
  html += "<li><b>ESP32 Arduino Core</b> (WiFi, WebServer, Wire, ArduinoOTA, Preferences, LittleFS, ESPmDNS, esp_now, Update) - <i>LGPL / Apache 2.0 License</i></li>";
  html += "<li><b>WiFiManager</b> by tzapu - <i>MIT License</i></li>";
  html += "<li><b>ArduinoJson</b> by Benoit Blanchon - <i>MIT License</i></li>";
  html += "<li><b>PubSubClient</b> (MQTT) by Nick O'Leary - <i>MIT License</i></li>";
  html += "<li><b>Adafruit GFX & SSD1306</b> by Adafruit Industries - <i>BSD License</i></li>";
  html += "<li><b>DHT Sensor Library</b> by Adafruit - <i>MIT License</i></li>";
  html += "<li><b>PCF8574</b> (I2C Expander) - <i>MIT License</i></li>";
  html += "</ul>";
  
  html += "<p style='font-size:11px; color:#95a5a6; border-top:1px solid #ecf0f1; padding-top:10px; margin-bottom:0;'>" + t("Die jeweiligen Lizenzen erlauben die freie und kommerzielle Nutzung unter Nennung der Urheber. Die originalen Quellcodes und Lizenztexte der eingebundenen Bibliotheken sind auf GitHub frei verfügbar.", "The respective licenses allow free and commercial use with attribution to the authors. The original source codes and license texts of the included libraries are freely available on GitHub.") + "</p>";
  html += "</div>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ============================================================
// DIE LIVE-DIAGNOSE & LOG SEITE (Hacker-Style)
// ============================================================
void handleDiagnose() {
  if (USE_AUTH && !server.authenticate(authUser.c_str(), authPass.c_str())) return server.requestAuthentication();

  String html; html.reserve(6000);
  html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><meta http-equiv='refresh' content='5'>";
  html += getNavbar();
  html += "<title>" + t("System-Diagnose", "System Diagnostics") + "</title>";
  html += "<style>body{font-family:'Segoe UI', Tahoma, Verdana, sans-serif; background-color:#1e1e1e; color:#ecf0f1; padding:15px;} ";
  html += ".terminal{background:#000; padding:15px; border-radius:8px; border-left:4px solid #2ecc71; font-family:'Courier New', monospace; font-size:14px; line-height:1.5; overflow-y:auto; height:300px; box-shadow:inset 0 0 10px rgba(0,0,0,0.8);} ";
  html += ".stats{display:flex; flex-wrap:wrap; gap:10px; margin-bottom:20px;} .stat-box{background:#2c3e50; padding:15px; border-radius:8px; flex:1; min-width:140px; text-align:center;} ";
  html += ".stat-val{font-size:24px; font-weight:bold; color:#3498db; margin-top:5px;} ";
  html += ".btn-back{background-color:#7f8c8d; color:white; padding:12px; text-decoration:none; border-radius:8px; font-weight:bold; display:block; text-align:center; margin-bottom:20px;} ";
  html += "</style></head><body>";

  html += "<a href='/' class='btn-back'>🔙 " + t("Zurück zum Dashboard", "Back to Dashboard") + "</a>";
  html += "<h2 style='text-align:center; color:#2ecc71;'>🩺 " + t("Live-Diagnose", "Live Diagnostics") + "</h2>";

  // System Stats Boxen
  html += "<div class='stats'>";
  html += "<div class='stat-box'><div>" + t("Freier RAM", "Free RAM") + "</div><div class='stat-val'>" + String(ESP.getFreeHeap() / 1024) + " KB</div></div>";
  html += "<div class='stat-box'><div>" + t("WLAN Signal", "WiFi Signal") + "</div><div class='stat-val'>" + String(WiFi.RSSI()) + " dBm</div></div>";
  html += "<div class='stat-box'><div>" + t("Uptime", "Uptime") + "</div><div class='stat-val'>" + String(millis() / 60000) + " Min</div></div>";
  html += "<div class='stat-box'><div>" + t("Letzter Check", "Last Check") + "</div><div class='stat-val'>" + aktuelleZeit + "</div></div>";
  html += "</div>";

  // Das Terminal Fenster
  html += "<h3>🖥️ " + t("Ereignis-Protokoll (Live)", "Event Log (Live)") + "</h3>";
  html += "<div class='terminal' id='term'>";
  for(int i=0; i<MAX_LOG_LINES; i++) {
    if(sysLogs[i] != "") {
      html += "<div style='color:#2ecc71;'>" + sysLogs[i] + "</div>";
    }
  }
  if(sysLogs[MAX_LOG_LINES-1] == "") html += "<div style='color:#7f8c8d;'><i>" + t("Noch keine Ereignisse aufgezeichnet...", "No events recorded yet...") + "</i></div>";
  html += "</div>";

  // Auto-Scroll Script für das Terminal
  html += "<script>var term = document.getElementById('term'); term.scrollTop = term.scrollHeight;</script>";
  
  html += "<p style='font-size:12px; color:#7f8c8d; text-align:center; margin-top:20px;'>" + t("Diese Seite aktualisiert sich alle 5 Sekunden automatisch. Die Logs werden beim Neustart aus dem RAM gelöscht.", "This page refreshes automatically every 5 seconds. Logs are cleared from RAM upon restart.") + "</p>";
  
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ============================================================
// DIE VIRTUELLE PIN-SCHALTZENTRALE
// ============================================================

void schaltePin(int pin, int status) {
  if (pin == -1) return; // Pin ist deaktiviert / Keiner ausgewählt
  
  if (pin < 100) {
    // Es ist ein echter ESP32-Pin (z.B. Pin 27)
    digitalWrite(pin, status);
  } else {
    // Pin ist 100 oder höher -> Das Signal geht an den PCF8574!
    if (USE_PCF) {
      int pcfPin = pin - 100; // Aus 103 wird P3
      pcf.write(pcfPin, status);
    }
  }
}

// ============================================================
// MQTT TELEMETRIE (DATEN AN HOME ASSISTANT SENDEN)
// ============================================================
void sendeMQTTDaten() {
  if (!USE_MQTT || !mqttClient.connected()) return;

  // Wir packen alle Daten in ein schickes, universelles JSON-Paket!
  JsonDocument doc;
  
  doc["system"] = systemName;
  doc["tank"] = tankVoll ? "OK" : t("LEER", "EMPTY");
  if (ZWEITER_KREISLAUF) doc["tank2"] = tankVoll2 ? "OK" : t("LEER", "EMPTY");
  doc["phase"] = aktuellePhase;
  doc["system_status"] = systemPausiert ? "PAUSE" : "AKTIV";
  
  // Klima-Daten
  for(int i=0; i<ANZAHL_DHT; i++) {
    doc["klima"][String(i+1)]["temp"] = aktuelleTemp[i];
    doc["klima"][String(i+1)]["luft"] = aktuelleLuft[i];
  }
  
// Pflanzen-Daten
  for(int i=0; i<ANZAHL_TOEPFE; i++) {
    String potName = "topf_" + String(i+1);
    doc["pflanzen"][potName]["name"] = NAME_TOPF[i];
    doc["pflanzen"][potName]["feuchte"] = aktuelleErde[i];
    doc["pflanzen"][potName]["ziel"] = ZIEL_FEUCHTIGKEIT[i];
    doc["pflanzen"][potName]["letztes_giessen"] = letztesGiessen[i];
    
   // --- NEU: SUPER SMARTER STATUS FÜR HOME ASSISTANT ---
        String werteText = " (" + String(aktuelleErde[i]) + "% / " + String(ZIEL_FEUCHTIGKEIT[i]) + "%)";
        
        if (pumpenSperre[i]) {
            doc["pflanzen"][potName]["status"] = "ALARM" + werteText;
        } else if (aktuellePhase == 2 && imDeepCheck[i]) {
            doc["pflanzen"][potName]["status"] = "CHECK" + werteText;
        } else if (aktuelleErde[i] <= (ZIEL_FEUCHTIGKEIT[i] - 5)) { // <--- HIER IST DER FIX!
            doc["pflanzen"][potName]["status"] = t("TROCKEN", "DRY") + werteText;
        } else if (aktuelleErde[i] > (ZIEL_FEUCHTIGKEIT[i] + 15)) {
            // NEU: Wenn 15% über Zielwert, zeige Überwässerung an!
            doc["pflanzen"][potName]["status"] = t("SEHR NASS", "TOO WET") + werteText;
        } else {
            doc["pflanzen"][potName]["status"] = "OK" + werteText;
        }
  }

  // Den JSON-Baum in einen Text umwandeln
  String output;
  serializeJson(doc, output);
  
  // Und ab damit ins Postamt!
  String topic = mqttTopic + "/state";
  mqttClient.publish(topic.c_str(), output.c_str());
  
  Serial.println("📤 MQTT Telemetrie gesendet: " + output);
}

// ============================================================
// DER WÄCHTER 3.0: STRICT PRIORITY OVERRULING (FIREWALL)
// ============================================================
void runAutomation() {
  unsigned long currentMillis = millis();
  siemBlockWatering = false; // Schild wird jede Sekunde frisch bewertet

  int wunschZustand[110];
  for (int i = 0; i < 110; i++) { wunschZustand[i] = -1; pinGesperrt[i] = false; }


  // 🚨 INTELLIGENTER HARDWARE-KILLSWITCH (Trockenlaufschutz)
  if (!tankVoll) {
    if (!pinGesperrt[PIN_PUMPE]) addLog(t("Tank leer! Pumpe gesperrt.", "Tank empty! Pump blocked.")); // <--- LOG
    wunschZustand[PIN_PUMPE] = HIGH; 
    pinGesperrt[PIN_PUMPE] = true;   
  }
  
  if (ZWEITER_KREISLAUF && !tankVoll2) {
    wunschZustand[PIN_PUMPE2] = HIGH; // Zwingend AUS
    pinGesperrt[PIN_PUMPE2] = true;   // Schloss dran!
  }

  struct tm timeinfo;

  bool timeValid = getLocalTime(&timeinfo);
  String aktuelleHHMM = "";
  if (timeValid) {
    char timeBuff[9]; strftime(timeBuff, sizeof(timeBuff), "%H:%M:%S", &timeinfo);
    aktuelleHHMM = String(timeBuff);
  }

  // WIR ARBEITEN STRENG VON PLATZ 1 (CHEF) BIS PLATZ 11 (LETZTER) AB!
  for (int platz = 1; platz <= 11; platz++) {
    
    // --- TIMER PRÜFEN ---
    for (int i = 0; i < 3; i++) {
      if (timerPrio[i] == platz) {
        int p = mosfetPin[i];
        if (p < 0 || p >= 110) continue;

        bool shouldBeOn = false;
        if (timeValid) {
          
          // 1. STIMMT DIE UHRZEIT?
          bool timeMatch = false;
          if (mosfetStart[i] < mosfetStop[i]) {
            if (aktuelleHHMM >= mosfetStart[i] && aktuelleHHMM < mosfetStop[i]) timeMatch = true;
          } else { 
            if (aktuelleHHMM >= mosfetStart[i] || aktuelleHHMM < mosfetStop[i]) timeMatch = true;
          }

          // 2. STIMMT DER TAG / DÜNGER-MODUS?
          bool dayMatch = false;
          int wday = timeinfo.tm_wday; // 0=Sonntag, 1=Montag... 6=Samstag
          int mday = timeinfo.tm_mday; // Tag des Monats (1 bis 31)
          int yday = timeinfo.tm_yday; // Tag des Jahres (0-365)
          int mode = mosfetMode[i];

          if (mode == 0) dayMatch = true; // Jeden Tag
          else if (mode == 1 && wday == 1) dayMatch = true; // Nur Montags
          else if (mode == 2 && wday == 2) dayMatch = true; // Nur Dienstags
          else if (mode == 3 && wday == 3) dayMatch = true; // Nur Mittwochs
          else if (mode == 4 && wday == 4) dayMatch = true; // Nur Donnerstags
          else if (mode == 5 && wday == 5) dayMatch = true; // Nur Freitags
          else if (mode == 6 && wday == 6) dayMatch = true; // Nur Samstags
          else if (mode == 7 && wday == 0) dayMatch = true; // Nur Sonntags
          else if (mode == 8 && (yday % 3 == 0)) dayMatch = true; // Alle 3 Tage
          else if (mode == 9 && (yday % 14 == 0)) dayMatch = true; // Alle 14 Tage
          else if (mode == 10 && (yday % 21 == 0)) dayMatch = true; // Alle 21 Tage
          else if (mode == 11 && mday == 1) dayMatch = true; // Immer am 1. des Monats

          // 3. WENN UHRZEIT UND TAG STIMMEN -> PRÜFEN, OB EINE REGEL VERKNÜPFT IST!
          if (timeMatch && dayMatch) {
              shouldBeOn = true; 
              
              // 4. NEU: DIE UX-VERKNÜPFUNG PRÜFEN!
              if (timerLink[i] > 0) {
                  int rIdx = timerLink[i] - 1; // Index 0-7 für das Regel-Array
                  
                  // Nur checken, wenn die verknüpfte Regel auch existiert und aktiv ist
                  if (ruleActive[rIdx] && ruleTrigger[rIdx] > 0) {
                      float currentVal = -999; int trig = ruleTrigger[rIdx];
                      
                      // Sensorwert der Fremd-Regel auslesen
                      if (trig >= 1 && trig <= 3) currentVal = aktuelleTemp[trig - 1];
                      else if (trig >= 4 && trig <= 6) currentVal = aktuelleLuft[trig - 4];
                      else if (trig >= 10 && trig <= 15) currentVal = aktuelleErde[trig - 10];
                      else if (trig >= 20 && trig <= 21 && extraSensorPin[trig - 20] != -1) currentVal = analogRead(extraSensorPin[trig - 20]);
                      else if (trig == 30) currentVal = mqttSensorWert[0];
                      else if (trig == 31) currentVal = mqttSensorWert[1];

                      if (currentVal != -999 && !isnan(currentVal)) {
                          bool conditionMet = false;
                          if (ruleCondition[rIdx] == 0) conditionMet = (currentVal < ruleValue[rIdx]);
                          else if (ruleCondition[rIdx] == 1) conditionMet = (currentVal > ruleValue[rIdx]);
                          
                          // Wenn die Fremd-Regel NICHT zutrifft (z.B. Erde ist noch nass), blockieren wir den Timer!
                          if (!conditionMet) {
                              shouldBeOn = false; 
                          }
                      } else {
                          shouldBeOn = false; // Sensor kaputt? Sicherheitshalber nicht düngen!
                      }
                  } else {
                      shouldBeOn = false; // Wenn der User eine Regel verknüpft, sie aber auf "Inaktiv" steht -> Blockieren!
                  }
              }
          }
        } // <-- Ende von if(timeValid)

        // FIREWALL LOGIK (Schloss setzen):
        if (shouldBeOn) {
          if (!pinGesperrt[p]) { wunschZustand[p] = LOW; pinGesperrt[p] = true; } 
        } else {
          if (!pinGesperrt[p]) { wunschZustand[p] = HIGH; } 
        }
      }
    }

    // --- REGELN PRÜFEN ---
    for (int i = 0; i < MAX_RULES; i++) {
      if (rulePrio[i] == platz) {
        if (!ruleActive[i] || ruleTrigger[i] == 0) continue;

        float currentVal = -999; int trig = ruleTrigger[i];
        if (trig >= 1 && trig <= 3) currentVal = aktuelleTemp[trig - 1];
        else if (trig >= 4 && trig <= 6) currentVal = aktuelleLuft[trig - 4];
        else if (trig >= 10 && trig <= 15) currentVal = aktuelleErde[trig - 10];
        else if (trig >= 20 && trig <= 21 && extraSensorPin[trig - 20] != -1) currentVal = analogRead(extraSensorPin[trig - 20]);
        else if (trig == 30) currentVal = mqttSensorWert[0]; // MQTT 1 einlesen
        else if (trig == 31) currentVal = mqttSensorWert[1]; // MQTT 2 einlesen

        if (currentVal == -999 || isnan(currentVal)) continue; 

        bool conditionMet = false;
        if (ruleCondition[i] == 0) conditionMet = (currentVal < ruleValue[i]);
        else if (ruleCondition[i] == 1) conditionMet = (currentVal > ruleValue[i]);

        int p1 = ruleAction1[i]; int p2 = ruleAction2[i];
        int mode = ruleMode[i];
        int targetState = (mode == 0) ? LOW : HIGH; // 0 = Relais AN, 1 = Relais AUS/GESPERRT

        if (conditionMet) {
          // BEDINGUNG WAHR: Aktion ausführen!
          if (ruleDuration[i] == 0) {
            // DAUERHAFT AN / DAUERHAFT GESPERRT
            if ((p1 == 999 || p2 == 999) && mode == 1) siemBlockWatering = true; // SYSTEM SPERRE!
            if (p1 >= 0 && p1 < 110 && !pinGesperrt[p1]) { wunschZustand[p1] = targetState; pinGesperrt[p1] = true; }
            if (p2 >= 0 && p2 < 110 && !pinGesperrt[p2]) { wunschZustand[p2] = targetState; pinGesperrt[p2] = true; }
          } else {
            // TIMER / INTERVALL MODUS
            if (ruleTimer[i] == 0) ruleTimer[i] = currentMillis; 
            
            unsigned long elapsed = currentMillis - ruleTimer[i];
            unsigned long durMs = ruleDuration[i] * 1000UL;
            unsigned long pauMs = rulePause[i] * 1000UL;

            if (elapsed <= durMs) {
              // PHASE 1: Aktion läuft
              if ((p1 == 999 || p2 == 999) && mode == 1) siemBlockWatering = true; // SYSTEM SPERRE!
              if (p1 >= 0 && p1 < 110 && !pinGesperrt[p1]) { wunschZustand[p1] = targetState; pinGesperrt[p1] = true; }
              if (p2 >= 0 && p2 < 110 && !pinGesperrt[p2]) { wunschZustand[p2] = targetState; pinGesperrt[p2] = true; }
            } else {
              // PHASE 2: Cooldown (Immer alles auf HIGH = Aus / Entsperrt)
              if (p1 >= 0 && p1 < 110 && !pinGesperrt[p1]) { wunschZustand[p1] = HIGH; pinGesperrt[p1] = true; }
              if (p2 >= 0 && p2 < 110 && !pinGesperrt[p2]) { wunschZustand[p2] = HIGH; pinGesperrt[p2] = true; }
              if (pauMs > 0 && elapsed > (durMs + pauMs)) ruleTimer[i] = currentMillis; 
            }
          }
        } else {
          // BEDINGUNG FALSCH: Relais dauerhaft aus (Schloss wird entfernt, Timer genullt)
          ruleTimer[i] = 0;
          if (p1 >= 0 && p1 < 110 && !pinGesperrt[p1]) wunschZustand[p1] = HIGH;
          if (p2 >= 0 && p2 < 110 && !pinGesperrt[p2]) wunschZustand[p2] = HIGH;
        }
      }
    }
  }

  // --- HARDWARE SCHALTEN ---
  for (int p = 0; p < 110; p++) {
    if (wunschZustand[p] != -1) schaltePin(p, wunschZustand[p]);
  }
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  server.handleClient(); 
  ArduinoOTA.handle(); 

  if (USE_MQTT) {
    if (!mqttClient.connected()) reconnectMQTT();
    mqttClient.loop();
  }

  unsigned long loopTime = millis();
  
// DEIN NEUER AUTOMATOR-WÄCHTER (Läuft 1x pro Sekunde)
  static unsigned long lastAutoCheck = 0;
  if (loopTime - lastAutoCheck > 1000) {
    runAutomation();
    lastAutoCheck = loopTime;
  }

  
  int werteP1[MAX_TOEPFE][15]; 
  aktuellePhase = 1; 
  

  for(int min = 0; min < 15; min++) {
    minutenInPhase = min; 
    String p1Text = "P1 (" + String(min + 1) + "/15)";
    warteUndRotiere(MINUTE_IN_MS, p1Text); 
    for(int i=0; i<ANZAHL_TOEPFE; i++) werteP1[i][min] = aktuelleErde[i];

    // --- NEU: ESP-NOW UPDATE JEDE MINUTE IN PHASE 1 ---
    if(ESP_NOW_ACTIVE) sendeDaten();
  }

  bool brauchtWasser = false; 
  int ergebnisP1[MAX_TOEPFE];
  
  for(int i=0; i<ANZAHL_TOEPFE; i++) {
    imDeepCheck[i] = false; 
    if(pumpenSperre[i]) continue; 
    
    ergebnisP1[i] = berechneStabilenWert(werteP1[i], 15);
    
    // --- NEU: VIRTUELLER FLOW-SENSOR RESET (Wenn Erde feuchter wurde!) ---
    if (ergebnisP1[i] != -999 && gedaechtnisWert[i] != -1) {
        if (ergebnisP1[i] > (gedaechtnisWert[i] + 2)) {
            fehlversuche[i] = 0; // Wasser kommt an! Counter auf 0!
            soakTimeAktiv[i] = false; // Sicker-Pause ebenfalls zurücksetzen!
        }
    }
    
    if (ergebnisP1[i] != -999) gedaechtnisWert[i] = ergebnisP1[i];

           
    
    // --- NEU: KABELBRUCH ERKENNUNG (< 3%) ---
    if (ergebnisP1[i] != -999 && ergebnisP1[i] < 3) {
        pumpenSperre[i] = true; // Schloss dran!
        addLog("⚠️ " + t("SENSOR FEHLER bei ", "SENSOR ERROR at ") + NAME_TOPF[i]);
    }
    
    if (ergebnisP1[i] != -999 && ergebnisP1[i] <= (ZIEL_FEUCHTIGKEIT[i] - 5) && !pumpenSperre[i]) {
      brauchtWasser = true;
      imDeepCheck[i] = true; 
    }
  }

  if (brauchtWasser) {
     aktuellePhase = 2; 
     int werteP2[MAX_TOEPFE][30]; 
     
     for(int min = 0; min < 30; min++) {
        minutenInPhase = min; 
        String p2Text = "P2 (" + String(min + 1) + "/30)";
        warteUndRotiere(MINUTE_IN_MS, p2Text);
        for(int i=0; i<ANZAHL_TOEPFE; i++) werteP2[i][min] = aktuelleErde[i];

        // --- NEU: ESP-NOW UPDATE JEDE MINUTE IN PHASE 2 ---
        if(ESP_NOW_ACTIVE) sendeDaten();
     }
     
     for(int i=0; i<ANZAHL_TOEPFE; i++) {
        if(pumpenSperre[i]) continue;
        int ergebnisP2 = berechneStabilenWert(werteP2[i], 30);
        bool sicher = (ergebnisP2 != -999 && ergebnisP2 > 2); // Gießt absolut NICHT mehr bei 0%

        // ==========================================================
        // --- DIE HIGH-AVAILABILITY PUMPEN-WEICHE ---
        // ==========================================================
        bool darfGiessen = false;
        int aktivePumpe = PIN_PUMPE; // Standard-Fallback
        
        // Szenario 0: Nur Pumpe 1 (Regentonne)
        if (PUMPEN_WAHL[i] == 0 && tankVoll) { 
            darfGiessen = true; 
            aktivePumpe = PIN_PUMPE; 
        } 
        // Szenario 1: Nur Pumpe 2 (Festwasser)
        else if (PUMPEN_WAHL[i] == 1 && ZWEITER_KREISLAUF && tankVoll2) { 
            darfGiessen = true; 
            aktivePumpe = PIN_PUMPE2; 
        } 
        // Szenario 2: Auto-Fallback (Versuche Tonne, sonst nimm Schlauch!)
        else if (PUMPEN_WAHL[i] == 2 && ZWEITER_KREISLAUF) {
            if (tankVoll) { 
                darfGiessen = true; 
                aktivePumpe = PIN_PUMPE; 
            } else if (tankVoll2) { 
                darfGiessen = true; 
                aktivePumpe = PIN_PUMPE2; 
            }
        }

        // --- DER FINALE GIESS-BEFEHL (MIT VIRTUELLEM FLOW-SENSOR & SIEM-SCHILD Smart Rules) ---
        if (darfGiessen && sicher && ergebnisP2 < ZIEL_FEUCHTIGKEIT[i]) {
            
            // 🚨 NEU: PAUSE-MODUS CHECK
           if (systemPausiert) {
              Serial.println("Gießen blockiert: SYSTEM PAUSIERT!");
              continue; // Überspringt diesen Topf!
           }

           // 🚨 1. CHECK: Hat das SIEM das System GANZ gesperrt? (Atom-Option)
           if (siemBlockWatering) {
              Serial.println("Gießen GLOBAL durch SIEM blockiert!");
              continue; // Bricht das Gießen sofort ab!
           }

           // 🚨 2. CHECK: Hat das SIEM DIESES SPEZIELLE Ventil gesperrt? (Tomaten-Haus-Option)
           if (pinGesperrt[PIN_VENTIL[i]]) {
              Serial.println("Gießen für " + NAME_TOPF[i] + " durch SIEM-Spezialregel blockiert!");
              continue; // Bricht das Gießen NUR für diesen Topf ab!
           }

           // 3. CHECK: Haben wir schon 2x erfolglos gegossen?
           if (fehlversuche[i] >= 2) {
              pumpenSperre[i] = true; // ALARM! Schloss dran!
              addLog("🏜️ " + t("WASSER-ALARM bei ", "WATER ALARM at ") + NAME_TOPF[i]); // <--- HIER EINFÜGEN!
              Serial.println("ALARM: 2x gegossen ohne Erfolg bei " + NAME_TOPF[i]);
              continue; // Gießen abbrechen!
           }

           // ==========================================================
           // --- NEU: DIE 1h 30m SICKER-PAUSE (SOAK TIME) ---
           // ==========================================================
           if (fehlversuche[i] == 1) {
               if (!soakTimeAktiv[i]) {
                   // Erster Versuch war vor 45 Min. Wasser ist noch nicht am Sensor.
                   // Wir verweigern das Gießen für diese Runde und geben dem Topf 45 Min extra Zeit!
                   soakTimeAktiv[i] = true;
                   addLog("⏳ " + NAME_TOPF[i] + " bekommt extra Sicker-Zeit (45 Min)...");
                   Serial.println("Soak-Time aktiv für " + NAME_TOPF[i] + ". Gießen übersprungen.");
                   continue; // Bricht das Gießen NUR für diese Runde ab!
               } else {
                   // Das System hat jetzt 2 Runden (1h 30m) gewartet. Wasser immer noch nicht da.
                   // Jetzt darf es den 2. und letzten Versuch abfeuern!
                   soakTimeAktiv[i] = false; 
               }
           }
           // ==========================================================
           
           // 4. COUNTER HOCHZÄHLEN
           fehlversuche[i]++;
        

           // 5. HARDWARE STARTEN
           addLog(t("Gieße: ", "Watering: ") + NAME_TOPF[i]); 
           
           if(USE_DISPLAY) {
             display.clearDisplay(); display.setCursor(10, 25); display.setTextSize(2);
             display.println(t("GIESSEN\n", "WATERING\n") + NAME_TOPF[i]); display.display();
           }
           
           schaltePin(PIN_VENTIL[i], LOW);  
           
          // Nur wenn das Ventil sicher offen ist und KEIN Notfall vorliegt, Pumpe an!
          if (smartDelay(500, aktivePumpe)) {  
              schaltePin(aktivePumpe, LOW);    
              smartDelay(GIESS_DAUER[i], aktivePumpe, i); // <--- HIER DAS 'i' HINZUFÜGEN!
          }
           
           // EGAL was passiert ist, am Ende ALLES sicher abschalten!
           schaltePin(aktivePumpe, HIGH);   
           smartDelay(500, aktivePumpe); // Druck entweichen lassen                      
           schaltePin(PIN_VENTIL[i], HIGH);
           
           struct tm tinfo;
           if(getLocalTime(&tinfo)){
              char tBuff[15]; 
              strftime(tBuff, sizeof(tBuff), "%d.%m. %H:%M", &tinfo);
              letztesGiessen[i] = String(tBuff);
           } else {
              letztesGiessen[i] = aktuelleZeit;
           }

           // 💾 NEU: Den neuen Zeitstempel sofort ausfallsicher auf der Festplatte speichern!
           preferences.begin("kraeuter", false);
           preferences.putString(("lGi" + String(i)).c_str(), letztesGiessen[i]);
           preferences.end();

           if(ESP_NOW_ACTIVE) sendeDaten();
        }
     }
  } 
}
