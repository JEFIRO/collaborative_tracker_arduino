
#include <BluetoothSerial.h>
BluetoothSerial serialBT;

int rele_pin = 18;

void setup() {
 

  SerialBT.begin("Tracker_collaborative")

}

void loop() {
 if (serialBT.available())

}
