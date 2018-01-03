/*
 * Project GPS-Tracker - Simple Site Survey Tool
 * Description: For determining signal quality at propsective Electron sites
 * Author: Chip McClelland - See Insights LLC - seeinsights.com
 * Sponsor: Simple Sense - simplesense.io
 * Date: 12-12-17
 */

 // Variables I want to change often and pull them all together here
 #define SOFTWARERELEASENUMBER "0.55"

 #include "Particle.h"
 #include "TinyGPS++.h"                       // https://github.com/mikalhart/TinyGPSPlus
 #include "electrondoc.h"                     // Documents pinout

 // Prototypes and System Mode calls
 SYSTEM_THREAD(ENABLED);         // Means my code will not be held up by Particle processes.
 FuelGauge batteryMonitor;       // Prototype for the fuel gauge (included in Particle core library)
 TinyGPSPlus gps;                // The TinyGPS++ object

 // State Maching Variables
 enum State { INITIALIZATION_STATE, ERROR_STATE, IDLE_STATE, REPORTING_STATE, RESP_WAIT_STATE, RESP_RECEIVED_STATE };
 State state = INITIALIZATION_STATE;

 // Pin definitions
 const int enablePin = B4;                    // Hold low to power down the device
 const int fixPin = B3;                       // From the GPS modlue - tracks the status "red" LED
 const int ledPin = D7;                       // To give a visual indication when a datapoint is recorded
 const int donePin = D6;                      // Pin the Electron uses to "pet" the watchdog
 const int wakeUpPin = A7;                    // This is the Particle Electron WKP pin
 const int intPin = D3;                       // PIR Sensor interrupt pin



 // Reporting intervals
 const unsigned long Ubidots_Frequency = 60000;     // How often will we report to Ubidots
 const unsigned long webhookWaitTime = 10000;       // How long will we wait for a webhook response
 const unsigned long Particle_Frequency = 1000;     // Will limit how often we send updates

 // Program control valriables
 unsigned long ubidotsPublish = 0;               // When was the last time we published
 unsigned long lastPublish = 0;
 bool dataInFlight = false;                       // Tells us we send data but it has not yet been acknowledged by Ubidots
 volatile bool fixFlag = false;                   // Tracks fixLED interrupts
 unsigned long lastFixFlash = 0;                  // when was the last flash
 bool gpsFix = false;                             // keeps track of our fix Status
 bool sensorDetect = false;                       // So you can start to test the PIR sensor
 int resetCount = 0;                              // Can track resets for future reporting
 int errorCount = 0;                              // So we don't reset everytime a datapoint gets lost
 int RSSI = 0;                                    // Signal strength in dBi
 int gpsSamples = 0;                              // Counts the nubmer of samples received by Ubidots

 // Battery monitor
 int stateOfCharge = 0;                           // Stores battery charge level value

 //Menu and Program Variables
 const char* releaseNumber = SOFTWARERELEASENUMBER;   // Displays the release on the menu
 retained char Signal[17];                            // Used to communicate Wireless RSSI and Description
 char Status[17] = "";                                // Used to communciate updates on System Status
 const char* levels[6] = {"Poor", "Low", "Medium", "Good", "Very Good", "Great"};

 void setup()
 {
  Serial1.begin(9600);                 // The GPS module is connected to Serial 1
  pinMode(enablePin,OUTPUT);           // Can be used to turn the GPS on and off - not used in this version
  digitalWrite(enablePin,HIGH);        // Initially we will have the GPS on
  pinMode(ledPin,OUTPUT);              // So we can signal when a datapoint is received
  digitalWrite(ledPin,LOW);
  pinMode(fixPin,INPUT);               // Tied to the red indicator LED on the GPS Module
  pinMode(wakeUpPin,INPUT);            // This pin is active HIGH
  pinMode(donePin,OUTPUT);             // Allows us to pet the watchdog
  digitalWrite(donePin,HIGH);
  digitalWrite(donePin,LOW);           // Pet the watchdog
  pinMode(intPin,INPUT);               // PIR Sensor Interrupt pin

  char responseTopic[125];
  String deviceID = System.deviceID();                                // Multiple Electrons share the same hook - keeps things straight
  deviceID.toCharArray(responseTopic,125);
  Particle.subscribe(responseTopic, UbidotsHandler, MY_DEVICES);      // Subscribe to the integration response event

  Particle.variable("Signal", Signal);                    // You will be able to monitor the device using the Particle mobile app
  Particle.variable("ResetCount", resetCount);
  Particle.variable("Release",releaseNumber);
  Particle.variable("stateOfChg", stateOfCharge);
  Particle.variable("Samples",gpsSamples);

  Particle.function("Reset",resetNow);

  attachInterrupt(intPin,sensorISR,RISING);   // Will know when the PIR sensor is triggered
  attachInterrupt(wakeUpPin, watchdogISR, RISING);   // The watchdog timer will signal us and we have to respond
  attachInterrupt(fixPin,fixISR,RISING);                    // Going to see when we have a fix
  lastFixFlash = millis();

  Time.zone(-5);                                            // Set time zone to Eastern USA daylight saving time
  takeMeasurements();

  state = IDLE_STATE;                                     // IDLE unless error from above code
  waitUntil(meterParticlePublish);                       // Meter our Particle publishes
  Particle.publish("State","Idle");
  lastPublish = millis();
 }

 void loop()
 {
   switch(state) {

   case IDLE_STATE:
     if (fixFlag) {
       fixFlag = false;                                     // Clear the fixFlag
       if (millis()-lastFixFlash >= 10000)  gpsFix = true;  // Flashes every 15 sec when it has a fix
       else  gpsFix = false;                                // Flashes every second when it is looking
       lastFixFlash = millis();                 // Reset the flag timer
     }
     if ((millis() - ubidotsPublish >= Ubidots_Frequency) && gpsFix) {
       state = REPORTING_STATE;
       waitUntil(meterParticlePublish);     // Meter our Particle publishes
       Particle.publish("State","Reporting");
       lastPublish = millis();
     }
     if (sensorDetect) {
       sensorDetect = false;
       waitUntil(meterParticlePublish);
       Particle.publish("State","IDLE - PIR Event detected");
       lastPublish = millis();
     }
   break;

   case REPORTING_STATE:
     if (Serial1.available() > 0) {
       if (gps.encode(Serial1.read())) {
         bool nonZero = displayInfo();
         takeMeasurements();
         waitUntil(meterParticlePublish);     // Meter our Particle publishes
         if (nonZero) {
           sendEvent();
           state = RESP_WAIT_STATE;                            // Wait for Response
           Particle.publish("State","Waiting for Response");
         }
         else {
           state = IDLE_STATE;
           Particle.publish("State","IDLE - Invalid GPS");
         }
         lastPublish = millis();
       }
     }
   break;

   case RESP_WAIT_STATE:
     if (millis() - ubidotsPublish >= webhookWaitTime)
     {
       state = ERROR_STATE;                               // Response timed out
       waitUntil(meterParticlePublish);     // Meter our Particle publishes
       Particle.publish("State","Response Timeout Error");
       lastPublish = millis();
     }
   break;

   case RESP_RECEIVED_STATE:
     state = IDLE_STATE;
     waitUntil(meterParticlePublish);     // Meter our Particle publishes
     Particle.publish("State","Idle");
     lastPublish = millis();
     gpsSamples++;                                         // So you can see how many samples are recorded
     digitalWrite(ledPin,HIGH);                         // So you can see when a datapoint is received
     delay(2000);
     digitalWrite(ledPin,LOW);
     gpsFix = false;
     lastFixFlash = millis();
   break;

   case ERROR_STATE:                                      // To be enhanced - where we deal with errors
     if (errorCount >= 3) {
       waitUntil(meterParticlePublish);     // Meter our Particle publishes
       Particle.publish("State","Too many errors - resetting Electron");
       lastPublish = millis();
       delay(3000);                                         // Give time to publish
       System.reset();
     }
     errorCount++;                                      // Today, only way out is reset
   break;
   }
 }

bool displayInfo()
{
  char buf[128];
  if (gps.location.isValid()) snprintf(buf, sizeof(buf), "%f,%f,%f", gps.location.lat(), gps.location.lng(), gps.altitude.meters());
  else strcpy(buf, "no location");
  waitUntil(meterParticlePublish);     // Meter our Particle publishes
  Particle.publish("gps", buf);
  lastPublish = millis();
  if (int(gps.location.lat() + gps.location.lng())) return 1;
  else return 0;
}


void sendEvent()
{
  char data[256];                                         // Store the date in this character array - not global
  snprintf(data, sizeof(data), "{\"battery\":%i, \"signal\":%i, \"lat\":%f, \"lng\":%f}",stateOfCharge, RSSI, gps.location.lat(), gps.location.lng());
  waitUntil(meterParticlePublish);     // Meter our Particle publishes
  Particle.publish("GPSlog_hook", data, PRIVATE);
  lastPublish = millis();
  ubidotsPublish = millis();            // This is how we meter out publishing to Ubidots
  dataInFlight = true;                  // set the data inflight flag - cleared when we get the 201 response
}

void UbidotsHandler(const char *event, const char *data)  // Looks at the response from Ubidots - Will reset Photon if no successful response
{
  // Response Template: "{{hourly.0.status_code}}"
  if (!data) {                                            // First check to see if there is any data
    waitUntil(meterParticlePublish);     // Meter our Particle publishes
    Particle.publish("Webhook Response","Empty");
    lastPublish = millis();
    return;
  }
  int responseCode = atoi(data);                          // Response is only a single number thanks to Template
  if ((responseCode == 200) || (responseCode == 201))
  {
    waitUntil(meterParticlePublish);     // Meter our Particle publishes
    Particle.publish("State","Response Received");
    lastPublish = millis();
    state = RESP_RECEIVED_STATE;                                // Data has been received
  }
  else {
    waitUntil(meterParticlePublish);     // Meter our Particle publishes
    Particle.publish("Webhook Response",data);
    lastPublish = millis();
  }
}

void getSignalStrength()
{
    CellularSignal sig = Cellular.RSSI();  // Prototype for Cellular Signal Montoring
    RSSI = sig.rssi;
    int strength = map(RSSI, -131, -51, 0, 5);
    snprintf(Signal,17, "%s: %d", levels[strength], RSSI);
    waitUntil(meterParticlePublish);     // Meter our Particle publishes
    Particle.publish("Signal",Signal);
    lastPublish = millis();
}

void takeMeasurements() {
  if (Cellular.ready()) getSignalStrength();                // Test signal strength if the cellular modem is on and ready
  stateOfCharge = int(batteryMonitor.getSoC());             // Percentage of full charge
}

int resetNow(String command)   // Will reset the Electron
{
  if (command == "1")
  {
    System.reset();
    return 1;
  }
  else return 0;
}

void fixISR()
{
  fixFlag = true;
}

bool meterParticlePublish(void)
{
  if(millis() - lastPublish >= Particle_Frequency) return 1;
  else return 0;
}

void watchdogISR()
{
  digitalWrite(donePin, HIGH);                              // Pet the watchdog
  digitalWrite(donePin, LOW);
}

void sensorISR()
{
  sensorDetect = true;                                      // sets the sensor flag for the main loop
}
