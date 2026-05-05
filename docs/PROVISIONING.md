# PROVISIONING.md — Secuencia de provisión segura del HomeKey-ESP32 (ESP32-C6)

Guía paso a paso para provisionar un dispositivo HomeKey-ESP32 (cerradura inteligente con soporte HomeKit y NFC) de forma segura, protegiendo las claves criptográficas mediante mecanismos de hardware (eFuse HMAC, NVS encryption, Flash Encryption, Secure Boot V2).

**Última actualización:** 2026-05-03  
**Versión del firmware:** con T4.3 (KeyVault HMAC wrapping) y T4.2/T4.4 pendientes ⛔

---

## 1. Prerrequisitos

### Herramientas requeridas

- **esptool.py** — flashear firmware: `pip install esptool`
- **espsecure.py** — manejar eFuses y claves de Secure Boot: `pip install espressif/espsecure` (v2.0+)
- **idf.py** — compilar y gestionar particiones: `source $IDF_PATH/export.sh`
- **pyserial** — monitorizar consola: `pip install pyserial`

### Hardware requerido

1. **Placa de desarrollo ESP32-C6** (ej. ESP32-C6-DevKitC-1 de Espressif)
   - Para ensayar **sin quema de eFuses**: provisión segura reversible
   - Pinout: UART para consola serie, SPI para PN532, GPIO para NFC IRQ

2. **Dispositivo de producción ESP32-C6**
   - Una vez quemadas irreversibles (eFuses, Secure Boot), no se puede revertir
   - Se recomienda ensayar la secuencia completa en dev-board **antes de tocar la cerradura definitiva**

3. **Cable USB** para conexión serie (UART/USB-JTAG nativo en C6)

### Entorno de compilación

```bash
# Clonar repositorio y preparar
cd /path/to/HomeKey-ESP32
git submodule update --init --recursive
export IDF_PATH=/path/to/esp-idf  # v5.1+
source $IDF_PATH/export.sh

# Configurar herramientas
export ESPTOOL_PATH=$(python -c "import esptool; print(esptool.__path__[0])")
```

---

## 2. Secuencia para DEV-BOARD (sin quema de eFuses)

Propósito: **validar funcionalidad, flujo de provisión y comportamiento criptográfico sin efectos permanentes**.

### 2.1 Compilar firmware de desarrollo

```bash
# Desde la raíz del proyecto
idf.py set-target esp32c6
idf.py menuconfig

# Verificar:
#   - CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_single.csv" (o with_ota.csv)
#   - CONFIG_HK_ENABLE_KEYVAULT=n (opcional: lo probaremos con y sin)
#   - CONFIG_SECURE_BOOT=n (deshabilitado para dev)
#   - CONFIG_SECURE_FLASH_ENC_ENABLED=n (deshabilitado para dev)

idf.py build
```

### 2.2 Flashear firmware

```bash
# Borrar flash e inyectar firmware limpio
esptool.py --chip esp32c6 --port /dev/ttyACM0 erase_flash

# Flashear con idf.py (automático)
idf.py -p /dev/ttyACM0 flash

# O manual con esptool.py
esptool.py --chip esp32c6 --port /dev/ttyACM0 write_flash \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x20000 build/homekey_esp32.bin
```

### 2.3 Monitor y primer arranque

```bash
idf.py -p /dev/ttyACM0 monitor

# En los logs buscar:
#   - "WiFi mode: AP" → acceso portal de provisión
#   - "AP password: <random>" o "AP password: homekey123" (según T1.3)
#   - "setupCode: 46637726" (default, riesgoso; debe cambiarse)
#   - "KeyVault: eFuse HMAC key not provisioned" (si CONFIG_HK_ENABLE_KEYVAULT=y)
```

### 2.4 Provisionar a través del portal cautivo (AP)

1. Conectarse a SSID `HomeKey-ESP32` con contraseña mostrada en logs
2. Abrir navegador a `http://192.168.4.1`
3. En la UI:
   - **Cambiar setupCode** de `46637726` a uno único (ej. `123456`)
   - Ingresar credenciales WiFi (SSID y password STA)
   - Guardar y reiniciar

### 2.5 Verificar funcionamiento en STA

```bash
# Tras reinicio, el dispositivo intenta conectarse a WiFi STA
# Si setupCode sigue siendo `46637726` y T1.3 está habilitado:
#   - Log: "ERROR: setupCode is default; refusing to start HomeKit"
#   - Vuelve a AP
# Si setupCode cambió correctamente:
#   - Log: "HomeKit setup code: <nuevo código>"
#   - Accesible por mDNS: `http://<mDNS-name>.local` (HTTPS si T1.6 = enabled)
```

### 2.6 Pruebas de funcionalidad en DEV

```bash
# Test 1: NFC tap
# - Colocar tarjeta MIFARE sobre PN532
# - Log: "DDK authenticate..." → "Reader authenticated to issuer"
# - Comando de lock ejecutado (publicado en MQTT si está configurado)

# Test 2: HomeKit pairing
# - Usar Home.app en iOS con setup code previamente ingresado
# - Verificar: "HomeKit accessory paired successfully"

# Test 3: WebUI autenticada (si T1.4 habilitado)
# - GET http://<ip>:1200 requiere HTTP Basic Auth
# - Credenciales por defecto: admin/password (deben cambiar en UI)
```

### 2.7 Snapshot de eFuses en DEV (para comparación)

```bash
# Crear backup de estado sin quemar
esptool.py --chip esp32c6 --port /dev/ttyACM0 read_efuse summary > efuse_dev_clean.txt

# Contenido típico en dev sin provisión:
#   SECURE_BOOT_EN: 0
#   SPI_BOOT_CRYPT_CNT: 0
#   HMAC_KEY0_PURPOSE: [unquemado]
```

---

## 3. Secuencia de provisión segura (dispositivo de producción)

Pasos **irreversibles** marcados con ⛔; ejecutar **solo en dispositivo definitivo** y tras validar en dev-board.

### 3.1 Flash del firmware base (T1 completado)

**Prequisitos:**
- Firmware compilado con FASE 1 completada (T1.1–T1.10 según ACTION_PLAN.md)
  - ✅ OTA deshabilitado
  - ✅ Tabla de particiones single-app con `nvs_keys`
  - ✅ setupCode default rechazado
  - ✅ WebUI autenticada por defecto

**Acciones:**

```bash
# En la máquina de provisión
cd /path/to/HomeKey-ESP32
idf.py set-target esp32c6

# Configurar para producción (dev-board):
idf.py menuconfig
# - CONFIG_HK_ENABLE_KEYVAULT=y  (habilitar wrapping de keys)
# - CONFIG_SECURE_BOOT=n  (aún NO, lo hacemos en T4.4)
# - CONFIG_SECURE_FLASH_ENC_ENABLED=n  (aún NO)

idf.py build

# Flashear dispositivo de producción por USB/UART
esptool.py --chip esp32c6 --port /dev/ttyUSB0 erase_flash
idf.py -p /dev/ttyUSB0 flash
```

Verificación:
```bash
# Monitor serie durante ~30 segundos
idf.py -p /dev/ttyUSB0 monitor

# Buscar logs:
# ✅ "WiFi mode: AP" o "Setup mode"
# ✅ "AP SSID: HomeKey-ESP32"
# ✅ "AP password: <random>" (T1.3)
# ✅ "setupCode: 46637726 (DEFAULT - MUST CHANGE)"
# ✅ "KeyVault: eFuse HMAC key not provisioned yet"
```

---

### 3.2 Arranque inicial: cambiar setupCode, anotar contraseña AP

**Acción manual en el portal cautivo:**

1. Conectarse WiFi a `HomeKey-ESP32` con contraseña de logs
2. Abrir `http://192.168.4.1` en navegador
3. Ir a pestaña **"Config"** → **"Misc"** o **"Setup"**
4. Campo **"Setup Code"**: cambiar de `46637726` a valor único (ej. `654321`)
   - Anotar este código: necesario para pairing HomeKit
5. Guardar → reinicia dispositivo

**Por qué no usar el default 46637726:**
- Es el mismo en todos los forks públicos → accesible a cualquiera
- Firmware con T1.3 refusa arrancar si no se cambia
- Apple HomeKit+iCloud requieren setupCode único para cada dispositivo

---

### 3.3 KeyVault provisioning: quemar eFuse HMAC ⛔

**Estado actual de firmware:** CONFIG_HK_ENABLE_KEYVAULT=y  
**Efecto:** `reader_sk` (clave privada ECC del reader) será cifrada con AES-256-GCM usando KEK derivada de HMAC  
**Reversibilidad:** ⛔ IRREVERSIBLE — eFuse quemada permanentemente

#### Opción A: Mediante endpoint web (recomendado)

Si el firmware contiene endpoint `/api/keyvault/provision`:

```bash
# Desde navegador o curl tras acceder a WebUI
curl -X POST http://<ip>:1200/api/keyvault/provision \
  -H "Authorization: Basic $(echo -n admin:password | base64)" \
  -H "Content-Type: application/json" \
  -d '{}'

# Respuesta esperada:
# {"status": "provisioned", "message": "HMAC key burned to eFuse block 0"}

# O en WebUI: botón "Provision KeyVault" en sección "Security"
```

#### Opción B: Mediante comando en consola serie

```bash
# En el puerto serie durante ejecución:
# Escribir comando (según HomeSpan debug commands)
P  # o comando específico KeyVault si existe

# Log esperado:
# [KeyVault] HMAC key burned to eFuse block 0
# [KeyVault] Provisioning irreversible; key now in eFuse HMAC_UP
```

#### Opción C: Programáticamente (durante compilación/factory)

Crear tarea early-boot que llame `KeyVault::provision()` si aún no está quemado:

```cpp
// En main.cpp:setup()
if (!KeyVault::isProvisioned()) {
    ESP_LOGI(TAG, "Auto-provisioning HMAC key...");
    esp_err_t ret = KeyVault::provision();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "KeyVault provisioning failed: %s", esp_err_to_name(ret));
        // Entrar en AP/portal para retry manual
    } else {
        ESP_LOGI(TAG, "KeyVault provisioned OK, restarting...");
        esp_restart();
    }
}
```

**Verificación post-quema:**

```bash
# Revisar estado eFuse
esptool.py --chip esp32c6 --port /dev/ttyUSB0 read_efuse summary

# Búscar en output:
# HMAC_KEY0_PURPOSE:          HMAC_UP

# En logs del monitor serie:
# [KeyVault] eFuse HMAC key provisioned ✅
# [ReaderDataManager] Wrapping reader_sk with AES-256-GCM...
```

**Si la quema falla:**
- Dispositivo queda en estado inconsistente (KEY0 parcialmente quemado)
- Opción: revertir toda la imagen desde backup (requiere acceso a JTAG)
- Evitable: **hacer esto en dev-board primero**

---

### 3.4 Habilitar CONFIG_HK_ENABLE_KEYVAULT, recompilar y reflashear

**Acción:**

```bash
# Asegurar KeyVault habilitado
idf.py menuconfig
# CONFIG_HK_ENABLE_KEYVAULT=y ✅

idf.py build

# Reflashear solo la partición app (el bootloader no cambia)
esptool.py --chip esp32c6 --port /dev/ttyUSB0 write_flash 0x20000 build/homekey_esp32.bin
```

**Comportamiento post-flash:**

- Dispositivo arranca con KeyVault disponible
- Al iniciar sesión HomeKit (pairing), la clave `reader_sk` se envuelve mediante `KeyVault::wrap()`
- Almacenada en NVS como `[IV(12) || CT(len) || TAG(16)]` — **no legible sin eFuse HMAC del chip**
- En siguientes arranques, `ReaderDataManager::load()` desenvuelve automáticamente si eFuse está quemada

**Verificación:**

```bash
# En monitor serie:
idf.py -p /dev/ttyUSB0 monitor

# Log esperado:
# [KeyVault] eFuse HMAC key is provisioned ✅
# [ReaderDataManager] Loaded reader_sk (wrapped, length=XX bytes)
```

---

### 3.5 ⛔ Habilitar NVS encryption (T4.2)

**Estado actual:** ⛔ PENDIENTE en codebase  
**Reversibilidad:** ⛔ IRREVERSIBLE a nivel de flash (se quema SPI_BOOT_CRYPT_CNT en eFuse)

#### Pre-requisito: partición `nvs_keys`

Tabla de particiones `partitions_single.csv`:
```csv
# Name,    Type, SubType, Offset,  Size,    Flags
nvs,       data, nvs,     ,        0x10000,
nvs_keys,  data, nvs_keys,,        0x1000,  encrypted
phy_init,  data, phy,     ,        0x1000,
factory,   app,  factory, ,        0x300000,
spiffs,    data, spiffs,  ,        0x80000,
```

#### Activación

```bash
# Compilar con NVS encryption
idf.py menuconfig
# CONFIG_NVS_ENCRYPTION=y ✅

idf.py build

# **ANTES de flashear, generar clave NVS si aún no existe:**
# La primera vez que arranca con CONFIG_NVS_ENCRYPTION=y, el firmware llama
# nvs_flash_generate_keys() automáticamente y graba la clave en nvs_keys

# Flashear firmware + partición table
esptool.py --chip esp32c6 --port /dev/ttyUSB0 write_flash \
  0x8000 build/partition_table/partition-table.bin \
  0x20000 build/homekey_esp32.bin
```

#### Validación

```bash
# Monitor durante arranque
idf.py -p /dev/ttyUSB0 monitor

# Log esperado:
# [NVS] Generating NVS encryption key...
# [nvs_keys] Key stored in partition, encrypted by Flash Encryption (if enabled)

# Dump NVS sin eFuse key → datos cifrados
esptool.py --chip esp32c6 --port /dev/ttyUSB0 read_flash 0x9000 0x10000 > nvs_encrypted.bin
# → el contenido es AES-XTS encrypted, no legible

# Dump desde dispositivo en ejecución (por UART/HTTP dump endpoint)
# → datos desencriptados (clave en RAM durante lectura)
```

**Riesgos post-activación:**
- Si se pierde la clave `nvs_keys`, el NVS se vuelve inaccesible
- No hay "reset factory" fácil sin re-flashear
- **Backup crucial:** guardar partición `nvs_keys` antes de producción

---

### 3.6 ⛔⛔ Habilitar Flash Encryption + Secure Boot V2 (T4.4)

**Estado actual:** ⛔⛔ MUY CRÍTICO — Una vez ejecutado, no se puede deshacer  
**Irreversibilidad:** EFuses SECURE_BOOT_EN y SPI_BOOT_CRYPT_CNT se queman para siempre

#### Preparación

```bash
# Generar claves RSA-3072 para Secure Boot V2
# (Guardar en ubicación segura, ej. HSM o partición protegida)
espsecure.py generate_signing_key --version 2 secure_boot_signing_key.pem

# Alternativamente, ECDSA-P256 (más compacto, recomendado para C6)
espsecure.py generate_signing_key --version 2 --ecdsa secure_boot_signing_key.pem
```

#### Compilación en release

```bash
# Crear sdkconfig separado para producción
cp sdkconfig.defaults sdkconfig.production

# Editar sdkconfig.production:
cat >> sdkconfig.production << 'EOF'
CONFIG_SECURE_BOOT=y
CONFIG_SECURE_BOOT_V2_ENABLED=y
CONFIG_SECURE_BOOT_SIGNING_KEY="secure_boot_signing_key.pem"
CONFIG_SECURE_BOOT_VERSION=2
CONFIG_SECURE_FLASH_ENC_ENABLED=y
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y
CONFIG_SECURE_FLASH_REQUIRE_ALREADY_ENABLED=n
CONFIG_SECURE_BOOT_INSIST_DOWNLOAD_MODE=n
CONFIG_EFUSE_VIRTUAL=n
EOF

# Compilar con perfil production
idf.py -DSDKCONFIG=sdkconfig.production set-target esp32c6
idf.py -DSDKCONFIG=sdkconfig.production build

# Verificar tamaño de bootloader (Secure Boot v2 agrega ~4–5 KB)
ls -lh build/bootloader/bootloader.bin
```

#### Quema de eFuses y flasheo (irreversible)

```bash
# PUNTO DE NO RETORNO ⛔⛔
# Una vez ejecutado, el dispositivo SOLO aceptará firmware firmado con la clave privada
# y NUNCA se podrá volver a modo de desarrollo

# Opción 1: Flasheo con esptool + quema simultánea (recomendado)
esptool.py --chip esp32c6 --port /dev/ttyUSB0 \
  --secure-pad \
  --secure-digest-alg SHA256 \
  write_flash_encrypt_file \
  --no-append \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x20000 build/homekey_esp32.bin

# Opción 2: Flasheo manual + espsecure.py burn_key
# (Para mayor control en línea de producción)

# 1. Flashear sin eFuse burning
esptool.py --chip esp32c6 --port /dev/ttyUSB0 \
  --no-stub write_flash \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x20000 build/homekey_esp32.bin

# 2. Quemar eFuses (IRREVERSIBLE)
espsecure.py --chip esp32c6 --port /dev/ttyUSB0 burn_key \
  SECURE_BOOT_DIGEST0 build/bootloader/bootloader.bin \
  --no-read

# 3. Habilitar SECURE_BOOT_EN y SPI_BOOT_CRYPT_CNT
espsecure.py --chip esp32c6 --port /dev/ttyUSB0 burn_efuse SECURE_BOOT_EN 1
espsecure.py --chip esp32c6 --port /dev/ttyUSB0 burn_efuse SPI_BOOT_CRYPT_CNT 1
```

#### Validación post-quema

```bash
# Revisar estado de eFuses
esptool.py --chip esp32c6 --port /dev/ttyUSB0 read_efuse summary

# Output esperado:
# SECURE_BOOT_EN:              1
# SPI_BOOT_CRYPT_CNT:          1
# SECURE_BOOT_DIGEST0:         [datos del hash del bootloader]
# ... otros HMAC_KEY0, etc.

# Intentar leer flash — datos encriptados
esptool.py --chip esp32c6 --port /dev/ttyUSB0 read_flash 0x0 256 -o firmware_encrypted.bin

# Contenido: aleatorio (encriptado AES-XTS)

# Monitor serie durante arranque
idf.py -p /dev/ttyUSB0 monitor

# Log esperado:
# [Boot] Secure boot v2 enabled ✓
# [Boot] Flash encryption enabled ✓
# [Boot] Verifying bootloader signature...
# [Boot] eFuse key from block N...
# [WiFi] WiFi initialized
```

#### Prohibir re-flasheo sin firma

Una vez ejecutado:
```bash
# Intentar flashear firmware NO FIRMADO → **rechazado**
esptool.py --chip esp32c6 --port /dev/ttyUSB0 write_flash 0x20000 unsigned_firmware.bin
# Error: "Secure boot enabled, cannot flash unsigned image"

# Único modo permitido: firmware FIRMADO
# idf.py build genera automáticamente firmware.signed.bin si SECURE_BOOT está ON
esptool.py --chip esp32c6 --port /dev/ttyUSB0 write_flash \
  0x20000 build/homekey_esp32.bin  # Ya está firmado por IDF
```

---

## 4. Backup y recuperación

### 4.1 Backup pre-irreversible

**Ejecutar antes de cada paso ⛔:**

```bash
# Full flash dump (antes de T3.3 KeyVault provision)
esptool.py --chip esp32c6 --port /dev/ttyUSB0 read_flash 0 0x400000 > flash_backup_pre_keyvault.bin

# NVS keys partition backup (antes de T3.5 NVS encryption)
esptool.py --chip esp32c6 --port /dev/ttyUSB0 read_flash 0x9000 0x1000 > nvs_keys_backup.bin

# eFuse state snapshot (antes de T3.6 Secure Boot)
esptool.py --chip esp32c6 --port /dev/ttyUSB0 read_efuse summary > efuse_state_pre_secureboot.txt
```

### 4.2 Recuperación post-fallo

**Si algo falla antes de T4.4 (Secure Boot):**

```bash
# Dispositivo aún está en modo dev (Secure Boot deshabilitado)
# Revertir flash a backup anterior
esptool.py --chip esp32c6 --port /dev/ttyUSB0 erase_flash
esptool.py --chip esp32c6 --port /dev/ttyUSB0 write_flash 0 flash_backup_pre_keyvault.bin
```

**Si falla después de T4.4 (Secure Boot habilitado):**

```bash
# ⛔⛔⛔ Dispositivo **IRRECUPERABLE** sin:
#   1. La clave privada de Secure Boot (secure_boot_signing_key.pem)
#   2. Acceso JTAG (costoso, requiere programador especializado)
#   3. Permiso de manufacturer para desbloqueo por OTA (no disponible)

# Prevención: **NEVER** perder secure_boot_signing_key.pem
# - Guardar en HSM (Hardware Security Module) si es posible
# - Backup en almacenamiento seguro (encrypted USB, vault, etc.)
# - Control de versiones en repositorio privado (Git con branch protegida)

# Si aún tienes el firmware anterior sin firmar y Secure Boot = ON:
# → Imposible flashear, dispositivo es un "brick" criptográfico

# Única salida (costosa):
# 1. Contactar a Espressif con proof of ownership
# 2. Factory reset vía JTAG (requiere hardware específico)
# 3. O desechar y reemplazar PCB
```

---

## 5. Tabla de eFuses relevantes

| eFuse | Bits | Función | Efecto de quema |
|-------|------|---------|-----------------|
| `SECURE_BOOT_EN` | 1 | Habilitar Secure Boot V2 | Bootloader verifica firma antes de ejecutar app; sin clave privada = irrecuperable |
| `SPI_BOOT_CRYPT_CNT` | 3 | Contador de activación de Flash Encryption | Encriptación AES-XTS en flash; 0=deshabilitada, 1+ = habilitada en orden de bits |
| `HMAC_KEY0` | 256 bits | Clave HMAC para derivar KEK | Datos en eFuse (no legible por SW normal); KeyVault::wrap() la usa |
| `HMAC_KEY0_PURPOSE` | 4 bits | Propósito del KEY0 | HMAC_UP (0x04) = usado para derivar claves; otra = diferente propósito |
| `STRAP_JTAG_SEL` | 1 | Habilitar JTAG | Si Secure Boot=ON, JTAG requiere autenticación con clave privada |

### Ejemplos de estado en distintos puntos

**Estado inicial (virgen):**
```
SECURE_BOOT_EN:                 0
SPI_BOOT_CRYPT_CNT:             0x0
HMAC_KEY0_PURPOSE:              [unquemado]
```

**Post T3.3 (KeyVault quemado):**
```
SECURE_BOOT_EN:                 0
SPI_BOOT_CRYPT_CNT:             0x0
HMAC_KEY0_PURPOSE:              HMAC_UP
```

**Post T4.4 (Secure Boot + Flash Encryption):**
```
SECURE_BOOT_EN:                 1
SPI_BOOT_CRYPT_CNT:             0x1
HMAC_KEY0_PURPOSE:              HMAC_UP
SECURE_BOOT_DIGEST0:            [hash del bootloader firmware]
```

---

## 6. Checklist de provisión de producción

```
[ ] Compilación FASE 1 completada (T1.1–T1.10 per ACTION_PLAN.md)
    - OTA disabled
    - Single-app partition table
    - setupCode default rechazado
    - WebUI autenticada

[ ] Ensayo en DEV-BOARD
    - Flasheo sin eFuse burning
    - Provisión AP → cambio setupCode
    - HomeKit pairing OK
    - NFC tap OK

[ ] Backup pre-irreversible
    - Flash dump
    - eFuse snapshot
    - Claves guardadas en HSM/vault

[ ] T3.3 KeyVault provision
    - Endpoint POST /api/keyvault/provision llamado
    - eFuse HMAC_KEY0_PURPOSE = HMAC_UP
    - reader_sk wrapped post-reboot

[ ] T3.5 NVS encryption (opcional)
    - CONFIG_NVS_ENCRYPTION=y
    - nvs_keys generada y backeada
    - NVS inaccesible sin eFuse key

[ ] T4.4 Secure Boot + Flash Encryption
    - Clave privada secure_boot_signing_key.pem en HSM
    - SECURE_BOOT_EN=1, SPI_BOOT_CRYPT_CNT=1
    - Flash read → datos encriptados
    - Monitor de arranque: "Secure boot v2 enabled"

[ ] Producción
    - HomeKey pareado con iPhone
    - NFC taps exitosos
    - MQTT conectado (TLS obligatorio)
    - Firmware sólo updatable con firma

[ ] Documentación
    - Serial number y fecha de provisión registrados
    - Claves de Secure Boot guardadas con referencia de dispositivo
    - Procedimiento de RMA si es necesario
```

---

## 7. Flujo completo de provisión (resumen visual)

```
┌─────────────────────────────────┐
│  1. Flash firmware base (T3.1)  │
│     • idf.py flash              │
│     • setupCode=46637726        │
│     • KeyVault=disabled         │
└──────────────┬──────────────────┘
               ↓
┌─────────────────────────────────┐
│  2. Portal AP: cambiar          │
│     setupCode (T3.2)            │
│     • SSID: HomeKey-ESP32       │
│     • URL: 192.168.4.1          │
│     • Guardar nuevo setupCode   │
└──────────────┬──────────────────┘
               ↓
┌─────────────────────────────────┐
│  3. KeyVault: quemar eFuse ⛔   │
│     (T3.3)                      │
│     • POST /api/keyvault/        │
│       provision                 │
│     • O comando UART "P"         │
│     • HMAC_KEY0 → eFuse         │
└──────────────┬──────────────────┘
               ↓
┌─────────────────────────────────┐
│  4. Recompile + reflash         │
│     CONFIG_HK_ENABLE_KEYVAULT=y │
│     • reader_sk wrapped         │
│     • NVS keys protected        │
└──────────────┬──────────────────┘
               ↓
┌─────────────────────────────────┐
│  5. NVS encryption ⛔ (T3.5)    │
│     CONFIG_NVS_ENCRYPTION=y     │
│     • nvs_keys generated        │
│     • BACKUP nvs_keys partition │
└──────────────┬──────────────────┘
               ↓
┌─────────────────────────────────┐
│  6. Secure Boot v2 + Flash Enc. │
│     ⛔⛔ (T4.4)                 │
│     • Generar clave privada     │
│     • Quemar SECURE_BOOT_EN=1   │
│     • Flash → AES-XTS           │
│     • IRRECUPERABLE             │
└──────────────┬──────────────────┘
               ↓
        ┌──────────────┐
        │  PRODUCCIÓN  │
        │  Dispositivo │
        │  protegido   │
        └──────────────┘
```

---

## 8. Troubleshooting

### Problema: setupCode aún visible en HTTP plano

**Síntomas:** JSON response contiene `"setupCode":"46637726"`

**Solución:** T1.7 (enmascarar setupCode en serializeToJson)
```bash
# Verificar en WebServerManager.cpp: handleGetConfig()
# Debe filtrar setupCode excepto en primer boot
```

### Problema: KeyVault provision falla con "HMAC key derivation failed"

**Síntomas:** Log `[KeyVault] HMAC key derivation failed`

**Causas:**
1. eFuse HMAC_KEY0 no está correctamente quemada
2. Chip distinto (claves no portables entre chips)

**Solución:**
```bash
# Verificar estado eFuse
esptool.py read_efuse summary | grep HMAC

# Si HMAC_KEY0_PURPOSE != HMAC_UP, re-ejecutar T3.3
# Si en chip diferente, restaurar desde imagen original
```

### Problema: NVS encryption + KeyVault wrap = lentitud en arranque

**Síntomas:** arranque tarda > 5 segundos

**Causas:**
1. Derivación KEK + desencriptación NVS simultáneas
2. Weak key derivation (sin aceleración HW)

**Solución:**
- Verificar `CONFIG_MBEDTLS_HARDWARE_AES=y`
- Considerar caché en RAM post-boot (riesgo: exposición en memoria)

### Problema: Secure Boot quemado pero no se puede flashear imagen nueva (aún sin firmar)

**Síntomas:** `esptool.py write_flash` → "Secure boot enabled, cannot flash unsigned"

**Solución:**
1. Asegurar que `idf.py build` genera `build/homekey_esp32.bin.signed`
2. Si firma falta, sospechar que `.pem` no está en ruta o `CONFIG_SECURE_BOOT_SIGNING_KEY` mal configurado
3. **No hay vuelta atrás**: si perdiste la clave privada, dispositivo es irrecuperable

---

## 9. Referencias

- **ESP-IDF Security Guide:** https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/security/index.html
- **esptool.py:** https://github.com/espressif/esptool
- **espsecure.py:** https://github.com/espressif/espsecure
- **ESP32-C6 Datasheet:** https://www.espressif.com/sites/default/files/documentation/esp32-c6_datasheet_en.pdf
- **HomeSpan GitHub:** https://github.com/HomeSpan/HomeSpan
- **ACTION_PLAN.md:** Pasos de código asociados a cada tarea de provisión

---

**Documento generado:** 2026-05-03  
**Mantenedor:** HomeKey-ESP32 Security Team  
**Última revisión:** Pending review — T4.2, T4.4 aún por implementar en codebase
