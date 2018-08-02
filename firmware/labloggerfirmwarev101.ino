/*
TODO: write intro part (this part)
*/

//we're adding systhem threading so we can detect network errors without the code not executing.
SYSTEM_THREAD(ENABLED);

#define DEBUG TRUE


/* 
The following block is fixed settings for analog measurements
analog reference and ADCBits are defined to make porting this
code to other hardware easier.
*/
#define AREF 3.3
#define ADCBITS 4096

/*if the Photon is longer offline than this time-limit (in ms),
it will reset itself, thus trying to re-connect to network
*/
#define OFFLINETIMELIMIT 120000


//default settings
#define DEFAULTSAMPLETIME 10000
#define IDENTDEFAULT 1234
#define DEFAULTNRSENSORS 6
#define DEFAULTNUMSAMPLES 50
#define DEFAULTSTORELOCAL FALSE
#define DEFAULTTOPIC "measurement"


//empty string object to hold topic once it is loaded from EEPROM
String  topicString;

//the main timer that initializes the measurements
Timer measurementTimer(DEFAULTSAMPLETIME, doMeasurement);

//variables to track online status.
bool offline = FALSE;
long offlineTime;

//the structure type that holds the settings. These are read from EEPROM. If they have 
//never been set, they are filled with the default values.
struct settingsStruct {
    long sampleTime;
    int nrOfSensors;
    int numSamples;
    bool storeLocal;
    int ident;
    char topic[64];
};

//the address in EEPROM where the settings are stored
const int addr = 0;
//the actual instance of the settings struct
settingsStruct settings;


void setup() {
    //only if debugging, open Serial to monitor over USB
    if (DEBUG) Serial.begin(9600);
    
    //get the settings from EEPROM memory.
    EEPROM.get(addr, settings);
    
    /*
    if settings never set before, set settings to defaults. 
    The ident flag is used to know if the settings have ever been changed.
    If the device is freshly flashed with firmware and the EEPROM is also
    flashed, ident will be equal to 0000, this triggering the below if-statement.
    This will load the default settings and store them in EEPROM. A subsequent reset 
    (either through user or power failure) will not erase the settings.
    
    EEPROM can be read at no cost, but has limited write-cycles. By storing the settings in
    this fashion the amount of EEPROM writes is minimized. 
    
    Credit to Kees van Beek of Delft University of Technology for teaching Rolf Hut how to
    store settings in this elegant way.
    */
    if (settings.ident != IDENTDEFAULT){ 
        //load default settings
        settings.sampleTime     = DEFAULTSAMPLETIME;
        settings.nrOfSensors    = DEFAULTNRSENSORS;
        settings.numSamples     = DEFAULTNUMSAMPLES;
        settings.storeLocal     = DEFAULTSTORELOCAL;
        String(DEFAULTTOPIC).toCharArray(settings.topic,64);
        
        //set the ident flag to indicate settings have been stored and not 
        //overwrite the next time the device reboots
        settings.ident = IDENTDEFAULT;
        //store the settings.
        EEPROM.put(addr, settings);
    }
    
    topicString = String(settings.topic);
    
    
    /*
    Subscribe to the topic to change settings. This will trigger the handleSettings
    function anytime a message with the topic "settings/[DEVICEID]/[SETTING]" is published.
    [DEVICEID] is the ID of the Particle Photon. IF a fleet of devices is used, this makes sure
    that only the targeted device reacts.
    [SETTINGS] is a settings type, see the documentation in the handleSettings function below, or in
    the general documentation on GitHub.
    */
    Particle.subscribe("settings/"+System.deviceID(), handleSettings, MY_DEVICES);
    Particle.subscribe("settings/all", handleSettings, MY_DEVICES);
    
    
    /*
    If local storage is used, it is assumed that a SparkFun OpenLog is connected to 
    the Tx and Rx pins. All measurements will be stored on the OpenLog.
    */
    if (settings.storeLocal == TRUE) Serial1.begin(9600);

    //set the sampleTime and start the clock!
    measurementTimer.changePeriod(settings.sampleTime);
    measurementTimer.start();

}

/*
Nothing to do in the main loop because measurements are triggered using the measurementTimer object
and changing settings is handles in the handleSettings function
*/
void loop() {

}


/*
The doMeasurement function loops through the analog sensors, does measurements and
uploads the results. 
*/
void doMeasurement(){
    //loop through the analog sensors first
    for (int n=0;n<settings.nrOfSensors;n++){
        
        //if this is the fourth sensor, send data to cloud, wait a second, to not
        //overload the 4 messages per second limit on the cloud.
        if ((n % 4)==0) {
            Particle.process();
            delay(1000);
        }
        
        //do measurement, convert to volts. Add the current timestamp.
        float rawMeas = analogAverage(A0+n, settings.numSamples);
        float volts = rawMeas * (AREF / ADCBITS);
        int currentTime = Time.now();
        
        /*construct the message for this measurement. First the sensor number, where A0 = 10, A1 = 11 
        etc. Then the time (epoch), than the measurement (volts at digital pin)
        */
        String pubMessage = String(A0 + n, DEC) + ","+String(currentTime) + "," + String(volts,2);
        
        //write pubMessage to OpenLog
        if (settings.storeLocal == TRUE) Serial1.println(pubMessage);
        
        //Publish to cloud
        if (Particle.connected()){
            offline = FALSE;
            Particle.publish(topicString,pubMessage,PRIVATE);
        } else {
            if (offline){
                if ((millis() - offlineTime) > OFFLINETIMELIMIT){
                    System.reset();
                }
            } else {
                offline = TRUE;
                offlineTime = millis();
            }
        }

        
        
        //if debugging, print measurement over Serial 
        if (DEBUG) {
            Serial.println("sensor: " + String(A0 + n, DEC));
            Serial.println("voltage: " + String(volts,2) + " V");
        }
  
    }
}

float analogAverage(int pin, int numSamples){
    /*
    averages numSamples measurements on pin. 
    */
    uint8_t i;
    //array to hold the samples
    uint16_t totalSamples=0;
    // take N samples in a row, with a slight delay
    for (i=0; i< numSamples; i++) {
        totalSamples += analogRead(pin);
        delay(10);
    }
    
    return (((float) totalSamples) / numSamples);
}

void handleSettings(const char *topic, const char *data){

    //for easy manipulation, change the received topic and data to Strings
    String topicString = String(topic);
    String stringData=String(data);
    
    //if debugging, print the received topic and data
    if (DEBUG) Serial.println("received " + topicString + ": " + stringData);
    
    //isolate the ID received. This should always either be the ID of the device of
    //"all". This check should be redundent, but is left in place to be sure.
    String IDreceived = topicString.substring(topicString.indexOf("/") + 1,topicString.lastIndexOf("/"));
    
    if ((IDreceived == System.deviceID()) || (IDreceived == "all")){
        
        //isolate the settingType from the topic, than cycle through the options
        String settingType = topicString.substring(topicString.lastIndexOf("/") + 1);
        
        //if "uploadName" is the settingType, we return by publishing the name of the device
        if (settingType == "uploadName"){
            Particle.subscribe("particle/device/name", handlerNameAnswer);
            Particle.publish("particle/device/name");
            
        /*
        if "sampleTime" is the settingType, the data will be set as the new sampleTime (in seconds)
        minimum sampleTime is 5 seconds, to not overload the network
        */
        } else if (settingType == "sampleTime"){
            long requestedSampleTime = stringData.toInt();
            if (requestedSampleTime < 5){
                Particle.publish("logging","sampleTime must be at least 5 seconds");
            } 
            else {
                settings.sampleTime = requestedSampleTime * 1000;
                measurementTimer.changePeriod(settings.sampleTime);
                EEPROM.put(addr, settings);
                Particle.publish("logging","sampleTime set to " + String(settings.sampleTime) + " milliseconds.");
            }
        /*
        if "nrOfSensors" is the settingType, the data will be set as the new number of sensors (starting at A0)
        with a maximum of 6 sensors.
        */
        } else if (settingType == "nrOfSensors"){
            int requestedNrOfSensors = stringData.toInt();
            if (requestedNrOfSensors > 6){
                Particle.publish("logging","nrOfSensors must be at no bigger than 6");
            } 
            else {
                settings.nrOfSensors = requestedNrOfSensors;
                EEPROM.put(addr, settings);
                Particle.publish("logging","nrOfSensors set to " + String(settings.nrOfSensors));
            }
        /*
        if "topic" is the settingType, the data will be set as the new topic used for publishing to 
        the Partilce Cloud
        */
        } else if (settingType == "topic"){
            
            stringData.toCharArray(settings.topic,64);
            topicString = String(settings.topic);
            Particle.publish("logging","topic set to: " + topicString);
            EEPROM.put(addr, settings);
        /*
        Any other settingsType will results in a published error.
        */
        } else {
            Particle.publish("logging","incorrect settings topic received.");
        }
    } else {
        Particle.publish("logging","ID received is not this device");
    }

}


//this functions handles passing the name of the device to the website    
void handlerNameAnswer(const char *topic, const char *data) {
    Particle.publish("updateName",String(data));
}