#pragma once
#include <Arduino.h>

// CONTRATO ENTRE OS DOIS CORES
// main.cpp roda no CORE 1: lê o HX711, filtra, tara, calibra, grava NVS.
// web_interface.cpp roda no CORE 0: sobe Wi-Fi + servidor HTTP, serve a página
// e as rotas /api/*, e roda a máquina de estados de "pesagem automática".
//
// Variáveis tocadas pelos dois lados precisam estar dentro de EstadoCompartilhado
// e lidas/escritas segurando estado_mutex.
// Pedidos são rastreados por id sequencial incrementado pelo core 0,
// para evitar condições de corrida tipo "pedido antigo executado duas vezes".

/**
 * Tipos de pedido que o core 0 (web) enfileira para o core 1 (sensor) executar.
 * Apenas um pedido pendente por vez; a UI desabilita os botões em andamento.
 */
enum TipoPedido {
  PEDIDO_NENHUM = 0,
  PEDIDO_TARA,             // tara simples (rápida)
  PEDIDO_CAPTURA_CAL,      // captura robusta para um ponto de calibração manual
  PEDIDO_CAPTURA_AUTO,     // captura robusta disparada pela detecção automática de objeto
  PEDIDO_RESETAR_CAL,      // apaga calibração (rápido, mas passa pelo mesmo pipeline por simplicidade)
  PEDIDO_REMOVER_PONTO,    // remove um ponto de calibração pelo índice
};

/**
 * Estado da máquina de pesagem automática, espelhado na UI.
 */
enum EstadoPesagem {
  PESAGEM_ZERO = 0,        // célula vazia (dentro do limiar de zero)
  PESAGEM_DETECTADO,       // saiu do zero, ainda não iniciou captura robusta
  PESAGEM_CALCULANDO,      // captura robusta em andamento (~5s)
  PESAGEM_RESULTADO,       // captura concluída, objeto ainda sobre a célula
  PESAGEM_ZERANDO,         // objeto retirado, aguardando estabilizar no zero
};

/**
 * Ponto de calibração. Duplicado de PontoCalibracao (main.cpp) de propósito
 * para evitar dependência cíclica de includes; os dois lados só concordam no LAYOUT.
 */
struct PontoCalWeb {
  float peso_real_g;
  float leitura_raw;
};

constexpr int WEB_MAX_PONTOS_CAL = 20;

// ±5g em torno de zero conta como "célula vazia". Fora dessa faixa, a
// pesagem automática detecta objeto e dispara captura robusta.
// Definido aqui como fonte única de verdade (usado em web_interface.cpp).
constexpr float LIMIAR_ZERO_AUTO_G = 5.0f;

// Duração mínima da animação de "zerando" (ms) — garante que o feedback
// visual de peso retirado seja perceptível antes de voltar a "célula vazia".
constexpr uint32_t DURACAO_MINIMA_ZERANDO_MS = 1500;

/**
 * Única struct que atravessa os dois cores.
 * Sempre acessar segurando estado_mutex via TravaEstado.
 */
struct EstadoCompartilhado {
  // --- leitura ao vivo (escrito pelo core 1, lido pelo core 0) ---
  float peso_raw_filtrado   = 0;
  float peso_gramas         = 0;
  bool  hx711_ok            = false;
  bool  sinal_estavel       = false;
  long  variacao_atual      = 0;

  // --- wifi (escrito pelo core 0) ---
  bool  wifi_conectado      = false;
  char  wifi_ip[16]         = "";

  // --- calibração (escrito pelo core 1 após tara/calibrar, lido pelo core 0) ---
  float cal_fator           = 1.0f;
  float cal_offset          = 0.0f;
  float margem_seguranca    = 0.0f;
  bool  calibrado           = false;

  int   num_pontos_cal      = 0;
  PontoCalWeb pontos_cal[WEB_MAX_PONTOS_CAL];

  // --- pedidos: core 0 escreve, core 1 consome ---
  TipoPedido pedido_tipo = PEDIDO_NENHUM;
  uint32_t   pedido_seq  = 0;   // incrementado a cada novo pedido
  uint32_t   pedido_seq_atendido  = 0;   // último seq que o core 1 já processou
  float      pedido_peso_g        = 0;   // parâmetro do pedido (peso conhecido, para PEDIDO_CAPTURA_CAL)
  int        pedido_idx           = 0;   // parâmetro do pedido (índice, para PEDIDO_REMOVER_PONTO)

  // --- resultado do último pedido processado (core 1 escreve, core 0 lê) ---
  bool  ultimo_pedido_ok    = true;
  char  ultimo_pedido_erro[64] = "";
  bool  pedido_em_andamento = false;

  // --- máquina de pesagem automática (core 0 decide o estado, core 1 só executa a captura quando mandado) ---
  EstadoPesagem estado_pesagem = PESAGEM_ZERO;
  float peso_auto_resultado_g  = 0;
};

extern EstadoCompartilhado g_estado;
extern SemaphoreHandle_t   g_estado_mutex;

// Pequenos helpers RAII-like para não esquecer o Give() em algum return
// antecipado -- usar sempre via macro para não precisar lembrar o padrão.
struct TravaEstado {
  TravaEstado()  { xSemaphoreTake(g_estado_mutex, portMAX_DELAY); }
  ~TravaEstado() { xSemaphoreGive(g_estado_mutex); }
};
#define COM_ESTADO_TRAVADO TravaEstado _trava_estado_raii;

/**
 * Inicializa a interface web e as tasks do core 0.
 * Deve ser chamado no setup() após a criação do mutex g_estado_mutex.
 */
void iniciarInterfaceWeb();

/**
 * Enfileira um pedido para o core 1 executar.
 * @param tipo Tipo de pedido a ser processado
 * @param peso_g Peso opcional (utilizado para calibração)
 * @param idx Índice opcional (utilizado para remover ponto de calibração)
 * @returns true se enfileirado com sucesso, false se já houver pedido em andamento
 */
bool enfileirarPedido(TipoPedido tipo, float peso_g = 0, int idx = 0);
