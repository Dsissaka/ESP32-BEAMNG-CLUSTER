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
    if (campos != 11) return false;
  } else if (strncmp(linha, "DASH2,", 6) == 0) {
    campos = sscanf(linha, "DASH2,%f,%d,%d,%f,%d,%d,%d %c",
      &d.velocidade, &d.rpm, &d.marcha, &d.combustivel,
      &d.shift, &d.escalaRpm, &d.limiteShift, &extra);
    if (campos != 7) return false;
  } else {
    campos = sscanf(linha, "DASH1,%f,%d,%d,%f %c",
      &d.velocidade, &d.rpm, &d.marcha, &d.combustivel, &extra);
    if (campos != 4) return false;
    d.shift = 0;
    d.escalaRpm = RPM_ESCALA;
    d.limiteShift = 6500;
  }
  return (d.shift == 0 || d.shift == 1) &&
    d.absEstado >= -1 && d.absEstado <= 1 &&
    (d.turboDisponivel == 0 || d.turboDisponivel == 1) &&
    isfinite(d.turbo) && d.turbo >= -5 && d.turbo <= 100 &&
    isfinite(d.turboEscala) && d.turboEscala >= 0.1f && d.turboEscala <= 100 &&
    d.escalaRpm >= 1000 && d.escalaRpm <= 30000 &&
    d.limiteShift >= 1000 && d.limiteShift <= d.escalaRpm &&
    isfinite(d.velocidade) &&
    isfinite(d.combustivel) && d.velocidade >= 0 && d.velocidade <= 500 &&
    d.rpm >= 0 && d.rpm <= 30000 && d.marcha >= -1 && d.marcha <= 30 &&
    d.combustivel >= 0 && d.combustivel <= 100;
}

uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
  }
  return ~crc;
}
uint32_t u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
bool lerExato(uint8_t* p, size_t n) {
  size_t total = 0;
  uint32_t inicio = millis();
  while (total < n) {
    if (millis() - inicio > 3000) return false;
    int a = Serial.available();
    if (a > 0) {
      size_t falta = n - total;
      size_t qtd = (size_t)a < falta ? (size_t)a : falta;
      total += Serial.read(p + total, qtd);
    } else vTaskDelay(pdMS_TO_TICKS(1));
  }
  return true;
}

void tarefaSerial(void*) {
  uint8_t pagina = 1;
  for (;;) {
    xQueueReceive(filaPagina, &pagina, 0);
    // Um pedido por resposta; o mesmo canal carrega telemetria e GPS.
    while (Serial.available()) Serial.read();
    Serial.printf("REQ,%u\n", pagina);
    uint8_t h[13];
    bool ok = lerExato(h, sizeof(h));
    if (ok && memcmp(h, "RLY1", 4) == 0) {
      uint32_t tamanho = u32(h + 5);
      if (tamanho > 0 && tamanho <= MAX_DADOS && lerExato(dadosRx, tamanho) &&
          crc32(dadosRx, tamanho) == u32(h + 9)) {
        dadosRx[tamanho] = 0;
        Quadro q = {tamanho, (char)h[4], pagina};
        xQueueSend(filaQuadro, &q, portMAX_DELAY);
        // Buffer so pode ser reutilizado depois que a UI terminar a leitura.
        xSemaphoreTake(quadroLivre, portMAX_DELAY);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(pagina == 1 ? 100 : 30));
  }
}

void textoCentral(Adafruit_GFX& destino, const char* texto, int centroX,
                  int y, uint8_t tamanho, uint16_t cor) {
  destino.setFont(nullptr);
  destino.setTextSize(tamanho);
  destino.setTextColor(cor);
  destino.setTextWrap(false);
  int16_t x1, y1;
  uint16_t w, h;
  destino.getTextBounds(texto, 0, 0, &x1, &y1, &w, &h);
  destino.setCursor(centroX - (int)w / 2, y);
  destino.print(texto);
}

void abas() {
  tft.fillRect(0, 208, 320, 32, FUNDO);
  tft.fillRoundRect(3, 211, 154, 27, 4, paginaAtual == 1 ? 0x2945 : 0x1082);
  tft.fillRoundRect(163, 211, 154, 27, 4, paginaAtual == 2 ? 0x2945 : 0x1082);
  textoCentral(tft, "PAINEL", 80, 216, 1, paginaAtual == 1 ? AMARELO : CINZA);
  textoCentral(tft, "MINIMAPA", 240, 216, 1, paginaAtual == 2 ? AMARELO : CINZA);
  textoCentral(tft, "< DESLIZE >", 160, 230, 1, CINZA);
}
void mensagem(const char* s) {
  tft.fillRect(0, 0, 320, 208, FUNDO);
  textoCentral(tft, s, 160, 92, 1, AMARELO);
  abas();
}

void linhaRadial(float angulo, int interno, int externo, uint16_t cor) {
  float a = angulo * 0.01745329252f;
  mostrador.drawLine(88 + lroundf(cosf(a) * interno),
                    88 + lroundf(sinf(a) * interno),
                    88 + lroundf(cosf(a) * externo),
                    88 + lroundf(sinf(a) * externo), cor);
}

void desenharMostrador(const Telemetria* d) {
  mostrador.fillScreen(FUNDO);
  // Arco de 270 graus, com abertura na parte inferior.
  int escala = d ? d->escalaRpm : RPM_ESCALA;
  int limite = d ? d->limiteShift : 6500;
  float faixaVermelha = 270.0f * limite / escala;
  float fracao = d ? constrain(d->rpm / (float)escala, 0.0f, 1.0f) : 0;
  for (int i = 0; i <= 270; ++i) {
    uint16_t cor = i >= faixaVermelha ? 0x6000 : 0x2104;
    if (d && i <= fracao * 270 && d->rpm > 0) {
      cor = i >= faixaVermelha ? VERMELHO : AMARELO;
    }
    linhaRadial(135 + i, 77, 83, cor);
  }
  for (int i = 0; i <= 32; ++i) {
    float posicao = i * 270.0f / 32;
    linhaRadial(135 + posicao, i % 4 == 0 ? 64 : 70,
                75, posicao >= faixaVermelha ? VERMELHO : ILI9341_WHITE);
    if (i % 4 == 0) {
      float a = (135 + posicao) * 0.01745329252f;
      char numero[8];
      float valor = escala * i / 32000.0f;
      if (fabsf(valor - roundf(valor)) < 0.01f) snprintf(numero, sizeof(numero), "%.0f", valor);
      else snprintf(numero, sizeof(numero), "%.1f", valor);
      textoCentral(mostrador, numero, 88 + lroundf(cosf(a) * 53),
                    85 + lroundf(sinf(a) * 53), 1, DISCRETO);
    }
  }
  if (d) {
    // Indicador no anel: nao atravessa a marcha central.
    float angulo = 135 + fracao * 270;
    for (int i = -1; i <= 1; ++i) {
      linhaRadial(angulo + i, 63, 84, ILI9341_WHITE);
    }
  }
  mostrador.drawCircle(88, 88, 39, CINZA);
  char marcha[8] = "-";
  if (d) {
    if (d->marcha < 0) snprintf(marcha, sizeof(marcha), "R");
    else if (d->marcha == 0) snprintf(marcha, sizeof(marcha), "N");
    else snprintf(marcha, sizeof(marcha), "%d", d->marcha);
  }
  textoCentral(mostrador, marcha, 88, 69, 5, ILI9341_WHITE);
  textoCentral(mostrador, "MARCHA", 88, 110, 1, AMARELO);
  char rpm[12] = "----";
  if (d) snprintf(rpm, sizeof(rpm), "%d", d->rpm);
  textoCentral(mostrador, rpm, 88, 141, 2, ILI9341_WHITE);
  textoCentral(mostrador, "RPM x1000", 88, 160, 1, DISCRETO);
  tft.drawRGBBitmap(72, 28, mostrador.getBuffer(), 176, 176);
}

void desenharLaterais(const Telemetria* d) {
  char texto[12] = "---";
  valorLateral.fillScreen(FUNDO);
  if (d) snprintf(texto, sizeof(texto), "%d", (int)lroundf(d->velocidade));
  textoCentral(valorLateral, texto, 32, 9, 3, ILI9341_WHITE);
  tft.drawRGBBitmap(4, 91, valorLateral.getBuffer(), 64, 48);

  valorLateral.fillScreen(FUNDO);
  if (d) snprintf(texto, sizeof(texto), "%d%%", (int)lroundf(d->combustivel));
  else snprintf(texto, sizeof(texto), "--%%");
  uint16_t cor = d && d->combustivel < 15 ? VERMELHO : ILI9341_WHITE;
  textoCentral(valorLateral, texto, 32, 13, 2, cor);
  tft.drawRGBBitmap(252, 91, valorLateral.getBuffer(), 64, 48);

  tft.fillRect(256, 151, 55, 7, FUNDO);
  tft.drawRect(255, 150, 57, 9, CINZA);
  if (d) {
    int largura = lroundf(d->combustivel * 55.0f / 100.0f);
    if (largura > 0) tft.fillRect(256, 151, largura, 7,
        d->combustivel < 15 ? VERMELHO : AMARELO);
  }
}

void desenharExtras(const Telemetria* d) {
  // Turbo abaixo da velocidade. Escala cheia nao altera o valor numerico.
  tft.fillRect(3, 166, 66, 42, FUNDO);
  textoCentral(tft, "TURBO", 36, 168, 1, AMARELO);
  char texto[16] = "-- bar";
  if (d && d->turboDisponivel) snprintf(texto, sizeof(texto), "%.2f bar", d->turbo);
  textoCentral(tft, texto, 36, 181, 1, ILI9341_WHITE);
  tft.drawRect(7, 195, 58, 8, CINZA);
  if (d && d->turboDisponivel) {
    int w = lroundf(constrain(d->turbo / d->turboEscala, 0.0f, 1.0f) * 56);
    if (w > 0) tft.fillRect(8, 196, w, 6, AMARELO);
  }
  // Icone compacto: ABS entre circulo e parenteses.
  tft.fillRect(252, 166, 64, 42, FUNDO);
  uint16_t cor = !d || d->absEstado < 0 ? 0x2104 :
                d->absEstado == 1 ? AMARELO : CINZA;
  tft.drawCircle(284, 186, 14, cor);
  textoCentral(tft, "ABS", 284, 183, 1, cor);
  for (int i = -55; i <= 55; ++i) {
    float a = i * 0.01745329252f;
    tft.drawPixel(284 + lroundf(cosf(a) * 19), 186 + lroundf(sinf(a) * 19), cor);
    tft.drawPixel(284 - lroundf(cosf(a) * 19), 186 + lroundf(sinf(a) * 19), cor);
  }
}

void desenharPainel() {
  tft.fillScreen(FUNDO);
  textoCentral(tft, "BEAMNG", 160, 8, 2, ILI9341_WHITE);
  tft.drawFastHLine(10, 24, 300, CINZA);
  textoCentral(tft, "VELOCIDADE", 36, 75, 1, AMARELO);
  textoCentral(tft, "km/h", 36, 143, 1, DISCRETO);
  textoCentral(tft, "COMBUST.", 284, 75, 1, AMARELO);
  desenharMostrador(nullptr);
  desenharLaterais(nullptr);
  desenharExtras(nullptr);
  abas();
}

bool blocoJpeg(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* p) {
  if (x < 0 || y < 0 || x + w > 320 || y + h > 208) return false;
  tft.drawRGBBitmap(x, y, p, w, h);
  return true;
}

void setup() {
  Serial.setRxBufferSize(4096);
  Serial.begin(BAUD);
  Serial.println("HELLO,RLY1,460800");
  pinMode(5, OUTPUT); digitalWrite(5, HIGH);
  pinMode(22, OUTPUT); digitalWrite(22, HIGH);
  SPI.begin(18, 19, 23);
  tft.begin(); tft.setRotation(3);
  // Aloca apos a inicializacao do Arduino, com a memoria disponivel.
  bufferMostrador = new (std::nothrow) GFXcanvas16(176, 176);
  touch.begin(); // Rotacao padrao: preserva a calibracao medida.
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(blocoJpeg);
  tft.fillScreen(FUNDO);
  filaQuadro = xQueueCreate(1, sizeof(Quadro));
  filaPagina = xQueueCreate(1, sizeof(uint8_t));
  quadroLivre = xSemaphoreCreateBinary();
  if ((!bufferMostrador || !mostrador.getBuffer() || !valorLateral.getBuffer()) || !filaQuadro || !filaPagina || !quadroLivre) {
    Serial.printf("ERRO,MEMORIA,canvas=%d,heap=%u,maior=%u\n",
      bufferMostrador && mostrador.getBuffer() != nullptr && valorLateral.getBuffer() != nullptr, ESP.getFreeHeap(),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    mensagem("Erro de memoria. Reinicie.");
    return;
  }
  desenharPainel();
  pronto = xTaskCreate(tarefaSerial, "SerialRX", 4096, nullptr, 2, nullptr) == pdPASS;
  if (!pronto) {
    Serial.println("ERRO,TAREFA_SERIAL");
    mensagem("Erro ao iniciar comunicacao");
  }
}


int destinoGesto(int pagina, int x0, int y0, int x1, int y1, uint32_t duracao) {
  int dx = x1-x0, dy = y1-y0;
  if (duracao <= 1800 && abs(dy) <= 65) {
    if (dx <= -55 && pagina == 1) return 2;
    if (dx >= 55 && pagina == 2) return 1;
  }
  if (duracao <= 800 && abs(dx) < 20 && abs(dy) < 20 && y0 >= 208 && y1 >= 208)
    return x1 < 160 ? 1 : 2;
  return pagina;
}

void loop() {
  static bool rastreando = false, temDados = false, avisoConexao = false;
  static int x0=0, y0=0, x1=0, y1=0, ultimoShift=-1;
  static uint32_t inicioGesto=0, ultimaLeitura=0, ultimoQuadro=0;
  static Telemetria anterior = {};
  if (pronto) {
    if (millis()-ultimaLeitura >= 20) {
      ultimaLeitura = millis();
      bool tocou = touch.touched();
      if (tocou) {
        TS_Point p = touch.getPoint();
        if (p.z >= 200) {
          int x = constrain(map(p.x,3540,549,20,299),0,319);
          int y = constrain(map(p.y,3288,754,20,219),0,239);
          if (!rastreando) {
            x0=x; y0=y; inicioGesto=millis(); rastreando=true;
          }
          x1=x; y1=y;
        }
      } else if (rastreando) {
        rastreando=false;
        uint8_t nova = destinoGesto(paginaAtual,x0,y0,x1,y1,millis()-inicioGesto);
        if (nova != paginaAtual) {
          paginaAtual=nova;
          xQueueOverwrite(filaPagina,&paginaAtual);
          temDados=false; ultimoShift=-1; avisoConexao=false; ultimoQuadro=millis();
          if (nova==1) desenharPainel();
          else mensagem("Carregando minimapa...");
        }
      }
    }
    Quadro q;
    if (xQueueReceive(filaQuadro,&q,0)==pdTRUE) {
      bool valido=false;
      if (q.pagina==paginaAtual) {
        if (q.tipo=='T' && paginaAtual==1) {
          Telemetria d;
          valido=interpretar((char*)dadosRx,d);
          if (valido) {
            if (!temDados) { desenharPainel(); ultimoShift=-1; }
            if (!temDados || d.rpm!=anterior.rpm || d.marcha!=anterior.marcha ||
                d.escalaRpm!=anterior.escalaRpm || d.limiteShift!=anterior.limiteShift)
              desenharMostrador(&d);
            if (!temDados || lroundf(d.velocidade)!=lroundf(anterior.velocidade) ||
                lroundf(d.combustivel)!=lroundf(anterior.combustivel)) desenharLaterais(&d);
            if (!temDados || d.absEstado!=anterior.absEstado || d.turboDisponivel!=anterior.turboDisponivel ||
                lroundf(d.turbo*100)!=lroundf(anterior.turbo*100) || d.turboEscala!=anterior.turboEscala)
              desenharExtras(&d);
            anterior=d; temDados=true;
          }
        } else if (q.tipo=='J' && paginaAtual==2) {
          uint16_t w=0,h=0;
          valido=TJpgDec.getJpgSize(&w,&h,dadosRx,q.tamanho)==JDR_OK && w==320 && h==208 &&
                 TJpgDec.drawJpg(0,0,dadosRx,q.tamanho)==JDR_OK;
        } else if (q.tipo=='E') {
          mensagem((char*)dadosRx); valido=true; temDados=false; ultimoShift=-1;
        }
        if (valido) { ultimoQuadro=millis(); avisoConexao=false; }
        else { mensagem("Quadro invalido. Confira o PC"); temDados=false; ultimoShift=-1; }
      }
      xSemaphoreGive(quadroLivre);
    }
    if (paginaAtual==1 && temDados) {
      bool troca=anterior.shift==1 && anterior.marcha>0;
      int estado=troca ? ((millis()/250)%2==0 ? 2:1):0;
      if (estado!=ultimoShift) {
        ultimoShift=estado;
        tft.fillRect(100,3,120,19,estado==2 ? VERMELHO:FUNDO);
        if (estado==2) textoCentral(tft,"SHIFT",160,5,2,ILI9341_WHITE);
        else if (estado==0) textoCentral(tft,"BEAMNG",160,8,2,ILI9341_WHITE);
      }
    }
    if (!avisoConexao && millis()-ultimoQuadro>5000) {
      mensagem("Sem resposta do PC. Confira a COM6");
      avisoConexao=true; temDados=false; ultimoShift=-1;
    }
  }
  vTaskDelay(pdMS_TO_TICKS(10));
}
