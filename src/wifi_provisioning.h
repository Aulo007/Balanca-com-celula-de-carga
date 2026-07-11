#pragma once
#include <Arduino.h>

// Gerencia credenciais Wi-Fi persistidas na NVS para evitar senhas no código-fonte.
//
// Fluxo de boot:
//   1. Tenta carregar SSID/senha da NVS ("wifi_prov").
//   2. Se encontrar, tenta conectar em modo STA (até WIFI_STA_TIMEOUT_MS).
//   3. Se não encontrar ou falhar, sobe em modo Access Point "Balanca-Config"
//      e serve o portal captivo de configuração.
//   4. No portal, o usuário digita SSID + senha. O ESP32 salva na NVS e reinicia.
//   5. No próximo boot, conecta normalmente.

constexpr char WIFI_AP_SSID[]    = "Balanca-Config";
constexpr uint32_t WIFI_STA_TIMEOUT_MS  = 15000;
constexpr uint32_t WIFI_POLL_INTERVAL_MS = 500;

constexpr int WIFI_MAX_SSID_LEN = 33;   // 32 chars + null
constexpr int WIFI_MAX_PASS_LEN = 65;   // 64 chars + null

constexpr int WIFI_MAX_PASS_LEN = 65;   // 64 chars + null

constexpr char NVS_WIFI_NAMESPACE[] = "wifi_prov";
constexpr char NVS_KEY_SSID[]       = "ssid";
constexpr char NVS_KEY_PASS[]       = "pass";

/**
 * Inicia a conexão Wi-Fi via credenciais da NVS ou portal captivo.
 * Bloqueia a execução até que a conexão STA seja estabelecida, 
 * ou o usuário reconfigure via AP e o ESP32 reinicie.
 */
void iniciarConexaoWiFi();

/**
 * Verifica a existência de credenciais salvas sem testá-las.
 * @returns true se as chaves existirem na NVS.
 */
bool credenciaisWiFiSalvas();

/**
 * Remove credenciais de Wi-Fi da NVS.
 * Útil para forçar o portal de configuração no próximo boot.
 */
void apagarCredenciaisWiFi();
