#include <Arduino.h>
#include <LoRa.h>
#include <APRS-Decoder.h>
#include <TinyGPS++.h>
#include <TimeLib.h>
#include <WiFi.h>
#include "display.h"
#include "pins.h"
#include "power_management.h"
#include "configuration.h"
Configuration Config;

#include "power_management.h"
PowerManagement powerManagement;

#include "logger.h"

HardwareSerial ss(1);
TinyGPSPlus gps;

void setup_lora();
void setup_gps();
void load_config();

String create_lat_aprs(RawDegrees lat);
String create_long_aprs(RawDegrees lng);
String createDateString(time_t t);
String createTimeString(time_t t);
String getSmartBeaconState();

static bool send_update = true;

// Initial lat/lng pos, change to your base station coordnates
float lastTxLat = 0;
float lastTxLng = 0;
float lastTxdistance;
unsigned long txInterval = 60000L;  // Initial 60 secs internal
int previousHeading, currentHeading, headingDelta, headingTresh = 0;
int lastTxTime = millis();

// cppcheck-suppress unusedFunction
void setup()
{
	Serial.begin(115200);
	

#ifdef TTGO_T_Beam_V1_0
	Wire.begin(SDA, SCL);
	if (!powerManagement.begin(Wire))
	{
		Serial.println("[INFO] AXP192 init done!");
	}
	else
	{
		Serial.println("[ERROR] AXP192 init failed!");
	}
	powerManagement.activateLoRa();
	powerManagement.activateOLED();
	powerManagement.activateGPS();
	powerManagement.activateMeasurement();
#endif
	
	delay(500);
	Serial.println("[INFO] LoRa APRS Tracker by OE5BPA (Peter Buchegger)");
	setup_display();
	
	show_display("OE5BPA", "LoRa APRS Tracker", "by Peter Buchegger", "[INFO] Smart Beacon is " + getSmartBeaconState(), 2000);
    load_config();
	
	setup_gps();
	setup_lora();

	// make sure wifi and bt is off as we don't need it:
	WiFi.mode(WIFI_OFF);
	btStop();

    Serial.println("[INFO] Smart Beacon is " + getSmartBeaconState());
	Serial.println("[INFO] setup done...");
	delay(500);
	
}

// cppcheck-suppress unusedFunction
void loop()
{
if (Config.debug == false)
	while (ss.available() > 0)
	{
		char c = ss.read();
		//Serial.print(c);
		gps.encode(c);
	}
else
{
	while (Serial.available() > 0)
	{
		char c = Serial.read();
		//Serial.print(c);
		gps.encode(c);
	}
}

	bool gps_time_update = gps.time.isUpdated();
	bool gps_loc_update = gps.location.isUpdated();
	static time_t nextBeaconTimeStamp = -1;

	if(gps.time.isValid())
	{
		setTime(gps.time.hour(),gps.time.minute(),gps.time.second(),gps.date.day(),gps.date.month(),gps.date.year());

		if (nextBeaconTimeStamp <= now() || nextBeaconTimeStamp == -1)
		{
			send_update = true;
		}

	}

  if (Config.smart_beacon.active)
  {
	send_update = false;
  	if ( gps_loc_update )
	{
    	lastTxdistance = TinyGPSPlus::distanceBetween(
        	gps.location.lat(),
       		gps.location.lng(),
        	lastTxLat,
        	lastTxLng
		);
          
   		// Get headings and heading delta
    	currentHeading = (int) gps.course.deg();
    	if ( currentHeading >= 180 )
		{
    		currentHeading = currentHeading-180;
    	}
      	headingDelta = (int) ( previousHeading - currentHeading ) % 360;
		int turn_slope = 100;
		headingTresh = (float) Config.smart_beacon.turn_min + turn_slope / gps.speed.kmph();

		int lastTx = millis() - lastTxTime;
		
		if ( lastTx > Config.smart_beacon.min_bcn * 1000)
		{
			//send_update = true;
			// Check for heading more than 25 degrees
			if ( (headingDelta < -Config.smart_beacon.turn_min || headingDelta > Config.smart_beacon.turn_min) && lastTxdistance > Config.smart_beacon.min_tx_dist )
			{
				send_update = true;
			}
		}

		if ( lastTx >= txInterval )
		{
			// Trigger Tx Tracker when Tx interval is reach 
			// Will not Tx if stationary bcos speed < 5 and lastTxDistance < 20
			if ( lastTxdistance > 20 )
			{
				send_update = true;
			}
		}
	}
  }

	if(send_update && gps.location.isValid() && gps_loc_update)
	{
	#ifdef 	TTGO_T_Beam_V1_0
		String volts = "Bat: " + powerManagement.getBatteryVoltageStr() + " V";
        String amps = "Cur:" + powerManagement.getBatteryChargeDischargeCurrentStr() + " mA";
		String sats = String("Sats: ") + gps.satellites.value();
		
	#endif	
		powerManagement.deactivateMeasurement();
		
		nextBeaconTimeStamp = now() + (Config.beacon.timeout * SECS_PER_MIN);
		String nextbcnStr = String("Nxt Bcn: ") + createTimeString(nextBeaconTimeStamp);
		send_update = false;

		APRSMessage msg;
		msg.setSource(Config.callsign);
		msg.setDestination("APLT0");
		String lat = create_lat_aprs(gps.location.rawLat());
		String lng = create_long_aprs(gps.location.rawLng());
		if (Config.enable_data_injection)
		{
		msg.getAPRSBody()->setData(String("=") + lat + Config.beacon.overlay + lng + Config.beacon.symbol + Config.beacon.message  + " - " + volts + " " + amps + " " + sats + " {data} ");
		}
		else
		{
		msg.getAPRSBody()->setData(String("=") + lat + Config.beacon.overlay + lng + Config.beacon.symbol + Config.beacon.message  + " - " + volts + " " + amps + " " + sats);	
		}
		String data = msg.encode();
		Serial.println(data);
		show_display("<< TX >>", data);
		LoRa.beginPacket();
		// Header:
		LoRa.write('<');
		LoRa.write(0xFF);
		LoRa.write(0x01);
		// APRS Data:
		LoRa.write((const uint8_t *)data.c_str(), data.length());
		LoRa.endPacket();
		powerManagement.activateMeasurement();

		if (Config.smart_beacon.active)
  		{
			lastTxLat = gps.location.lat();
			lastTxLng = gps.location.lng();
			previousHeading = currentHeading;
			lastTxdistance = 0;
			lastTxTime = millis();
    	}
	}

	if(gps_time_update)
	{
#ifdef TTGO_T_Beam_V1_0
		String batteryVoltage(powerManagement.getBatteryVoltage(), 2);
		String batteryChargeCurrent(powerManagement.getBatteryChargeDischargeCurrent(), 0);
#endif

		show_display(Config.callsign,
			createDateString(now()) + " " + createTimeString(now()),
			String("Sats: ") + gps.satellites.value() + " HDOP: " + gps.hdop.hdop(),
			String("Nxt Bcn: ") + createTimeString(nextBeaconTimeStamp)
#ifdef TTGO_T_Beam_V1_0
			, String("Bat: ") + batteryVoltage + "V " + batteryChargeCurrent + "mA"
#endif
			, String("Smart Beacon is " + getSmartBeaconState())
			);

if (Config.smart_beacon.active)
  		{
			// Change the Tx internal based on the current speed
			if ( gps.speed.kmph() < 5 )
			{
    			txInterval = 300000;         // Change Tx internal to 5 mins
    		}
			else if ( gps.speed.kmph() < Config.smart_beacon.slow_speed )
			{
    			txInterval = Config.smart_beacon.slow_rate * 1000;          // Change Tx interval
    		}
			else if ( gps.speed.kmph() > Config.smart_beacon.fast_speed)
			{
    		    txInterval = Config.smart_beacon.fast_rate * 1000;          // Change Tx interval
    		}
			else
			{
    			// Interval inbetween low and high speed 
    		    txInterval = (Config.smart_beacon.fast_speed / gps.speed.kmph()) * Config.smart_beacon.fast_rate * 1000;
    		}
		}

	}

   
	if ((Config.debug == false) && (millis() > 5000 && gps.charsProcessed() < 10))
	{
		Serial.println("No GPS detected!");
	}



}

void setup_lora()
{
	Serial.println("[INFO] Set SPI pins!");
	SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
	LoRa.setPins(LORA_CS, LORA_RST, LORA_IRQ);
	Serial.println("[INFO] Set LoRa pins!");

	long freq = Config.lora.frequencyTx;
	Serial.print("[INFO] frequency: ");
	Serial.println(freq);
	if (!LoRa.begin(freq)) {
		Serial.println("[ERROR] Starting LoRa failed!");
		show_display("ERROR", "Starting LoRa failed!");
		while (1);
	}
	LoRa.setFrequency(Config.lora.frequencyTx);
	LoRa.setSpreadingFactor(Config.lora.spreadingFactor);
	LoRa.setSignalBandwidth(Config.lora.signalBandwidth);
	LoRa.setCodingRate4(Config.lora.codingRate4);
	LoRa.enableCrc();

	LoRa.setTxPower(20);
	Serial.println("[INFO] LoRa init done!");
	show_display("INFO", "LoRa init done!", 2000);
}

void setup_gps()
{
	ss.begin(9600, SERIAL_8N1, GPS_TX, GPS_RX);
}

void load_config()
{
	ConfigurationManagement confmg("/is-cfg.json");
	Config = confmg.readConfiguration();
	if(Config.callsign == "NOCALL-10")
	{
		logPrintlnE("You have to change your settings in 'data/is-cfg.json' and upload it via \"Upload File System image\"!");
		show_display("ERROR", "You have to change your settings in 'data/is-cfg.json' and upload it via \"Upload File System image\"!");
		while (true)
		{}
	}

#ifndef ETH_BOARD
	if(Config.aprs_is.active && !Config.wifi.active)
	{
		logPrintlnE("You have to activate Wifi for APRS IS to work, please check your settings!");
		show_display("ERROR", "You have to activate Wifi for APRS IS to work, please check your settings!");
		while (true)
		{}
	}
#endif
}
String create_lat_aprs(RawDegrees lat)
{
	char str[20];
	char n_s = 'N';
	if(lat.negative)
	{
		n_s = 'S';
	}
	sprintf(str, "%02d%05.2f%c", lat.deg, lat.billionths / 1000000000.0 * 60.0, n_s);
	String lat_str(str);
	return lat_str;
}

String create_long_aprs(RawDegrees lng)
{
	char str[20];
	char e_w = 'E';
	if(lng.negative)
	{
		e_w = 'W';
	}
	sprintf(str, "%03d%05.2f%c", lng.deg, lng.billionths / 1000000000.0 * 60.0, e_w);
	String lng_str(str);
	return lng_str;
}

String createDateString(time_t t)
{
	char line[30];
	sprintf(line, "%02d.%02d.%04d", day(t), month(t), year(t));
	return String(line);
}

String createTimeString(time_t t)
{
	if(t == -1)
	{
		return String("00:00:00");
	}
	char line[30];
	sprintf(line, "%02d:%02d:%02d", hour(t), minute(t), second(t));
	return String(line);
}

String getSmartBeaconState()
{
  String   sm_beaconstate = "";
	if (Config.smart_beacon.active == true)
	{
		sm_beaconstate = "On";
	}
	else
	{
		sm_beaconstate = "Off";
	}
	return sm_beaconstate;
}
