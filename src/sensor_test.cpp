#include "sensor_test.h"
#include "chess_utils.h"
#include <Arduino.h>

SensorTest::SensorTest(BoardDriver* bd) : boardDriver(bd) {
}

void SensorTest::begin() {
  Serial.println("Place pieces on the board to see them light up!");
  boardDriver->clearAllLEDs();
}

void SensorTest::update() {
  boardDriver->readSensors();

  for (int row = 0; row < 8; row++)
    for (int col = 0; col < 8; col++)
      if (boardDriver->getSensorState(row, col))
        boardDriver->setSquareLED(row, col, 0, 0, 0, 255);
      else
        boardDriver->setSquareLED(row, col, 0, 0, 0, 0);

  boardDriver->showLEDs();
}