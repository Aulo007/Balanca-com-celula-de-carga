#include <Arduino.h>
#include <Preferences.h>
#include "esp_bt.h"
#include "HX711.h"
#include "web_interface.h"

// DIAGRAMA DE LIGAÇÃO (WIRING)
//
// 1. Célula de Carga -> HX711 (lado analógico)
//   Fio Vermelho -> E+  (Excitação Positiva)
//   Fio Preto    -> E-  (Excitação Negativa)
//   Fio Verde    -> A-  (Sinal Negativo)
//   Fio Branco   -> A+  (Sinal Positivo)
//
// 2. HX711 -> ESP32 (lado digital)
//   VCC -> 3V3 do ESP32 (NUNCA 5V/VIN)
//   GND -> qualquer GND do ESP32
//   DT  -> GPIO 32
//   SCK -> GPIO 33

// ARQUITETURA DUAL-CORE
// Esta versão volta a separar os dois trabalhos em cores diferentes:
//
//   CORE 1 (este arquivo, task_sensores): leitura do HX711, filtros,
//   tara, calibração, gravação em NVS. É o trabalho sensível a timing --
//   o HX711 bit-bang não pode ter jitter grande entre chamadas, e enfiar
//   isso no mesmo loop que atende HTTP (como a v2.0 single-core fazia)
//   significa que uma requisição lenta atrasa a leitura do sensor.
//
//   CORE 0 (web_interface.cpp, task_servidor_web): Wi-Fi + WebServer.
//   O framework Arduino já roda o rádio Wi-Fi em tasks internas presas
//   ao core 0 assim que WiFi.begin() é chamado -- colocar o servidor
//   HTTP no mesmo core evita que ele dispute CPU com a leitura da célula
//   de carga, que fica isolada no core 1.
//
// Os dois lados só se comunicam através de EstadoCompartilhado (ver
// web_interface.h), protegido por g_estado_mutex. Nenhuma variável de
// sensor/calibração é lida ou escrita fora dali quando envolve os dois
// cores -- dentro deste arquivo, os buffers de filtro (buffer_leituras_raw,
// buffer_convergencia, tabela de pontos) continuam sendo globais comuns,
// porque só a task deste core toca neles.
//
// O log Serial mantém exatamente a mesma frequência e conteúdo de antes:
// tudo que já era Serial.printf no meio da leitura/tara/calibração
// continua rodando na mesma task e no mesmo ritmo, só que agora presa
// ao core 1 em vez de rodar dentro do loop() padrão do Arduino.

const int PINO_DT = 32;
const int PINO_SCK = 33;

HX711 balanca;
Preferences prefs;

SemaphoreHandle_t g_estado_mutex;
EstadoCompartilhado g_estado;

// CALIBRAÇÃO DINÂMICA (N PONTOS) -- cópia de trabalho local a este core.
// Espelhada em g_estado.pontos_cal a cada mudança, para o core 0 ler.
const int MAX_PONTOS_CAL = WEB_MAX_PONTOS_CAL;

struct PontoCalibracao {
  float peso_real_g;
  float leitura_raw;
};

PontoCalibracao pontos_cal[MAX_PONTOS_CAL];
int num_pontos_cal = 0;

const float FATOR_NAO_CALIBRADO = 1.0;
float cal_fator = FATOR_NAO_CALIBRADO;
float cal_offset = 0.0;
bool sistema_calibrado = false;
float margem_seguranca_zero = 0.0;

// FILTRO DE MÉDIA MÓVEL -- buffer circular de 30 amostras raw.
const int NUM_AMOSTRAS_MEDIA = 30;
long buffer_leituras_raw[NUM_AMOSTRAS_MEDIA];
int indice_buffer_media = 0;
int amostras_no_buffer = 0;
int64_t soma_buffer_media = 0;

// SISTEMA DE CONVERGÊNCIA / ESTABILIDADE
const int NUM_AMOSTRAS_CONVERGENCIA = 25;
long buffer_convergencia[NUM_AMOSTRAS_CONVERGENCIA];
int indice_convergencia = 0;
bool buffer_convergencia_cheio = false;
bool sinal_estavel = false;
long variacao_atual = 0;
long media_convergencia = 0;
const long TOLERANCIA_CONVERGENCIA = 1500;

// leitura "de trabalho" local -- espelhada em g_estado.peso_raw_filtrado
float peso_raw_filtrado_local = 0.0;
bool hx711_ok_local = false;

// DETECÇÃO DE PICOS / SINAL TRAVADO
long ultimo_raw_data = 0;
bool tem_raw_anterior = false;
int repeticoes_consecutivas = 0;
const int LIMITE_REPETICOES_SUSPEITAS = 200;

const long LIMIAR_PICO_RAW = 5000;
const int MAX_PICOS_CONSECUTIVOS_IGNORADOS = 5;
const float LIMIAR_ZONA_MORTA = 50.0;
int picos_consecutivos_ignorados = 0;
bool tem_valor_filtrado_inicial = false;



// PERSISTÊNCIA (NVS) -- idêntica à v2.0, sem mudanças de comportamento.
void salvarCalibracao() {
  prefs.begin("balanca", false);
  prefs.putFloat("fator", cal_fator);
  prefs.putFloat("offset", cal_offset);
  prefs.putBool("calibrado", sistema_calibrado);
  prefs.putInt("num_pts", num_pontos_cal);
  prefs.putFloat("margem_z", margem_seguranca_zero);
  for (int i = 0; i < num_pontos_cal; i++) {
    char kp[8], kr[8];
    snprintf(kp, sizeof(kp), "pt%d_p", i);
    snprintf(kr, sizeof(kr), "pt%d_r", i);
    prefs.putFloat(kp, pontos_cal[i].peso_real_g);
    prefs.putFloat(kr, pontos_cal[i].leitura_raw);
  }
  prefs.end();
  Serial.println("[CAL] Calibração persistida na memória NVS.");
}

void carregarCalibracao() {
  prefs.begin("balanca", false);

  cal_fator = prefs.getFloat("fator", FATOR_NAO_CALIBRADO);
  cal_offset = prefs.getFloat("offset", 0.0);
  sistema_calibrado = prefs.getBool("calibrado", false);
  num_pontos_cal = prefs.getInt("num_pts", 0);
  margem_seguranca_zero = prefs.getFloat("margem_z", 0.0);

  if (num_pontos_cal > MAX_PONTOS_CAL) num_pontos_cal = MAX_PONTOS_CAL;

  for (int i = 0; i < num_pontos_cal; i++) {
    char kp[8], kr[8];
    snprintf(kp, sizeof(kp), "pt%d_p", i);
    snprintf(kr, sizeof(kr), "pt%d_r", i);
    pontos_cal[i].peso_real_g = prefs.getFloat(kp, 0.0);
    pontos_cal[i].leitura_raw = prefs.getFloat(kr, 0.0);
  }
  prefs.end();
  Serial.printf("[CAL] Carregado: fator=%.4f offset=%.4f margem=%.1f pontos=%d calibrado=%d\n",
                cal_fator, cal_offset, margem_seguranca_zero, num_pontos_cal, sistema_calibrado);
}

// Publica o estado local de calibração/pontos no EstadoCompartilhado.
// Chamado sempre que algo muda (tara, calibrar, remover ponto, resetar) --
// não a cada volta do loop, só nas transições, para não segurar o mutex
// mais que o necessário.
void publicarCalibracaoNoEstado() {
  COM_ESTADO_TRAVADO
  g_estado.cal_fator = cal_fator;
  g_estado.cal_offset = cal_offset;
  g_estado.margem_seguranca = margem_seguranca_zero;
  g_estado.calibrado = sistema_calibrado;
  g_estado.num_pontos_cal = num_pontos_cal;
  for (int i = 0; i < num_pontos_cal; i++) {
    g_estado.pontos_cal[i].peso_real_g = pontos_cal[i].peso_real_g;
    g_estado.pontos_cal[i].leitura_raw = pontos_cal[i].leitura_raw;
  }
}

// REGRESSÃO LINEAR (Mínimos Quadrados) -- idêntica à v2.0.
void recalcularCalibracao() {
  if (num_pontos_cal == 0) {
    sistema_calibrado = false;
    return;
  }

  if (num_pontos_cal == 1) {
    float offset_efetivo = cal_offset + margem_seguranca_zero;
    if (pontos_cal[0].peso_real_g != 0) {
      cal_fator = (pontos_cal[0].leitura_raw - offset_efetivo) / pontos_cal[0].peso_real_g;
    }
  } else {
    float offset_efetivo = cal_offset + margem_seguranca_zero;
    double sum_x = 0, sum_y = (double) offset_efetivo, sum_xy = 0, sum_xx = 0;
    int n = num_pontos_cal + 1;

    for (int i = 0; i < num_pontos_cal; i++) {
      double x = pontos_cal[i].peso_real_g;
      double y = pontos_cal[i].leitura_raw;
      sum_x += x;
      sum_y += y;
      sum_xy += x * y;
      sum_xx += x * x;
    }

    double denom = (n * sum_xx) - (sum_x * sum_x);
    cal_fator = 0; // garante que o fallback abaixo é sempre ativado se denom ≈ 0
    if (fabs(denom) > 1e-9) {
      cal_fator = ((n * sum_xy) - (sum_x * sum_y)) / denom;
      cal_offset = (sum_y - cal_fator * sum_x) / n;
    }
  }

  if (cal_fator == 0) {
    Serial.println("[CAL] AVISO: regressao resultou em fator=0 (pontos degenerados?). Usando fallback neutro.");
    cal_fator = FATOR_NAO_CALIBRADO;
  }

  sistema_calibrado = true;
  salvarCalibracao();
  publicarCalibracaoNoEstado();
  Serial.printf("[CAL] Recalculado -> Fator(Raw/g)=%.4f Offset=%.4f Margem=%.1f (n=%d pontos manuais)\n",
                cal_fator, cal_offset, margem_seguranca_zero, num_pontos_cal);
}

// VERIFICAÇÃO DE PRESENÇA FÍSICA DO HX711 -- idêntica à v2.0.
const int AMOSTRAS_VERIFICACAO_PRESENCA = 10;

bool verificarPresencaFisicaHX711() {
  long primeira_leitura = 0;
  bool obteve_primeira = false;
  bool encontrou_variacao = false;

  for (int i = 0; i < AMOSTRAS_VERIFICACAO_PRESENCA; i++) {
    if (balanca.is_ready()) {
      long leitura = balanca.read();
      if (!obteve_primeira) {
        primeira_leitura = leitura;
        obteve_primeira = true;
      } else if (leitura != primeira_leitura) {
        encontrou_variacao = true;
        break;
      }
    }
    delay(15);
  }

  if (!obteve_primeira) return false;

  if (!encontrou_variacao) {
    Serial.println("[SENSOR] AVISO: leituras identicas em todas as amostras -- sinal suspeito de pino flutuante (DT sem HX711 real conectado).");
  }

  return encontrou_variacao;
}

// TARA -- amostras reduzidas para zerar mais rápido (era 50), margem de
// segurança continua 2x o pico observado.
const int AMOSTRAS_TARA = 15;

struct ResultadoTara {
  long offset;
  long min_val;
  long max_val;
  float margem;
  int amostras_lidas;
  bool sucesso;
};

ResultadoTara executarTara(int num_amostras) {
  ResultadoTara r;
  r.sucesso = false;
  r.offset = 0;
  r.min_val = 0;
  r.max_val = 0;
  r.margem = 0;
  r.amostras_lidas = 0;

  int64_t soma = 0;
  bool tem_primeiro = false;
  uint32_t inicio = millis();
  const uint32_t TIMEOUT_MS = 10000;

  if (balanca.is_ready()) balanca.read();
  delay(10);
  if (balanca.is_ready()) balanca.read();
  delay(10);

  Serial.printf("[TARA] Iniciando tara com %d amostras...\n", num_amostras);

  while (r.amostras_lidas < num_amostras && (millis() - inicio) < TIMEOUT_MS) {
    if (balanca.is_ready()) {
      long val = balanca.read();
      soma += val;

      if (!tem_primeiro) {
        r.min_val = val;
        r.max_val = val;
        tem_primeiro = true;
      } else {
        if (val < r.min_val) r.min_val = val;
        if (val > r.max_val) r.max_val = val;
      }

      r.amostras_lidas++;

      if (r.amostras_lidas % 10 == 0) {
        Serial.printf("[TARA] Progresso: %d/%d amostras (variacao ate agora: %ld)\n",
                      r.amostras_lidas, num_amostras, r.max_val - r.min_val);
      }
    }
    delay(5); // delay() já chama vTaskDelay internamente, yield() adicional é desnecessário
  }

  if (r.amostras_lidas == 0) {
    Serial.println("[TARA] ERRO: nenhuma amostra lida. HX711 nao responde.");
    return r;
  }

  float media = (float) soma / (float) r.amostras_lidas;
  r.offset = (long) media;

  float pico_acima_media = (float)(r.max_val) - media;
  r.margem = pico_acima_media * 2.0;
  if (r.margem < 50.0) r.margem = 50.0;

  r.sucesso = true;

  long variacao_total = r.max_val - r.min_val;
  Serial.printf("[TARA] Resultado: %d amostras, media=%.1f, min=%ld, max=%ld\n",
                r.amostras_lidas, media, r.min_val, r.max_val);
  Serial.printf("[TARA] Variacao total: %ld raw-counts\n", variacao_total);
  Serial.printf("[TARA] Pico acima da media: %.1f, margem de seguranca aplicada: %.1f\n",
                pico_acima_media, r.margem);
  Serial.printf("[TARA] Offset final (media): %ld + margem %.1f = offset efetivo %.1f\n",
                r.offset, r.margem, (float) r.offset + r.margem);

  return r;
}

// CAPTURA ROBUSTA (100 amostras -- era 50, aumentado para mais robustez a
// ruído). Usada tanto por calibração manual (PEDIDO_CAPTURA_CAL) quanto pela
// pesagem automática (PEDIDO_CAPTURA_AUTO) -- é o mesmo pipeline preciso
// nos dois casos.
const int AMOSTRAS_CAPTURA_CALIBRACAO = 100;

struct ResultadoCaptura {
  float media;
  long min_val;
  long max_val;
  long variacao;
  bool sucesso;
};

ResultadoCaptura executarCapturaRobusta(int num_amostras) {
  ResultadoCaptura r;
  r.sucesso = false;
  r.media = 0;
  r.min_val = 0;
  r.max_val = 0;
  r.variacao = 0;

  int64_t soma = 0;
  int amostras_lidas = 0;
  bool tem_primeiro = false;
  uint32_t inicio = millis();
  const uint32_t TIMEOUT_MS = 10000;

  if (balanca.is_ready()) balanca.read();
  delay(5);

  while (amostras_lidas < num_amostras && (millis() - inicio) < TIMEOUT_MS) {
    if (balanca.is_ready()) {
      long val = balanca.read();
      soma += val;

      if (!tem_primeiro) {
        r.min_val = val;
        r.max_val = val;
        tem_primeiro = true;
      } else {
        if (val < r.min_val) r.min_val = val;
        if (val > r.max_val) r.max_val = val;
      }

      amostras_lidas++;

      if (amostras_lidas % 10 == 0) {
        Serial.printf("[CAPTURA] %d/%d amostras lidas...\n", amostras_lidas, num_amostras);
      }
    }
    delay(5); // delay() já chama vTaskDelay internamente, yield() adicional é desnecessário
  }

  if (amostras_lidas < num_amostras) {
    Serial.printf("[CAPTURA] AVISO: so conseguiu %d de %d amostras (timeout)\n", amostras_lidas, num_amostras);
  }

  if (amostras_lidas == 0) {
    Serial.println("[CAPTURA] ERRO: nenhuma amostra lida. HX711 nao responde.");
    return r;
  }

  r.media = (float) soma / (float) amostras_lidas;
  r.variacao = r.max_val - r.min_val;
  r.sucesso = true;

  Serial.printf("[CAPTURA] Concluida: %d amostras, media=%.1f, min=%ld, max=%ld, variacao=%ld\n",
                amostras_lidas, r.media, r.min_val, r.max_val, r.variacao);

  return r;
}

// RESET COMPLETO DO ESTADO DOS FILTROS -- idêntico à v2.0.
void resetarEstadoFiltros(float novo_raw_referencia) {
  peso_raw_filtrado_local = novo_raw_referencia;
  soma_buffer_media = 0;
  amostras_no_buffer = 0;
  indice_buffer_media = 0;
  for (int i = 0; i < NUM_AMOSTRAS_MEDIA; i++) {
    buffer_leituras_raw[i] = 0;
  }

  buffer_convergencia_cheio = false;
  indice_convergencia = 0;
  sinal_estavel = false;
  variacao_atual = 0;
  for (int i = 0; i < NUM_AMOSTRAS_CONVERGENCIA; i++) {
    buffer_convergencia[i] = 0;
  }

  tem_valor_filtrado_inicial = false;
  picos_consecutivos_ignorados = 0;

  tem_raw_anterior = false;
  repeticoes_consecutivas = 0;
  ultimo_raw_data = 0;
}

// Calcula peso em gramas a partir do raw filtrado local, usando a
// calibração local -- mesma fórmula de routeData() da v2.0, mas rodando
// aqui no core 1 para publicar já pronto em g_estado.
float calcularPesoGramas(float raw_filtrado) {
  float offset_efetivo = cal_offset + margem_seguranca_zero;
  float peso = (cal_fator != 0) ? (raw_filtrado - offset_efetivo) / cal_fator : 0;
  // Permite valores negativos (drift de tara, ruído simétrico em torno de zero)
  // para que a máquina de pesagem automática não confunda oscilação com peso real.
  // A interface web exibe max(0, peso) para o usuário final.
  return peso;
}

// LEITURA CONTÍNUA DO SENSOR -- idêntica à v2.0 na lógica de filtro, só
// que agora publica o resultado em g_estado a cada volta (barato: só copia
// alguns floats/bools sob mutex, não faz I/O).
void atualizarSensores() {
  bool leitura_ok = balanca.is_ready();
  long raw_data = 0;

  if (leitura_ok) {
    raw_data = balanca.read();

    if (tem_raw_anterior && raw_data == ultimo_raw_data) {
      repeticoes_consecutivas++;
    } else {
      repeticoes_consecutivas = 0;
    }
    ultimo_raw_data = raw_data;
    tem_raw_anterior = true;

    if (repeticoes_consecutivas == LIMITE_REPETICOES_SUSPEITAS) {
      Serial.println("[SENSOR] AVISO: sinal raw travado no mesmo valor por ~2s -- possivel desconexao do HX711.");
    }
  }

  bool sinal_suspeito = (repeticoes_consecutivas >= LIMITE_REPETICOES_SUSPEITAS);
  bool eh_pico_isolado = false;

  if (leitura_ok && !sinal_suspeito) {
    if (tem_valor_filtrado_inicial) {
      float desvio = fabs((float) raw_data - peso_raw_filtrado_local);
      if (desvio > LIMIAR_PICO_RAW && picos_consecutivos_ignorados < MAX_PICOS_CONSECUTIVOS_IGNORADOS) {
        eh_pico_isolado = true;
        picos_consecutivos_ignorados++;
      } else {
        picos_consecutivos_ignorados = 0;
      }
    } else {
      tem_valor_filtrado_inicial = true;
    }
  }

  hx711_ok_local = leitura_ok && !sinal_suspeito;

  if (leitura_ok && !sinal_suspeito && !eh_pico_isolado) {
    if (amostras_no_buffer == NUM_AMOSTRAS_MEDIA) {
      soma_buffer_media -= (int64_t) buffer_leituras_raw[indice_buffer_media];
    } else {
      amostras_no_buffer++;
    }

    buffer_leituras_raw[indice_buffer_media] = raw_data;
    soma_buffer_media += (int64_t) raw_data;

    indice_buffer_media++;
    if (indice_buffer_media >= NUM_AMOSTRAS_MEDIA) {
      indice_buffer_media = 0;
    }

    float media_raw = (float)((double) soma_buffer_media / (double) amostras_no_buffer);

    float desvio_media = fabs(media_raw - peso_raw_filtrado_local);
    if (desvio_media > LIMIAR_ZONA_MORTA || amostras_no_buffer == 1) {
      peso_raw_filtrado_local = media_raw;
    }

    buffer_convergencia[indice_convergencia] = raw_data;
    indice_convergencia++;
    if (indice_convergencia >= NUM_AMOSTRAS_CONVERGENCIA) {
      indice_convergencia = 0;
      buffer_convergencia_cheio = true;
    }

    if (buffer_convergencia_cheio) {
      long conv_max = buffer_convergencia[0];
      long conv_min = buffer_convergencia[0];
      int64_t conv_soma = 0;

      for (int i = 0; i < NUM_AMOSTRAS_CONVERGENCIA; i++) {
        if (buffer_convergencia[i] > conv_max) conv_max = buffer_convergencia[i];
        if (buffer_convergencia[i] < conv_min) conv_min = buffer_convergencia[i];
        conv_soma += buffer_convergencia[i];
      }

      variacao_atual = conv_max - conv_min;
      media_convergencia = (long)(conv_soma / NUM_AMOSTRAS_CONVERGENCIA);
      bool era_estavel = sinal_estavel;
      sinal_estavel = (variacao_atual <= TOLERANCIA_CONVERGENCIA);

      if (sinal_estavel && !era_estavel) {
        Serial.printf("[CONV] -> ESTAVEL. Variacao=%ld (<= %ld). Media convergida: %ld\n",
                      variacao_atual, TOLERANCIA_CONVERGENCIA, media_convergencia);
      } else if (!sinal_estavel && era_estavel) {
        Serial.printf("[CONV] -> OSCILANDO. Variacao=%ld (> %ld)\n",
                      variacao_atual, TOLERANCIA_CONVERGENCIA);
      }
    }
  }

  // Publica o snapshot desta volta em g_estado -- rápido, só floats/bools.
  float peso_g = calcularPesoGramas(peso_raw_filtrado_local);
  {
    COM_ESTADO_TRAVADO
    g_estado.peso_raw_filtrado = peso_raw_filtrado_local;
    g_estado.peso_gramas = peso_g;
    g_estado.hx711_ok = hx711_ok_local;
    g_estado.sinal_estavel = sinal_estavel;
    g_estado.variacao_atual = variacao_atual;
  }
}

// PROCESSAMENTO DE PEDIDOS VINDOS DO CORE 0 (web)
// Chamado a cada volta da task deste core. Só age quando pedido_seq mudou
// desde o último atendido -- senão é um no-op de custo desprezível (um
// take/give de mutex e uma comparação de inteiro).
void processarPedidosPendentes() {
  TipoPedido tipo;
  uint32_t seq;
  float peso_g_pedido;
  int idx_pedido;

  {
    COM_ESTADO_TRAVADO
    seq = g_estado.pedido_seq;
    if (seq == g_estado.pedido_seq_atendido) {
      return; // nada novo
    }
    tipo = g_estado.pedido_tipo;
    peso_g_pedido = g_estado.pedido_peso_g;
    idx_pedido = g_estado.pedido_idx;
    g_estado.pedido_em_andamento = true;
  }

  bool ok = true;
  char erro[64] = "";

  switch (tipo) {
    case PEDIDO_TARA: {
      Serial.println("[TARA] ======================================");
      Serial.printf("[TARA] Executando tara (%d amostras) solicitada via HTTP...\n", AMOSTRAS_TARA);

      ResultadoTara resultado = executarTara(AMOSTRAS_TARA);
      if (!resultado.sucesso) {
        Serial.println("[TARA] Falha na tara: HX711 nao respondeu.");
        ok = false;
        strncpy(erro, "falha ao ler HX711", sizeof(erro) - 1);
        break;
      }

      float delta_offset = (float)resultado.offset - cal_offset;
      
      cal_offset = (float) resultado.offset;
      margem_seguranca_zero = resultado.margem;

      // Desloca todos os pontos de calibração para manter a regressão linear consistente
      for (int i = 0; i < num_pontos_cal; i++) {
        pontos_cal[i].leitura_raw += delta_offset;
      }

      float novo_ref = (float) resultado.offset + resultado.margem;
      resetarEstadoFiltros(novo_ref);

      recalcularCalibracao(); // Recalcula a regressão e salva (chama salvarCalibracao e publicarCalibracaoNoEstado)

      Serial.printf("[TARA] Tara concluida com sucesso!\n");
      Serial.printf("[TARA]   cal_offset = %.0f\n", cal_offset);
      Serial.printf("[TARA]   margem_seguranca = %.1f\n", margem_seguranca_zero);
      Serial.printf("[TARA]   offset_efetivo = %.1f\n", cal_offset + margem_seguranca_zero);
      Serial.printf("[TARA]   ruido observado (max-min) = %ld raw-counts\n",
                    resultado.max_val - resultado.min_val);
      Serial.println("[TARA] ======================================");
      break;
    }

    case PEDIDO_CAPTURA_CAL: {
      Serial.printf("[CAL] Iniciando captura robusta de %d amostras para %.1fg...\n",
                    AMOSTRAS_CAPTURA_CALIBRACAO, peso_g_pedido);
      ResultadoCaptura captura = executarCapturaRobusta(AMOSTRAS_CAPTURA_CALIBRACAO);

      if (!captura.sucesso) {
        ok = false;
        strncpy(erro, "falha ao ler HX711", sizeof(erro) - 1);
        break;
      }

      float raw_capturado = captura.media;

      const float TOLERANCIA_PESO_DUPLICADO = 0.05;
      int idx_existente = -1;
      for (int i = 0; i < num_pontos_cal; i++) {
        if (fabs(pontos_cal[i].peso_real_g - peso_g_pedido) < TOLERANCIA_PESO_DUPLICADO) {
          idx_existente = i;
          break;
        }
      }

      bool substituiu = (idx_existente >= 0);

      if (substituiu) {
        Serial.printf("[CAL] Ponto de %.1fg ja existia -- substituindo leitura antiga (raw %.0f -> raw %.0f)\n",
                      peso_g_pedido, pontos_cal[idx_existente].leitura_raw, raw_capturado);
        pontos_cal[idx_existente].leitura_raw = raw_capturado;
      } else {
        if (num_pontos_cal >= MAX_PONTOS_CAL) {
          ok = false;
          strncpy(erro, "limite de pontos atingido", sizeof(erro) - 1);
          break;
        }
        pontos_cal[num_pontos_cal].peso_real_g = peso_g_pedido;
        pontos_cal[num_pontos_cal].leitura_raw = raw_capturado;
        num_pontos_cal++;
      }

      recalcularCalibracao(); // já publica em g_estado e salva NVS

      Serial.printf("[CAL] Ponto: %.1f g <-> raw %.0f (variacao=%ld) (%s)\n",
                    peso_g_pedido, raw_capturado, captura.variacao,
                    substituiu ? "substituido" : "novo");
      break;
    }

    case PEDIDO_CAPTURA_AUTO: {
      // Mesmo pipeline da calibração manual, só que sem peso conhecido --
      // o resultado vai direto para g_estado.peso_auto_resultado_g, não
      // para a tabela de calibração.
      Serial.println("[PESAGEM] Objeto detectado -- iniciando captura robusta automatica...");
      ResultadoCaptura captura = executarCapturaRobusta(AMOSTRAS_CAPTURA_CALIBRACAO);

      if (!captura.sucesso) {
        ok = false;
        strncpy(erro, "falha ao ler HX711", sizeof(erro) - 1);
        Serial.println("[PESAGEM] Captura automatica falhou: HX711 nao respondeu.");
        break;
      }

      float peso_g = calcularPesoGramas(captura.media);

      Serial.printf("[PESAGEM] Captura automatica concluida: raw=%.1f peso=%.2fg (variacao=%ld)\n",
                    captura.media, peso_g, captura.variacao);

      {
        COM_ESTADO_TRAVADO
        g_estado.peso_auto_resultado_g = peso_g;
        g_estado.estado_pesagem = PESAGEM_RESULTADO;
      }
      break;
    }

    case PEDIDO_REMOVER_PONTO: {
      if (idx_pedido >= 0 && idx_pedido < num_pontos_cal) {
        for (int i = idx_pedido; i < num_pontos_cal - 1; i++) {
          pontos_cal[i] = pontos_cal[i + 1];
        }
        num_pontos_cal--;
        recalcularCalibracao();
      }
      break;
    }

    case PEDIDO_RESETAR_CAL: {
      num_pontos_cal = 0;
      cal_fator = FATOR_NAO_CALIBRADO;
      cal_offset = 0.0;
      margem_seguranca_zero = 0.0;
      sistema_calibrado = false;
      salvarCalibracao();
      publicarCalibracaoNoEstado();
      break;
    }

    default:
      break;
  }

  COM_ESTADO_TRAVADO
  g_estado.ultimo_pedido_ok = ok;
  strncpy(g_estado.ultimo_pedido_erro, erro, sizeof(g_estado.ultimo_pedido_erro) - 1);
  g_estado.ultimo_pedido_erro[sizeof(g_estado.ultimo_pedido_erro) - 1] = '\0';
  g_estado.pedido_seq_atendido = seq;
  g_estado.pedido_em_andamento = false;
}

// TASK DO CORE 1 -- leitura contínua + processamento de pedidos.
// Mantém exatamente o mesmo ritmo de antes: sem delay() fixo, só yield(),
// então a taxa de leitura do HX711 continua limitada só pela taxa nativa
// do chip (~10Hz via is_ready()), igual ao loop() da v2.0 single-core.
void taskSensores(void *pv) {
  Serial.printf("[TASK] task_sensores iniciada no core %d\n", xPortGetCoreID());

  for (;;) {
    atualizarSensores();
    processarPedidosPendentes();
    // vTaskDelay cede tempo ao Idle Task (prioridade 0), evitando starvation
    // e reinicializações pelo Task Watchdog Timer (TWDT). O HX711 a ~10 Hz
    // não sofre impacto perceptível com 10 ms de pausa entre leituras.
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// SETUP PRINCIPAL
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("==========================================");
  Serial.println("   BALANCA DIGITAL - ESP32 + HX711");
  Serial.println("   Firmware v3.0 - Dual Core (sensor/web separados)");
  Serial.println("==========================================");

  btStop();
  esp_bt_controller_disable();

  g_estado_mutex = xSemaphoreCreateMutex();
  // Fail Fast: se o heap estiver esgotado e o mutex não for criado, qualquer
  // chamada posterior a COM_ESTADO_TRAVADO travará indefinidamente. Uma mensagem
  // clara no Serial é muito mais fácil de depurar do que um deadlock misterioso.
  if (g_estado_mutex == NULL) {
    Serial.println("[FATAL] Falha ao criar mutex -- heap insuficiente. Sistema parado.");
    while (true) { delay(1000); }
  }

  for (int i = 0; i < NUM_AMOSTRAS_MEDIA; i++) buffer_leituras_raw[i] = 0;
  indice_buffer_media = 0;
  amostras_no_buffer = 0;
  soma_buffer_media = 0;

  for (int i = 0; i < NUM_AMOSTRAS_CONVERGENCIA; i++) buffer_convergencia[i] = 0;
  indice_convergencia = 0;
  buffer_convergencia_cheio = false;
  sinal_estavel = false;

  carregarCalibracao();
  publicarCalibracaoNoEstado();

  // Ganho 64x -- mesma escolha da v2.0 (sensibilidade menor, mais estável).
  balanca.begin(PINO_DT, PINO_SCK, 64);

  bool hx711_pronto = false;
  for (int tentativas = 0; tentativas < 50; tentativas++) {
    if (balanca.is_ready()) {
      hx711_pronto = true;
      break;
    }
    delay(100);
  }

  bool hx711_presente = false;
  if (hx711_pronto) {
    Serial.println("[SETUP] DT respondeu, verificando se o sinal e real...");
    hx711_presente = verificarPresencaFisicaHX711();
  }

  if (hx711_presente) {
    delay(500);
    Serial.println("[SETUP] Verificação rápida do sensor (10 amostras)...");
    ResultadoTara tara_inicial = executarTara(10);

    if (tara_inicial.sucesso) {
      Serial.printf("[SETUP] Sensor OK! offset bruto=%ld, variacao=%ld\n",
                    tara_inicial.offset, tara_inicial.max_val - tara_inicial.min_val);
      Serial.println("[SETUP] Calibração salva na NVS permanece intacta.");
    } else {
      Serial.println("[SETUP] AVISO: verificação inicial falhou, mas continuando...");
    }
  } else {
    if (!hx711_pronto) {
      Serial.println("[SETUP] ERRO: HX711 nao respondeu (DT nunca foi a LOW). Verifique a fiacao.");
    } else {
      Serial.println("[SETUP] ERRO: sinal no DT nao varia entre leituras -- HX711 provavelmente nao esta fisicamente conectado.");
    }
  }

  // Sobe o servidor web (Wi-Fi + rotas) na task do core 0 -- ver web_interface.cpp.
  iniciarInterfaceWeb();

  // Task de sensores presa ao CORE 1 -- prioridade acima da task padrão do
  // Arduino (loopTask, prioridade 1), já que a leitura do HX711 é sensível
  // a atraso.
  xTaskCreatePinnedToCore(
      taskSensores,
      "task_sensores",
      8192,
      NULL,
      2,
      NULL,
      1 // CORE 1
  );

  Serial.println("[SETUP] Task de sensores criada no core 1. Interface web no core 0.");
  Serial.println("[SETUP] ==========================================");
}

void loop() {
  // Toda a lógica roda nas tasks dedicadas (core 0 = web, core 1 = sensores).
  // O loop padrão do Arduino fica praticamente ocioso.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
