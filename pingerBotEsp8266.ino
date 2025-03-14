#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <ESP8266Ping.h>
#include <FS.h>

const char* ssid = "";
const char* password = "";
const char* botToken = "";
WiFiClientSecure client;
UniversalTelegramBot bot(botToken, client);

const long pingInterval = 300000; // 5 минут
unsigned long lastPingTime = 0;
unsigned long lastMessageCheck = 0;
const long messageCheckInterval = 2000; // Проверка сообщений каждые 2 сек.

const String configFile = "/config.json";
std::vector<String> admins = {"406758980", "6954297693", "391877634"};
std::vector<String> hosts;

void logMessage(const String& message) {
  Serial.println(message);
}

void connectWiFi() {
  Serial.print(F("Connecting to Wi-Fi: "));
  WiFi.begin(ssid, password);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 20) {
    delay(500);
    Serial.print(".");
    attempt++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\nConnected to Wi-Fi"));
  } else {
    Serial.println(F("\nFailed to connect to Wi-Fi"));
  }
}

bool fileExists(const String& path) {
  File file = SPIFFS.open(path, "r");
  bool exists = file;
  file.close();
  return exists;
}

void loadConfig() {
  if (!fileExists(configFile)) {
    logMessage(F("Config file not found. Creating new config..."));
    StaticJsonDocument<512> doc;
    JsonArray hostsArray = doc.createNestedArray("hosts");
    File file = SPIFFS.open(configFile, "w");
    serializeJson(doc, file);
    file.close();
    logMessage(F("Config file created."));
    return;
  }

  File file = SPIFFS.open(configFile, "r");
  if (!file) return;
  StaticJsonDocument<512> doc;
  deserializeJson(doc, file);
  file.close();

  JsonArray hostArray = doc["hosts"].as<JsonArray>();
  if (hostArray.size() == 0) {
    logMessage(F("No hosts found in config."));
    return;
  }

  hosts.clear();
  for (JsonVariant v : hostArray) {
    hosts.push_back(v.as<String>());
  }
}

void saveConfig() {
  StaticJsonDocument<512> doc;
  JsonArray array = doc.createNestedArray("hosts");
  for (const auto& host : hosts) array.add(host);
  File file = SPIFFS.open(configFile, "w");
  serializeJson(doc, file);
  file.close();
}

bool isAdmin(const String& chat_id) {
  return std::find(admins.begin(), admins.end(), chat_id) != admins.end();
}

void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;

    if (!isAdmin(chat_id)) {
      bot.sendMessage(chat_id, F("⚠️ У вас нет доступа к этому боту."), "");
      continue;
    }

    if (text == "/ping_all") {
      if (hosts.empty()) {
        bot.sendMessage(chat_id, F("⚠️ Нет хостов для пинга."), "");
      } else {
        for (auto& host : hosts) {
          bot.sendMessage(chat_id, "Pinging " + host + "...", "");
          bool success = Ping.ping(host.c_str(), 3);
          bot.sendMessage(chat_id, host + " is " + (success ? "online" : "offline"), "");
        }
      }
    } else if (text.startsWith("/ping ")) {
      String host = text.substring(6);
      if (host.isEmpty()) {
        bot.sendMessage(chat_id, F("⚠️ Хост не может быть пустым."), "");
        continue;
      }
      bool success = Ping.ping(host.c_str(), 3);
      bot.sendMessage(chat_id, host + " is " + (success ? "online" : "offline"), "");
    } else if (text.startsWith("/add ")) {
      String host = text.substring(5);
      if (!host.isEmpty()) {
        hosts.push_back(host);
        saveConfig();
        bot.sendMessage(chat_id, "Added " + host, "");
      }
    } else if (text.startsWith("/remove ")) {
      String host = text.substring(8);
      auto it = std::find(hosts.begin(), hosts.end(), host);
      if (it != hosts.end()) {
        hosts.erase(it);
        saveConfig();
        bot.sendMessage(chat_id, "Removed " + host, "");
      } else {
        bot.sendMessage(chat_id, "Host not found in config.", "");
      }
    } else if (text == "/config") {
      if (hosts.empty()) {
        bot.sendMessage(chat_id, F("⚠️ В конфиге нет хостов для пинга."), "");
      } else {
        String configMessage = F("🔧 Содержимое конфигурации:\n");
        for (const auto& host : hosts) {
          configMessage += host + "\n";
        }
        bot.sendMessage(chat_id, configMessage, "");
      }
    } else if (text == "/delete_config" && chat_id == "406758980") {
      if (SPIFFS.exists(configFile)) {
        SPIFFS.remove(configFile);
        hosts.clear();
        bot.sendMessage(chat_id, F("⚠️ Файл конфигурации удалён."), "");
      } else {
        bot.sendMessage(chat_id, F("⚠️ Файл конфигурации не существует."), "");
      }
    } else if (text == "/help") {
      // Отправляем информацию о доступных командах
      String helpMessage = "🛠 Доступные команды:\n\n";
      helpMessage += "/ping_all - Пингует все хосты, указанные в конфиге.\n";
      helpMessage += "/ping <host> - Пингует указанный хост.\n";
      helpMessage += "/add <host> - Добавляет новый хост для пинга.\n";
      helpMessage += "/remove <host> - Удаляет хост из списка для пинга.\n";
      helpMessage += "/config - Показывает содержимое конфигурации.\n";
      helpMessage += "/delete_config - Удаляет файл конфигурации.\n";
      helpMessage += "/help - Показывает это сообщение.\n";
      bot.sendMessage(chat_id, helpMessage, "");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println(F("Starting ESP8266 Telegram Bot..."));
  
  SPIFFS.begin();
  connectWiFi();
  loadConfig();
  client.setInsecure();
  Serial.println(F("Telegram bot initialized."));
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("Wi-Fi lost, reconnecting..."));
    connectWiFi();
  }

  if (millis() - lastMessageCheck > messageCheckInterval) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    if (numNewMessages) {
      handleNewMessages(numNewMessages);
    }
    lastMessageCheck = millis();
  }

  if (millis() - lastPingTime > pingInterval) {
    if (!hosts.empty()) {
      for (auto& host : hosts) {
        bool success = Ping.ping(host.c_str(), 3);
        if (!success) {
          for (auto& admin : admins) {
            bot.sendMessage(admin, host + " is offline!", "");
          }
        }
      }
    }
    lastPingTime = millis();
  }
}
