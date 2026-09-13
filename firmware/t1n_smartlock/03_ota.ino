static String resolveRedirectUrl(const String& currentUrl, String location) {
  location.trim();
  if (location.startsWith("http://") || location.startsWith("https://")) return location;

  int schemeEnd = currentUrl.indexOf("://");
  if (schemeEnd < 0) return location;

  String scheme = currentUrl.substring(0, schemeEnd);
  if (location.startsWith("//")) return scheme + ":" + location;

  int hostStart = schemeEnd + 3;
  int pathStart = currentUrl.indexOf('/', hostStart);
  String origin = pathStart < 0 ? currentUrl : currentUrl.substring(0, pathStart);

  if (location.startsWith("/")) return origin + location;

  String base = pathStart < 0 ? currentUrl + "/" : currentUrl.substring(0, currentUrl.lastIndexOf('/') + 1);
  return base + location;
}

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

  String currentUrl = url;
  const uint8_t maxRedirects = 6;

  for (uint8_t redirectCount = 0; redirectCount <= maxRedirects; redirectCount++) {
    HTTPClient http;
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    bool isHttps = currentUrl.startsWith("https://");

    // A new client is created for every hop. This avoids reusing a TLS connection
    // when GitHub redirects the release URL to a different HTTPS host.
    bool began = false;
    if (isHttps) {
      secureClient.setInsecure();
      began = http.begin(secureClient, currentUrl);
    } else {
      began = http.begin(plainClient, currentUrl);
    }

    if (!began) {
      result = String("Could not open firmware URL: ") + currentUrl;
      return false;
    }

    http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setConnectTimeout(15000);
    http.setTimeout(15000);

    addLog(String("[REMOTE OTA] GET hop ") + redirectCount + " " + currentUrl);
    int code = http.GET();

    if (code == HTTP_CODE_MOVED_PERMANENTLY || code == HTTP_CODE_FOUND ||
        code == HTTP_CODE_SEE_OTHER || code == HTTP_CODE_TEMPORARY_REDIRECT ||
        code == HTTP_CODE_PERMANENT_REDIRECT) {
      String nextUrl = resolveRedirectUrl(currentUrl, http.getLocation());
      http.end();

      if (!(nextUrl.startsWith("http://") || nextUrl.startsWith("https://"))) {
        result = String("Redirect did not provide a usable URL: ") + nextUrl;
        return false;
      }
      if (redirectCount >= maxRedirects) {
        result = "Too many firmware download redirects";
        return false;
      }

      addLog(String("[REMOTE OTA] redirect -> ") + nextUrl);
      currentUrl = nextUrl;
      delay(10);
      continue;
    }

    if (code != HTTP_CODE_OK) {
      if (code < 0) {
        result = String("Firmware connection failed ") + code + ": " + HTTPClient::errorToString(code);
      } else {
        result = String("Firmware server returned HTTP ") + code;
      }
      http.end();
      return false;
    }

    int totalSize = http.getSize();
    if (totalSize == 0) {
      result = "Firmware download was empty";
      http.end();
      return false;
    }

    addLog(String("[REMOTE OTA] download ") + currentUrl +
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

  result = "Too many firmware download redirects";
  return false;
}

