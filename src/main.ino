#include "Adafruit_GPS.h"
#include "Adafruit_LIS3DH.h"
#include "GPS_Math.h"
#include "math.h"
#include "ctype.h"

#define NAME "/sensor/toddbike/"
#define mySerial Serial1
#define CLICKTHRESHHOLD 100
#define LED D7
#define LOWBATCUTOFFP 15.0 // % of battery below which to sleep loop until charged
#define LOWBATCUTOFFV 3.70 // battery voltage below which to sleep loop until charged
// battery must be below both % and voltage to be considered "low"
// since battery gauge doesn't always do a good job updating %'s

// publish GPS once per minute
#define PUBLISH_INTERVAL (60 * 1000)

// if no motion for two publish cycles, sleep! (publish battery state)
#define NO_MOTION_IDLE_SLEEP_DELAY (2 * PUBLISH_INTERVAL + 20 * 1000)

// wake up to check in every 8 hours even w/o motion
#define SLEEP_DURATION_SECONDS (8 * 60 * 60)

// how long it should sleep while waiting for more charge
#define LOWBAT_SLEEP_DURATION_SECONDS (60 * 60)

Adafruit_GPS GPS(&mySerial);
Adafruit_LIS3DH accel = Adafruit_LIS3DH(A2, A5, A4, A3);
FuelGauge fuel;
SYSTEM_MODE(SEMI_AUTOMATIC); // manual control of cell connection
STARTUP(System.enableFeature(FEATURE_RETAINED_MEMORY));

bool justWoke = true;
int lastSecond = 0;
bool ledState = false;
unsigned long lastMotionTime = 0;
unsigned long lastPublishTime = 0;
float latitude = 0.0;
float longitude = 0.0;
float lastLat = 0.0;
float lastLong = 0.0;


/* ===== SETUP ===== */

void setup() {
  initGPS();
  initAccel();
  pinMode(LED, OUTPUT);
  digitalWrite(LED, LOW);
  Serial.begin(9600);
}

void initGPS() {
  pinMode(D6, OUTPUT);
  digitalWrite(D6, LOW);
  GPS.begin(9600);
  mySerial.begin(9600);
  //# request a HOT RESTART, in case we were in standby mode before.
  GPS.sendCommand("$PMTK101*32");
  delay(250);
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_ALLDATA); // request everything!
  delay(250);
  GPS.sendCommand(PGCMD_NOANTENNA); // turn off antenna updates
  delay(250);
}

void initAccel() {
  accel.begin(LIS3DH_DEFAULT_ADDRESS);
  accel.setDataRate(LIS3DH_DATARATE_LOWPOWER_5KHZ); // 5kHz low-power sampling
  accel.setRange(LIS3DH_RANGE_2_G); // 2 gravities range - pretty sensitive

  // listen for single-tap events at the threshold
  // keep the pin high for 1s, wait 1s between clicks
  //uint8_t c, uint8_t clickthresh, uint8_t timelimit, uint8_t timelatency, uint8_t timewindow
  accel.setClick(1, CLICKTHRESHHOLD);//, 0, 100, 50);
  delay(250);
}


/* ====== LOOPING ===== */

void loop() {
  if (justWoke) {
    justWoke = false;
    // if not enough power to init connection, sleep (and hopefully charge)
    if (fuel.getSoC() < LOWBATCUTOFFP && fuel.getVCell() < LOWBATCUTOFFV) {
      turnOff(LOWBAT_SLEEP_DURATION_SECONDS);
    }
  }

  // keeping track of time
  unsigned long now = millis();
  if (lastMotionTime > now) { lastMotionTime = now; }
  if (lastPublishTime > now) { lastPublishTime = now; }

  // if motion / woken by motion, flash LED and update motion timestamp
  bool hasMotion = digitalRead(WKP);
  digitalWrite(LED, (hasMotion) ? HIGH : LOW); // flash on motion
  if (hasMotion) {
    lastMotionTime = now;
  }

  // while awake, maintain a connection (won't be awake for long)
  // use Particle connection as a proxy for general on / off state
  if (Particle.connected() == false) {
    Serial.println("connecting");
    turnOn();
    Serial.println("connected");
  }

  // while awake, publish regularly
  checkGPS();
  if ((now - lastPublishTime) > PUBLISH_INTERVAL) {
    lastPublishTime = now;
    publishGPS();
  }

  // if nothing's happened for a while, publish battery state and go to sleep
  if ((now - lastMotionTime) > NO_MOTION_IDLE_SLEEP_DELAY) {
    turnOff(SLEEP_DURATION_SECONDS);
  }
  delay(10);
}

void turnOn() { // draws ~180mA while on & connect to GPS + Cell
  fuel.wakeup(); // battery monitoring
  digitalWrite(D6, LOW); // GPS
  Particle.connect(); // cell + cloud
}

void turnOff(int sleepSeconds) { // draws ~130uA in sleep
  String batt = String::format("%.2fv,%.1f%%", fuel.getVCell(), fuel.getSoC());
  Serial.println(batt);
  if (Particle.connected() == true) {
    Particle.publish(NAME + String("b"), batt, 16777215, PRIVATE);
  }
  delay(15*1000);
  lastPublishTime = 0;
  lastMotionTime = 0;
  latitude = 0.0;
  longitude = 0.0;
  justWoke = true;
  digitalWrite(D6, HIGH); // GPS
  Particle.disconnect(); // cloud
  Cellular.off(); // cell
  fuel.sleep(); // battery monitoring (~200uA)
  delay(10*1000); // settle down before sleep
  System.sleep(SLEEP_MODE_DEEP, sleepSeconds);
}

// process and dump everything from the module through the library.
void checkGPS() {
  while (mySerial.available()) {
    char c = GPS.read();
    if (GPS.newNMEAreceived()) {
      GPS.parse(GPS.lastNMEA());
      float lat = convertDegMinToDecDeg(GPS.latitude);
      // flip longitude to be correct
      float lon = -1 * convertDegMinToDecDeg(GPS.longitude);
      if (round(lat) != 0 && round(lon) != 0) {
        latitude = lat;
        longitude = lon;
      }
    }
  }
}

// publish location to Particle cloud as an event
void publishGPS() {
  String latLong = String::format("%f,%f", latitude, longitude);
  Serial.println(latLong);
  Particle.publish(NAME + String("g"), latLong, 16777215, PRIVATE);
  lastLat = latitude;
  lastLong = longitude;

  /* example of all the things we could log
    unsigned int msSincelastMotionTime = (millis() - lastMotionTime);
    bool motionInTheLastMinute = (msSincelastMotionTime < 60000);

    String gps_line =
          "{\"lat\":"    + String(convertDegMinToDecDeg(GPS.latitude))
        + ",\"lon\":-"   + String(convertDegMinToDecDeg(GPS.longitude))
        + ",\"a\":"     + String(GPS.altitude)
        + ",\"q\":"     + String(GPS.fixquality)
        + ",\"spd\":"   + String(GPS.speed)
        + ",\"mot\":"   + String(motionInTheLastMinute)
        + ",\"s\": "  + String(GPS.satellites)
        + ",\"vcc\":"   + String(fuel.getVCell())
        + ",\"soc\":"   + String(fuel.getSoC())
        + "}";
  */
}
