#include <Arduino.h>
#include <M5Cardputer.h>

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setTextColor(WHITE);
    M5Cardputer.Display.setTextSize(2);

    // Test arrow characters
    M5Cardputer.Display.setCursor(10, 10);
    M5Cardputer.Display.println("ASCII arrows:");
    M5Cardputer.Display.setCursor(10, 30);
    M5Cardputer.Display.println("^ v < >");

    M5Cardputer.Display.setCursor(10, 60);
    M5Cardputer.Display.println("Unicode arrows:");
    M5Cardputer.Display.setCursor(10, 80);
    M5Cardputer.Display.println("\xe2\x86\x91  \xe2\x86\x93  \xe2\x86\x90  \xe2\x86\x92");

    M5Cardputer.Display.setCursor(10, 110);
    M5Cardputer.Display.println("VS15 (text):");
    M5Cardputer.Display.setCursor(10, 130);
    M5Cardputer.Display.println("\xef\xb8\x8f");

    M5Cardputer.Display.setCursor(10, 160);
    M5Cardputer.Display.println("Emoji arrows:");
    M5Cardputer.Display.setCursor(10, 180);
    M5Cardputer.Display.println("\xf0\x9f\xa1\xb5  \xf0\x9f\xa1\xb7  \xf0\x9f\xa1\xb4  \xf0\x9f\xa1\xb6");
}

void loop() {
    delay(100);
}
