#include "Adafruit_TinyUSB.h"
#include <light_CD74HC4067.h>


CD74HC4067 mux(9, 8, 7, 6); 
const int inputPin = 3;                 // Pin Connected to Sig pin of CD74HC4067
const int muxEnablePin = 7;            // Pin Connected to Sig pin of CD74HC4067


const int numChannels = 16;
uint8_t pinHistory[numChannels];
bool Buttons[numChannels];
const int debounceDelay = 10;
unsigned long lastDebounceTime[numChannels];
const int historyUpdateInterval = 10;  // Input polling update interval

void setup() {
    Serial.begin(115200);
    pinMode(inputPin, INPUT_PULLUP);    // Set as input for reading through signal pin with resistor pull-up
    pinMode(muxEnablePin, OUTPUT);     // sets the 4067 enable pin as output
    digitalWrite(muxEnablePin, HIGH);  // sets the 4067 enable pin as HIGH
}

void loop() {
  for (int channel = 0; channel < numChannels; ++channel) {
    updatePinHistory(channel);
    debounceButton(channel);
//      Serial.println(channel);
        delay(00);

  }

}

void updatePinHistory(int channel) {
  if ((micros() - lastDebounceTime[channel]) > historyUpdateInterval) {
    pinHistory[channel] = (pinHistory[channel] << 1);
    mux.channel(channel);
    int pinValue = digitalRead(inputPin);
    bitWrite(pinHistory[channel], 0, pinValue);
    lastDebounceTime[channel] = micros();
  }
}

void debounceButton(int channel) {
  int buttonState = bitRead(pinHistory[channel], 0);
  
  if (buttonState != Buttons[channel]) {
    lastDebounceTime[channel] = millis();
  }

  if ((millis() - lastDebounceTime[channel]) > debounceDelay) {
    Buttons[channel] = buttonState;

    if (buttonState == HIGH) {
      NoteOn(channel);
    } else {
      NoteOff(channel);
    }
  }
}

void NoteOn(int channel) {
  Serial.print("NoteOn - Channel ");
  Serial.println(channel);
  // Add your custom NoteOn logic here
}

void NoteOff(int channel) {
  Serial.print("NoteOff - Channel ");
  Serial.println(channel);
  // Add your custom NoteOff logic here
}
