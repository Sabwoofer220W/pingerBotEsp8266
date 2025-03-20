#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
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

struct DatabaseConfig {
  String dbType;
  String host;
  int port;
};
std::vector<DatabaseConfig> databases;

// Флаги для подтверждения удаления конфигурации
bool pendingDeleteConfirmation = false;
String pendingDeleteChatId = "";

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
    // Создаем два массива: hosts и databases
    JsonArray hostsArray = doc.createNestedArray("hosts");
    JsonArray dbArray = doc.createNestedArray("databases");
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

  // Загружаем хосты
  JsonArray hostArray = doc["hosts"].as<JsonArray>();
  hosts.clear();
  for (JsonVariant v : hostArray) {
    hosts.push_back(v.as<String>());
  }
  
  // Загружаем конфигурации баз данных
  JsonArray dbArray = doc["databases"].as<JsonArray>();
  databases.clear();
  for (JsonVariant v : dbArray) {
    DatabaseConfig db;
    db.dbType = v["db_type"].as<String>();
    db.host = v["host"].as<String>();
    db.port = v["port"].as<int>();
    databases.push_back(db);
  }
}

void saveConfig() {
  StaticJsonDocument<512> doc;
  JsonArray hostsArray = doc.createNestedArray("hosts");
  for (const auto& host : hosts) {
    hostsArray.add(host);
  }
  
  JsonArray dbArray = doc.createNestedArray("databases");
  for (const auto& db : databases) {
    JsonObject obj = dbArray.createNestedObject();
    obj["db_type"] = db.dbType;
    obj["host"] = db.host;
    obj["port"] = db.port;
  }
  
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
      // Добавление обычного хоста
      String host = text.substring(5);
      if (!host.isEmpty()) {
        hosts.push_back(host);
        saveConfig();
        bot.sendMessage(chat_id, "Added host: " + host, "");
      }
    } else if (text.startsWith("/remove ")) {
      // Удаление хоста по id
      String idStr = text.substring(8);
      idStr.trim();
      int id = idStr.toInt();
      if (id <= 0 || id > hosts.size()) {
        bot.sendMessage(chat_id, F("⚠️ Неверный id хоста."), "");
      } else {
        String removedHost = hosts[id - 1];
        hosts.erase(hosts.begin() + (id - 1));
        saveConfig();
        bot.sendMessage(chat_id, "Removed host: " + removedHost, "");
      }
    } else if (text.startsWith("/add_db ")) {
      // Формат команды: /add_db <db_type> <host> <port>
      String params = text.substring(8);
      params.trim();
      int firstSpace = params.indexOf(' ');
      if (firstSpace == -1) {
        bot.sendMessage(chat_id, F("⚠️ Неверный формат. Используйте: /add_db <db_type> <host> <port>"), "");
        continue;
      }
      String dbType = params.substring(0, firstSpace);
      String remaining = params.substring(firstSpace + 1);
      remaining.trim();
      int secondSpace = remaining.indexOf(' ');
      if (secondSpace == -1) {
        bot.sendMessage(chat_id, F("⚠️ Неверный формат. Используйте: /add_db <db_type> <host> <port>"), "");
        continue;
      }
      String hostParam = remaining.substring(0, secondSpace);
      String portStr = remaining.substring(secondSpace + 1);
      portStr.trim();
      int port = portStr.toInt();
      if (port <= 0) {
        bot.sendMessage(chat_id, F("⚠️ Неверный номер порта."), "");
        continue;
      }
      DatabaseConfig db;
      db.dbType = dbType;
      db.host = hostParam;
      db.port = port;
      databases.push_back(db);
      saveConfig();
      bot.sendMessage(chat_id, "Added DB: " + dbType + " " + hostParam + ":" + String(port), "");
    } else if (text.startsWith("/remove_db ")) {
      // Удаление конфигурации БД по id
      String idStr = text.substring(11);
      idStr.trim();
      int id = idStr.toInt();
      if (id <= 0 || id > databases.size()) {
        bot.sendMessage(chat_id, F("⚠️ Неверный id базы данных."), "");
      } else {
        DatabaseConfig removedDb = databases[id - 1];
        databases.erase(databases.begin() + (id - 1));
        saveConfig();
        bot.sendMessage(chat_id, "Removed DB: " + removedDb.dbType + " " + removedDb.host + ":" + String(removedDb.port), "");
      }
    } else if (text == "/config") {
      // Вывод конфигурации: сначала список хостов, затем настроек БД
      String configMessage = "🔧 Конфигурация:\n\n";
      configMessage += "Хосты:\n";
      if (hosts.empty()) {
        configMessage += "  Нет хостов.\n";
      } else {
        for (int i = 0; i < hosts.size(); i++) {
          configMessage += String(i + 1) + ". " + hosts[i] + "\n";
        }
      }
      configMessage += "\nБазы данных:\n";
      if (databases.empty()) {
        configMessage += "  Нет конфигураций баз данных.\n";
      } else {
        for (int i = 0; i < databases.size(); i++) {
          DatabaseConfig db = databases[i];
          configMessage += String(i + 1) + ". " + db.dbType + " " + db.host + ":" + String(db.port) + "\n";
        }
      }
      bot.sendMessage(chat_id, configMessage, "");
    } else if (text == "/delete_config" && chat_id == "406758980") {
      // Запрос подтверждения на удаление конфигурационного файла
      pendingDeleteConfirmation = true;
      pendingDeleteChatId = chat_id;
      bot.sendMessage(chat_id, F("⚠️ Вы уверены, что хотите удалить конфигурационный файл? Для подтверждения введите /confirm_delete"), "");
    } else if (text == "/confirm_delete" && pendingDeleteConfirmation && chat_id == pendingDeleteChatId) {
      if (SPIFFS.exists(configFile)) {
        SPIFFS.remove(configFile);
        hosts.clear();
        databases.clear();
        bot.sendMessage(chat_id, F("⚠️ Файл конфигурации удалён."), "");
      } else {
        bot.sendMessage(chat_id, F("⚠️ Файл конфигурации не существует."), "");
      }
      pendingDeleteConfirmation = false;
      pendingDeleteChatId = "";
    } else if (text == "/fs_status") {
      // Получение информации о файловой системе
      FSInfo fs_info;
      SPIFFS.info(fs_info);
      unsigned long totalBytes = fs_info.totalBytes;
      unsigned long usedBytes = fs_info.usedBytes;
      int percentage = (usedBytes * 100) / totalBytes;
      // Формирование графической шкалы заполнения (10 блоков)
      int totalBlocks = 10;
      int usedBlocks = (percentage * totalBlocks) / 100;
      String bar = "";
      for (int j = 0; j < usedBlocks; j++) {
        bar += "🟩";
      }
      for (int j = usedBlocks; j < totalBlocks; j++) {
        bar += "⬜";
      }
      String message = "📂 Файловая система:\n";
      message += "Занято: " + String(usedBytes) + " байт / " + String(totalBytes) + " байт (" + String(percentage) + "%)\n";
      message += bar;
      
      // Вывод структуры файловой системы
      message += "\n\n📄 Структура файловой системы:\n";
      Dir dir = SPIFFS.openDir("/");
      while (dir.next()) {
        String fileName = dir.fileName();
        size_t fileSize = dir.fileSize();
        message += fileName + " (" + String(fileSize) + " байт)\n";
      }
      bot.sendMessage(chat_id, message, "");
    } else if (text.startsWith("/db_check ")) {
      // Формат команды: /db_check <db_type> <host> <port>
      String params = text.substring(10);
      params.trim();
      int firstSpace = params.indexOf(' ');
      if (firstSpace == -1) {
        bot.sendMessage(chat_id, F("⚠️ Неверный формат. Используйте: /db_check <db_type> <host> <port>"), "");
        continue;
      }
      String dbType = params.substring(0, firstSpace);
      String remaining = params.substring(firstSpace + 1);
      remaining.trim();
      int secondSpace = remaining.indexOf(' ');
      if (secondSpace == -1) {
        bot.sendMessage(chat_id, F("⚠️ Неверный формат. Используйте: /db_check <db_type> <host> <port>"), "");
        continue;
      }
      String hostParam = remaining.substring(0, secondSpace);
      String portStr = remaining.substring(secondSpace + 1);
      portStr.trim();
      int port = portStr.toInt();
      if (port <= 0) {
        bot.sendMessage(chat_id, F("⚠️ Неверный номер порта."), "");
        continue;
      }
      
      // Попытка установить TCP-соединение для проверки доступности БД
      WiFiClient dbClient;
      bot.sendMessage(chat_id, "Проверка БД " + dbType + " по адресу " + hostParam + ":" + String(port) + "...", "");
      if (dbClient.connect(hostParam.c_str(), port)) {
        dbClient.stop();
        bot.sendMessage(chat_id, "✅ База данных " + dbType + " на " + hostParam + ":" + String(port) + " доступна.", "");
      } else {
        bot.sendMessage(chat_id, "❌ База данных " + dbType + " на " + hostParam + ":" + String(port) + " недоступна.", "");
      }
    } else if (text == "/help") {
      // Отправляем информацию о доступных командах
      String helpMessage = "🛠 Доступные команды:\n\n";
      helpMessage += "/ping_all - Пингует все хосты, указанные в конфиге.\n";
      helpMessage += "/ping <host> - Пингует указанный хост.\n";
      helpMessage += "/add <host> - Добавляет новый хост для пинга.\n";
      helpMessage += "/remove <id> - Удаляет хост по его id из списка для пинга.\n";
      helpMessage += "/add_db <db_type> <host> <port> - Добавляет новую конфигурацию базы данных.\n";
      helpMessage += "/remove_db <id> - Удаляет базу данных по ее id из списка.\n";
      helpMessage += "/config - Показывает содержимое конфигурации (хосты и базы данных).\n";
      helpMessage += "/fs_status - Выводит заполненность файловой системы с графическим отображением и структурой файлов.\n";
      helpMessage += "/db_check <db_type> <host> <port> - Проверяет работоспособность базы данных указанного типа.\n";
      helpMessage += "/delete_config - Запускает процедуру удаления файла конфигурации (требует подтверждения).\n";
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
    // Пинг обычных хостов
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
    // Проверка баз данных
    if (!databases.empty()) {
      for (auto& db : databases) {
        WiFiClient dbClient;
        if (!dbClient.connect(db.host.c_str(), db.port)) {
          for (auto& admin : admins) {
            bot.sendMessage(admin, db.dbType + " database at " + db.host + ":" + String(db.port) + " is offline!", "");
          }
        }
      }
    }
    lastPingTime = millis();
  }
}
