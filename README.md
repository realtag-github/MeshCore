## HOW TO BUILD

On Ubuntu or Debian try running

chown +x build_heltec_v4_repeater.sh
./build_heltec_v4_repeater.sh
cd .pio/build/heltec_v4_repeater/

and if all goes well it will build a binary inside .pio/build/heltec_v4_repeater/

## 🧰 Repeater CLI (WiFi/Webhook)

These commands are available in the Simple Repeater serial console (and via remote admin).

- Example:
  - `wifi.ssid MyWiFi`
  - `wifi.pwd MySecretPass`
  - `wifi.webhook https://discord.com/api/webhooks/...`
  - `wifi.webhook test`
  - `ping.public off`

- `wifi.status` — show WiFi state (off/err/ok + IP).
- `wifi.ssid <ssid>` — set SSID and save to prefs.
- `wifi.pwd <password>` — set WiFi password and save to prefs.
- `wifi.webhook <url>` — set Discord webhook URL and save to prefs.
- `wifi.webhook clear` — clear the stored webhook URL.
- `wifi.webhook test` / `wifi.test` — send a test webhook message.
- `wifi.connect` — force a reconnect using saved credentials.
- `ping.public` — show whether public `!ping` replies are enabled.
- `ping.public on|off` — enable/disable public `!ping` replies.

## 📟 LCD (Heltec v3/v4)

On Heltec boards, the repeater LCD shows WiFi status:

- `WiFi:OFF` — SSID not set.
- `WiFi:ERR` — SSID set but not connected.
- `WiFi:OK <ip>` — connected and showing the IP address.

## ⏱ Test Message

The repeater sends an hourly report message to #test channel. 
