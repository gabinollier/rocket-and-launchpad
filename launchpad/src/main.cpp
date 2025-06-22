// LAUNCHPAD CODE

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include <WebSocketsServer.h>
#include <vector>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>
#include <SD.h>
#include <SPI.h>
#include "DFRobot_MPX5700.h"

// Constants

const char* wifiSSID = "Pas de tir 🚀";
const char* wifiPassword = "hippocampe";
const char* externalWifiSSID = "Livebox-D040";
const char* externalWifiPassword = NULL;
const char* dnsName = "launchpad.local"; // on peut mettre autre chose que .local si on veut
const byte DNS_PORT = 53;
const float MAX_PRESSURE = 10.0; // bars
const float LAUNCH_CLEARING_DELAY = 2; // seconds
const float PRESSURE_MARGIN = 0.5; // bars 
const int MAX_LOGS = 200;
const char* SD_FLIGHT_DATA_DIR = "/flight_data"; 
#define PRESSURE_SENSOR_I2C_ADDRESS 0x16 // mpx5700AP
DFRobot_MPX5700 pressureSensor(&Wire, PRESSURE_SENSOR_I2C_ADDRESS);

// PINS
const int SERVO_PIN = 5; // grove
const int VALVE_PIN = 6; // grove
const int DISTRIBUTOR_PIN_ATMO = 9; // grove
const int DISTRIBUTOR_PIN_COMP = 10; // grove

const int FLOW_METER_PIN = 11;

// SD Card pins
const int SD_CS_PIN = 13;   

// Data structures

struct LogEntry 
{
    uint32_t timestamp;
    String message;
};

enum LaunchpadState 
{
    NOT_INITIALIZED,
    IDLING,
    WATER_FILLING,
    PRESSURIZING,
    READY_FOR_LAUNCH,
    LAUNCHING,
};

enum DistributorState 
{
    ATMOSPHERE,
    COMPRESSOR,
    LOCKED,
};

// Global variables

WebServer httpServer(80);
WebSocketsServer webSocketServer = WebSocketsServer(81);
DNSServer dnsServer;
LaunchpadState currentLaunchpadState = NOT_INITIALIZED;
String currentRocketState = "UNKNOWN";
LogEntry logs[MAX_LOGS];
int logIndex = 0;
float currentWaterVolume = 0.0;
float targetWaterVolume = 0.0;
float currentPressure = 0.0;
float targetPressure = 0.0;
unsigned long launchTime = 0;
unsigned long deltaTime = 0;
volatile int flowCount = 0;
Servo lockServo;
bool isLockSystemLocked = true;
unsigned long espStartTime = 0;
DistributorState currentDistributorState = ATMOSPHERE;

// Function declarations

void changeState(LaunchpadState newState);
String launchpadStateToString(LaunchpadState state);
void updatePressureAndIncrementVolume();

// Setup functions
bool setupFileSystem();
bool setupSDCard();
bool setupWiFiAP();
bool setupConnectionToExternalWiFi();
bool setupLocalDNS();
bool setupHttpServer();
bool setupWebSocketServer();
void setupAPIEndpoints();
void setupFileEndpoints();
bool setupPressureSensor();

// API endpoint handlers
void handleAPICloseFairing();
void handleAPIOpenFairing();
void handleAPIGetRocketState();
void handleAPIGetLogs();
void handleAPIStartFilling();
void handleAPILaunch();
void handleAPIAbort();
void handleAPINewRocketState();
void handleAPIUploadFlightData();
void handleAPIGetLaunchpadState();
void handleAPIGetWaterVolume();
void handleAPIGetPressure();
void handleAPIGetAllFlightTimestamps();
void handleAPIGetFlightData();
void handleAPIDownloadFlightCSV();
void handleAPIRotateServo();
void handleAPISkipWaterFilling();
void handleAPISkipPressurizing();

// WebSocket senders
void sendWSNewLog(String timestamp, String message);
void sendWSNewLaunchpadState(String newLaunchpadState);
void sendWSNewRocketState(String newRocketState);
void sendWSFilling(float waterVolume, float pressure);
void sendWSOpenFairing();
void sendWSCloseFairing();

// Hardware sensors
float getPressure();
void IRAM_ATTR onFlowMeterInterrupt();

// Hardware control functions
void openValve();
void closeValve();
void setPressureDistributorToAtmosphere();
void setPressureDistributorToCompressor();
void setPressureDistributorToLocked();
void closeLockSystem();
void openLockSystem();
void rotateServo(float turns);

// Debug functions
void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length);
void println(String message);

unsigned long getCurrentSecondsSinceEpoch();


void setup()
{
    Serial.begin(115200);
    delay(1000);

    println("\\nInitialisation...");    
    bool setupSuccess = true;

    // Setup pins
    pinMode(DISTRIBUTOR_PIN_ATMO, OUTPUT);
    pinMode(DISTRIBUTOR_PIN_COMP, OUTPUT);
    pinMode(VALVE_PIN, OUTPUT);
    pinMode(FLOW_METER_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(FLOW_METER_PIN), onFlowMeterInterrupt, RISING);

    // Setup servo
    lockServo.attach(SERVO_PIN);
    lockServo.writeMicroseconds(1500); // Initialize servo to stopped state
    println("Servo initialisé.");

    setupSuccess = setupSuccess && setupFileSystem();
    setupSuccess = setupSuccess && setupSDCard(); 
    setupSuccess = setupSuccess && setupWiFiAP();
    setupConnectionToExternalWiFi();
    setupSuccess = setupSuccess && setupLocalDNS();
    setupSuccess = setupSuccess && setupHttpServer();
    setupSuccess = setupSuccess && setupWebSocketServer();
    setupSuccess = setupSuccess && setupPressureSensor(); 

    if (setupSuccess) 
    {
        println("Le pas de tir est prêt !");
        changeState(IDLING);
    } 
    else 
    {
        println("Initialisation terminée avec des erreurs. Le pas de tir peut ne pas fonctionner correctement.");
    }
}

void loop() 
{
    static unsigned long previousMillis = millis();

    deltaTime = millis() - previousMillis;
    previousMillis = millis();
    
    currentPressure = getPressure(); 
    currentWaterVolume += flowCount * 0.002;
    flowCount = 0; 

    dnsServer.processNextRequest();
    httpServer.handleClient();
    webSocketServer.loop();

    if (currentLaunchpadState == WATER_FILLING) 
    {
        if (currentWaterVolume < targetWaterVolume) 
        {
            sendWSFilling(currentWaterVolume, currentPressure);
        } 
        else 
        {
            changeState(PRESSURIZING);
        }
    } 
    else if (currentLaunchpadState == PRESSURIZING) 
    {
        // Dumb pressure control logic
        if (currentPressure < targetPressure) 
        {
            sendWSFilling(currentWaterVolume, currentPressure);
        } 
        else 
        {
            changeState(READY_FOR_LAUNCH);
        }
    }  
    else if (currentLaunchpadState == READY_FOR_LAUNCH)
    {
        // Smart pressure control logic
        if (currentDistributorState == LOCKED)
        {
            if (currentPressure < targetPressure - PRESSURE_MARGIN) 
            {
                setPressureDistributorToCompressor();
            } 
            else if (currentPressure > targetPressure + PRESSURE_MARGIN) 
            {
                setPressureDistributorToAtmosphere();
            } 
        }
        else if (currentDistributorState == ATMOSPHERE && currentPressure <= targetPressure)
        {
            setPressureDistributorToLocked();
        }
        else if (currentDistributorState == COMPRESSOR && currentPressure >= targetPressure)
        {
            setPressureDistributorToLocked();
        }

        sendWSFilling(currentWaterVolume, currentPressure);
    }
    else if (currentLaunchpadState == LAUNCHING) 
    {
        sendWSFilling(0, currentPressure);
        if (millis() > launchTime + LAUNCH_CLEARING_DELAY * 1000) 
        { 
            changeState(IDLING);
        }
    }

    delay(10);
}

void changeState(LaunchpadState newState) 
{
    LaunchpadState previousState = currentLaunchpadState;
    currentLaunchpadState = newState;

    if (newState == previousState) 
    {
        println("Pas de changement d'état : " + launchpadStateToString(newState));
        return;
    }

    if (newState != IDLING)
    { 
        // This ensures that the sequence is followed in the correct order.
        if ((newState == NOT_INITIALIZED) 
            || (newState == WATER_FILLING && previousState != IDLING)
            || (newState == PRESSURIZING && previousState != WATER_FILLING)
            || (newState == READY_FOR_LAUNCH && previousState != PRESSURIZING)
            || (newState == LAUNCHING && previousState != READY_FOR_LAUNCH))
        {
            println("Erreur : impossible de passer à l'état " + launchpadStateToString(newState) + " depuis l'état " + launchpadStateToString(previousState) + ". Annulation de la séquence de lancement.");
            changeState(IDLING);
            return;
        }
    }

    println("Changement d'état : " + launchpadStateToString(previousState) + " -> " + launchpadStateToString(newState));
    sendWSNewLaunchpadState(launchpadStateToString(newState));

    switch(newState) 
    {
        case IDLING:
            currentWaterVolume = 0;
            sendWSFilling(currentWaterVolume, currentPressure);
            closeLockSystem();
            closeValve();
            setPressureDistributorToAtmosphere();
            break;

        case WATER_FILLING:
            currentWaterVolume = 0;
            setPressureDistributorToAtmosphere();
            openValve();
            break;

        case PRESSURIZING:
            closeValve();
            setPressureDistributorToCompressor();
            break;

        case READY_FOR_LAUNCH:
            setPressureDistributorToLocked();
            break;

        case LAUNCHING:
            launchTime = millis();
            openLockSystem();
            break;
    }
}

String launchpadStateToString(LaunchpadState state) 
{
    switch(state) 
    {
        case NOT_INITIALIZED: return "NOT_INITIALIZED";
        case IDLING: return "IDLING";
        case WATER_FILLING: return "WATER_FILLING";
        case PRESSURIZING: return "PRESSURIZING";
        case READY_FOR_LAUNCH: return "READY_FOR_LAUNCH";
        case LAUNCHING: return "LAUNCHING";
        default: return "UNKNOWN";
    }
}

bool setupPressureSensor() 
{
    Wire.begin();
    pressureSensor.begin();
    
    for (int i = 0; i < 5; i++)
    {
        if (pressureSensor.begin())
        {
            println("Capteur de pression MPX5700 initialisé avec succès.");
            pressureSensor.setMeanSampleSize(5);
            return true;
        }
    }

    
    return false;
}

bool setupFileSystem() 
{
    if (!LittleFS.begin(true)) 
    {
        println("L'initialisation et le formatage de LittleFS ont échoué");
        return false;
    }

    println("LittleFS initialisé avec succès.");
    return true;
}

bool setupSDCard() 
{
    // Initialize SPI with custom pins
    SPI.begin();
    
    if (!SD.begin(SD_CS_PIN)) 
    {
        println("Échec de l'initialisation de la carte SD");
        return false;
    }
    
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) 
    {
        println("Aucune carte SD détectée");
        return false;
    }
      println("Carte SD initialisée avec succès");
    
    // Create flight_data directory if it doesn't exist
    if (!SD.exists(SD_FLIGHT_DATA_DIR)) 
    {
        if (SD.mkdir(SD_FLIGHT_DATA_DIR)) 
        {
            println("Dossier " + String(SD_FLIGHT_DATA_DIR) + " créé");
        } 
        else 
        {
            println("Échec de la création du dossier " + String(SD_FLIGHT_DATA_DIR));
        }
    }
    
    return true;
}

bool setupWiFiAP() 
{
    WiFi.mode(WIFI_AP_STA); 
    WiFi.softAP(wifiSSID, wifiPassword);
    //WiFi.onEvent(onWifiEvent);

    println("Point d'accès WiFi initialisé. IP locale : " + WiFi.softAPIP().toString());
    return true;
}

bool setupConnectionToExternalWiFi() 
{
    if (externalWifiSSID == NULL || externalWifiPassword == NULL) 
    {
        println("Aucun réseau Wi-Fi externe configuré. Fonctionnement en mode AP uniquement.");
        return false;
    }

    // DEBUG : Scan for available networks :

    // Serial.println("Scan des réseaux Wi-Fi disponibles...");
    // int n = WiFi.scanNetworks();
    // if (n == 0) 
    // {
    //     Serial.println("Aucun réseau Wi-Fi trouvé.");
    //     return false;
    // } 
    // else 
    // {
    //     Serial.println("Réseaux Wi-Fi trouvés : " + String(n));
    //     for (int i = 0; i < n; i++) 
    //     {
    //         Serial.println("- " + WiFi.SSID(i) + " (" + (100+WiFi.RSSI(i)) + "%)");
    //     }
    // }

    WiFi.disconnect();

    int attemptCount = 0;
    while (WiFi.status() != WL_CONNECTED && attemptCount < 5) 
    {
        delay(1000);
        WiFi.begin(externalWifiSSID, externalWifiPassword);
        println("Tentative de connexion au réseau Wi-Fi externe : " + String(attemptCount + 1) + "/5...");
        delay(2000);
        println("Statut de la connexion : " + String(WiFi.status()));
        attemptCount++;
    }

    if (WiFi.status() == WL_CONNECTED) 
    {
        println("Connecté au réseau Wi-Fi externe. IP locale assignée : " + WiFi.localIP().toString());
        return true;
    } 
    else 
    {
        println("Échec de connexion au réseau Wi-Fi externe. Fonctionnement en mode AP uniquement.");
        return false; 
    }
}

bool setupLocalDNS() 
{
    dnsServer.start(DNS_PORT, dnsName, WiFi.softAPIP());
    println("Serveur DNS local initialisé pour http://" + String(dnsName));
    return true;
}

bool setupHttpServer() 
{
    setupAPIEndpoints();
    setupFileEndpoints();
    
    httpServer.begin();
    println("Serveur HTTP initialisé.");
    return true;
}

bool setupWebSocketServer() 
{
    webSocketServer.begin();
    webSocketServer.onEvent(onWebSocketEvent);
    println("Serveur WebSocket initialisé.");
    return true;
}

void setupAPIEndpoints()
{
    // Endpoint for the frontend
    httpServer.on("/api/get-logs", HTTP_GET, handleAPIGetLogs);
    httpServer.on("/api/get-rocket-state", HTTP_GET, handleAPIGetRocketState);
    httpServer.on("/api/get-launchpad-state", HTTP_GET, handleAPIGetLaunchpadState);
    httpServer.on("/api/get-water-volume", HTTP_GET, handleAPIGetWaterVolume);
    httpServer.on("/api/get-pressure", HTTP_GET, handleAPIGetPressure);
    httpServer.on("/api/get-all-flight-timestamps", HTTP_GET, handleAPIGetAllFlightTimestamps);
    httpServer.on("/api/get-flight-data", HTTP_GET, handleAPIGetFlightData);
    httpServer.on("/api/download-flight-csv", HTTP_GET, handleAPIDownloadFlightCSV);

    httpServer.on("/api/start-filling", HTTP_POST, handleAPIStartFilling);
    httpServer.on("/api/launch", HTTP_POST, handleAPILaunch);
    httpServer.on("/api/abort", HTTP_POST, handleAPIAbort);
    httpServer.on("/api/rotate-servo", HTTP_POST, handleAPIRotateServo);
    httpServer.on("/api/skip-water-filling", HTTP_POST, handleAPISkipWaterFilling);
    httpServer.on("/api/skip-pressurizing", HTTP_POST, handleAPISkipPressurizing);
    httpServer.on("/api/close-fairing", HTTP_POST, handleAPICloseFairing);
    httpServer.on("/api/open-fairing", HTTP_POST, handleAPIOpenFairing);

    // Endpoints for the rocket
    httpServer.on("/api/new-rocket-state", HTTP_POST, handleAPINewRocketState);
    httpServer.on("/api/upload-flight-data", HTTP_POST, handleAPIUploadFlightData);
}

void handleAPIGetRocketState()
{
    JsonDocument doc;
    doc["rocket-state"] = currentRocketState;;
    String response;
    serializeJson(doc, response);
    httpServer.send(200, "application/json", response);
}

void handleAPIGetLaunchpadState() 
{
    JsonDocument doc;
    doc["launchpad-state"] = launchpadStateToString(currentLaunchpadState);
    String response;
    serializeJson(doc, response);
    httpServer.send(200, "application/json", response);
}

void handleAPIGetWaterVolume() 
{
    JsonDocument doc;
    doc["water-volume"] = currentWaterVolume;
    String response;
    serializeJson(doc, response);
    httpServer.send(200, "application/json", response);
}

void handleAPIGetPressure() 
{
    JsonDocument doc;
    doc["pressure"] = getPressure();
    String response;
    serializeJson(doc, response);
    httpServer.send(200, "application/json", response);
}

void setupFileEndpoints() 
{
    const char* routesToIndex[] = {"", "/", "/launch", "/flight-data-list", "/flight-data", "/debug"}; // URIs that serve index.html

    for (const char* route : routesToIndex) 
    {
        httpServer.serveStatic(route, LittleFS, "/index.html");
    }

    httpServer.onNotFound([]() 
    {
        println("404: Le client a essayé d'accéder à " + httpServer.uri() + " mais cela n'existe pas.");
        File file = LittleFS.open("/index.html", "r");
        httpServer.streamFile(file, "text/html; charset=UTF-8");
        file.close();
    });

    httpServer.serveStatic("/pages/", LittleFS, "/pages/");
    httpServer.serveStatic("/css/", LittleFS, "/css/");
    httpServer.serveStatic("/js/", LittleFS, "/js/");
}

void handleAPIGetLogs() 
{
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();
    for (int i = 0; i < logIndex; i++) 
    {
        JsonObject entry = array.add<JsonObject>();
        entry["timestamp"] = logs[i].timestamp;
        entry["message"] = logs[i].message;
    }
    String response;
    serializeJson(doc, response);
    httpServer.send(200, "application/json", response);
}

void handleAPIGetAllFlightTimestamps() 
{
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    
    File root = SD.open(SD_FLIGHT_DATA_DIR);
    if (!root) {
        println("Erreur lors de l'ouverture du dossier " + String(SD_FLIGHT_DATA_DIR));
        String response;
        serializeJson(doc, response);
        httpServer.send(200, "application/json", response);
        return;
    }
    
    if (!root.isDirectory()) {
        println(String(SD_FLIGHT_DATA_DIR) + " n'est pas un dossier");
        root.close();
        String response;
        serializeJson(doc, response);
        httpServer.send(200, "application/json", response);
        return;
    }
    
    File file = root.openNextFile();

    while (file) {
        if (!file.isDirectory()) {
            String filename = String(file.name());
            // Extract timestamp from filename (format: flight_TIMESTAMP.csv)
            if (filename.startsWith("flight_") && filename.endsWith(".csv")) {
                int startIndex = filename.indexOf("_") + 1;
                int endIndex = filename.indexOf(".csv");
                if (startIndex > 0 && endIndex > startIndex) {
                    String timestampStr = filename.substring(startIndex, endIndex);
                    arr.add(timestampStr);
                }
            }
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    
    String response;
    serializeJson(doc, response);
    httpServer.send(200, "application/json", response);
}

void handleAPIGetFlightData() 
{
    if (!httpServer.hasArg("timestamp")) {
        httpServer.send(400, "application/json", "{\"error\":\"timestamp parameter required\"}");
        return;
    }
    String requestTimestamp = httpServer.arg("timestamp");
    String filename = String(SD_FLIGHT_DATA_DIR) + "/flight_" + requestTimestamp + ".csv";
    
    File file = SD.open(filename);
    if (!file) {
        println("Fichier de vol non trouvé : " + filename);
        httpServer.send(404, "application/json", "{\"error\":\"Flight data not found\"}");
        return;
    }
    
    JsonDocument doc; 
    JsonArray flightDataArray = doc.createNestedArray("flightData");

    unsigned long launchtime = 0;
    float targetPressure = 0.0;
    float targetWaterVolume = 0.0;
    
    // Skip the CSV header line
    String headerLine = file.readStringUntil('\n');
    headerLine.trim(); 

    bool firstDataRow = true;
    
    // Read and parse each line of CSV data
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim(); 
        
        if (line.length() == 0) continue; 
        
        int fieldIndex = 0;
        int startIndex = 0;
        JsonObject obj = flightDataArray.createNestedObject();
        
        for (int i = 0; i <= line.length(); i++) {
            if (i == line.length() || line.charAt(i) == ',') {
                String field = line.substring(startIndex, i);
                field.trim(); 
                
                // CSV columns: timestamp,temperature,pressure,relativeAltitude,accelX,accelY,accelZ,gyroX,gyroY,gyroZ,launchtime,targetPressure,targetWaterVolume
                switch (fieldIndex) {
                    case 0: obj["timestamp"] = field; break;
                    case 1: obj["temperature"] = field.toFloat(); break;
                    case 2: obj["pressure"] = field.toFloat(); break;
                    case 3: obj["relativeAltitude"] = field.toFloat(); break;
                    case 4: obj["accelX"] = field.toFloat(); break;
                    case 5: obj["accelY"] = field.toFloat(); break;
                    case 6: obj["accelZ"] = field.toFloat(); break;
                    case 7: obj["gyroX"] = field.toFloat(); break;
                    case 8: obj["gyroY"] = field.toFloat(); break;
                    case 9: obj["gyroZ"] = field.toFloat(); break;
                    case 10: // launchtime
                        if (firstDataRow && field.length() > 0) {
                            launchtime = strtoul(field.c_str(), NULL, 10);
                        }
                        break;
                    case 11: // targetPressure
                        if (firstDataRow && field.length() > 0) {
                            targetPressure = field.toFloat();
                        }
                        break;
                    case 12: // targetWaterVolume
                        if (firstDataRow && field.length() > 0) {
                            targetWaterVolume = field.toFloat();
                        }
                        break;
                }
                
                fieldIndex++;
                startIndex = i + 1;
            }
        }
        if (firstDataRow) {
            firstDataRow = false;
        }
    }
    
    file.close();

    // Add top-level fields
    doc["launchtime"] = launchtime;
    doc["targetPressure"] = targetPressure;
    doc["targetWaterVolume"] = targetWaterVolume;
    
    String response;
    serializeJson(doc, response);
    httpServer.send(200, "application/json", response);
}

void handleAPIDownloadFlightCSV() 
{
    if (!httpServer.hasArg("timestamp")) {
        httpServer.send(400, "text/plain", "timestamp parameter required");
        return;
    }
    
    String timestamp = httpServer.arg("timestamp");
    String filename = String(SD_FLIGHT_DATA_DIR) + "/flight_" + timestamp + ".csv";
    
    File file = SD.open(filename);
    if (!file) {
        httpServer.send(404, "text/plain", "Flight data not found");
        return;
    }
    
    httpServer.sendHeader("Content-Disposition", "attachment; filename=flight_" + timestamp + ".csv");
    httpServer.streamFile(file, "text/csv");
    file.close();
}

void handleAPIStartFilling()
{
    if (currentLaunchpadState != IDLING) 
    {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Le pas de tir n'est pas prêt pour le remplissage, il est dans l'état " + launchpadStateToString(currentLaunchpadState) + " mais il devrait être dans l'état IDLING";
        String response;
        serializeJson(doc, response);
        httpServer.send(400, "application/json", response);
        return;
    }
    
    if (!httpServer.hasArg("water-volume") || !httpServer.hasArg("pressure") || !httpServer.hasArg("epoch-time"))
    {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Les paramètres de volume, de pression et epoch-time sont requis";
        String response;
        serializeJson(doc, response);
        httpServer.send(400, "application/json", response);
        return;
    }

    targetWaterVolume = httpServer.arg("water-volume").toFloat();
    targetPressure = httpServer.arg("pressure").toFloat();
    unsigned long epochTime = httpServer.arg("epoch-time").toInt();

    espStartTime = epochTime - (millis() / 1000);

    if (targetWaterVolume <= 0) 
    {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Volume d'eau invalide (min : 0L | demandé : " + String(targetWaterVolume) + "L)";
        String response;
        serializeJson(doc, response);
        httpServer.send(400, "application/json", response);
        return;
    }

    if (targetPressure <= 1 || targetPressure > MAX_PRESSURE) 
    { 
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Valeur de pression invalide (min : 1 bar | max : " + String(MAX_PRESSURE) + " bars | demandé : " + String(targetPressure) + " bars)";
        String response;
        serializeJson(doc, response);
        httpServer.send(400, "application/json", response);
        return;
    }

    changeState(WATER_FILLING);
    httpServer.send(200, "application/json");
}

void handleAPICloseFairing() 
{
    sendWSCloseFairing();
    httpServer.send(200, "application/json");
}

void handleAPIOpenFairing() 
{
    sendWSOpenFairing();
    httpServer.send(200, "application/json");
}

void handleAPILaunch() 
{
    if (currentLaunchpadState != READY_FOR_LAUNCH) 
    {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Le pas de tir n'est pas prêt pour le lancement";
        String response;
        serializeJson(doc, response);
        httpServer.send(400, "application/json", response);
        return;
    }

    changeState(LAUNCHING);
    httpServer.send(200, "application/json");
}

void handleAPIAbort() 
{
    changeState(IDLING);
    httpServer.send(200, "application/json");
}

void handleAPINewRocketState() // This API endpoint serves as the rocket authentication too
{
    String body = httpServer.arg("plain");
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);

    if (error) 
    {
        httpServer.send(400, "application/json", "{\"error\":\"JSON invalide\"}");
        return;
    }

    if (!doc["rocket-state"].is<String>()) 
    {
        httpServer.send(400, "application/json", "{\"error\":\"Le champ rocket-state est requis\"}");
        return;
    }

    currentRocketState = doc["rocket-state"].as<String>();

    httpServer.send(200, "application/json");

    sendWSNewRocketState(currentRocketState);
}

void handleAPIUploadFlightData()
{
    println("Receiving flight data... there are currently " + String(ESP.getFreeHeap()) + " bytes available in the heap");

    String body = httpServer.arg("plain");
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);

    if (error) {
        httpServer.send(400, "application/json", "{\"error\":\"JSON invalide\"}");
        return;
    }

    // Extract top-level fields
    unsigned long launchtime = doc["launchtime"] | 0;
    float targetPressure = doc["targetPressure"] | 0.0;
    float targetWaterVolume = doc["targetWaterVolume"] | 0.0;

    if (!doc["flightData"].is<JsonArray>()) {
        httpServer.send(400, "application/json", "{\"error\":\"Le champ flightData doit être un tableau JSON\"}");
        return;
    }
    JsonArray arr = doc["flightData"].as<JsonArray>();
    if (arr.size() == 0) {
        httpServer.send(400, "application/json", "{\"error\":\"Aucune donnée de vol fournie dans flightData\"}");
        return;
    }    
    
    // Store flight data in SD card as CSV
    String filename = String(SD_FLIGHT_DATA_DIR) + "/flight_" + String(launchtime) + ".csv";
    File file = SD.open(filename, FILE_WRITE);
    
    if (file) {
        println("Enregistrement des données de vol dans " + filename);
        
        // Write CSV header
        file.println("timestamp,temperature,pressure,relativeAltitude,accelX,accelY,accelZ,gyroX,gyroY,gyroZ,launchtime,targetPressure,targetWaterVolume");
        
        bool firstDataRow = true;
        // Write each data point
        for (JsonObject obj : arr) {
            unsigned long timestamp = obj["timestamp"] | 0;
            float temperature = obj["temperature"] | 0.0;
            float pressure = obj["pressure"] | 0.0;
            float relativeAltitude = obj["relativeAltitude"] | 0.0;
            float accelX = obj["accelX"] | 0.0;
            float accelY = obj["accelY"] | 0.0;
            float accelZ = obj["accelZ"] | 0.0;
            float gyroX = obj["gyroX"] | 0.0;
            float gyroY = obj["gyroY"] | 0.0;
            float gyroZ = obj["gyroZ"] | 0.0;
            
            file.print(timestamp);
            file.print(",");
            file.print(temperature, 2);
            file.print(",");
            file.print(pressure, 6);
            file.print(",");
            file.print(relativeAltitude, 2);
            file.print(",");
            file.print(accelX, 2);
            file.print(",");
            file.print(accelY, 2);
            file.print(",");
            file.print(accelZ, 2);
            file.print(",");
            file.print(gyroX, 2);
            file.print(",");
            file.print(gyroY, 2);
            file.print(",");
            file.print(gyroZ, 2);
            file.print(",");

            if (firstDataRow) {
                file.print(launchtime);
                file.print(",");
                file.print(targetPressure, 2);
                file.print(",");
                file.println(targetWaterVolume, 2);
                firstDataRow = false;
            } else {
                file.print(",,");
                file.println();
            }
        }
        
        file.close();
        println("Données de vol enregistrées avec succès (" + String(arr.size()) + " échantillons)");
    } 
    else 
    {
        println("Erreur lors de l'ouverture du fichier " + filename + " pour l'écriture");
    }

    httpServer.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleAPIRotateServo()
{
    if (!httpServer.hasArg("turns"))
    {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Le paramètre 'turns' est requis";
        String response;
        serializeJson(doc, response);
        httpServer.send(400, "application/json", response);
        return;
    }

    float turns = httpServer.arg("turns").toFloat();
    println("Rotation finale demandée : " + String(turns) + " tours.");
    float servoTurns = turns * (30.0/22.0);
    println("Rotation du servo commandée : " + String(servoTurns) + " tours.");
    rotateServo(servoTurns);
    println("Rotation du servo terminée.");

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Servo tourné de " + String(servoTurns) + " tours.";
    String response;
    serializeJson(doc, response);
    httpServer.send(200, "application/json", response);
}

void handleAPISkipWaterFilling() 
{
    println("Skipping water filling.");
    changeState(PRESSURIZING);
    httpServer.send(200, "application/json", "{\"status\": \"Water filling skipped\"}");
}

void handleAPISkipPressurizing() 
{
    println("Skipping pressurizing.");
    changeState(READY_FOR_LAUNCH);
    httpServer.send(200, "application/json", "{\"status\": \"Pressurizing skipped\"}");
}

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) // Just for debugging
{
    switch(type) 
    {
        case WStype_DISCONNECTED:
            println("WS : Client #" + String(num) + " déconnecté du WebSocket");
            break;
        case WStype_CONNECTED:
            {
                IPAddress ip = webSocketServer.remoteIP(num);
                println("WS : Client #" + String(num) + " connecté depuis " + ip.toString());
            }
            break;
        case WStype_TEXT:
            println("WS : Message WebSocket reçu du client #" + String(num));
            break;
        default:
            break;
    }
}

void sendWSNewLog(String timestamp, String message) 
{
    JsonDocument doc;
    doc["type"] = "new-log";
    doc["timestamp"] = timestamp;
    doc["message"] = message;
    
    String jsonString;
    serializeJson(doc, jsonString);
    webSocketServer.broadcastTXT(jsonString);
}

void sendWSOpenFairing() 
{
    JsonDocument doc;
    doc["type"] = "open-fairing";
    
    String jsonString;
    serializeJson(doc, jsonString);
    webSocketServer.broadcastTXT(jsonString);
}

void sendWSCloseFairing() 
{
    JsonDocument doc;
    doc["type"] = "close-fairing";
    
    String jsonString;
    serializeJson(doc, jsonString);
    webSocketServer.broadcastTXT(jsonString);
}

void sendWSNewLaunchpadState(String newLaunchpadState) 
{
    JsonDocument doc;
    doc["type"] = "new-launchpad-state";
    doc["launchpad-state"] = newLaunchpadState;
    
    String jsonString;
    serializeJson(doc, jsonString);
    webSocketServer.broadcastTXT(jsonString);
}

void sendWSNewRocketState(String newRocketState) 
{
    JsonDocument doc;
    doc["type"] = "new-rocket-state";
    doc["rocket-state"] = newRocketState;
    
    String jsonString;
    serializeJson(doc, jsonString);
    webSocketServer.broadcastTXT(jsonString);
}

void sendWSFilling(float waterVolume, float pressure) 
{
    JsonDocument doc;
    doc["type"] = "filling";
    doc["water-volume"] = waterVolume;
    doc["pressure"] = pressure;
    doc["target-water-volume"] = targetWaterVolume;
    doc["target-pressure"] = targetPressure;
    doc["epoch-time"] = getCurrentSecondsSinceEpoch();
    
    String jsonString;
    serializeJson(doc, jsonString);
    webSocketServer.broadcastTXT(jsonString);
}

void println(String message) 
{
    Serial.println(message);

    message.replace("\\", "\\\\"); 
    message.replace("\n", "\\n"); 
    message.replace("\r", "\\r");  
    message.replace("\t", "\\t");  
    message.replace("\"", "\\\""); 

    logs[logIndex] = { millis(), message };
    logIndex = (logIndex + 1) % MAX_LOGS;

    sendWSNewLog(String(millis()), message);
}

float getPressure() 
{
    return pressureSensor.getPressureValue_kpa(1) / 100.0; // Convert kPa to bar
}

void openValve() 
{
    digitalWrite(VALVE_PIN, HIGH);
}

void closeValve()
{
    digitalWrite(VALVE_PIN, LOW);
}

void setPressureDistributorToAtmosphere()
{
    digitalWrite(DISTRIBUTOR_PIN_ATMO, HIGH);
    digitalWrite(DISTRIBUTOR_PIN_COMP, LOW);
    currentDistributorState = ATMOSPHERE;
}

void setPressureDistributorToCompressor() 
{
    digitalWrite(DISTRIBUTOR_PIN_ATMO, LOW); 
    digitalWrite(DISTRIBUTOR_PIN_COMP, HIGH);
    currentDistributorState = COMPRESSOR;
}

void setPressureDistributorToLocked()
{
    digitalWrite(DISTRIBUTOR_PIN_ATMO, LOW);
    digitalWrite(DISTRIBUTOR_PIN_COMP, LOW);
    currentDistributorState = LOCKED;
}

void closeLockSystem()
{
    if (isLockSystemLocked) 
    {
        return;
    }
    isLockSystemLocked = true;
    float servoTurns = (1.0/6.0)*(30.0/22.0);
    rotateServo(+servoTurns);
    println("Système de verrouillage fermé.");
}

void openLockSystem()
{
    if (!isLockSystemLocked) 
    {
        return;
    }
    isLockSystemLocked = false;
    float servoTurns = (1.0/6.0)*(30.0/22.0);
    rotateServo(-servoTurns);
    println("Système de verrouillage ouvert.");
}

void rotateServo(float turns)
{
    const int DURATION_FOR_ONE_TURN = 1916; // milliseconds

    int pulse_length = 1200; // microseconds

    if (turns < 0)
    {
        turns *= -1;
        pulse_length = 1700;
    }

    float duration = DURATION_FOR_ONE_TURN * turns;
    
    unsigned long startTime = millis();
    while (millis() < startTime + duration)
    {
        lockServo.writeMicroseconds(pulse_length);
        delay(20); 
    }

    lockServo.writeMicroseconds(1500);
}

void IRAM_ATTR onFlowMeterInterrupt()
{
    flowCount++;
}

unsigned long getCurrentSecondsSinceEpoch()
{
    return espStartTime + millis() / 1000;
}