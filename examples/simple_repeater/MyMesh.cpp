#include "MyMesh.h"
#include <algorithm>
#include <ctype.h>

#if defined(ESP32)
  #include <WiFi.h>
  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>
#endif

/* ------------------------------ Config -------------------------------- */

#ifndef LORA_FREQ
  #define LORA_FREQ 915.0
#endif
#ifndef LORA_BW
  #define LORA_BW 250
#endif
#ifndef LORA_SF
  #define LORA_SF 10
#endif
#ifndef LORA_CR
  #define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER 20
#endif

#ifndef ADVERT_NAME
  #define ADVERT_NAME "repeater"
#endif
#ifndef ADVERT_LAT
  #define ADVERT_LAT 0.0
#endif
#ifndef ADVERT_LON
  #define ADVERT_LON 0.0
#endif

#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY 300
#endif

#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY 200
#endif

#define FIRMWARE_VER_LEVEL       2

#define REQ_TYPE_GET_STATUS         0x01 // same as _GET_STATS
#define REQ_TYPE_KEEP_ALIVE         0x02
#define REQ_TYPE_GET_TELEMETRY_DATA 0x03
#define REQ_TYPE_GET_ACCESS_LIST    0x05
#define REQ_TYPE_GET_NEIGHBOURS     0x06
#define REQ_TYPE_GET_OWNER_INFO     0x07     // FIRMWARE_VER_LEVEL >= 2

#define RESP_SERVER_LOGIN_OK        0 // response to ANON_REQ

#define ANON_REQ_TYPE_REGIONS      0x01
#define ANON_REQ_TYPE_OWNER        0x02
#define ANON_REQ_TYPE_BASIC        0x03   // just remote clock

#define CLI_REPLY_DELAY_MILLIS      600

#define LAZY_CONTACTS_WRITE_DELAY    5000
#define PING_REPLY_ATTEMPTS          3
#define PING_REPLY_RETRY_DELAY_MS    2000
#define TEST_BROADCAST_INTERVAL_MS   (60UL * 60UL * 1000UL)

#define PUBLIC_GROUP_PSK  "izOH6cXN6mrJ5e26oRXNcg=="
static const uint8_t TEST_GROUP_SECRET[16] = {
  0x9c, 0xd8, 0xfc, 0xf2, 0x2a, 0x47, 0x33, 0x3b,
  0x59, 0x1d, 0x96, 0xa2, 0xb8, 0x48, 0xb7, 0x3f
};

#if defined(ESP32)
  #define DISCORD_WEBHOOK_MIN_INTERVAL_MS 200
  #define DISCORD_WEBHOOK_RETRY_DELAY_MS 2000

static size_t jsonEscape(const char* src, char* dst, size_t dst_len) {
  if (dst_len == 0) return 0;
  size_t w = 0;
  for (; *src && w + 1 < dst_len; ++src) {
    unsigned char c = (unsigned char)*src;
    if (c == '\\' || c == '"') {
      if (w + 2 >= dst_len) break;
      dst[w++] = '\\';
      dst[w++] = (char)c;
    } else if (c == '\n') {
      if (w + 2 >= dst_len) break;
      dst[w++] = '\\';
      dst[w++] = 'n';
    } else if (c == '\r') {
      if (w + 2 >= dst_len) break;
      dst[w++] = '\\';
      dst[w++] = 'r';
    } else if (c == '\t') {
      if (w + 2 >= dst_len) break;
      dst[w++] = '\\';
      dst[w++] = 't';
    } else if (c < 0x20) {
      if (w + 6 >= dst_len) break;
      snprintf(&dst[w], dst_len - w, "\\u%04x", c);
      w += 6;
    } else {
      dst[w++] = (char)c;
    }
  }
  dst[w] = 0;
  return w;
}
#endif

uint8_t MyMesh::calcBusyPercent() {
  uint32_t now_ms = millis();
  uint32_t tx_ms = getTotalAirTime();
  uint32_t rx_ms = getReceiveAirTime();
  if (last_busy_sample_ms == 0 || now_ms <= last_busy_sample_ms) {
    last_busy_sample_ms = now_ms;
    last_busy_tx_ms = tx_ms;
    last_busy_rx_ms = rx_ms;
    return 0;
  }

  uint32_t delta_ms = now_ms - last_busy_sample_ms;
  uint32_t delta_air_ms = (tx_ms - last_busy_tx_ms) + (rx_ms - last_busy_rx_ms);

  last_busy_sample_ms = now_ms;
  last_busy_tx_ms = tx_ms;
  last_busy_rx_ms = rx_ms;

  if (delta_ms == 0) return 0;
  uint32_t busy = (delta_air_ms * 100UL) / delta_ms;
  if (busy > 100) busy = 100;
  return (uint8_t)busy;
}

static void formatPathString(const uint8_t* path, uint8_t path_len, char* dest, size_t dest_len) {
  if (dest_len == 0) return;
  dest[0] = 0;
  if (path_len == 0) {
    strncpy(dest, "direct", dest_len);
    dest[dest_len - 1] = 0;
    return;
  }

  size_t used = 0;
  for (uint8_t i = 0; i < path_len; i++) {
    const char* sep = (i == 0) ? "" : ">";
    int written = snprintf(dest + used, dest_len - used, "%s%02X", sep, path[i]);
    if (written < 0 || (size_t)written >= dest_len - used) {
      dest[dest_len - 1] = 0;
      return;
    }
    used += (size_t)written;
  }
}

static const char* findMessageBody(const char* text, const char* node_name, char* sender, size_t sender_len) {
  if (sender_len == 0) return NULL;
  sender[0] = 0;
  const char* sep = strstr(text, ": ");
  if (sep) {
    size_t name_len = sep - text;
    if (name_len >= sender_len) name_len = sender_len - 1;
    memcpy(sender, text, name_len);
    sender[name_len] = 0;
    if (name_len > 0 && strncmp(text, node_name, name_len) == 0 && node_name[name_len] == 0) {
      return NULL;  // ignore our own messages
    }
    text = sep + 2;
  }
  while (*text == ' ') text++;
  return text;
}

static const char* findMessageBodyAllowSelf(const char* text, char* sender, size_t sender_len) {
  if (sender_len == 0) return NULL;
  sender[0] = 0;
  const char* sep = strstr(text, ": ");
  if (sep) {
    size_t name_len = sep - text;
    if (name_len >= sender_len) name_len = sender_len - 1;
    memcpy(sender, text, name_len);
    sender[name_len] = 0;
    text = sep + 2;
  }
  while (*text == ' ') text++;
  return text;
}

static bool containsSubstringCaseInsensitive(const char* haystack, const char* needle) {
  if (!haystack || !needle) return false;
  if (needle[0] == 0) return true;
  for (const char* h = haystack; *h; ++h) {
    const char* h_it = h;
    const char* n_it = needle;
    while (*h_it && *n_it &&
           tolower(static_cast<unsigned char>(*h_it)) ==
               tolower(static_cast<unsigned char>(*n_it))) {
      ++h_it;
      ++n_it;
    }
    if (*n_it == 0) return true;
  }
  return false;
}

void MyMesh::putNeighbour(const mesh::Identity &id, uint32_t timestamp, float snr) {
#if MAX_NEIGHBOURS // check if neighbours enabled
  // find existing neighbour, else use least recently updated
  uint32_t oldest_timestamp = 0xFFFFFFFF;
  NeighbourInfo *neighbour = &neighbours[0];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    // if neighbour already known, we should update it
    if (id.matches(neighbours[i].id)) {
      neighbour = &neighbours[i];
      break;
    }

    // otherwise we should update the least recently updated neighbour
    if (neighbours[i].heard_timestamp < oldest_timestamp) {
      neighbour = &neighbours[i];
      oldest_timestamp = neighbour->heard_timestamp;
    }
  }

  // update neighbour info
  neighbour->id = id;
  neighbour->advert_timestamp = timestamp;
  neighbour->heard_timestamp = getRTCClock()->getCurrentTime();
  neighbour->snr = (int8_t)(snr * 4);
#endif
}

uint8_t MyMesh::handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood) {
  ClientInfo* client = NULL;
  if (data[0] == 0) {   // blank password, just check if sender is in ACL
    client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    if (client == NULL) {
    #if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Login, sender not in ACL");
    #endif
    }
  }
  if (client == NULL) {
    uint8_t perms;
    if (strcmp((char *)data, _prefs.password) == 0) { // check for valid admin password
      perms = PERM_ACL_ADMIN;
    } else if (strcmp((char *)data, _prefs.guest_password) == 0) { // check guest password
      perms = PERM_ACL_GUEST;
    } else {
#if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Invalid password: %s", data);
#endif
      return 0;
    }

    client = acl.putClient(sender, 0);  // add to contacts (if not already known)
    if (sender_timestamp <= client->last_timestamp) {
      MESH_DEBUG_PRINTLN("Possible login replay attack!");
      return 0;  // FATAL: client table is full -OR- replay attack
    }

    MESH_DEBUG_PRINTLN("Login success!");
    client->last_timestamp = sender_timestamp;
    client->last_activity = getRTCClock()->getCurrentTime();
    client->permissions &= ~0x03;
    client->permissions |= perms;
    memcpy(client->shared_secret, secret, PUB_KEY_SIZE);

    if (perms != PERM_ACL_GUEST) {   // keep number of FS writes to a minimum
      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
    }
  }

  if (is_flood) {
    client->out_path_len = -1;  // need to rediscover out_path
  }

  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  reply_data[4] = RESP_SERVER_LOGIN_OK;
  reply_data[5] = 0;  // Legacy: was recommended keep-alive interval (secs / 16)
  reply_data[6] = client->isAdmin() ? 1 : 0;
  reply_data[7] = client->permissions;
  getRNG()->random(&reply_data[8], 4);   // random blob to help packet-hash uniqueness
  reply_data[12] = FIRMWARE_VER_LEVEL;  // New field

  return 13;  // reply length
}

uint8_t MyMesh::handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++ & 0x3F;
    memcpy(reply_path, data, reply_path_len);
    // data += reply_path_len;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)

    return 8 + region_map.exportNamesTo((char *) &reply_data[8], sizeof(reply_data) - 12, REGION_DENY_FLOOD);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++ & 0x3F;
    memcpy(reply_path, data, reply_path_len);
    // data += reply_path_len;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    sprintf((char *) &reply_data[8], "%s\n%s", _prefs.node_name, _prefs.owner_info);

    return 8 + strlen((char *) &reply_data[8]);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++ & 0x3F;
    memcpy(reply_path, data, reply_path_len);
    // data += reply_path_len;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    reply_data[8] = 0;  // features
#ifdef WITH_RS232_BRIDGE
    reply_data[8] |= 0x01;  // is bridge, type UART
#elif WITH_ESPNOW_BRIDGE
    reply_data[8] |= 0x03;  // is bridge, type ESP-NOW
#endif
    if (_prefs.disable_fwd) {   // is this repeater currently disabled
      reply_data[8] |= 0x80;  // is disabled
    }
    // TODO:  add some kind of moving-window utilisation metric, so can query 'how busy' is this repeater
    return 9;   // reply length
  }
  return 0;
}

int MyMesh::handleRequest(ClientInfo *sender, uint32_t sender_timestamp, uint8_t *payload, size_t payload_len) {
  // uint32_t now = getRTCClock()->getCurrentTimeUnique();
  // memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  memcpy(reply_data, &sender_timestamp, 4); // reflect sender_timestamp back in response packet (kind of like a 'tag')

  if (payload[0] == REQ_TYPE_GET_STATUS) {  // guests can also access this now
    RepeaterStats stats;
    stats.batt_milli_volts = board.getBattMilliVolts();
    stats.curr_tx_queue_len = _mgr->getOutboundCount(0xFFFFFFFF);
    stats.noise_floor = (int16_t)_radio->getNoiseFloor();
    stats.last_rssi = (int16_t)radio_driver.getLastRSSI();
    stats.n_packets_recv = radio_driver.getPacketsRecv();
    stats.n_packets_sent = radio_driver.getPacketsSent();
    stats.total_air_time_secs = getTotalAirTime() / 1000;
    stats.total_up_time_secs = uptime_millis / 1000;
    stats.n_sent_flood = getNumSentFlood();
    stats.n_sent_direct = getNumSentDirect();
    stats.n_recv_flood = getNumRecvFlood();
    stats.n_recv_direct = getNumRecvDirect();
    stats.err_events = _err_flags;
    stats.last_snr = (int16_t)(radio_driver.getLastSNR() * 4);
    stats.n_direct_dups = ((SimpleMeshTables *)getTables())->getNumDirectDups();
    stats.n_flood_dups = ((SimpleMeshTables *)getTables())->getNumFloodDups();
    stats.total_rx_air_time_secs = getReceiveAirTime() / 1000;
    stats.n_recv_errors = radio_driver.getPacketsRecvErrors();
    memcpy(&reply_data[4], &stats, sizeof(stats));

    return 4 + sizeof(stats); //  reply_len
  }
  if (payload[0] == REQ_TYPE_GET_TELEMETRY_DATA) {
    uint8_t perm_mask = ~(payload[1]); // NEW: first reserved byte (of 4), is now inverse mask to apply to permissions

    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);

    // query other sensors -- target specific
    if ((sender->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
      perm_mask = 0x00;  // just base telemetry allowed
    }
    sensors.querySensors(perm_mask, telemetry);

	// This default temperature will be overridden by external sensors (if any)
    float temperature = board.getMCUTemperature();
    if(!isnan(temperature)) { // Supported boards with built-in temperature sensor. ESP32-C3 may return NAN
      telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature); // Built-in MCU Temperature
    }

    uint8_t tlen = telemetry.getSize();
    memcpy(&reply_data[4], telemetry.getBuffer(), tlen);
    return 4 + tlen; // reply_len
  }
  if (payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()) {
    uint8_t res1 = payload[1];   // reserved for future  (extra query params)
    uint8_t res2 = payload[2];
    if (res1 == 0 && res2 == 0) {
      uint8_t ofs = 4;
      for (int i = 0; i < acl.getNumClients() && ofs + 7 <= sizeof(reply_data) - 4; i++) {
        auto c = acl.getClientByIdx(i);
        if (c->permissions == 0) continue;  // skip deleted entries
        memcpy(&reply_data[ofs], c->id.pub_key, 6); ofs += 6;  // just 6-byte pub_key prefix
        reply_data[ofs++] = c->permissions;
      }
      return ofs;
    }
  }
  if (payload[0] == REQ_TYPE_GET_NEIGHBOURS) {
    uint8_t request_version = payload[1];
    if (request_version == 0) {

      // reply data offset (after response sender_timestamp/tag)
      int reply_offset = 4;

      // get request params
      uint8_t count = payload[2]; // how many neighbours to fetch (0-255)
      uint16_t offset;
      memcpy(&offset, &payload[3], 2); // offset from start of neighbours list (0-65535)
      uint8_t order_by = payload[5]; // how to order neighbours. 0=newest_to_oldest, 1=oldest_to_newest, 2=strongest_to_weakest, 3=weakest_to_strongest
      uint8_t pubkey_prefix_length = payload[6]; // how many bytes of neighbour pub key we want
      // we also send a 4 byte random blob in payload[7...10] to help packet uniqueness

      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS count=%d, offset=%d, order_by=%d, pubkey_prefix_length=%d", count, offset, order_by, pubkey_prefix_length);

      // clamp pub key prefix length to max pub key length
      if(pubkey_prefix_length > PUB_KEY_SIZE){
        pubkey_prefix_length = PUB_KEY_SIZE;
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS invalid pubkey_prefix_length=%d clamping to %d", pubkey_prefix_length, PUB_KEY_SIZE);
      }

      // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
      int16_t neighbours_count = 0;
      NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
      for (int i = 0; i < MAX_NEIGHBOURS; i++) {
        auto neighbour = &neighbours[i];
        if (neighbour->heard_timestamp > 0) {
          sorted_neighbours[neighbours_count] = neighbour;
          neighbours_count++;
        }
      }

      // sort neighbours based on order
      if (order_by == 0) {
        // sort by newest to oldest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting newest to oldest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp > b->heard_timestamp; // desc
        });
      } else if (order_by == 1) {
        // sort by oldest to newest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting oldest to newest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp < b->heard_timestamp; // asc
        });
      } else if (order_by == 2) {
        // sort by strongest to weakest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting strongest to weakest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr > b->snr; // desc
        });
      } else if (order_by == 3) {
        // sort by weakest to strongest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting weakest to strongest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr < b->snr; // asc
        });
      }

      // build results buffer
      int results_count = 0;
      int results_offset = 0;
      uint8_t results_buffer[130];
      for(int index = 0; index < count && index + offset < neighbours_count; index++){
        
        // stop if we can't fit another entry in results
        int entry_size = pubkey_prefix_length + 4 + 1;
        if(results_offset + entry_size > sizeof(results_buffer)){
          MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS no more entries can fit in results buffer");
          break;
        }

        // add next neighbour to results
        auto neighbour = sorted_neighbours[index + offset];
        uint32_t heard_seconds_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
        memcpy(&results_buffer[results_offset], neighbour->id.pub_key, pubkey_prefix_length); results_offset += pubkey_prefix_length;
        memcpy(&results_buffer[results_offset], &heard_seconds_ago, 4); results_offset += 4;
        memcpy(&results_buffer[results_offset], &neighbour->snr, 1); results_offset += 1;
        results_count++;

      }

      // build reply
      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS neighbours_count=%d results_count=%d", neighbours_count, results_count);
      memcpy(&reply_data[reply_offset], &neighbours_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_buffer, results_offset); reply_offset += results_offset;

      return reply_offset;
    }
  } else if (payload[0] == REQ_TYPE_GET_OWNER_INFO) {
    sprintf((char *) &reply_data[4], "%s\n%s\n%s", FIRMWARE_VERSION, _prefs.node_name, _prefs.owner_info);
    return 4 + strlen((char *) &reply_data[4]);
  }
  return 0; // unknown command
}

mesh::Packet *MyMesh::createSelfAdvert() {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len = _cli.buildAdvertData(ADV_TYPE_REPEATER, app_data);

  return createAdvert(self_id, app_data, app_data_len);
}

File MyMesh::openAppend(const char *fname) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return _fs->open(fname, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(fname, "a");
#else
  return _fs->open(fname, "a", true);
#endif
}

bool MyMesh::allowPacketForward(const mesh::Packet *packet) {
  if (_prefs.disable_fwd) return false;
  if (packet->isRouteFlood() && packet->path_len >= _prefs.flood_max) return false;
  if (packet->isRouteFlood() && recv_pkt_region == NULL) {
    MESH_DEBUG_PRINTLN("allowPacketForward: unknown transport code, or wildcard not allowed for FLOOD packet");
    return false;
  }
  return true;
}

const char *MyMesh::getLogDateTime() {
  static char tmp[32];
  uint32_t now = getRTCClock()->getCurrentTime();
  DateTime dt = DateTime(now);
  sprintf(tmp, "%02d:%02d:%02d - %d/%d/%d U", dt.hour(), dt.minute(), dt.second(), dt.day(), dt.month(),
          dt.year());
  return tmp;
}

void MyMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
#if MESH_PACKET_LOGGING
  Serial.print(getLogDateTime());
  Serial.print(" RAW: ");
  mesh::Utils::printHex(Serial, raw, len);
  Serial.println();
#endif
}

void MyMesh::logRx(mesh::Packet *pkt, int len, float score) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 1) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": RX, len=%d (type=%d, route=%s, payload_len=%d) SNR=%d RSSI=%d score=%d", len,
               pkt->getPayloadType(), pkt->isRouteDirect() ? "D" : "F", pkt->payload_len,
               (int)_radio->getLastSNR(), (int)_radio->getLastRSSI(), (int)(score * 1000));

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTx(mesh::Packet *pkt, int len) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 0) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX, len=%d (type=%d, route=%s, payload_len=%d)", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTxFail(mesh::Packet *pkt, int len) {
  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX FAIL!, len=%d (type=%d, route=%s, payload_len=%d)\n", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);
      f.close();
    }
  }
}

int MyMesh::calcRxDelay(float score, uint32_t air_time) const {
  if (_prefs.rx_delay_base <= 0.0f) return 0;
  return (int)((pow(_prefs.rx_delay_base, 0.85f - score) - 1.0) * air_time);
}

uint32_t MyMesh::getRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->path_len + packet->payload_len + 2) * _prefs.tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}
uint32_t MyMesh::getDirectRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->path_len + packet->payload_len + 2) * _prefs.direct_tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}

bool MyMesh::filterRecvFloodPacket(mesh::Packet* pkt) {
  // just try to determine region for packet (apply later in allowPacketForward())
  if (pkt->getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD) {
    recv_pkt_region = region_map.findMatch(pkt, REGION_DENY_FLOOD);
  } else if (pkt->getRouteType() == ROUTE_TYPE_FLOOD) {
    if (region_map.getWildcard().flags & REGION_DENY_FLOOD) {
      recv_pkt_region = NULL;
    } else {
      recv_pkt_region =  &region_map.getWildcard();
    }
  } else {
    recv_pkt_region = NULL;
  }
  // do normal processing
  return false;
}

void MyMesh::onAnonDataRecv(mesh::Packet *packet, const uint8_t *secret, const mesh::Identity &sender,
                            uint8_t *data, size_t len) {
  if (packet->getPayloadType() == PAYLOAD_TYPE_ANON_REQ) { // received an initial request by a possible admin
                                                           // client (unknown at this stage)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    data[len] = 0;  // ensure null terminator
    uint8_t reply_len;

    reply_path_len = -1;
    if (data[4] == 0 || data[4] >= ' ') {   // is password, ie. a login request
      reply_len = handleLoginReq(sender, secret, timestamp, &data[4], packet->isRouteFlood());
    } else if (data[4] == ANON_REQ_TYPE_REGIONS && packet->isRouteDirect()) {
      reply_len = handleAnonRegionsReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_OWNER && packet->isRouteDirect()) {
      reply_len = handleAnonOwnerReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_BASIC && packet->isRouteDirect()) {
      reply_len = handleAnonClockReq(sender, timestamp, &data[5]);
    } else {
      reply_len = 0;  // unknown/invalid request type
    }

    if (reply_len == 0) return;   // invalid request

    if (packet->isRouteFlood()) {
      // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
      mesh::Packet* path = createPathReturn(sender, secret, packet->path, packet->path_len,
                                            PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
      if (path) sendFlood(path, SERVER_RESPONSE_DELAY);
    } else if (reply_path_len < 0) {
      mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply_data, reply_len);
      if (reply) sendFlood(reply, SERVER_RESPONSE_DELAY);
    } else {
      mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply_data, reply_len);
      if (reply) sendDirect(reply, reply_path, reply_path_len, SERVER_RESPONSE_DELAY);
    }
  }
}

void MyMesh::initPublicChannel() {
  public_channel_ready = false;
  memset(public_channel.hash, 0, sizeof(public_channel.hash));
  memset(public_channel.secret, 0, sizeof(public_channel.secret));
  static const uint8_t public_secret[16] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72
  };
  memcpy(public_channel.secret, public_secret, sizeof(public_secret));
  mesh::Utils::sha256(public_channel.hash, sizeof(public_channel.hash), public_channel.secret, sizeof(public_secret));
  public_channel_ready = true;
}

void MyMesh::initTestChannel() {
  test_channel_ready = false;
  memset(test_channel.hash, 0, sizeof(test_channel.hash));
  memset(test_channel.secret, 0, sizeof(test_channel.secret));
  memcpy(test_channel.secret, TEST_GROUP_SECRET, sizeof(TEST_GROUP_SECRET));
  mesh::Utils::sha256(test_channel.hash, sizeof(test_channel.hash), test_channel.secret, sizeof(TEST_GROUP_SECRET));
  test_channel_ready = true;
}

#if defined(ESP32)
void MyMesh::initWifiClient() {
  if (_prefs.wifi_ssid[0] == 0 || _prefs.wifi_pwd[0] == 0) return;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  MESH_DEBUG_PRINTLN("WiFi: connecting to AP '%s'", _prefs.wifi_ssid);
  WiFi.begin(_prefs.wifi_ssid, _prefs.wifi_pwd);
  next_wifi_attempt_at = futureMillis(15000);
}

void MyMesh::queueDiscordWebhook(const char* sender, const char* body) {
  if (_prefs.discord_webhook_url[0] == 0) return;
  if (webhook_count >= DISCORD_WEBHOOK_QUEUE_SIZE) {
    MESH_DEBUG_PRINTLN("Discord webhook queue full, dropping oldest");
    webhook_head = (uint8_t)((webhook_head + 1) % DISCORD_WEBHOOK_QUEUE_SIZE);
    webhook_count--;
  }
  WebhookItem* item = &webhook_queue[webhook_tail];
  StrHelper::strncpy(item->sender, sender, sizeof(item->sender));
  StrHelper::strncpy(item->body, body, sizeof(item->body));
  webhook_tail = (uint8_t)((webhook_tail + 1) % DISCORD_WEBHOOK_QUEUE_SIZE);
  webhook_count++;
  next_webhook_attempt_at = 0;
}

void MyMesh::pumpDiscordWebhook() {
  if (webhook_count == 0) return;

  if (WiFi.status() != WL_CONNECTED) {
    if (next_wifi_attempt_at && millisHasNowPassed(next_wifi_attempt_at)) {
      if (_prefs.wifi_ssid[0] == 0 || _prefs.wifi_pwd[0] == 0) return;
      MESH_DEBUG_PRINTLN("WiFi: not connected, retrying AP '%s'", _prefs.wifi_ssid);
      WiFi.begin(_prefs.wifi_ssid, _prefs.wifi_pwd);
      next_wifi_attempt_at = futureMillis(15000);
    }
    return;
  }

  if (next_webhook_attempt_at && !millisHasNowPassed(next_webhook_attempt_at)) return;

  WebhookItem* item = &webhook_queue[webhook_head];

  char esc_sender[80];
  char esc_body[256];
  jsonEscape(item->sender, esc_sender, sizeof(esc_sender));
  jsonEscape(item->body, esc_body, sizeof(esc_body));

  char payload[512];
  snprintf(payload, sizeof(payload),
           "{\"username\":\"%s\",\"avatar_url\":\"http://cedarmesh.ca/images/mc_logo.png\",\"content\":\"%s\"}",
           esc_sender, esc_body);

  char url_buf[192];
  StrHelper::strncpy(url_buf, _prefs.discord_webhook_url, sizeof(url_buf));
  size_t start = 0;
  while (url_buf[start] && (unsigned char)url_buf[start] <= ' ') start++;
  size_t end = strlen(url_buf);
  while (end > start && (unsigned char)url_buf[end - 1] <= ' ') end--;
  url_buf[end] = 0;
  const char* url = url_buf + start;
  if (*url == 0) return;

  bool is_https = false;
  bool is_http = false;
  const char* p = url;
  const char* https_scheme = "https://";
  const char* http_scheme = "http://";
  auto starts_with_ci = [](const char* s, const char* prefix, int n) -> bool {
    for (int i = 0; i < n; i++) {
      char c = s[i];
      if (!c) return false;
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      char pfx = prefix[i];
      if (c != pfx) return false;
    }
    return true;
  };
  if (starts_with_ci(p, https_scheme, 8)) {
    is_https = true;
  } else if (starts_with_ci(p, http_scheme, 7)) {
    is_http = true;
  } else {
    // Search for a scheme inside the string in case of hidden prefix chars.
    const char* s = p;
    while (*s) {
      if (starts_with_ci(s, https_scheme, 8)) { p = s; is_https = true; break; }
      if (starts_with_ci(s, http_scheme, 7)) { p = s; is_http = true; break; }
      s++;
    }
  }
  if (!is_https && !is_http) {
    MESH_DEBUG_PRINTLN("Discord webhook invalid scheme");
    return;
  }
  char lead_hex[24];
  int lh = 0;
  for (int i = 0; i < 8 && url[i]; i++) {
    lh += snprintf(lead_hex + lh, sizeof(lead_hex) - lh, "%02X", (unsigned char)url[i]);
  }
  MESH_DEBUG_PRINTLN("Discord webhook scheme: %s lead=%s", is_https ? "https" : "http", lead_hex);

  const char* host = url;
  if (is_https) host += 8;
  else if (is_http) host += 7;

  const char* path = strchr(host, '/');
  char host_buf[128];
  if (path) {
    size_t host_len = (size_t)(path - host);
    if (host_len >= sizeof(host_buf)) host_len = sizeof(host_buf) - 1;
    memcpy(host_buf, host, host_len);
    host_buf[host_len] = 0;
  } else {
    StrHelper::strncpy(host_buf, host, sizeof(host_buf));
    path = "/";
  }

  int code = -1;
  String resp;
  if (is_https) {
    WiFiClientSecure client;
    client.setInsecure();
    if (!client.connect(host_buf, 443)) {
      MESH_DEBUG_PRINTLN("Discord webhook TLS connect failed");
      next_webhook_attempt_at = futureMillis(DISCORD_WEBHOOK_RETRY_DELAY_MS);
      return;
    }
    client.printf("POST %s HTTP/1.1\r\n", path);
    client.printf("Host: %s\r\n", host_buf);
    client.printf("User-Agent: MeshCore\r\n");
    client.printf("Content-Type: application/json\r\n");
    client.printf("Content-Length: %u\r\n", (unsigned)strlen(payload));
    client.printf("Connection: close\r\n\r\n");
    client.write((const uint8_t*)payload, strlen(payload));

    // Read status line
    String status = client.readStringUntil('\n');
    status.trim();
    if (status.startsWith("HTTP/")) {
      int sp = status.indexOf(' ');
      if (sp > 0) {
        code = status.substring(sp + 1).toInt();
      }
    }
    // Read headers, then body
    while (client.connected()) {
      String line = client.readStringUntil('\n');
      if (line == "\r" || line.length() == 0) break;
    }
    resp = client.readString();
    client.stop();
  } else {
    HTTPClient http;
    WiFiClient client;
    http.begin(client, host_buf, 80, path, false);
    http.addHeader("Content-Type", "application/json");
    code = http.POST((uint8_t*)payload, strlen(payload));
    resp = http.getString();
    http.end();
  }

  if (code >= 200 && code < 300) {
    webhook_head = (uint8_t)((webhook_head + 1) % DISCORD_WEBHOOK_QUEUE_SIZE);
    webhook_count--;
    MESH_DEBUG_PRINTLN("Discord webhook OK: HTTP %d", code);
    next_webhook_attempt_at = futureMillis(DISCORD_WEBHOOK_MIN_INTERVAL_MS);
  } else {
    char resp_buf[160];
    resp_buf[0] = 0;
    if (resp.length() > 0) {
      size_t n = (resp.length() < sizeof(resp_buf) - 1) ? resp.length() : sizeof(resp_buf) - 1;
      memcpy(resp_buf, resp.c_str(), n);
      resp_buf[n] = 0;
    }
    MESH_DEBUG_PRINTLN("Discord webhook failed: HTTP %d body=%s", code, resp_buf[0] ? resp_buf : "<empty>");
    next_webhook_attempt_at = futureMillis(DISCORD_WEBHOOK_RETRY_DELAY_MS);
  }
}
#endif

bool MyMesh::isPingChannel(const mesh::GroupChannel& channel) const {
  if (_prefs.ping_public_enabled && public_channel_ready && memcmp(channel.hash, public_channel.hash, PATH_HASH_SIZE) == 0) return true;
  if (test_channel_ready && memcmp(channel.hash, test_channel.hash, PATH_HASH_SIZE) == 0) return true;
  return false;
}

void MyMesh::sendPingReply(const mesh::GroupChannel& channel, const char* reply_text) {
  if (!public_channel_ready && !test_channel_ready) return;

  int attempt = PING_REPLY_ATTEMPTS - pending_ping_retries + 1;
  if (attempt < 1) attempt = 1;
  if (attempt > PING_REPLY_ATTEMPTS) attempt = PING_REPLY_ATTEMPTS;

  uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
  uint8_t temp[MAX_PACKET_PAYLOAD];
  memcpy(temp, &timestamp, 4);
  temp[4] = 0;  // TXT_TYPE_PLAIN

  snprintf((char*)&temp[5], MAX_PACKET_PAYLOAD - 5, "%s: %s [%d/%d]",
           _prefs.node_name, reply_text, attempt, PING_REPLY_ATTEMPTS);
  size_t msg_len = strlen((char*)&temp[5]);

  auto reply = createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel, temp, 5 + msg_len);
  if (reply) {
    sendFlood(reply, SERVER_RESPONSE_DELAY);
  }
}

void MyMesh::sendPublicBroadcast(const mesh::GroupChannel& channel) {
  if (!test_channel_ready && !public_channel_ready) return;

  uint32_t timestamp = millis() / 1000;
  int16_t noise_floor = (int16_t)_radio->getNoiseFloor();
  uint8_t busy_pct = calcBusyPercent();
  uint8_t temp[MAX_PACKET_PAYLOAD];
  memcpy(temp, &timestamp, 4);
  temp[4] = 0;  // TXT_TYPE_PLAIN

  snprintf((char*)&temp[5], MAX_PACKET_PAYLOAD - 5, "%s: NF %ddBm Busy %u%%",
           _prefs.node_name, noise_floor, busy_pct);
  size_t msg_len = strlen((char*)&temp[5]);
  if (msg_len > 134) {
    msg_len = 134;
    temp[5 + msg_len] = 0;
  }

  auto pkt = createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel, temp, 5 + msg_len);
  if (pkt) {
    sendFlood(pkt, SERVER_RESPONSE_DELAY);
  }
}

int MyMesh::searchPeersByHash(const uint8_t *hash) {
  int n = 0;
  for (int i = 0; i < acl.getNumClients(); i++) {
    if (acl.getClientByIdx(i)->id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i; // store the INDEXES of matching contacts (for subsequent 'peer' methods)
    }
  }
  return n;
}

int MyMesh::searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) {
  if (max_matches <= 0) return 0;
  int n = 0;
  if (public_channel_ready && memcmp(public_channel.hash, hash, PATH_HASH_SIZE) == 0) {
    channels[n++] = public_channel;
  }
  if (test_channel_ready && n < max_matches && memcmp(test_channel.hash, hash, PATH_HASH_SIZE) == 0) {
    channels[n++] = test_channel;
  }
  return n;
}

void MyMesh::getPeerSharedSecret(uint8_t *dest_secret, int peer_idx) {
  int i = matching_peer_indexes[peer_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    // lookup pre-calculated shared_secret
    memcpy(dest_secret, acl.getClientByIdx(i)->shared_secret, PUB_KEY_SIZE);
  } else {
    MESH_DEBUG_PRINTLN("getPeerSharedSecret: Invalid peer idx: %d", i);
  }
}

static bool isShare(const mesh::Packet *packet) {
  if (packet->hasTransportCodes()) {
    return packet->transport_codes[0] == 0 && packet->transport_codes[1] == 0;  // codes { 0, 0 } means 'send to nowhere'
  }
  return false;
}

void MyMesh::onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id, uint32_t timestamp,
                          const uint8_t *app_data, size_t app_data_len) {
  mesh::Mesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len); // chain to super impl

  // if this a zero hop advert (and not via 'Share'), add it to neighbours
  if (packet->path_len == 0 && !isShare(packet)) {
    AdvertDataParser parser(app_data, app_data_len);
    if (parser.isValid() && parser.getType() == ADV_TYPE_REPEATER) { // just keep neigbouring Repeaters
      putNeighbour(id, timestamp, packet->getSNR());
    }
  }
}

void MyMesh::onPeerDataRecv(mesh::Packet *packet, uint8_t type, int sender_idx, const uint8_t *secret,
                            uint8_t *data, size_t len) {
  int i = matching_peer_indexes[sender_idx];
  if (i < 0 || i >= acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("onPeerDataRecv: invalid peer idx: %d", i);
    return;
  }
  ClientInfo* client = acl.getClientByIdx(i);

  if (type == PAYLOAD_TYPE_REQ) { // request (from a Known admin client!)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    if (timestamp > client->last_timestamp) { // prevent replay attacks
      int reply_len = handleRequest(client, timestamp, &data[4], len - 4);
      if (reply_len == 0) return; // invalid command

      client->last_timestamp = timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      if (packet->isRouteFlood()) {
        // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
        mesh::Packet *path = createPathReturn(client->id, secret, packet->path, packet->path_len,
                                              PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
        if (path) sendFlood(path, SERVER_RESPONSE_DELAY);
      } else {
        mesh::Packet *reply =
            createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret, reply_data, reply_len);
        if (reply) {
          if (client->out_path_len >= 0) { // we have an out_path, so send DIRECT
            sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
          } else {
            sendFlood(reply, SERVER_RESPONSE_DELAY);
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  } else if (type == PAYLOAD_TYPE_TXT_MSG && len > 5 && client->isAdmin()) { // a CLI command
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4); // timestamp (by sender's RTC clock - which could be wrong)
    uint8_t flags = (data[4] >> 2);        // message attempt number, and other flags

    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA)) {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: unsupported text type received: flags=%02x", (uint32_t)flags);
    } else if (sender_timestamp >= client->last_timestamp) { // prevent replay attacks
      bool is_retry = (sender_timestamp == client->last_timestamp);
      client->last_timestamp = sender_timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      // len can be > original length, but 'text' will be padded with zeroes
      data[len] = 0; // need to make a C string again, with null terminator

      if (flags == TXT_TYPE_PLAIN) { // for legacy CLI, send Acks
        uint32_t ack_hash; // calc truncated hash of the message timestamp + text + sender pub_key, to prove
                           // to sender that we got it
        mesh::Utils::sha256((uint8_t *)&ack_hash, 4, data, 5 + strlen((char *)&data[5]), client->id.pub_key,
                            PUB_KEY_SIZE);

        mesh::Packet *ack = createAck(ack_hash);
        if (ack) {
          if (client->out_path_len < 0) {
            sendFlood(ack, TXT_ACK_DELAY);
          } else {
            sendDirect(ack, client->out_path, client->out_path_len, TXT_ACK_DELAY);
          }
        }
      }

      uint8_t temp[166];
      char *command = (char *)&data[5];
      char *reply = (char *)&temp[5];
      if (is_retry) {
        *reply = 0;
      } else {
        handleCommand(sender_timestamp, command, reply);
      }
      int text_len = strlen(reply);
      if (text_len > 0) {
        uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
        if (timestamp == sender_timestamp) {
          // WORKAROUND: the two timestamps need to be different, in the CLI view
          timestamp++;
        }
        memcpy(temp, &timestamp, 4);        // mostly an extra blob to help make packet_hash unique
        temp[4] = (TXT_TYPE_CLI_DATA << 2); // NOTE: legacy was: TXT_TYPE_PLAIN

        auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + text_len);
        if (reply) {
          if (client->out_path_len < 0) {
            sendFlood(reply, CLI_REPLY_DELAY_MILLIS);
          } else {
            sendDirect(reply, client->out_path, client->out_path_len, CLI_REPLY_DELAY_MILLIS);
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  }
}

void MyMesh::onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel, uint8_t* data, size_t len) {
  if (type != PAYLOAD_TYPE_GRP_TXT || (!public_channel_ready && !test_channel_ready)) return;
  if (len <= 5 || len >= MAX_PACKET_PAYLOAD) return;

  uint8_t txt_type = data[4] >> 2;
  if (txt_type != TXT_TYPE_PLAIN) return;

  data[len] = 0;  // ensure null-terminated string
  const char* text = (const char*)&data[5];
  char sender_name[40];
  const char* body = findMessageBody(text, _prefs.node_name, sender_name, sizeof(sender_name));

#if defined(ESP32)
  if (public_channel_ready && memcmp(channel.hash, public_channel.hash, PATH_HASH_SIZE) == 0) {
    char any_sender[40];
    const char* any_body = findMessageBodyAllowSelf(text, any_sender, sizeof(any_sender));
    if (any_body) {
      char path_str[384];
      formatPathString(packet->path, packet->path_len, path_str, sizeof(path_str));
      char body_with_hops[320];
      snprintf(body_with_hops, sizeof(body_with_hops),
               "%s [From: \"Public Channel\" Hops: %s]",
               any_body, path_str);
      queueDiscordWebhook("cedarmesh.ca", body_with_hops);
    }
  }
#endif

  if (!body) return;
  if (!isPingChannel(channel)) return;
  bool is_ping = containsSubstringCaseInsensitive(body, "!ping");
  bool is_test = containsSubstringCaseInsensitive(body, "test");
  if (!is_ping && !is_test) return;

  char path_str[384];
  formatPathString(packet->path, packet->path_len, path_str, sizeof(path_str));

  char reply_text[256];
  if (sender_name[0] == 0) {
    StrHelper::strncpy(sender_name, "Unknown", sizeof(sender_name));
  }
  int16_t noise_floor = (int16_t)_radio->getNoiseFloor();
  uint8_t busy_pct = calcBusyPercent();
  snprintf(reply_text, sizeof(reply_text),
           "Pong, @[%s]. Path: %s. NF %ddBm Busy %u%%",
           sender_name, path_str, noise_floor, busy_pct);

  StrHelper::strncpy(pending_ping_reply, reply_text, sizeof(pending_ping_reply));
  pending_ping_channel = channel;
  pending_ping_channel_ready = true;
  pending_ping_retries = PING_REPLY_ATTEMPTS;
  next_ping_send_at = 0;

  if (pending_ping_retries > 0) {
    sendPingReply(pending_ping_channel, pending_ping_reply);
    pending_ping_retries--;
    if (pending_ping_retries > 0) {
      next_ping_send_at = futureMillis(PING_REPLY_RETRY_DELAY_MS);
    }
  }
}

bool MyMesh::onPeerPathRecv(mesh::Packet *packet, int sender_idx, const uint8_t *secret, uint8_t *path,
                            uint8_t path_len, uint8_t extra_type, uint8_t *extra, uint8_t extra_len) {
  // TODO: prevent replay attacks
  int i = matching_peer_indexes[sender_idx];

  if (i >= 0 && i < acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("PATH to client, path_len=%d", (uint32_t)path_len);
    auto client = acl.getClientByIdx(i);

    memcpy(client->out_path, path, client->out_path_len = path_len); // store a copy of path, for sendDirect()
    client->last_activity = getRTCClock()->getCurrentTime();
  } else {
    MESH_DEBUG_PRINTLN("onPeerPathRecv: invalid peer idx: %d", i);
  }

  // NOTE: no reciprocal path send!!
  return false;
}

#define CTL_TYPE_NODE_DISCOVER_REQ   0x80
#define CTL_TYPE_NODE_DISCOVER_RESP  0x90

void MyMesh::onControlDataRecv(mesh::Packet* packet) {
  uint8_t type = packet->payload[0] & 0xF0;    // just test upper 4 bits
  if (type == CTL_TYPE_NODE_DISCOVER_REQ && packet->payload_len >= 6
      && !_prefs.disable_fwd && discover_limiter.allow(rtc_clock.getCurrentTime())
  ) {
    int i = 1;
    uint8_t  filter = packet->payload[i++];
    uint32_t tag;
    memcpy(&tag, &packet->payload[i], 4); i += 4;
    uint32_t since;
    if (packet->payload_len >= i+4) {   // optional since field
      memcpy(&since, &packet->payload[i], 4); i += 4;
    } else {
      since = 0;
    }

    if ((filter & (1 << ADV_TYPE_REPEATER)) != 0 && _prefs.discovery_mod_timestamp >= since) {
      bool prefix_only = packet->payload[0] & 1;
      uint8_t data[6 + PUB_KEY_SIZE];
      data[0] = CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_REPEATER;   // low 4-bits for node type
      data[1] = packet->_snr;   // let sender know the inbound SNR ( x 4)
      memcpy(&data[2], &tag, 4);     // include tag from request, for client to match to
      memcpy(&data[6], self_id.pub_key, PUB_KEY_SIZE);
      auto resp = createControlData(data, prefix_only ? 6 + 8 : 6 + PUB_KEY_SIZE);
      if (resp) {
        sendZeroHop(resp, getRetransmitDelay(resp)*4);  // apply random delay (widened x4), as multiple nodes can respond to this
      }
    }
  }
}

MyMesh::MyMesh(mesh::MainBoard &board, mesh::Radio &radio, mesh::MillisecondClock &ms, mesh::RNG &rng,
               mesh::RTCClock &rtc, mesh::MeshTables &tables)
    : mesh::Mesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      _cli(board, rtc, sensors, acl, &_prefs, this), telemetry(MAX_PACKET_PAYLOAD - 4), region_map(key_store), temp_map(key_store),
      discover_limiter(4, 120),  // max 4 every 2 minutes
      anon_limiter(4, 180)   // max 4 every 3 minutes
#if defined(WITH_RS232_BRIDGE)
      , bridge(&_prefs, WITH_RS232_BRIDGE, _mgr, &rtc)
#endif
#if defined(WITH_ESPNOW_BRIDGE)
      , bridge(&_prefs, _mgr, &rtc)
#endif
{
  last_millis = 0;
  uptime_millis = 0;
  next_local_advert = next_flood_advert = 0;
  dirty_contacts_expiry = 0;
  set_radio_at = revert_radio_at = 0;
  _logging = false;
  public_channel_ready = false;
  test_channel_ready = false;
  region_load_active = false;
  pending_ping_reply[0] = 0;
  pending_ping_retries = 0;
  next_ping_send_at = 0;
  next_public_broadcast_at = 0;
  last_busy_sample_ms = 0;
  last_busy_tx_ms = 0;
  last_busy_rx_ms = 0;
  pending_ping_channel_ready = false;
#if defined(ESP32)
  next_wifi_attempt_at = 0;
  next_webhook_attempt_at = 0;
  webhook_head = 0;
  webhook_tail = 0;
  webhook_count = 0;
#endif

#if MAX_NEIGHBOURS
  memset(neighbours, 0, sizeof(neighbours));
#endif

  // defaults
  memset(&_prefs, 0, sizeof(_prefs));
  _prefs.airtime_factor = 1.0;   // one half
  _prefs.rx_delay_base = 0.0f;   // turn off by default, was 10.0;
  _prefs.tx_delay_factor = 0.5f; // was 0.25f
  _prefs.direct_tx_delay_factor = 0.2f; // was zero
  StrHelper::strncpy(_prefs.node_name, ADVERT_NAME, sizeof(_prefs.node_name));
  _prefs.node_lat = ADVERT_LAT;
  _prefs.node_lon = ADVERT_LON;
  StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, sizeof(_prefs.password));
  _prefs.freq = LORA_FREQ;
  _prefs.sf = LORA_SF;
  _prefs.bw = LORA_BW;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.advert_interval = 1;        // default to 2 minutes for NEW installs
  _prefs.flood_advert_interval = 12; // 12 hours
  _prefs.flood_max = 64;
  _prefs.interference_threshold = 0; // disabled

  // bridge defaults
  _prefs.bridge_enabled = 1;    // enabled
  _prefs.bridge_delay   = 500;  // milliseconds
  _prefs.bridge_pkt_src = 0;    // logTx
  _prefs.bridge_baud = 115200;  // baud rate
  _prefs.bridge_channel = 1;    // channel 1

  StrHelper::strncpy(_prefs.bridge_secret, "LVSITANOS", sizeof(_prefs.bridge_secret));

  // GPS defaults
  _prefs.gps_enabled = 0;
  _prefs.gps_interval = 0;
  _prefs.advert_loc_policy = ADVERT_LOC_PREFS;

  _prefs.adc_multiplier = 0.0f; // 0.0f means use default board multiplier

  _prefs.wifi_ssid[0] = 0;
  _prefs.wifi_pwd[0] = 0;
  _prefs.discord_webhook_url[0] = 0;
  _prefs.ping_public_enabled = 0;
#ifdef WIFI_SSID
  StrHelper::strncpy(_prefs.wifi_ssid, WIFI_SSID, sizeof(_prefs.wifi_ssid));
#endif
#ifdef WIFI_PWD
  StrHelper::strncpy(_prefs.wifi_pwd, WIFI_PWD, sizeof(_prefs.wifi_pwd));
#endif
#ifdef DISCORD_WEBHOOK_URL
  StrHelper::strncpy(_prefs.discord_webhook_url, DISCORD_WEBHOOK_URL, sizeof(_prefs.discord_webhook_url));
#endif
}

void MyMesh::begin(FILESYSTEM *fs) {
  mesh::Mesh::begin();
  _fs = fs;
  // load persisted prefs
  _cli.loadPrefs(_fs);
  acl.load(_fs, self_id);
  // TODO: key_store.begin();
  region_map.load(_fs);
  initPublicChannel();
  initTestChannel();

#if defined(ESP32)
  initWifiClient();
#endif

#if defined(WITH_BRIDGE)
  if (_prefs.bridge_enabled) {
    bridge.begin();
  }
#endif

  radio_set_params(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_set_tx_power(_prefs.tx_power_dbm);

  updateAdvertTimer();
  updateFloodAdvertTimer();
  next_public_broadcast_at = futureMillis(5000);

  board.setAdcMultiplier(_prefs.adc_multiplier);

#if ENV_INCLUDE_GPS == 1
  applyGpsPrefs();
#endif
}

void MyMesh::applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) {
  set_radio_at = futureMillis(2000); // give CLI reply some time to be sent back, before applying temp radio params
  pending_freq = freq;
  pending_bw = bw;
  pending_sf = sf;
  pending_cr = cr;

  revert_radio_at = futureMillis(2000 + timeout_mins * 60 * 1000); // schedule when to revert radio params
}

bool MyMesh::formatFileSystem() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return InternalFS.format();
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  return SPIFFS.format();
#else
#error "need to implement file system erase"
  return false;
#endif
}

void MyMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  mesh::Packet *pkt = createSelfAdvert();
  if (pkt) {
    if (flood) {
      sendFlood(pkt, delay_millis);
    } else {
      sendZeroHop(pkt, delay_millis);
    }
  } else {
    MESH_DEBUG_PRINTLN("ERROR: unable to create advertisement packet!");
  }
}

void MyMesh::updateAdvertTimer() {
  if (_prefs.advert_interval > 0) { // schedule local advert timer
    next_local_advert = futureMillis(((uint32_t)_prefs.advert_interval) * 2 * 60 * 1000);
  } else {
    next_local_advert = 0; // stop the timer
  }
}

void MyMesh::updateFloodAdvertTimer() {
  if (_prefs.flood_advert_interval > 0) { // schedule flood advert timer
    next_flood_advert = futureMillis(((uint32_t)_prefs.flood_advert_interval) * 60 * 60 * 1000);
  } else {
    next_flood_advert = 0; // stop the timer
  }
}

void MyMesh::dumpLogFile() {
#if defined(RP2040_PLATFORM)
  File f = _fs->open(PACKET_LOG_FILE, "r");
#else
  File f = _fs->open(PACKET_LOG_FILE);
#endif
  if (f) {
    while (f.available()) {
      int c = f.read();
      if (c < 0) break;
      Serial.print((char)c);
    }
    f.close();
  }
}

void MyMesh::setTxPower(uint8_t power_dbm) {
  radio_set_tx_power(power_dbm);
}

void MyMesh::formatNeighborsReply(char *reply) {
  char *dp = reply;

#if MAX_NEIGHBOURS
  // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
  int16_t neighbours_count = 0;
  NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    auto neighbour = &neighbours[i];
    if (neighbour->heard_timestamp > 0) {
      sorted_neighbours[neighbours_count] = neighbour;
      neighbours_count++;
    }
  }

  // sort neighbours newest to oldest
  std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
    return a->heard_timestamp > b->heard_timestamp; // desc
  });

  for (int i = 0; i < neighbours_count && dp - reply < 134; i++) {
    NeighbourInfo *neighbour = sorted_neighbours[i];

    // add new line if not first item
    if (i > 0) *dp++ = '\n';

    char hex[10];
    // get 4 bytes of neighbour id as hex
    mesh::Utils::toHex(hex, neighbour->id.pub_key, 4);

    // add next neighbour
    uint32_t secs_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
    sprintf(dp, "%s:%d:%d", hex, secs_ago, neighbour->snr);
    while (*dp)
      dp++; // find end of string
  }
#endif
  if (dp == reply) { // no neighbours, need empty response
    strcpy(dp, "-none-");
    dp += 6;
  }
  *dp = 0; // null terminator
}

void MyMesh::removeNeighbor(const uint8_t *pubkey, int key_len) {
#if MAX_NEIGHBOURS
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    NeighbourInfo *neighbour = &neighbours[i];
    if (memcmp(neighbour->id.pub_key, pubkey, key_len) == 0) {
      neighbours[i] = NeighbourInfo(); // clear neighbour entry
    }
  }
#endif
}

void MyMesh::formatStatsReply(char *reply) {
  StatsFormatHelper::formatCoreStats(reply, board, *_ms, _err_flags, _mgr);
}

void MyMesh::formatRadioStatsReply(char *reply) {
  StatsFormatHelper::formatRadioStats(reply, _radio, radio_driver, getTotalAirTime(), getReceiveAirTime());
}

void MyMesh::formatPacketStatsReply(char *reply) {
  StatsFormatHelper::formatPacketStats(reply, radio_driver, getNumSentFlood(), getNumSentDirect(), 
                                       getNumRecvFlood(), getNumRecvDirect());
}

void MyMesh::saveIdentity(const mesh::LocalIdentity &new_id) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#elif defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#else
#error "need to define saveIdentity()"
#endif
  store.save("_main", new_id);
}

void MyMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables *)getTables())->resetStats();
}

void MyMesh::handleCommand(uint32_t sender_timestamp, char *command, char *reply) {
  if (region_load_active) {
    if (StrHelper::isBlank(command)) {  // empty/blank line, signal to terminate 'load' operation
      region_map = temp_map;  // copy over the temp instance as new current map
      region_load_active = false;

      sprintf(reply, "OK - loaded %d regions", region_map.getCount());
    } else {
      char *np = command;
      while (*np == ' ') np++;   // skip indent
      int indent = np - command;

      char *ep = np;
      while (RegionMap::is_name_char(*ep)) ep++;
      if (*ep) { *ep++ = 0; }  // set null terminator for end of name

      while (*ep && *ep != 'F') ep++;  // look for (optional) flags

      if (indent > 0 && indent < 8 && strlen(np) > 0) {
        auto parent = load_stack[indent - 1];
        if (parent) {
          auto old = region_map.findByName(np);
          auto nw = temp_map.putRegion(np, parent->id, old ? old->id : 0);  // carry-over the current ID (if name already exists)
          if (nw) {
            nw->flags = old ? old->flags : (*ep == 'F' ? 0 : REGION_DENY_FLOOD);   // carry-over flags from curr

            load_stack[indent] = nw;  // keep pointers to parent regions, to resolve parent_id's
          }
        }
      }
      reply[0] = 0;
    }
    return;
  }

  while (*command == ' ') command++; // skip leading spaces

  if (strlen(command) > 4 && command[2] == '|') { // optional prefix (for companion radio CLI)
    memcpy(reply, command, 3);                    // reflect the prefix back
    reply += 3;
    command += 3;
  }

  // handle ACL related commands
  if (memcmp(command, "setperm ", 8) == 0) {   // format:  setperm {pubkey-hex} {permissions-int8}
    char* hex = &command[8];
    char* sp = strchr(hex, ' ');   // look for separator char
    if (sp == NULL) {
      strcpy(reply, "Err - bad params");
    } else {
      *sp++ = 0;   // replace space with null terminator

      uint8_t pubkey[PUB_KEY_SIZE];
      int hex_len = min(sp - hex, PUB_KEY_SIZE*2);
      if (mesh::Utils::fromHex(pubkey, hex_len / 2, hex)) {
        uint8_t perms = atoi(sp);
        if (acl.applyPermissions(self_id, pubkey, hex_len / 2, perms)) {
          dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);   // trigger acl.save()
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Err - invalid params");
        }
      } else {
        strcpy(reply, "Err - bad pubkey");
      }
    }
  } else if (sender_timestamp == 0 && strcmp(command, "get acl") == 0) {
    Serial.println("ACL:");
    for (int i = 0; i < acl.getNumClients(); i++) {
      auto c = acl.getClientByIdx(i);
      if (c->permissions == 0) continue;  // skip deleted (or guest) entries

      Serial.printf("%02X ", c->permissions);
      mesh::Utils::printHex(Serial, c->id.pub_key, PUB_KEY_SIZE);
      Serial.printf("\n");
    }
    reply[0] = 0;
  } else if (memcmp(command, "wifi", 4) == 0) {
#if defined(ESP32)
    char *sub = command + 4;
    if (*sub == '.' || *sub == ' ') sub++;
    while (*sub == ' ') sub++;

    if (*sub == 0 || strcmp(sub, "status") == 0) {
      if (_prefs.wifi_ssid[0] == 0) {
        strcpy(reply, "wifi: off");
      } else if (WiFi.status() == WL_CONNECTED) {
        String ip = WiFi.localIP().toString();
        snprintf(reply, 160, "wifi: ok ip=%s", ip.c_str());
      } else {
        strcpy(reply, "wifi: err");
      }
    } else if (memcmp(sub, "ssid", 4) == 0) {
      const char* val = sub + 4;
      while (*val == ' ') val++;
      if (*val == 0) {
        if (_prefs.wifi_ssid[0]) {
          snprintf(reply, 160, "wifi.ssid=%s", _prefs.wifi_ssid);
        } else {
          strcpy(reply, "wifi.ssid=empty");
        }
      } else {
        StrHelper::strncpy(_prefs.wifi_ssid, val, sizeof(_prefs.wifi_ssid));
        savePrefs();
        initWifiClient();
        strcpy(reply, "OK");
      }
    } else if (memcmp(sub, "pwd", 3) == 0 || memcmp(sub, "pass", 4) == 0) {
      const char* val = (memcmp(sub, "pass", 4) == 0) ? (sub + 4) : (sub + 3);
      while (*val == ' ') val++;
      if (*val == 0) {
        if (_prefs.wifi_pwd[0]) {
          snprintf(reply, 160, "wifi.pwd=*** (len=%u)", (unsigned)strlen(_prefs.wifi_pwd));
        } else {
          strcpy(reply, "wifi.pwd=empty");
        }
      } else {
        StrHelper::strncpy(_prefs.wifi_pwd, val, sizeof(_prefs.wifi_pwd));
        savePrefs();
        initWifiClient();
        strcpy(reply, "OK");
      }
    } else if (memcmp(sub, "test", 4) == 0) {
      if (_prefs.discord_webhook_url[0] == 0) {
        strcpy(reply, "Err - webhook not set");
      } else {
        queueDiscordWebhook("WebhookTest", "test message");
        strcpy(reply, "OK");
      }
    } else if (memcmp(sub, "webhook", 7) == 0) {
      const char* val = sub + 7;
      while (*val == ' ') val++;
      if (*val == 0) {
        if (_prefs.discord_webhook_url[0]) {
          snprintf(reply, 160, "wifi.webhook=set (len=%u)", (unsigned)strlen(_prefs.discord_webhook_url));
        } else {
          strcpy(reply, "wifi.webhook=empty");
        }
      } else if (strcmp(val, "test") == 0) {
        if (_prefs.discord_webhook_url[0] == 0) {
          strcpy(reply, "Err - webhook not set");
        } else {
          queueDiscordWebhook("WebhookTest", "test message");
          strcpy(reply, "OK");
        }
      } else if (strcmp(val, "clear") == 0) {
        _prefs.discord_webhook_url[0] = 0;
        savePrefs();
        strcpy(reply, "OK");
      } else {
        StrHelper::strncpy(_prefs.discord_webhook_url, val, sizeof(_prefs.discord_webhook_url));
        savePrefs();
        strcpy(reply, "OK");
      }
    } else if (strcmp(sub, "connect") == 0) {
      initWifiClient();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - ??");
    }
#else
    strcpy(reply, "Err - unsupported");
#endif
  } else if (memcmp(command, "region", 6) == 0) {
    reply[0] = 0;

    const char* parts[4];
    int n = mesh::Utils::parseTextParts(command, parts, 4, ' ');
    if (n == 1) {
      region_map.exportTo(reply, 160);
    } else if (n >= 2 && strcmp(parts[1], "load") == 0) {
      temp_map.resetFrom(region_map);   // rebuild regions in a temp instance
      memset(load_stack, 0, sizeof(load_stack));
      load_stack[0] = &temp_map.getWildcard();
      region_load_active = true;
    } else if (n >= 2 && strcmp(parts[1], "save") == 0) {
      _prefs.discovery_mod_timestamp = rtc_clock.getCurrentTime();   // this node is now 'modified' (for discovery info)
      savePrefs();
      bool success = region_map.save(_fs);
      strcpy(reply, success ? "OK" : "Err - save failed");
    } else if (n >= 3 && strcmp(parts[1], "allowf") == 0) {
      auto region = region_map.findByNamePrefix(parts[2]);
      if (region) {
        region->flags &= ~REGION_DENY_FLOOD;
        strcpy(reply, "OK");
      } else {
        strcpy(reply, "Err - unknown region");
      }
    } else if (n >= 3 && strcmp(parts[1], "denyf") == 0) {
      auto region = region_map.findByNamePrefix(parts[2]);
      if (region) {
        region->flags |= REGION_DENY_FLOOD;
        strcpy(reply, "OK");
      } else {
        strcpy(reply, "Err - unknown region");
      }
    } else if (n >= 3 && strcmp(parts[1], "get") == 0) {
      auto region = region_map.findByNamePrefix(parts[2]);
      if (region) {
        auto parent = region_map.findById(region->parent);
        if (parent && parent->id != 0) {
          sprintf(reply, " %s (%s) %s", region->name, parent->name, (region->flags & REGION_DENY_FLOOD) ? "" : "F");
        } else {
          sprintf(reply, " %s %s", region->name, (region->flags & REGION_DENY_FLOOD) ? "" : "F");
        }
      } else {
        strcpy(reply, "Err - unknown region");
      }
    } else if (n >= 3 && strcmp(parts[1], "home") == 0) {
      auto home = region_map.findByNamePrefix(parts[2]);
      if (home) {
        region_map.setHomeRegion(home);
        sprintf(reply, " home is now %s", home->name);
      } else {
        strcpy(reply, "Err - unknown region");
      }
    } else if (n == 2 && strcmp(parts[1], "home") == 0) {
      auto home = region_map.getHomeRegion();
      sprintf(reply, " home is %s", home ? home->name : "*");
    } else if (n >= 3 && strcmp(parts[1], "put") == 0) {
      auto parent = n >= 4 ? region_map.findByNamePrefix(parts[3]) : &region_map.getWildcard();
      if (parent == NULL) {
        strcpy(reply, "Err - unknown parent");
      } else {
        auto region = region_map.putRegion(parts[2], parent->id);
        if (region == NULL) {
          strcpy(reply, "Err - unable to put");
        } else {
          strcpy(reply, "OK");
        }
      }
    } else if (n >= 3 && strcmp(parts[1], "remove") == 0) {
      auto region = region_map.findByName(parts[2]);
      if (region) {
        if (region_map.removeRegion(*region)) {
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Err - not empty");
        }
      } else {
        strcpy(reply, "Err - not found");
      }
    } else if (n >= 3 && strcmp(parts[1], "list") == 0) {
      uint8_t mask = 0;
      bool invert = false;
      
      if (strcmp(parts[2], "allowed") == 0) {
        mask = REGION_DENY_FLOOD;
        invert = false;  // list regions that DON'T have DENY flag
      } else if (strcmp(parts[2], "denied") == 0) {
        mask = REGION_DENY_FLOOD;
        invert = true;   // list regions that DO have DENY flag
      } else {
        strcpy(reply, "Err - use 'allowed' or 'denied'");
        return;
      }
      
      int len = region_map.exportNamesTo(reply, 160, mask, invert);
      if (len == 0) {
        strcpy(reply, "-none-");
      }
    } else {
      strcpy(reply, "Err - ??");
    }
  } else if (memcmp(command, "ping.public", 11) == 0) {
    const char* val = command + 11;
    while (*val == ' ') val++;
    if (*val == 0) {
      strcpy(reply, _prefs.ping_public_enabled ? "ping.public=on" : "ping.public=off");
    } else if (strcmp(val, "on") == 0 || strcmp(val, "1") == 0) {
      _prefs.ping_public_enabled = 1;
      savePrefs();
      strcpy(reply, "OK");
    } else if (strcmp(val, "off") == 0 || strcmp(val, "0") == 0) {
      _prefs.ping_public_enabled = 0;
      savePrefs();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - ??");
    }
  } else{
    _cli.handleCommand(sender_timestamp, command, reply);  // common CLI commands
  }
}

void MyMesh::loop() {
#ifdef WITH_BRIDGE
  bridge.loop();
#endif

  mesh::Mesh::loop();

#if defined(ESP32)
  pumpDiscordWebhook();
#endif

  if (next_flood_advert && millisHasNowPassed(next_flood_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    if (pkt) sendFlood(pkt);

    updateFloodAdvertTimer(); // schedule next flood advert
    updateAdvertTimer();      // also schedule local advert (so they don't overlap)
  } else if (next_local_advert && millisHasNowPassed(next_local_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    if (pkt) sendZeroHop(pkt);

    updateAdvertTimer(); // schedule next local advert
  }

  if (set_radio_at && millisHasNowPassed(set_radio_at)) { // apply pending (temporary) radio params
    set_radio_at = 0;                                     // clear timer
    radio_set_params(pending_freq, pending_bw, pending_sf, pending_cr);
    MESH_DEBUG_PRINTLN("Temp radio params");
  }

  if (revert_radio_at && millisHasNowPassed(revert_radio_at)) { // revert radio params to orig
    revert_radio_at = 0;                                        // clear timer
    radio_set_params(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
    MESH_DEBUG_PRINTLN("Radio params restored");
  }

  // is pending dirty contacts write needed?
  if (dirty_contacts_expiry && millisHasNowPassed(dirty_contacts_expiry)) {
    acl.save(_fs);
    dirty_contacts_expiry = 0;
  }

  if (pending_ping_retries && next_ping_send_at && millisHasNowPassed(next_ping_send_at)) {
    if (pending_ping_channel_ready) {
      sendPingReply(pending_ping_channel, pending_ping_reply);
    }
    pending_ping_retries--;
    if (pending_ping_retries) {
      next_ping_send_at = futureMillis(PING_REPLY_RETRY_DELAY_MS);
    } else {
      next_ping_send_at = 0;
    }
  }

  if (next_public_broadcast_at && millisHasNowPassed(next_public_broadcast_at)) {
    if (test_channel_ready) {
      sendPublicBroadcast(test_channel);
    }
    next_public_broadcast_at = futureMillis(TEST_BROADCAST_INTERVAL_MS);
  }

  // update uptime
  uint32_t now = millis();
  uptime_millis += now - last_millis;
  last_millis = now;
}

// To check if there is pending work
bool MyMesh::hasPendingWork() const {
  return _mgr->getOutboundCount(0xFFFFFFFF) > 0;
}
