Gestural MIDI BLE Glove

- Based on Seeed XIAO nRF52840 Sense: https://www.seeedstudio.com/Seeed-XIAO-BLE-Sense-nRF52840-p-5253.html (compatible with other nRF52840 boards) 
- X,Y,Z Accelerometers send MIDI CC
- Buttons triggered by conductive material on thumb touching conductive material on fingers
- 74HC4067 multiplexed buttons using the Lightweight CD74HC4067 Library: https://github.com/SunitRaut/Lightweight-CD74HC4067-Arduino
- 12 MIDI note buttons (3 per finger)
- MIDI notes can be quantized to scales using code adapted from the AC Sensorizer project for MIDIbox: http://www.midibox.org/dokuwiki/doku.php?id=acsensorizer_04
- 2 modal buttons: one under the pinky and one on the side of the index finger
- FSR on thumb tip for note-on velocity and aftertouch
- flex resistor on index finger for MIDI CC, etc.
- LED strip with 7x SK6812
- Powered by a LiPo battery (400mAh)

- Gesture recognition using TinyML / TensorFlow Lite: https://wiki.seeedstudio.com/XIAO-BLE-Sense-TFLite-Getting-Started/


INSTALLATION NOTES:
To get TinyML running on the XIAO Sense, Seeed says to follow these steps:

Step 1. Use this Google Colab script:

https://colab.research.google.com/github/arduino/ArduinoTensorFlowLiteTutorials/blob/master/GestureToEmoji/arduino_tinyml_workshop.ipynb

PROBLEM

-the current libraries (as of 01.01.2024) cause the error:
"ValueError: x has 714 columns but y has 2 columns"

SOLUTION
- specify these older library versions:

!pip install pandas numpy matplotlib==3.1.2
!pip install tensorflow==2.8.0rc0

Step 2. Download their latest fork of TensorFlow Lite:
https://github.com/lakshanthad/tflite-micro-arduino-examples 

PROBLEM

- The latest library doesn't work with the Arduino 2.2.1 

SOLUTION 
-Delete the following unecessary folder and its contents:
libraries\Arduino_TensorFlowLite\src\test_over_serial 


