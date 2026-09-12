bool installFirmwareFromUrl(const String& url, String expectedSha256, String& result) {
  if (!(url.startsWith("http://") || url.startsWith("https://"))) {
    result = "URL must start with http:// or https://";
    return false;
  }
  if (!validSha256(expectedSha256)) {
    result = "SHA-256 must be blank or exactly 64 hexadecimal characters";
    return false;
  }
  expectedSha256.toLowerCase();
  expectedSha256.trim();

  gpio23Forced = false;
  gpio23ForceUntil = 0;
  setLockOutput(false, "remote-ota");

  HTTPClient http;
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  bool isHttps = url.startsWith("https://");
  bool began = false;

  if (isHttps) {
    // Keeps remote OTA practical without embedding a CA certificate that can expire/change.
    // For integrity, use the optional expected SHA-256 field in the WebUI.
    secureClient.setInsecure();
    began = http.begin(secureClient, url);
  } else {
    began = http.begin(plainClient, url);
  }
  if (!began) {
    result = "Could not open firmware URL";
    return false;
  }

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    result = String("Firmware server returned HTTP ") + code;
    http.end();
    return false;
  }

  int totalSize = http.getSize();
  if (totalSize == 0) {
    result = "Firmware download was empty";
    http.end();
    return false;
  }

  addLog(String("[REMOTE OTA] GET ") + url +
         " size=" + totalSize + (expectedSha256.length() ? " sha256=yes" : " sha256=no"));

  size_t beginSize = totalSize > 0 ? (size_t)totalSize : UPDATE_SIZE_UNKNOWN;
  if (!Update.begin(beginSize, U_FLASH)) {
    result = String("Update.begin failed: ") + Update.errorString();
    addLog(String("[REMOTE OTA] ") + result);
    http.end();
    return false;
  }

  mbedtls_md_context_t shaCtx;
  mbedtls_md_init(&shaCtx);
  const mbedtls_md_info_t* shaInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!shaInfo || mbedtls_md_setup(&shaCtx, shaInfo, 0) != 0 || mbedtls_md_starts(&shaCtx) != 0) {
    Update.abort();
    mbedtls_md_free(&shaCtx);
    http.end();
    result = "Could not initialize SHA-256";
    return false;
  }

  auto* stream = http.getStreamPtr();
  uint8_t buffer[4096];
  size_t writtenTotal = 0;
  int remaining = totalSize;
  uint32_t lastDataMs = millis();
  bool streamOk = true;

  while ((http.connected() || stream->available()) && (remaining > 0 || totalSize < 0)) {
    size_t available = stream->available();
    if (available) {
      size_t want = available;
      if (want > sizeof(buffer)) want = sizeof(buffer);
      if (remaining > 0 && want > (size_t)remaining) want = remaining;
      int got = stream->readBytes(buffer, want);
      if (got <= 0) {
        streamOk = false;
        result = "Firmware download stopped unexpectedly";
        break;
      }
      lastDataMs = millis();
      mbedtls_md_update(&shaCtx, buffer, got);
      size_t wrote = Update.write(buffer, got);
      if (wrote != (size_t)got) {
        streamOk = false;
        result = String("Flash write failed: ") + Update.errorString();
        break;
      }
      writtenTotal += wrote;
      if (remaining > 0) remaining -= got;
      delay(0);
    } else {
      if (millis() - lastDataMs > 15000) {
        streamOk = false;
        result = "Firmware download timed out";
        break;
      }
      delay(2);
    }
  }

  uint8_t digest[32];
  mbedtls_md_finish(&shaCtx, digest);
  mbedtls_md_free(&shaCtx);
  http.end();

  if (totalSize > 0 && writtenTotal != (size_t)totalSize) {
    streamOk = false;
    result = String("Incomplete firmware: received ") + writtenTotal + " of " + totalSize + " bytes";
  }

  String actualSha = bytesToHex(digest, sizeof(digest));
  if (streamOk && expectedSha256.length() && actualSha != expectedSha256) {
    streamOk = false;
    result = String("SHA-256 mismatch. Downloaded: ") + actualSha;
  }

  if (!streamOk) {
    Update.abort();
    addLog(String("[REMOTE OTA] aborted: ") + result);
    return false;
  }

  if (!Update.end(true)) {
    result = String("Update.end failed: ") + Update.errorString();
    addLog(String("[REMOTE OTA] ") + result);
    return false;
  }

  result = String("Firmware installed (") + writtenTotal + " bytes). SHA-256: " + actualSha + ". Rebooting now...";
  addLog(String("[REMOTE OTA] success bytes=") + writtenTotal + " sha256=" + actualSha);
  return true;
}

