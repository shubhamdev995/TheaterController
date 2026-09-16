
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h> 
/* Put your SSID & Password */const char* ssid = "sadmin";  const char* password = "12345678";  
/* Hard-assigned physical GPIO output maps */const int PIN_A = 2; const int PIN_B = 4; const int PIN_C = 5; const int PIN_D = 16; const int PIN_E = 17;
/* Static IP Address settings for the Access Point */IPAddress local_ip(192,168,0,1); IPAddress gateway(192,168,0,1);  IPAddress subnet(255,255,255,0); 
AsyncWebServer server(80);AsyncWebSocket ws("/ws");
// Structure to split a start/end cue into discrete turn-on and turn-off actions
struct CuePoint {
    unsigned long timeMs; // Execution time in milliseconds
    int pin;              // Physical GPIO Pin
    int state;            // HIGH or LOW
    bool executed;        // Track if this cue was already fired
};
const int MAX_CUES = 300; CuePoint cueTimeline[MAX_CUES];int totalCues = 0;
// Central Playback Clock Engine
unsigned long baseAudioTimeMs = 0;   unsigned long espSyncAnchorMicros = 0; bool isPlaying = false;              bool isManualOverride = false; 
// Converts input character parameters into operational GPIO channel identifiers
int getPinFromChar(char c) {
    switch (c) {
        case 'a': return PIN_A; case 'b': return PIN_B; case 'c': return PIN_C; case 'd': return PIN_D; case 'e': return PIN_E;
        default:  return -1; 
    }
}
// Drives a bundle string of pins to a matching high/low configurationv
void processPins(String pinList, int state) {
    for (size_t i = 0; i < pinList.length(); i++) {
        char pinChar = pinList.charAt(i);
        int targetPin = getPinFromChar(pinChar);
        
        if (targetPin != -1) {
            digitalWrite(targetPin, state);
            Serial.printf("Manual Link Pin: Setting '%c' (GPIO %d) to %s\n", pinChar, targetPin, state == HIGH ? "HIGH" : "LOW");
        }
    }
}
// Drops all operational paths to a low electrical configuration
void turnAllPinsOff() {
    digitalWrite(PIN_A, LOW); digitalWrite(PIN_B, LOW); digitalWrite(PIN_C, LOW); digitalWrite(PIN_D, LOW); digitalWrite(PIN_E, LOW);
}
// Recalculates exact target states for all 5 lamps based on the seek timestamp
void recalibrateAndSyncLampStates(unsigned long targetTimeMs) {
    int targetStates[5] = {LOW, LOW, LOW, LOW, LOW};
    int pins[5] = {PIN_A, PIN_B, PIN_C, PIN_D, PIN_E};
    unsigned long latestActionTimes[5] = {0, 0, 0, 0, 0};

    for (int i = 0; i < totalCues; i++) {
        if (cueTimeline[i].timeMs <= targetTimeMs) {
            int pinIdx = -1;
            for (int p = 0; p < 5; p++) {
                if (pins[p] == cueTimeline[i].pin) { pinIdx = p; break; }
            }
            
            if (pinIdx != -1 && cueTimeline[i].timeMs >= latestActionTimes[pinIdx]) {
                latestActionTimes[pinIdx] = cueTimeline[i].timeMs;
                targetStates[pinIdx] = cueTimeline[i].state;
            }
            cueTimeline[i].executed = true; 
        } else {
            cueTimeline[i].executed = false;
        }
    }

    for (int p = 0; p < 5; p++) {
        digitalWrite(pins[p], targetStates[p]);
    }
}
// Extrapolates current timeline location using hardware micrometer counters between interval signals
unsigned long getCurrentAudioTimeMs() {
    if (!isPlaying) return baseAudioTimeMs;
    unsigned long elapsedMicros = micros() - espSyncAnchorMicros;
    return baseAudioTimeMs + (elapsedMicros / 1000);
}
// Transmits formatted text telemetry packets over the active socket session pipeline
void sendSyncTelemetry(long driftMs, const char* status, unsigned long currentBackendTime) {
    char jsonResponseBuffer[128];
    snprintf(jsonResponseBuffer, sizeof(jsonResponseBuffer),
             "{\"type\":\"sync_telemetry\",\"drift_ms\":%ld,\"drift_status\":\"%s\",\"backend_time_ms\":%lu}",
             driftMs, status, currentBackendTime);
             
    ws.textAll(jsonResponseBuffer); 
}
// Core payload parsing engine handling incoming frontend transactionsv
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        data[len] = 0; 
        
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, (char*)data);
        if (error) {
            Serial.printf("JSON Parse Failed: %s\n", error.c_str());
            return;
        }

        String msgType = doc["type"];

        // 1. TYPE: manual_lamp (WebSocket manual interface handler)
        if (msgType == "manual_lamp") {
            isManualOverride = true; 
            isPlaying = false; // Freeze background sequence checking actions

            String lampStr = doc["lamp"].as<String>();
            bool shouldTurnOn = doc["on"].as<bool>();

            if (lampStr.length() > 0) {
                int targetPin = getPinFromChar(lampStr.charAt(0));
                if (targetPin != -1) {
                    digitalWrite(targetPin, shouldTurnOn ? HIGH : LOW);
                    Serial.printf("[WS MANUAL] Lamp '%s' (GPIO %d) set to %s\n", 
                                  lampStr.c_str(), targetPin, shouldTurnOn ? "ON" : "OFF");
                }
            }
        } 
        // 2. TYPE: load_events
        else if (msgType == "load_events") {
            JsonArray eventsArray = doc["events"].as<JsonArray>();
            totalCues = 0;
            turnAllPinsOff();
            isManualOverride = false; 
            
            for (JsonObject e : eventsArray) {
                String lampStr = e["lamp"].as<String>();
                if (lampStr.length() == 0) continue;
                
                int pinTarget = getPinFromChar(lampStr.charAt(0));
                if (pinTarget == -1) continue;

                unsigned long startTimeMs = (unsigned long)(e["start"].as<double>() * 1000.0);
                unsigned long endTimeMs = (unsigned long)(e["end"].as<double>() * 1000.0);

                if (totalCues < MAX_CUES) { cueTimeline[totalCues] = { startTimeMs, pinTarget, HIGH, false }; totalCues++; }
                if (totalCues < MAX_CUES) { cueTimeline[totalCues] = { endTimeMs, pinTarget, LOW, false }; totalCues++; }
            }
            Serial.printf("[LOADED] Compiled %d executable toggle actions.\n", totalCues);
        } 
        // 3. TYPE: sync
        else if (msgType == "sync") {
            isManualOverride = false; // Re-align tracker precedence patterns back to automatic timeline
            
            unsigned long frontendTimeMs = (unsigned long)(doc["time"].as<double>() * 1000.0);
            bool incomingPlaying = doc["playing"].as<bool>();
            
            unsigned long internalClockSnapshot = getCurrentAudioTimeMs();
            long driftMs = (long)frontendTimeMs - (long)internalClockSnapshot;
            const char* statusReport = "NORMAL_TRACKING";

            if (abs(driftMs) > 500) {
                recalibrateAndSyncLampStates(frontendTimeMs);
                statusReport = "FIXED_VIA_RECALIBRATION";
            } else {
                statusReport = "FIXED_VIA_CLOCK_STEP";
            }

            baseAudioTimeMs = frontendTimeMs;
            espSyncAnchorMicros = micros();
            isPlaying = incomingPlaying;

            sendSyncTelemetry(driftMs, statusReport, baseAudioTimeMs);
        }
        // 4. TYPE: play
        else if (msgType == "play") {
            isManualOverride = false; 
            baseAudioTimeMs = (unsigned long)(doc["time"].as<double>() * 1000.0);
            espSyncAnchorMicros = micros();
            recalibrateAndSyncLampStates(baseAudioTimeMs); 
            isPlaying = true;
            Serial.printf("[PLAY] Started running timeline tracking at: %lu ms\n", baseAudioTimeMs);
        }
        // 5. TYPE: pause
        else if (msgType == "pause") {
            baseAudioTimeMs = (unsigned long)(doc["time"].as<double>() * 1000.0);
            isPlaying = false;
            Serial.printf("[PAUSE] Timeline paused at position: %lu ms\n", baseAudioTimeMs);
        }
        // 6. TYPE: seek / forward
        else if (msgType == "seek" || msgType == "forward") {
            isManualOverride = false;
            baseAudioTimeMs = (unsigned long)(doc["time"].as<double>() * 1000.0);
            espSyncAnchorMicros = micros();
            recalibrateAndSyncLampStates(baseAudioTimeMs); 
        }
        // 7. TYPE: reset
        else if (msgType == "reset") {
            isManualOverride = false;
            baseAudioTimeMs = 0;
            isPlaying = false;
            turnAllPinsOff();
            recalibrateAndSyncLampStates(0);
            Serial.println("[RESET] Project tracking metrics restored to 0.00s base");
        }
    }
}
void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.printf("[WS] UI Terminal session #%u connected.\n", client->id());
            break;
        case WS_EVT_DISCONNECT:
            Serial.printf("[WS] Terminal session #%u disconnected.\n", client->id());
            isPlaying = false;
            turnAllPinsOff(); 
            break;
        case WS_EVT_DATA:
            handleWebSocketMessage(arg, data, len);
            break;
        default:
            break;
    }
}
void setup() {
    // Declare physical load pins
    pinMode(PIN_A, OUTPUT); pinMode(PIN_B, OUTPUT); pinMode(PIN_C, OUTPUT); pinMode(PIN_D, OUTPUT); pinMode(PIN_E, OUTPUT);
    turnAllPinsOff();

    Serial.begin(115200);
    LittleFS.begin();
    
    // Mount SoftAP infrastructure rules
    WiFi.softAPConfig(local_ip, gateway, subnet);
    WiFi.softAP(ssid, password);
    delay(100);
    
    // Bind WebSocket tasks into central core servers
    ws.onEvent(onEvent);
    server.addHandler(&ws);


// Standard static base page mapping route
server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
request->send(LittleFS, "/index.html", "text/html");
});
// Fallback legacy manual HTTP path handling rules
server.on("/switch", HTTP_GET, [](AsyncWebServerRequest *request) {
isManualOverride = true;
isPlaying = false;
if (request->hasParam("on")) processPins(request->getParam("on")->value(), HIGH);
if (request->hasParam("off")) processPins(request->getParam("off")->value(), LOW);
request->send(200, "text/plain", "HTTP Control Acknowledged.");
});
server.begin();
Serial.println("[INIT] Systems completely compiled and online.");
}
void loop() {
ws.cleanupClients();
// Check scheduling timelines if user is not in manual mode
if (!isManualOverride && isPlaying && totalCues > 0) {
unsigned long currentTimelineMs = getCurrentAudioTimeMs();
for (int i = 0; i < totalCues; i++) {
if (!cueTimeline[i].executed && currentTimelineMs >= cueTimeline[i].timeMs) {
digitalWrite(cueTimeline[i].pin, cueTimeline[i].state);
cueTimeline[i].executed = true;
Serial.printf("[AUTOMATION] GPIO %d changed state to %s at internal clock tracking: %lu ms\n",
cueTimeline[i].pin, cueTimeline[i].state == HIGH ? "HIGH" : "LOW", currentTimelineMs);
}
}
}
}

