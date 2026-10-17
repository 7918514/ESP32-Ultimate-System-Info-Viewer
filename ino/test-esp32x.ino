/*
 * ============================================================================
 *  ESP32 Ultimate System Info Viewer
 *  --------------------------------------------------------------------------
 *  Кросс-платформенный скетч для Arduino Core 3.3.12 (ESP-IDF v5.5.5).
 *  Корректно компилируется и работает на всех актуальных чипах ESP32:
 *  ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C5, ESP32-C6, ESP32-P4, ESP32-H2.
 *
 *  Что выводит:
 *    - Модель, ревизию, количество ядер, частоты
 *    - Источник тактирования CPU, частоту кварца, RTC slow clock
 *    - Уникальные идентификаторы (MAC-адреса, eFuse, Flash UID)
 *    - Характеристики Flash-памяти и внешней PSRAM
 *    - Состояние внутренней SRAM с оценкой фрагментации
 *    - Температуру ядра
 *    - Детальную причину последнего сброса для каждого ядра
 *    - Версии ESP-IDF и Arduino Core
 * ============================================================================
 */

// ----------------------------------------------------------------------------
//  ПОДКЛЮЧАЕМЫЕ ЗАГОЛОВОЧНЫЕ ФАЙЛЫ
// ----------------------------------------------------------------------------
#include <WiFi.h>                 // Нужен для получения MAC-адреса Wi-Fi
#include "esp_chip_info.h"        // Структура с информацией о чипе
#include "esp_system.h"           // Системные функции (esp_get_idf_version)
#include "esp_flash.h"            // Функции для работы с flash-памятью
#include "esp_mac.h"              // Чтение MAC-адресов
#include "esp_heap_caps.h"        // Работа с кучей (heap) и типами памяти
#include "esp_private/esp_clk.h"  // Публичные функции тактирования (esp_clk_xtal_freq, esp_clk_cpu_freq)
#include "esp_clk_tree.h"         // API Clock Tree (esp_clk_tree_src_get_freq_hz)
#include "esp_rom_sys.h"          // esp_rom_get_reset_reason()
#include "soc/reset_reasons.h"    // Константы RESET_REASON_* для разных чипов
#include "soc/clk_tree_defs.h"    // Константы SOC_RTC_SLOW_CLK_SRC_*

// Подключаем esp_psram.h только если он есть в данной сборке
#if __has_include("esp_psram.h")
  #include "esp_psram.h"
#endif

// ============================================================================
//  ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// ============================================================================

// ----------------------------------------------------------------------------
//  Форматирование MAC-адреса (6 байт) в строку "AA:BB:CC:DD:EE:FF".
//  ВАЖНО: используем поиндексовый доступ mac[0]...mac[5], иначе в буфер
//  попадёт адрес ячейки памяти, а не реальный MAC-адрес.
// ----------------------------------------------------------------------------
String macToString(const uint8_t* mac) {
  char buf[18]; // 6 байт * 2 символа + 5 двоеточий + нулевой байт = 18
  sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// ----------------------------------------------------------------------------
//  Проверка поддержки диапазона 5 ГГц.
//  На момент написания скетча только ESP32-C5 и ESP32-E22 поддерживают 5 ГГц.
//  ESP32-C6, несмотря на Wi-Fi 6, работает только в 2.4 ГГц.
// ----------------------------------------------------------------------------
bool chipSupports5GHz() {
#if CONFIG_IDF_TARGET_ESP32C5
  return true;   // ESP32-C5 — нативный чип с поддержкой 2.4 и 5 ГГц
#elif CONFIG_IDF_TARGET_ESP32E22
  return true;   // ESP32-E22 — сопроцессор Wi-Fi 6E (2.4/5/6 ГГц)
#else
  return false;  // Все остальные чипы работают только в 2.4 ГГц
#endif
}

// ----------------------------------------------------------------------------
//  Высокоуровневая причина сброса (esp_reset_reason_t) — переносимая.
//  Этот тип есть во всех семействах ESP32 и не зависит от ревизии чипа.
// ----------------------------------------------------------------------------
String espResetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:    return "Unknown";
    case ESP_RST_POWERON:    return "Power-on reset";
    case ESP_RST_EXT:        return "External pin reset";
    case ESP_RST_SW:         return "Software reset";
    case ESP_RST_PANIC:      return "Exception/Panic reset";
    case ESP_RST_INT_WDT:    return "Interrupt watchdog reset";
    case ESP_RST_TASK_WDT:   return "Task watchdog reset";
    case ESP_RST_WDT:        return "Other watchdog reset";
    case ESP_RST_DEEPSLEEP:  return "Deep sleep wakeup";
    case ESP_RST_BROWNOUT:   return "Brownout reset";
    case ESP_RST_SDIO:       return "SDIO reset";
    case ESP_RST_USB:        return "USB reset";
    case ESP_RST_JTAG:       return "JTAG reset";
    case ESP_RST_EFUSE:      return "eFuse error reset";
    case ESP_RST_PWR_GLITCH: return "Power glitch reset";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup reset";
    default:                 return "Unknown";
  }
}

// ----------------------------------------------------------------------------
//  Низкоуровневая причина сброса — для каждого чипа свой набор констант.
//  Тип аргумента 'int' позволяет избежать несоответствия типов, так как
//  soc_reset_reason_t в разных чипах определён по-разному.
// ----------------------------------------------------------------------------
String romResetReasonToString(int reason) {

#if CONFIG_IDF_TARGET_ESP32
  // ======================= ESP32 (классический) =======================
  if (reason == RESET_REASON_CHIP_POWER_ON)      return "Power-on reset";
  if (reason == RESET_REASON_CORE_SW)            return "Software reset (digital core)";
  if (reason == RESET_REASON_CORE_DEEP_SLEEP)    return "Deep sleep reset";
  if (reason == RESET_REASON_CORE_SDIO)          return "SDIO reset";
  if (reason == RESET_REASON_CORE_MWDT0)         return "Main watchdog 0 reset (digital core)";
  if (reason == RESET_REASON_CORE_MWDT1)         return "Main watchdog 1 reset (digital core)";
  if (reason == RESET_REASON_CORE_RTC_WDT)       return "RTC watchdog reset (digital core)";
  if (reason == RESET_REASON_CPU0_MWDT0)         return "Main watchdog 0 reset (CPU0)";
  if (reason == RESET_REASON_CPU1_MWDT1)         return "Main watchdog 1 reset (CPU1)";
  if (reason == RESET_REASON_CPU0_SW)            return "Software reset (CPU0)";
  if (reason == RESET_REASON_CPU1_SW)            return "Software reset (CPU1)";
  if (reason == RESET_REASON_CPU0_RTC_WDT)       return "RTC watchdog reset (CPU0)";
  if (reason == RESET_REASON_CPU1_RTC_WDT)       return "RTC watchdog reset (CPU1)";
  if (reason == RESET_REASON_CPU1_CPU0)          return "CPU0 resets CPU1";
  if (reason == RESET_REASON_SYS_BROWN_OUT)      return "Brownout reset (system)";
  if (reason == RESET_REASON_SYS_RTC_WDT)        return "RTC watchdog reset (system)";
  return "Unknown";

#elif CONFIG_IDF_TARGET_ESP32S2
  // ======================= ESP32-S2 =======================
  // ВАЖНО: Для ESP32-S2 в soc/reset_reasons.h (release/v5.5) отсутствуют
  // RESET_REASON_CORE_SDIO, RESET_REASON_CORE_USB_UART и RESET_REASON_CORE_USB_JTAG.
  // Зато есть RESET_REASON_SYS_CLK_GLITCH.
  if (reason == RESET_REASON_CHIP_POWER_ON)      return "Power-on reset";
  if (reason == RESET_REASON_CORE_SW)            return "Software reset (digital core)";
  if (reason == RESET_REASON_CORE_DEEP_SLEEP)    return "Deep sleep reset";
  if (reason == RESET_REASON_CORE_MWDT0)         return "Main watchdog 0 reset (digital core)";
  if (reason == RESET_REASON_CORE_MWDT1)         return "Main watchdog 1 reset (digital core)";
  if (reason == RESET_REASON_CORE_RTC_WDT)       return "RTC watchdog reset (digital core)";
  if (reason == RESET_REASON_CPU0_MWDT0)         return "Main watchdog 0 reset (CPU0)";
  if (reason == RESET_REASON_CPU0_SW)            return "Software reset (CPU0)";
  if (reason == RESET_REASON_CPU0_RTC_WDT)       return "RTC watchdog reset (CPU0)";
  if (reason == RESET_REASON_SYS_BROWN_OUT)      return "Brownout reset (system)";
  if (reason == RESET_REASON_SYS_RTC_WDT)        return "RTC watchdog reset (system)";
  if (reason == RESET_REASON_CPU0_MWDT1)         return "Main watchdog 1 reset (CPU0)";
  if (reason == RESET_REASON_SYS_SUPER_WDT)      return "Super watchdog reset (system)";
  if (reason == RESET_REASON_SYS_CLK_GLITCH)     return "Clock glitch reset";
  if (reason == RESET_REASON_CORE_EFUSE_CRC)     return "eFuse CRC error reset";
  return "Unknown";

#elif CONFIG_IDF_TARGET_ESP32S3
  // ======================= ESP32-S3 =======================
  if (reason == RESET_REASON_CHIP_POWER_ON)      return "Power-on reset";
  if (reason == RESET_REASON_CORE_SW)            return "Software reset (digital core)";
  if (reason == RESET_REASON_CORE_DEEP_SLEEP)    return "Deep sleep reset";
  if (reason == RESET_REASON_CORE_MWDT0)         return "Main watchdog 0 reset (digital core)";
  if (reason == RESET_REASON_CORE_MWDT1)         return "Main watchdog 1 reset (digital core)";
  if (reason == RESET_REASON_CORE_RTC_WDT)       return "RTC watchdog reset (digital core)";
  if (reason == RESET_REASON_CPU0_MWDT0)         return "Main watchdog 0 reset (CPU0)";
  if (reason == RESET_REASON_CPU0_SW)            return "Software reset (CPU0)";
  if (reason == RESET_REASON_CPU0_RTC_WDT)       return "RTC watchdog reset (CPU0)";
  if (reason == RESET_REASON_SYS_BROWN_OUT)      return "Brownout reset (system)";
  if (reason == RESET_REASON_SYS_RTC_WDT)        return "RTC watchdog reset (system)";
  if (reason == RESET_REASON_CPU0_MWDT1)         return "Main watchdog 1 reset (CPU0)";
  if (reason == RESET_REASON_SYS_SUPER_WDT)      return "Super watchdog reset (system)";
  if (reason == RESET_REASON_SYS_CLK_GLITCH)     return "Clock glitch reset";
  if (reason == RESET_REASON_CORE_EFUSE_CRC)     return "eFuse CRC error reset";
  if (reason == RESET_REASON_CORE_USB_UART)      return "USB UART reset";
  if (reason == RESET_REASON_CORE_USB_JTAG)      return "USB JTAG reset";
  if (reason == RESET_REASON_CORE_PWR_GLITCH)    return "Power glitch reset";
  return "Unknown";

#elif CONFIG_IDF_TARGET_ESP32C3
  // ======================= ESP32-C3 =======================
  if (reason == RESET_REASON_CHIP_POWER_ON)      return "Power-on reset";
  if (reason == RESET_REASON_CORE_SW)            return "Software reset (digital core)";
  if (reason == RESET_REASON_CORE_DEEP_SLEEP)    return "Deep sleep reset";
  if (reason == RESET_REASON_CORE_MWDT0)         return "Main watchdog 0 reset (digital core)";
  if (reason == RESET_REASON_CORE_MWDT1)         return "Main watchdog 1 reset (digital core)";
  if (reason == RESET_REASON_CORE_RTC_WDT)       return "RTC watchdog reset (digital core)";
  if (reason == RESET_REASON_CPU0_MWDT0)         return "Main watchdog 0 reset (CPU0)";
  if (reason == RESET_REASON_CPU0_SW)            return "Software reset (CPU0)";
  if (reason == RESET_REASON_CPU0_RTC_WDT)       return "RTC watchdog reset (CPU0)";
  if (reason == RESET_REASON_SYS_BROWN_OUT)      return "Brownout reset (system)";
  if (reason == RESET_REASON_SYS_RTC_WDT)        return "RTC watchdog reset (system)";
  if (reason == RESET_REASON_SYS_SUPER_WDT)      return "Super watchdog reset (system)";
  if (reason == RESET_REASON_SYS_CLK_GLITCH)     return "Clock glitch reset";
  if (reason == RESET_REASON_CORE_EFUSE_CRC)     return "eFuse CRC error reset";
  return "Unknown";

#elif CONFIG_IDF_TARGET_ESP32C6
  // ======================= ESP32-C6 =======================
  // ВАЖНО: Для ESP32-C6 в ESP-IDF v5.5 отсутствуют константы
  // RESET_REASON_SYS_CLK_GLITCH и RESET_REASON_CORE_PWR_GLITCH.
  // Зато есть RESET_REASON_CPU0_JTAG.
  // Источник: components/soc/esp32c6/include/soc/reset_reasons.h (release/v5.5).
  if (reason == RESET_REASON_CHIP_POWER_ON)      return "Power-on reset";
  if (reason == RESET_REASON_CORE_SW)            return "Software reset (digital core)";
  if (reason == RESET_REASON_CORE_DEEP_SLEEP)    return "Deep sleep reset";
  if (reason == RESET_REASON_CORE_SDIO)          return "SDIO reset";
  if (reason == RESET_REASON_CORE_MWDT0)         return "Main watchdog 0 reset (digital core)";
  if (reason == RESET_REASON_CORE_MWDT1)         return "Main watchdog 1 reset (digital core)";
  if (reason == RESET_REASON_CORE_RTC_WDT)       return "RTC watchdog reset (digital core)";
  if (reason == RESET_REASON_CPU0_MWDT0)         return "Main watchdog 0 reset (CPU0)";
  if (reason == RESET_REASON_CPU0_SW)            return "Software reset (CPU0)";
  if (reason == RESET_REASON_CPU0_RTC_WDT)       return "RTC watchdog reset (CPU0)";
  if (reason == RESET_REASON_SYS_BROWN_OUT)      return "Brownout reset (system)";
  if (reason == RESET_REASON_SYS_RTC_WDT)        return "RTC watchdog reset (system)";
  if (reason == RESET_REASON_CPU0_MWDT1)         return "Main watchdog 1 reset (CPU0)";
  if (reason == RESET_REASON_SYS_SUPER_WDT)      return "Super watchdog reset (system)";
  if (reason == RESET_REASON_CORE_EFUSE_CRC)     return "eFuse CRC error reset";
  if (reason == RESET_REASON_CORE_USB_UART)      return "USB UART reset";
  if (reason == RESET_REASON_CORE_USB_JTAG)      return "USB JTAG reset";
  if (reason == RESET_REASON_CPU0_JTAG)          return "JTAG reset (CPU0)";
  return "Unknown";

#elif CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32C5
  // ============== ESP32-H2, ESP32-C5 (сокращённый набор констант) ==============
  if (reason == RESET_REASON_CHIP_POWER_ON)      return "Power-on reset";
  if (reason == RESET_REASON_CORE_SW)            return "Software reset (digital core)";
  if (reason == RESET_REASON_CORE_DEEP_SLEEP)    return "Deep sleep reset";
  if (reason == RESET_REASON_CORE_MWDT0)         return "Main watchdog 0 reset (digital core)";
  if (reason == RESET_REASON_CORE_MWDT1)         return "Main watchdog 1 reset (digital core)";
  if (reason == RESET_REASON_CORE_RTC_WDT)       return "RTC watchdog reset (digital core)";
  if (reason == RESET_REASON_CPU0_MWDT0)         return "Main watchdog 0 reset (CPU0)";
  if (reason == RESET_REASON_CPU0_SW)            return "Software reset (CPU0)";
  if (reason == RESET_REASON_CPU0_RTC_WDT)       return "RTC watchdog reset (CPU0)";
  if (reason == RESET_REASON_SYS_BROWN_OUT)      return "Brownout reset (system)";
  if (reason == RESET_REASON_SYS_RTC_WDT)        return "RTC watchdog reset (system)";
  if (reason == RESET_REASON_CPU0_MWDT1)         return "Main watchdog 1 reset (CPU0)";
  if (reason == RESET_REASON_SYS_SUPER_WDT)      return "Super watchdog reset (system)";
  return "Unknown";

#elif CONFIG_IDF_TARGET_ESP32P4
  // ======================= ESP32-P4 =======================
  // ВАЖНО: Для ESP32-P4 используется обновлённый набор констант
  // без суффиксов 0/1, а RTC_WDT заменён на RWDT.
  if (reason == RESET_REASON_CHIP_POWER_ON)      return "Power-on reset";
  if (reason == RESET_REASON_CORE_SW)            return "Software reset (digital core)";
  if (reason == RESET_REASON_CORE_DEEP_SLEEP)    return "Deep sleep reset";
  if (reason == RESET_REASON_CORE_MWDT)          return "Main watchdog reset (digital core)";
  if (reason == RESET_REASON_CORE_RWDT)          return "RTC watchdog reset (digital core)";
  if (reason == RESET_REASON_CPU_MWDT)           return "Main watchdog reset (CPU)";
  if (reason == RESET_REASON_CPU_RWDT)           return "RTC watchdog reset (CPU)";
  if (reason == RESET_REASON_SYS_BROWN_OUT)      return "Brownout reset (system)";
  if (reason == RESET_REASON_SYS_RWDT)           return "RTC watchdog reset (system)";
  if (reason == RESET_REASON_SYS_SUPER_WDT)      return "Super watchdog reset (system)";
  return "Unknown";

#else
  // ======================= Неизвестный чип =======================
  return "Unknown chip (code " + String(reason) + ")";
#endif
}

// ----------------------------------------------------------------------------
//  Печать разделителя строк для наглядности вывода.
// ----------------------------------------------------------------------------
void printSeparator() {
  Serial.println("--------------------------------------------------");
}

// ============================================================================
//  SETUP — выполняется один раз при старте
// ============================================================================
void setup() {
  Serial.begin(115200);
  
  // ---- Классический паттерн ожидания готовности последовательного порта ----
  unsigned long t0 = millis();                  // Запоминаем момент старта ожидания
  while (!Serial && (millis() - t0) < 3000) {   // Ждём, пока Serial не готов ИЛИ не прошло 3 секунды
    delay(10);                                  // Небольшая пауза между проверками
  }
  delay(1500);

  Serial.println();
  Serial.println("==================================================");
  Serial.printf("    %s — Ultimate System Info Viewer\n", ESP.getChipModel());
  Serial.println("==================================================");

  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  // ==========================================================================
  //  БЛОК 1: ЧИП И АРХИТЕКТУРА
  // ==========================================================================
  printSeparator();
  Serial.println("[ ЧИП И АРХИТЕКТУРА ]");
  printSeparator();

  Serial.printf("Модель чипа       : %s\n", ESP.getChipModel());
  Serial.printf("Ревизия чипа      : v%d.%d (raw: %d)\n",
                chip_info.revision / 100,
                chip_info.revision % 100,
                chip_info.revision);
  Serial.printf("Количество ядер   : %d\n", chip_info.cores);
  Serial.printf("Частота CPU       : %lu МГц\n", (unsigned long)getCpuFrequencyMhz());

  // ---- Реальная частота кварца ----
  int xtal_hz = esp_clk_xtal_freq();
  Serial.printf("Частота кварца    : %lu МГц\n", (unsigned long)(xtal_hz / 1000000));

  // ---- Источник тактирования CPU ----
  uint32_t cpu_freq_hz = 0;
  esp_err_t err_cpu = esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU,
                                                   ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
                                                   &cpu_freq_hz);
  if (err_cpu == ESP_OK) {
    Serial.printf("CPU (Clock Tree)  : %lu МГц\n", (unsigned long)(cpu_freq_hz / 1000000));
  } else {
    Serial.println("CPU (Clock Tree)  : Не удалось определить");
  }

  // ---- Источник медленного тактирования RTC ----
  uint32_t rtc_slow_hz = 0;
  esp_err_t err_rtc = esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_RTC_SLOW,
                                                   ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
                                                   &rtc_slow_hz);
  const char* slow_str = "Неизвестно";
  if (err_rtc == ESP_OK) {
    if (rtc_slow_hz > 140000 && rtc_slow_hz < 160000) {
      slow_str = "RC_SLOW (~150 кГц)";
    } else if (rtc_slow_hz == 32768) {
      slow_str = "XTAL32K (32.768 кГц)";
    } else if (rtc_slow_hz > 0) {
      slow_str = "RC_FAST / Другой источник";
    }
    Serial.printf("RTC slow clock    : %s (%lu Гц)\n", slow_str, (unsigned long)rtc_slow_hz);
  } else {
    Serial.println("RTC slow clock    : Не удалось определить");
  }

  // ---- Встроенные фичи: Flash, PSRAM ----
  Serial.print("Встроенный Flash  : ");
  Serial.println((chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "Да" : "Нет (внешний)");

  Serial.print("Встроенный PSRAM  : ");
  Serial.println((chip_info.features & CHIP_FEATURE_EMB_PSRAM) ? "Да" : "Нет (внешний)");

  // ---- Беспроводная связь ----
  Serial.printf("Wi-Fi 2.4 ГГц     : %s\n",
                (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "Да" : "Нет");
  Serial.printf("Wi-Fi 5 ГГц       : %s\n",
                chipSupports5GHz() ? "Да" : "Нет");
  Serial.printf("Bluetooth Classic : %s\n",
                (chip_info.features & CHIP_FEATURE_BT) ? "Да" : "Нет");
  Serial.printf("Bluetooth LE      : %s\n",
                (chip_info.features & CHIP_FEATURE_BLE) ? "Да" : "Нет");
  Serial.printf("IEEE 802.15.4     : %s\n",
                (chip_info.features & CHIP_FEATURE_IEEE802154) ? "Да" : "Нет");

  // ==========================================================================
  //  БЛОК 2: УНИКАЛЬНЫЕ ИДЕНТИФИКАТОРЫ
  // ==========================================================================
  printSeparator();
  Serial.println("[ УНИКАЛЬНЫЕ ИДЕНТИФИКАТОРЫ ]");
  printSeparator();

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);    Serial.printf("Wi-Fi STA MAC     : %s\n", macToString(mac).c_str());
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP); Serial.printf("Wi-Fi AP MAC      : %s\n", macToString(mac).c_str());
  esp_read_mac(mac, ESP_MAC_BT);          Serial.printf("Bluetooth MAC     : %s\n", macToString(mac).c_str());
  esp_read_mac(mac, ESP_MAC_BASE);        Serial.printf("Base MAC          : %s\n", macToString(mac).c_str());

  uint64_t chipid = ESP.getEfuseMac();
  Serial.printf("eFuse MAC (hex)   : %04llX%08llX\n",
                (unsigned long long)(chipid >> 32),
                (unsigned long long)(chipid & 0xFFFFFFFF));
  Serial.printf("Chip ID (64-bit)  : %llu\n", (unsigned long long)chipid);

  uint64_t flash_uid = 0;
  esp_err_t err = esp_flash_read_unique_chip_id(NULL, &flash_uid);
  if (err == ESP_OK) {
    Serial.printf("Flash Unique ID   : %016llX\n", (unsigned long long)flash_uid);
  } else {
    Serial.println("Flash Unique ID   : Не поддерживается кристаллом Flash");
  }

  // ==========================================================================
  //  БЛОК 3: FLASH-ПАМЯТЬ
  // ==========================================================================
  printSeparator();
  Serial.println("[ FLASH-ПАМЯТЬ ]");
  printSeparator();

  uint32_t flash_size = 0;
  esp_flash_get_size(NULL, &flash_size);
  Serial.printf("Тип размещения    : %s\n",
                (chip_info.features & CHIP_FEATURE_EMB_FLASH)
                  ? "Встроенная (Embedded)"
                  : "Внешняя на плате (External)");
  Serial.printf("Размер Flash      : %lu МБ (%lu байт)\n",
                (unsigned long)(flash_size / (1024 * 1024)),
                (unsigned long)flash_size);
  Serial.printf("Скорость Flash    : %lu МГц\n",
                (unsigned long)(ESP.getFlashChipSpeed() / 1000000));

  FlashMode_t mode = ESP.getFlashChipMode();
  Serial.printf("Режим шины Flash  : %s\n",
                (mode == FM_QIO)  ? "QIO (Quad I/O)" :
                (mode == FM_QOUT) ? "QOUT (Quad Output)" :
                (mode == FM_DIO)  ? "DIO (Dual I/O)" :
                (mode == FM_DOUT) ? "DOUT (Dual Output)" :
                                    "OPI (Octal SPI) / Специфичный");

  // ==========================================================================
  //  БЛОК 4: ВНЕШНЯЯ PSRAM
  // ==========================================================================
  printSeparator();
  Serial.println("[ PSRAM (ВНЕШНЯЯ RAM) ]");
  printSeparator();

#if defined(BOARD_HAS_PSRAM)
  if (psramFound()) {
    Serial.println("Статус PSRAM      : Обнаружена и готова к работе");
    Serial.printf("Размер PSRAM      : %lu МБ (%lu байт)\n",
                  (unsigned long)(ESP.getPsramSize() / (1024 * 1024)),
                  (unsigned long)ESP.getPsramSize());
    Serial.printf("Свободно в PSRAM  : %lu байт\n",
                  (unsigned long)ESP.getFreePsram());
  } else {
    Serial.println("Статус PSRAM      : Не обнаружена / отключена в настройках");
  }
#else
  Serial.println("Статус PSRAM      : Не поддерживается данной сборкой (BOARD_HAS_PSRAM не задан)");
#endif

  // ==========================================================================
  //  БЛОК 5: ВНУТРЕННЯЯ ПАМЯТЬ (SRAM / HEAP)
  // ==========================================================================
  printSeparator();
  Serial.println("[ ВНУТРЕННЯЯ ПАМЯТЬ (SRAM) ]");
  printSeparator();

  multi_heap_info_t heap_info;
  heap_caps_get_info(&heap_info, MALLOC_CAP_INTERNAL);

  size_t total_internal = heap_info.total_free_bytes + heap_info.total_allocated_bytes;
  Serial.printf("Всего RAM (Heap)  : %d байт\n", (int)ESP.getHeapSize());
  Serial.printf("Текущий своб. Heap: %d байт\n", (int)ESP.getFreeHeap());
  Serial.printf("Мин. когда-либо   : %d байт\n", (int)ESP.getMinFreeHeap());
  Serial.printf("Макс. блок аллок. : %d байт\n", (int)ESP.getMaxAllocHeap());
  Serial.printf("Физическая SRAM   : %d байт\n", (int)total_internal);
  Serial.printf("  крупнейший блок : %d байт\n", (int)heap_info.largest_free_block);
  Serial.printf("  выделено блоков : %d\n", heap_info.allocated_blocks);
  Serial.printf("  свободно блоков : %d\n", heap_info.free_blocks);

  double frag = 0.0;
  if (heap_info.total_free_bytes > 0) {
    frag = 100.0 * (1.0 - (double)heap_info.largest_free_block / heap_info.total_free_bytes);
  }
  Serial.printf("  фрагментация    : %.1f%%\n", frag);

#if CONFIG_IDF_TARGET_ESP32S3
  Serial.println("Архитектура       : ESP32-S3 (унифицированная SRAM)");
  Serial.println("                    DRAM и IRAM — это одна и та же память.");
#elif CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6
  Serial.println("Архитектура       : D/IRAM (часть памяти может быть и данными, и инструкциями)");
  Serial.printf("  DRAM (8-bit)    : %d байт\n", (int)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
  Serial.printf("  IRAM (32-bit)   : %d байт\n", (int)heap_caps_get_free_size(MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL));
#elif CONFIG_IDF_TARGET_ESP32
  Serial.println("Архитектура       : ESP32 (классический, раздельные DRAM/IRAM)");
  size_t free_8  = heap_caps_get_free_size(MALLOC_CAP_8BIT  | MALLOC_CAP_INTERNAL);
  size_t free_32 = heap_caps_get_free_size(MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL);
  Serial.printf("  DRAM (8-bit)    : %d байт\n", (int)free_8);
  Serial.printf("  IRAM (32-bit)   : %d байт\n", (int)(free_32 - free_8));
#endif

  // ==========================================================================
  //  БЛОК 6: БЕЗОПАСНОСТЬ И ЭЛЕКТРОНИКА
  // ==========================================================================
  printSeparator();
  Serial.println("[ БЕЗОПАСНОСТЬ И ЭЛЕКТРОНИКА ]");
  printSeparator();

  bool flash_enc = false;
  bool secure_boot = false;

#if defined(CONFIG_SECURE_FLASH_ENC_ENABLED) \
 || defined(CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT) \
 || defined(CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE)
  flash_enc = true;
#endif

#if defined(CONFIG_SECURE_BOOT_ENABLED) || defined(CONFIG_SECURE_BOOT_V2_ENABLED)
  secure_boot = true;
#endif

  Serial.printf("Шифрование Flash  : %s\n", flash_enc    ? "ВКЛЮЧЕНО (аппаратное)" : "Выключено");
  Serial.printf("Secure Boot       : %s\n", secure_boot  ? "ВКЛЮЧЕНО"              : "Выключено");

  float chip_temp = temperatureRead();
  if (!isnan(chip_temp) && chip_temp > -50.0f) {
    Serial.printf("Температура ядра  : %+.1f °C\n", chip_temp);
  } else {
    Serial.println("Температура ядра  : Ошибка датчика / не поддерживается");
  }

  // ==========================================================================
  //  БЛОК 7: СИСТЕМНЫЕ ДАННЫЕ И СБРОС
  // ==========================================================================
  printSeparator();
  Serial.println("[ СИСТЕМНЫЕ ДАННЫЕ И СБРОС ]");
  printSeparator();

  esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("esp_reset_reason(): %s\n", espResetReasonToString(reason).c_str());

  int rtc0 = (int)esp_rom_get_reset_reason(0);
  Serial.printf("ROM Reset CPU0    : %s\n", romResetReasonToString(rtc0).c_str());

  if (chip_info.cores > 1) {
    int rtc1 = (int)esp_rom_get_reset_reason(1);
    Serial.printf("ROM Reset CPU1    : %s\n", romResetReasonToString(rtc1).c_str());
  }

  Serial.printf("ESP-IDF           : %s\n", esp_get_idf_version());
#if defined(ESP_ARDUINO_VERSION_MAJOR)
  Serial.printf("Arduino Core      : v%d.%d.%d\n",
                ESP_ARDUINO_VERSION_MAJOR,
                ESP_ARDUINO_VERSION_MINOR,
                ESP_ARDUINO_VERSION_PATCH);
#endif
  Serial.printf("Время аптайма     : %lu мс\n", (unsigned long)millis());

  printSeparator();
  Serial.println("=== Конец информации ===");
  Serial.println();
}

void loop() {
  for (int i = 0; i < 100; i++) {
    delay(100);
  }
}