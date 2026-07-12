# Balança com Célula de Carga (ESP32 + HX711)

Um firmware para ESP32 de alta precisão projetado para operar balanças baseadas em células de carga (via módulo HX711). Ele oferece calibração inteligente por múltiplos pontos, detecção automática de pesagem e uma interface web limpa projetada para uso em laboratório ou bancada.

## Funcionalidades

- **Arquitetura Dual-Core**: Leitura crítica de tempo do HX711 e cálculos de pesagem isolados no Core 1. O Core 0 gerencia exclusivamente a conectividade Wi-Fi e a interface Web, garantindo que o servidor não afete o tempo de resposta ou a precisão do sensor.
- **Calibração Multiponto Inteligente**: Utiliza regressão linear (Mínimos Quadrados) com suporte para múltiplos pontos manuais, oferecendo precisão muito superior a sistemas de ponto único.
- **Pesagem Automática com Detecção de Estabilidade**: A máquina de estados identifica automaticamente quando um objeto é colocado na balança, aguarda a estabilização do peso, executa uma captura robusta de múltiplas amostras (100 amostras) e trava o resultado na tela.
- **Interface Web Limpa**: Interface laboratorial e minimalista, focada na visualização da curva de calibração em tempo real e na indicação clara de que o processamento (amostragem) está ocorrendo.
- **Persistência de Dados**: Calibração e curva de regressão são salvas automaticamente na memória NVS (Non-Volatile Storage) do ESP32, sobrevivendo a reinicializações.

## Pré-requisitos

- Placa baseada em **ESP32** (ex: ESP32-DevKitC, NodeMCU-32S, etc).
- Módulo Conversor A/D **HX711**.
- Célula de carga compatível com sua faixa de peso.
- [PlatformIO](https://platformio.org/) (extensão do VSCode ou CLI) para compilação e envio do código.

## Configuração do Hardware (Wiring)

Conecte a célula de carga ao módulo HX711 e este ao ESP32 seguindo o esquema abaixo:

| Célula de Carga | HX711 (Analógico) |
| :--- | :--- |
| Fio Vermelho | `E+` (Excitação Positiva) |
| Fio Preto | `E-` (Excitação Negativa) |
| Fio Verde | `A-` (Sinal Negativo) |
| Fio Branco | `A+` (Sinal Positivo) |

| HX711 (Digital) | ESP32 |
| :--- | :--- |
| `VCC` | `3.3V` (**NUNCA USE 5V/VIN**) |
| `GND` | `GND` |
| `DT` | `GPIO 32` |
| `SCK` | `GPIO 33` |

*Os pinos digitais podem ser alterados editando as constantes `PINO_DT` e `PINO_SCK` em `src/main.cpp`.*

## Instalação e Compilação

O projeto é gerenciado via PlatformIO. As dependências (como a biblioteca HX711 por bogde) estão pré-configuradas no `platformio.ini`.

1. Clone o repositório ou abra a pasta do projeto no VSCode com PlatformIO.
2. Certifique-se de que sua placa correta está selecionada no `platformio.ini` (o padrão é `esp32dev`).
3. Conecte o ESP32 via USB.
4. Compile e envie o código:

```bash
# Se estiver usando o CLI do PlatformIO
pio run --target upload

# Para visualizar os logs seriais do processo de boot e calibração
pio device monitor
```

## Estrutura do Projeto

```text
.
├── src/
│   ├── main.cpp              # (CORE 1) Leitura do HX711, regressão linear, calibração e NVS
│   ├── web_interface.cpp     # (CORE 0) Servidor HTTP, UI e máquina de pesagem automática
│   ├── web_interface.h       # Estruturas compartilhadas (EstadoCompartilhado e Mutex)
│   ├── wifi_provisioning.cpp # Gerenciamento de conexão Wi-Fi (SmartConfig/AP/STA)
│   └── wifi_provisioning.h
├── platformio.ini            # Dependências, flags de compilação e configurações da placa
└── PRODUCT.md                # Diretrizes de UX, design de UI e propósito do produto
```

## Como Usar (Interface Web e Calibração)

1. **Conexão**: Ao ligar o ESP32, ele tentará conectar à última rede Wi-Fi. (Se aplicável, siga as instruções de provisionamento descritas nos logs via porta serial).
2. **Acesso**: Abra o monitor serial para descobrir o endereço IP atribuído ao ESP32. Digite esse IP no navegador de um dispositivo na mesma rede.
3. **Tara (Zero)**: Certifique-se de que a balança está completamente vazia e clique em "Tarar". O sistema coletará amostras e definirá o zero absoluto.
4. **Calibração**:
   - Coloque um peso conhecido (ex: 500g) na balança.
   - Na interface web, digite o peso real (500) e clique no botão de adição.
   - O sistema executará uma amostragem de 100 pontos para alta precisão, adicionará esse ponto ao gráfico e re-calculará instantaneamente a linha de regressão.
   - Repita para mais pesos para maior precisão (multiponto).
5. **Pesagem Automática**: Após calibrado, simplesmente coloque qualquer objeto na balança. A interface indicará visualmente que está "Calculando" e travará no peso final detectado. Remova o item para o sistema retornar ao estado de "Zero".
