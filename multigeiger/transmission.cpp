// measurements data transmission related code
// - via WiFi to internet servers
// - via LoRa to TTN (to internet servers)

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include "log.h"
#include "display.h"
#include "userdefines.h"
#include "webconf.h"
#include "loraWan.h"

#include "transmission.h"

#include "ca_certs.h"
#include "utils.h"
// Hosts for data delivery

// use http for now, could we use https?
#define MADAVI "http://api-rrd.madavi.de/data.php"

// use http for now, server operator tells there are performance issues with https.
#define SENSORCOMMUNITY "http://api.sensor.community/v1/push-sensor-data/"

// Send http(s) post requests to a custom server
// Note: Custom toilet URLs from https://ptsv2.com/ can be used for debugging
// and work with https and http.
#define CUSTOMSRV "https://ptsv2.com/t/xxxxx-yyyyyyyyyy/post"
// Get your own toilet URL and put it here before setting this to true.
#define SEND2CUSTOMSRV false
extern uint16_t influxPort;
static String http_software_version;
static unsigned int lora_software_version;
static String chipID;
static bool isLoraBoard;

typedef struct https_client {
  WiFiClientSecure *wc;
  HTTPClient *hc;
} HttpsClient;

static HttpsClient c_madavi, c_sensorc, c_customsrv, c_influxsrv;

void setup_transmission(const char *version, char *ssid, bool loraHardware) {
  chipID = String(ssid);
  chipID.replace("ESP32", "esp32");
  isLoraBoard = loraHardware;

  http_software_version = String(version);

  if (isLoraBoard) {
    int major, minor, patch;
    sscanf(version, "V%d.%d.%d", &major, &minor, &patch);
    lora_software_version = (major << 12) + (minor << 4) + patch;
    setup_lorawan();
  }

  c_madavi.wc = new WiFiClientSecure;
  c_madavi.wc->setCACert(ca_certs);
  c_madavi.hc = new HTTPClient;

  c_sensorc.wc = new WiFiClientSecure;
  c_sensorc.wc->setCACert(ca_certs);
  c_sensorc.hc = new HTTPClient;

  c_customsrv.wc = new WiFiClientSecure;
  c_customsrv.wc->setCACert(ca_certs);
  c_customsrv.hc = new HTTPClient;

  c_influxsrv.wc = new WiFiClientSecure;
  c_influxsrv.wc->setCACert(ca_certs);
  c_influxsrv.hc = new HTTPClient;

  set_status(STATUS_SCOMM, sendToCommunity ? ST_SCOMM_INIT : ST_SCOMM_OFF);
  set_status(STATUS_MADAVI, sendToMadavi ? ST_MADAVI_INIT : ST_MADAVI_OFF);
  set_status(STATUS_TTN, sendToLora ? ST_TTN_INIT : ST_TTN_OFF);
  set_status(STATUS_INFLUX, sendToInflux ? ST_INFLUX_INIT : ST_INFLUX_OFF);
}

void poll_transmission() {
  if (isLoraBoard) {
    // The LMIC needs to be polled a lot; and this is very low cost if the LMIC isn't
    // active. So we just act as a bridge. We need this routine so we can see
    // `isLoraBoard`. Most C compilers will notice the tail call and optimize this
    // to a jump.
    poll_lorawan();
  }
}

void prepare_http(HttpsClient *client, const char *host) {
  if (host[4] == 's')  // https
    client->hc->begin(*client->wc, host);
  else  // http
    client->hc->begin(host);
  client->hc->addHeader("Content-Type", "application/json; charset=UTF-8");
  client->hc->addHeader("Connection", "keep-alive");
  client->hc->addHeader("X-Sensor", chipID);
}

int send_http(HttpsClient *client, String body) {
  if (DEBUG_SERVER_SEND)
    log(DEBUG, "http request body: %s", body.c_str());
  int httpResponseCode = client->hc->POST(body);
  if (httpResponseCode > 0) {
    if (DEBUG_SERVER_SEND) {
      log(DEBUG, "http code: %d", httpResponseCode);
      String response = client->hc->getString();
      log(DEBUG, "http response: %s", response.c_str());
    }
  } else {
    log(ERROR, "Error on sending POST: %d", httpResponseCode);
  }
  client->hc->end();
  return httpResponseCode;
}

int send_http_geiger(HttpsClient *client, const char *host, unsigned int timediff, unsigned int hv_pulses,
                     unsigned int gm_counts, unsigned int cpm, int xpin) {
  char body[500];
  prepare_http(client, host);
  if (xpin != XPIN_NO_XPIN) {
    client->hc->addHeader("X-PIN", String(xpin));
  }
  const char *json_format = R"=====(
{
 "software_version": "%s",
 "sensordatavalues": [
  {"value_type": "counts_per_minute", "value": "%d"},
  {"value_type": "hv_pulses", "value": "%d"},
  {"value_type": "counts", "value": "%d"},
  {"value_type": "sample_time_ms", "value": "%d"}
 ]
}
)=====";
  snprintf(body, 500, json_format,
           http_software_version.c_str(),
           cpm,
           hv_pulses,
           gm_counts,
           timediff);
  return send_http(client, body);
}

int send_http_thp(HttpsClient *client, const char *host, float temperature, float humidity, float pressure, int xpin) {
  char body[300];
  prepare_http(client, host);
  if(xpin != XPIN_NO_XPIN) {
    client->hc->addHeader("X-PIN", String(xpin));
  }
  const char *json_format = R"=====(
{
 "software_version": "%s",
 "sensordatavalues": [
  {"value_type": "temperature", "value": "%.2f"},
  {"value_type": "humidity", "value": "%.2f"},
  {"value_type": "pressure", "value": "%.2f"}
 ]
}
)=====";
  snprintf(body, 300, json_format,
           http_software_version.c_str(),
           temperature,
           humidity,
           pressure);
  return send_http(client, body);
}

// two extra functions for MADAVI, because MADAVI needs the sensorname in value_type to recognize the sensors
int send_http_geiger_2_madavi(HttpsClient *client, String tube_type, unsigned int timediff, unsigned int hv_pulses,
                               unsigned int gm_counts, unsigned int cpm) {
  char body[500];
  prepare_http(client, MADAVI);
  tube_type = tube_type.substring(10);
  const char *json_format = R"=====(
{
 "software_version": "%s",
 "sensordatavalues": [
  {"value_type": "%s_counts_per_minute", "value": "%d"},
  {"value_type": "%s_hv_pulses", "value": "%d"},
  {"value_type": "%s_counts", "value": "%d"},
  {"value_type": "%s_sample_time_ms", "value": "%d"}
 ]
}
)=====";
  snprintf(body, 500, json_format,
           http_software_version.c_str(),
           tube_type.c_str(), cpm,
           tube_type.c_str(), hv_pulses,
           tube_type.c_str(), gm_counts,
           tube_type.c_str(), timediff);
  return send_http(client, body);
}

int send_http_thp_2_madavi(HttpsClient *client, float temperature, float humidity, float pressure) {
  char body[500];
  prepare_http(client, MADAVI);
  const char *json_format = R"=====(
{
 "software_version": "%s",
 "sensordatavalues": [
  {"value_type": "BME280_temperature", "value": "%.2f"},
  {"value_type": "BME280_humidity", "value": "%.2f"},
  {"value_type": "BME280_pressure", "value": "%.2f"}
 ]
}
)=====";
  snprintf(body, 500, json_format,
           http_software_version.c_str(),
           temperature,
           humidity,
           pressure);
  return send_http(client, body);
}

// LoRa payload:
// To minimise airtime and follow the 'TTN Fair Access Policy', we only send necessary bytes.
// We do NOT use Cayenne LPP.
// The payload will be translated via http integration and a small program to be compatible with sensor.community.
// For byte definitions see ttn2luft.pdf in docs directory.
int send_ttn_geiger(int tube_nbr, unsigned int dt, unsigned int gm_counts) {
  unsigned char ttnData[10];
  // first the number of GM counts
  ttnData[0] = (gm_counts >> 24) & 0xFF;
  ttnData[1] = (gm_counts >> 16) & 0xFF;
  ttnData[2] = (gm_counts >> 8) & 0xFF;
  ttnData[3] = gm_counts & 0xFF;
  // now 3 bytes for the measurement interval [in ms] (max ca. 4 hours)
  ttnData[4] = (dt >> 16) & 0xFF;
  ttnData[5] = (dt >> 8) & 0xFF;
  ttnData[6] = dt & 0xFF;
  // next two bytes are software version
  ttnData[7] = (lora_software_version >> 8) & 0xFF;
  ttnData[8] = lora_software_version & 0xFF;
  // next byte is the tube number
  ttnData[9] = tube_nbr;
  return lorawan_send(1, ttnData, 10, false, NULL, NULL, NULL);
}

int send_ttn_thp(float temperature, float humidity, float pressure) {
  unsigned char ttnData[5];
  ttnData[0] = ((int)(temperature * 10)) >> 8;
  ttnData[1] = ((int)(temperature * 10)) & 0xFF;
  ttnData[2] = (int)(humidity * 2);
  ttnData[3] = ((int)(pressure / 10)) >> 8;
  ttnData[4] = ((int)(pressure / 10)) & 0xFF;
  return lorawan_send(2, ttnData, 5, false, NULL, NULL, NULL);
}
//************influx-DB********************
int send_http_influx(HttpsClient *client, String body) {
  if (DEBUG_SERVER_SEND)
    log(DEBUG, "http request body: %s", body.c_str());
  if(strlen(influxUser) >0 || strlen(influxPassword)>0)
      client->hc->setAuthorization(influxUser,influxPassword);
  int httpResponseCode = client->hc->POST(body);
  if (httpResponseCode >= HTTP_CODE_OK && httpResponseCode <= HTTP_CODE_ALREADY_REPORTED) {
			log(DEBUG,"Sent to influx-db, status: ok, http: %d", httpResponseCode);
	} else if (httpResponseCode >= HTTP_CODE_BAD_REQUEST) {
			log(DEBUG,"Sent to influx-db, status: error, http: %d", httpResponseCode);
			log(DEBUG,"Details: %s", client->hc->getString().c_str());
	}
  client->hc->end();
  return httpResponseCode;
}

int send_http_geiger_2_influx(HttpsClient *client, const char *host, unsigned int timediff, unsigned int hv_pulses,
                     unsigned int gm_counts, unsigned int cpm, float Dose_Rate, int have_thp, float temperature, float humidity, float pressure) {
  char body[300];
  if (host[4] == 's')  // https
    client->hc->begin(*client->wc, host);
  else  // http
    client->hc->begin(influxServer,influxPort,influxPath);
  client->hc->addHeader("Content-Type", "application/x-www-form-urlencoded");
  client->hc->addHeader("X-Sensor", chipID);

  client->hc->addHeader("", String(influxMeasurement));
  if(!have_thp){
    snprintf(body, 300, "%s cpm=%d,hv_pulses=%d,gm_count=%d,timediff=%d,dose_rate=%f\n", influxMeasurement,
           cpm,
           hv_pulses,
           gm_counts,
           timediff,Dose_Rate);
  }
  else{
     snprintf(body, 300, "%s cpm=%d,hv_pulses=%d,gm_count=%d,timediff=%d,dose_rate=%f,temperature=%f,humidity=%f,pressure=%f\n", influxMeasurement,
           cpm,
           hv_pulses,
           gm_counts,
           timediff,Dose_Rate,temperature,humidity,pressure);

  }
  log(DEBUG,"Influx-Body: %s",body);
   return send_http_influx(client, body);

}
void transmit_data(String tube_type, int tube_nbr, unsigned int dt, unsigned int hv_pulses, unsigned int gm_counts, unsigned int cpm,float Dose_Rate,
                   int have_thp, float temperature, float humidity, float pressure, int wifi_status) {
  int rc1, rc2;
  RESERVE_STRING (logstr,SMALL_STR);
  bool transfer_ok;

  #if SEND2CUSTOMSRV
    //bool customsrv_ok;
    //log(INFO, "Sending to CUSTOMSRV ...");
    logstr=FPSTR(TRA_SEND_TO_INFO);
    logstr.replace("{ext}", F("CUSTOMSRV"));
    log(INFO, logstr.c_str());

    rc1 = send_http_geiger(&c_customsrv, CUSTOMSRV, dt, hv_pulses, gm_counts, cpm, XPIN_NO_XPIN);
    rc2 = have_thp ? send_http_thp(&c_customsrv, CUSTOMSRV, temperature, humidity, pressure, XPIN_NO_XPIN) : 200;
    transfer_ok = (rc1 == 200) && (rc2 == 200):true:false;
  //log(INFO, "Sent to CUSTOMSRV, status: %s, http: %d %d", customsrv_ok ? "ok" : "error", rc1, rc2);
    logstr=FPSTR(TRA_SENT_TO_INFO);
    logstr.replace("{ext}", F("CUSTOMSRV"));
    logstr.replace("{s}", transfer_ok ? "ok" : "error");
    logstr += rc1;
    logstr += F(", ");
    logstr += rc2;
    log(INFO, logstr.c_str());
  #endif

  if(sendToMadavi && (wifi_status == ST_WIFI_CONNECTED)) {
    //bool madavi_ok;
    //log(INFO, "Sending to Madavi ...");
    logstr=FPSTR(TRA_SEND_TO_INFO);
    logstr.replace("{ext}", F("Madavi"));
    log(INFO, logstr.c_str());

    set_status(STATUS_MADAVI, ST_MADAVI_SENDING);
    display_status();
    rc1 = send_http_geiger_2_madavi(&c_madavi, tube_type, dt, hv_pulses, gm_counts, cpm);
    rc2 = have_thp ? send_http_thp_2_madavi(&c_madavi, temperature, humidity, pressure) : 200;
    delay(300);
    transfer_ok = (rc1 == 200) && (rc2 == 200)?true:false;
    //log(INFO, "Sent to Madavi, status: %s, http: %d %d", madavi_ok ? "ok" : "error", rc1, rc2);
    logstr=FPSTR(TRA_SENT_TO_INFO);
    logstr.replace("{ext}", F("Madavi"));
    logstr.replace("{s}", transfer_ok ? "ok" : "error");
    logstr += rc1;
    logstr += F(", ");
    logstr += rc2;
    log(INFO, logstr.c_str());
    set_status(STATUS_MADAVI, transfer_ok ? ST_MADAVI_IDLE : ST_MADAVI_ERROR);
    display_status();
  }

  if(sendToCommunity  && (wifi_status == ST_WIFI_CONNECTED)) {
    //bool scomm_ok;
    //log(INFO, "Sending to sensor.community ...");
    logstr=FPSTR(TRA_SEND_TO_INFO);
    logstr.replace("{ext}", F("sensor.community"));
    log(INFO, logstr.c_str());
    set_status(STATUS_SCOMM, ST_SCOMM_SENDING);
    display_status();
    rc1 = send_http_geiger(&c_sensorc, SENSORCOMMUNITY, dt, hv_pulses, gm_counts, cpm, XPIN_RADIATION);
    rc2 = have_thp ? send_http_thp(&c_sensorc, SENSORCOMMUNITY, temperature, humidity, pressure, XPIN_BME280) : 201;
    delay(300);
    transfer_ok = (rc1 == 201) && (rc2 == 201)?true:false;
    //log(INFO, "Sent to sensor.community, status: %s, http: %d %d", scomm_ok ? "ok" : "error", rc1, rc2);
    logstr=FPSTR(TRA_SENT_TO_INFO);
    logstr.replace("{ext}", F("sensor.community"));
    logstr.replace("{s}", transfer_ok ? "ok" : "error");
    logstr += rc1;
    logstr += F(", ");
    logstr += rc2;
    log(INFO, logstr.c_str());
    set_status(STATUS_SCOMM, transfer_ok ? ST_SCOMM_IDLE : ST_SCOMM_ERROR);
    display_status();
  }

  if(isLoraBoard && sendToLora && (strcmp(appeui, "") != 0)) {    // send only, if we have LoRa credentials
    //bool ttn_ok;
    //log(INFO, "Sending to TTN ...");
    logstr=FPSTR(TRA_SEND_TO_INFO);
    logstr.replace("{ext}", "TTN");
    log(INFO, logstr.c_str());
    set_status(STATUS_TTN, ST_TTN_SENDING);
    display_status();
    rc1 = send_ttn_geiger(tube_nbr, dt, gm_counts);
    rc2 = have_thp ? send_ttn_thp(temperature, humidity, pressure) : TX_STATUS_UPLINK_SUCCESS;
    transfer_ok = (rc1 == TX_STATUS_UPLINK_SUCCESS) && (rc2 == TX_STATUS_UPLINK_SUCCESS)?true:false;
    set_status(STATUS_TTN, transfer_ok ? ST_TTN_IDLE : ST_TTN_ERROR);
    display_status();
  }
//InfuxDB
	if (sendToInflux  && (wifi_status == ST_WIFI_CONNECTED)) {
    //bool influx_ok;
    //log(INFO, "Sending to Influx-DB ...");
    logstr=FPSTR(TRA_SEND_TO_INFO);
    logstr.replace("{ext}", F("Influx-DB"));
    log(INFO, logstr.c_str());
    set_status(STATUS_INFLUX, ST_INFLUX_SENDING);
    display_status();

    log(DEBUG,"Measured data: cpm=%d, HV=%d, DoseRate=%f,gm_count=%d,timediff=%d", cpm, hv_pulses,Dose_Rate,gm_counts,dt);
    rc1 = send_http_geiger_2_influx(&c_influxsrv, influxServer, dt, hv_pulses, gm_counts, cpm, Dose_Rate,have_thp,temperature, humidity, pressure);

    transfer_ok = (rc1 >= HTTP_CODE_OK && rc1 <= HTTP_CODE_ALREADY_REPORTED)?true:false;
    //log(INFO, "Sent to influx server %s, status: %s, http: %d", influxServer, influx_ok ? "ok" : "error", rc1);
    logstr=FPSTR(TRA_SENT_TO_INFO);
    logstr.replace("{ext}", F("Influx-DB"));
    logstr.replace("{s}", transfer_ok ? "ok" : "error");
    logstr += rc1;
    log(INFO, logstr.c_str());
    set_status(STATUS_INFLUX, transfer_ok ? ST_INFLUX_IDLE : ST_INFLUX_ERROR);
    display_status();
	}
}

