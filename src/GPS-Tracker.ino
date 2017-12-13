/*
 * Project GPS-Tracker
 * Description:
 * Author:
 * Date:
 */
 // Port of TinyGPS for the Particle AssetTracker
 // https://github.com/mikalhart/TinyGPSPlus

 // Variables I want to change often and pull them all together here
 #define SOFTWARERELEASENUMBER "0.75"
 #define TESTMODE 1


 #include "Particle.h"
 #include "TinyGPS++.h"
 #include "electrondoc.h"                                 // Documents pinout

 // Prototypes and System Mode calls
 SYSTEM_THREAD(ENABLED);         // Means my code will not be held up by Particle processes.
 FuelGauge batteryMonitor;       // Prototype for the fuel gauge (included in Particle core library)
 TinyGPSPlus gps;                // The TinyGPS++ object

 // State Maching Variables
 enum State { INITIALIZATION_STATE, ERROR_STATE, IDLE_STATE, REPORTING_STATE, RESP_WAIT_STATE };
 State state = INITIALIZATION_STATE;

 // A sample NMEA stream.
 const char *gpsStream = "$GPRMC,045103.000,A,3014.1984,N,09749.2872,W,0.67,161.46,030913,,,A*7C\r\n";
 // Pin definitions
 const int enablePin = B4;      // Hold low to power down the device
 const int fixPin = B3;
 const int ledPin = D7;

 // Reporting intervals
 const unsigned long PUBLISH_PERIOD = 30000;     // How often will we report to Ubidots
 const unsigned long SERIAL_PERIOD = 5000;        // How often will we read the serial data
 const unsigned long webhookWaitTime = 20000;     // How long will we wait for a webhook response

 // Program control valriables
 unsigned long lastSerial = 0;
 unsigned long lastPublish = 0;
 unsigned long webhookTimeStamp = 0;
 unsigned long startFix = 0;
 bool gettingFix = false;
 bool dataInFlight = false;
 volatile bool fixFlag = false;             // Tracks fix interrupts
 unsigned long lastFixFlash = 0;            // when was the last flash
 bool gpsFix = false;                       // keeps track of our fix Status
 int resetCount = 0;
 int RSSI = 0;

 // Battery monitor
 int stateOfCharge = 0;                      // stores battery charge level value

 //Menu and Program Variables
 const char* releaseNumber = SOFTWARERELEASENUMBER;  // Displays the release on the menu
 retained char Signal[17];             // Used to communicate Wireless RSSI and Description
 char Status[17] = "";                 // Used to communciate updates on System Status
 const char* levels[6] = {"Poor", "Low", "Medium", "Good", "Very Good", "Great"};


 void setup()
 {
 	Serial.begin(9600);                  // For debugging - removed for production
  delay(3000);
  Serial1.begin(9600);                 // The GPS module is connected to Serial 1
  pinMode(enablePin,OUTPUT);           // Can be used to turn the GPS on and off
  digitalWrite(enablePin,HIGH);        // Initially we will have the GPS on
  pinMode(ledPin,OUTPUT);
  digitalWrite(ledPin,LOW);
  pinMode(fixPin,INPUT);

  char responseTopic[125];
  String deviceID = System.deviceID();                                // Multiple Electrons share the same hook - keeps things straight
  deviceID.toCharArray(responseTopic,125);
  Particle.subscribe(responseTopic, UbidotsHandler, MY_DEVICES);      // Subscribe to the integration response event

  Particle.variable("Signal", Signal);
  Particle.variable("ResetCount", resetCount);
  Particle.variable("Release",releaseNumber);
  Particle.variable("stateOfChg", stateOfCharge);

  Particle.function("Reset",resetNow);

  attachInterrupt(fixPin,fixISR,RISING);                              // Going to see when we have a fix
  lastFixFlash = millis();
  startFix = millis();
  gettingFix = true;

  Time.zone(-5);                                                        // Set time zone to Eastern USA daylight saving time
  takeMeasurements();

  if (state != ERROR_STATE) {
    state = IDLE_STATE;                         // IDLE unless error from above code
    Serial.println("State - Idle");
  }

  if (TESTMODE) {
    gps.encode(*gpsStream);
    gpsFix = true;
  }

 }

 void loop()
 {
   switch(state) {
   case IDLE_STATE: {

     if (fixFlag) {
       int difference = millis()-lastFixFlash;
       fixFlag = false;  // Clear the fixFlag
       if (difference >= 10000) {
         gpsFix = true;           // Flashes every 15 sec when it has a fix
         lastFixFlash = millis();
         Serial.print("GPS Fix, difference: ");
         Serial.println(difference);
       }
       else {
         gpsFix = false;                                             // Flashes every second when it is looking
         lastFixFlash = millis();
         Serial.print("No fix, difference: ");
         Serial.println(difference);
       }
     }
     if ((millis() - lastPublish >= PUBLISH_PERIOD) && gpsFix) {
       state = REPORTING_STATE;
       Serial.println("State - Reporting");
     }
   } break;

   case REPORTING_STATE:
     while (Serial1.available() > 0) {
       if (gps.encode(Serial1.read())) {
         Serial.println("Serial data received");
         displayInfo();
         takeMeasurements();
         sendEvent();
         state = RESP_WAIT_STATE;                            // Wait for Response
         Serial.println("State - Waiting for Response");
       }
     }
     break;

   case RESP_WAIT_STATE:

     if (!dataInFlight)                                  // Response received
     {
       state = IDLE_STATE;
       Serial.println("State - Idle");
       digitalWrite(ledPin,HIGH);
       delay(1000);
       digitalWrite(ledPin,LOW);
     }
     else if (millis() >= (webhookTimeStamp + webhookWaitTime)) {
       state = ERROR_STATE;  // Response timed out
       Serial.println("State - Response Timeout Error");
     }
     break;

   case ERROR_STATE:                                          // To be enhanced - where we deal with errors
     Serial.println("Error state - resetting Electron");
     delay(1000);
     //System.reset();                 // Today, only way out is reset
     state = IDLE_STATE;
     break;
   }
 }

void displayInfo()
{
  Serial.println("Display Info");
 	if (millis() - lastSerial >= SERIAL_PERIOD) {
 		lastSerial = millis();
 		char buf[128];
 		if (gps.location.isValid()) {
      Serial.println("Valid GPS");
 			snprintf(buf, sizeof(buf), "%f,%f,%f", gps.location.lat(), gps.location.lng(), gps.altitude.meters());
 			if (gettingFix) {
 				gettingFix = false;
 				unsigned long elapsed = millis() - startFix;
 				Serial.printlnf("%lu milliseconds to get GPS fix", elapsed);
 			}
 		}
 		else {
 			strcpy(buf, "no location");
 			if (!gettingFix) {
 				gettingFix = true;
 				startFix = millis();
 			}
 		}
 		Serial.println(buf);
    if (Particle.connected()) {
       lastPublish = millis();
       Particle.publish("gps", buf);
    }
 	}
}


void sendEvent()
{
  char data[256];                                         // Store the date in this character array - not global
  float lat =
  snprintf(data, sizeof(data), "{\"battery\":%i, \"signal\":%i, \"lat\":%f, \"lng\":%f}",stateOfCharge, RSSI, gps.location.lat(), gps.location.lng());
  Serial.println(data);
  Particle.publish("GPSlog_hook", data, PRIVATE);
  webhookTimeStamp = millis();
  dataInFlight = true; // set the data inflight flag
}

void UbidotsHandler(const char *event, const char *data)  // Looks at the response from Ubidots - Will reset Photon if no successful response
{
  // Response Template: "{{hourly.0.status_code}}"
  if (!data) {                                            // First check to see if there is any data
    Particle.publish("Ubidots Hook", "No Data");
    return;
  }
  int responseCode = atoi(data);                          // Response is only a single number thanks to Template
  if ((responseCode == 200) || (responseCode == 201))
  {
    Particle.publish("State","Response Received");
    Serial.println("Response Received");
    dataInFlight = false;                                 // Data has been received
  }
  else Particle.publish("Ubidots Hook", data);             // Publish the response code
}

void getSignalStrength()
{
    CellularSignal sig = Cellular.RSSI();  // Prototype for Cellular Signal Montoring
    RSSI = sig.rssi;
    int strength = map(RSSI, -131, -51, 0, 5);
    snprintf(Signal,17, "%s: %d", levels[strength], RSSI);
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
