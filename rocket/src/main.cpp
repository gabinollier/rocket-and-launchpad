// ROCKET CODE

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_DPS310.h>
#include <LSM6DS3.h>
#include <Wire.h>
#include <ESP32Servo.h>
#include <WebSocketsClient.h>

// Constants

const char* LAUNCHPAD_SSID = "Pas de tir 🚀";
const char* LAUNCHPAD_PASSWORD = "hippocampe";
const float g = 9.80665; // m/s²
const int PRELAUNCH_BUFFER_DURATION = 2; // seconds
const int TOTAL_BUFFER_DURATION = 60; // seconds

const int PARACHUTE_SERVO_PIN = 10;
const int BUZZER_PIN = 6;

const float LAUNCH_DETECTION_THRESHOLD = 0.5; // meters
const float APOGEE_DETECTION_SPEED_THRESHOLD = -1; // m/s
const float APOGEE_DETECTION_ALTITUDE_THRESHOLD = 15; // meters
const int PARACHUTE_ALTITUDE_THRESHOLD = 70; // meters
const int LANDING_DETECTION_ALTITUDE_THRESHOLD = 3; // meters
const int LANDING_DETECTION_DURATION = 3; // seconds

// Sampling rate constants
const int HZ_WAITING_FOR_LAUNCH = 32; // Hz
const int HZ_ASCENDING = 20; // Hz
const int HZ_DEFAULT = 8; // Hz
const unsigned long MAX_ASCENDING_DURATION = 10; // seconds

const int PRELAUNCH_BUFFER_SIZE = HZ_WAITING_FOR_LAUNCH * PRELAUNCH_BUFFER_DURATION;
const int TOTAL_BUFFER_SIZE = 400; // samples

// Data structures

enum RocketState 
{
    ERROR,
    NOT_INITIALIZED,
    IDLING_OPEN,
    IDLING_CLOSED,
    WAITING_FOR_LAUNCH,
    ASCENDING,
    FREE_FALLING,
    PARACHUTE_FALLING,
    RECONNECTING,
    SENDING_DATA
};

struct FlightData 
{
    unsigned long timestamp;
    float temperature;
    float pressure;
    float relativeAltitude;
    float accelX;
    float accelY;
    float accelZ;
    float gyroX;
    float gyroY;
    float gyroZ;
};

// Global variables

String launchpadIP; 
RocketState currentState = NOT_INITIALIZED;
Adafruit_DPS310 pressureSensor;
LSM6DS3 accelerationSensor(I2C_MODE, 0x6A);
WebSocketsClient webSocketClient;
float launchpadAltitude = 0.0; // meters
FlightData prelaunchBuffer[PRELAUNCH_BUFFER_SIZE];
int prelaunchBufferIndex = 0;
FlightData* flightBuffer = NULL;
int flightBufferIndex = 0;
Servo parachuteServo;
long int lastWifiTryTime = 0; //ms
unsigned long ascendingStateEnterTime = 0;
unsigned long espStartTime = 0; // s
float targetPressure = 0.0;
float targetWaterVolume = 0.0;
unsigned int launchtime = 0; // seconds since epoch

// Function prototypes

bool setupWifiConnection(int attempts);

bool setupPressureSensor();
bool setupAccelerationSensor();
bool setupI2C();
void scanI2CDevices();

bool tryToConnectToLaunchpad();

void changeState(RocketState newState);
String stateToString(RocketState state);

bool detectLaunch(const FlightData &data);
bool detectLanding(const FlightData &data);
bool shouldOpenParachute(const FlightData &data);
bool detectApogee(const FlightData &data, int deltaTime);

FlightData getCurrentFlightData();
void getPressureAndTemperature(float &pressure, float &temperature);
void getAcceleration(float &accelX, float &accelY, float &accelZ);
void getGyroscope(float &gyroX, float &gyroY, float &gyroZ);
float calculateAltitude(float pressure);
float calculateMagnitude(float x, float y, float z);

void recordDataInPrelaunchBuffer(const FlightData &data);
void recordData(const FlightData &data);
void copyPrelaunchBufferToFlightBuffer();
bool resetDataBuffers();

void rocketWebSocketEvent(WStype_t type, uint8_t * payload, size_t length);

bool sendRocketState(int attempts);
bool uploadFlightData(int attempts);

unsigned long getSamplingIntervalMs(); // New function prototype

void openParachute();
void closeParachute();

void validationBuzzer();
void longErrorBuzzer();
void shortErrorBuzzer();
void fallingBuzzer();

unsigned long getCurrentSecondsSinceEpoch();


void setup() 
{
    
    

    Serial.begin(115200);
    delay(1000);

    Serial.println("\nSetup...");

    parachuteServo.attach(PARACHUTE_SERVO_PIN);
    pinMode(BUZZER_PIN, OUTPUT);

    openParachute();

    bool initializationSuccess = true;
    initializationSuccess = initializationSuccess && setupI2C();
    initializationSuccess = initializationSuccess && setupAccelerationSensor();
    initializationSuccess = initializationSuccess && setupPressureSensor();
    initializationSuccess = initializationSuccess && resetDataBuffers();
    initializationSuccess = initializationSuccess && setupWifiConnection(15);
    initializationSuccess = initializationSuccess && sendRocketState(5);
    if (!initializationSuccess) 
    {
        Serial.println("Échec du setup. Mise en erreur.");
        changeState(ERROR);
        return;
    }

    Serial.println("Setup terminé.\n");
    changeState(IDLING_OPEN);
}

void loop()  {
    static unsigned long lastSampleTime = 0;

    webSocketClient.loop();
    unsigned long currentTime = millis();
    unsigned long currentSamplingInterval = getSamplingIntervalMs();
    bool shouldSample = (currentTime - lastSampleTime >= currentSamplingInterval);

    if (shouldSample)
    {
        lastSampleTime = currentTime;
    }

    if (currentState == WAITING_FOR_LAUNCH)
    {
        const FlightData data = getCurrentFlightData();
        if (shouldSample)
        {
            recordDataInPrelaunchBuffer(data);
        }

        if (detectLaunch(data))
        {
            changeState(ASCENDING);
        }
    }
    else if (currentState == ASCENDING)
    {
        const FlightData data = getCurrentFlightData();

        if (shouldSample)
        {
            recordData(data);
        }

        if (detectApogee(data, currentSamplingInterval)) 
        {
            changeState(FREE_FALLING);
        }
    }
    else if (currentState == FREE_FALLING)
    {
        const FlightData data = getCurrentFlightData();

        if (shouldSample)
        {
            recordData(data);
        }

        if (shouldOpenParachute(data))
        {
            changeState(PARACHUTE_FALLING);
        }

        fallingBuzzer();
    }
    else if (currentState == PARACHUTE_FALLING)
    {
        const FlightData data = getCurrentFlightData();

        if (shouldSample)
        {
            recordData(data);
        }
        
        if (detectLanding(data))
        {
            changeState(RECONNECTING);
        }

        fallingBuzzer();

    }

    if (currentState == ERROR)
    {
        return; 
    }

    delay(10);
}

bool detectLaunch(const FlightData &data)
{
    return data.relativeAltitude >= LAUNCH_DETECTION_THRESHOLD;
}

bool setupWifiConnection(int attempts)
{
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);

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

    for (int i = 0; i < attempts; i++) 
    {
        if (tryToConnectToLaunchpad())
        {
            return true;
        }

        shortErrorBuzzer();
    }

    WiFi.disconnect();
    return false;
}

bool setupI2C()
{
    if (!Wire.begin())
    {
        Serial.println("Échec de l'initialisation I2C");
        return false;
    }

    Serial.println("Initialisation I2C réussie");
    return true;
}

bool resetDataBuffers()
{
    for (int i = 0; i < PRELAUNCH_BUFFER_SIZE; i++)
    {
        prelaunchBuffer[i] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    }
    prelaunchBufferIndex = 0;

    if (flightBuffer != NULL) 
    {
        free(flightBuffer);
        flightBuffer = NULL;
    }
    flightBufferIndex = 0;
    
    flightBuffer = (FlightData*)malloc(sizeof(FlightData) * TOTAL_BUFFER_SIZE);
    if (!flightBuffer) 
    {
        Serial.println("Erreur d'allocation de mémoire pour le buffer de vol.");
        return false;
    }

    Serial.println("Buffer de vol réinitialisé avec succès.");
    return true;
}

void recordDataInPrelaunchBuffer(const FlightData &data)
{
    prelaunchBuffer[prelaunchBufferIndex] = data;
    prelaunchBufferIndex = (prelaunchBufferIndex + 1) % PRELAUNCH_BUFFER_SIZE; // Circular buffer
}

void recordData(const FlightData &data)
{
    if (flightBufferIndex < TOTAL_BUFFER_SIZE)
    {
        flightBuffer[flightBufferIndex] = data;
        flightBufferIndex++;
    }
}

FlightData getCurrentFlightData()
{
    float accelX = 0, accelY = 0, accelZ = 0, gyroX = 0, gyroY = 0, gyroZ = 0, pressure = 0, temperature = 0;
    getAcceleration(accelX, accelY, accelZ);
    getGyroscope(gyroX, gyroY, gyroZ);
    getPressureAndTemperature(pressure, temperature);
    float relativeAltitude = pressureSensor.readAltitude() - launchpadAltitude;

    FlightData data = {millis(), temperature, pressure, relativeAltitude, accelX, accelY, accelZ, gyroX, gyroY, gyroZ};
    return data;
}

void scanI2CDevices() {
    Serial.println("Scan des périphériques I2C...");
    byte error, address;
    int nDevices = 0;
 
    for(address = 1; address < 127; address++) 
    {
        Wire.beginTransmission(address);
        error = Wire.endTransmission();
        if (error == 0) {
            Serial.print("Périphérique I2C trouvé à l'adresse 0x");
            if (address < 16) Serial.print("0");
            Serial.println(address, HEX);
            nDevices++;
        }
    }
    
    if (nDevices == 0) 
    {
        Serial.println("Aucun périphérique I2C trouvé\n");
    } else 
    {
        Serial.println("Scan terminé\n");
    }
}

bool setupAccelerationSensor()
{
    if (accelerationSensor.begin() != 0) 
    {
        Serial.println("Échec de l'initialisation de l'accéléromètre LSM6DS.");
        return false;
    }

    Serial.println("Accéléromètre LSM6DS initialisé avec succès");
    return true;
}


bool setupPressureSensor()
{
    if (!pressureSensor.begin_I2C())
    {
        Serial.println("Échec de l'initialisation du capteur de pression DPS310");
        return false;
    }
    else
    {
        Serial.println("Capteur de pression DPS310 initialisé");
    }

    pressureSensor.setMode(DPS310_CONT_PRESTEMP);
    pressureSensor.configurePressure(DPS310_64HZ, DPS310_8SAMPLES);
    pressureSensor.configureTemperature(DPS310_64HZ, DPS310_8SAMPLES);

    return true;

}

void changeState(RocketState newState) 
{
    RocketState previousState = currentState;
    currentState = newState;

    if (newState == previousState) 
    {
        Serial.println("Pas de changement de state : " + stateToString(newState));
        return;
    }

    Serial.println("Changement de state : " + stateToString(previousState) + " -> " + stateToString(newState));

    if ((newState == NOT_INITIALIZED)
        || (newState == IDLING_OPEN && previousState != SENDING_DATA && previousState != NOT_INITIALIZED && previousState != IDLING_CLOSED)
        || (newState == IDLING_CLOSED && previousState != IDLING_OPEN)
        || (newState == WAITING_FOR_LAUNCH && previousState != IDLING_CLOSED && previousState != IDLING_OPEN)
        || (newState == ASCENDING && previousState != WAITING_FOR_LAUNCH)
        || (newState == FREE_FALLING && previousState != ASCENDING)
        || (newState == PARACHUTE_FALLING && previousState != FREE_FALLING)
        || (newState == RECONNECTING && previousState != PARACHUTE_FALLING)
        || (newState == SENDING_DATA && previousState != RECONNECTING))
    { 
        Serial.println("Impossible de passer au state " + stateToString(newState) + " depuis " + stateToString(previousState) + ". Mise en erreur.");
        changeState(ERROR);
        return;
    }

    if (newState == ERROR)
    {
        WiFi.disconnect();

        for (int i = 0; i < 5; i++)
        {
            longErrorBuzzer();
            delay(1000);
        }
        ESP.restart();
    }

    if (newState != ERROR)
    {
        validationBuzzer();
    }

    switch(newState)
    {
        case ERROR:
            sendRocketState(5);
            break;

        case IDLING_OPEN:
            sendRocketState(5);
            openParachute();
            launchpadAltitude = pressureSensor.readAltitude();
            break;

        case IDLING_CLOSED:
            sendRocketState(5);
            closeParachute();
            launchpadAltitude = pressureSensor.readAltitude();
            break;

        case WAITING_FOR_LAUNCH:
            sendRocketState(5);
            WiFi.disconnect();
            launchpadIP = "";
            break;

        case ASCENDING:
            ascendingStateEnterTime = millis();
            copyPrelaunchBufferToFlightBuffer();
            launchtime = getCurrentSecondsSinceEpoch();
            break;

        case FREE_FALLING:
            break;

        case PARACHUTE_FALLING:
            openParachute();
            break;

        case RECONNECTING:
            if (setupWifiConnection(1000))
            {
                if (sendRocketState(5))
                {
                    changeState(SENDING_DATA);
                }
                else
                {
                    changeState(ERROR);
                }
            }
            break;

        case SENDING_DATA:
            if (uploadFlightData(5))
            {
                changeState(IDLING_OPEN);
            }
            else
            {
                changeState(ERROR);
            }
            break;
    }
}

void copyPrelaunchBufferToFlightBuffer()
{
    flightBufferIndex = 0;
    for (int i = prelaunchBufferIndex; i < prelaunchBufferIndex + PRELAUNCH_BUFFER_SIZE; i++)
    {
        FlightData data = prelaunchBuffer[i % PRELAUNCH_BUFFER_SIZE];
        flightBuffer[flightBufferIndex] = data;
        flightBufferIndex++;
    }
}

String stateToString(RocketState state) 
{
    switch(state) 
    {
        case ERROR: return "ERROR";
        case NOT_INITIALIZED: return "NOT_INITIALIZED";
        case IDLING_CLOSED: return "IDLING_CLOSED";
        case IDLING_OPEN: return "IDLING_OPEN";
        case WAITING_FOR_LAUNCH: return "WAITING_FOR_LAUNCH";
        case ASCENDING: return "ASCENDING";
        case FREE_FALLING: return "FREE_FALLING";
        case PARACHUTE_FALLING: return "PARACHUTE_FALLING";
        case RECONNECTING: return "RECONNECTING";
        case SENDING_DATA: return "SENDING_DATA";
        default: return "UNKNOWN";
    }
}

bool detectLanding(const FlightData &data)
{
    static unsigned long entryTime = 0; // milliseconds

    if (data.relativeAltitude < LANDING_DETECTION_ALTITUDE_THRESHOLD)
    {
        if (entryTime == 0) // just entered the zone
        {
            entryTime = millis();
        }

        if (millis() >= entryTime + LANDING_DETECTION_DURATION * 1000)
        {
            return true;
        }

        return false;
    }
    else
    {
        entryTime = 0;
        return false;
    }
}

bool sendRocketState(int attempts)
{
    if (launchpadIP == "") 
    {
        Serial.println("L'adresse IP du pas de tir n'est pas définie. Impossible d'envoyer l'état de la fusée.");
        return false;
    }

    for (int i = 0; i < attempts; i++)
    {
        HTTPClient httpClient;
        String serverPath = "http://" + launchpadIP + "/api/new-rocket-state";
        
        JsonDocument doc;
        doc["rocket-state"] = stateToString(currentState);
        String payload;
        serializeJson(doc, payload);
        
        httpClient.begin(serverPath);
        httpClient.addHeader("Content-Type", "application/json");
        
        Serial.println("Envoi de la requête d'identification au pas de tir...");
    
        int httpResponseCode = httpClient.POST(payload);
        String responsePayload = httpClient.getString();

        httpClient.end();
    
        if (httpResponseCode == 200) 
        {
            Serial.println("OK.");
            return true;
        } else 
        {
            Serial.println("Erreur : " + String(httpResponseCode) + " " + responsePayload);
        }

        shortErrorBuzzer();

        delay(1000);
    }

    return false;
}

void getPressureAndTemperature(float& pressure, float& temperature)
{
    sensors_event_t temp_event, pressure_event;
    pressureSensor.getEvents(&temp_event, &pressure_event);
    pressure = pressure_event.pressure / 1000.0; // Convert hPa to bars
    temperature = temp_event.temperature;
}

float calculateAltitude(float pressure)
{
    return 44330.0 * (1 - pow(pressure / 1013.25, 0.1903));
}

void getAcceleration(float& accelX, float& accelY, float& accelZ)
{
    accelX = accelerationSensor.readFloatAccelX() * g;
    accelY = accelerationSensor.readFloatAccelY() * g;
    accelZ = accelerationSensor.readFloatAccelZ() * g;
}

void getGyroscope(float& gyroX, float& gyroY, float& gyroZ)
{
    gyroX = accelerationSensor.readFloatGyroX();
    gyroY = accelerationSensor.readFloatGyroY();
    gyroZ = accelerationSensor.readFloatGyroZ();
}

float calculateMagnitude(float x, float y, float z)
{
    return sqrt(x * x + y * y + z * z);
}

bool detectApogee(const FlightData &data, int deltaTime)
{
    // Speed calculation (using a circular buffer for smoothing)

    static const int BUFFER_SIZE = 32;
    static float previousAltitude = 0.0;
    static int speedBufferIndex = 0;
    static float speedBuffer[BUFFER_SIZE] = {0};

    float currentSpeed = 1000 * (data.relativeAltitude - previousAltitude) / deltaTime; // m/s
    previousAltitude = data.relativeAltitude;

    speedBuffer[speedBufferIndex] = currentSpeed;
    speedBufferIndex = (speedBufferIndex + 1) % BUFFER_SIZE;

    float avgSpeed = 0.0;
    for (int i = 0; i < BUFFER_SIZE; i++) 
    {
        avgSpeed += speedBuffer[i];
    }
    avgSpeed /= BUFFER_SIZE; // m/s

    Serial.print("Vitesse moyenne : ");
    Serial.println(avgSpeed);

    // Detection of apogee (using a threshold on the speed and altitude)

    return avgSpeed <= APOGEE_DETECTION_SPEED_THRESHOLD && APOGEE_DETECTION_ALTITUDE_THRESHOLD <= data.relativeAltitude ;
}

bool shouldOpenParachute(const FlightData &data)
{
    return (data.relativeAltitude <= PARACHUTE_ALTITUDE_THRESHOLD);
}

void rocketWebSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    if (type == WStype_TEXT)
    {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload, length);
        if (error) {
            Serial.print(F("deserializeJson() failed: "));
            Serial.println(error.f_str());
            return;
        }        
        
        String type = doc["type"];

        Serial.print("Message websocket reçu. Type : ");
        Serial.println(type);

        if (type == "new-launchpad-state")
        {
            String newLaunchpadState = doc["launchpad-state"];
            if (newLaunchpadState.length() > 0)
            {
                if (newLaunchpadState == "IDLING" && currentState != IDLING_CLOSED && currentState != IDLING_OPEN)
                {
                    changeState(IDLING_OPEN);
                }
                else if (newLaunchpadState == "PRESSURIZING")
                {
                    changeState(WAITING_FOR_LAUNCH);
                }
            }
            
        }

        else if (type == "close-fairing")
        {
            changeState(IDLING_CLOSED);
        }

        else if (type == "open-fairing")
        {
            changeState(IDLING_OPEN);
        }

        else if (type == "filling")
        {
            espStartTime = doc["epoch-time"].as<unsigned long>() - millis() / 1000; 
            targetPressure = doc["target-pressure"].as<float>();
            targetWaterVolume = doc["target-water-volume"].as<float>();
        }
    }
}


bool tryToConnectToLaunchpad()
{
    Serial.println("Connexion au Wi-Fi du pas de tir...");
    WiFi.begin(LAUNCHPAD_SSID, LAUNCHPAD_PASSWORD);

    delay(2000);

    if (WiFi.status() == WL_CONNECTED) 
    {
        launchpadIP = WiFi.gatewayIP().toString();

        Serial.println("Connecté au Wi-Fi du pas de tir. IP locale : " + WiFi.localIP().toString() + ", IP du pas de tir : " + launchpadIP);

        webSocketClient.begin(launchpadIP, 81, "/"); 
        webSocketClient.onEvent(rocketWebSocketEvent);
        webSocketClient.setReconnectInterval(1000);

        return true;
    }
    else
    {
        String reason = "";
        switch (WiFi.status()) 
        {
            case WL_NO_SSID_AVAIL:
                reason = "SSID non disponible.";
                break;
            case WL_CONNECT_FAILED:
                reason = "Échec de la connexion.";
                break;
            case WL_CONNECTION_LOST:
                reason = "Connexion perdue.";
                break;
            case WL_DISCONNECTED:
                reason = "Déconnecté.";
                break;
            case WL_IDLE_STATUS:
                reason = "État d'idle.";
                break;
            case WL_NO_SHIELD:
                reason = "Pas de shield Wi-Fi.";
                break;
            case WL_SCAN_COMPLETED:
                reason = "Scan terminé.";
                break;
            case WL_CONNECTED:
                reason = "Déjà connecté.";
                break;
            default:
                reason = "Raison inconnue : " + String(WiFi.status());
                break;
        }

        Serial.println("Échec. Raison : " + reason);
        WiFi.disconnect();
        return false;
    }
}

void openParachute()
{
    Serial.println("Déploiement du parachute...");
    parachuteServo.write(15); 
    delay(1000); 
    Serial.println("Parachute déployé.");
}

void closeParachute()
{
    Serial.println("Fermeture du parachute...");
    parachuteServo.write(40); 
    delay(1000); 
    Serial.println("Parachute fermé.");
}

bool uploadFlightData(int attempts)
{
    if (launchpadIP == "") 
    {
        Serial.println("Erreur : l'adresse IP du pas de tir n'est pas définie.");
        return false;
    }

    Serial.print("Début uploadFlightData. Free heap: ");
    Serial.println(ESP.getFreeHeap());

    for (int i = 0; i < attempts; i++)
    {
        HTTPClient httpClient;
        String serverPath = "http://" + launchpadIP + "/api/upload-flight-data";
        
        JsonDocument doc;
        // Add top-level fields
        doc["launchtime"] = launchtime;
        doc["targetPressure"] = targetPressure;
        doc["targetWaterVolume"] = targetWaterVolume;

        // Create a nested array for flight data
        JsonArray flightDataArray = doc.createNestedArray("flightData");

        for (int j = 0; j < flightBufferIndex; j++) {
            JsonObject flightDataObject = flightDataArray.add<JsonObject>();
            flightDataObject["timestamp"] = flightBuffer[j].timestamp;
            flightDataObject["temperature"] = flightBuffer[j].temperature;
            flightDataObject["pressure"] = flightBuffer[j].pressure;
            flightDataObject["relativeAltitude"] = flightBuffer[j].relativeAltitude;
            flightDataObject["accelX"] = flightBuffer[j].accelX;
            flightDataObject["accelY"] = flightBuffer[j].accelY;
            flightDataObject["accelZ"] = flightBuffer[j].accelZ;
            flightDataObject["gyroX"] = flightBuffer[j].gyroX;
            flightDataObject["gyroY"] = flightBuffer[j].gyroY;
            flightDataObject["gyroZ"] = flightBuffer[j].gyroZ;
        }
        
        Serial.print("Après population JsonDocument. Free heap: ");
        Serial.println(ESP.getFreeHeap());
        
        String payload;
        size_t jsonSize = serializeJson(doc, payload);
        
        Serial.print("Après serializeJson. Free heap: ");
        Serial.println(ESP.getFreeHeap());
        Serial.print("Taille du payload JSON (bytes): ");
        Serial.println(jsonSize);

        if (jsonSize == 0 && flightBufferIndex > 0) {
            Serial.println("Erreur: La sérialisation JSON a échoué (payload trop grand ou OOM).");
            shortErrorBuzzer();
            delay(1000);
            continue; 
        }
        
        httpClient.begin(serverPath);
        httpClient.addHeader("Content-Type", "application/json");
        
        Serial.println("Envoi des données de vol au pas de tir...");
    
        int httpResponseCode = httpClient.POST(payload);
        String responsePayload = httpClient.getString();

        httpClient.end();
        
        Serial.print("Après httpClient.POST. Free heap: ");
        Serial.println(ESP.getFreeHeap());
    
        if (httpResponseCode == 200) 
        {
            Serial.println("Données de vol envoyées avec succès.");
            return true;
        } 
        else 
        {
            Serial.println("Erreur lors de l'envoi des données de vol au pas de tir : " + String(httpResponseCode) + " " + responsePayload);
        }

        shortErrorBuzzer();
        delay(1000);
    }

    return false;
}

unsigned long getSamplingIntervalMs() {
    switch (currentState) {
        case WAITING_FOR_LAUNCH:
            return 1000 / HZ_WAITING_FOR_LAUNCH;
        case ASCENDING:
            if (millis() - ascendingStateEnterTime < (MAX_ASCENDING_DURATION * 1000)) {
                return 1000 / HZ_ASCENDING;
            } else {
                return 1000 / HZ_DEFAULT;
            }
        default: 
            return 1000 / HZ_DEFAULT;
    }
}

void validationBuzzer()
{
    analogWrite(BUZZER_PIN, 16);
    delay(200);

    analogWrite(BUZZER_PIN, 255);
    delay(250);
    
    analogWrite(BUZZER_PIN, LOW);
}

void longErrorBuzzer()
{
    analogWrite(BUZZER_PIN, 16);
    delay(500);

    analogWrite(BUZZER_PIN, LOW);
}

void shortErrorBuzzer()
{
    analogWrite(BUZZER_PIN, 16);
    delay(50);

    analogWrite(BUZZER_PIN, LOW);
}

void fallingBuzzer()
{
    static unsigned long lastBuzzTime = 0;
    unsigned long currentTime = millis();

    if (currentTime - lastBuzzTime < 600) {
        lastBuzzTime = currentTime;
        analogWrite(BUZZER_PIN, 16);
        delay(200);
        analogWrite(BUZZER_PIN, LOW);
        return;
    }
}

unsigned long getCurrentSecondsSinceEpoch() {
    return espStartTime + millis() / 1000;
}