#include "wifi_provisioning.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "web_interface.h"  // para COM_ESTADO_TRAVADO / g_estado

// PERSISTÊNCIA DAS CREDENCIAIS
static Preferences wifi_prefs;

bool credenciaisWiFiSalvas() {
  wifi_prefs.begin(NVS_WIFI_NAMESPACE, true); // read-only
  bool tem = wifi_prefs.isKey(NVS_KEY_SSID);
  wifi_prefs.end();
  return tem;
}

void apagarCredenciaisWiFi() {
  wifi_prefs.begin(NVS_WIFI_NAMESPACE, false);
  wifi_prefs.clear();
  wifi_prefs.end();
  Serial.println("[WIFI] Credenciais apagadas da NVS.");
}

static bool carregarCredenciais(char* ssid, char* pass) {
  wifi_prefs.begin(NVS_WIFI_NAMESPACE, true);
  bool ok = wifi_prefs.isKey(NVS_KEY_SSID);
  if (ok) {
    wifi_prefs.getString(NVS_KEY_SSID, ssid, WIFI_MAX_SSID_LEN);
    wifi_prefs.getString(NVS_KEY_PASS, pass, WIFI_MAX_PASS_LEN);
  }
  wifi_prefs.end();
  return ok;
}

static void salvarCredenciais(const char* ssid, const char* pass) {
  wifi_prefs.begin(NVS_WIFI_NAMESPACE, false);
  wifi_prefs.putString(NVS_KEY_SSID, ssid);
  wifi_prefs.putString(NVS_KEY_PASS, pass);
  wifi_prefs.end();
  Serial.printf("[WIFI] Credenciais salvas: SSID='%s'\n", ssid);
}

// TENTATIVA DE CONEXÃO STA
static bool tentarConectarSTA(const char* ssid, const char* pass) {
  Serial.printf("[WIFI] Tentando conectar a '%s'...\n", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(ssid, pass);

  uint32_t inicio = millis();
  while (millis() - inicio < WIFI_STA_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      IPAddress ip = WiFi.localIP();
      char ip_buf[16];
      snprintf(ip_buf, sizeof(ip_buf), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
      Serial.printf("[WIFI] Conectado! IP: %s\n", ip_buf);
      { COM_ESTADO_TRAVADO
        g_estado.wifi_conectado = true;
        strncpy(g_estado.wifi_ip, ip_buf, sizeof(g_estado.wifi_ip) - 1);
        g_estado.wifi_ip[sizeof(g_estado.wifi_ip) - 1] = '\0';
      }
      return true;
    }
    delay(WIFI_POLL_INTERVAL_MS);
    Serial.print(".");
  }
  Serial.println("\n[WIFI] Timeout — credenciais incorretas ou rede fora de alcance.");
  WiFi.disconnect(true);
  return false;
}

// PORTAL CAPTIVO DE CONFIGURAÇÃO
// Página HTML do portal — visual alinhado com o tema da balança (dark, fósforo)
static const char PORTAL_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Balança — Configuração Wi-Fi</title>
<style>
  :root {
    --bg: #0F1210; --panel: #161B17; --linha: #2A2E22;
    --ink: #E7ECE3; --ink-dim: #8A9080;
    --phosphor: #7FBF6B; --phosphor-dim: #4A7A3E;
    --amber: #C9A227; --rust: #C46B54;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg); color: var(--ink);
    font-family: 'Courier New', monospace;
    min-height: 100vh;
    display: flex; align-items: center; justify-content: center;
    padding: 16px;
  }
  .card {
    background: var(--panel);
    border: 1px solid var(--linha);
    border-radius: 16px;
    padding: 36px 32px;
    width: 100%; max-width: 420px;
    box-shadow: 0 8px 40px rgba(0,0,0,0.5);
  }
  .logo { text-align: center; margin-bottom: 28px; }
  .logo-icon {
    font-size: 40px; display: block; margin-bottom: 8px;
    filter: drop-shadow(0 0 12px rgba(127,191,107,0.5));
  }
  h1 { font-size: 18px; color: var(--phosphor); margin-bottom: 4px; }
  .sub { font-size: 12px; color: var(--ink-dim); margin-bottom: 28px; text-align: center; }
  label { display: block; font-size: 11px; color: var(--ink-dim); text-transform: uppercase; letter-spacing: 1px; margin-bottom: 6px; }
  input {
    width: 100%;
    background: var(--bg); border: 1px solid var(--linha);
    color: var(--ink); padding: 10px 12px;
    border-radius: 8px; font-family: inherit; font-size: 14px;
    margin-bottom: 18px;
    transition: border-color 0.2s;
  }
  input:focus { outline: none; border-color: var(--phosphor-dim); }
  .scan-hint { font-size: 11px; color: var(--ink-dim); margin-bottom: 6px; }
  select {
    width: 100%; background: var(--bg); border: 1px solid var(--linha);
    color: var(--ink); padding: 10px 12px; border-radius: 8px;
    font-family: inherit; font-size: 13px; margin-bottom: 18px;
    cursor: pointer;
  }
  button {
    width: 100%; background: var(--phosphor-dim); color: var(--ink);
    border: none; border-radius: 8px;
    padding: 12px; font-family: inherit; font-size: 14px;
    font-weight: bold; letter-spacing: 0.5px;
    cursor: pointer; transition: background 0.2s, transform 0.1s;
  }
  button:hover { background: var(--phosphor); color: #0a0f08; }
  button:active { transform: scale(0.98); }
  .divider { text-align: center; color: var(--ink-dim); font-size: 11px; margin: 14px 0; }
  .msg {
    display: none; text-align: center; padding: 10px;
    border-radius: 8px; font-size: 13px; margin-bottom: 16px;
  }
  .msg.erro { display: block; background: rgba(196,107,84,0.15); color: var(--rust); border: 1px solid var(--rust); }
  .msg.ok   { display: block; background: rgba(127,191,107,0.15); color: var(--phosphor); border: 1px solid var(--phosphor); }
  .spinner { display: none; text-align: center; color: var(--amber); margin-top: 16px; font-size: 13px; }
  .spinner.ativo { display: block; }
</style>
</head>
<body>
<div class="card">
  <div class="logo">
    <span class="logo-icon">⚖️</span>
    <h1>Balança Digital</h1>
  </div>
  <p class="sub">Conecte a balança à sua rede Wi-Fi para acessar o painel de controle.</p>

  <div id="msg" class="msg">__MSG__</div>

  <form id="form" onsubmit="return salvar(event)">
    <label>Rede Wi-Fi (SSID)</label>
    <input type="text" id="ssid" name="ssid" placeholder="Nome da rede" required maxlength="32" autocomplete="off" autocapitalize="none">

    <label>Senha</label>
    <input type="password" id="pass" name="pass" placeholder="Senha da rede" maxlength="64" autocomplete="current-password">

    <button type="submit" id="btn">Salvar e Conectar</button>
  </form>

  <div class="spinner" id="spin">⏳ Salvando... a balança irá reiniciar.</div>
</div>

<script>
function salvar(e) {
  e.preventDefault();
  const ssid = document.getElementById('ssid').value.trim();
  const pass = document.getElementById('pass').value;
  if (!ssid) return false;

  document.getElementById('btn').disabled = true;
  document.getElementById('spin').classList.add('ativo');

  const params = new URLSearchParams({ ssid, pass });
  fetch('/salvar', { method: 'POST', body: params,
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' } })
  .then(r => r.json())
  .then(d => {
    const msg = document.getElementById('msg');
    msg.className = 'msg ' + (d.ok ? 'ok' : 'erro');
    msg.innerText = d.ok
      ? '✓ Salvo! Reiniciando e conectando à rede "' + ssid + '"...'
      : '✗ ' + (d.erro || 'Erro ao salvar.');
    document.getElementById('spin').classList.remove('ativo');
  })
  .catch(() => {
    document.getElementById('btn').disabled = false;
    document.getElementById('spin').classList.remove('ativo');
  });
  return false;
}
</script>
</body>
</html>
)=====";

// Gera página com mensagem de erro opcional injetada
static String gerarPaginaPortal(const char* msg_classe, const char* msg_texto) {
  String html = String(PORTAL_HTML);
  if (msg_texto && strlen(msg_texto) > 0) {
    html.replace("__MSG__", String(msg_texto));
    // Garante que a div de mensagem apareça com a classe correta
    html.replace("class=\"msg\"", String("class=\"msg ") + msg_classe + "\"");
  } else {
    html.replace("__MSG__", "");
  }
  return html;
}

// MODO ACCESS POINT + PORTAL CAPTIVO
static void iniciarPortalCaptivo() {
  Serial.println("[WIFI] Iniciando modo AP + portal captivo...");
  Serial.printf("[WIFI] SSID do AP: '%s' (sem senha)\n", WIFI_AP_SSID);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID);  // AP aberto, sem senha — fácil de conectar
  delay(100);

  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("[WIFI] IP do AP: %s\n", apIP.toString().c_str());

  // DNS que redireciona qualquer domínio para o IP do AP (captive portal)
  DNSServer dns;
  const uint8_t DNS_PORT = 53;
  dns.start(DNS_PORT, "*", apIP);

  // Servidor HTTP exclusivo do portal (porta 80) — não interfere com o
  // WebServer principal da balança que só sobe depois que conectar em STA.
  WebServer portal(80);

  // Rota principal e redirecionamentos comuns de captive portal
  auto servirPortal = [&]() {
    // Força Content-Type correto para o cliente não tentar parsear como texto
    portal.send(200, "text/html; charset=utf-8", gerarPaginaPortal("", ""));
  };

  portal.on("/", HTTP_GET, servirPortal);
  portal.on("/generate_204", HTTP_GET, servirPortal);  // Android
  portal.on("/hotspot-detect.html", HTTP_GET, servirPortal);  // iOS
  portal.on("/ncsi.txt", HTTP_GET, servirPortal);  // Windows

  bool reiniciar = false;
  char ssid_novo[WIFI_MAX_SSID_LEN] = "";
  char pass_novo[WIFI_MAX_PASS_LEN] = "";

  portal.on("/salvar", HTTP_POST, [&]() {
    if (!portal.hasArg("ssid") || portal.arg("ssid").length() == 0) {
      portal.send(400, "application/json", "{\"ok\":false,\"erro\":\"SSID vazio\"}");
      return;
    }

    String s = portal.arg("ssid");
    String p = portal.arg("pass");

    if (s.length() >= WIFI_MAX_SSID_LEN) {
      portal.send(400, "application/json", "{\"ok\":false,\"erro\":\"SSID muito longo\"}");
      return;
    }
    if (p.length() >= WIFI_MAX_PASS_LEN) {
      portal.send(400, "application/json", "{\"ok\":false,\"erro\":\"Senha muito longa\"}");
      return;
    }

    strncpy(ssid_novo, s.c_str(), WIFI_MAX_SSID_LEN - 1);
    strncpy(pass_novo, p.c_str(), WIFI_MAX_PASS_LEN - 1);

    salvarCredenciais(ssid_novo, pass_novo);
    portal.send(200, "application/json", "{\"ok\":true}");
    reiniciar = true;
  });

  // Qualquer outra rota → redireciona para o portal
  portal.onNotFound([&]() {
    portal.sendHeader("Location", "http://192.168.4.1/", true);
    portal.send(302, "text/plain", "");
  });

  portal.begin();
  Serial.println("[WIFI] Portal captivo ativo. Conecte-se ao AP 'Balanca-Config'.");
  Serial.println("[WIFI] Acesse http://192.168.4.1 no navegador.");

  // Loop do portal — permanece aqui até o usuário salvar as credenciais
  while (!reiniciar) {
    dns.processNextRequest();
    portal.handleClient();
    delay(5);
  }

  portal.stop();
  dns.stop();

  Serial.println("[WIFI] Credenciais recebidas. Reiniciando em 2 segundos...");
  delay(2000);
  ESP.restart();
}

// PONTO DE ENTRADA PRINCIPAL
void iniciarConexaoWiFi() {
  char ssid[WIFI_MAX_SSID_LEN] = "";
  char pass[WIFI_MAX_PASS_LEN] = "";

  bool tem_credenciais = carregarCredenciais(ssid, pass);

  if (tem_credenciais) {
    Serial.printf("[WIFI] Credenciais encontradas na NVS (SSID='%s'). Conectando...\n", ssid);
    if (tentarConectarSTA(ssid, pass)) {
      return; // Conectado com sucesso — retorna e o firmware continua normalmente
    }
    // Falhou: credenciais inválidas ou rede fora de alcance → apaga e abre portal
    Serial.println("[WIFI] Falha na conexão. Abrindo portal para reconfiguração.");
    apagarCredenciaisWiFi();
  } else {
    Serial.println("[WIFI] Sem credenciais na NVS. Abrindo portal de configuração.");
  }

  // Portal captivo — só retorna desta função via ESP.restart()
  iniciarPortalCaptivo();
}
