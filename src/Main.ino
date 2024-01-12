/*****************************************************************************/
// Sneaky-Gestures V2 - MIDI BLE glove
/*******************************************************************************/

#include "Wire.h"
#include "Adafruit_TinyUSB.h"  // USB Library
#include <light_CD74HC4067.h>  // 74HC4067 Library
#include <bluefruit.h>         // Adafruit Bluefruit BLE Library
#include <MIDI.h>              // MIDI Library
#include "LSM6DS3.h"           // IMU Library
#include "ACHarmonizer.h"

#define NEOPIXEL_ENABLED 1    //  Enable/disable TensorFlow Lite gesture recognition
#define GESTURE_RECOGNITION_ENABLED 1   //  Enable/disable TensorFlow Lite gesture recognition


#ifdef NEOPIXEL_ENABLED
#include <Adafruit_NeoPixel.h> // Neopixel Library

// Which pin on the Arduino is connected to the NeoPixels?
// On a Trinket or Gemma we suggest changing this to 1:
#define LED_PIN 5

// How many NeoPixels are attached to the Arduino?
#define LED_COUNT 7

// NeoPixel brightness, 0 (min) to 255 (max)
#define BRIGHTNESS 50 // Set BRIGHTNESS to about 1/5 (max = 255)

// Declare our NeoPixel strip object:
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRBW + NEO_KHZ800);

#endif // NEOPIXEL_ENABLED for NEOPIXEL Animations


// Setup IMU Double Tap Interrupt
#define int1Pin PIN_LSM6DS3TR_C_INT1
bool DoubleTapState; // the current state of button
bool gestureRecognize; // Ring finger modal button to enable gesture recognition state if 


#ifdef GESTURE_RECOGNITION_ENABLED
float detect = 0; // convert TFL comparison reading to variable for triggering gesture action

// TensorFlow Lite libraries
#define TF_LITE_STRIP_ERROR_STRINGS 0 // disable error reporting 
#include <TensorFlowLite.h>
#include <tensorflow/lite/micro/all_ops_resolver.h>
#include <tensorflow/lite/micro/micro_error_reporter.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/schema/schema_generated.h>

// User-generated gesture library
#include "model.h"

// The above was generated with
// https://colab.research.google.com/github/arduino/ArduinoTensorFlowLiteTutorials/blob/master/GestureToEmoji/arduino_tinyml_workshop.ipynb
//
// Please note the above Google Colab script has the following issues!
//
// PROBLEM: the current libraries (as of 01.01.2024) cause the error: "ValueError: x has 714 columns but y has 2 columns"
// SOLUTION: specify these older library versions instead
// !pip install pandas numpy matplotlib==3.1.2
// !pip install tensorflow==2.8.0rc0
//

// TinyML variables
const float accelerationThreshold = 4; // threshold of significant in G's
const int numSamples = 119;
int samplesRead = numSamples;

// global variables used for TensorFlow Lite (Micro)
tflite::MicroErrorReporter tflErrorReporter;

// pull in all the TFLM ops, you can remove this line and
// only pull in the TFLM ops you need, if would like to reduce
// the compiled size of the sketch.
tflite::AllOpsResolver tflOpsResolver;

const tflite::Model *tflModel = nullptr;
tflite::MicroInterpreter *tflInterpreter = nullptr;
TfLiteTensor *tflInputTensor = nullptr;
TfLiteTensor *tflOutputTensor = nullptr;

// Create a static memory buffer for TFLM, the size may need to
// be adjusted based on the model you are using
constexpr int tensorArenaSize = 8 * 1024;
byte tensorArena[tensorArenaSize] __attribute__((aligned(16)));

// array to map gesture index to a name
const char* GESTURES[] = {
  "oct-up",
  "oct-down",
  "fist-out",
  "fist-down"
};

#define NUM_GESTURES (sizeof(GESTURES) / sizeof(GESTURES[0]))

#endif  // GESTURE_RECOGNITION_ENABLED    

// Initiate BLE MIDI
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
// - unsigned int AccelZ;                   // Z axis not needed for MIDI CC
unsigned int lastAccelX; // Previous IMU reading
unsigned int lastAccelY;
// - unsigned int lastAccelZ;               // Z axis not needed for MIDI CC
unsigned long lastExecutionTimeAccel = 0; // Timer for sending IMU MIDI CC's
const unsigned long intervalAccel = 20;   // Send IMU MIDI CC's every X ms
unsigned int CCAccelX = 74;               // Set IMU X acis to MIDI CC 74
unsigned int CCAccelY = 71;               // Set IMU X acis to MIDI CC 71

// Temporary variables for processing analog inputs
int modPot = 0;   // analog pin A0
int pitchPot = 1; // analog pin A1
int lastModVal;
int lastPitchVal;

// Set variables for reading 4067 inputs and triggering MIDI notes
CD74HC4067 mux(9, 8, 7, 6); // 4067 Pins for S0, S1, S2 and S3
const int inputPin = 3;
const int muxEnablePin = 7;
const int firstChannel = 1;                  // First 4067 Channel to demux / scan from
const int numChannels = 16;                  // Last 4067 Channel to scan from (assuming all inputs are connected sequentially, eg. 1-15)
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

// Read 4067 inputs
int digitalReadFromMultiplexer(int channel)
{ // Read each 4067 channel input
  mux.channel(channel);
  return digitalRead(inputPin);
}

// Send MIDI note on command
void NoteOn(int channel)
{ // Note On routine
  Serial.print("NoteOn - Channel ");
  Serial.println(channel);
  harmonizedNote = Keys[(channel - 3)];
  MIDI.sendNoteOn(myHarmonizer.harmonize(harmonizedNote, Scale), 100, MIDIchannel); // Send a MIDI note on from the Keys array based on the triggered 4067 channel (Notes only on 4067 channels 3-14)
  Serial.println(myHarmonizer.harmonize(harmonizedNote, Scale));
  Serial.println(Scale);
}

// Send MIDI note off command
void NoteOff(int channel)
{ // Note Off routine
  Serial.print("NoteOff - Channel ");
  Serial.println(channel);
  harmonizedNote = Keys[(channel - 3)];
  MIDI.sendNoteOff(myHarmonizer.harmonize(harmonizedNote, Scale), 100, MIDIchannel); // Send a MIDI note on from the Keys array based on the triggered 4067 channel (Notes only on 4067 channels 3-14)
}

// Process incoming MIDI messages
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

// Initialize BLE 
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

// Process incoming MIDI note on messages
void handleNoteOn(byte channel, byte pitch, byte velocity)
{

  // Log when a note is pressed.
  Serial.printf("Note on: channel = %d, pitch = %d, velocity - %d", channel, pitch, velocity);
  Serial.println();
}

// Process incoming MIDI note off messages
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

// Note Spread: change the semitone spacing between the notes 
// Examples of different spreads for note handling arrays for the 12 finger contact notes:
// {60,61,62,63,64,65,66,67,68,69,70,71}; // +1 Chromatic note
// {60,62,64,66,68,70,72,74,76,78,80,82}; // +2 semitone note
// {60,63,66,69,72,75,78,81,84,87,90,93}; // +3 semitone note
// {60,64,68,72,76,80,84,88,92,96,100,104}; // +4 semitone note

void NoteSpread(int RootNote, int Spread, int RootNoteOffset)
{ // Change number of semitones between the keys, eg. +1, +2, +3 & +4
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

// Debounce 4067 inputs and send MIDI notes or trigger index finger & pinky finger modal buttons accordingly
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
      else if ((buttonState == LOW) && (channel == 1))
      { // Top of ring finger contact latch for TinyML gesture recognition mode
      Serial.println("Recognizing gestures - ON");
      gestureRecognize = true;

      }
      else if ((buttonState == HIGH) && (channel == 1))
      { // Top of ring finger contact latch for TinyML gesture recognition mode
      Serial.println("Recognizing gestures - OFF");
      gestureRecognize = false;
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

// Transpose root note +/- octaves
void OctaveTranspose(int Octave)
{ // Change octave transpose
}

// Transpose root note +/- semitones
void RootNoteTranspose()
{ // Change root note and transpose all notes accordingly
}

// Thumb FSR ADC MIDI Velocity
void NoteVelocity()
{ // Read max value from thumb FSR ADC during Note On for X ms to calculate Note On velocity
}

// Thumb FSR ADC MIDI controls
void Aftertouch()
{ // Read thumb FSR ADC and send scaled value to Aftertouch
}

// Index finger Flex Sensor ADC MIDI controls
void FlexSensor()
{ // Read thumb Flex Sensor ADC and send scale quantized note values
}


// IMU Double Tap Interrupt Setup
void setupDoubleTapInterrupt() {
  uint8_t error = 0;
  uint8_t dataToWrite = 0;

  // Double Tap Config
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_CTRL1_XL, 0x60);
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_TAP_CFG1, 0x8E);// INTERRUPTS_ENABLE, SLOPE_FDS
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_TAP_THS_6D, 0x8C);
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_INT_DUR2, 0x7F);
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_WAKE_UP_THS, 0x80);
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_MD1_CFG, 0x08);
}


// Onboard XIAO Sense RGB LED control 
void setLED_REDGB(bool red, bool green, bool blue) {
  if (!blue) { digitalWrite(LED_BLUE, HIGH); } else { digitalWrite(LED_BLUE, LOW); }
  if (!green) { digitalWrite(LED_GREEN, HIGH); } else { digitalWrite(LED_GREEN, LOW); }
  if (!red) { digitalWrite(LED_RED, HIGH); } else { digitalWrite(LED_RED, LOW); }
}

// IMU Double Tap Interrupt handler
void int1ISR()
{
  DoubleTapState = !DoubleTapState;
  setLED_REDGB(false, DoubleTapState, false); // set green only
  Serial.println("Double tap");
}


#ifdef GESTURE_RECOGNITION_ENABLED

// Recognize gestures from IMU data using TensorFlow Lite TinyML model
void gestureRead() {
    float aX, aY, aZ;

  // wait for significant motion
  while (samplesRead == numSamples) {
      // read the acceleration data
      aX = myIMU.readFloatAccelX();
      aY = myIMU.readFloatAccelY();
      aZ = myIMU.readFloatAccelZ();

      // sum up the absolutes
      float aSum = fabs(aX) + fabs(aY) + fabs(aZ);

      // check if it's above the threshold
      if (aSum >= accelerationThreshold) {
        // reset the sample read count
        samplesRead = 0;
        break;
      }
  }

  // check if the all the required samples have been read since
  // the last time the significant motion was detected
  while (samplesRead < numSamples) {
    // check if new acceleration AND gyroscope data is available
      // read the acceleration and gyroscope data
      aX = myIMU.readFloatAccelX();
      aY = myIMU.readFloatAccelY();
      aZ = myIMU.readFloatAccelZ();


      // normalize the IMU data between 0 to 1 and store in the model's
      // input tensor
      tflInputTensor->data.f[samplesRead * 3 + 0] = (aX + 4.0) / 8.0;
      tflInputTensor->data.f[samplesRead * 3 + 1] = (aY + 4.0) / 8.0;
      tflInputTensor->data.f[samplesRead * 3 + 2] = (aZ + 4.0) / 8.0;

      samplesRead++;

      if (samplesRead == numSamples) {
        // Run inferencing
        TfLiteStatus invokeStatus = tflInterpreter->Invoke();
        if (invokeStatus != kTfLiteOk) {
          Serial.println("Invoke failed!");
          while (1);
          return;
        }

        // Loop through the output tensor values from the model
        for (int i = 0; i < NUM_GESTURES; i++) {
          Serial.print(GESTURES[i]);
          Serial.print(": ");
    
          detect = (tflOutputTensor->data.f[i]);
          Serial.println(detect); 
   //                 Serial.println(tflOutputTensor->data.f[i], 6);      
          if ((i == 0) && ((detect) > 0.5)){
          pulseWhite(5);
          } else if ((i == 1) && ((detect) > 0.5)) {
            pulseRed(5);
          } else if ((i == 2) && ((detect) > 0.5)) {
            pulseGreen(5);
          } else if ((i == 3) && ((detect) > 0.5)) {
            pulseBlue(5);
          } 


        }
        Serial.println();
      }
  }
}

#endif  // GESTURE_RECOGNITION_ENABLED   

#ifdef NEOPIXEL_ENABLED

void pulseWhite(uint8_t wait)
  {
    strip.fill(strip.Color(0, 0, 0, 255));
        strip.show();
    delay(wait);
    strip.fill(strip.Color(0, 0, 0, 0));
        strip.show();
}



void pulseRed(uint8_t wait)
  {
    strip.fill(strip.Color(255, 0, 0, 0));
        strip.show();
    delay(wait);
    strip.fill(strip.Color(0, 0, 0, 0));
        strip.show();
}

void pulseGreen(uint8_t wait)
  {
    strip.fill(strip.Color(0, 255, 0, 0));
        strip.show();
    delay(wait);
    strip.fill(strip.Color(0, 0, 0, 0));
        strip.show();
}

void pulseBlue(uint8_t wait)
  {
    strip.fill(strip.Color(0, 0, 255, 0));
        strip.show();
    delay(wait);
    strip.fill(strip.Color(0, 0, 0, 0));
        strip.show();
}


#endif // NEOPIXEL_ENABLED for NEOPIXEL Animations

void setup()
{
  // put your setup code here, to run once:

//  while (!Serial)
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
//  while (!Serial)
//    delay(10); // for nrf52840 with native usb

  Serial.println("Adafruit Bluefruit52 MIDI over Bluetooth LE Example");

  // Config the peripheral connection with maximum bandwidth
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);

  Bluefruit.begin();
  Bluefruit.setName("Bluefruit52 MIDI");
  Bluefruit.setTxPower(8);

  // Setup the on board blue LED to be enabled on CONNECT
  Bluefruit.autoConnLed(true);

  // Configure and Start BLE Device Information Service
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

  // Initialize 4067 Multiplexer
  pinMode(inputPin, INPUT_PULLUP);  // Initialize 4067 signal pin return as input with 3.3V internal pullup
  pinMode(muxEnablePin, OUTPUT);    // Initialize 4067 Enable pin as Output
  digitalWrite(muxEnablePin, HIGH); // Turn on 4067 Enable pin permanently

  // Initiate note array starting with the root note and how many semitones until the next note, aka Spread
  NoteSpread(RootNote, Spread, RootNoteOffset);

  // Initiated MIDI scale note quantizer
  myHarmonizer.begin();

  // Setup IMU Double Tap Interrupt
  setupDoubleTapInterrupt();
  pinMode(int1Pin, INPUT);
  attachInterrupt(digitalPinToInterrupt(int1Pin), int1ISR, RISING);

  #ifdef GESTURE_RECOGNITION_ENABLED

  // get the TensorFlowLite representation of the model byte array
  tflModel = tflite::GetModel(model);
  if (tflModel->version() != TFLITE_SCHEMA_VERSION)
  {
    Serial.println("Model schema mismatch!");
    while (1)
      ;
  }

  // Create an interpreter to run the model
  tflInterpreter = new tflite::MicroInterpreter(tflModel, tflOpsResolver, tensorArena, tensorArenaSize, &tflErrorReporter);

  // Allocate memory for the model's input and output tensors
  tflInterpreter->AllocateTensors();

  // Get pointers for the model's input and output tensors
  tflInputTensor = tflInterpreter->input(0);
  tflOutputTensor = tflInterpreter->output(0);

  #endif  // GESTURE_RECOGNITION_ENABLED   

#ifdef NEOPIXEL_ENABLED // NEOPIXEL animations

  // Initiatlize Neopixels
    // Initiatlize Neopixels
  strip.begin(); // INITIALIZE NeoPixel strip object (REQUIRED)
  strip.show();  // Turn OFF all pixels ASAP
  strip.setBrightness(BRIGHTNESS);
  pulseWhite(5); // Init White Flash for LED strip 
#endif // NEOPIXEL_ENABLED for NEOPIXEL Animations

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

#ifdef GESTURE_RECOGNITION_ENABLED
  // If the ring finger contact if pressed, begin gesture recognition
  if (gestureRecognize) {
    gestureRead();     
  }

#endif  // GESTURE_RECOGNITION_ENABLED   
  
#ifdef NEOPIXEL_ENABLED // NEOPIXEL animations


#endif // NEOPIXEL_ENABLED for NEOPIXEL Animations

}
