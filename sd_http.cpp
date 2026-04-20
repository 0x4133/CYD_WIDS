#include "sd_http.h"

#include "sd_writer.h"
#include <Arduino.h>
#include <SD.h>
#include <WebServer.h>
#include <WiFi.h>

static WebServer server(80);
static bool gRunning = false;
static char gUrl[64] = "";
static File gUploadFile;
static char gUploadPath[96] = "";

static void sanitizeName(const String& in, char* out, size_t cap) {
  size_t j = 0;
  for (size_t i = 0; i < in.length() && j + 1 < cap; i++) {
    char c = in[i];
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
        c == '"' || c == '<' || c == '>' || c == '|') continue;
    out[j++] = c;
  }
  if (j == 0) {
    strlcpy(out, "upload.bin", cap);
    return;
  }
  out[j] = 0;
}

static String htmlEscape(const char* s) {
  String out;
  while (*s) {
    char c = *s++;
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

static void handleRoot() {
  if (!sdWriterReady()) {
    server.send(503, "text/plain", "SD card not ready");
    return;
  }
  String html;
  html.reserve(4096);
  html += "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>CYD SD Card</title></head><body>";
  html += "<h2>CYD SD Card</h2>";
  html += "<form method='POST' action='/upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='file'><button type='submit'>Upload</button></form>";
  html += "<p><a href='/'>Refresh</a></p><hr><ul>";

  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    server.send(500, "text/plain", "failed to open root");
    return;
  }
  File f;
  while ((f = root.openNextFile())) {
    String name = f.name();
    const char* last = strrchr(name.c_str(), '/');
    String base = last ? String(last + 1) : name;
    if (base.length() == 0) { f.close(); continue; }
    if (f.isDirectory()) {
      html += "<li>[DIR] ";
      html += htmlEscape(base.c_str());
      html += "</li>";
    } else {
      html += "<li>";
      html += htmlEscape(base.c_str());
      html += " (" + String((unsigned long)f.size()) + " bytes) ";
      html += "<a href='/download?name=" + base + "'>download</a> ";
      html += "<a href='/delete?name=" + base + "' onclick=\"return confirm('Delete file?')\">delete</a>";
      html += "</li>";
    }
    f.close();
  }
  root.close();
  html += "</ul></body></html>";
  server.send(200, "text/html", html);
}

static bool buildPathFromName(char* out, size_t outN) {
  if (!server.hasArg("name")) return false;
  char clean[48];
  sanitizeName(server.arg("name"), clean, sizeof(clean));
  if (!clean[0]) return false;
  snprintf(out, outN, "/%s", clean);
  return true;
}

static void handleDownload() {
  if (!sdWriterReady()) {
    server.send(503, "text/plain", "SD card not ready");
    return;
  }
  char path[96];
  if (!buildPathFromName(path, sizeof(path))) {
    server.send(400, "text/plain", "missing name");
    return;
  }
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    server.send(404, "text/plain", "not found");
    return;
  }
  server.sendHeader("Content-Type", "application/octet-stream");
  server.sendHeader("Content-Disposition", String("attachment; filename=\"") + (path + 1) + "\"");
  server.streamFile(f, "application/octet-stream");
  f.close();
}

static void handleDelete() {
  if (!sdWriterReady()) {
    server.send(503, "text/plain", "SD card not ready");
    return;
  }
  char path[96];
  if (!buildPathFromName(path, sizeof(path))) {
    server.send(400, "text/plain", "missing name");
    return;
  }
  if (SD.exists(path)) SD.remove(path);
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleUploadDone() {
  if (gUploadFile) gUploadFile.close();
  gUploadPath[0] = 0;
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleUploadStream() {
  if (!sdWriterReady()) return;
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (gUploadFile) gUploadFile.close();
    char clean[48];
    sanitizeName(upload.filename, clean, sizeof(clean));
    snprintf(gUploadPath, sizeof(gUploadPath), "/%s", clean);
    if (SD.exists(gUploadPath)) SD.remove(gUploadPath);
    gUploadFile = SD.open(gUploadPath, FILE_WRITE);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (gUploadFile) gUploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
    if (gUploadFile) gUploadFile.close();
    gUploadPath[0] = 0;
  }
}

void sdHttpBegin() {
  if (!sdWriterReady()) {
    Serial.println("[SDHTTP] SD not ready");
    return;
  }
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP("CYD-SD", "12345678")) {
    Serial.println("[SDHTTP] softAP failed");
    return;
  }

  IPAddress ip = WiFi.softAPIP();
  snprintf(gUrl, sizeof(gUrl), "http://%u.%u.%u.%u/", ip[0], ip[1], ip[2], ip[3]);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/delete", HTTP_GET, handleDelete);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUploadStream);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
  gRunning = true;
  Serial.printf("[SDHTTP] ready at %s\n", gUrl);
}

void sdHttpTick() {
  if (gRunning) server.handleClient();
}

bool sdHttpRunning() { return gRunning; }

const char* sdHttpUrl() { return gUrl; }
