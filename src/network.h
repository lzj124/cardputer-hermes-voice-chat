#pragma once
// ClawVoice — Network: HTTP client + USB Serial transport
// WiFi mode: raw WiFiClient, ZERO heap String allocations.
// USB mode: Serial bridge to Mac-side usb_bridge.py → proxy.py
// Supports SD card: TTS response can be streamed to file.

#include <WiFiClient.h>
#include <SD.h>
#include "config.h"

// Connection mode (defined in config.h)
// enum class Transport { WIFI, USB };

class Network {
public:
    String proxyHost = PROXY_HOST;
    int    proxyPort = atoi(PROXY_PORT);
    Transport transport = Transport::WIFI;

    // ── USB Serial Protocol ───────────────────────────────
    // ESP32 ↔ Mac bridge (usb_bridge.py):
    //   ESP32→Mac: "HELLO\n"              handshake
    //   Mac→ESP32: "OK\n"                 handshake ack
    //   ESP32→Mac: "SEND <len>\n" <bytes> send PCM recording
    //   Mac→ESP32: "RECV <len>\n" <bytes> receive TTS PCM
    //   Mac→ESP32: "ERR\n"                pipeline failed

    // Non-blocking: send HELLO once, check for OK (call repeatedly)
    bool usbPending = false;
    unsigned long usbHelloTime = 0;

    bool usbPoll() {
        if (!usbPending) {
            // Send HELLO, wait for next poll to check
            Serial.println("HELLO");
            Serial.flush();
            usbPending = true;
            usbHelloTime = millis();
            return false;
        }
        // Check for OK response
        if (Serial.available()) {
            char buf[16];
            size_t len = Serial.readBytesUntil('\n', buf, sizeof(buf) - 1);
            if (len > 0) {
                buf[len] = '\0';
                if (buf[len-1] == '\r') buf[len-1] = '\0';
                if (strcmp(buf, "OK") == 0) {
                    Serial.println("[USB] Bridge connected");
                    usbPending = false;
                    return true;
                }
            }
        }
        // Timeout after 3s, reset to try again later
        if (millis() - usbHelloTime > 3000) {
            usbPending = false;
        }
        return false;
    }

    // ── Voice Pipeline ────────────────────────────────────
    // mode 0: send PCM from RAM pointer, receive to RAM buffer
    // mode 1: send PCM from SD file (stream), receive to SD file
    //
    // pcmData/pcmSamples: RAM recording data (mode 0) or nullptr/0 (mode 1)
    // outBuffer/outMaxSamples: RAM output buffer (mode 0)
    // useSD: stream TTS response to SD_TTS_FILE instead of RAM
    // sdRecBytes: bytes of recording on SD (used when pcmData is null)
    // tick(): called during blocking waits to keep display alive

    size_t sendVoice(int16_t* pcmData, size_t pcmSamples,
                     int16_t* outBuffer, size_t outMaxSamples,
                     bool useSD, size_t sdRecBytes = 0,
                     void (*tick)() = nullptr) {
        WiFiClient client;
        client.setTimeout(15);

        size_t pcmBytes;
        if (pcmData) {
            pcmBytes = pcmSamples * sizeof(int16_t);
        } else {
            // SD mode: we'll stream from file
            pcmBytes = sdRecBytes;
        }

        Serial.printf("[NET] POST /voice  %zu bytes PCM (%.1fs) [%s]\n",
                      pcmBytes, (float)(pcmBytes / sizeof(int16_t)) / MIC_SAMPLE_RATE,
                      pcmData ? "DRAM" : "SD");

        if (!client.connect(proxyHost.c_str(), proxyPort)) {
            Serial.println("[NET] Connection failed");
            return 0;
        }

        client.printf("POST /voice HTTP/1.1\r\n"
                      "Host: %s:%d\r\n"
                      "Content-Type: application/pcm\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      proxyHost.c_str(), proxyPort, pcmBytes);

        // Send PCM body
        if (pcmData) {
            // RAM mode: send directly
            const size_t CHUNK = 2048;
            size_t sent = 0;
            const uint8_t* ptr = (const uint8_t*)pcmData;
            while (sent < pcmBytes) {
                size_t n = (pcmBytes - sent > CHUNK) ? CHUNK : pcmBytes - sent;
                if (client.write(ptr + sent, n) == 0) {
                    Serial.println("[NET] Write failed");
                    client.stop(); return 0;
                }
                sent += n;
                yield();
            }
        } else {
            // SD mode: stream from SD card file
            File f = SD.open(SD_RECORD_FILE, FILE_READ);
            if (!f) {
                Serial.println("[NET] Can't open SD record file");
                client.stop(); return 0;
            }
            uint8_t chunk[2048];
            size_t sent = 0;
            while (sent < pcmBytes) {
                size_t want = (pcmBytes - sent > sizeof(chunk)) ? sizeof(chunk) : pcmBytes - sent;
                size_t got = f.read(chunk, want);
                if (got == 0) break;
                if (client.write(chunk, got) == 0) {
                    Serial.println("[NET] SD stream write failed");
                    f.close(); client.stop(); return 0;
                }
                sent += got;
                yield();
            }
            f.close();
            Serial.printf("[NET] SD stream sent: %zu/%zu bytes\n", sent, pcmBytes);
        }

        // ── Read response ─────────────────────────────────
        unsigned long t0 = millis();
        int contentLength = -1;

        while (!client.available()) {
            if (millis() - t0 > HTTP_TIMEOUT) {
                Serial.println("[NET] Response timeout");
                client.stop(); return 0;
            }
            if (tick) tick(); else yield();
        }

        // Read headers
        char lineBuf[160];
        bool headerDone = false;
        while (!headerDone) {
            while (!client.available()) {
                if (millis() - t0 > HTTP_TIMEOUT) break;
                if (tick) tick(); else yield();
            }
            if (!client.available()) break;

            size_t len = client.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
            if (len == 0) break;
            lineBuf[len] = '\0';

            if (len > 0 && lineBuf[len - 1] == '\r') {
                lineBuf[len - 1] = '\0';
                len--;
            }

            if (strncmp(lineBuf, "Content-Length:", 15) == 0) {
                contentLength = atoi(lineBuf + 15);
            }
            if (len == 0) headerDone = true;
        }

        if (contentLength <= 0) {
            Serial.println("[NET] No valid Content-Length");
            client.stop(); return 0;
        }

        // ── Read body ─────────────────────────────────────
        size_t totalRead = 0;
        uint8_t chunk[1024];

        if (useSD) {
            // Stream TTS response to SD file
            SD.remove(SD_TTS_FILE);
            File ttsFile = SD.open(SD_TTS_FILE, FILE_WRITE);
            if (!ttsFile) {
                Serial.println("[NET] Can't open SD TTS file for writing");
                client.stop(); return 0;
            }

            while (totalRead < (size_t)contentLength) {
                if (client.available()) {
                    int n = client.read(chunk, min(sizeof(chunk), (size_t)contentLength - totalRead));
                    if (n > 0) {
                        ttsFile.write(chunk, n);
                        totalRead += n;
                    } else break;
                } else {
                    if (!client.connected()) {
                        delay(100);
                        if (!client.available()) break;
                        continue;
                    }
                    if (millis() - t0 > HTTP_TIMEOUT) {
                        Serial.printf("[NET] SD body timeout at %zu/%d\n", totalRead, contentLength);
                        break;
                    }
                    if (tick) tick(); else yield();
                }
            }
            ttsFile.close();
            client.stop();

            size_t outSamples = totalRead / sizeof(int16_t);
            Serial.printf("[NET] TTS → SD: %zu bytes (%.1fs @ %dHz)\n",
                          totalRead, (float)outSamples / PLAY_SAMPLE_RATE, PLAY_SAMPLE_RATE);
            return outSamples;
        } else {
            // RAM mode: read to buffer
            size_t outBytes = outMaxSamples * sizeof(int16_t);
            size_t targetRead = min((size_t)contentLength, outBytes);
            uint8_t* outPtr = (uint8_t*)outBuffer;

            while (totalRead < targetRead) {
                if (client.available()) {
                    int n = client.read(chunk, min(sizeof(chunk), targetRead - totalRead));
                    if (n > 0) {
                        memcpy(outPtr + totalRead, chunk, n);
                        totalRead += n;
                    } else break;
                } else {
                    if (!client.connected()) {
                        delay(100);
                        if (!client.available()) break;
                        continue;
                    }
                    if (millis() - t0 > HTTP_TIMEOUT) {
                        Serial.printf("[NET] Body timeout at %zu/%d\n", totalRead, contentLength);
                        break;
                    }
                    if (tick) tick(); else yield();
                }
            }

            client.stop();
            size_t outSamples = totalRead / sizeof(int16_t);
            Serial.printf("[NET] Got %zu bytes PCM (%.1fs @ %dHz)\n",
                          totalRead, (float)outSamples / PLAY_SAMPLE_RATE, PLAY_SAMPLE_RATE);
            return outSamples;
        }
    }

    // ── USB Voice Pipeline ────────────────────────────────
    // Sends PCM over Serial to Mac bridge, receives TTS PCM back.
    // Works with SD or RAM recording data.

    size_t sendVoiceUSB(int16_t* pcmData, size_t pcmSamples,
                        bool useSD, size_t sdRecBytes = 0,
                        void (*tick)() = nullptr) {
        size_t pcmBytes;
        if (pcmData) {
            pcmBytes = pcmSamples * sizeof(int16_t);
        } else {
            pcmBytes = sdRecBytes;
        }

        Serial.printf("[USB] SEND %zu bytes (%.1fs) [%s]\n",
                      pcmBytes, (float)(pcmBytes / sizeof(int16_t)) / MIC_SAMPLE_RATE,
                      pcmData ? "DRAM" : "SD");

        // Send header
        Serial.printf("SEND %zu\n", pcmBytes);
        Serial.flush();

        // Send PCM body
        if (pcmData) {
            Serial.write((const uint8_t*)pcmData, pcmBytes);
            Serial.flush();
        } else {
            // Stream from SD
            File f = SD.open(SD_RECORD_FILE, FILE_READ);
            if (!f) {
                Serial.println("[USB] Can't open SD record file");
                return 0;
            }
            uint8_t chunk[2048];
            size_t sent = 0;
            while (sent < pcmBytes) {
                size_t want = (pcmBytes - sent > sizeof(chunk)) ? sizeof(chunk) : pcmBytes - sent;
                size_t got = f.read(chunk, want);
                if (got == 0) break;
                Serial.write(chunk, got);
                sent += got;
                if (tick) tick(); else yield();
            }
            Serial.flush();
            f.close();
        }

        // ── Read response header ──────────────────────────
        unsigned long t0 = millis();
        char lineBuf[64];

        while (millis() - t0 < HTTP_TIMEOUT) {
            if (Serial.available()) {
                size_t len = Serial.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
                if (len > 0) {
                    lineBuf[len] = '\0';
                    if (len > 0 && lineBuf[len-1] == '\r') { lineBuf[len-1] = '\0'; len--; }

                    // ERR response
                    if (strcmp(lineBuf, "ERR") == 0) {
                        Serial.println("[USB] Bridge returned ERR");
                        return 0;
                    }

                    // RECV <len> response
                    if (strncmp(lineBuf, "RECV ", 5) == 0) {
                        size_t respBytes = atol(lineBuf + 5);
                        if (respBytes == 0) return 0;

                        Serial.printf("[USB] RECV %zu bytes incoming\n", respBytes);

                        // Read response body
                        if (useSD) {
                            // Stream to SD file
                            SD.remove(SD_TTS_FILE);
                            File ttsFile = SD.open(SD_TTS_FILE, FILE_WRITE);
                            if (!ttsFile) {
                                Serial.println("[USB] Can't open SD TTS file");
                                return 0;
                            }
                            // Drain any leftover from header read
                            delay(100);
                            Serial.setTimeout(5000);  // 5s per readBytes call
                            size_t totalRead = 0;
                            unsigned long lastData = millis();
                            while (totalRead < respBytes) {
                                size_t want = respBytes - totalRead;
                                uint8_t chunk[256];
                                size_t toRead = (want > sizeof(chunk)) ? sizeof(chunk) : want;
                                int n = Serial.readBytes((char*)chunk, toRead);
                                if (n > 0) {
                                    ttsFile.write(chunk, n);
                                    totalRead += n;
                                    lastData = millis();
                                } else {
                                    if (millis() - lastData > 10000) {
                                        Serial.printf("[USB] TTS timeout: %zu/%zu\n", totalRead, respBytes);
                                        break;
                                    }
                                    if (tick) tick();
                                    delay(1);
                                }
                            }
                            Serial.setTimeout(1000);  // restore default
                            ttsFile.close();
                            size_t samples = totalRead / sizeof(int16_t);
                            Serial.printf("[USB] TTS → SD: %zu bytes (%.1fs)\n",
                                          totalRead, (float)samples / PLAY_SAMPLE_RATE);
                            return samples;
                        } else {
                            // RAM mode: read and discard (USB needs SD)
                            Serial.println("[USB] Warning: USB mode requires SD for TTS");
                            size_t totalRead = 0;
                            unsigned long lastData = millis();
                            uint8_t chunk[4096];
                            while (totalRead < respBytes) {
                                if (Serial.available()) {
                                    int n = Serial.readBytes((char*)chunk,
                                        min(sizeof(chunk), respBytes - totalRead));
                                    if (n > 0) {
                                        totalRead += n;
                                        lastData = millis();
                                    }
                                } else {
                                    if (millis() - lastData > 10000) break;
                                    delay(5);
                                }
                            }
                            return totalRead / sizeof(int16_t);
                        }
                    }
                }
            }
            if (tick) tick(); else delay(10);
        }

        Serial.println("[USB] Response timeout");
        return 0;
    }

    // ── Text Pipeline (unchanged — always RAM, WiFi only) ─
    size_t sendText(const char* text, int16_t* outBuffer, size_t outMaxSamples) {
        WiFiClient client;
        client.setTimeout(15);

        char json[512];
        char escaped[384];
        int ei = 0;
        for (const char* p = text; *p && ei < (int)sizeof(escaped) - 3; p++) {
            if (*p == '"')       { escaped[ei++] = '\\'; escaped[ei++] = '"'; }
            else if (*p == '\\') { escaped[ei++] = '\\'; escaped[ei++] = '\\'; }
            else if (*p == '\n') { escaped[ei++] = '\\'; escaped[ei++] = 'n'; }
            else if ((uint8_t)*p < 0x20) { continue; }
            else { escaped[ei++] = *p; }
        }
        escaped[ei] = '\0';

        int bodyLen = snprintf(json, sizeof(json), "{\"text\":\"%s\"}", escaped);
        Serial.printf("[NET] POST /text  (%d bytes): %.60s\n", bodyLen, text);

        if (!client.connect(proxyHost.c_str(), proxyPort)) {
            Serial.println("[NET] Connection failed");
            return 0;
        }

        client.printf("POST /text HTTP/1.1\r\n"
                      "Host: %s:%d\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %d\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      proxyHost.c_str(), proxyPort, bodyLen);
        client.write((const uint8_t*)json, bodyLen);

        unsigned long t0 = millis();
        int contentLength = -1;

        while (!client.available()) {
            if (millis() - t0 > HTTP_TIMEOUT) { client.stop(); return 0; }
            yield();
        }

        char lineBuf[160];
        bool headerDone = false;
        while (!headerDone) {
            while (!client.available()) {
                if (millis() - t0 > HTTP_TIMEOUT) break;
                yield();
            }
            if (!client.available()) break;

            size_t len = client.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
            if (len == 0) break;
            lineBuf[len] = '\0';

            if (len > 0 && lineBuf[len - 1] == '\r') {
                lineBuf[len - 1] = '\0';
                len--;
            }

            if (strncmp(lineBuf, "Content-Length:", 15) == 0) {
                contentLength = atoi(lineBuf + 15);
            }
            if (len == 0) headerDone = true;
        }

        if (contentLength <= 0) { client.stop(); return 0; }

        size_t outBytes = outMaxSamples * sizeof(int16_t);
        size_t targetRead = min((size_t)contentLength, outBytes);
        size_t totalRead = 0;
        uint8_t* outPtr = (uint8_t*)outBuffer;
        uint8_t chunk[1024];

        while (totalRead < targetRead) {
            if (client.available()) {
                int n = client.read(chunk, min(sizeof(chunk), targetRead - totalRead));
                if (n > 0) { memcpy(outPtr + totalRead, chunk, n); totalRead += n; }
                else break;
            } else {
                if (!client.connected()) {
                    delay(100);
                    if (!client.available()) break;
                    continue;
                }
                if (millis() - t0 > HTTP_TIMEOUT) break;
                yield();
            }
        }

        client.stop();
        size_t outSamples = totalRead / sizeof(int16_t);
        Serial.printf("[NET] TTS received %zu bytes (%.1fs)\n",
                      totalRead, (float)outSamples / PLAY_SAMPLE_RATE);
        return outSamples;
    }
};
