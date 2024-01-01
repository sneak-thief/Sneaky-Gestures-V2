/*****************************************************************************/
// Sneaky-Gestures V2 - MIDI BLE glove
/*******************************************************************************/

#include "Wire.h"
#include "Adafruit_TinyUSB.h"  // USB Library
#include <light_CD74HC4067.h>  // 74HC4067 Library
#include <bluefruit.h>         // Adafruit Bluefruit BLE Library
#include <MIDI.h>              // MIDI Library
#include "LSM6DS3.h"           // IMU Library
#include <Adafruit_NeoPixel.h> // Neopixel Library
#include "ACHarmonizer.h"

BLEDis bledis;
BLEMidi blemidi;

// Create a new instance of the Arduino MIDI Library, and attach BluefruitLE MIDI as the transport.
MIDI_CREATE_BLE_INSTANCE(blemidi);

// MIDI Channel
unsigned int MIDIchannel = 1;

// MIDI Note defaults
unsigned int Keys[12]; // Assign an array to hold the notes for each key
unsigned int RootNote = 60;
unsigned int Spread = 1;
signed int RootNoteOffset = 0;
unsigned int IndexLatch = 0;
unsigned int harmonizedNote;
unsigned int Scale = 0; // Default 0 as musical scale from ACHarmonizer.cpp (0 = no scale quantization)

ACHarmonizer myHarmonizer; // Create an instance of ACHarmonizer

// Accelerometer variables
float rawAccelX; // Raw IMU data
float rawAccelY;
float rawAccelZ;
unsigned int AccelX; // Scaled IMU data
unsigned int AccelY;
// unsigned int AccelZ;                   // Z axis not needed for MIDI CC
unsigned int lastAccelX; // Previous IMU reading
unsigned int lastAccelY;
// unsigned int lastAccelZ;               // Z axis not needed for MIDI CC
unsigned long lastExecutionTimeAccel = 0; // Timer for sending IMU MIDI CC's
const unsigned long intervalAccel = 20;   // Send IMU MIDI CC's every X ms
unsigned int CCAccelX = 74;               // Set IMU X acis to MIDI CC 74
unsigned int CCAccelY = 71;               // Set IMU X acis to MIDI CC 71

// Temporary variables for processing analog inputs
int modPot = 0;   // analog pin A0
int pitchPot = 1; // analog pin A1
int lastModVal;
int lastPitchVal;

CD74HC4067 mux(9, 8, 7, 6); // 4067 Pins for S0, S1, S2 and S3
const int inputPin = 3;
const int muxEnablePin = 7;

const int firstChannel = 2;                  // First 4067 Channel to demux / scan from
const int numChannels = 16;                  // Last 4067 Channel to scan from (assuming all inputs are connected in a )
const int debounceDelay = 1;                 // Debounce delay in milliseconds
const int noteOffDelay = 5;                  // Delay for Note Off in milliseconds
unsigned long lastDebounceTime[numChannels]; // Debounce Timer
bool Buttons[numChannels];
bool prevButtons[numChannels];
bool noteOffFlag[numChannels];
bool noteStates[127] = {false}; // keep track of the play state of each note

// Create a instance of class LSM6DS3
LSM6DS3 myIMU(I2C_MODE, 0x6A); // I2C device address 0x6A
uint16_t errorsAndWarnings = 0;

void midiRead()
{

  // Don't continue if we aren't connected.
  if (!Bluefruit.connected())
  {
    return;
  }

  // Don't continue if the connected device isn't ready to receive messages.
  if (!blemidi.notifyEnabled())
  {
    return;
  }

  // read any new MIDI messages
  MIDI.read();
}

void startAdv(void)
{

  // Set General Discoverable Mode flag
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);

  // Advertise TX Power
  Bluefruit.Advertising.addTxPower();

  // Advertise BLE MIDI Service
  Bluefruit.Advertising.addService(blemidi);

  // Secondary Scan Response packet (optional)
  Bluefruit.ScanResponse.addName();

  // Start Advertising
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244); // in unit of 0.625 ms
  Bluefruit.Advertising.setFastTimeout(30);   // number of seconds in fast mode
  Bluefruit.Advertising.start(0);             // 0 = Don't stop advertising after n seconds
}

int digitalReadFromMultiplexer(int channel)
{ // Read each 4067 channel input
  mux.channel(channel);
  return digitalRead(inputPin);
}

void NoteOn(int channel)
{ // Note On routine
  Serial.print("NoteOn - Channel ");
  Serial.println(channel);
  harmonizedNote = Keys[(channel - 3)];
  MIDI.sendNoteOn(myHarmonizer.harmonize(harmonizedNote, Scale), 100, MIDIchannel); // Send a MIDI note on from the Keys array based on the triggered 4067 channel (Notes only on 4067 channels 3-14)
  Serial.println(myHarmonizer.harmonize(harmonizedNote, Scale));
  Serial.println(Scale);
}

void NoteOff(int channel)
{ // Note Off routine
  Serial.print("NoteOff - Channel ");
  Serial.println(channel);
  harmonizedNote = Keys[(channel - 3)];
  MIDI.sendNoteOff(myHarmonizer.harmonize(harmonizedNote, Scale), 100, MIDIchannel); // Send a MIDI note on from the Keys array based on the triggered 4067 channel (Notes only on 4067 channels 3-14)
}

void handleNoteOn(byte channel, byte pitch, byte velocity)
{

  // Log when a note is pressed.
  Serial.printf("Note on: channel = %d, pitch = %d, velocity - %d", channel, pitch, velocity);
  Serial.println();
}

void handleNoteOff(byte channel, byte pitch, byte velocity)
{

  // Log when a note is released.
  Serial.printf("Note off: channel = %d, pitch = %d, velocity - %d", channel, pitch, velocity);
  Serial.println();
}

// Read accelerometer and send MIDI CC's accordingly
void accelRead()
{

  // Convert accelerometer data to 7-bit 0-127 MIDI CC data
  rawAccelX = constrain((myIMU.readFloatAccelY()), -8, 8) + 8; // Reverse X and Y because of IMU orientation on hand
  rawAccelY = constrain((myIMU.readFloatAccelX()), -8, 8) + 8; // Reverse X and Y because of IMU orientation on hand
  // rawAccelZ = constrain((myIMU.readFloatAccelZ()),-8,8) + 8; // Z acis not needed for MIDI CC for now

  // IMU Output debug - delete later
  //    Serial.print(rawAccelX);
  //    Serial.print(" ");
  //    Serial.print(rawAccelY);
  //    Serial.print(" ");
  //    Serial.println(rawAccelZ);

  AccelX = labs(constrain(round((rawAccelX) * 8), 0, 127));
  AccelY = labs(constrain(round((rawAccelY) * 8), 0, 127));
  // AccelZ = labs(constrain(round((rawAccelZ) * 8),0,127));

  // Send MIDI CC data only if the accelerometer data has changed
  if (AccelX != lastAccelX)
  {
    //    Serial.print(" X1 = ");
    //    Serial.println(AccelX);
    MIDI.sendControlChange(CCAccelX, AccelX, MIDIchannel);
    lastAccelX = AccelX;
  }

  if (AccelY != lastAccelY)
  {
    //    Serial.print("         Y1 = ");
    //    Serial.println(AccelY);
    MIDI.sendControlChange(CCAccelY, AccelY, MIDIchannel);
    lastAccelY = AccelY;
  }
}

void NoteSpread(int RootNote, int Spread, int RootNoteOffset)
{ // Change number of semitones between the keys, eg. 1, 2, 3 & 4
  for (int i = 1; i < 13; ++i)
  {
    Keys[i - 1] = 60 + ((i - 1) * Spread) + RootNoteOffset;
  }
  for (size_t i = 0; i < 12; ++i)
  {
    Serial.print("Keys[");
    Serial.print(i);
    Serial.print("] = ");
    Serial.println(Keys[i]);
  }
}

void debounceButton(int channel)
{ // Debounce signal input pin
  int buttonState = digitalReadFromMultiplexer(channel);

  if (buttonState != prevButtons[channel])
  {
    lastDebounceTime[channel] = millis();
  }

  if ((millis() - lastDebounceTime[channel]) > debounceDelay)
  {
    if (buttonState != Buttons[channel])
    {
      Buttons[channel] = buttonState;

      if ((buttonState == LOW) && (channel > 2) && (channel < 15))
      { // Trigger Note On when signal pin input is low and only on 4067 channels 3-14
        NoteOn(channel);
        noteOffFlag[channel] = false; // Reset Note Off flag when Note On is triggered
      }
      else if ((channel > 2) && (channel < 15))
      {
        noteOffFlag[channel] = true; // Set Note Off flag when button is released from 4067 channels 3-14
      }
      else if ((buttonState == LOW) && (channel == 2))
      { // Pinky palm contact toggle for NoteSpread: toggle between 1, 2, 3 and 4 semitones between keys
        Spread = (Spread % 4) + 1;
        NoteSpread(RootNote, Spread, RootNoteOffset);
      }
      else if ((buttonState == LOW) && (channel == 15))
      { // Side of index finger contact latch for alternate control modes
        Scale = (Scale % 19) + 1;
        //        IndexLatch = 1;
      }
    }
  }

  // Check if it's time to send NoteOff
  if (noteOffFlag[channel] && millis() > (lastDebounceTime[channel] + noteOffDelay))
  {
    noteOffFlag[channel] = false; // Reset Note Off flag
    NoteOff(channel);
  }

  prevButtons[channel] = buttonState;
}

void OctaveTranspose(int Octave)
{ // Change octave transpose
}

void RootNoteTranspose()
{ // Change root note and transpose all notes accordingly
}

void NoteVelocity()
{ // Read max value from thumb FSR ADC during Note On for X ms to calculate Note On velocity
}

void Aftertouch()
{ // Read thumb FSR ADC and send scaled value to Aftertouch
}

void FlexSensor()
{ // Read thumb Flex Sensor ADC and send scale quantized note values
}

void setup()
{
  // put your setup code here, to run once:

  while (!Serial)
    ;
  // Call .begin() to configure the IMUs
  if (myIMU.begin() != 0)
  {
    Serial.println("Device error");
  }
  else
  {
    Serial.println("Device OK!");
  }

  myIMU.begin();
  uint8_t dataToWrite = 0; // Temporary variable

  // Setup the accelerometer******************************
  dataToWrite = 0; // Start Fresh!
  dataToWrite |= LSM6DS3_ACC_GYRO_BW_XL_100Hz;
  dataToWrite |= LSM6DS3_ACC_GYRO_FS_XL_2g;
  dataToWrite |= LSM6DS3_ACC_GYRO_ODR_XL_104Hz;

  // Now, write the patched together data
  errorsAndWarnings += myIMU.writeRegister(LSM6DS3_ACC_GYRO_CTRL1_XL, dataToWrite);

  // Set the ODR bit
  errorsAndWarnings += myIMU.readRegister(&dataToWrite, LSM6DS3_ACC_GYRO_CTRL4_C);
  dataToWrite &= ~((uint8_t)LSM6DS3_ACC_GYRO_BW_SCAL_ODR_ENABLED);

  Serial.begin(115200);
  while (!Serial)
    delay(10); // for nrf52840 with native usb

  Serial.println("Adafruit Bluefruit52 MIDI over Bluetooth LE Example");

  // Config the peripheral connection with maximum bandwidth
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);

  Bluefruit.begin();
  Bluefruit.setName("Bluefruit52 MIDI");
  Bluefruit.setTxPower(8);

  // Setup the on board blue LED to be enabled on CONNECT
  Bluefruit.autoConnLed(true);

  // Configure and Start Device Information Service
  bledis.setManufacturer("Adafruit Industries");
  bledis.setModel("Bluefruit Feather52");
  bledis.begin();

  // Initialize MIDI, and listen to all MIDI channels, will also call blemidi service's begin()
  MIDI.begin(MIDI_CHANNEL_OMNI);

  // Attach the handleNoteOn function to the MIDI Library. It will
  // be called whenever the Bluefruit receives MIDI Note On messages.
  MIDI.setHandleNoteOn(handleNoteOn);

  // Do the same for MIDI Note Off messages.
  MIDI.setHandleNoteOff(handleNoteOff);

  // Set up and start advertising
  startAdv();

  // Start MIDI read loop
  Scheduler.startLoop(midiRead);

  pinMode(inputPin, INPUT_PULLUP);  // Initialize 4067 signal pin return as input with 3.3V internal pullup
  pinMode(muxEnablePin, OUTPUT);    // Initialize 4067 Enable pin as Output
  digitalWrite(muxEnablePin, HIGH); // Turn on 4067 Enable pin permanently

  // Initiate note array starting with the root note and how many semitones until the next note, aka Spread
  NoteSpread(RootNote, Spread, RootNoteOffset);

  myHarmonizer.begin();
}

void loop()
{

  // Don't continue if we aren't connected.
  if (!Bluefruit.connected())
  {
    return;
  }
  // Don't continue if the connected device isn't ready to receive messages.
  if (!blemidi.notifyEnabled())
  {
    return;
  }

  // Start cycling through the 4067 channels to read the thumb contact touching the finger pads
  for (int channel = firstChannel; channel < numChannels; ++channel)
  {
    // Debounce the incoming input from the thumb connector that's connected to ground and pulling the input low
    debounceButton(channel);
  }

  // Check analog values
  int modVal = analogRead(modPot);
  int pitchVal = analogRead(pitchPot);
  pitchVal = map(pitchVal, 0, 1023, -8000, 8000);
  modVal = modVal / 8;

  // send new mod value if it has changed
  if (lastModVal != modVal)
  {
    //   Serial.print("modWheel = ");
    //   Serial.println(modVal);
    //   MIDI.sendControlChange(1, modVal, 1);
    lastModVal = modVal;
  }

  // send new pitch value if it has changed
  if (lastPitchVal != pitchVal)
  {
    //   Serial.print("pitchBend = ");
    //   Serial.println(pitchVal);
    //   MIDI.sendPitchBend(pitchVal, 1); //pot value sent as pitch bend
    lastPitchVal = pitchVal;
  }

  // Send accelerometer MIDI CC's every X ms
  if (millis() - lastExecutionTimeAccel >= intervalAccel)
  {
    accelRead(); // Send accelerometer MIDI CC's

    // Update the last execution time
    lastExecutionTimeAccel = millis();
  }
}
