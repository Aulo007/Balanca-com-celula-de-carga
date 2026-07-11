#include <WiFi.h>
#include <WebServer.h>
#include "web_interface.h"
#include "wifi_provisioning.h"

// Credenciais Wi-Fi gerenciadas pelo módulo wifi_provisioning (NVS + portal captivo).
// Não existe mais senha hardcoded aqui -- seguro para o repositório git.

WebServer server(80);

// ESTADO LOCAL DE CAPTURA DE CALIBRAÇÃO MANUAL
// Só as rotas /api/cal/iniciar, /api/cal/confirmar e /api/cal/cancelar
// tocam nisso -- todas rodam dentro do mesmo core (0), chamadas
// sequencialmente por server.handleClient(), então não precisa de mutex
// nem de estar em EstadoCompartilhado.
float g_peso_a_capturar = 0.0;
bool g_aguardando_captura = false;

// PEDIDO -- helper compartilhado com o core 1 (declarado em web_interface.h)
bool enfileirarPedido(TipoPedido tipo, float peso_g, int idx) {
  COM_ESTADO_TRAVADO
  if (g_estado.pedido_em_andamento || g_estado.pedido_seq != g_estado.pedido_seq_atendido) {
    return false; // já tem algo em andamento/na fila
  }
  g_estado.pedido_tipo = tipo;
  g_estado.pedido_peso_g = peso_g;
  g_estado.pedido_idx = idx;
  g_estado.pedido_seq = g_estado.pedido_seq + 1;
  return true;
}

// MÁQUINA DE ESTADOS DA PESAGEM AUTOMÁTICA
// Roda dentro da task deste core (core 0), decidindo transições com base no
// peso já filtrado que o core 1 publica em g_estado a cada volta. A única
// coisa que este lado pede ao core 1 é a captura robusta (PEDIDO_CAPTURA_AUTO)
// quando decide que "objeto foi colocado" -- o core 1 devolve o resultado e
// já deixa o estado em PESAGEM_RESULTADO diretamente (ver main.cpp).
//
// Transições:
//   ZERO        -> DETECTADO   : |peso filtrado| > LIMIAR_ZERO_AUTO_G
//   DETECTADO   -> CALCULANDO  : pedido de captura robusta aceito
//   CALCULANDO  -> RESULTADO   : core 1 terminou a captura (muda o estado direto)
//   RESULTADO   -> ZERO        : |peso filtrado| <= LIMIAR_ZERO_AUTO_G de novo
//                                 (objeto retirado da célula)
//
// LIMIAR_ZERO_AUTO_G = 5.0g -- mesmo valor usado no core 1 para decisão de
// zero, mantido em sincronia aqui só como constante local de leitura (não
// precisa ser a mesma variável, os dois lados concordam no número).
// LIMIAR_ZERO_AUTO_G e DURACAO_MINIMA_ZERANDO_MS definidos em web_interface.h.
uint32_t g_ultima_transicao_pesagem_ms = 0;
const uint32_t DEBOUNCE_DETECCAO_MS = 300;   // evita disparar captura por um solavanco de 1 amostra
const uint32_t TIMEOUT_CALCULANDO_MS = 15000; // guarda-chuva: se ficar "calculando" mais que isso
                                              // sem o core 1 responder (pedido rejeitado por disputa
                                              // com uma tara/calibração manual concorrente, por
                                              // exemplo), volta pro estado anterior em vez de travar
                                              // a UI para sempre em "Calculando...".

// Flag simples: true quando este core AINDA precisa tentar enfileirar a
// captura automática (setado ao entrar em CALCULANDO, limpo assim que
// enfileirarPedido() aceita o pedido). Assim, se a primeira tentativa
// falhar por disputa, a próxima volta do loop tenta de novo sozinha --
// sem isso, um enfileiramento perdido travava a UI em "Calculando..."
// para sempre, já que nada mais tira o estado de CALCULANDO a não ser
// o core 1 terminar um pedido que nunca chegou a ser enviado.
bool g_captura_auto_pendente_de_envio = false;

void processarMaquinaPesagemAuto() {
  COM_ESTADO_TRAVADO

  float peso_atual = g_estado.peso_gramas;
  EstadoPesagem estado = g_estado.estado_pesagem;
  uint32_t agora = millis();

  switch (estado) {
    case PESAGEM_ZERO:
      // Só detecta objeto se o sistema estiver calibrado E o sinal estável --
      // sem calibração, peso_gramas é lixo (raw / 1.0 = milhares de "gramas"),
      // e sem estabilidade, picos de ruído disparam detecção fantasma.
      if (g_estado.calibrado && g_estado.sinal_estavel &&
          fabs(peso_atual) > LIMIAR_ZERO_AUTO_G) {
        g_estado.estado_pesagem = PESAGEM_DETECTADO;
        g_ultima_transicao_pesagem_ms = agora;
      }
      break;

    case PESAGEM_DETECTADO:
      // Debounce: exige que o objeto permaneça fora da faixa de zero por
      // um instante antes de disparar a captura de 5s -- evita disparo por
      // um único pico de ruído isolado que a média móvel ainda não absorveu.
      if (fabs(peso_atual) <= LIMIAR_ZERO_AUTO_G) {

        g_estado.estado_pesagem = PESAGEM_ZERO;
        break;
      }
      // Exige estabilidade do sinal além do debounce temporal -- assim,
      // oscilações sustentadas (ex: vibração na mesa) não disparam captura.
      if (g_estado.sinal_estavel &&
          agora - g_ultima_transicao_pesagem_ms >= DEBOUNCE_DETECCAO_MS) {
        // Marca a intenção de capturar. O envio de fato do pedido
        // acontece fora deste lock, em dispararCapturaAutoSeNecessario() --
        // não podemos chamar enfileirarPedido() aqui dentro porque ele
        // também tenta travar g_estado_mutex (não é recursivo).
        g_estado.estado_pesagem = PESAGEM_CALCULANDO;
        g_ultima_transicao_pesagem_ms = agora; // reaproveitado como início do timeout de CALCULANDO
        g_captura_auto_pendente_de_envio = true;
      }
      break;

    case PESAGEM_CALCULANDO:
      // Guarda-chuva: se por algum motivo o pedido nunca foi aceito (ex:
      // disputou com uma tara manual disparada ao mesmo tempo) e o timeout
      // estourou, desiste e volta para DETECTADO -- na próxima volta o
      // ciclo de debounce tenta de novo, em vez de travar para sempre.
      if (!g_captura_auto_pendente_de_envio &&
          (agora - g_ultima_transicao_pesagem_ms >= TIMEOUT_CALCULANDO_MS)) {
        Serial.println("[PESAGEM] AVISO: captura automatica nao concluiu a tempo -- tentando de novo.");
        g_estado.estado_pesagem = PESAGEM_DETECTADO;
        g_ultima_transicao_pesagem_ms = agora;
      }
      break;

    case PESAGEM_RESULTADO:
      if (fabs(peso_atual) <= LIMIAR_ZERO_AUTO_G) {
        g_estado.estado_pesagem = PESAGEM_ZERANDO;
        g_ultima_transicao_pesagem_ms = agora;  // marca início para duração mínima da animação
      }
      break;

    case PESAGEM_ZERANDO:
      if (fabs(peso_atual) > LIMIAR_ZERO_AUTO_G) {
        // Peso colocado de volta -- retoma detecção imediatamente
        g_estado.estado_pesagem = PESAGEM_DETECTADO;
        g_ultima_transicao_pesagem_ms = agora;
      } else if (g_estado.sinal_estavel &&
                 (agora - g_ultima_transicao_pesagem_ms >= DURACAO_MINIMA_ZERANDO_MS)) {
        // Exige estabilidade E duração mínima -- garante que a animação
        // de "zerando" seja visível por pelo menos 1.5s, mesmo que o
        // sinal estabilize instantaneamente.
        g_estado.estado_pesagem = PESAGEM_ZERO;
      }
      break;
  }
}

// Chamado logo após processarMaquinaPesagemAuto(), fora do mutex -- se
// há uma captura automática pendente de envio (estado acabou de virar
// CALCULANDO, ou uma tentativa anterior foi rejeitada por disputa com
// outro pedido), tenta enfileirar agora.
void dispararCapturaAutoSeNecessario() {
  if (!g_captura_auto_pendente_de_envio) return;

  bool ainda_em_calculando;
  { COM_ESTADO_TRAVADO ainda_em_calculando = (g_estado.estado_pesagem == PESAGEM_CALCULANDO); }
  if (!ainda_em_calculando) {
    // Já foi tirado do estado CALCULANDO por outro caminho (ex: timeout,
    // ou o próprio objeto foi retirado antes de conseguirmos enviar).
    g_captura_auto_pendente_de_envio = false;
    return;
  }

  if (enfileirarPedido(PEDIDO_CAPTURA_AUTO)) {
    g_captura_auto_pendente_de_envio = false;
  }
  // Se enfileirarPedido() retornou false (outro pedido em andamento),
  // deixa a flag ligada -- a próxima volta do loop tenta de novo, até
  // o timeout de PESAGEM_CALCULANDO desistir.
}


// PÁGINA HTML (Front-End)
const char pagina_html[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Balança Digital</title>
<style>
:root {
  --bg: #0F1210;
  --panel: #161B17;
  --linha: #2A2E22;
  --ink: #E7ECE3;
  --ink-dim: #8A9080;
  --phosphor: #7FBF6B;
  --phosphor-dim: #4A7A3E;
  --amber: #C9A227;
  --rust: #C46B54;
}
* { box-sizing: border-box; }
body {
  background: var(--bg);
  color: var(--ink);
  font-family: 'Courier New', monospace;
  margin: 0;
  padding: 16px;
}
.frame { max-width: 720px; margin: 0 auto; }
.conn-warn {
  display: none;
  background: var(--rust);
  color: #1a0d0a;
  padding: 8px 14px;
  border-radius: 6px;
  margin-bottom: 12px;
  font-weight: bold;
  text-align: center;
}
.conn-warn.ativo { display: block; }

.status-strip {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
  margin-bottom: 16px;
}
.seg {
  background: var(--panel);
  border: 1px solid var(--linha);
  border-radius: 20px;
  padding: 4px 12px;
  font-size: 12px;
  color: var(--ink-dim);
  display: flex;
  align-items: center;
  gap: 6px;
}
.dot { width: 8px; height: 8px; border-radius: 50%; background: var(--rust); }
.dot.on { background: var(--phosphor); }
.dot.off { background: var(--rust); }

/* ---- painel principal de pesagem automática ---- */
.readout-auto {
  background: var(--panel);
  border: 1px solid var(--linha);
  border-radius: 12px;
  padding: 32px 20px;
  text-align: center;
  margin-bottom: 16px;
  transition: border-color 0.4s cubic-bezier(0.25, 1, 0.5, 1), box-shadow 0.4s cubic-bezier(0.25, 1, 0.5, 1);
}
.readout-auto.estado-zero { border-color: var(--linha); }
.readout-auto.estado-detectado { border-color: var(--amber); box-shadow: 0 4px 20px rgba(201, 162, 39, 0.08); }
.readout-auto.estado-calculando { border-color: var(--amber); box-shadow: 0 4px 20px rgba(201, 162, 39, 0.15); }
.readout-auto.estado-zerando { border-color: var(--amber); }
.readout-auto.estado-resultado { border-color: var(--phosphor); box-shadow: 0 4px 20px rgba(127, 191, 107, 0.15); }

.auto-label {
  font-size: 13px;
  color: var(--ink-dim);
  text-transform: uppercase;
  letter-spacing: 1px;
  margin-bottom: 12px;
}
.auto-valor {
  font-size: 56px;
  font-weight: bold;
  line-height: 1;
  transition: color 0.4s ease-out, font-size 0.4s cubic-bezier(0.25, 1, 0.5, 1);
}
.auto-valor.zero { color: var(--ink-dim); }
.auto-valor.calculando { color: var(--amber); font-size: 28px; }
.auto-valor.zerando { color: var(--amber); font-size: 28px; animation: pulsar 1.5s ease-in-out infinite; }
.auto-valor.resultado { color: var(--phosphor); }
.auto-unidade { font-size: 22px; color: var(--ink-dim); margin-left: 6px; }

.spinner {
  display: inline-block;
  width: 14px; height: 14px;
  border: 2px solid var(--linha);
  border-top-color: var(--amber);
  border-radius: 50%;
  animation: girar 0.8s linear infinite;
  margin-right: 8px;
  vertical-align: middle;
}
@keyframes girar { to { transform: rotate(360deg); } }
@keyframes pulsar { 0%,100%{opacity:1} 50%{opacity:0.5} }

.readout {
  background: var(--panel);
  border: 1px solid var(--linha);
  border-radius: 12px;
  padding: 16px 20px;
  margin-bottom: 16px;
}
.peso-valor { font-size: 28px; font-weight: bold; }
.peso-unidade { font-size: 16px; color: var(--ink-dim); margin-left: 4px; }
.raw-linha { font-size: 12px; color: var(--ink-dim); margin-top: 6px; }
.estabilidade-linha { font-size: 12px; margin-top: 4px; }
.estabilidade-linha.estavel { color: var(--phosphor); }
.estabilidade-linha.oscilando { color: var(--amber); }

.panel {
  background: var(--panel);
  border: 1px solid var(--linha);
  border-radius: 12px;
  padding: 16px 20px;
  margin-bottom: 16px;
}
.panel-head { display: flex; justify-content: space-between; align-items: baseline; margin-bottom: 12px; }
.panel-head h2 { font-size: 15px; margin: 0; }
.sub { font-size: 12px; color: var(--ink-dim); }

.cal-form { display: flex; gap: 8px; }
.cal-form input {
  flex: 1;
  background: var(--bg);
  border: 1px solid var(--linha);
  color: var(--ink);
  padding: 8px 10px;
  border-radius: 6px;
  font-family: inherit;
}
button {
  font-family: inherit;
  border: none;
  border-radius: 6px;
  padding: 8px 14px;
  cursor: pointer;
  font-size: 13px;
  font-weight: bold;
  letter-spacing: 0.5px;
  transition: background-color 0.2s ease-out, color 0.2s ease-out, transform 0.1s cubic-bezier(0.25, 1, 0.5, 1);
}
button:active:not(:disabled) { transform: scale(0.96); }
.btn-primary { background: var(--phosphor-dim); color: var(--ink); }
.btn-primary:hover:not(:disabled) { background: var(--phosphor); color: #0a0f08; }
.btn-secondary { background: var(--linha); color: var(--ink); }
.btn-secondary:hover:not(:disabled) { background: #383d2e; }
.btn-danger { background: var(--rust); color: #1a0d0a; }
.btn-danger:hover:not(:disabled) { background: #da755c; }
.btn-capture { background: var(--phosphor); color: #0a0f08; }
.btn-capture:hover:not(:disabled) { background: #8cd475; }
.btn-mini { padding: 4px 8px; font-size: 11px; }
button:disabled { opacity: 0.5; cursor: not-allowed; }

.captura-pendente { display: none; margin-top: 12px; padding: 12px; background: var(--bg); border-radius: 8px; }
.captura-pendente.ativo { display: block; }

.grafico-wrap { }
.grafico-legenda { display: flex; gap: 14px; font-size: 11px; color: var(--ink-dim); margin-top: 8px; }
.legenda-marca { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 4px; }

table { width: 100%; border-collapse: collapse; font-size: 12px; margin-top: 8px; }
th, td { text-align: left; padding: 6px 4px; border-bottom: 1px solid var(--linha); }
th.num, td.num { text-align: right; }
.residuo-ok { color: var(--phosphor); }
.residuo-atencao { color: var(--amber); }
.residuo-alto { color: var(--rust); }
.empty-state { color: var(--ink-dim); font-size: 12px; padding: 12px 0; }

.acoes-linha { display: flex; gap: 8px; margin-top: 12px; flex-wrap: wrap; }
.cal-info { display: flex; gap: 14px; font-size: 11px; color: var(--ink-dim); flex-wrap: wrap; margin-top: 8px; }
</style>
</head>
<body>
<div class="frame">
    <div class="conn-warn" id="aviso_conexao">Sem resposta do ESP32 — verifique a conexão</div>

    <div class="status-strip">
        <span class="seg" id="badge_hx711"><span class="dot" id="dot_hx711"></span>célula</span>
        <span class="seg" id="badge_wifi"><span class="dot" id="dot_wifi"></span>rede</span>
        <span class="seg" id="badge_cal"><span class="dot" id="dot_cal"></span>calibração</span>
        <span class="seg" id="badge_estab"><span class="dot" id="dot_estab"></span>sinal</span>
    </div>

    <!-- Painel de pesagem automática: zero -> detectado -> calculando -> resultado -->
    <div class="readout-auto estado-zero" id="painel_auto">
        <div class="auto-label" id="auto_label">célula vazia</div>
        <div class="auto-valor zero" id="auto_valor">0.0<span class="auto-unidade">g</span></div>
    </div>

    <div class="readout">
        <div class="raw-linha">leitura bruta <strong id="val_adc">---</strong></div>
        <div class="estabilidade-linha" id="linha_estab">aguardando convergência...</div>
    </div>

    <div class="panel">
        <div class="panel-head">
            <h2>Novo ponto de calibração</h2>
            <span class="sub" id="pontos_count_sub">0 pontos</span>
        </div>
        <div class="cal-form">
            <input type="number" id="input_peso" step="0.1" placeholder="peso conhecido em gramas">
            <button class="btn-primary" id="btn_iniciar" onclick="iniciarCaptura()">Iniciar captura</button>
        </div>
        <div class="captura-pendente" id="painel_pendente">
            <div>aguardando <strong id="peso_pendente_label">--</strong> g estabilizar na célula</div>
            <div style="display:flex; gap:10px; margin-top:8px;">
                <button class="btn-capture" onclick="confirmarCaptura()">Confirmar</button>
                <button class="btn-secondary" onclick="cancelarCaptura()">Cancelar</button>
            </div>
        </div>
    </div>

    <div class="panel">
        <div class="panel-head">
            <h2>Reta de calibração</h2>
            <span class="sub">peso real × leitura bruta</span>
        </div>
        <div class="grafico-wrap">
            <svg id="svg_grafico" viewBox="0 0 700 280" xmlns="http://www.w3.org/2000/svg"></svg>
            <div class="grafico-legenda">
                <span><span class="legenda-marca" style="background:var(--phosphor)"></span>ajuste bom</span>
                <span><span class="legenda-marca" style="background:var(--amber)"></span>desvio moderado</span>
                <span><span class="legenda-marca" style="background:var(--rust)"></span>desvio alto</span>
            </div>
        </div>
    </div>

    <div class="panel">
        <div class="panel-head">
            <h2>Pontos capturados</h2>
        </div>
        <table id="tabela_pontos">
            <thead>
                <tr><th>#</th><th class="num">peso real</th><th class="num">leitura raw</th><th class="num">desvio</th><th></th></tr>
            </thead>
            <tbody id="tbody_pontos"></tbody>
        </table>
        <div id="empty_pontos" class="empty-state" style="display:none;">Nenhum ponto capturado ainda — a leitura acima mostra o valor bruto sem escala.</div>
        <div class="cal-info">
            <span>fator <strong id="info_fator">--</strong></span>
            <span>offset <strong id="info_intercept">--</strong></span>
            <span>margem <strong id="info_margem">--</strong></span>
            <span>pontos <strong id="info_npontos">--</strong></span>
        </div>
        <div class="acoes-linha">
            <button class="btn-danger" onclick="resetarCalibracao()">Apagar calibração</button>
            <button class="btn-secondary" onclick="reTarar()">Zerar (tara)</button>
        </div>
    </div>

</div>

<script>
let falhasConsecutivas = 0;
let ultimosPontos = [];

function classificarResiduo(residuo_g) {
    const abs = Math.abs(residuo_g);
    if (abs < 0.5) return 'ok';
    if (abs < 2.0) return 'atencao';
    return 'alto';
}

function atualizarDashboard() {
    fetch('/api/data')
    .then(r => r.json())
    .then(data => {
        falhasConsecutivas = 0;
        document.getElementById('aviso_conexao').classList.remove('ativo');

        // ---- painel de pesagem automática ----
        const painelAuto = document.getElementById('painel_auto');
        const labelAuto = document.getElementById('auto_label');
        const valorAuto = document.getElementById('auto_valor');

        painelAuto.className = 'readout-auto estado-' + data.estado_pesagem;

        if (!data.calibrado) {
            labelAuto.innerText = 'calibração pendente';
            valorAuto.className = 'auto-valor zero';
            valorAuto.innerHTML = '---<span class="auto-unidade">g</span>';
        } else if (data.estado_pesagem === 'zero') {
            labelAuto.innerText = 'célula vazia';
            valorAuto.className = 'auto-valor zero';
            valorAuto.innerHTML = '0.0<span class="auto-unidade">g</span>';
        } else if (data.estado_pesagem === 'detectado') {
            labelAuto.innerText = 'objeto detectado...';
            valorAuto.className = 'auto-valor calculando';
            valorAuto.innerHTML = '<span class="spinner"></span>preparando';
        } else if (data.estado_pesagem === 'calculando') {
            labelAuto.innerText = 'pesando';
            valorAuto.className = 'auto-valor calculando';
            valorAuto.innerHTML = '<span class="spinner"></span>Calculando...';
        } else if (data.estado_pesagem === 'zerando') {
            labelAuto.innerText = 'peso retirado — estabilizando';
            valorAuto.className = 'auto-valor zerando';
            valorAuto.innerHTML = '<span class="spinner"></span>Zerando...';
        } else if (data.estado_pesagem === 'resultado') {
            labelAuto.innerText = 'peso medido';
            valorAuto.className = 'auto-valor resultado';
            valorAuto.innerHTML = data.peso_auto.toFixed(1) + '<span class="auto-unidade">g</span>';
        }

        // ---- leitura bruta / estabilidade (diagnóstico) ----
        document.getElementById('val_adc').innerText = data.adc_bruto;

        const linhaEstab = document.getElementById('linha_estab');
        const dotEstab = document.getElementById('dot_estab');
        const badgeEstab = document.getElementById('badge_estab');
        if (data.estavel) {
            linhaEstab.className = 'estabilidade-linha estavel';
            linhaEstab.innerHTML = '&#x2714; ESTÁVEL — variação: ' + data.variacao;
            dotEstab.className = 'dot on';
            badgeEstab.lastChild.textContent = 'sinal: estável';
        } else {
            linhaEstab.className = 'estabilidade-linha oscilando';
            linhaEstab.innerHTML = '&#x25CB; Oscilando — variação: ' + data.variacao;
            dotEstab.className = 'dot off';
            badgeEstab.lastChild.textContent = 'sinal: oscilando';
        }

        setBadge('badge_hx711', 'dot_hx711', data.hx711_ok, 'célula', data.hx711_ok ? 'ok' : 'falha');
        setBadge('badge_wifi', 'dot_wifi', data.wifi_conectado, 'rede', data.wifi_conectado ? 'online' : 'offline', data.wifi_conectado ? data.wifi_ip : null);
        setBadge('badge_cal', 'dot_cal', data.calibrado, 'calibração', data.calibrado ? 'ok' : 'pendente');
    })
    .catch(e => {
        falhasConsecutivas++;
        if (falhasConsecutivas >= 3) {
            document.getElementById('aviso_conexao').classList.add('ativo');
        }
    })
    .finally(() => {
        setTimeout(atualizarDashboard, 500);
    });
}

function setBadge(segId, dotId, ok, label, textoCurto, tooltip) {
    const seg = document.getElementById(segId);
    const dot = document.getElementById(dotId);
    seg.lastChild.textContent = label + ': ' + textoCurto;
    dot.className = 'dot ' + (ok ? 'on' : 'off');
    if (tooltip) seg.title = tooltip;
}

function atualizarPontos() {
    fetch('/api/cal/pontos')
    .then(r => r.json())
    .then(data => {
        ultimosPontos = data.pontos;
        renderizarTabela(data.pontos);
        renderizarGrafico(data.pontos, data.fator, data.intercept);

        document.getElementById('info_fator').innerText = data.fator.toFixed(4);
        document.getElementById('info_intercept').innerText = data.intercept.toFixed(4);
        document.getElementById('info_margem').innerText = data.margem.toFixed(1);
        document.getElementById('info_npontos').innerText = data.pontos.length;
        document.getElementById('pontos_count_sub').innerText = data.pontos.length + (data.pontos.length === 1 ? ' ponto' : ' pontos');
    })
    .catch(e => console.error('Erro ao buscar pontos:', e));
}

function renderizarTabela(pontos) {
    const tbody = document.getElementById('tbody_pontos');
    const empty = document.getElementById('empty_pontos');
    tbody.innerHTML = '';

    if (pontos.length === 0) {
        empty.style.display = 'block';
        return;
    }
    empty.style.display = 'none';

    pontos.forEach((p, idx) => {
        const classe = classificarResiduo(p.residuo_g);
        const sinal = p.residuo_g >= 0 ? '+' : '';
        const tr = document.createElement('tr');
        tr.innerHTML = `<td>${idx + 1}</td>
                         <td class="num">${p.peso_real_g.toFixed(1)}g</td>
                         <td class="num">${p.leitura_raw.toFixed(0)}</td>
                         <td class="num residuo-${classe}">${sinal}${p.residuo_g.toFixed(2)}g</td>
                         <td><button class="btn-danger btn-mini" onclick="removerPonto(${idx})">remover</button></td>`;
        tbody.appendChild(tr);
    });
}

const COR = {
    linha: '#2A2E22',
    inkDim: '#8A9080',
    phosphor: '#7FBF6B',
    phosphorDim: '#4A7A3E',
    amber: '#C9A227',
    rust: '#C46B54'
};

function renderizarGrafico(pontos, fator, intercept) {
    const svg = document.getElementById('svg_grafico');
    const W = 700, H = 280, PAD_L = 56, PAD_R = 20, PAD_T = 16, PAD_B = 36;

    if (pontos.length === 0) {
        svg.innerHTML = `<text x="${W/2}" y="${H/2}" fill="${COR.inkDim}" font-family="monospace" font-size="13" text-anchor="middle">sem pontos capturados</text>`;
        return;
    }

    const pesos = pontos.map(p => p.peso_real_g);
    const raws = pontos.map(p => p.leitura_raw);
    const minX = Math.min(0, ...pesos), maxX = Math.max(...pesos) * 1.1;
    const minY = Math.min(...raws, intercept), maxY = Math.max(...raws, intercept + fator * maxX);

    const xToPx = x => PAD_L + ((x - minX) / (maxX - minX || 1)) * (W - PAD_L - PAD_R);
    const yToPx = y => H - PAD_B - ((y - minY) / (maxY - minY || 1)) * (H - PAD_T - PAD_B);

    let svgContent = '';

    for (let i = 0; i <= 4; i++) {
        const yy = PAD_T + (i / 4) * (H - PAD_T - PAD_B);
        svgContent += `<line x1="${PAD_L}" y1="${yy}" x2="${W - PAD_R}" y2="${yy}" stroke="${COR.linha}" stroke-width="1"/>`;
    }

    const x1 = minX, x2 = maxX;
    const y1 = intercept + fator * x1, y2 = intercept + fator * x2;
    svgContent += `<line x1="${xToPx(x1)}" y1="${yToPx(y1)}" x2="${xToPx(x2)}" y2="${yToPx(y2)}" stroke="${COR.phosphorDim}" stroke-width="1.5" stroke-dasharray="4 3"/>`;

    svgContent += `<line x1="${PAD_L}" y1="${PAD_T}" x2="${PAD_L}" y2="${H - PAD_B}" stroke="${COR.inkDim}" stroke-width="1"/>`;
    svgContent += `<line x1="${PAD_L}" y1="${H - PAD_B}" x2="${W - PAD_R}" y2="${H - PAD_B}" stroke="${COR.inkDim}" stroke-width="1"/>`;
    svgContent += `<text x="${PAD_L}" y="${H - 10}" fill="${COR.inkDim}" font-family="monospace" font-size="10">0g</text>`;
    svgContent += `<text x="${W - PAD_R}" y="${H - 10}" fill="${COR.inkDim}" font-family="monospace" font-size="10" text-anchor="end">${maxX.toFixed(0)}g</text>`;

    pontos.forEach(p => {
        const classe = classificarResiduo(p.residuo_g);
        const cor = classe === 'ok' ? COR.phosphor : (classe === 'atencao' ? COR.amber : COR.rust);
        const px = xToPx(p.peso_real_g), py = yToPx(p.leitura_raw);
        svgContent += `<circle cx="${px}" cy="${py}" r="5" fill="${cor}"/>`;
        svgContent += `<circle cx="${px}" cy="${py}" r="9" fill="${cor}" opacity="0.18"/>`;
    });

    svg.innerHTML = svgContent;
}

function iniciarCaptura() {
    const pesoInput = document.getElementById('input_peso');
    const peso = parseFloat(pesoInput.value);
    if (isNaN(peso) || peso <= 0) return alert('Informe um peso válido.');

    const btn = document.getElementById('btn_iniciar');
    btn.disabled = true;

    fetch('/api/cal/iniciar?peso=' + peso, { method: 'POST' })
    .then(r => r.json())
    .then(data => {
        if (data.ok) {
            document.getElementById('peso_pendente_label').innerText = peso.toFixed(1);
            document.getElementById('painel_pendente').classList.add('ativo');
        } else {
            alert('Erro: ' + (data.erro || 'desconhecido'));
        }
    })
    .catch(() => alert('Falha de comunicação com o ESP32.'))
    .finally(() => { btn.disabled = false; });
}

function confirmarCaptura() {
    const btn = document.querySelector('.btn-capture');
    btn.disabled = true;
    btn.innerText = 'Calibrando...';

    fetch('/api/cal/confirmar', { method: 'POST' })
    .then(r => r.json())
    .then(data => {
        document.getElementById('painel_pendente').classList.remove('ativo');
        document.getElementById('input_peso').value = '';
        if (data.ok) atualizarPontos();
        else alert('Erro: ' + (data.erro || 'desconhecido'));
    })
    .catch(() => alert('Falha de comunicação com o ESP32.'))
    .finally(() => {
        btn.disabled = false;
        btn.innerText = 'Confirmar';
    });
}

function cancelarCaptura() {
    fetch('/api/cal/cancelar', { method: 'POST' }).then(() => {
        document.getElementById('painel_pendente').classList.remove('ativo');
    });
}

function removerPonto(idx) {
    if (!confirm('Remover este ponto de calibração?')) return;
    fetch('/api/cal/remover?idx=' + idx, { method: 'POST' }).then(() => atualizarPontos());
}

function resetarCalibracao() {
    if (!confirm('Isso apaga todos os pontos de calibração salvos. Continuar?')) return;
    fetch('/api/cal/resetar', { method: 'POST' }).then(() => atualizarPontos());
}

function reTarar() {
    if (!confirm('Confirme: a célula está vazia?')) return;
    fetch('/api/tarar', { method: 'POST' })
    .then(r => r.json())
    .then(data => {
        if (!data.ok) alert('Falha ao tarar: ' + (data.erro || 'desconhecido'));
    })
    .catch(() => alert('Falha de comunicação com o ESP32.'));
}

atualizarDashboard();
atualizarPontos();
</script>
</body>
</html>
)=====";

// FUNÇÕES AUXILIARES HTTP
void sendHeadersHTTP() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
}

// ROTAS — RESET WI-FI
void routeWifiResetar() {
  // Apaga as credenciais da NVS e reinicia o ESP32, que vai abrir o portal
  // captivo de configuração no próximo boot.
  sendHeadersHTTP();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"reiniciando para portal de configuracao Wi-Fi\"}");
  delay(500);
  apagarCredenciaisWiFi();
  delay(200);
  ESP.restart();
}

const char* nomeEstadoPesagem(EstadoPesagem e) {
  switch (e) {
    case PESAGEM_ZERO: return "zero";
    case PESAGEM_DETECTADO: return "detectado";
    case PESAGEM_CALCULANDO: return "calculando";
    case PESAGEM_ZERANDO: return "zerando";
    case PESAGEM_RESULTADO: return "resultado";
  }
  return "zero";
}

// ROTAS — DASHBOARD / DADOS
void routeHome() {
  sendHeadersHTTP();
  server.send_P(200, "text/html", pagina_html);
}

void routeData() {
  // Copia rápida sob mutex -- solta o lock ANTES de formatar o JSON e
  // enviar pela rede. server.send() faz I/O de socket, que pode levar
  // alguns ms; segurar o mutex durante isso atrasaria desnecessariamente
  // o core 1, que chama esta trava a cada volta do loop de leitura.
  EstadoCompartilhado copia;
  {
    COM_ESTADO_TRAVADO
    copia = g_estado;
  }

  char jsonBuffer[480];
  snprintf(jsonBuffer, sizeof(jsonBuffer),
           "{\"adc_bruto\":%.0f,\"peso\":\"%.1f\",\"hx711_ok\":%s,\"wifi_conectado\":%s,\"wifi_ip\":\"%s\",\"calibrado\":%s,"
           "\"estavel\":%s,\"variacao\":%ld,\"estado_pesagem\":\"%s\",\"peso_auto\":%.2f}",
           copia.peso_raw_filtrado,
           copia.peso_gramas,
           copia.hx711_ok ? "true" : "false",
           copia.wifi_conectado ? "true" : "false",
           copia.wifi_ip,
           copia.calibrado ? "true" : "false",
           copia.sinal_estavel ? "true" : "false",
           copia.variacao_atual,
           nomeEstadoPesagem(copia.estado_pesagem),
           copia.peso_auto_resultado_g);

  sendHeadersHTTP();
  server.send(200, "application/json", jsonBuffer);
}

// ROTAS — CALIBRAÇÃO DINÂMICA
const size_t TAM_BUFFER_PONTOS = 140 * WEB_MAX_PONTOS_CAL + 128;

void routeCalPontos() {
  static char jsonBuffer[TAM_BUFFER_PONTOS];
  int n;
  float fator, offset, margem;
  PontoCalWeb pontos_copia[WEB_MAX_PONTOS_CAL];

  {
    COM_ESTADO_TRAVADO
    n = g_estado.num_pontos_cal;
    fator = g_estado.cal_fator;
    margem = g_estado.margem_seguranca;
    offset = g_estado.cal_offset + margem;
    for (int i = 0; i < n; i++) pontos_copia[i] = g_estado.pontos_cal[i];
  }

  size_t pos = 0;
  pos += snprintf(jsonBuffer + pos, TAM_BUFFER_PONTOS - pos, "{\"pontos\":[");

  for (int i = 0; i < n && pos < TAM_BUFFER_PONTOS - 120; i++) {
    float peso_previsto = (fator != 0) ? (pontos_copia[i].leitura_raw - offset) / fator : 0;
    float residuo_g = peso_previsto - pontos_copia[i].peso_real_g;

    pos += snprintf(jsonBuffer + pos, TAM_BUFFER_PONTOS - pos,
                     "%s{\"peso_real_g\":%.2f,\"leitura_raw\":%.0f,\"residuo_g\":%.2f}",
                     (i > 0) ? "," : "",
                     pontos_copia[i].peso_real_g,
                     pontos_copia[i].leitura_raw,
                     residuo_g);
  }

  snprintf(jsonBuffer + pos, TAM_BUFFER_PONTOS - pos,
           "],\"fator\":%.6f,\"intercept\":%.4f,\"margem\":%.1f}",
           fator, offset, margem);

  sendHeadersHTTP();
  server.send(200, "application/json", jsonBuffer);
}

void routeCalIniciar() {
  if (!server.hasArg("peso")) {
    sendHeadersHTTP();
    server.send(400, "application/json", "{\"ok\":false,\"erro\":\"peso ausente\"}");
    return;
  }
  float peso = server.arg("peso").toFloat();
  if (peso <= 0) {
    sendHeadersHTTP();
    server.send(400, "application/json", "{\"ok\":false,\"erro\":\"peso invalido\"}");
    return;
  }

  g_peso_a_capturar = peso;
  g_aguardando_captura = true;

  sendHeadersHTTP();
  server.send(200, "application/json", "{\"ok\":true}");
}

void routeCalConfirmar() {
  if (!g_aguardando_captura) {
    sendHeadersHTTP();
    server.send(400, "application/json", "{\"ok\":false,\"erro\":\"nenhuma captura iniciada\"}");
    return;
  }

  if (!enfileirarPedido(PEDIDO_CAPTURA_CAL, g_peso_a_capturar)) {
    sendHeadersHTTP();
    server.send(409, "application/json", "{\"ok\":false,\"erro\":\"outro pedido em andamento\"}");
    return;
  }

  // Espera o core 1 terminar (bloqueia esta requisição HTTP, mas não trava
  // o core 1 nem a leitura contínua -- só esta conexão TCP específica fica
  // parada até o pedido concluir, ~5s, igual ao tempo de captura já
  // documentado na UI).
  uint32_t inicio = millis();
  uint32_t seq_pedido;
  { COM_ESTADO_TRAVADO seq_pedido = g_estado.pedido_seq; }

  bool concluido = false;
  while (millis() - inicio < 10000) {
    { COM_ESTADO_TRAVADO concluido = (g_estado.pedido_seq_atendido == seq_pedido); }
    if (concluido) break;
    delay(20);
  }

  // Se o loop encerrou por timeout (não por conclusão), não podemos ler
  // ultimo_pedido_ok pois ele reflete o pedido ANTERIOR, não este.
  if (!concluido) {
    g_aguardando_captura = false;
    sendHeadersHTTP();
    server.send(504, "application/json", "{\"ok\":false,\"erro\":\"timeout: core 1 nao respondeu\"}");
    return;
  }

  bool ok;
  char erro[64];
  { COM_ESTADO_TRAVADO
    ok = g_estado.ultimo_pedido_ok;
    strncpy(erro, g_estado.ultimo_pedido_erro, sizeof(erro) - 1);
    erro[sizeof(erro) - 1] = '\0';
  }

  g_aguardando_captura = false;

  sendHeadersHTTP();
  if (ok) {
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":false,\"erro\":\"%s\"}", erro);
    server.send(500, "application/json", resp);
  }
}

void routeCalCancelar() {
  g_aguardando_captura = false;
  sendHeadersHTTP();
  server.send(200, "application/json", "{\"ok\":true}");
}

void routeCalRemover() {
  if (!server.hasArg("idx")) {
    sendHeadersHTTP();
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  int idx = server.arg("idx").toInt();

  enfileirarPedido(PEDIDO_REMOVER_PONTO, 0, idx);
  // Não bloqueia esperando -- é rápido e a UI já re-busca /api/cal/pontos
  // em seguida; se a remoção ainda não tiver sido processada no primeiro
  // fetch, o próximo polling (500ms) já mostra o resultado atualizado.

  sendHeadersHTTP();
  server.send(200, "application/json", "{\"ok\":true}");
}

void routeCalResetar() {
  enfileirarPedido(PEDIDO_RESETAR_CAL);
  sendHeadersHTTP();
  server.send(200, "application/json", "{\"ok\":true}");
}

void routeTarar() {
  if (!enfileirarPedido(PEDIDO_TARA)) {
    sendHeadersHTTP();
    server.send(409, "application/json", "{\"ok\":false,\"erro\":\"tara ja em andamento\"}");
    return;
  }
  sendHeadersHTTP();
  server.send(200, "application/json", "{\"ok\":true}");
}

// TASK DO CORE 0 -- Wi-Fi + servidor HTTP + máquina de pesagem automática.
void taskServidorWeb(void *pv) {
  Serial.printf("[TASK] task_servidor_web iniciada no core %d\n", xPortGetCoreID());

  // Conecta via credenciais NVS ou abre portal captivo de configuração
  iniciarConexaoWiFi();

  server.on("/", routeHome);
  server.on("/api/data", routeData);
  server.on("/api/cal/pontos", HTTP_GET, routeCalPontos);
  server.on("/api/cal/iniciar", HTTP_POST, routeCalIniciar);
  server.on("/api/cal/confirmar", HTTP_POST, routeCalConfirmar);
  server.on("/api/cal/cancelar", HTTP_POST, routeCalCancelar);
  server.on("/api/cal/remover", HTTP_POST, routeCalRemover);
  server.on("/api/cal/resetar", HTTP_POST, routeCalResetar);
  server.on("/api/tarar", HTTP_POST, routeTarar);
  server.on("/api/wifi/resetar", HTTP_POST, routeWifiResetar);
  server.begin();

  Serial.println("[SETUP] Servidor HTTP iniciado no core 0.");

  for (;;) {
    server.handleClient();

    processarMaquinaPesagemAuto();
    dispararCapturaAutoSeNecessario();

    // Sincroniza wifi_conectado nos dois sentidos: tanto ao desconectar
    // quanto ao reconectar automaticamente pelo driver Wi-Fi do ESP32.
    {
      COM_ESTADO_TRAVADO
      g_estado.wifi_conectado = (WiFi.status() == WL_CONNECTED);
    }

    // vTaskDelay cede tempo ao Idle Task (prioridade 0), evitando starvation
    // e reinicializações pelo Task Watchdog Timer (TWDT).
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void iniciarInterfaceWeb() {
  xTaskCreatePinnedToCore(
      taskServidorWeb,
      "task_servidor_web",
      8192,
      NULL,
      1, // prioridade padrão -- não compete com a leitura do sensor no outro core
      NULL,
      0 // CORE 0
  );
}
