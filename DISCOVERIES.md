# DISCOVERIES — HomeKey-ESP32 (audit on `claude/homekey-esp32-review-1YaCk`)

Hardware objetivo: **ESP32-C6** (WiFi 6 + BLE 5.3 + IEEE 802.15.4 nativo).
PN532 por SPI, alimentación DC con futuro LiPo.

Severidad: 🔴 crítica · 🟠 alta · 🟡 media · 🟢 baja · ℹ️ informativa.

---

## 1. Seguridad

### 1.1 🔴 Las claves del Reader y los persistent keys de HomeKey se guardan en NVS sin cifrar
- Archivo: `main/ReaderDataManager.cpp` (NVS namespace `SAVED_DATA`, key `READERDATA`).
- `readerData_t` contiene `reader_sk` (clave privada del reader, 32 bytes ECC P-256), `reader_pk`, `reader_gid`, `reader_id`, lista de issuers (LTPK Apple) y `endpoint_prst_k` (claves persistentes derivadas tras pair-setup).
- `sdkconfig.defaults` **no** habilita `CONFIG_NVS_ENCRYPTION` ni `CONFIG_SECURE_FLASH_ENC_ENABLED`. Cualquiera que extraiga la flash con `esptool.py read_flash` recupera todas las claves y puede clonar el reader o impersonarlo frente a llaves Apple ya emparejadas.
- Igualmente sensibles: `MQTTSSLDATA` (clave privada cliente MQTT) y `HTTPSDATA` (clave privada del servidor HTTPS) — `ConfigManager::saveCertificate` los persiste también en NVS plaintext.
- HomeSpan también persiste su `LTSK` (clave a largo plazo del accesorio HAP) en otra namespace NVS (`HAPSRP`), también sin cifrar.

### 1.2 🔴 Flash encryption / Secure Boot v2 deshabilitados
- `sdkconfig.defaults` no incluye `CONFIG_SECURE_BOOT`, `CONFIG_SECURE_FLASH_ENC_ENABLED`, ni `CONFIG_SECURE_BOOT_V2_ENABLED`.
- ESP32-C6 soporta Secure Boot V2 (RSA-3072 o ECDSA-P256) y Flash Encryption AES-XTS-256 con clave en eFuse. Sin ello, el firmware se puede dumpear y modificar.
- Decisión del dueño: implementar Flash Encryption desde cero, asumiendo re-flash.

### 1.3 🔴 Contraseña hardcoded para AP de provisioning
- `main/main.cpp:77` → `WiFi.softAP("HomeKey-ESP32", "homekey123", ...)`.
- Cualquiera al alcance del dispositivo desemparejado puede entrar al portal cautivo y reconfigurar la cerradura, subir certificados, cambiar el `setupCode`.
- Además se usa `WIFI_CIPHER_TYPE_AES_CMAC128` (cipher de protección de management frames, no de pareja unicast) — combinación inusual; algunos clientes Apple/Android no asocian correctamente.

### 1.4 🟠 Defaults débiles para WebUI / OTA
- `main/include/defaults.h`:
  - `WEB_AUTH_ENABLED false` por defecto → la WebUI completa (incluido upload de certs y reset HK) queda **sin autenticación** salvo que el usuario active manualmente la auth desde la propia UI.
  - `WEB_AUTH_USERNAME "admin"` / `WEB_AUTH_PASSWORD "password"`.
  - `OTA_PWD "homespan-ota"`.
  - `SETUP_CODE "46637726"` (mismo en todos los forks).

### 1.5 🟠 OTA está activo pese a la decisión del dueño de desactivarlo
- `main/HomeKitLock.cpp:229` → `homeSpan.enableOTA(...)` siempre.
- `main/WebServerManager.cpp:311,1974` → endpoint `POST /ota/*` registrado y operativo.
- Partición `with_ota.csv` reserva 2× 1.875 MB (`app0`+`app1`) cuando solo se usará una.

### 1.6 🟠 HTTPS para WebUI desactivado por defecto y degradación silenciosa a HTTP
- `main/include/config.hpp` → `webHttpsEnabled = false`.
- `main/WebServerManager.cpp:169-180`: si `httpd_ssl_start` falla con TLS, **vuelve a intentar en modo INSECURE** sin avisar al usuario (sólo log).
- En modo AP, el server **siempre** se fuerza a `HTTPD_SSL_TRANSPORT_INSECURE` (`L149`). El portal cautivo recibe el `setupCode` y credenciales WiFi en plano.

### 1.7 🟠 Comparación de credenciales no constant-time (timing attack)
- `main/WebServerManager.cpp:251-272 basicAuth()` → `return authReq == digest;` (`std::string::operator==`).
- Aunque vivimos en LAN, la comparación con `mbedtls_constant_time_memcmp` es trivial y elimina el vector.

### 1.8 🟡 `MQTT_ALLOW_INSECURE` desactiva validación de CN
- `main/MqttManager.cpp:621` → `skip_cert_common_name_check = m_mqttConfig.allowInsecure`. OK como flag opt-in pero el log es `WARN` y no impide guardar la opción.

### 1.9 🟡 El `setupCode` (HomeKit pairing) viaja en JSON HTTP plano
- `main/WebServerManager.cpp:1269,1385,1446-1457` lo manda a la UI.
- En modo AP (forzado a HTTP), cualquiera en el SSID lo captura.
- `serializeToJson` ya enmascara passwords (`"********"`) pero `setupCode` no se enmascara.

### 1.10 🟡 Secrets logueados en serie por debug commands
- `HomeKitLock.cpp:386-390` (`SpanUserCommand 'P'`) imprime issuer LTPKs en hex por consola.
- `setupDebugCommands` instalado siempre, incluso en builds release.

### 1.11 🟡 Endpoint `POST /certificates` permite subir clave privada por HTTP
- En modo AP siempre HTTP. En STA depende de `webHttpsEnabled`.

### 1.12 🟡 Tras 6 desconexiones WiFi se entra agresivamente a modo AP
- `main/main.cpp:184-190` → `homeSpan.processSerialCommand("A")` resetea credenciales y abre AP.
- DoS posible: deauthenticar 6 veces el WiFi al dispositivo lo deja en AP plano con `homekey123`.

### 1.13 🟢 Sesión cookie 32 bytes de `randombytes_buf` (libsodium)
- `WebServerManager::begin` L127-129 — generación correcta, hex 64 chars. ✅ OK.

### 1.14 🟢 Validación de certificados en upload con mbedTLS
- `ConfigManager::validateCertificateWithMbedTLS` parsea, comprueba fechas, key-cert pair. ✅ OK.

### 1.15 🟢 Sanity sobre tamaños
- `MAX_CONFIG_SIZE = 8192`, `MAX_CERT_SIZE = 16384` en ConfigManager — bien.

### 1.16 ℹ️ El radio C6 puede usar HMAC/Digital Signature peripheral para envolver claves
- Hoy `reader_sk` está en RAM en claro durante la autenticación (`DDKAuthenticationContext`). Con DS peripheral las claves nunca dejarían el HW.

---

## 2. Eficiencia energética

### 2.1 🔴 `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` no es válido en C6
- `sdkconfig.defaults` viene del fork original (ESP32-S3). El C6 sólo soporta 80/120/160 MHz; `idf.py menuconfig` debería rechazarlo, pero la línea queda como ruido y no se aprovecha DFS.

### 2.2 🟠 PM activo pero sin perfil definido
- `CONFIG_PM_ENABLE=y` y `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` están bien, pero en ningún sitio se llama a `esp_pm_configure()` con un `esp_pm_config_esp32c6_t`. DFS por defecto sólo bajará a XTAL cuando idle, no entra en light sleep.

### 2.3 🟠 Polling NFC continuo a 5–100 ms
- `main/NfcManager.cpp:396-437`. La radio del PN532 está siempre encendida (`InCommunicateThru` + `InListPassiveTarget`). En batería esto es inviable (~80 mA continuos del PN532).
- ESP32-C6 con el resto de tareas paradas + light sleep podría bajar a μA, pero PN532 manda.
- PN532 soporta **modos low-power**: `PowerDown` con wake por SPI/IRQ. No se usa.
- ECP "Apple Express" se podría disparar sólo cuando el PN532 detecta presencia (LPCD — Low Power Card Detection en PN5180; el PN532 no lo tiene nativo, pero hay `RFConfiguration` con duty cycling: bytes "Polling Period" 0xB).

### 2.4 🟠 No se usa el IRQ del PN532 (línea NFC[1]?)
- `nfcGpioPins_t` reserva 4 pines (SS/SCK/MISO/MOSI) — la línea IRQ no está expuesta. Sin IRQ no se puede dormir y despertar al detectar carrier.

### 2.5 🟡 WiFi sin power-save explícito
- Nunca se llama `esp_wifi_set_ps(WIFI_PS_MAX_MODEM)` ni se configura `listen_interval`. C6 soporta TWT (Target Wake Time) WiFi 6 — drástica reducción de consumo, no se aprovecha.

### 2.6 🟡 Tarea `auth_precompute` permanente con polls de 1 s
- `NfcManager::authPrecomputeTask()` hace `ulTaskNotifyTake(... 1000)` aunque la cache esté llena. Aceptable, pero impide light sleep prolongado.

### 2.7 🟡 `loop()` en main hace `vTaskDelay(50ms)` constante
- `main/main.cpp:194-196`. Mantiene CPU activa cada 50 ms aunque no haya nada que hacer.

### 2.8 🟡 NeoPixel con driver RMT siempre inicializado aunque no se use
- `HardwareManager::begin()` arranca `Pixel` si pin != 255. Si no hay pixel en la PCB final, asegurarse de fijar 255.

### 2.9 🟡 `LOG_LOCAL_LEVEL=ESP_LOG_VERBOSE` compile-time
- `main/CMakeLists.txt` añade `-DLOG_LOCAL_LEVEL=ESP_LOG_VERBOSE`. Genera binarios grandes y logs detallados aunque runtime esté en WARN. Cada `ESP_LOGV` evaluado en runtime.

### 2.10 🟡 Brownout detector
- En batería LiPo, la curva de descarga puede meter al chip en boots cíclicos sin BOR configurado. No hay `CONFIG_ESP_BROWNOUT_DET_LVL` explícito en defaults.

### 2.11 ℹ️ Arduino-as-component pesa
- `arduino-esp32 3.3.5` arrastra muchos servicios (Ticker, EEPROM ya están desactivados con `ARDUINO_SELECTIVE_*`, pero sigue cargando WiFi/HTTP arduino layers). Pasar a IDF puro reduciría ~80–150 KB de flash y RAM.

---

## 3. Oportunidades del ESP32-C6 no aprovechadas

### 3.1 🟢 Thread/Matter nativo (802.15.4)
- `CONFIG_IEEE802154_ENABLED=n` en `sdkconfig.defaults`.
- El dueño tiene red Thread + HA. Una vía limpia: exponer el lock como **Matter Door Lock** sobre Thread, eliminar dependencia de MQTT broker y de HomeSpan-WiFi para el flujo HA. HomeKit-via-WiFi seguiría existiendo en paralelo (HomeSpan), pero también HomeKit puede hablar Matter (Matter-over-Thread también lo entiende Apple Home 16+).
- Trade-off: complica el firmware, requiere certificado DAC de Matter para producción (no necesario en hobby con CHIP-tool).

### 3.2 🟢 Cripto-aceleradores HW
- C6 tiene SHA, AES, ECC accelerator, HMAC peripheral, DS peripheral, RNG TRNG.
- HomeKey usa P-256 ECDH/ECDSA y AES-CTR/HKDF — todos acelerables.
- `libsodium` sólo aprovecha ED25519/X25519 (Curve25519); el flujo P-256 va por mbedTLS — verificar que `MBEDTLS_HARDWARE_ECC` y `MBEDTLS_HARDWARE_AES` estén ON.

### 3.3 🟢 eFuse + HMAC peripheral para wrapping
- `reader_sk` puede cifrarse con AES-GCM cuya clave se deriva del HMAC peripheral con un KEY_PURPOSE en eFuse (no extraíble por SW). Patrón estándar Espressif "key wrapping con HMAC SOFTWARE_DS_KEY".

### 3.4 🟢 Deep sleep + RTC GPIO wake
- C6 conserva ~30 KB RTC RAM. Wake-on-IRQ del PN532 permitiría idle <10 μA.

### 3.5 🟢 USB Serial JTAG nativo
- Ya activado (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`), bien. No cuesta energía si no hay host.

---

## 4. Deuda técnica relevante

### 4.1 🟡 Submódulos vacíos en este worktree
- `components/DigitalDoorKey`, `pn532_hal`, `pn532_cxx`, `loggable*`, `HomeSpan/upstream`, `msgpack-c/upstream` no están checkout-eados aquí. Cualquier auditoría profunda del flujo HK debe hacerse fetch de submódulos antes de ejecutar.
- `.gitmodules` aún declara `components/event_bus` (ya no se usa, migrado a `esp_event`).

### 4.2 🟡 Singleton frágil
- `HomeKitLock::s_instance` con `esp_restart()` si hay duplicado. Funciona, pero impide tests unitarios.

### 4.3 🟡 Busy-wait en lambda de desconexión
- `main/main.cpp:79-81`: `while(true){ vTaskDelay(100); }` dentro de la callback de conexión cuando status==0. Bloquea el contexto del callback de HomeSpan. Si se llama desde una tarea con stack pequeño, riesgo de starvation.

### 4.4 🟡 Headers inconsistentes
- Comentarios Doxygen de varios destructores hablan de "Unsubscribes from EventBus" pero el cuerpo es `= default` y el unsubscribe lo hace el RAII de `SubscriptionHandle`. Solo un detalle docs/code drift.

### 4.5 🟡 `ConfigManager::loadConfigFromNvs` no usa lock
- Solo se llama en `begin()`, antes de arrancar cualquier task que lea config. Ahora mismo seguro, pero si se añadiera reload runtime se rompería.

### 4.6 🟡 `WebServerManager.cpp` 2452 LoC en un único archivo
- Maintainability baja; trozos repetidos de boilerplate `httpd_resp_set_status` etc. No urgente pero invita a regresiones.

### 4.7 🟢 Tabla de particiones con OTA dual
- `with_ota.csv` reserva 2× `0x1E0000` para OTA. Si se elimina OTA: una sola partición `factory` de ~3.7 MB libera espacio para LittleFS / NVS más grande.

### 4.8 🟢 Sin política de auth-rate-limit
- WebUI no limita reintentos basicAuth; cookie+sessionId existen pero no hay lockout.

---

## 5. Resumen ejecutivo

| Frente | Acción más urgente |
|--------|---------------------|
| Seguridad de claves | Flash Encryption + Secure Boot V2 + NVS encryption (eFuse-backed) |
| Defaults | Forzar cambio de `setupCode`, password AP, web auth y eliminar `enableOTA` |
| Energía | Conectar IRQ del PN532, configurar `esp_pm_configure`, `esp_wifi_set_ps`, evaluar TWT |
| Plataforma | Decidir Thread/Matter vs WiFi-only; pasar partition table a single-app |
| Código | Sanitizar `loop()` busy-wait, comparación constant-time, no logear LTPKs |

Detalles ejecutables en `ACTION_PLAN.md`.
