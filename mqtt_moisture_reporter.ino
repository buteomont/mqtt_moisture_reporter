/**
 * This is an ESP8266 program to measure relative moisture in the soil. It utilizes the 
 * ESP8266's sleep mode to maximize battery life.
 *
 * Configuration is done via serial connection.  Enter:
 *  broker=<broker name or address>
 *  port=<port number>   (defaults to 1883)
 *  topicroot=<topic root> (something like buteomont/water/pressure/ - must end with / and 
 *  "percent" or "value" will be added)
 *  user=<mqtt user>
 *  pass=<mqtt password>
 *  ssid=<wifi ssid>
 *  wifipass=<wifi password>
 *  wet=<reading when sensor in saturated soil>
 *  dry=<reading when sensor is in dry soil>
 *  sleepTime=<seconds to sleep between measurements> (set to zero for continuous readings)
 *  debug=<1|0> to turn serial port debug statements on or off
 *  
 * Some configuration parameters can be set via MQTT topic $TOPICROOT/command.
 */
#define VERSION "20.11.13.1"  //remember to update this after every change! YY.MM.DD.REV

#include <PubSubClient.h> 
#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include "mqtt_moisture_reporter.h"

//PubSubClient callback function header.  This must appear before the PubSubClient constructor.
void incomingMqttHandler(char* reqTopic, byte* payload, unsigned int length);

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// These are the settings that get stored in EEPROM.  They are all in one struct which
// makes it easier to store and retrieve from EEPROM.
typedef struct 
  {
  unsigned int validConfig=0; 
  char ssid[SSID_SIZE] = "";
  char wifiPassword[PASSWORD_SIZE] = "";
  char mqttBrokerAddress[ADDRESS_SIZE]=""; //default
  int mqttBrokerPort=1883;
  char mqttUsername[USERNAME_SIZE]="";
  char mqttPassword[PASSWORD_SIZE]="";
  char mqttTopicRoot[MQTT_TOPIC_SIZE]="";
  int wet=485;  // Reported value in saturated soil
  int dry=876;  // Reported value in dry soil
  int sleepTime=10; //seconds to sleep between distance checks
  char mqttClientId[MQTT_CLIENTID_SIZE]=""; //will be the same across reboots
  bool debug=false;
  } conf;

conf settings; //all settings in one struct makes it easier to store in EEPROM
boolean settingsAreValid=false;

String commandString = "";     // a String to hold incoming commands from serial
bool commandComplete = false;  // goes true when enter is pressed

int moisture=0; //the last measurement
int reading=0; //the last value read from the sensor
unsigned long doneTimestamp=0; //used to allow publishes to complete before sleeping

char* clientId = settings.mqttClientId;

void setup() 
  {
  pinMode(LED_BUILTIN,OUTPUT);// The blue light on the board shows activity
  
  Serial.begin(115200);
  Serial.setTimeout(10000);
  Serial.println();
  
  while (!Serial); // wait here for serial port to connect.

  EEPROM.begin(sizeof(settings)); //fire up the eeprom section of flash
  if (settings.debug)
    {
    Serial.print("Settings object size=");
    Serial.println(sizeof(settings));
    }
    
  commandString.reserve(200); // reserve 200 bytes of serial buffer space for incoming command string

  loadSettings(); //set the values from eeprom
  if (settings.mqttBrokerPort < 0) //then this must be the first powerup
    {
    Serial.println("\n*********************** Resetting All EEPROM Values ************************");
    initializeSettings();
    saveSettings();
    delay(2000);
    ESP.restart();
    }

  if (settingsAreValid)
    {
    // ********************* attempt to connect to Wifi network
    if (settings.debug)
      {
      Serial.print("Attempting to connect to WPA SSID \"");
      Serial.print(settings.ssid);
      Serial.println("\"");
      }
    WiFi.mode(WIFI_STA); //station mode, we are only a client in the wifi world
    WiFi.begin(settings.ssid, settings.wifiPassword);
    while (WiFi.status() != WL_CONNECTED) 
      {
      // not yet connected
      if (settings.debug)
        {
        Serial.print(".");
        }
      checkForCommand(); // Check for input in case something needs to be changed to work
      delay(500);
      }
  
    if (settings.debug)
      {
      Serial.println("Connected to network.");
      Serial.println();
      }

    // ********************* Initialize the MQTT connection
    mqttClient.setBufferSize(JSON_STATUS_SIZE);
    mqttClient.setServer(settings.mqttBrokerAddress, settings.mqttBrokerPort);
    mqttClient.setCallback(incomingMqttHandler);
    reconnect();  // connect to the MQTT broker
     
  // let's do this 
    reading=measure();
    moisture=map(reading, settings.wet, settings.dry,100,0);
    report();
    if (doneTimestamp<millis()) //don't set this if someone else already did
      doneTimestamp=millis(); //this is to allow the publish to complete before sleeping
    }
  else
    {
    showSettings();
    }
  }


/**
 * Handler for incoming MQTT messages.  The payload is the command to perform. 
 * The MQTT message topic sent is the topic root plus the command.
 * Implemented commands are: 
 * MQTT_PAYLOAD_SETTINGS_COMMAND: sends a JSON payload of all user-specified settings
 * MQTT_PAYLOAD_REBOOT_COMMAND: Reboot the controller
 * MQTT_PAYLOAD_VERSION_COMMAND Show the version number
 * MQTT_PAYLOAD_STATUS_COMMAND Show the most recent flow values
 * 
 * Note that unless sleeptime is zero, any MQTT command must be sent in the short time
 * between connecting to the MQTT server and going to sleep.  In this case it is best
 * to send the command with the "retain" flag on. Be sure to remove the retained message
 * after it has been received by sending a new empty message with the retained flag set.
 */
void incomingMqttHandler(char* reqTopic, byte* payload, unsigned int length) 
  {
  if (settings.debug)
    {
    Serial.print("*************************** Received topic ");
    Serial.println(reqTopic);
    }
  payload[length]='\0'; //this should have been done in the caller code, shouldn't have to do it here
  boolean rebootScheduled=false; //so we can reboot after sending the reboot response
  char charbuf[100];
  sprintf(charbuf,"%s",reqTopic);
  char* response;
  char topic[MQTT_TOPIC_SIZE];

  //General command?
  strcpy(topic,settings.mqttTopicRoot);
  strcat(topic,MQTT_TOPIC_COMMAND_REQUEST);
  if (strcmp(charbuf,topic)==0) //then we have received a command
    {
    sprintf(charbuf,"%s",payload);
  
    //if the command is MQTT_PAYLOAD_SETTINGS_COMMAND, send all of the settings
    if (strcmp(charbuf,MQTT_PAYLOAD_SETTINGS_COMMAND)==0)
      {
      char tempbuf[35]; //for converting numbers to strings
      char jsonStatus[JSON_STATUS_SIZE];
      
      strcpy(jsonStatus,"{");
      strcat(jsonStatus,"\"broker\":\"");
      strcat(jsonStatus,settings.mqttBrokerAddress);
      strcat(jsonStatus,"\", \"port\":");
      sprintf(tempbuf,"%d",settings.mqttBrokerPort);
      strcat(jsonStatus,tempbuf);
      strcat(jsonStatus,", \"topicroot\":\"");
      strcat(jsonStatus,settings.mqttTopicRoot);
      strcat(jsonStatus,"\", \"user\":\"");
      strcat(jsonStatus,settings.mqttUsername);
      strcat(jsonStatus,"\", \"pass\":\"");
      strcat(jsonStatus,settings.mqttPassword);
      strcat(jsonStatus,"\", \"ssid\":\"");
      strcat(jsonStatus,settings.ssid);
      strcat(jsonStatus,"\", \"wifipass\":\"");
      strcat(jsonStatus,settings.wifiPassword);
      strcat(jsonStatus,"\", \"wet\":\"");
      sprintf(tempbuf,"%d",settings.wet);
      strcat(jsonStatus,tempbuf);
      strcat(jsonStatus,"\", \"dry\":\"");
      sprintf(tempbuf,"%d",settings.dry);
      strcat(jsonStatus,tempbuf);
      strcat(jsonStatus,"\", \"sleepTime\":\"");
      sprintf(tempbuf,"%d",settings.sleepTime);
      strcat(jsonStatus,tempbuf);
      strcat(jsonStatus,"\", \"mqttClientId\":\"");
      strcat(jsonStatus,settings.mqttClientId);
      strcat(jsonStatus,"\", \"debug\":\"");
      strcat(jsonStatus,settings.debug?"true":"false");

      strcat(jsonStatus,"\"}");
      response=jsonStatus;
      }
    else if (strcmp(charbuf,MQTT_PAYLOAD_VERSION_COMMAND)==0) //show the version number
      {
      char tmp[15];
      strcpy(tmp,VERSION);
      response=tmp;
      }
    else if (strcmp(charbuf,MQTT_PAYLOAD_STATUS_COMMAND)==0) //show the latest value
      {
      report();
      
      char tmp[25];
      strcpy(tmp,"Status report complete");
      response=tmp;
      }
    else if (strcmp(charbuf,MQTT_PAYLOAD_REBOOT_COMMAND)==0) //reboot the controller
      {
      char tmp[10];
      strcpy(tmp,"REBOOTING");
      response=tmp;
      rebootScheduled=true;
      }
    else if (processCommand(charbuf))
      {
      response="OK";
      }
    else
      {
      char badCmd[18];
      strcpy(badCmd,"(empty)");
      response=badCmd;
      }
      
    char topic[MQTT_TOPIC_SIZE];
    strcpy(topic,settings.mqttTopicRoot);
    strcat(topic,charbuf); //the incoming command becomes the topic suffix
  
    doneTimestamp=millis(); //this will keep us from sleeping before responding

    if (!publish(topic,response,false)) //do not retain
      {
      int code=mqttClient.state();
      Serial.print("************ Failure ");
      Serial.print(code);
      Serial.println(" when publishing command response!");
      }
    }
  if (rebootScheduled)
    {
    delay(2000); //give publish time to complete
    ESP.restart();
    }
  }

/*
 * Save the settings object to EEPROM and publish a "show settings" command.
 */
void saveAndShow()
  {
  saveSettings();
  char tmp[25]; 
  char topic[MQTT_TOPIC_SIZE];
  strcpy(topic,settings.mqttTopicRoot);
  strcat(topic,MQTT_TOPIC_COMMAND_REQUEST); //Send ourself the command to display settings

  if (!publish(topic,MQTT_PAYLOAD_SETTINGS_COMMAND,false)) //do not retain
    Serial.println("************ Failure when publishing show settings response!");
  }

void loop()
  {
  // may need to reconnect to the MQTT broker. This is true even if the report is 
  // already sent, because a MQTT command may come in
  reconnect();  
  
  checkForCommand(); // Check for serial input in case something needs to be changed
  
  if (settingsAreValid && settings.sleepTime==0) //if sleepTime is zero then don't sleep
    {
    reading=measure();  
    moisture=map(reading, settings.wet, settings.dry,100,0);
    report();    
    } 
  else if (settingsAreValid                        //setup has been done and
          && millis()-doneTimestamp>PUBLISH_DELAY) //waited long enough for report to finish
    {  
    Serial.print("Sleeping for ");
    Serial.print(settings.sleepTime);
    Serial.println(" seconds");
    ESP.deepSleep(settings.sleepTime*1000000);
    }
  }

// Read the moisture 10 times and return the dominant value
int measure()
  {
  int vals[10];
  int answer,answerCount=0;

  //get 10 samples
  for (int i=0;i<10;i++)
    {
    // Turn on the LED to show activity
    digitalWrite(LED_BUILTIN,LED_ON);
    
    vals[i]=readSensor();

    // Turn off the LED
    digitalWrite(LED_BUILTIN,LED_OFF);
    
    delay(50); //give it some space
    }

  //find the most common value within the sample set
  //This code is not very efficient but hey, it's only 10 values
  for (int i=0;i<9;i++) //using 9 here because the last one can only have a count of 1
    {
    int candidate=vals[i];
    int candidateCount=1;  
    for (int j=i+1;j<10;j++)
      {
      if (candidate==vals[j])
        {
        candidateCount++;
        }
      }
    if (candidateCount>answerCount)
      {
      answer=candidate;
      answerCount=candidateCount;
      }
    }
  return answer;
  }



//Take a measurement
int readSensor()
  {  
  return analogRead(A0);
  }


void showSettings()
  {
  Serial.print("broker=<MQTT broker host name or address> (");
  Serial.print(settings.mqttBrokerAddress);
  Serial.println(")");
  Serial.print("port=<port number>   (");
  Serial.print(settings.mqttBrokerPort);
  Serial.println(")");
  Serial.print("topicroot=<topic root> (");
  Serial.print(settings.mqttTopicRoot);
  Serial.println(")  Note: must end with \"/\"");  
  Serial.print("user=<mqtt user> (");
  Serial.print(settings.mqttUsername);
  Serial.println(")");
  Serial.print("pass=<mqtt password> (");
  Serial.print(settings.mqttPassword);
  Serial.println(")");
  Serial.print("ssid=<wifi ssid> (");
  Serial.print(settings.ssid);
  Serial.println(")");
  Serial.print("wifipass=<wifi password> (");
  Serial.print(settings.wifiPassword);
  Serial.println(")");
  Serial.print("wet=<saturated soil reading> (");
  Serial.print(settings.wet);
  Serial.println(")");
  Serial.print("dry=<dry soil reading> (");
  Serial.print(settings.dry);
  Serial.println(")");
  Serial.print("sleeptime=<seconds to sleep between measurements> (");
  Serial.print(settings.sleepTime);
  Serial.println(")");
  Serial.print("MQTT Client ID is ");
  Serial.println(settings.mqttClientId);
  Serial.print("debug=1|0 (");
  Serial.print(settings.debug);
  Serial.println(")");
  Serial.println("\n*** Use \"factorydefaults=yes\" to reset all settings ***\n");
  }

/*
 * Reconnect to the MQTT broker
 */
void reconnect() 
  {
  mqttClient.loop(); //This has to happen every so often or we can't receive messages
  // Loop until we're reconnected
  while (!mqttClient.connected()) 
    {
    if (settings.debug)
      {
      Serial.print("Attempting MQTT connection...");
      }
    
    // Attempt to connect
    if (mqttClient.connect(settings.mqttClientId,settings.mqttUsername,settings.mqttPassword))
      {
      if (settings.debug)
        {
        Serial.println("connected to MQTT broker.");
        }
      //subscribe to the incoming message topics
      char topic[MQTT_TOPIC_SIZE];
      strcpy(topic,settings.mqttTopicRoot);
      strcat(topic,MQTT_TOPIC_COMMAND_REQUEST);
      int subok=mqttClient.subscribe(topic);
      showSub(topic);
      }
    else 
      {
      Serial.print("failed, rc=");
      Serial.println(mqttClient.state());
      Serial.println("Will try again in a second");
      
      // Wait a second before retrying
      // In the meantime check for input in case something needs to be changed to make it work
      checkForCommand(); 
      
      delay(1000);
      }
    }
  }

void showSub(char* topic)
  {
  if (settings.debug)
    {
    Serial.print("++++++Subscribing to ");
    Serial.print(topic);
    Serial.print(":\t");
    Serial.println();
    }
  }

  
/*
 * Check for configuration input via the serial port.  Return a null string 
 * if no input is available or return the complete line otherwise.
 */
String getConfigCommand()
  {
  if (commandComplete) 
    {
    Serial.println(commandString);
    String newCommand=commandString;

    commandString = "";
    commandComplete = false;
    return newCommand;
    }
  else return "";
  }

bool processCommand(String cmd)
  {
  const char *str=cmd.c_str();
  char *val=NULL;
  char *nme=strtok((char *)str,"=");
  if (nme!=NULL)
    val=strtok(NULL,"=");

  //Get rid of the carriage return
  if (val!=NULL && strlen(val)>0 && val[strlen(val)-1]==13)
    val[strlen(val)-1]=0; 

  if (nme==NULL || val==NULL || strlen(nme)==0 || strlen(val)==0)
    {
    showSettings();
    return false;   //bad or missing command
    }
  else if (strcmp(nme,"broker")==0)
    {
    strcpy(settings.mqttBrokerAddress,val);
    saveSettings();
    }
  else if (strcmp(nme,"port")==0)
    {
    settings.mqttBrokerPort=atoi(val);
    saveSettings();
    }
  else if (strcmp(nme,"topicroot")==0)
    {
    strcpy(settings.mqttTopicRoot,val);
    saveSettings();
    }
  else if (strcmp(nme,"user")==0)
    {
    strcpy(settings.mqttUsername,val);
    saveSettings();
    }
  else if (strcmp(nme,"pass")==0)
    {
    strcpy(settings.mqttPassword,val);
    saveSettings();
    }
  else if (strcmp(nme,"ssid")==0)
    {
    strcpy(settings.ssid,val);
    saveSettings();
    }
  else if (strcmp(nme,"wifipass")==0)
    {
    strcpy(settings.wifiPassword,val);
    saveSettings();
    }
  else if (strcmp(nme,"dry")==0)
    {
    settings.dry=atoi(val);
    saveSettings();
    }
  else if (strcmp(nme,"wet")==0)
    {
    settings.wet=atoi(val);
    saveSettings();
    }
  else if (strcmp(nme,"sleeptime")==0)
    {
    settings.sleepTime=atoi(val);
    saveSettings();
    }
  else if (strcmp(nme,"debug")==0)
    {
    settings.debug=atoi(val)==1?true:false;
    saveSettings();
    }
 else if ((strcmp(nme,"factorydefaults")==0) && (strcmp(val,"yes")==0)) //reset all eeprom settings
    {
    Serial.println("\n*********************** Resetting EEPROM Values ************************");
    initializeSettings();
    saveSettings();
    delay(2000);
    ESP.restart();
    }
  else //invalid command
    {
    showSettings();
    return false;
    }
  return true;
  }

void initializeSettings()
  {
  settings.validConfig=0; 
  strcpy(settings.ssid,"");
  strcpy(settings.wifiPassword,"");
  strcpy(settings.mqttBrokerAddress,""); //default
  settings.mqttBrokerPort=1883;
  strcpy(settings.mqttUsername,"");
  strcpy(settings.mqttPassword,"");
  strcpy(settings.mqttTopicRoot,"");
  settings.wet=485;
  settings.dry=876;
  settings.sleepTime=10;
  strcpy(settings.mqttClientId,strcat(MQTT_CLIENT_ID_ROOT,String(random(0xffff), HEX).c_str()));
  }

void checkForCommand()
  {
  if (Serial.available())
    {
    serialEvent();
    String cmd=getConfigCommand();
    if (cmd.length()>0)
      {
      processCommand(cmd);
      }
    }
  }


/************************
 * Do the MQTT thing
 ************************/
void report()
  {  
  char topic[MQTT_TOPIC_SIZE];
  char value[18];
  boolean success=false;

  //publish the last reading value
  strcpy(topic,settings.mqttTopicRoot);
  strcat(topic,MQTT_TOPIC_READING);
  sprintf(value,"%d",reading); 
  success=publish(topic,value,true); //retain
  if (!success)
    Serial.println("************ Failed publishing sensor reading!");

  //publish the moisture content
  strcpy(topic,settings.mqttTopicRoot);
  strcat(topic,MQTT_TOPIC_MOISTURE);
  sprintf(value,"%d",moisture); //item within range window
  success=publish(topic,value,true); //retain
  if (!success)
    Serial.println("************ Failed publishing moisture value!");
  }

boolean publish(char* topic, char* reading, bool retain)
  {
  Serial.print(topic);
  Serial.print(" ");
  Serial.println(reading);
  return mqttClient.publish(topic,reading,retain);
  }

  
/*
*  Initialize the settings from eeprom and determine if they are valid
*/
void loadSettings()
  {
  EEPROM.get(0,settings);
  if (settings.validConfig==VALID_SETTINGS_FLAG)    //skip loading stuff if it's never been written
    {
    settingsAreValid=true;
    if (settings.debug)
      {
      Serial.println("Loaded configuration values from EEPROM");
      }
//    showSettings();
    }
  else
    {
    Serial.println("Skipping load from EEPROM, device not configured.");    
    settingsAreValid=false;
    }
  }

/*
 * Save the settings to EEPROM. Set the valid flag if everything is filled in.
 */
boolean saveSettings()
  {
  if (strlen(settings.ssid)>0 &&
    strlen(settings.wifiPassword)>0 &&
    strlen(settings.mqttBrokerAddress)>0 &&
    settings.mqttBrokerPort!=0 &&
    strlen(settings.mqttTopicRoot)>0 &&
    strlen(settings.mqttClientId)>0 &&
    settings.wet!=0 &&
    settings.dry!=0 
    )
    {
    Serial.println("Settings deemed complete");
    settings.validConfig=VALID_SETTINGS_FLAG;
    settingsAreValid=true;
    }
  else
    {
    Serial.println("Settings still incomplete");
    settings.validConfig=0;
    settingsAreValid=false;
    }
    
  //The mqttClientId is not set by the user, but we need to make sure it's set  
  if (strlen(settings.mqttClientId)==0)
    {
    strcpy(settings.mqttClientId,strcat(MQTT_CLIENT_ID_ROOT,String(random(0xffff), HEX).c_str()));
    }
    
    
  EEPROM.put(0,settings);
  return EEPROM.commit();
  }

  
/*
  SerialEvent occurs whenever a new data comes in the hardware serial RX. This
  routine is run between each time loop() runs, so using delay inside loop can
  delay response. Multiple bytes of data may be available.
*/
void serialEvent() 
  {
  while (Serial.available()) 
    {
    // get the new byte
    char inChar = (char)Serial.read();

    // if the incoming character is a newline, set a flag so the main loop can
    // do something about it 
    if (inChar == '\n') 
      {
      commandComplete = true;
      }
    else
      {
      // add it to the inputString 
      commandString += inChar;
      }
    }
  }
