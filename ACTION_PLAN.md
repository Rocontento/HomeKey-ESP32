# ACTION_PLAN — HomeKey-ESP32 (target ESP32-C6)

Tareas atómicas, ordenadas por dependencias y riesgo. Cada tarea declara fichero/función, criterio de aceptación, dificultad (D1=trivial, D5=muy alta) y riesgo (R1=bajo, R5=alto).

⛔ = irreversible (eFuse, secure boot, flash encryption). Hacer en último lugar y SOLO sobre dispositivo definitivo.

> Convenciones para el modelo ejecutor:
> - No introducir cambios fuera del scope descrito.
> - Mantener el estilo: ESP-IDF + Arduino-as-component, C++20, `fmt`, RAII guards.
> - Tras cada cambio relevante: `idf.py build` debe pasar sin warnings nuevos.

---

## FASE 0 — Higiene del repo (D1 / R1)

### T0.1 — Sincronizar submódulos
- Acción: ejecutar `git submodule update --init --recursive`.
- Aceptación: `components/DigitalDoorKey/`, `pn532_hal/`, `pn532_cxx/`, `loggable*/`, `HomeSpan/upstream/`, `msgpack-c/upstream/` contienen código.
- Razón: imprescindible para todas las tareas que tocan flujo HK.

### T0.2 — Borrar `event_bus` obsoleto del `.gitmodules`
- Archivo: `.gitmodules`.
- Quitar el bloque `[submodule "components/event_bus"]` (no se compila ya).
- Aceptación: `git config -f .gitmodules --get-regexp event_bus` no devuelve nada.
- Dificultad D1 / Riesgo R1.

---

## FASE 1 — Endurecer defaults sin tocar HW (D1–D2 / R1–R2)

### T1.1 — Eliminar OTA del firmware
- Archivos:
  - `main/HomeKitLock.cpp:229` → borrar `homeSpan.enableOTA(miscConfig.otaPasswd.c_str());`.
  - `main/include/config.hpp` → borrar campo `otaPasswd` y su entrada en `m_configMap["misc"]`.
  - `main/include/defaults.h` → borrar `OTA_PWD`.
  - `main/WebServerManager.cpp` → borrar entrada `{"/ota/*", HTTP_POST, handleOTAUpload, this}` en `setupRoutes`, métodos `handleOTAUpload` y todo el bloque `esp_ota_*`. Reducir `ssl_config.httpd.max_uri_handlers` a 18.
  - `main/include/WebServerManager.hpp` → quitar declaraciones `handleOTAUpload`, `OtaState`, etc.
  - `main/CMakeLists.txt` → no quitar deps (mqtt sigue existiendo).
- Aceptación: el binario no contiene la URI `/ota`, `homeSpan.enableOTA` no aparece, `esp_ota_*` no se enlaza (`idf.py size --components` no debe listar `app_update`).
- Dificultad D2 / Riesgo R2.

### T1.2 — Cambiar a tabla de particiones single-app
- Archivos: nuevo `partitions_single.csv`, `sdkconfig.defaults`.
- Plantilla:
  ```csv
  # Name,    Type, SubType, Offset,  Size,    Flags
  nvs,       data, nvs,     ,        0x10000,
  nvs_keys,  data, nvs_keys,,        0x1000,  encrypted
  phy_init,  data, phy,     ,        0x1000,
  factory,   app,  factory, ,        0x300000,
  spiffs,    data, spiffs,  ,        0x80000,
  ```
- En `sdkconfig.defaults`:
  ```
  CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_single.csv"
  CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n
  ```
- Borrar `with_ota.csv`.
- Aceptación: `idf.py partition-table` muestra `factory` única; LittleFS sigue montando.
- Dificultad D2 / Riesgo R3 (rompe upgrades in-place; el dueño asume re-flash).

### T1.3 — Forzar cambio de `setupCode` y password AP en primer arranque
- Archivos: `main/main.cpp` (función `setup`), `main/include/defaults.h`.
- Idea:
  1. Sustituir `WiFi.softAP("HomeKey-ESP32","homekey123",...)` por una contraseña aleatoria generada con `randombytes_buf(8)` y mostrada por consola la primera vez. Persistirla en NVS bajo `MISCDATA["apPassword"]`.
  2. Si `setupCode == "46637726"` al pasar a STA, refusar arrancar HomeSpan y forzar entrada al portal cautivo.
- Esqueleto Kconfig en `main/Kconfig.projbuild`:
  ```kconfig
  config HK_REQUIRE_NONDEFAULT_SETUP
      bool "Refuse to start HomeKit if setup code is the default"
      default y
  ```
- Aceptación: con setupCode default y `HK_REQUIRE_NONDEFAULT_SETUP=y`, log `ERROR` y no se llama `homeSpan.begin()`; el portal AP usa contraseña random no-default.
- Dificultad D3 / Riesgo R2.

### T1.4 — `webAuthEnabled = true` por defecto + bloqueo de password default
- Archivos: `main/include/defaults.h` (`WEB_AUTH_ENABLED true`), `main/WebServerManager.cpp` (handler `handleSaveConfig` cuando se actualiza misc): rechazar guardar si `webPassword == "password"` o longitud <8.
- Aceptación: build sin auth no permitido; intento de guardar password débil devuelve HTTP 400 con JSON `{"error":"weak_password"}`.
- Dificultad D2 / Riesgo R2.

### T1.5 — Comparación constant-time en basicAuth
- Archivo: `main/WebServerManager.cpp:251-272 basicAuth()`.
- Reemplazar `return authReq == digest;` por:
  ```cpp
  if (authReq.size() != digest.size()) return false;
  return mbedtls_ct_memcmp(authReq.data(), digest.data(), digest.size()) == 0;
  ```
- Incluir `<mbedtls/constant_time.h>`.
- Aceptación: misma funcionalidad; revisar con dos credenciales que sólo difieren en 1er byte vs último byte → tiempos similares (logs ya no revelan).
- Dificultad D1 / Riesgo R1.

### T1.6 — Eliminar fallback silencioso a HTTP en arranque del web server
- Archivo: `main/WebServerManager.cpp:169-180`.
- Quitar el bloque que reintenta `httpd_ssl_start` con `HTTPD_SSL_TRANSPORT_INSECURE` cuando hay TLS configurado y falla. En su lugar: `ESP_LOGE` y `return` (web no arranca, evento publicado por status timer).
- Mantener INSECURE únicamente cuando `isApMode || !isHttpsEnabled`.
- Aceptación: Si TLS se configura mal, el server no arranca degradado.
- Dificultad D1 / Riesgo R2.

### T1.7 — Enmascarar `setupCode` en `serializeToJson`
- Archivo: `main/ConfigManager.cpp:862-868`.
- Añadir condición: `if (key == "setupCode") cJSON_AddStringToObject(...,"********")`.
- Y en `WebServerManager.cpp:1269` no añadir manualmente el setupCode salvo si la sesión proviene de portal cautivo en primer setup (flag `firstBoot`).
- Aceptación: `GET /config?type=misc` devuelve `"setupCode":"********"` cuando ya hay HK pareado.
- Dificultad D2 / Riesgo R1.

### T1.8 — Sanitizar el debug command 'P'
- Archivo: `main/HomeKitLock.cpp:379-392`.
- Imprimir solo el `issuerId` (8 bytes hash) y *no* el LTPK. Hacer el comando opt-in vía Kconfig `HK_DEBUG_DUMP_LTPK` (default n).
- Aceptación: con flag off, `P` imprime sólo IDs.
- Dificultad D1 / Riesgo R1.

### T1.9 — Quitar busy-wait en callback de desconexión
- Archivo: `main/main.cpp:71-83`.
- Sustituir el `while(true){vTaskDelay(100);}` por simple `return;`. La tarea queda en AP+webserver activos vía los `begin()` previos.
- Aceptación: no hay tarea en busy-wait; el dispositivo sigue respondiendo en AP.
- Dificultad D1 / Riesgo R2 (validar que HomeSpan no requiere nada mas).

### T1.10 — Reducir agresividad del re-AP por desconexiones
- Archivo: `main/main.cpp:184-190`.
- Subir umbral de 6 a 30 desconexiones consecutivas y reiniciar contador en `ARDUINO_EVENT_WIFI_STA_CONNECTED`.
- Aceptación: deauth flood de 6 paquetes ya no fuerza modo AP.
- Dificultad D2 / Riesgo R2.

---

## FASE 2 — Energía sin tocar HW (D2–D3 / R2–R3)

### T2.1 — Corregir frecuencia de CPU para C6
- Archivo: `sdkconfig.defaults`.
- Sustituir `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` por `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y`.
- Aceptación: `idf.py menuconfig` muestra 160 MHz; arranque a 160 MHz confirmado en log.
- Dificultad D1 / Riesgo R1.

### T2.2 — Configurar `esp_pm_configure` con DFS + light sleep
- Nuevo: `main/PowerManager.cpp` + `main/include/PowerManager.hpp`.
- Esqueleto:
  ```cpp
  // PowerManager.hpp
  #pragma once
  class PowerManager {
  public:
      void begin();
  };
  ```
  ```cpp
  // PowerManager.cpp
  #include "PowerManager.hpp"
  #include "esp_pm.h"
  #include "esp_sleep.h"
  #include "esp_log.h"
  void PowerManager::begin() {
      esp_pm_config_t cfg = {
          .max_freq_mhz = 160,
          .min_freq_mhz = 40,
          .light_sleep_enable = true,
      };
      ESP_ERROR_CHECK(esp_pm_configure(&cfg));
      // TODO(impl): pm_lock para bloquear durante autenticación HK
  }
  ```
- Llamar `powerManager->begin()` en `main.cpp:setup()` antes de `homeSpan.begin()`.
- Añadir a `main/CMakeLists.txt` SRCS.
- Aceptación: log `pm: configured`. Light sleep activable cuando todas las tareas usan `vTaskDelay`.
- Dificultad D3 / Riesgo R3 (incompatibilidades con SPI/PN532; ver T2.3).

### T2.3 — pm_lock alrededor del flujo PN532 SPI
- Archivo: `main/NfcManager.cpp` (constructor, `pollingTask`).
- Crear `esp_pm_lock_handle_t m_pmLockApb;` con `esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, ...)`.
- `acquire` antes de cada `m_nfc->WriteRegister/InCommunicateThru/InListPassiveTarget/InDataExchange`, `release` tras `vTaskDelay(pollDelayTicks)`.
- Aceptación: sin glitches SPI; consumo medio baja entre polls.
- Dificultad D3 / Riesgo R3.

### T2.4 — WiFi power save explícito
- Archivo: `main/main.cpp` (al final de `setup` o tras conexión STA).
- Añadir:
  ```cpp
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
  ```
- Aceptación: `iw dev` o sniffer muestra modem-sleep activo. HomeKit sigue respondiendo a iPhone (latencia +20–80 ms aceptable).
- Dificultad D1 / Riesgo R2.

### T2.5 — Compilar logs verbose fuera
- Archivo: `main/CMakeLists.txt`.
- Cambiar `LOG_LOCAL_LEVEL=ESP_LOG_VERBOSE` por `LOG_LOCAL_LEVEL=ESP_LOG_INFO` (release) y dejar VERBOSE detrás de un flag CMake `-DHK_DEV=1`.
- Esqueleto:
  ```cmake
  if(HK_DEV)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC "-DLOG_LOCAL_LEVEL=ESP_LOG_VERBOSE")
  else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC "-DLOG_LOCAL_LEVEL=ESP_LOG_INFO")
  endif()
  ```
- Aceptación: `idf.py size` reduce flash (~50–120 KB) en build release.
- Dificultad D1 / Riesgo R1.

### T2.6 — Aumentar `vTaskDelay` del main loop con guardia HomeSpan
- Archivo: `main/main.cpp:194-196`.
- Subir delay a `pdMS_TO_TICKS(20)` y mover a `xTaskCreatePinnedToCore` dedicado para HomeSpan, dejando `loop()` vacío. Esto solo si HomeSpan tolera (verificar tras cambio).
- Aceptación: latencia HomeKit similar; CPU idle aumenta.
- Dificultad D3 / Riesgo R3.

### T2.7 — Brownout detector configurado
- Archivo: `sdkconfig.defaults`.
- Añadir:
  ```
  CONFIG_ESP_BROWNOUT_DET=y
  CONFIG_ESP_BROWNOUT_DET_LVL_SEL_7=y
  ```
- Aceptación: arranque limpio en LiPo a 3.3 V; reset si <2.85 V.
- Dificultad D1 / Riesgo R2.

---

## FASE 3 — Conectar IRQ del PN532 (D3 / R3) [requiere HW]

### T3.1 — Añadir 5º pin (IRQ) a la config NFC
- Archivos:
  - `main/include/config.hpp` → `std::array<uint8_t, 5> nfcGpioPins{...};` (extender preset y custom).
  - `main/include/defaults.h` → añadir `IRQ_PIN`.
  - `main/ConfigManager.cpp` → ampliar el `std::array<uint8_t,4>` mapping a `std::array<uint8_t,5>` (añadir variant alternative y manejo en deserialize/serialize).
  - `main/NfcManager.cpp:259-271 begin()` → registrar GPIO ISR sobre IRQ_PIN.
- Esqueleto ISR:
  ```cpp
  static void IRAM_ATTR pn532_irq_isr(void* arg) {
      auto* self = static_cast<NfcManager*>(arg);
      BaseType_t hpw = pdFALSE;
      vTaskNotifyGiveFromISR(self->m_pollingTaskHandle, &hpw);
      portYIELD_FROM_ISR(hpw);
  }
  ```
- Polling task pasa de busy-poll a `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(IDLE_POLL_MS))`.
- Aceptación: tap detectado < 200 ms tras presencia; CPU idle entre taps > 95 %.
- Dificultad D4 / Riesgo R3 (cambio de cableado físico).

### T3.2 — Light sleep entre taps con wake por IRQ
- Archivo: `main/NfcManager.cpp::pollingTask`.
- Cuando no haya tag por > N segundos, llamar `esp_sleep_enable_gpio_wakeup()` + `esp_light_sleep_start()`. Despertar por IRQ_PIN low.
- Riesgo: WiFi STA debe permanecer en modem-sleep no full sleep para HomeKit advert. Validar.
- Aceptación: consumo medio en idle < 25 mA (placa entera con WiFi STA conectada).
- Dificultad D5 / Riesgo R4.

---

## FASE 4 — Cripto/Claves protegidas por HW (D3–D5 / R3–R5)

### T4.1 — Verificar aceleración HW de mbedTLS
- Archivo: `sdkconfig.defaults`.
- Añadir/verificar:
  ```
  CONFIG_MBEDTLS_HARDWARE_AES=y
  CONFIG_MBEDTLS_HARDWARE_GCM=y
  CONFIG_MBEDTLS_HARDWARE_SHA=y
  CONFIG_MBEDTLS_HARDWARE_ECC=y
  ```
- Aceptación: `idf.py menuconfig` los muestra activos. Tiempo de auth HK Fast Flow se mantiene o baja.
- Dificultad D1 / Riesgo R2.

### T4.2 — Habilitar NVS encryption (clave en partición `nvs_keys`) ⛔
- Pre-requisito: T1.2 (partición `nvs_keys`).
- Archivo: `sdkconfig.defaults`.
  ```
  CONFIG_NVS_ENCRYPTION=y
  ```
- Acción runtime: `nvs_flash_init` ya gestiona la lectura de la clave de `nvs_keys`. Generar la clave una vez con `nvs_flash_generate_keys`.
- Aceptación: NVS namespace `SAVED_DATA` queda cifrado AES-XTS; lectura por esptool no revela `READERDATA`.
- Dificultad D3 / Riesgo R4. ⛔ porque depende de Flash Encryption (T4.4).

### T4.3 — Wrapping de `reader_sk` con HMAC peripheral
- Archivos nuevos: `main/include/KeyVault.hpp`, `main/KeyVault.cpp`.
- Idea: usar `esp_hmac_calculate(HMAC_KEY_x, info, …)` con clave en eFuse (purpose `HMAC_UP`) para derivar una KEK; cifrar `reader_sk` y `endpoint_prst_k` con AES-GCM(KEK) antes de persistir; descifrar al cargar.
- Esqueleto:
  ```cpp
  // KeyVault.hpp
  #pragma once
  #include <vector>
  #include <cstdint>
  namespace KeyVault {
    bool wrap(const uint8_t* plain, size_t len, std::vector<uint8_t>& out);
    bool unwrap(const uint8_t* wrapped, size_t len, std::vector<uint8_t>& out);
    bool ensure_efuse_key_provisioned(); // burns eFuse if first boot
  }
  ```
- Integración: `ReaderDataManager::pack_readerData_t` envuelve `reader_sk`; `unpack_readerData_t` desenvuelve.
- Aceptación: dump flash + read NVS no revela 32 bytes ECC; tras restaurar imagen en otro chip C6, descifrado falla (clave HMAC ligada a eFuse del chip original).
- Dificultad D5 / Riesgo R4.

### T4.4 — Habilitar Flash Encryption + Secure Boot V2 ⛔
- Archivo: `sdkconfig.defaults` (release build, separado).
- Añadir:
  ```
  CONFIG_SECURE_BOOT=y
  CONFIG_SECURE_BOOT_V2_ENABLED=y
  CONFIG_SECURE_BOOT_SIGNING_KEY="secure_boot_signing_key.pem"
  CONFIG_SECURE_FLASH_ENC_ENABLED=y
  CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y
  CONFIG_SECURE_FLASH_REQUIRE_ALREADY_ENABLED=n
  CONFIG_SECURE_BOOT_INSIST_DOWNLOAD_MODE=n
  CONFIG_EFUSE_VIRTUAL=n
  ```
- Generar claves: `espsecure.py generate_signing_key --version 2 secure_boot_signing_key.pem`.
- Procedimiento documentado en `docs/SECURE_BOOT.md` (crear).
- Aceptación: dispositivo arranca; `esptool.py read_flash` devuelve datos cifrados; eFuses `SECURE_BOOT_EN`, `SPI_BOOT_CRYPT_CNT` quemados.
- Dificultad D5 / Riesgo R5. ⛔⛔ Una vez quemado, no se puede revertir.

### T4.5 — Procedimiento de provisión seguro
- Documento `docs/PROVISIONING.md` con secuencia:
  1. Flash de fábrica con `idf.py encrypted-app-flash monitor` en modo dev.
  2. Quemar eFuse HMAC key (KeyVault).
  3. Generar par de claves HK desde dentro del dispositivo (no inyectar).
  4. Configurar setupCode aleatorio (UI primer arranque).
  5. Quemar SECURE_BOOT_EN.
- Dificultad D3 / Riesgo R3 (proceso, no código).

---

## FASE 5 — Integración Home Assistant

### T5.1 — Decidir arquitectura: MQTT vs Matter-over-Thread
- Recomendación inicial: **mantener MQTT** (ya implementado, funcional con Mosquitto que el dueño tiene) y postergar Matter como FASE 6 opcional.
- Razón: Matter Door Lock requiere stack chip-tool y certificados; coste/beneficio ahora bajo.
- Dificultad D1 / Riesgo R1 (decisión).

### T5.2 — TLS obligatorio en MQTT
- Archivo: `main/include/defaults.h`.
- Cambiar `MQTT_USE_SSL false` → `true` y `MQTT_ALLOW_INSECURE` siempre `false`.
- En `MqttManager::begin()`, refusar arranque sin CA cert.
- Aceptación: MQTT no conecta a 1883 plano por defecto; UI obliga a subir CA.
- Dificultad D2 / Riesgo R2.

### T5.3 — QoS 1 para topics de estado y comandos
- Archivo: `main/MqttManager.cpp` (`publish*`, `subscribe`).
- Cambiar QoS por defecto de 0 a 1 en publish de lock state, hk tap, status. Suscripciones a 1.
- Aceptación: HA no pierde mensajes en breves cortes WiFi.
- Dificultad D1 / Riesgo R2.

---

## FASE 6 — Opcional: Matter-over-Thread (D5 / R4)

### T6.1 — Habilitar 802.15.4 + OpenThread + esp-matter
- Archivo: `sdkconfig.defaults`.
- `CONFIG_IEEE802154_ENABLED=y`, `CONFIG_OPENTHREAD_ENABLED=y`.
- Añadir component `espressif/esp_matter`.
- Crear `main/MatterLock.cpp` con `Endpoint::DoorLock` que reenvíe a `LockManager`.
- Aceptación: dispositivo se anuncia en red Thread, HA-Matter lo detecta como Door Lock.
- Riesgo: incompatibilidad con WiFi simultáneo en C6 (coexistencia OK pero RAM ajustada).
- Dificultad D5 / Riesgo R4.

---

## Tabla resumen de orden recomendado

| Orden | Tarea | Cuándo |
|------:|-------|--------|
| 1 | T0.1, T0.2 | inmediato |
| 2 | T1.1 → T1.10 | misma sesión |
| 3 | T2.1, T2.4, T2.5, T2.7 | tras T1 |
| 4 | T2.2, T2.3, T2.6 | requiere medir consumo |
| 5 | T3.1, T3.2 | requiere modificar PCB |
| 6 | T4.1 | rápido tras T2 |
| 7 | T5.2, T5.3 | en paralelo con T4 |
| 8 | T4.3 (KeyVault) | sobre dev-board primero |
| 9 | T4.2 (NVS enc) ⛔ | sobre dispositivo definitivo |
| 10 | T4.4 (Secure Boot+FE) ⛔ | última |
| 11 | T6.* (Matter) | opcional |

---

## Plantillas de código para T1.1 (eliminar OTA) — referencia rápida

Diff esperado en `main/HomeKitLock.cpp`:
```diff
-    homeSpan.enableOTA(miscConfig.otaPasswd.c_str());
     homeSpan.setPortNum(1201);
```

Diff esperado en `main/include/config.hpp`:
```diff
   struct misc_config_t {
     std::string deviceName = DEVICE_NAME;
-    std::string otaPasswd = OTA_PWD;
     uint8_t hk_key_color = HOMEKEY_COLOR;
```

Y en `ConfigManager.cpp` quitar la línea `{"otaPasswd", &m_miscConfig.otaPasswd},`.

---

## Notas para el ejecutor

- Tras cada fase, re-flashear y validar con un iPhone real:
  1. Tap exitoso (HomeKey FAST flow) en < 600 ms.
  2. HomeKit `Lock/Unlock` desde la app.
  3. `mosquitto_sub -t '<id>/homekit/state'` recibe estados.
- Cualquier tarea ⛔ requiere confirmación humana explícita y backup del firmware/eFuses (`espefuse.py summary`) antes de ejecutar.
- Si una fase falla (especialmente FASE 4 ⛔), el dispositivo puede quedar inservible. Ensayar todo el flujo en una placa de desarrollo C6 distinta antes de tocar la cerradura definitiva.
