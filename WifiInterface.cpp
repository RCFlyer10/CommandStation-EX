/*
    © 2020, Chris Harlow. All rights reserved.
    © 2020, Harald Barth.

    This file is part of CommandStation-EX

    This is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    It is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with CommandStation.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "WifiInterface.h"        /* config.h included there */
#include <avr/pgmspace.h>
#include "DIAG.h"
#include "StringFormatter.h"

#include "WifiInboundHandler.h"

const char  FLASH READY_SEARCH[]  = "\r\nready\r\n";
const char  FLASH OK_SEARCH[] = "\r\nOK\r\n";
const char  FLASH END_DETAIL_SEARCH[] = "@ 1000";
const char  FLASH SEND_OK_SEARCH[] = "\r\nSEND OK\r\n";
const char  FLASH IPD_SEARCH[] = "+IPD";
const unsigned long LOOP_TIMEOUT = 2000;
bool WifiInterface::connected = false;
Stream * WifiInterface::wifiStream;

#ifndef WIFI_CONNECT_TIMEOUT
// Tested how long it takes to FAIL an unknown SSID on firmware 1.7.4.
#define WIFI_CONNECT_TIMEOUT 14000
#endif

////////////////////////////////////////////////////////////////////////////////
//
// Figure out number of serial ports depending on hardware
//
#if defined(ARDUINO_AVR_UNO) || defined(ARDUINO_AVR_NANO)
#define NUM_SERIAL 0
#endif
 
#if (defined(ARDUINO_AVR_MEGA) || defined(ARDUINO_AVR_MEGA2560))
#define NUM_SERIAL 3
#endif

#ifndef NUM_SERIAL
#define NUM_SERIAL 1
#endif

bool WifiInterface::setup(long serial_link_speed, 
                          const FSH *wifiESSID,
                          const FSH *wifiPassword,
                          const FSH *hostname,
                          const int port) {

  wifiSerialState wifiUp = WIFI_NOAT;

#if NUM_SERIAL == 0
  // no warning about unused parameters. 
  (void) serial_link_speed;
  (void) wifiESSID;
  (void) wifiPassword;
  (void) hostname;
  (void) port;
#endif  
  
#if NUM_SERIAL > 0
  Serial1.begin(serial_link_speed);
  wifiUp = setup(Serial1, wifiESSID, wifiPassword, hostname, port);
#endif

// Other serials are tried, depending on hardware.
#if NUM_SERIAL > 1
  if (wifiUp == WIFI_NOAT)
  {
    Serial2.begin(serial_link_speed);
    wifiUp = setup(Serial2, wifiESSID, wifiPassword, hostname, port);
  }
#endif
  
#if NUM_SERIAL > 2
  if (wifiUp == WIFI_NOAT)
  {
    Serial3.begin(serial_link_speed);
    wifiUp = setup(Serial3, wifiESSID, wifiPassword, hostname, port);
  }
#endif

  if (wifiUp == WIFI_NOAT) // here and still not AT commands found
      return false;

  DCCEXParser::setAtCommandCallback(ATCommand);
  // CAUTION... ONLY CALL THIS ONCE 
  WifiInboundHandler::setup(wifiStream);
  if (wifiUp == WIFI_CONNECTED)
      connected = true;
  else
      connected = false;
  return connected; 
}

wifiSerialState WifiInterface::setup(Stream & setupStream,  const FSH* SSid, const FSH* password,
				     const FSH* hostname,  int port) {
  wifiSerialState wifiState;
  static uint8_t ntry = 0;
  ntry++;

  wifiStream = &setupStream;

  DIAG(F("\n++ Wifi Setup Try %d ++\n"), ntry);

  wifiState = setup2( SSid, password, hostname,  port);

  if (wifiState == WIFI_NOAT) {
      DIAG(F("\n++ Wifi Setup NO AT ++\n"));
      return wifiState;
  }
 
  if (wifiState == WIFI_CONNECTED) {
    StringFormatter::send(wifiStream, F("ATE0\r\n")); // turn off the echo 
    checkForOK(200, OK_SEARCH, true);      
  }

    
  DIAG(F("\n++ Wifi Setup %S ++\n"), wifiState == WIFI_CONNECTED ? F("CONNECTED") : F("DISCONNECTED"));
  return wifiState;
}

#ifdef DONT_TOUCH_WIFI_CONF
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
wifiSerialState WifiInterface::setup2(const FSH* SSid, const FSH* password,
				      const FSH* hostname, int port) {
  bool ipOK = false;
  bool oldCmd = false;

  char macAddress[17];  //  mac address extraction   
     
  // First check... Restarting the Arduino does not restart the ES. 
  //  There may alrerady be a connection with data in the pipeline.
  // If there is, just shortcut the setup and continue to read the data as normal.
  if (checkForOK(200,IPD_SEARCH, true)) {
    DIAG(F("\nPreconfigured Wifi already running with data waiting\n"));
   // loopstate=4;  // carry on from correct place... or not as the case may be  
    return WIFI_CONNECTED; 
  }

  StringFormatter::send(wifiStream, F("AT\r\n"));   // Is something here that understands AT?
  if(!checkForOK(200, OK_SEARCH, true))
    return WIFI_NOAT;                               // No AT compatible WiFi module here

  StringFormatter::send(wifiStream, F("ATE1\r\n")); // Turn on the echo, se we can see what's happening
  checkForOK(2000, OK_SEARCH, true);                // Makes this visible on the console

  // Display the AT version information
  StringFormatter::send(wifiStream, F("AT+GMR\r\n")); 
  checkForOK(2000, OK_SEARCH, true, false);      // Makes this visible on the console

#ifdef DONT_TOUCH_WIFI_CONF
  DIAG(F("\nDONT_TOUCH_WIFI_CONF was set: Using existing config\n"));
#else
  StringFormatter::send(wifiStream, F("AT+CWMODE=1\r\n")); // configure as "station" = WiFi client
  checkForOK(1000, OK_SEARCH, true);                       // Not always OK, sometimes "no change"

  // Older ES versions have AT+CWJAP, newer ones have AT+CWJAP_CUR and AT+CWHOSTNAME
  StringFormatter::send(wifiStream, F("AT+CWJAP?\r\n"));
  if (checkForOK(2000, OK_SEARCH, true)) {
      oldCmd=true;
      while (wifiStream->available()) StringFormatter::printEscape( wifiStream->read()); /// THIS IS A DIAG IN DISGUISE
  }

  const char *yourNetwork = "Your network ";
  if (strncmp_P(yourNetwork, (const char*)SSid, 13) == 0 || strncmp_P("", (const char*)SSid, 13) == 0) {
    if (strncmp_P(yourNetwork, (const char*)password, 13) == 0) {
      // If the source code looks unconfigured, check if the
      // ESP8266 is preconfigured in station mode.
      // We check the first 13 chars of the SSid and the password

      // give a preconfigured ES8266 a chance to connect to a router
      // typical connect time approx 7 seconds
      delay(8000);
      StringFormatter::send(wifiStream, F("AT+CIFSR\r\n"));
      if (checkForOK(5000, (const char*) F("+CIFSR:STAIP"), true,false))
	  if (!checkForOK(1000, (const char*) F("0.0.0.0"), true,false))
	      ipOK = true;
    }
  } else {
      // SSID was configured, so we assume station (client) mode.
      if (oldCmd) {
	// AT command early version supports CWJAP/CWSAP
	StringFormatter::send(wifiStream, F("AT+CWJAP=\"%S\",\"%S\"\r\n"), SSid, password);
	ipOK = checkForOK(WIFI_CONNECT_TIMEOUT, OK_SEARCH, true);
      } else {
      // later version supports CWJAP_CUR
        StringFormatter::send(wifiStream, F("AT+CWHOSTNAME=\"%S\"\r\n"), hostname); // Set Host name for Wifi Client
	checkForOK(2000, OK_SEARCH, true); // dont care if not supported
      
        StringFormatter::send(wifiStream, F("AT+CWJAP_CUR=\"%S\",\"%S\"\r\n"), SSid, password);
	ipOK = checkForOK(WIFI_CONNECT_TIMEOUT, OK_SEARCH, true);
      }

      if (ipOK) {
	// But we really only have the ESSID and password correct
        // Let's check for IP (via DHCP)
        ipOK = false;
	StringFormatter::send(wifiStream, F("AT+CIFSR\r\n"));
	if (checkForOK(5000, (const char*) F("+CIFSR:STAIP"), true,false))
	  if (!checkForOK(1000, (const char*) F("0.0.0.0"), true,false))
	    ipOK = true;
      }
  }

  if (!ipOK) {
    // If we have not managed to get this going in station mode, go for AP mode

//    StringFormatter::send(wifiStream, F("AT+RST\r\n"));
//    checkForOK(1000, OK_SEARCH, true); // Not always OK, sometimes "no change"

    int i=0;
    do {
      // configure as AccessPoint. Try really hard as this is the
      // last way out to get any Wifi connectivity.
      StringFormatter::send(wifiStream, F("AT+CWMODE=2\r\n")); 
    } while (!checkForOK(1000+i*500, OK_SEARCH, true) && i++<10);

    while (wifiStream->available()) StringFormatter::printEscape( wifiStream->read()); /// THIS IS A DIAG IN DISGUISE

    // Figure out MAC addr
    StringFormatter::send(wifiStream, F("AT+CIFSR\r\n")); // not TOMATO
    // looking fpr mac addr eg +CIFSR:APMAC,"be:dd:c2:5c:6b:b7"
    if (checkForOK(5000, (const char*) F("+CIFSR:APMAC,\""), true,false)) {
      // Copy 17 byte mac address
      for (int i=0; i<17;i++) {
        while(!wifiStream->available());
	macAddress[i]=wifiStream->read();
	StringFormatter::printEscape(macAddress[i]);
      }
    } else {
	memset(macAddress,'f',sizeof(macAddress));
    }
    char macTail[]={macAddress[9],macAddress[10],macAddress[12],macAddress[13],macAddress[15],macAddress[16],'\0'};

    while (wifiStream->available()) StringFormatter::printEscape( wifiStream->read()); /// THIS IS A DIAG IN DISGUISE

    i=0;
    do {
      if (strncmp_P(yourNetwork, (const char*)password, 13) == 0) {
	// unconfigured
        StringFormatter::send(wifiStream, F("AT+CWSAP%s=\"DCCEX_%s\",\"PASS_%s\",1,4\r\n"), oldCmd ? "" : "_CUR", macTail, macTail);
      } else {
        // password configured by user
	StringFormatter::send(wifiStream, F("AT+CWSAP%s=\"DCCEX_%s\",\"%S\",1,4\r\n"), oldCmd ? "" : "_CUR", macTail, password);
      }
    } while (!checkForOK(WIFI_CONNECT_TIMEOUT, OK_SEARCH, true) && i++<2); // do twice if necessary but ignore failure as AP mode may still be ok
    if (i >= 2)
	DIAG(F("\nWarning: Setting AP SSID and password failed\n"));       // but issue warning

    if (!oldCmd) {
      StringFormatter::send(wifiStream, F("AT+CIPRECVMODE=0\r\n"), port); // make sure transfer mode is correct
      checkForOK(2000, OK_SEARCH, true);
    }
  }

  StringFormatter::send(wifiStream, F("AT+CIPSERVER=0\r\n")); // turn off tcp server (to clean connections before CIPMUX=1)
  checkForOK(1000, OK_SEARCH, true); // ignore result in case it already was off
   
  StringFormatter::send(wifiStream, F("AT+CIPMUX=1\r\n")); // configure for multiple connections
  if (!checkForOK(1000, OK_SEARCH, true)) return WIFI_DISCONNECTED;

  StringFormatter::send(wifiStream, F("AT+CIPSERVER=1,%d\r\n"), port); // turn on server on port
  if (!checkForOK(1000, OK_SEARCH, true)) return WIFI_DISCONNECTED;
#endif //DONT_TOUCH_WIFI_CONF
 
  StringFormatter::send(wifiStream, F("AT+CIFSR\r\n")); // Display  ip addresses to the DIAG 
  if (!checkForOK(1000, (const char *)F("IP,\"") , true, false)) return WIFI_DISCONNECTED;
  // Copy the IP address
  {
    const byte MAX_IP_LENGTH=15;
    char ipString[MAX_IP_LENGTH+1];
    byte ipLen=0;
    for(byte ipLen=0;ipLen<MAX_IP_LENGTH;ipLen++) {
      while(!wifiStream->available());
      int ipChar=wifiStream->read();
      StringFormatter::printEscape(ipChar);
      if (ipChar=='"') {
        ipString[ipLen]='\0';
        break;
      }
      ipString[ipLen]=ipChar;
    }
    LCD(4,F("%s"),ipString);  // There is not enough room on some LCDs to put a title to this      
  }
  // suck up anything after the IP. 
  if (!checkForOK(1000, OK_SEARCH, true, false)) return WIFI_DISCONNECTED;
  LCD(5,F("PORT=%d\n"),port);
   
  return WIFI_CONNECTED;
}
#ifdef DONT_TOUCH_WIFI_CONF
#pragma GCC diagnostic pop
#endif


// This function is used to allow users to enter <+ commands> through the DCCEXParser
// Once the user has made whatever changes to the AT commands, a <+X> command can be used
// to force on the connectd flag so that the loop will start picking up wifi traffic.
// If the settings are corrupted <+RST> will clear this and then you must restart the arduino.
 
void WifiInterface::ATCommand(const byte * command) {
  command++;
  if (*command=='X') {
     connected = true;
     DIAG(F("\n++++++ Wifi Connction forced on ++++++++\n"));
  }
  else {
        StringFormatter::  send(wifiStream, F("AT+%s\r\n"), command);
        checkForOK(10000, OK_SEARCH, true);
  }
}



bool WifiInterface::checkForOK( const unsigned int timeout, const char * waitfor, bool echo, bool escapeEcho) {
  unsigned long  startTime = millis();
  char  const *locator = waitfor;
  DIAG(F("\nWifi Check: [%E]"), waitfor);
  while ( millis() - startTime < timeout) {
    while (wifiStream->available()) {
      int ch = wifiStream->read();
      if (echo) {
        if (escapeEcho) StringFormatter::printEscape( ch); /// THIS IS A DIAG IN DISGUISE
        else DIAG(F("%c"), ch); 
      }
      if (ch != GETFLASH(locator)) locator = waitfor;
      if (ch == GETFLASH(locator)) {
        locator++;
        if (!GETFLASH(locator)) {
          DIAG(F("\nFound in %dms"), millis() - startTime);
          return true;
        }
      }
    }
  }
  DIAG(F("\nTIMEOUT after %dms\n"), timeout);
  return false;
}


void WifiInterface::loop() {
  if (connected) {
    WifiInboundHandler::loop(); 
  }
}
