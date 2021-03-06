/*  This program obtains live ECG readings from a sensor, uses the Pan-Tompkins 
 *  QRS-detection algorithm to calculate the corresponding Beats Per Minute (BPM), 
 *  and stores the BPMs and their corresponding time stamps on the Arduino 101's 
 *  2MB external flash memory chip. It also uses the 101's accelerometer to count 
 *  steps, and it stores that data on the chip as well. When connected to the phone, 
 *  it retrieves the BPMs and steps along with their time information from the chip 
 *  and sends them to the phone live via a custom Bluetooth Low Energy service. It 
 *  also sends live ECG measurements to the phone for a graph of the heart beat. 
 *  When the phone is disconnected for a while and then reconnects, the device 
 *  quickly sends all the accumulated BPM + time stamp data to the phone in bathces
 *  and then resumes live updating.
 *  */

#include "CurieTimerOne.h"
#include <QueueArray.h>
#include <stdio.h>
#include <stdlib.h>
#include <CurieBLE.h>
#include "CurieIMU.h"
#include <CurieTime.h>
#include <SerialFlash.h>

#define usb      // COMMENT THIS OUT IF CONNECTED TO BATTERY INSTEAD OF COMPUTER
#define nate     // COMMENT THIS OUT IF USING AN NRF APP INSTEAD OF NATE'S APP
#define actively // COMMENT THIS OUT IF WE'RE NOT INTERESTED IN
                 // THE AMOUNT OF ACTIVE TIME PER STEP EXCURSION

/* The portions of this code that implement the Pan-Tompkins QRS-detection algorithm were 
 modified from code taken from Blake Milner's real_time_QRS_detection GitHub repository:
https://github.com/blakeMilner/real_time_QRS_detection/blob/master/QRS_arduino/QRS.ino */


// BPM Memory management stuff ------------------------

#define FSIZE 128000 // the size of a file on the flash chip

#define NUM_BUFFS 11 // changing this changes the number of files in memory

#define DSIZE 8 // size of each unit of data stored in memory (BPM + time stamp)

#define DATA_PER_FILE (FSIZE / DSIZE) // how many data can be stored per file

#define STORAGE_LENGTH (DATA_PER_FILE * NUM_BUFFS) // total data that can be stored

SerialFlashFile flashFiles[NUM_BUFFS]; // holds all the files 

// keeps track of which read and write file we're on
short writeFileIndex, readFileIndex, ackFileIndex;

unsigned long nextToPlace;    // next location on the chip to write BPMs
unsigned long nextToRetrieve; // next location on the chip to read BPMs
                              // from and send them to the phone
unsigned long nextToAck;      // next location that hasn't been confirmed received

unsigned long toMemBuff[2];   // holds a BPM and time stamp on its way to the chip
unsigned long fromMemBuff[2]; // holds a BPM and time stamp on its way back
                              // the chip to be sent to the phone

const int FlashChipSelect = 21; // digital pin for flash chip CS pin


// ------Step Memory Management Stuff--------------------

#define NUM_BUFFS_STEP 3 // changing this changes the number of files in memory

#ifndef actively
#define FSIZE_STEP 128000 // the size of a file on the flash chip
#define DSIZE_STEP 8  // size of each unit of data stored in memory
#endif                // (time stamp + offset + step count)

#ifdef actively
#define FSIZE_STEP 128000 // the size of a file on the flash chip
#define DSIZE_STEP 10 // size of each unit of data stored in memory
#endif                // (time stamp + offset + step count + active time)
                     
// how many data can be stored per file
#define DATA_PER_FILE_STEP (FSIZE_STEP / DSIZE_STEP) 

// how many data can be stored total
#define STORAGE_LENGTH_STEP (DATA_PER_FILE_STEP * NUM_BUFFS_STEP) 

SerialFlashFile flashFilesStep[NUM_BUFFS_STEP]; // holds all the files 

// keeps track of which read and write file we're on
short writeFileIndexStep, readFileIndexStep;

unsigned long nextToPlaceStep;    // next location on the chip to write BPMs
unsigned long nextToRetrieveStep; // next location on the chip to read BPMs
                                  // from and send them to the phone

unsigned char toMemBuffStep[DSIZE_STEP];   // holds a time stamp + offset + step count on its
                                           // way to memory 
unsigned char fromMemBuffStep[DSIZE_STEP]; // two time stamps and a step count on their way 
                                           // back from the chip to be sent to the phone
                                           

// General BPM and ECG stuff --------------------------------

#define ECG_PIN A0              // the number of the ECG pin (analog)
#define LED_PIN 13              // indicates whether Bluetooth is connected

// pins for leads off detection
#define LEADS_OFF_PLUS_PIN 10   // the number of the LO+ pin (digital)
#define LEADS_OFF_MINUS_PIN 11  // the number of the LO- pin (digital) 

const int frequency = 5000; // rate at which the heart rate is checked
                            // (in microseconds): works out to 200 hz

QueueArray<unsigned long> bpmQueue; // holds the BPMs to be printed

QueueArray<int> ecgQueue; // holds the ECGs to be printed

int ecgQCount; // used to space out the ECG measurements sent to

boolean timeInitiated = false; // whether time has ever been set by phone

boolean safeToFill = true; // used to prevent the ECG measurement queue
                           // from being added to too early after the 
                           // phone has recently reconnected

// variables used for the periodically checking in with the phone to make 
// sure it hasn't gone out of range
unsigned long lastTimeSent; // the time stamp of the most recent data point
                            // send to the phone
int sentSinceCheckin;    // the amount of BPMs sent since we last checked in
#define timeToCheckin 10 // the seconds between times when we check in
                         // with the phone to make sure it's still connected


// General Step Detection stuff ------------------------

unsigned long stepStartTime; // the time the last excursion started
unsigned long stepEndTime;   // the last time a step was recorded
int stepCount;               // the step count of the current excursion

#ifdef actively
unsigned long lastActive; // the last time a step was taken
unsigned long activeTime; // the number of seconds the user has been active so
                          // far in the current excursion
boolean active; // keeps track of whether the user is currently "active," 
                // meaning they've taken a step recently
#endif

// keeps track of whether we're currently in an excursion
boolean detectingSteps;

// the number of seconds a user can go without taking a step
// before the excursion is considered over
#define MAX_SECS_BETWEEN_STEPS 60 

// the number of seconds a user can go without taking a step
// before they are 
#define ACTIVE_THRESHOLD 15

// Bluetooth stuff --------------------------------

// the number of bytes it takes to send a BPM + time stamp
// via Bluetooth (1 for the BPM and 4 for the time stamp)
#define BYTES_PER_PCKG 5

// the number of packages that can be send in one batch 
#define NUM_PCKGS 4

// 4 packages of 5 bytes is 20 bytes total, the maximum
//  size of a Bluetooth packet
#define BATCH_SIZE (NUM_PCKGS * BYTES_PER_PCKG)

// the number of ECGs sent at a time
#define NUM_ECGS 20

// the number of bytes it takes to send a step package
// (four for the start time, 2 for the offset, 2 for
//  the number of steps, and 2 extra if we want to 
//  include the number of active seconds)
#ifndef actively
#define BYTES_PER_PCKG_STEP 8
#endif
#ifdef actively
#define BYTES_PER_PCKG_STEP 10
#endif

// the number of step count packages we can fit in one
// Bluetooth batch. It's always 2 because, whether we're sending
// packages of 8 or 10 bytes, 2 is the smallest amount that will
// fit under our maximum of 20 bytes
#define NUM_PCKGS_STEP 2
#define BATCH_SIZE_STEP (NUM_PCKGS_STEP * BYTES_PER_PCKG_STEP)

boolean bleConnected = false;  // keeps track of whether the Bluetooth is connected

BLEPeripheral blePeripheral; // BLE Peripheral Device (the Arduino)
BLEService myService("aa7b3c40-f6ed-4ffc-bc29-5750c59e74b3"); 

// Custom BLE characteristic to hold BPM
BLECharacteristic bpmChar("b0351694-25e6-4eb5-918c-ca9403ddac47",  
    BLENotify, BYTES_PER_PCKG);  

// Custom BLE characteristic to batches of BPMs
BLECharacteristic batchChar("3cd43730-fc61-4ea7-aa18-6e7c3d798d74",  
    BLENotify, BATCH_SIZE); 

// Custom BLE characteristic to hold chunks of raw ECG measurements
BLECharacteristic ecgChar("1bf9168b-cae4-4143-a228-dc7850a37d98", 
    BLENotify, NUM_ECGS); 

// Custom BLE characteristic to confirm the last time stamp received by the phone
BLECharacteristic checkinChar("3750215f-b147-4bdf-9271-0b32c1c5c49d", 
    BLEWrite | BLENotify, 4);

// Custom BLE characteristic to send steps 
// (4 bytes for start time, 2 bytes for offset, 2 bytes for step count)
BLECharacteristic stepChar("81d4ef8b-bb65-4fef-b701-2d7d9061e492", 
    BLENotify, BYTES_PER_PCKG_STEP); 

BLECharacteristic liveStepChar("f579caa3-9390-46ae-ac67-1445b6f5b9fd", 
    BLENotify, 2); 

// the Blutooth central device (the phone)
BLECentral central = blePeripheral.central();
    
// ------------------------------------------------


// QRS-detection stuff-----------------------------

#define M             5   // for high pass filter
#define N             30  // for low pass filter
#define win_size      200 // size of the window 
#define HP_CONSTANT   ((float) 1 / (float) M)
#define MAX_BPM       100

// resolution of random number generator
#define RAND_RES 100000000

// timing variables
unsigned long found_time_micros = 0;     // time at which last QRS was found
unsigned long old_found_time_micros = 0; // time at which QRS before last was found

// interval at which to take samples and iterate algorithm (microseconds)
const long PERIOD = 1000000 / win_size;

// circular buffer for BPM averaging
float bpm = 0; 
#define BPM_BUFFER_SIZE 5
unsigned long bpm_buff[BPM_BUFFER_SIZE] = {0};
int bpm_buff_WR_idx = 0;
int bpm_buff_RD_idx = 0;
int tmp = 0;

// ----------------------------------------------------------------------------


void setup() { // called when the program starts

  #ifdef usb  
  Serial.begin(115200); // set up the serial monitor
  while(!Serial);     // wait for the serial monitor
  ecgQueue.setPrinter(Serial); // allows the ecg queue to print error messages
  bpmQueue.setPrinter(Serial);
  #endif

  sentSinceCheckin = 0;
  ecgQCount = 0;

  setUpFlash(); // sets up the flash memory chip and creates files to store BPMs

  setUpBLE(); // sets up the bluetooth services and characteristics 

  setUpStepDetection(); // sets up CurieIMU for detecting steps

  pinMode(LED_PIN, OUTPUT);

  pinMode(LEADS_OFF_PLUS_PIN, INPUT); // Setup for leads off detection LO +
  pinMode(LEADS_OFF_MINUS_PIN, INPUT); // Setup for leads off detection LO -

  CurieTimerOne.start(frequency, &updateHeartRate);  // set timer and callback
  
}

void setUpFlash() { // sets up the flash chip for memory management
  
   if (!SerialFlash.begin(FlashChipSelect)) { // make sure we successfully 
    #ifdef usb                                // connect to the flash chip
    Serial.println("Unable to access SPI Flash chip");
    #endif                                                                                                                                                                             
  }

  #ifdef usb
  Serial.println("Erasing flash chip...");
  #endif
  
  SerialFlash.eraseAll(); // erase the flash chip to start with a clean slate
  while(!SerialFlash.ready()); // don't do anything till the flash chip is erased

  #ifdef usb
  Serial.println("Flash chip erased");
  #endif

  // create NUM_BUFFS number of files and open them
  int i;
  for (i = 0; i < NUM_BUFFS; i++) {
  
    String fname = "file";
    fname = fname + String(i);  // creating the file name
    int fname_size = fname.length() + 1;
    char filename[fname_size];
    fname.toCharArray(filename, fname_size);

    if (!createIfNotExists(filename)) { // creating the file
      #ifdef usb
      Serial.println("Not enough space to create files");
      #endif
      return;
    }
    flashFiles[i] = SerialFlash.open(filename); // opening the file
  }

  // create NUM_BUFFS_STEP number of files and open them
  int j;
  for (j = 0; j < NUM_BUFFS_STEP; j++) {
  
    String fname = "stepfile";
    fname = fname + String(j);  // creating the file name
    int fname_size = fname.length() + 1;
    char filename[fname_size];
    fname.toCharArray(filename, fname_size);

    if (!createIfNotExists(filename)) { // creating the file
      #ifdef usb
      Serial.println("Not enough space to create step files");
      #endif
      return;
    }
    flashFilesStep[j] = SerialFlash.open(filename); // opening the file
  }
  
  writeFileIndex = 0; // we need to keep track of which files 
  readFileIndex = 0;  // we're currently reading and writing on 
  ackFileIndex = 0;

  nextToPlace = 0; // begin at the beginning of 
  nextToRetrieve = 0; // the first file 
  nextToAck = 0;

  writeFileIndexStep = 0;
  readFileIndexStep = 0;

  nextToPlaceStep = 0;  
  nextToRetrieveStep = 0;  
}

// creates a file if a file with that same name doesn't
// exist already
boolean createIfNotExists (const char *filename) {
  if (!SerialFlash.exists(filename)) {
    #ifdef usb
    Serial.println("Creating file " + String(filename));
    #endif
    return SerialFlash.createErasable(filename, FSIZE);
  }
  #ifdef usb
  Serial.println("File " + String(filename) + " already exists");
  #endif
  return true;
}

// adds all of the services and characteristics to the BLE peripheral
// and begins communication
void setUpBLE() {
  
    blePeripheral.setLocalName("R. Dinny");
    blePeripheral.setAdvertisedServiceUuid(myService.uuid()); // add service UUID
    blePeripheral.addAttribute(myService);// add the BLE service
    blePeripheral.addAttribute(bpmChar);  // add the BPM characteristic
    blePeripheral.addAttribute(batchChar);// add the BPM batch characteristic
    blePeripheral.addAttribute(ecgChar);  // add the ECG characteristic
    blePeripheral.addAttribute(checkinChar);  // add the checkin characteristic
    blePeripheral.addAttribute(stepChar);     // add the step characteristic
    blePeripheral.addAttribute(liveStepChar); // add the live step characteristic
 
    // Activate the BLE device.  It will start continuously transmitting BLE
    // advertising packets and will be visible to remote BLE central devices
    // until it receives a new connection
    blePeripheral.begin();
  
    #ifdef usb
    Serial.println("Bluetooth device active, waiting for connections...");
    #endif
  
}

// Turn on step detection and initialize variables that keep track
// of steps
void setUpStepDetection() {
  
  CurieIMU.begin();
  // turn on step detection mode:
  CurieIMU.setStepDetectionMode(CURIE_IMU_STEP_MODE_SENSITIVE);
  // enable step counting:
  CurieIMU.setStepCountEnabled(true);

  // indicates that we're not currently in the middle of 
  // an excursion
  detectingSteps = false;
  stepCount = 0;
  #ifdef actively
  active = false;
  #endif
  
}

void loop() { // called continuously

  if (bpmQueue.count() >= 2) { // check if there's the BPM to print

    // remove a BPM from the queue and send it to memory
    unsigned long heartRate = bpmQueue.dequeue();
    unsigned long timeStamp = bpmQueue.dequeue();
    
    toMemBuff[0] = heartRate; // put the BPM and time stamp into the buffer
    toMemBuff[1] = timeStamp; //   before placing the buffer's contents in memory
    placeInMemory();
     
  }

  sendECG(); // attempt to send any current ECG measurements to the phone

  if(timeInitiated){
    checkForSteps(); // see if any steps have been taken and, if so, process them
  }
  
  if (bleConnected) { // only consider sending data if we believe we're connected

    #ifdef nate
    if (sentSinceCheckin > timeToCheckin) {
      checkin(); // periodically, we check to make sure the phone hasn't gone out of
                 //   range without properly disconnecting
    }
    #endif
    
    if (blePeripheral.connected()) { // check if we're still connected to the phone

      if (caughtUp()) { // if we don't have a lot of data in memory, we send BPMs
        liveSend();     //   to the phone one at a time
        
      } else {          // if we have plenty of data in memory, we send BPMs to the
        batchSend();    //    phone in batches
      }

      liveSendStep();   // send any available step information to the phone
      
    } else { // If we realize the phone has disconnected, we call the function that          
      handleDisconnect(); // responds to the disconnection
    }
    
  } else { // If we're not currently connected to the phone, we attempt to connect
    tryToConnect();
  }
}

// attempt to connect to a BLE central device
void tryToConnect() {

  central = blePeripheral.central();
  
  if (central) { // when we've successfully connected to the phone
    #ifdef usb
    Serial.print("Connected to central: ");
    // print the central's MAC address:
    Serial.println(central.address());
    #endif

    #ifdef nate
    obtainInitTime();
    #endif

    while(!timeInitiated){
      delay(1);
    }
    
    digitalWrite(LED_PIN, HIGH); // turn on the connection light

    // wait a bit before sending data to the phone because otherwise
    // the initial batch of bluetooth packets you send will get dropped
    // on the floor
    delay(1000);
    bleConnected = true; 
  }
}

// called after the phone disconnects, ties up loose ends
void handleDisconnect() {
    safeToFill = false; // tells the ecg queue not to get filled
    bleConnected = false; // keep track of whether bluetooth is connected
    #ifdef usb
    Serial.println("Disconnected from central");
    //Serial.println(central.address());
    #endif
    digitalWrite(LED_PIN, LOW); // turns off connection light
}

// gets the current from the phone and uses it to sync the
// Arduino's internal clock up with reality 
void obtainInitTime() {
  
  // don't do anything until the phone gives you the current
  // time in epoch time
  while(!checkinChar.written()) {
    delay(1);
  }

  // get the time from the phone as a byte array and 
  // combine each byte into one 4-byte number
  const unsigned char *fromPhone;
  fromPhone = checkinChar.value(); 
  unsigned long ts0 = ((unsigned long) fromPhone[0]);
  unsigned long ts1 = ((unsigned long) fromPhone[1]) << 8;
  unsigned long ts2 = ((unsigned long) fromPhone[2]) << 16;
  unsigned long ts3 = ((unsigned long) fromPhone[3]) << 24;
  unsigned long initTime = ts0 | ts1 | ts2 | ts3;

  setTime(initTime); // set the time using the CurieTime library

  // until this has been set to true for the first time, no BPMs
  // can be recorded because their time stamps would be in 1970
  timeInitiated = true;

  #ifdef usb
  Serial.println("time has been set");
  #endif
}

// send BPMs and time stamps to the phone one at a time
void liveSend() {
  if (retrieveFromMemory()) { // only update the characteristic if new data was read
    unsigned long timeStamp = fromMemBuff[1]; // get the time stamp
    unsigned char ts0 = timeStamp & 0xff;         // get each byte from the time 
    unsigned char ts1 = (timeStamp >> 8) & 0xff;  // stamp separately so that 
    unsigned char ts2 = (timeStamp >> 16) & 0xff; // the time stamp can be sent 
    unsigned char ts3 = (timeStamp >> 24) & 0xff; // in a bluetooth compatible format
  
    #ifdef usb
    Serial.print(fromMemBuff[0]);
    Serial.print(", ");
    Serial.println(timeStamp);
    #endif
    
    // package the the BPM and time stamp, switching to big-endian
    unsigned char bpmCharArray[BYTES_PER_PCKG] 
      = { ts0, ts1, ts2, ts3, (unsigned char) fromMemBuff[0] };
    bpmChar.setValue(bpmCharArray, BYTES_PER_PCKG); // send them to the phone

    lastTimeSent = timeStamp;
    sentSinceCheckin += 1;
  } 
}

// sends steps to the phone one at a time
void liveSendStep() {
  
  if (retrieveFromMemoryStep()) { // only update the characteristic if 
                                  // new data was read

    // package the data and send it to the phone
    unsigned char stepCharArray[BYTES_PER_PCKG_STEP];
    int i;
    for (i = 0; i < BYTES_PER_PCKG_STEP; i++){
      stepCharArray[i] = fromMemBuffStep[i];
    }
    stepChar.setValue(stepCharArray, BYTES_PER_PCKG_STEP);

    #ifdef usb
    Serial.println("STEP package");
    #endif
  } 
}

// send BPMs and time stamps to the phone in packages of 4 at a time
void batchSend() {
  unsigned char batchCharArray[BATCH_SIZE];
  
  short i;
  for (i = 0; i < NUM_PCKGS; i++) {
    if (retrieveFromMemory()) {
      unsigned long timeStamp = fromMemBuff[1];// get the time stamp
      unsigned char ts0 = timeStamp & 0xff;     // get each byte from the time 
      unsigned char ts1 = (timeStamp >> 8) & 0xff;// stamp separately so that the 
      unsigned char ts2 = (timeStamp >> 16) & 0xff;// time stamp can be sent in 
      unsigned char ts3 = (timeStamp >> 24) & 0xff; // a bluetooth compatible format

      short offset = i * BYTES_PER_PCKG;

      #ifdef usb
      Serial.print(fromMemBuff[0]);
      Serial.print(", ");
      Serial.print(timeStamp);
      Serial.print('\t');
      #endif
      
      batchCharArray[offset] = ts0;
      batchCharArray[offset+1] = ts1;
      batchCharArray[offset+2] = ts2;
      batchCharArray[offset+3] = ts3;
      batchCharArray[offset+4] = (unsigned char) fromMemBuff[0];

    }
  }

  batchChar.setValue(batchCharArray, BATCH_SIZE); // send to the phone

  #ifdef usb
  Serial.println();
  #endif
}

// send ECG measurements to the phone to be graphed
void sendECG() {
  // check if there are at least 20 ECG measurements to print
  if (ecgQueue.count() >= NUM_ECGS) {
      
     // remove 20 ECG measurements from the queue and package them
     unsigned char ecgCharArray[NUM_ECGS];
     int i;
     for (i = 0; i < NUM_ECGS; i++) {
       ecgCharArray[i] = ecgQueue.dequeue();
     }
     
     // allow ECG measurements to be added to the queue   
     safeToFill = true; 
     
     // send the array of 20 ECG measurements to the phone to be graphed
     ecgChar.setValue(ecgCharArray, NUM_ECGS); 
                                         
     } else {    
      safeToFill = true; // allow ECG measurements to be added to the queue
     }
}

// ----------------------------------------------------------------------

// store BPMs and timestamps on the flash chip
void placeInMemory() {

  flashFiles[writeFileIndex].seek(nextToPlace); // move cursor
  flashFiles[writeFileIndex].write(toMemBuff, DSIZE); // write to chip
  nextToPlace += DSIZE; // increment the offset to be written to next

  // check if it's time to switch files
  if (nextToPlace >= FSIZE) {
    switchWriteFiles();
  }
}

// retrieve BPMs and time stamps from the flash chip to send to the phone
boolean retrieveFromMemory() {
                               
  if ((readFileIndex == writeFileIndex) 
      && (nextToRetrieve >= nextToPlace)) {
    return false; // if the read pointer has caught up to the write
                  // pointer, don't try to read any new data yet
  }
  flashFiles[readFileIndex].seek(nextToRetrieve); // move cursor
  flashFiles[readFileIndex].read(fromMemBuff, DSIZE); // read from chip
  nextToRetrieve += DSIZE; // increment the offset to be read from next

  // check if it's time to switch files
  if (nextToRetrieve >= FSIZE) {
    switchReadFiles();
  }
  return true;
}

// when one file in memory gets full, we erase the
//  next file and start writing to it
void switchWriteFiles() {

  // change the current file we're writing to
  writeFileIndex = (writeFileIndex + 1) % NUM_BUFFS;
  // erase it so we can write to it
  flashFiles[writeFileIndex].erase();
  // start writing to the beginning of this file
  nextToPlace = 0;

  #ifdef usb
  Serial.print("Switching WRITE file to ");
  Serial.println(writeFileIndex);
  #endif

  // if the file that's just been erased was the file
  // that the ack or the read pointer is currently on,
  // evict those pointers to the next file
  if (writeFileIndex == ackFileIndex) {
    switchAckFiles();
  }
  if (writeFileIndex == readFileIndex) {
    switchReadFiles();
  }
}

// evict the ack pointer to the next file
void switchAckFiles() {

  // change the current file we're reading from
  ackFileIndex = (ackFileIndex + 1) % NUM_BUFFS;
  // start reading from the beginning of the this file
  nextToAck = 0;

  #ifdef usb
  Serial.print("Switching ACK file to ");
  Serial.println(ackFileIndex);
  #endif
}

// when we've read everthing in one file, we 
//  start reading from the next one
void switchReadFiles() {

  // change the current file we're reading from
  readFileIndex = (readFileIndex + 1) % NUM_BUFFS;
  // start reading from the beginning of the this file
  nextToRetrieve = 0;

  #ifdef usb
  Serial.print("Switching READ file to ");
  Serial.println(readFileIndex);
  #endif
}

// Checks whether the read pointer is close enough to the write pointer 
// to necessitate sending data points one at a time instead of in batches
boolean caughtUp() {

  // calculate logical read and write indices 
  int logicalWriteIndex = (writeFileIndex * DATA_PER_FILE)
                            + (nextToPlace / DSIZE);
  int logicalReadIndex = (readFileIndex * DATA_PER_FILE)
                            + (nextToRetrieve / DSIZE);

  // figure out how many data units are between the write and read pointer
  int distance = ((logicalWriteIndex - logicalReadIndex) + STORAGE_LENGTH)
                   % STORAGE_LENGTH;

  // return true if there are at least enough data points left to fill a batch
  return (distance < NUM_PCKGS);
  
}

// ------------------------------------------------------------------------

void checkForSteps() {

  // if we're currently in the middle of an excursion...
  if (detectingSteps) {

    // get the latest step count from the step counter
    int latestCount = CurieIMU.getStepCount();
    // get the current time
    unsigned long currentTime = now();

    // if the step count has changed
    if (stepCount != latestCount) {

      // update the step count so it can be
      // displayed on the phone
      stepCount = latestCount;

      // update the last time that the 
      // person took a step
      stepEndTime = currentTime;

      // if the person had not previously been active,
      // we declare that they are now active and record
      // the time that they became active so that we
      // can later calculate how long they were active
      #ifdef actively
      if(!active){
        lastActive = currentTime;
        active = true;
      }
      #endif

      // if we're currently connected to the phone,
      // we send it the latest step count 
      if (bleConnected) {
        unsigned char sc0 = stepCount & 0xff;  
        unsigned char sc1 = (stepCount >> 8) & 0xff;
        unsigned char stepCountArr[2] = { sc0, sc1 };
        liveStepChar.setValue(stepCountArr, 2);
      }


    } else {

      // if no steps have been taken for a certain amount of time,
      // we stop recording "active" time
      #ifdef actively
      if (active && currentTime - lastActive > ACTIVE_THRESHOLD) {
        activeTime += (stepEndTime - lastActive);
        active = false;
      }
      #endif

      // if no steps have been taken for a certain amount of time, we
      // consider the excursion to be over
      if (currentTime - stepEndTime > MAX_SECS_BETWEEN_STEPS) {

        // calculate the length of the excursion
        unsigned long stepOffset = stepEndTime - stepStartTime;

        #ifdef usb
        Serial.print(stepStartTime);
        Serial.print(", ");
        Serial.print(stepOffset);
        Serial.print(", count: ");
        Serial.println(stepCount);
        #ifdef actively
        Serial.print("active time: ");
        Serial.println(activeTime);
        #endif
        #endif

        // send the start time, excursion length, number of
        // steps taken and optional active time to memory
        
        toMemBuffStep[0] = stepStartTime & 0xff;      
        toMemBuffStep[1] = (stepStartTime >> 8) & 0xff;  
        toMemBuffStep[2] = (stepStartTime >> 16) & 0xff;   
        toMemBuffStep[3] = (stepStartTime >> 24) & 0xff;

        toMemBuffStep[4] = stepOffset & 0xff;      
        toMemBuffStep[5] = (stepOffset >> 8) & 0xff;  

        toMemBuffStep[6] = stepCount & 0xff;      
        toMemBuffStep[7] = (stepCount >> 8) & 0xff; 

        #ifdef actively
        toMemBuffStep[8] = activeTime & 0xff; 
        toMemBuffStep[9] = (activeTime >> 8) & 0xff; 
        #endif
        
        placeInMemoryStep(); // store the excursion on the flash chip
        
        // reset the step count back to 0 for the next excursion
        CurieIMU.resetStepCount();
        stepCount = 0;
  
        // indicate that the excursion has concluded
        detectingSteps = false;
  
        #ifdef usb
        Serial.println("excursion ended");
        #endif
      }
    }

  // if we weren't currently in an excursion, check if it's time to
  // start a new one
  } else {
    
    if (CurieIMU.getStepCount() > 0) {
  
      // set the current time to the start time of the excursion
      stepStartTime = now();

      #ifdef actively
      activeTime = 0;
      lastActive = stepStartTime;
      active = true;
      #endif
  
      // indicate that an excursion has commenced
      detectingSteps = true;
  
      #ifdef usb
      Serial.println("excursion begun");
      #endif
    }
  }                                                  
}

// store steps and their time frames on the flash chip
void placeInMemoryStep() { 

  flashFilesStep[writeFileIndexStep].seek(nextToPlaceStep); // move cursor

  // write to the chip and increment the offset to be written to next
  flashFilesStep[writeFileIndexStep].write(toMemBuffStep, DSIZE_STEP); 
  nextToPlaceStep += DSIZE_STEP; 

  // check if it's time to switch files
  if (nextToPlaceStep >= FSIZE_STEP) {
    switchWriteFilesStep();
  }
}

// retrieve steps and time frames from the flash chip to send them to the phone
boolean retrieveFromMemoryStep() {
                               
  if ((readFileIndexStep == writeFileIndexStep) 
      && (nextToRetrieveStep >= nextToPlaceStep)) {
    return false; // if the read pointer has caught up to the write pointer,
                  // don't try to read any new data yet
  }
  // move cursor, read from chip, and increment the offset to be read from next
  flashFilesStep[readFileIndexStep].seek(nextToRetrieveStep);
  flashFilesStep[readFileIndexStep].read(fromMemBuffStep, DSIZE_STEP); 
  nextToRetrieveStep += DSIZE_STEP;

  // check if it's time to switch files
  if (nextToRetrieveStep >= FSIZE_STEP) {
    switchReadFilesStep();
  }
  return true;
}

// when one file in memory gets full, we erase the
//  next file and start writing to it
void switchWriteFilesStep() {

  // change the current file we're writing to
  writeFileIndexStep = (writeFileIndexStep + 1) % NUM_BUFFS_STEP;
  // erase it so we can write to it
  flashFilesStep[writeFileIndexStep].erase();
  // start writing to the beginning of this file
  nextToPlaceStep = 0;

  #ifdef usb
  Serial.print("Switching WRITE STEP file to ");
  Serial.println(writeFileIndexStep);
  #endif

  // if the file that's just been erased was the file
  // that the ack or the read pointer is currently on,
  // evict those pointers to the next file
  if (writeFileIndexStep == readFileIndexStep) {
    switchReadFilesStep();
  }
}

// when we've read everthing in one file, we 
//  start reading from the next one
void switchReadFilesStep() {

  // change the current file we're reading from
  readFileIndexStep = (readFileIndexStep + 1) % NUM_BUFFS_STEP;
  // start reading from the beginning of the this file
  nextToRetrieveStep = 0;

  #ifdef usb
  Serial.print("Switching READ STEP file to ");
  Serial.println(readFileIndexStep);
  #endif
}

// ----------------------------------------------------------------------------


// called periodically to make sure the phone is still connected
void checkin() {
  
  sentSinceCheckin = 0; // reset the time since the last checkin back to zero

  unsigned char ts0 = lastTimeSent & 0xff;     // get each byte from the 
  unsigned char ts1 = (lastTimeSent >> 8) & 0xff;// time stamp separately so that
  unsigned char ts2 = (lastTimeSent >> 16) & 0xff;// the time stamp can be sent in 
  unsigned char ts3 = (lastTimeSent >> 24) & 0xff;  // a bluetooth compatible format

  // send a time stamp to the phone
  unsigned char lastTimeArray[4] = { ts0, ts1, ts2, ts3 };  
  checkinChar.setValue(lastTimeArray, 4);

  // give the phone a second to respond
  delay(1000);

  // if the phone doesn't respond, it must have gone out of
  // range, we need to force a disconnect
  if (!checkinChar.written()) {

    if (blePeripheral.disconnect()) {
      #ifdef usb
      Serial.println("success");
      #endif
    } else {
      #ifdef usb
      Serial.println("failure");
      #endif
    }

    // move read pointer back toe the position after the last data
    // whose receipt was confirmed by the phone
    readFileIndex = ackFileIndex;
    nextToRetrieve = nextToAck;
    #ifdef usb
    Serial.println("Force disconnected"); 
    Serial.println("Reverting read pointer");
    #endif
    
  } else {
    // the phone will return a 1 if the last time stamp it's received
    // is greater than or equal to the time stamp I sent it and a 0
    // otherwise
    const unsigned char *fromPhone;
    fromPhone = checkinChar.value(); 

    // a 1 means we're caught up, so I can move the ack
    // pointer up to where the read pointer is
    if (((int)fromPhone[3]) == 1) {
      ackFileIndex = readFileIndex;
      nextToAck = nextToRetrieve;
      #ifdef usb
      Serial.println("Progressing ack pointer");
      #endif

    // a 0 means data has been lost, so I need to move the read pointer
    // back to the position after the last data whose receipt has been
    // confirmed so I can resend everything in between
    } else { 
      readFileIndex = ackFileIndex;
      nextToRetrieve = nextToAck;
      #ifdef usb
      Serial.println("Reverting read pointer");
      #endif
    } 
  }
}

// interrupt handler: 
// uses the QRS-detection algorithm to search for a new BPM
void updateHeartRate() { 

    // keeps track of whether it's time to update the BPM
    boolean QRS_detected = false; 
    
    // only read data if ECG chip has detected
    // that leads are attached to patient
    boolean leadsAreOn = (digitalRead(LEADS_OFF_PLUS_PIN) == 0)
                            && (digitalRead(LEADS_OFF_MINUS_PIN) == 0);
    if (leadsAreOn) {     
           
      // read next ECG data point
      int next_ecg_pt = analogRead(ECG_PIN);
      
      if (bleConnected && safeToFill) {
        ecgQCount++;

        // we only need to send every nth ECG value to the phone
        // a lower value in this loop means a higher resolution
        // for the graph on the phone
        if (ecgQCount > 2 && ecgQueue.count() < 70) { 
                           
          // scale each measurement to fit on the graph
          int ecg = map(next_ecg_pt, 0, 1023, 0, 100);
          ecgQueue.enqueue(ecg); // add each measurement to the queue
          ecgQCount = 0;
        }
      }
      
      // give next data point to algorithm
      QRS_detected = detect(next_ecg_pt);
            
      if (QRS_detected == true) {
        
        found_time_micros = micros();

        // use the time between when the last 
        // two peaks were detected to calculate BPM
        
        bpm_buff[bpm_buff_WR_idx] = (60.0 / 
           (((float) (found_time_micros - old_found_time_micros)) / 1000000.0));
        bpm_buff_WR_idx++;
        bpm_buff_WR_idx %= BPM_BUFFER_SIZE;
        bpm += bpm_buff[bpm_buff_RD_idx];
    
        tmp = bpm_buff_RD_idx - BPM_BUFFER_SIZE + 1;
        if (tmp < 0) tmp += BPM_BUFFER_SIZE;

        if (timeInitiated) {
          // sends the current average BPM to the queue to be printed
          bpmQueue.enqueue((unsigned long) (bpm/BPM_BUFFER_SIZE));
          bpmQueue.enqueue(now());
        }
    
        bpm -= bpm_buff[tmp];
        
        bpm_buff_RD_idx++;
        bpm_buff_RD_idx %= BPM_BUFFER_SIZE;

        old_found_time_micros = found_time_micros;

      }
    }
}

// Portion pertaining to Pan-Tompkins QRS detection -----------------------

// circular buffer for input ecg signal
// we need to keep a history of M + 1 samples for HP filter
float ecg_buff[M + 1] = {0};
int ecg_buff_WR_idx = 0;
int ecg_buff_RD_idx = 0;

// circular buffer for input ecg signal
// we need to keep a history of N+1 samples for LP filter
float hp_buff[N + 1] = {0};
int hp_buff_WR_idx = 0;
int hp_buff_RD_idx = 0;

// LP filter outputs a single point for every input point
// This goes straight to adaptive filtering for eval
float next_eval_pt = 0;

// running sums for HP and LP filters, values shifted in FILO
float hp_sum = 0;
float lp_sum = 0;

// working variables for adaptive thresholding
float threshold = 0;
boolean triggered = false;
int trig_time = 0;
float win_max = 0;
int win_idx = 0;

// number of starting iterations, used to
// determine when moving windows are filled
int number_iter = 0;

boolean detect(float new_ecg_pt) {
  
  // copy new point into circular buffer, increment index
  ecg_buff[ecg_buff_WR_idx++] = new_ecg_pt;  
  ecg_buff_WR_idx %= (M+1);

 
  /* High pass filtering */
  
  if (number_iter < M) {
    // first fill buffer with enough points for HP filter
    hp_sum += ecg_buff[ecg_buff_RD_idx];
    hp_buff[hp_buff_WR_idx] = 0;
    
  } else {
    hp_sum += ecg_buff[ecg_buff_RD_idx];
    
    tmp = ecg_buff_RD_idx - M;
    if (tmp < 0) tmp += M + 1;
    
    hp_sum -= ecg_buff[tmp];
    
    float y1 = 0;
    float y2 = 0;
    
    tmp = (ecg_buff_RD_idx - ((M+1)/2));
    if (tmp < 0) tmp += M + 1;
    
    y2 = ecg_buff[tmp];
    
    y1 = HP_CONSTANT * hp_sum;
    
    hp_buff[hp_buff_WR_idx] = y2 - y1;
  }
  
  // done reading ECG buffer, increment position
  ecg_buff_RD_idx++;
  ecg_buff_RD_idx %= (M+1);
  
  // done writing to HP buffer, increment position
  hp_buff_WR_idx++;
  hp_buff_WR_idx %= (N+1);
  

  /* Low pass filtering */
  
  // shift in new sample from high pass filter
  lp_sum += hp_buff[hp_buff_RD_idx] * hp_buff[hp_buff_RD_idx];
  
  if (number_iter < N) {
    // first fill buffer with enough points for LP filter
    next_eval_pt = 0;
    
  } else {
    // shift out oldest data point
    tmp = hp_buff_RD_idx - N;
    if (tmp < 0) tmp += (N+1);
    
    lp_sum -= hp_buff[tmp] * hp_buff[tmp];
    
    next_eval_pt = lp_sum;
  }
  
  // done reading HP buffer, increment position
  hp_buff_RD_idx++;
  hp_buff_RD_idx %= (N+1);
  

  /* Adapative thresholding beat detection */
  // set initial threshold        
  if (number_iter < win_size) {
    if (next_eval_pt > threshold) {
      threshold = next_eval_pt;
    }
    // only increment number_iter iff it is less than win_size
    // if it is bigger, then the counter serves no further purpose
    number_iter++;
  }
  
  // check if detection hold off period has passed
  if (triggered == true) {
    trig_time++;
    
    if (trig_time >= 100) {
      triggered = false;
      trig_time = 0;
    }
  }
  
  // find if we have a new max
  if (next_eval_pt > win_max) win_max = next_eval_pt;
  
  // find if we are above adaptive threshold
  if (next_eval_pt > threshold && !triggered) {
    triggered = true;

    return true;
  }
  // else we'll finish the function before returning FALSE,
  // to potentially change threshold
          
  // adjust adaptive threshold using max of signal found 
  // in previous window            
  if (win_idx++ >= win_size) {
    
    // weighting factor for determining the contribution of
    // the current peak value to the threshold adjustment
    float gamma = 0.175;
    
    // forgetting factor - rate at which we forget old observations
    // choose a random value between 0.01 and 0.1 for this, 
    float alpha = 0.01 + (((float) random(0, RAND_RES) 
                              / (float) (RAND_RES)) * ((0.1 - 0.01)));
    
    // compute new threshold
    threshold = alpha * gamma * win_max + (1 - alpha) * threshold;
    
    // reset current window index
    win_idx = 0;
    win_max = -10000000;
  }
      
  // return false if we didn't detect a new QRS
  return false;
    
}
