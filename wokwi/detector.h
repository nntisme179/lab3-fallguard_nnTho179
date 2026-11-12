#pragma once
#include <stdint.h>
#include <math.h>
// Logic doc lap phan cung, duoc chay CHI trong TaskCanhBao.
struct FallDetector {
  bool alarm=false,checking=false,first=true,source=false,holding=false;
  float oldA=1,oldTilt=0,beforeTilt=0;
  uint32_t checkAt=0,holdAt=0,alarmAt=0,sourceAt=0;
  void reset() { alarm=false; checking=false; first=true; holding=false; }
  bool update(float a,float tilt,uint32_t now,bool simulated) {
    if (first || simulated!=source) {
      alarm=false; checking=false; holding=false; first=false;
      source=simulated; sourceAt=now; oldA=a; oldTilt=tilt; return false;
    }
    if (alarm && now-alarmAt>=8000) alarm=false;
    if (!alarm && !checking && now-sourceAt>=800) {
      if ((a>2.2f && oldA<=2.2f) || (a<0.4f && oldA>=0.4f)) {
        checking=true; checkAt=now; beforeTilt=oldTilt; holding=false;
      }
    }
    bool fired=false;
    if (checking) {
      if (tilt>=55 && fabsf(tilt-beforeTilt)>=35 && a>=0.75f && a<=1.35f) {
        if (!holding) { holding=true; holdAt=now; }
        if (now-holdAt>=600) {
          alarm=true; alarmAt=now; checking=false; holding=false; fired=true;
        }
      } else holding=false;
      if (checking && now-checkAt>15000) { checking=false; holding=false; }
    }
    oldA=a; oldTilt=tilt;
    return fired;
  }
};
