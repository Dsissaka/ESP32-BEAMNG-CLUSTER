#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <math.h>
#include <XPT2046_Touchscreen.h>
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>
#include <new>

Adafruit_ILI9341 tft(5, 2, 4);
XPT2046_Touchscreen touch(22, -1);

// Conta-giros sem LEDs: buffer evita apagar a tela entre os quadros.
GFXcanvas16* bufferMostrador = nullptr;
#define mostrador (*bufferMostrador)
GFXcanvas16 valorLateral(64, 48);
constexpr int RPM_ESCALA = 8000; // Pode ser alterada pelo programa do PC.
constexpr uint16_t FUNDO = 0x0000;
constexpr uint16_t CINZA = 0x528A;
constexpr uint16_t DISCRETO = 0xAD55;
constexpr uint16_t AMARELO = 0xFE60;
constexpr uint16_t VERMELHO = 0xF986;

struct Telemetria {
  float velocidade;
  int rpm;
  int marcha;
  float combustivel;
  int shift;
  int escalaRpm;
  int limiteShift;
  int absEstado;
  float turbo;
  int turboDisponivel;
  float turboEscala;
};

constexpr uint32_t BAUD = 460800;
constexpr uint32_t MAX_DADOS = 50000;
uint8_t dadosRx[MAX_DADOS + 1];
struct Quadro { uint32_t tamanho; char tipo; uint8_t pagina; };
QueueHandle_t filaQuadro = nullptr;
QueueHandle_t filaPagina = nullptr;
SemaphoreHandle_t quadroLivre = nullptr;
bool pronto = false;
uint8_t paginaAtual = 1;

constexpr int ENCODER_CLK = 25;
constexpr int ENCODER_DT = 26;
portMUX_TYPE travaEncoder = portMUX_INITIALIZER_UNLOCKED;
volatile bool alternarPaginaEncoder = false;
uint8_t estadoEncoder = 0;
int8_t passosEncoder = 0;

// Transicoes validas somam ou subtraem um passo. O repique faz o caminho inverso.
DRAM_ATTR const int8_t transicoesEncoder[16] = {
   0,  1, -1,  0,
  -1,  0,  0,  1,
   1,  0,  0, -1,
   0, -1,  1,  0
};

uint8_t IRAM_ATTR lerEncoder() {
  return (digitalRead(ENCODER_CLK) << 1) | digitalRead(ENCODER_DT);
}

void IRAM_ATTR girouEncoder() {
  uint8_t novo = lerEncoder();
  if (novo == estadoEncoder) return;
  int8_t passo = transicoesEncoder[(estadoEncoder << 2) | novo];
  estadoEncoder = novo;
  passosEncoder = passo == 0 ? 0 : passosEncoder + passo;

  // Um ciclo completo alterna a pagina em qualquer sentido.
  if (passosEncoder == 4 || passosEncoder == -4) {
    passosEncoder = 0;
    portENTER_CRITICAL_ISR(&travaEncoder);
    alternarPaginaEncoder = !alternarPaginaEncoder;
    portEXIT_CRITICAL_ISR(&travaEncoder);
  }
}


bool interpretar(const char* linha, Telemetria& d) {
  char extra;
  int campos;
  d.absEstado = -1;
  d.turbo = 0;
  d.turboDisponivel = 0;
  d.turboEscala = 2;
  if (strncmp(linha, "DASH3,", 6) == 0) {
    campos = sscanf(linha, "DASH3,%f,%d,%d,%f,%d,%d,%d,%d,%f,%d,%f %c",
      &d.velocidade, &d.rpm, &d.marcha, &d.combustivel,
      &d.shift, &d.escalaRpm, &d.limiteShift, &d.absEstado,
      &d.turbo, &d.turboDisponivel, &d.turboEscala, &extra);
