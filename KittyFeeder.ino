#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <functional>
#include "switch.h"
#include "UpnpBroadcastResponder.h"
#include "CallbackFunction.h"
#include "SimpleTimer.h"

// Site specfic variables
const char* ssid = "ssid";
const char* password = "password";

const int catdetectorlowerthreshold = 10; //cm.  Readings over this value indicate cat is present in front of the sensor
const int catdetectorupperthreshold = 35; //cm.  Readings over this value indicate cat isnt present in front of the sensor, and its probably the wall 
const int catdetectorinvalidthreshold = 10; //cm.  Readings under this value are discarded as invalid
const int catdetectorfurrythreshold = 1500; // cm Readings over this value indicate cat is present in front of the sensor (furry mode)
// Oddly, sonar onto a cats fur sometimes returns a very large return delay equivalent to the max possible reading of the sensor
const int RelayPulseLength = 1200;    //Length of relay pulse in ms (has RC timeconstant on the relay drive to reduce startup pulse problem.
const int CatCountThreshold = 5; // The number of consectutive sucessfull sonar mesuarements before a cat is confirmed present at the bowl

boolean connectWifi();
boolean wifiConnected = false;

UpnpBroadcastResponder upnpBroadcastResponder;

Switch *feedthecat = NULL;
//Switch *catfeederboot = NULL;
//Switch *catlooptest = NULL;
Switch *catstability = NULL;
//Switch *catpresentnow = NULL;
Switch *catpresentlatch = NULL;
Switch *timedfeedlatch = NULL;

//due to a bug in the way Alexa discovers devices, only 4 switches can be relaibly detected and the remainder have been commented out.
// There is a lot of unused code in this file as a result. Sue me.


bool catdetectedstate = false;
bool catfeederbootstate = false;
bool PulseRelaySemaphore = false;
bool UDPActiveSemaphore = false;
bool catfeederloopstatus = false;
bool catfeederstabilitystatus = true;
bool catdetectedhistory = false;
bool alexafeedflag = false;
bool catdetectedlatch = false;
bool timedfeedflag = false;
bool UDPRestartTried = false; 
bool DebugMessages = false;
bool UseLinearMotor = true; // if set to true, the relay attached to catfeederrelay will energise for RelayPulseLength for each feed
bool UseStepperMotor = false; // if set to true, the stepper motor attached to StepPin will rotateSixthRotate pulses for each feed


const int catfeederrelay = 0;  // GPIO0 pin.
const int GPIO2 = 2;  // GPIO2 pin.
const int StepPin = 2;  // GPIO2 pin.

long duration, distance; // Duration used to calculate distance

const int echoPin = 3;  // 3 is the RX pin
const int trigPin = 1; // 1 is the TX Pin

int SixthRotate = 33 ; // this is modified to 34 every 3rd call to average out to 33.3 steps
int RotateSpeed = 500;
int ThirdCounter =0;



/*  WIRING DETAILS
 *  CPU TXD (pin 1) connects to the ultrasonic Trigger pin  
 *  CPU RXD (Pin 3) connects to the ultrasonic Echo pin
 */

 
// the timer object
SimpleTimer timer; // runs timers for the timed feed functions in this code
int TimedFeed; // Timer id for 12 hr feeding if alexa doesnt.
int CatCounter = 0; // Timer id for cat detection counter
int UDPmessageTimer = 0; // Timer id for checking UDP message frequency

void setup()
{
  pinMode(catfeederrelay, OUTPUT);
  pinMode(GPIO2, OUTPUT); 

  Serial.begin(9600);


    Serial.println("Booting...");

  // Initialise wifi connection
  wifiConnected = connectWifi();

  if (wifiConnected) {
    Serial.println("flashing slow to indicate wifi connected...");
    //flash slow a few times to indicate wifi connected OK. Only works for some variants of the ESP-01 board
    digitalWrite(GPIO2, LOW);
    delay(1000);
    digitalWrite(GPIO2, HIGH);
    delay(1000);
    digitalWrite(GPIO2, LOW);
    delay(1000);
    digitalWrite(GPIO2, HIGH);
    delay(1000);
    digitalWrite(GPIO2, LOW);
    delay(1000);
    digitalWrite(GPIO2, HIGH);

    Serial.println("starting upnp responder");
    upnpBroadcastResponder.beginUdpMulticast();

    // Define your switches here. Max 10 - I can only get 5 to load reliably, hence the commented out ones below
    // Format: Alexa invocation name, local port no, on callback, off callback, Alexa talley indicator 
    feedthecat = new Switch("1 Cat Feeder Alexa Operated", 90, catfeederrelayon, catfeederrelayoff, alexafeedflag);
    //catfeederboot = new Switch("7 Cat Feeder Reboot", 91, catfeederbooton, catfeederbootoff, false);
    //catlooptest = new Switch("6 Cat Feeder Loop Test", 92, catfeederloopon, catfeederloopoff, catfeederloopstatus);
    catstability = new Switch("3 Cat Feeder Has Restarted + Reset", 93, catfeederstabilityon, catfeederstabilityoff, catfeederstabilitystatus);
    //catpresentnow = new Switch("5 Cat is Present Now", 94, catpresentnowon, catpresentnowoff, catdetectedstate);
    catpresentlatch = new Switch("4 Cat Present Latch + Reset", 95, catpresentlatchon, catpresentlatchoff, catdetectedlatch);
    timedfeedlatch = new Switch("2 Cat Feeder - Time Operated Feed + Reset", 96, timedfeedon, timedfeedoff, timedfeedflag );
   
    Serial.println("Adding switches upnp broadcast responder");
    upnpBroadcastResponder.addDevice(*feedthecat);   
    //upnpBroadcastResponder.addDevice(*catfeederboot);
    //upnpBroadcastResponder.addDevice(*catlooptest);
    upnpBroadcastResponder.addDevice(*catstability);
    //upnpBroadcastResponder.addDevice(*catpresentnow);
    upnpBroadcastResponder.addDevice(*catpresentlatch);
    upnpBroadcastResponder.addDevice(*timedfeedlatch);
  }

  digitalWrite(catfeederrelay, LOW); // turn off relay
  digitalWrite(GPIO2, HIGH); // turn off LED

 if (DebugMessages == true)  { 
    Serial.println("Making RX pin into an INPUT"); // used to detect the presence of the cat via ultrasonic ranging
  }
  //GPIO 3 (RX) swap the pin to a GPIO.

  pinMode(echoPin, FUNCTION_3);
  pinMode(echoPin, INPUT);
  

TimedFeed = timer.setInterval(((1000*60*60*12)+(1000*60*2)), TimedFeedTask);// 12 hrs 2 mins
//TimedFeed = timer.setInterval(((1000*60*60*1)+(1000*60*2)), TimedFeedTask);// 1 hrs 2 mins
//TimedFeed = timer.setInterval(((1000*60)), TimedFeedTask);// 1 mins 

UDPmessageTimer = timer.setInterval((1000*60*15), UDPmessageTimerTask);// 15 mins
//UDPmessageTimer = timer.setInterval((1000*15), UDPmessageTimerTask);// 15 secs

UDPActiveSemaphore = LOW;

  }

void TimedFeedTask() {
  
   if (DebugMessages == true)  { 
      Serial.println("Alexa feeding hasnt occured as expected, so feed the cat autonomously");  

  }

    timedfeedflag = HIGH; // set the call back indicator to ON to show the timer fed the cat.
    PulseRelaySemaphore = HIGH; // set relay semaphore 
    // potential to use that timedfeedflag to reboot rather than indicate to alaexa that a timed feed has occured
    // you would have to allow the pulse relay routine to complete first
       
}

void UDPmessageTimerTask() {

  if (UDPRestartTried == HIGH) { // we have been here before, time for a restart this time...

   if (DebugMessages == true)  { 
      Serial.println("here if the second attempt to restart comms - Regular UDP messages havent occured as expected, so reboot"); 
    }
    // Actually, you cant do this, as the auto feeding timer would never fire if there was a prolonged alexa outage
     // ESP.reset();     
    
  }
  
 if (DebugMessages == true)  { 
    Serial.println("here if the first attempt to restart comms - Regular UDP messages havent occured as expected, so restart wifi"); 
  }

   WiFi.persistent(false);      
    WiFi.disconnect();          
    WiFi.persistent(true);
   
   wifiConnected = connectWifi();

   UDPRestartTried = HIGH; // set the flag to reboot the next time we enter here
   timer.restartTimer(UDPmessageTimer);
  if (DebugMessages == true)  { 
    Serial.println("setting UDPRestartTried flag & Dumping UDPmessage timer ...");
   }
   UDPActiveSemaphore = LOW;
 
       
}




void loop()
{

    if (UDPActiveSemaphore == HIGH) { // flag would have been set in either switch.cpp or UpnpBroadcastResponder functions

       timer.restartTimer(UDPmessageTimer);
      if (DebugMessages == true)  { 
        Serial.println("All Good - Dumping UDPmessage timer ...");
       }
       UDPActiveSemaphore = LOW;
       UDPRestartTried = LOW; // reset the flag to reboot at the next fail of the UDPSemaphore
    }

   

    // this is where the "polling" for the timers occurs
  timer.run();

  delay(50); // Main loop timer - sets frequency of measuring cat proximity

  distance = 1; //dummy value - will be overwritten by actual measurement each pass. Must be < lowerthreshold

  while (distance < catdetectorinvalidthreshold ) { // if its less than min, throw that reading away as invalid

   measuredistance();

      catdetectedstate = false ;  
  }
  
  if (distance > catdetectorlowerthreshold ) { // distance is valid
    //Serial.println("distance is > catdetectorlowerthreshold");
    if (distance < catdetectorupperthreshold ) {// check its not just the wall in the distance...
       if (DebugMessages == true)  { 
          Serial.println("distance is > catdetectorlowerthreshold and < catdetectorupperthreshold - its a target in the correct range which is probably the cat");
        }
        catdetectedstate = true ;
    }
    if (distance > catdetectorfurrythreshold ){ // Oddly, sometimes the cats fur returns a huge mesurement, approx 10 times what is real... I think its the nill responce from the sensor
      if (DebugMessages == true)  { 
        Serial.println("distance is > catdetectorfurrythreshold - its a real cat");
       }
       catdetectedstate = true ;
    }

   }
  
  
  
  if (catdetectedstate == LOW){
    if (catdetectedhistory == HIGH){
     if (DebugMessages == true)  { 
        Serial.println("Cat just left");
      }
    }
  }

    if (catdetectedstate == HIGH){
    if (catdetectedhistory == LOW){
     if (DebugMessages == true)  { 
        Serial.println("Cat just arrived");
      }
      
    }
  }
  
  if (catdetectedstate == HIGH){

 CatCounter = CatCounter +1;
  }
  else {
    CatCounter = 0;
    }

  if (CatCounter >= CatCountThreshold) {

   if (DebugMessages == true)  { 
      Serial.println("Cat has been detected X times sequentially - its really there!");  
    }
    catdetectedlatch = HIGH; // Set the latch bit for Alexa display
}

  
   catdetectedhistory = catdetectedstate; // store for edge detection next pass
   
  Serial.println(distance);

  if (wifiConnected) {

    upnpBroadcastResponder.serverLoop();

    //catfeederboot->serverLoop();
    feedthecat->serverLoop();
    //catlooptest->serverLoop();
    catstability->serverLoop();
    //catpresentnow->serverLoop();
    catpresentlatch->serverLoop();
    timedfeedlatch ->serverLoop();
  }

  if (PulseRelaySemaphore == HIGH) {
     if (UseLinearMotor == HIGH) {
          
       if (DebugMessages == true)  { 
          Serial.println("XXX Pulsing Relay on ...");
        }
        digitalWrite(catfeederrelay, HIGH); // turn on relay
        delay(RelayPulseLength);    ;
       if (DebugMessages == true)  { 
          Serial.println("XXX Pulsing Relay off again ...");
        }
        digitalWrite(catfeederrelay, LOW); // turn off relay
     }
     if (UseStepperMotor == HIGH) {
          
       if (DebugMessages == true)  { 
          Serial.println("Calling Stepper Motor Routine ...");
        }
        StepperFeedOnce();
     }
     
    // this code here, as a delay of >5 sec in the routines that run
    // as each control activates makes alexa think the devices are unresponsive
    // in this way, the alexa response is sent quickly, and the slow relay pulse or stepper motor functions
    // is done outside that loop

    PulseRelaySemaphore = LOW;
  }
}

bool catfeederrelayon() {
 if (DebugMessages == true)  { 
    Serial.println("Request to feed the cat received ...");
  }

    alexafeedflag = HIGH; // set the call back indicator to ON to show alexa fed the cat.
    PulseRelaySemaphore = HIGH; // set relay semaphore 
     
    timer.restartTimer(TimedFeed);
     if (DebugMessages == true)  { 
        Serial.println("Dumping Timed feed timer ...");
      }
      
  return alexafeedflag;
}

bool catfeederrelayoff() {

 if (DebugMessages == true)  { 
    Serial.println("Request to feed the cat received ...");
  }

    alexafeedflag = HIGH; // set the call back indicator to ON to show alexa fed the cat.
    PulseRelaySemaphore = HIGH; // set relay semaphore 
     
    timer.restartTimer(TimedFeed);
     if (DebugMessages == true)  { 
        Serial.println("Dumping Timed feed timer ...");
      }
      
  return alexafeedflag;

}


bool catfeederbooton() {
 if (DebugMessages == true)  { 
    Serial.println("Request to reboot cat feeder controller received... ");
  }

  ESP.reset(); 

  return true;
}

bool catfeederbootoff() {

if (DebugMessages == true)  { 
  Serial.println("Request to reboot cat feeder controller received... ");
 }

    ESP.reset(); 

  return true;
}

bool catfeederloopon() {
 if (DebugMessages == true)  { 
   if (DebugMessages == true)  { 
      Serial.println("Request to loop test the system received ...");
    }
  }
      
    catfeederloopstatus = !catfeederloopstatus;

  return catfeederloopstatus;
}

bool catfeederloopoff() {

 if (DebugMessages == true)  { 
    Serial.println("Request to loop test the system received ...");
  }
    
    catfeederloopstatus = !catfeederloopstatus;

  return catfeederloopstatus;
}


bool catfeederstabilityon() {
 if (DebugMessages == true)  { 
    Serial.println("Request to reset the stability indicator received ...");
  }
      
    catfeederstabilitystatus = !catfeederstabilitystatus;

  return catfeederstabilitystatus;
}

bool catfeederstabilityoff() {

 if (DebugMessages == true)  { 
    Serial.println("Request to reset the stability indicator received...");
  }
   
  catfeederstabilitystatus = !catfeederstabilitystatus;
  
  return catfeederstabilitystatus;
}

bool catpresentnowon() {
 if (DebugMessages == true)  { 
    Serial.println("Request to do nothing received");
  }

  
  return catdetectedstate;
}

bool catpresentnowoff() {

 if (DebugMessages == true)  { 
    Serial.println("Request to do nothing received");
  }
  
  
  return catdetectedstate;
}

bool catpresentlatchon() {
 if (DebugMessages == true)  { 
    Serial.println("Request to reset the cat detected latch indicator received ...");
  }

   catdetectedlatch = LOW; // Reset the indicator
  return catdetectedlatch;
}

bool catpresentlatchoff() {

 if (DebugMessages == true)  { 
    Serial.println("Request to reset the cat detected latch indicator received...");
  }
   
   catdetectedlatch = LOW; // Reset the indicator
  return catdetectedlatch;
}

bool timedfeedon() {
 if (DebugMessages == true)  { 
    Serial.println("Request to reset both feeding flag indicators received ...");
  }

   timedfeedflag = LOW; // Reset the indicator
   alexafeedflag = LOW;
   
  return timedfeedflag;
}

bool timedfeedoff() {

 if (DebugMessages == true)  { 
    Serial.println("Request to reset both feeding flag indicators received...");
  }

  timedfeedflag = LOW; // Reset the indicator
  alexafeedflag = LOW;
  
  return timedfeedflag;
}


// connect to wifi – returns true if successful or false if not
boolean connectWifi() {
  boolean state = true;
  int i = 0;

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("");
  Serial.println("Connecting to WiFi Network");

  // Wait for connection
  Serial.print("Connecting ...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(5000);
    Serial.print(".");
    if (i > 10) {
      state = false;
      break;
    }
    i++;
  }

  if (state) {
    Serial.println("");
    Serial.print("Connected to ");
    Serial.println(ssid);
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  }
  else {
    Serial.println("");
    Serial.println("Connection failed. Bugger");
  }

  return state;
}

void measuredistance() {
  delay(100); //pause to stabilise

  /* The following trigPin/echoPin cycle is used to determine the
    distance of the nearest object by bouncing soundwaves off of it.
    The ESP-01 running on a USB adaptor gets confused easily by power supply noise
    with this code. Beware!
    especally important to run the ultrasonic sensor on its own PSU - it gets
    very septic running on the USB power as well. A grunty capacitor across
    its PSU pins helps as well.
    YMMV
  */
  Serial.println("~"); // this will trigger the ultrasonic to take a measurement
  
  duration = pulseIn(echoPin, HIGH);
  //Calculate the distance (in cm) based on the speed of sound.
  distance = duration / 58.2;

}

void StepperFeedOnce() {
  
// Makes enough pulses for making exactly one sixth cycle rotation of the stepper motor. Assumes the direction pin is held in the desired state, and that any reset or inhibit lines are similarly held in the correct state on the stepper driver board.
// every third call, this routine will rotate one additional step, to account for the 33.3 steps required for an accurate 1/6th rotation
ThirdCounter = ThirdCounter +1;
if (ThirdCounter == 3) {
ThirdCounter = 0;
SixthRotate =34; // This causes the average number of pulses sent to the stepper motor to be 33.3. Ideally it should be 33.3333333
}
for(int x = 0; x < SixthRotate; x++) {
digitalWrite(StepPin,HIGH); 
delayMicroseconds(RotateSpeed); 
digitalWrite(StepPin,LOW); 
delayMicroseconds(RotateSpeed); 
}
SixthRotate = 33; 
}

