#pragma once
#include <stdint.h>
struct Sample {
  float ax=0,ay=0,az=0,accel=0,tilt=0;
  uint32_t ms=0,seq=0;
  bool simulated=false;
  uint8_t phase=0;
};
enum CommandType { CMD_MODE, CMD_LED, CMD_BUZZER, CMD_RESET };
struct Command { CommandType type=CMD_RESET; bool on=false; };
struct DeviceState {
  bool autoMode=true,led=false,buzzer=false,alarm=false,checking=false,sensorOk=true;
  uint32_t events=0,simEvents=0,statusSeq=0;
};
