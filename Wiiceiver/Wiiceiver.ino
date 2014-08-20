/*
 * (CC BY-NC-SA 4.0) 
 * http://creativecommons.org/licenses/by-nc-sa/4.0/
 *
 * WARNING WARNING WARNING: attaching motors to a skateboard is 
 * a terribly dangerous thing to do.  This software is totally
 * for amusement and/or educational purposes.  Don't obtain or
 * make a wiiceiver (see below for instructions and parts), 
 * don't attach it to a skateboard, and CERTAINLY don't use it
 * to zip around with just a tiny, ergonomic nunchuck instead
 * of a bulky R/C controller.
 *
 * This software is made freely available.  If you wish to 
 * sell it, don't.  If you wish to modify it, DO! (and please
 * let me know).  Much of the code is derived from others out
 * there, I've made attributuions where appropriate.
 *
 * http://austindavid.com/wiiceiver
 *  
 * latest software & schematic: 
 *    https://github.com/jaustindavid/wiiceiver
 *
 * Enjoy!  Be safe! 
 * 
 * (CC BY-NC-SA 4.0) Austin David, austin@austindavid.com
 * 12 May 2014
 *
 */
 
#include <stdlib.h>
#include <avr/wdt.h> 
#include <Wire.h>
#include <Servo.h>
#include <EEPROM.h>


#define WIICEIVER_VERSION "1.2 experimental"

// addys for vars stored in EEPROM
#define EEPROM_Y_ADDY            0
#define EEPROM_AUTOCRUISE_ADDY   1
#define EEPROM_WDC_ADDY          2
#define EEPROM_LOGGER_ADDY       16

#define DEBUGGING

#include "Blinker.h"
#include "Chuck.h"
#include "ElectronicSpeedController.h"
#include "Smoother.h"
#include "Throttle.h"
#include "StaticQueue.h"
#include "EEPROMAnything.h"
#include "Logger.h"
#include "pinouts.h"


/********
 * PATTERNS!
 * http://www.codeproject.com/Articles/721796/Design-patterns-in-action-with-Arduino
 ********/


Chuck* chuck;
ElectronicSpeedController* ESC;
Blinker green, red;
Throttle* throttle;
Logger* logger;


#define USE_DISPLAY

#ifdef USE_DISPLAY
#include <SPI.h>
#include "AdafruitGFX.h"
#include "AdafruitDisplay.h"

#include "Utils.h"
#include "Display.h"

Adafruit_SSD1306 adafruitDisplay(pinLocation(OLED_RESET_ID));

Display* display;

#endif

/********
 *  WATCHDOG STUFF
 * I referenced the following:
 * http://forum.arduino.cc/index.php?topic=63651.0
 * http://tushev.org/articles/arduino/item/46-arduino-and-watchdog-timer
 * http://www.ivoidwarranties.com/2012/02/arduino-mega-optiboot.html
 *
 ********/
 
/*
 * setup a watchdog timer for the given reset interval
 *
 * constants: WDTO_250MS, WDTO_8S, WDTO_60MS, etc
 */
void watchdog_setup(byte wd_interval) {
  wdt_reset();
  wdt_enable(wd_interval);
  cli();
  WDTCSR |= (1<<WDCE) | (1<<WDE) | (1<<WDIE); 
  sei();
} // watchdog_setup(unsigned int wd_interval)


// increment a _W_atch _D_og _C_ounter stored in EEPROM, for future debugging
ISR(WDT_vect) {
  byte wdt_counter = EEPROM.read(EEPROM_WDC_ADDY);
  EEPROM.write(EEPROM_WDC_ADDY, wdt_counter + 1);
} // ISR for the watchdog timer



// display the current value of the watchdog counter
void display_WDC(void) {
  byte wdt_counter = EEPROM.read(EEPROM_WDC_ADDY);
  Serial.print(F("Watchdog Resets: "));
  Serial.println((byte)(wdt_counter + 1));
} // display_WDC()



// maybe calibrate the joystick:
//   read the C button 250 times, once per 20ms (5s total); if it's constantly
//   down, calibrate the joystick
void maybeCalibrate(void) {
  int ctr = 0;
  int i = 0;

  chuck->update();
  if (chuck->C != 1 || ! chuck->isActive()) {
    return;
  }

  #ifdef USE_DISPLAY
  display->printMessage("Calibrate: Hold C ...");
  #endif
  red.update(10);
  green.update(10);
  while (i < 250 && chuck->C) {
    chuck->update();
    red.run();
    green.run();
    i++;
    ctr += chuck->C;
    delay(20);
  }

  #ifdef DEBUGGING
  Serial.print(F("C = "));
  Serial.println(ctr);
  #endif

  if (chuck->C == 1 && chuck->isActive()) {
    chuck->calibrateCenter();
    chuck->writeEEPROM();
    // side effect: reset the WDC
    EEPROM.write(EEPROM_WDC_ADDY, 255);
    Serial.println(F("Calibrated"));
    #ifdef USE_DISPLAY
    display->printMessage("Calibrated");
    delay(1000);
    #endif
  }

  red.update(1);
  green.update(1);
} // void maybeCalibrate() 


// an unambiguous startup display
void splashScreen() {
  int i;
  digitalWrite(pinLocation(GREEN_LED_ID), HIGH);
  digitalWrite(pinLocation(RED_LED_ID), HIGH);
  delay(250);
  for (i = 0; i < 5; i++) {
    digitalWrite(pinLocation(GREEN_LED_ID), HIGH);
    digitalWrite(pinLocation(RED_LED_ID), LOW);
    delay(50);
    digitalWrite(pinLocation(GREEN_LED_ID), LOW);
    digitalWrite(pinLocation(RED_LED_ID), HIGH);
    delay(50);
  }
  digitalWrite(pinLocation(GREEN_LED_ID), HIGH);
  digitalWrite(pinLocation(RED_LED_ID), HIGH);
  delay(250);
  digitalWrite(pinLocation(GREEN_LED_ID), LOW);    
  digitalWrite(pinLocation(RED_LED_ID), LOW);  
} // void splashScreen(void)


// flash the LEDs to indicate throttle position
void updateLEDs() {
  if (throttle->getThrottle() == 0) {
    green.update(1);
    red.update(1);
  } else {
    int bps = abs(int(throttle->getThrottle() * 20));

    if (throttle->getThrottle() > 0) {
      green.update(bps);
      red.update(1);
    } else {
      green.update(1);
      red.update(bps);
    }
  }
} // updateLEDs(float throttle)


// the nunchuck appears to be static: we lost connection!
// go "dead" for up to 5s, but keep checking the chuck to see if
// it comes back
void freakOut(void) {
  unsigned long targetMS = millis() + 5000;
  bool redOn = false;
  byte blinkCtr = 0;

#ifdef DEBUGGING
    Serial.print(millis());
    Serial.println(": freaking out");
#endif

  #ifdef USE_DISPLAY
  display->printMessage("NO SIGNAL");
  #endif
  red.stop();
  green.stop();
  while (!chuck->isActive() && targetMS > millis()) {
//  while (targetMS > millis()) {
    if (blinkCtr >= 4) {
      blinkCtr = 0;
      if (redOn) {
        red.high();
        green.low();
        redOn = false;
      } 
      else {
        red.low();
        green.high();
        redOn = true;
      }
    }
    blinkCtr ++;
    chuck->update();
    delay(20);
    wdt_reset();
  }
  green.start(1);
  red.start(1);
} // void freakOut(void)



void setup_pins() {
  /*
  pinMode(ESC_GROUND, OUTPUT);
  digitalWrite(ESC_GROUND, LOW);
  pinMode(WII_GROUND, OUTPUT);
  digitalWrite(WII_GROUND, LOW);
  */
  pinMode(pinLocation(WII_POWER_ID), OUTPUT);
  digitalWrite(pinLocation(WII_POWER_ID), HIGH);
  
  pinMode(pinLocation(WII_SCL_ID), INPUT_PULLUP);
  pinMode(pinLocation(WII_SDA_ID), INPUT_PULLUP);

} // setup_pins()


// wait up to 1s for something to happen
bool waitForActivity(void) {
  unsigned long timer = millis() + 1000;
  #ifdef DEBUGGING
  Serial.print(millis());
  Serial.print(F(" Waiting for activity ... "));
  #endif
  
  chuck->update();
  while (! chuck->isActive() && timer > millis()) {
    wdt_reset();
    delay(20);
    chuck->update();
  }

  #ifdef DEBUGGING
  Serial.print(millis());
  Serial.println(chuck->isActive() ? F(": active!") : F(": not active :("));
  #endif
  
  return chuck->isActive();
} // bool waitForActivity()


// dead code?
void stopChuck() {
#ifdef DEBUGGING
    Serial.println("Nunchuck: power off");
#endif
  digitalWrite(pinLocation(WII_POWER_ID), LOW);
  delay(250);
#ifdef DEBUGGING
    Serial.println(F("Nunchuck: power on"));
#endif
  digitalWrite(pinLocation(WII_POWER_ID), HIGH);
  delay(250);
} // stopChuck()



// returns true if the chuck appears "active"
// will retry 5 times, waiting 1s each
bool startChuck() {
  int tries = 0;
  
  while (tries < 10) {
    #ifdef DEBUGGING
    Serial.print(F("(Re)starting the nunchuck: #"));
    Serial.println(tries);
    #endif
  
    wdt_reset();
    chuck->setup();
    chuck->readEEPROM();
    tries ++;
    if (waitForActivity()) {
      return true;
    }
  }
  return false;
} // startChuck()


// pretty much what it sounds like
void handleInactivity() {
  watchdog_setup(WDTO_8S);  // some long-cycle stuff happens here
  #ifdef DEBUGGING
  Serial.print(millis());
  Serial.println(": handling inactivity");
  #endif
  
  throttle->zero();
  ESC->setLevel(0);
  
  // this loop: try to restart 5 times in 5s; repeat until active
  do {    
    freakOut();
    if (! chuck->isActive()) {
      // stopChuck();
      // delay(250);
      startChuck();
    }
  } while (! chuck->isActive());
  
  // active -- now wait for zero
  #ifdef DEBUGGING
  Serial.print(millis());
  Serial.println("Waiting for 0");
  #endif  

  #ifdef USE_DISPLAY  
  display->printMessage("WAITING...");
  #endif
  
  while (abs(chuck->Y) > 0.1) {
    chuck->update();
    wdt_reset();
    delay(20);
  }
  
  #ifdef DEBUGGING
  Serial.print(millis());
  Serial.println(": finished inactivity -- chuck is active");
  #endif

  #ifdef USE_DISPLAY
  display->printMessage("Active!");
  #endif
  
  watchdog_setup(WDTO_250MS);
} // handleInactivity()




void setup() {
  wdt_disable();
  Serial.begin(115200);

  Serial.print(F("Wiiceiver v "));
  Serial.print(F(WIICEIVER_VERSION));
  Serial.print(F(" (compiled "));
  Serial.print(F(__DATE__));
  Serial.print(F(" "));
  Serial.print(F(__TIME__));
  Serial.println(F(")"));

  #ifdef USE_DISPLAY
    #ifdef DEBUGGING
    Serial.println(F("Loading Display..."));
    #endif
  display = Display::getInstance();
  display->init(&adafruitDisplay);
  #endif  
  
  display_WDC();

  green.init(pinLocation(GREEN_LED_ID));
  red.init(pinLocation(RED_LED_ID));

  setup_pins();
  
    #ifdef DEBUGGING
    Serial.println(F("Starting ESC..."));
    #endif
  ESC = ElectronicSpeedController::getInstance();
  ESC->init(pinLocation(ESC_PPM_ID));

    #ifdef DEBUGGING
    Serial.println(F("Starting Logger..."));
    #endif  
  logger = Logger::getInstance();
  logger->init(pinLocation(AMMETER_ID));
  
  splashScreen();

  #ifdef DEBUGGING
  Serial.println("Starting the nunchuck ...");
  #endif
  chuck = Chuck::getInstance();
  green.high();
  red.high();
  if (startChuck()) {
    maybeCalibrate();
  } else {
    handleInactivity();
  }
  #ifdef DEBUGGING
  Serial.println("Nunchuck is active!");
  #endif
  
  throttle = Throttle::getInstance();
  throttle->init();

  green.start(10);
  red.start(10);
  
  green.update(1);
  red.update(1);
  watchdog_setup(WDTO_250MS);
} // void setup()



void loop() {
  static float lastThrottleValue = 0;
  unsigned long startMS = millis();
  wdt_reset();
  green.run();
  red.run();
  chuck->update();
  logger->update();
  #ifdef USE_DISPLAY
  display->update();
  #endif
  
  // for forcing a watchdog timeout
  #undef SUICIDAL_Z
  #ifdef SUICIDAL_Z
  if (chuck->Z) {
    Serial.println("sleepin' to reset");
    delay(9000);
  } // suicide!
  #endif 
  
  if (!chuck->isActive()) {
    #ifdef DEBUGGING
    Serial.println("INACTIVE!!");
    #endif
    handleInactivity();
  } else {
    float throttleValue = throttle->update();
    ESC->setLevel(throttleValue);
    if (throttleValue != lastThrottleValue) {
      updateLEDs();
      #ifdef DEBUGGING
      Serial.print(F("y="));
      Serial.print(chuck->Y, 4);
      Serial.print(F(", "));
      Serial.print(F("c="));
      Serial.print(chuck->C);      
      Serial.print(F(", z="));
      Serial.print(chuck->Z);
      Serial.print(F(", "));
      Serial.println(throttleValue, 4); 
      #endif
      lastThrottleValue = throttleValue;
    }
    int delayMS = constrain(startMS + 21 - millis(), 5, 20);
    #ifdef DEBUGGING_INTERVALS
    Serial.print(F("sleeping ")); 
    Serial.println(delayMS);
#   endif
    delay(delayMS);
  } // if (chuck->isActive())
}





