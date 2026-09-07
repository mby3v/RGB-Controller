/* =========================================================================
   10 KANAL RGB LED KONTROL PANELİ  —  v2.1
   NodeMCU (ESP8266) + 2 x PCA9685 (0x40 / 0x41)
   -------------------------------------------------------------------------
   v2.1 DEĞİŞİKLİKLERİ
     [A] Varsayılan ışık rengi #FFFFFF yerine #FFD9A8 (sıcak beyaz).
         Tek yerden yönetilir: DEFAULT_R/G/B makroları.
     [B] Açılış animasyonu yavaşlatıldı ve 4 faza çıkarıldı:
         açılım -> tam parlaklık -> uçtan uca süzülen hüzme -> oturma nefesi.
         Toplam ~4,9 sn. Süreler BOOT_* makrolarıyla ayarlanır.
   -------------------------------------------------------------------------
   v2 DEĞİŞİKLİKLERİ
     [1] Parlaklık ölçekleme artık gamma'dan SONRA, 12 bit alanda yapılıyor.
         %1 parlaklık gerçekten görünür; slider'ın alt ucu ölü değil.
     [2] COMMON_ANODE=1 iken "tam kapalı" doğru bit ile veriliyor (full-ON).
     [3] Framebuffer + gölge tampon: sadece DEĞİŞEN kanal I2C'ye yazılıyor.
         Kayan ışık gibi modlarda kare başına 30 kanal yerine ~6 kanal.
     [5] MODE_FIRE'daki random() artık kare başına bir kez çekiliyor.
     [+] Açılışta boot animasyonu, ardından sabit tam güç ışık.
     [+] Animasyon hızı 1..40, üstel skala (1500 ms  <->  2 ms).
   -------------------------------------------------------------------------
   OTA (kablosuz) firmware güncelleme:  http://192.168.1.202/update
   ========================================================================= */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

// =========================================================================
// ⚙️ KULLANICI AYARLARI
// =========================================================================

#define NUM_LEDS      10
#define COMMON_ANODE  0        // Ortak ANOT LED kullanıyorsanız 1 yapın
#define USE_GAMMA     1        // 0 = ham lineer PWM, 1 = göze doğrusal (önerilen)
#define PWM_FREQ      500      // PCA9685 PWM frekansı (Hz)
#define I2C_HZ        100000   // I2C hızı. Framebuffer sayesinde trafik çok azaldı;
                               // yine de 400000 denemeye değer (kablolar kısaysa).
#define REFRESH_MS    1500     // Çıkışın periyodik tam tazelenmesi. Olası bir I2C
                               // hatası kendiliğinden düzelsin diye. 0 = kapalı.
#define AP_NAME       "NodeMCU-LED-Kontrol"
#define HOSTNAME      "ledpanel"   // http://ledpanel.local

// --- Varsayılan (açılış) ışık rengi:  #FFD9A8  sıcak beyaz ---
// Hem açılış animasyonu hem de animasyondan sonraki sabit durum bu rengi
// kullanır; böylece animasyonun sonunda renk sıçraması olmaz.
#define DEFAULT_R      0xFF
#define DEFAULT_G      0xD9
#define DEFAULT_B      0xA8

// --- Açılış animasyonu ---
// Toplam süre = EXPAND + BLOOM + (SWEEP_MS * SWEEP_PASSES) + SETTLE
//             = 1500 + 1000 + (850 * 2) + 650 = ~4,9 sn
// Animasyon WiFi'dan ÖNCE çalıştığı için bu süre açılışa doğrudan eklenir;
// daha kısa istersen aşağıdaki süreleri küçült.
#define BOOT_ANIM         1     // 0 yaparsan açılışta doğrudan sabit renk yanar
#define BOOT_EXPAND_MS    1500  // Faz 1: merkezden dışa açılma süresi
#define BOOT_BLOOM_MS     1000  // Faz 2: tam parlaklığa yükselme süresi
#define BOOT_SWEEP_MS     850   // Faz 3: bir tarama geçişinin süresi
#define BOOT_SWEEP_PASSES 2     // Faz 3: kaç geçiş (gidiş + dönüş = 2)
#define BOOT_SETTLE_MS    650   // Faz 4: kapanış nefesinin süresi
#define BOOT_MID_LVL      55    // Faz 1'in bittiği ara seviye (%)
#define BOOT_SWEEP_BASE   45    // Faz 3: hüzmenin gerisindeki zemin seviyesi (%)
#define BOOT_SWEEP_WIDTH  1.7f  // Faz 3: hüzmenin yarı genişliği (LED cinsinden)
#define BOOT_SWEEP_TINT   0.6f  // Faz 3: hüzme tepesinin beyaza kayma oranı (0..1)
#define BOOT_SETTLE_DIP   22    // Faz 4: nefesin ne kadar alçaldığı (%)

// --- Hız skalası ---
#define SPEED_STEPS    40      // Slider üst sınırı (1..SPEED_STEPS)
#define SPEED_MAX_MS   1500.0f // Hız 1 -> en yavaş kare aralığı
#define SPEED_RATIO    0.8527f // Her kademede çarpan. 1500*0.8527^39 ≈ 3 ms

IPAddress local_IP(192, 168, 1, 202);
IPAddress gateway (192, 168, 1,   1);
IPAddress subnet  (255, 255, 255, 0);

// =========================================================================
// ⚠️ TİP TANIMLARI — BURASI DOSYADAKİ İLK FONKSİYONDAN ÖNCE OLMAK ZORUNDA
//
// Arduino IDE, fonksiyon prototiplerini otomatik üretip dosyadaki İLK
// fonksiyon tanımının hemen üstüne yapıştırır. Eğer enum/struct tanımları
// o noktadan sonra kalırsa "variable or field 'setMode' declared void"
// hatası alınır. Bu bloğu aşağı taşımayın.
// =========================================================================

struct RGBPinMap {
  uint8_t board;   // 1 = pwm1 (0x40), 2 = pwm2 (0x41)
  uint8_t r, g, b; // PCA9685 kanal numaraları
};

enum Mode {
  MODE_OFF, MODE_STATIC, MODE_INDIVIDUAL,
  MODE_BREATH, MODE_RAINBOW, MODE_WAVE, MODE_CHASE, MODE_FIRE, MODE_CYLON,
  MODE_STROBE, MODE_SPARKLE, MODE_RED_WHITE_FLOW, MODE_TRI_FLOW,
  MODE_CYLON_BG, MODE_CHASE_BG, MODE_METEOR, MODE_POLICE, MODE_MULTI_WAVE
};

struct ModeDef { const char *key; Mode mode; };

// =========================================================================

// Kalibre edilmiş fiziki LED sıralamanız (değiştirmeyin)
RGBPinMap ledPins[NUM_LEDS] = {
  //  { Kart,  R ,  G ,  B }
  { 1,   13,  12,  14 },  // 1. RGB LED
  { 2,   13,  12,  14 },  // 2. RGB LED
  { 1,   10,   9,  11 },  // 3. RGB LED
  { 1,    7,   6,   8 },  // 4. RGB LED
  { 2,    1,   0,   2 },  // 5. RGB LED
  { 2,    4,   3,   5 },  // 6. RGB LED
  { 1,    1,   0,   2 },  // 7. RGB LED
  { 1,    4,   3,   5 },  // 8. RGB LED
  { 2,   10,   9,  11 },  // 9. RGB LED
  { 2,    7,   6,   8 }   // 10. RGB LED
};

// Web arayüzündeki efekt anahtarı <-> dahili mod eşlemesi
static const ModeDef MODE_TABLE[] = {
  {"off",        MODE_OFF},        {"static",  MODE_STATIC},   {"individual", MODE_INDIVIDUAL},
  {"breath",     MODE_BREATH},     {"rainbow", MODE_RAINBOW},  {"wave",       MODE_WAVE},
  {"multiwave",  MODE_MULTI_WAVE}, {"fire",    MODE_FIRE},     {"sparkle",    MODE_SPARKLE},
  {"strobe",     MODE_STROBE},     {"cylon",   MODE_CYLON},    {"chase",      MODE_CHASE},
  {"cylonbg",    MODE_CYLON_BG},   {"chasebg", MODE_CHASE_BG}, {"meteor",     MODE_METEOR},
  {"police",     MODE_POLICE},     {"rwflow",  MODE_RED_WHITE_FLOW},
  {"triflow",    MODE_TRI_FLOW}
};
static const uint8_t MODE_COUNT = sizeof(MODE_TABLE) / sizeof(MODE_TABLE[0]);

// =========================================================================
// GAMMA TABLOSU (8 bit giriş -> 12 bit PWM, gamma 2.2)
// PWM'i göze doğrusal yapar; düşük parlaklıkta basamaklanmayı yok eder.
// =========================================================================
static const uint16_t GAMMA12[256] PROGMEM = {
     0,    1,    1,    1,    1,    1,    1,    2,    2,    3,    3,    4,    5,    6,    7,    8,
     9,   11,   12,   14,   15,   17,   19,   21,   23,   25,   27,   29,   32,   34,   37,   40,
    43,   46,   49,   52,   55,   59,   62,   66,   70,   73,   77,   82,   86,   90,   95,   99,
   104,  109,  114,  119,  124,  129,  135,  140,  146,  152,  158,  164,  170,  176,  182,  189,
   196,  202,  209,  216,  224,  231,  238,  246,  254,  261,  269,  277,  286,  294,  302,  311,
   320,  328,  337,  347,  356,  365,  375,  384,  394,  404,  414,  424,  435,  445,  456,  467,
   477,  488,  500,  511,  522,  534,  545,  557,  569,  581,  594,  606,  619,  631,  644,  657,
   670,  683,  697,  710,  724,  738,  752,  766,  780,  794,  809,  823,  838,  853,  868,  884,
   899,  914,  930,  946,  962,  978,  994, 1011, 1027, 1044, 1061, 1078, 1095, 1112, 1130, 1147,
  1165, 1183, 1201, 1219, 1237, 1256, 1274, 1293, 1312, 1331, 1350, 1370, 1389, 1409, 1429, 1449,
  1469, 1489, 1509, 1530, 1551, 1572, 1593, 1614, 1635, 1657, 1678, 1700, 1722, 1744, 1766, 1789,
  1811, 1834, 1857, 1880, 1903, 1926, 1950, 1974, 1997, 2021, 2045, 2070, 2094, 2119, 2143, 2168,
  2193, 2219, 2244, 2270, 2295, 2321, 2347, 2373, 2400, 2426, 2453, 2479, 2506, 2534, 2561, 2588,
  2616, 2644, 2671, 2700, 2728, 2756, 2785, 2813, 2842, 2871, 2900, 2930, 2959, 2989, 3019, 3049,
  3079, 3109, 3140, 3170, 3201, 3232, 3263, 3295, 3326, 3358, 3390, 3421, 3454, 3486, 3518, 3551,
  3584, 3617, 3650, 3683, 3716, 3750, 3784, 3818, 3852, 3886, 3920, 3955, 3990, 4025, 4060, 4095
};

static inline uint16_t toPwm(uint8_t v) {
#if USE_GAMMA
  return pgm_read_word(&GAMMA12[v]);
#else
  return ((uint32_t)v * 4095) / 255;
#endif
}

// [DÜZELTME 1] Parlaklık ölçeklemesi gamma'dan SONRA, 12 bit alanda.
// Eskiden 8 bit alanda ölçeklenip sonra gamma uygulanıyordu; bu yüzden
// düşük parlaklıkta çözünürlük çöküyordu (%1 parlaklık = kapalı).
// scale: 0..10000  (globalBrightness * localBrightness)
static inline uint16_t scaledPwm(uint8_t v, uint32_t scale) {
  if (scale == 0) return 0;
  return (uint16_t)(((uint32_t)toPwm(v) * scale) / 10000UL);   // max 4095*10000, uint32'ye sığar
}

// =========================================================================

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

Adafruit_PWMServoDriver pwm1 = Adafruit_PWMServoDriver(0x40);
Adafruit_PWMServoDriver pwm2 = Adafruit_PWMServoDriver(0x41);

// Cihaz TAM GÜÇ SICAK BEYAZ (#FFD9A8) açılır (açılış animasyonundan sonra).
Mode currentMode = MODE_STATIC;

uint8_t currentR = DEFAULT_R, currentG = DEFAULT_G, currentB = DEFAULT_B;  // ön plan
uint8_t bgR      = 255,       bgG      = 255,       bgB      = 255;        // zemin
// NOT: ön plan ile zemin varsayılanı birbirine çok yakın (sıcak beyaz / beyaz)
// olduğu için "ışık + zemin" kullanan modlarda
// (meteor, kayan ışık + zemin, yıldız ışıltısı...) iki renk aynı olur ve
// efekt görünmez. O modları kullanacaksan arayüzden zemin rengini değiştir
// ya da yukarıdaki bgR/bgG/bgB satırını örn. { 0, 40, 90 } yap.

uint8_t globalBrightness = 100;   // %1..100
uint8_t animSpeed        = 20;    // 1..SPEED_STEPS
uint16_t speedDelayMs    = 100;   // animSpeed'den türetilir, recalcSpeed() doldurur

// Tekli LED durumları — hepsi KAPALI başlar
uint8_t indR[NUM_LEDS]  = {0};
uint8_t indG[NUM_LEDS]  = {0};
uint8_t indB[NUM_LEDS]  = {0};
uint8_t indBr[NUM_LEDS] = {100, 100, 100, 100, 100, 100, 100, 100, 100, 100};

unsigned long lastAnimTick = 0;
unsigned long lastRefresh  = 0;
uint16_t animStep    = 0;
int8_t   scanPos     = 0;    // cylon / meteor konumu
int8_t   scanDir     = 1;
uint8_t  selectedLedIndex = 0;

// "Ekrana yazılması gereken bir değişiklik var" bayrağı.
// Statik modlarda hesabı boşuna tekrarlamamak için kullanılır.
bool ledsDirty = true;

// =========================================================================
// [DÜZELTME 3] FRAMEBUFFER + GÖLGE TAMPON
//
// Animasyonlar artık doğrudan I2C'ye değil, RAM'deki tampona yazıyor.
// loop() sonunda fbFlush() sadece DEĞİŞEN kanalları PCA9685'e gönderiyor.
//
// Kazanç: "Kayan Işık" gibi bir modda eskiden kare başına 30 kanal
// yazılıyordu (~15 ms @100 kHz). Şimdi sadece sönen + yanan LED, yani
// 6 kanal (~3 ms). Hız slider'ının üst ucu artık gerçekten çalışıyor ve
// server.handleClient() aç kalmıyor.
// =========================================================================

static uint16_t fbBuf[2][16];      // istenen değerler
static uint16_t fbShadow[2][16];   // PCA9685'e en son yazılan değerler
static bool     fbValid = false;   // false -> bir sonraki flush hepsini yazar

static inline void fbSet(uint8_t board, uint8_t ch, uint16_t v) {
  if (board < 1 || board > 2 || ch > 15) return;
  fbBuf[board - 1][ch] = v;
}

// [DÜZELTME 2] Ortak anotta "tam kapalı" = çıkışın sürekli HIGH olması,
// yani full-ON biti. Eskiden inversiyon v==0 kontrolünden ÖNCE yapıldığı
// için o kontrol hiç tetiklenmiyor, LED %0.02 sızıntıyla hafif yanık
// kalıyordu. Artık kontrol önce, inversiyon sonra.
static inline void pcaWrite(Adafruit_PWMServoDriver &drv, uint8_t ch, uint16_t v) {
  if (v > 4095) v = 4095;
#if COMMON_ANODE
  if (v == 0) { drv.setPWM(ch, 4096, 0); return; }   // tam kapalı -> çıkış hep HIGH
  drv.setPWM(ch, 0, 4095 - v);
#else
  if (v == 0) { drv.setPWM(ch, 0, 4096); return; }   // tam kapalı -> çıkış hep LOW
  drv.setPWM(ch, 0, v);
#endif
}

void fbFlush() {
  // Periyodik tam tazeleme: tek seferlik bir I2C hatasının gölge tamponda
  // kalıcı olarak takılı kalmasını engeller. Artık animasyonlu modlarda da
  // çalışıyor (eskiden sadece statik modlarda vardı).
  unsigned long t = millis();
  if (REFRESH_MS > 0 && (t - lastRefresh) >= (unsigned long)REFRESH_MS) {
    fbValid     = false;
    lastRefresh = t;
  }

  for (uint8_t b = 0; b < 2; b++) {
    Adafruit_PWMServoDriver &drv = (b == 0) ? pwm1 : pwm2;
    for (uint8_t ch = 0; ch < 16; ch++) {
      if (fbValid && fbBuf[b][ch] == fbShadow[b][ch]) continue;
      fbShadow[b][ch] = fbBuf[b][ch];
      pcaWrite(drv, ch, fbBuf[b][ch]);
    }
  }
  fbValid = true;
}

// =========================================================================
// RENK YARDIMCILARI
// =========================================================================

void hexToRGB(String hex, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (hex.startsWith("#")) hex = hex.substring(1);
  if (hex.length() < 6) { r = g = b = 0; return; }
  if (hex.length() > 6) hex = hex.substring(0, 6);   // taşmayı engelle
  unsigned long number = strtoul(hex.c_str(), NULL, 16);
  r = (number >> 16) & 0xFF;
  g = (number >>  8) & 0xFF;
  b =  number        & 0xFF;
}

String rgbToHex(uint8_t r, uint8_t g, uint8_t b) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
  return String(buf);
}

void HSVtoRGB(float h, float s, float v, uint8_t &r, uint8_t &g, uint8_t &b) {
  int i = (int)floor(h * 6);
  float f = h * 6 - i;
  float p = v * (1 - s);
  float q = v * (1 - f * s);
  float t = v * (1 - (1 - f) * s);
  float rF = 0, gF = 0, bF = 0;
  switch (i % 6) {
    case 0: rF = v; gF = t; bF = p; break;
    case 1: rF = q; gF = v; bF = p; break;
    case 2: rF = p; gF = v; bF = t; break;
    case 3: rF = p; gF = q; bF = v; break;
    case 4: rF = t; gF = p; bF = v; break;
    case 5: rF = v; gF = p; bF = q; break;
  }
  r = (uint8_t)(rF * 255);
  g = (uint8_t)(gF * 255);
  b = (uint8_t)(bF * 255);
}

// =========================================================================
// LED YAZMA KATMANI  (artık framebuffer'a yazar, I2C'ye değil)
// =========================================================================

// localBrightness gamma'dan sonra uygulandığı için artık gerçek ışık
// yüzdesi anlamına geliyor: 50 -> yarı ışık. Efektlerdeki kuyruk/zemin
// seviyeleri de bu parametreyle veriliyor.
void setSingleLED(uint8_t i, uint8_t r, uint8_t g, uint8_t b, uint8_t localBrightness) {
  if (i >= NUM_LEDS) return;

  uint32_t scale = (uint32_t)globalBrightness * localBrightness;   // 0..10000
  const RGBPinMap &m = ledPins[i];
  fbSet(m.board, m.r, scaledPwm(r, scale));
  fbSet(m.board, m.g, scaledPwm(g, scale));
  fbSet(m.board, m.b, scaledPwm(b, scale));
}

// Not: varsayılan argüman yerine aşırı yükleme (overload) kullanıldı.
// Arduino'nun otomatik prototip üreteci varsayılan argümanlarla çakışabiliyor.
void setSingleLED(uint8_t i, uint8_t r, uint8_t g, uint8_t b) {
  setSingleLED(i, r, g, b, 100);
}

void setAllLEDs(uint8_t r, uint8_t g, uint8_t b, uint8_t level) {
  for (uint8_t i = 0; i < NUM_LEDS; i++) setSingleLED(i, r, g, b, level);
}

void setAllLEDs(uint8_t r, uint8_t g, uint8_t b) {
  setAllLEDs(r, g, b, 100);
}

void applyIndividualLEDs() {
  for (uint8_t i = 0; i < NUM_LEDS; i++) setSingleLED(i, indR[i], indG[i], indB[i], indBr[i]);
}

// =========================================================================
// HIZ SKALASI
//
// Doğrusal map() yerine üstel skala: slider'ın alt ucunda saniyeler,
// üst ucunda milisaniyeler var ve aradaki her kademe eşit oranda hızlanıyor
// (kulak/göz oransal algılar, bu yüzden doğrusal skala kötü hissettiriyordu).
//   hız  1 -> 1500 ms      hız 20 -> ~73 ms      hız 40 -> ~3 ms
// ⚠️ Arayüzdeki spms() fonksiyonu bu formülün AYNISI olmak zorunda.
// =========================================================================

void recalcSpeed() {
  float d = SPEED_MAX_MS * pow(SPEED_RATIO, (float)animSpeed - 1.0f);
  if (d < 2.0f) d = 2.0f;
  speedDelayMs = (uint16_t)(d + 0.5f);
}

// =========================================================================
// AÇILIŞ ANİMASYONU  (varsayılan sıcak beyaz — DEFAULT_R/G/B)
//
// Faz 1  Açılım    : merkezden iki yana yumuşak açılım, %BOOT_MID_LVL'e
// Faz 2  Yükseliş  : tüm şerit birlikte tam parlaklığa çıkar
// Faz 3  Süzülme   : şerit hafifçe kısılır, uçtan uca parlak bir hüzme
//                    gidip gelir; hüzmenin tepesi bir tık saf beyaza kayar.
//                    Zemin, geçişin başında ve sonunda 100'e döndüğü için
//                    faz geçişlerinde sıçrama görünmez.
// Faz 4  Oturma    : tek bir yumuşak nefes; hafif alçalıp tam parlaklığa oturur
// Sonra: MODE_STATIC sabit sıcak beyaz devralır.
//
// setup() içinde, WiFi'dan ÖNCE çalışır: kullanıcı güç verir vermez görür.
// Bloklar ama toplam ~4,9 sn ve delay() watchdog'u besler.
// =========================================================================

// 0..1 aralığında yumuşatma (smoothstep). Sert kenarları ve ani hız
// değişimlerini yok eder; tüm fazlar bunu kullanır.
static inline float smoothStep(float p) {
  if (p <= 0.0f) return 0.0f;
  if (p >= 1.0f) return 1.0f;
  return p * p * (3.0f - 2.0f * p);
}

static bool renderBoot(unsigned long t) {
  const float center = (NUM_LEDS - 1) / 2.0f;
  const float maxD   = center;

  // --- Faz 1: merkezden dışa açılım ---
  if (t < BOOT_EXPAND_MS) {
    float p     = (float)t / (float)BOOT_EXPAND_MS;
    float front = p * (maxD + 1.2f);            // 1.2 -> son LED de tam açılsın
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
      float d = fabs((float)i - center);
      float v = smoothStep(front - d);          // smoothstep: sert kenar olmasın
      setSingleLED(i, currentR, currentG, currentB, (uint8_t)(v * BOOT_MID_LVL));
    }
    return true;
  }
  t -= BOOT_EXPAND_MS;

  // --- Faz 2: hep birlikte tam parlaklığa ---
  if (t < BOOT_BLOOM_MS) {
    float p = smoothStep((float)t / (float)BOOT_BLOOM_MS);
    setAllLEDs(currentR, currentG, currentB,
               (uint8_t)(BOOT_MID_LVL + p * (100 - BOOT_MID_LVL)));
    return true;
  }
  t -= BOOT_BLOOM_MS;

  // --- Faz 3: uçtan uca süzülen hüzme ---
  const unsigned long sweepTotal = (unsigned long)BOOT_SWEEP_MS * BOOT_SWEEP_PASSES;
  if (t < sweepTotal) {
    // Zemin seviyesi: taramanın başında ve sonunda 100'de kalır, arada
    // BOOT_SWEEP_BASE'e iner. Böylece Faz 2 ve Faz 4 ile arada sıçrama olmaz,
    // hüzme de taramanın orta bölümünde en belirgin görünür.
    float in   = smoothStep((float)t / (BOOT_SWEEP_MS * 0.5f));
    float out  = smoothStep((float)(sweepTotal - t) / (BOOT_SWEEP_MS * 0.5f));
    float env  = (in < out) ? in : out;
    float base = 100.0f - (100.0f - BOOT_SWEEP_BASE) * env;

    // Hüzmenin konumu: her geçişte yön değişir; şeridin dışından girip
    // dışından çıkar, böylece uçlarda takılıp kalmaz.
    uint8_t pass = (uint8_t)(t / BOOT_SWEEP_MS);
    float   p    = smoothStep((float)(t % BOOT_SWEEP_MS) / (float)BOOT_SWEEP_MS);
    if (pass & 1) p = 1.0f - p;
    float head = -BOOT_SWEEP_WIDTH + p * ((NUM_LEDS - 1) + 2.0f * BOOT_SWEEP_WIDTH);

    for (uint8_t i = 0; i < NUM_LEDS; i++) {
      float d = fabs((float)i - head) / BOOT_SWEEP_WIDTH;
      float g = (d >= 1.0f) ? 0.0f : smoothStep(1.0f - d);   // hüzme profili
      float lvl = base + (100.0f - base) * g;

      // Hüzmenin tepesi sıcak beyazdan bir tık saf beyaza kayar: baş kısmı
      // serin ve parlak görünür, arkası sıcak kalır.
      float mix = g * BOOT_SWEEP_TINT;
      uint8_t r  = (uint8_t)(currentR + (255 - currentR) * mix);
      uint8_t gg = (uint8_t)(currentG + (255 - currentG) * mix);
      uint8_t b  = (uint8_t)(currentB + (255 - currentB) * mix);
      setSingleLED(i, r, gg, b, (uint8_t)(lvl + 0.5f));
    }
    return true;
  }
  t -= sweepTotal;

  // --- Faz 4: kapanış nefesi (hafif alçalır, tam parlaklığa oturur) ---
  if (t < BOOT_SETTLE_MS) {
    float dip = sin((float)t / (float)BOOT_SETTLE_MS * PI);   // 0 -> 1 -> 0
    setAllLEDs(currentR, currentG, currentB,
               (uint8_t)(100.0f - BOOT_SETTLE_DIP * dip + 0.5f));
    return true;
  }
  return false;
}

void runBootAnimation() {
#if BOOT_ANIM
  unsigned long t0 = millis();
  for (;;) {
    if (!renderBoot(millis() - t0)) break;
    fbFlush();
    delay(3);            // yield -> WDT beslenir
  }
#endif
  setAllLEDs(currentR, currentG, currentB);   // kalıcı durum: tam güç sıcak beyaz
  fbFlush();
  ledsDirty = true;
}

// =========================================================================
// MOD YÖNETİMİ
// =========================================================================

void setMode(Mode m) {
  if (m != currentMode) {
    currentMode  = m;
    animStep     = 0;
    scanPos      = 0;
    scanDir      = 1;
    lastAnimTick = 0;
  }
  ledsDirty = true;
}

// Tekli LED moduna İLK girişte tüm LED'leri kapatır; böylece kullanıcı
// hangi LED'e dokunursa sadece o yanar, diğerleri eski renklerinde kalmaz.
void enterIndividual() {
  if (currentMode != MODE_INDIVIDUAL) {
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
      indR[i] = 0; indG[i] = 0; indB[i] = 0; indBr[i] = 100;
    }
    setMode(MODE_INDIVIDUAL);
  }
  ledsDirty = true;
}

// =========================================================================
// ANİMASYON MOTORU
// =========================================================================

void handleAnimations() {
  // Statik modlar: sadece bir şey değiştiyse yeniden hesapla.
  // Periyodik tazeleme artık fbFlush()'ın işi, burada gerek yok.
  switch (currentMode) {
    case MODE_OFF:
    case MODE_STATIC:
    case MODE_INDIVIDUAL:
      if (ledsDirty) {
        if      (currentMode == MODE_OFF)    setAllLEDs(0, 0, 0);
        else if (currentMode == MODE_STATIC) setAllLEDs(currentR, currentG, currentB);
        else                                 applyIndividualLEDs();
        ledsDirty = false;
      }
      return;
    default:
      break;
  }

  unsigned long now = millis();
  uint16_t speedDelay = speedDelayMs;

  switch (currentMode) {

    case MODE_BREATH:
      if (now - lastAnimTick >= (unsigned long)(speedDelay / 2 + 1)) {
        lastAnimTick = now;
        animStep = (animStep + 1) % 360;
        float f = (sin(animStep * DEG_TO_RAD) + 1.0) / 2.0;
        // Renk yerine SEVİYE ile soluyor: gamma'dan sonra ölçeklendiği için
        // nefes eğrisi göze gerçekten doğrusal, alt uçta zıplamıyor.
        setAllLEDs(currentR, currentG, currentB, (uint8_t)(f * 100));
      }
      break;

    case MODE_RAINBOW:
      if (now - lastAnimTick >= speedDelay) {
        lastAnimTick = now;
        animStep = (animStep + 1) % 1000;
        uint8_t r, g, b;
        HSVtoRGB(animStep / 1000.0, 1.0, 1.0, r, g, b);
        setAllLEDs(r, g, b);
      }
      break;

    case MODE_WAVE:
      if (now - lastAnimTick >= speedDelay) {
        lastAnimTick = now;
        animStep = (animStep + 1) % 1000;
        float base = animStep / 1000.0;
        for (uint8_t i = 0; i < NUM_LEDS; i++) {
          uint8_t r, g, b;
          HSVtoRGB(fmod(base + i * 0.08, 1.0), 1.0, 1.0, r, g, b);
          setSingleLED(i, r, g, b);
        }
      }
      break;

    case MODE_MULTI_WAVE:
      if (now - lastAnimTick >= (unsigned long)(speedDelay / 2 + 1)) {
        lastAnimTick = now;
        animStep = (animStep + 2) % 1000;
        float base = animStep / 1000.0;
        for (uint8_t i = 0; i < NUM_LEDS; i++) {
          uint8_t r, g, b;
          HSVtoRGB(fmod(base + i * 0.1, 1.0), 1.0, 1.0, r, g, b);
          setSingleLED(i, r, g, b);
        }
      }
      break;

    case MODE_CHASE:
      if (now - lastAnimTick >= speedDelay) {
        lastAnimTick = now;
        animStep = (animStep + 1) % NUM_LEDS;
        setAllLEDs(0, 0, 0);
        setSingleLED(animStep, currentR, currentG, currentB);
      }
      break;

    case MODE_CHASE_BG:
      if (now - lastAnimTick >= speedDelay) {
        lastAnimTick = now;
        animStep = (animStep + 1) % NUM_LEDS;
        setAllLEDs(bgR, bgG, bgB);
        setSingleLED(animStep, currentR, currentG, currentB);
      }
      break;

    case MODE_FIRE: {
      // [DÜZELTME 5] random() eskiden her loop turunda yeniden çekiliyordu;
      // koşul her seferinde farklı bir eşikle sınandığı için efektif gecikme
      // 5 ms'e doğru sapıyordu ve "rastgele" hissi kayboluyordu. Artık eşik
      // kare tetiklendiğinde bir kez belirleniyor.
      static uint16_t fireJitter = 0;
      if (now - lastAnimTick >= (unsigned long)speedDelay + fireJitter) {
        lastAnimTick = now;
        fireJitter = random(5, 30);
        for (uint8_t i = 0; i < NUM_LEDS; i++) {
          uint8_t flicker = random(120, 255);
          setSingleLED(i, flicker, flicker / 3, 0);
        }
      }
      break;
    }

    case MODE_CYLON:
    case MODE_CYLON_BG:
      if (now - lastAnimTick >= speedDelay) {
        lastAnimTick = now;
        if (currentMode == MODE_CYLON) setAllLEDs(0, 0, 0);
        else                           setAllLEDs(bgR, bgG, bgB);
        setSingleLED(scanPos, currentR, currentG, currentB);
        scanPos += scanDir;
        if (scanPos >= NUM_LEDS - 1 || scanPos <= 0) scanDir = -scanDir;
      }
      break;

    case MODE_METEOR:
      if (now - lastAnimTick >= speedDelay) {
        lastAnimTick = now;
        animStep = (animStep + 1) % NUM_LEDS;
        setAllLEDs(bgR, bgG, bgB, 20);                                  // sönük zemin
        setSingleLED(animStep, currentR, currentG, currentB, 100);      // baş
        setSingleLED((animStep - 1 + NUM_LEDS) % NUM_LEDS, currentR, currentG, currentB, 45);
        setSingleLED((animStep - 2 + NUM_LEDS) % NUM_LEDS, currentR, currentG, currentB, 18);
      }
      break;

    case MODE_POLICE:
      if (now - lastAnimTick >= (unsigned long)speedDelay * 2) {
        lastAnimTick = now;
        animStep = !animStep;
        for (uint8_t i = 0; i < NUM_LEDS; i++) {
          if (i < NUM_LEDS / 2) setSingleLED(i, animStep ? 255 : 0, 0, 0);
          else                  setSingleLED(i, 0, 0, animStep ? 255 : 0);
        }
      }
      break;

    case MODE_STROBE:
      if (now - lastAnimTick >= (unsigned long)speedDelay * 2) {
        lastAnimTick = now;
        animStep = !animStep;
        if (animStep) setAllLEDs(currentR, currentG, currentB);
        else          setAllLEDs(0, 0, 0);
      }
      break;

    case MODE_SPARKLE:
      if (now - lastAnimTick >= speedDelay) {
        lastAnimTick = now;
        setAllLEDs(bgR, bgG, bgB, 25);
        setSingleLED(random(0, NUM_LEDS), currentR, currentG, currentB, 100);
      }
      break;

    case MODE_RED_WHITE_FLOW:
      if (now - lastAnimTick >= (unsigned long)speedDelay * 2) {
        lastAnimTick = now;
        animStep = (animStep + 1) % 4;
        for (uint8_t i = 0; i < NUM_LEDS; i++) {
          if (((i + animStep) % 4) < 2) setSingleLED(i, 255, 0, 0);
          else                          setSingleLED(i, 255, 255, 255);
        }
      }
      break;

    case MODE_TRI_FLOW:
      if (now - lastAnimTick >= (unsigned long)speedDelay * 2) {
        lastAnimTick = now;
        animStep = (animStep + 1) % 3;
        for (uint8_t i = 0; i < NUM_LEDS; i++) {
          uint8_t pos = (i + animStep) % 3;
          if      (pos == 0) setSingleLED(i, 255, 0, 0);
          else if (pos == 1) setSingleLED(i, 0, 255, 0);
          else               setSingleLED(i, 0, 0, 255);
        }
      }
      break;

    default:
      break;
  }
}

// =========================================================================
// WEB ARAYÜZÜ (PROGMEM — RAM'de yer kaplamaz)
// =========================================================================

static const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html><html lang="tr"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>RGB LED Paneli</title>
<style>
:root{--bg:#0d0d10;--card:#191920;--line:#2b2b36;--txt:#eceff4;--dim:#8b8b9a;--acc:#4da6ff}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;padding:14px 12px 34px;background:var(--bg);color:var(--txt);
     font:15px/1.45 -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif}
.wrap{max-width:470px;margin:0 auto}
.hide{display:none!important}
header{display:flex;align-items:baseline;justify-content:space-between;padding:0 4px 12px}
h1{font-size:17px;margin:0;font-weight:650;letter-spacing:.2px}
#ip{font-size:11px;color:var(--dim)}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:14px;margin-bottom:12px}
.card h2{font-size:11px;letter-spacing:1px;text-transform:uppercase;color:var(--dim);
         margin:0 0 12px;font-weight:600;display:flex;justify-content:space-between;align-items:center;gap:8px}
.card h2 span{text-transform:none;letter-spacing:0;font-size:12px;color:var(--acc);font-weight:500;
              white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.hint{font-size:11.5px;color:var(--dim);margin:10px 2px 0;line-height:1.55}

/* --- Efekt menüsü --- */
select{width:100%;padding:13px 11px;border-radius:11px;background:#22222b;color:var(--txt);
       border:1px solid var(--line);font-size:15px;font-weight:600;outline:none;
       -webkit-appearance:none;appearance:none;
       background-image:linear-gradient(45deg,transparent 50%,var(--dim) 50%),
                        linear-gradient(135deg,var(--dim) 50%,transparent 50%);
       background-position:calc(100% - 19px) 50%,calc(100% - 13px) 50%;
       background-size:6px 6px,6px 6px;background-repeat:no-repeat;padding-right:38px}
select:focus{border-color:var(--acc)}
optgroup{color:var(--dim);font-size:12px;font-weight:600;background:#15151b}
option{color:var(--txt);font-weight:400;background:#1e1e26;padding:6px}

/* --- LED şeridi --- */
.strip{display:flex;gap:6px;justify-content:space-between}
.dot{flex:1;aspect-ratio:1;border-radius:50%;border:2px solid #33333f;position:relative;
     cursor:pointer;transition:transform .12s,border-color .12s;background:#000}
.dot.sel{border-color:#fff;transform:scale(1.14)}
.dot b{position:absolute;left:0;right:0;bottom:-15px;font-size:9px;color:var(--dim);
       text-align:center;font-weight:500}
.strip.ro .dot{cursor:default}

/* --- Sekmeler --- */
.tabs{display:flex;gap:5px;margin-bottom:12px;background:#12121a;padding:4px;border-radius:11px}
.tab{flex:1;padding:9px 2px;font-size:12.5px;border:0;background:none;color:var(--dim);
     border-radius:8px;cursor:pointer;font-weight:600}
.tab.on{background:#2f2f3c;color:#fff}

/* --- Renk önizleme --- */
.prow{display:flex;align-items:center;gap:12px;margin-bottom:14px}
.swatch{width:76px;height:56px;border-radius:12px;border:2px solid #3a3a48;position:relative;
        overflow:hidden;cursor:pointer;flex-shrink:0;
        background-image:linear-gradient(45deg,#2a2a33 25%,transparent 25%,transparent 75%,#2a2a33 75%),
                         linear-gradient(45deg,#2a2a33 25%,transparent 25%,transparent 75%,#2a2a33 75%);
        background-size:12px 12px;background-position:0 0,6px 6px}
.swatch i{position:absolute;inset:0;display:block}
.swatch input{position:absolute;inset:-10px;opacity:0;cursor:pointer;border:0;padding:0}
.pinfo{font-size:13px;line-height:1.6;min-width:0}
.pinfo b{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:15px;letter-spacing:.5px}
.pinfo em{display:block;font-style:normal;font-size:11.5px;color:var(--dim)}

/* --- Kaydırıcılar --- */
.lab{display:flex;justify-content:space-between;font-size:12.5px;color:var(--dim);margin:2px 2px 6px;gap:8px}
.lab b{color:var(--txt);font-weight:600;white-space:nowrap}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:26px;border-radius:13px;
                  outline:0;border:1px solid var(--line);margin:0 0 13px;background:#25252f}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;border-radius:50%;
   background:#fff;border:2px solid rgba(0,0,0,.55);box-shadow:0 1px 5px rgba(0,0,0,.7);cursor:pointer}
input[type=range]::-moz-range-thumb{width:20px;height:20px;border-radius:50%;background:#fff;
   border:2px solid rgba(0,0,0,.55);cursor:pointer}
#hue{background:linear-gradient(90deg,#f00,#ff0,#0f0,#0ff,#00f,#f0f,#f00)}
#hue.idle{opacity:.32}
.lab .park{color:#e8a33d;font-weight:600}
.lab .ms{color:var(--dim);font-weight:400}

/* --- Hazır renkler --- */
.sw-grid{display:grid;grid-template-columns:repeat(9,1fr);gap:6px;margin-top:2px}
.sw-grid i{aspect-ratio:1;border-radius:50%;cursor:pointer;border:1px solid rgba(255,255,255,.16)}
.sw-grid i:active{transform:scale(.9)}

/* --- Butonlar --- */
.btns{display:flex;gap:7px;flex-wrap:wrap}
.btns button{flex:1;min-width:110px;padding:11px 6px;font-size:12.5px;border-radius:10px;
             border:1px solid var(--line);background:#25252f;color:var(--txt);cursor:pointer;font-weight:600}
.btns button:active{transform:scale(.97)}
.danger{background:#3a1f22!important;border-color:#5d2b30!important;color:#ff9b9b!important}
footer{text-align:center;font-size:11px;color:#55555f;padding-top:6px}
footer a{color:#666672;text-decoration:none;margin:0 7px}
#toast{position:fixed;left:50%;bottom:18px;transform:translateX(-50%) translateY(60px);
       background:#e5484d;color:#fff;padding:9px 16px;border-radius:9px;font-size:12.5px;
       transition:transform .25s,opacity .25s;pointer-events:none;opacity:0;visibility:hidden}
#toast.on{transform:translateX(-50%) translateY(0);opacity:1;visibility:visible}
</style></head><body><div class="wrap">

<header><h1>RGB LED Paneli</h1><span id="ip">bağlanıyor…</span></header>

<div class="card">
  <h2>Efekt</h2>
  <select id="mode"></select>
  <p class="hint" id="mhint"></p>
</div>

<div class="card">
  <h2>LED Durumu <span id="selinfo"></span></h2>
  <div class="strip ro" id="strip"></div>
  <div style="height:14px"></div>
</div>

<div class="card hide" id="cLed">
  <h2>Tekli LED <span id="ledno">LED 1</span></h2>
  <p class="hint" style="margin:0 2px 13px">Yukarıdaki noktalardan bir LED seç; rengini aşağıdaki
     bölümden ayarla. Bu moda geçildiğinde tüm LED’ler kapalı başlar.</p>
  <div class="lab"><span>Bu LED’in parlaklığı</span><b id="lbrval">100%</b></div>
  <input type="range" id="lbr" min="0" max="100" value="100">
  <div class="btns">
    <button id="bAll">Rengi tüm LED’lere</button>
    <button id="bClear" class="danger">Tümünü kapat</button>
  </div>
</div>

<div class="card" id="cColor">
  <h2 id="ctitle">Renk</h2>
  <div class="tabs hide" id="tabs">
    <button class="tab on" data-t="fg">Ön Plan (ışık)</button>
    <button class="tab" data-t="bg">Zemin</button>
  </div>
  <div class="prow">
    <label class="swatch"><i id="sw"></i><input type="color" id="pick"></label>
    <div class="pinfo"><b id="hex">#000000</b><em id="ptip"></em></div>
  </div>
  <div class="lab"><span id="hlab">Renk tonu</span><b id="hval">0°</b></div>
  <input type="range" id="hue" min="0" max="359" value="200">
  <div class="lab"><span>Doygunluk</span><b id="sval">100%</b></div>
  <input type="range" id="sat" min="0" max="100" value="100">
  <div class="sw-grid" id="presets"></div>
</div>

<div class="card">
  <h2>Ayarlar</h2>
  <div class="lab"><span>Genel parlaklık</span><b id="brval">100%</b></div>
  <input type="range" id="br" min="1" max="100" value="100">
  <div id="spwrap">
    <div class="lab"><span>Animasyon hızı</span><b id="spval">20</b></div>
    <input type="range" id="sp" min="1" max="40" value="20">
    <p class="hint" style="margin:-6px 2px 0">Sol uç çok yavaş (1,5 sn/kare), sağ uç çok hızlı (~3 ms/kare).</p>
  </div>
</div>

<footer><a href="/update">OTA Güncelleme</a>·<a href="#" id="rf">Yenile</a></footer>
</div>
<div id="toast">Bağlantı hatası</div>

<script>
/* Her efektin hangi renkleri kullandığı firmware'deki animasyon koduyla birebir:
   c=''  -> renklerini kendi üretir      c='f'  -> sadece ön plan
   c='f b'-> ön plan + zemin             c='i'  -> her LED ayrı            */
var MODES=[
{k:'off',        n:'Kapalı',              c:'',   g:'Temel'},
{k:'static',     n:'Sabit Renk',          c:'f',  g:'Temel'},
{k:'individual', n:'Tekli LED Kontrolü',  c:'i',  g:'Temel'},
{k:'breath',     n:'Nefes',               c:'f',  g:'Tek renk kullanır'},
{k:'chase',      n:'Kayan Işık',          c:'f',  g:'Tek renk kullanır'},
{k:'cylon',      n:'Kara Şimşek',         c:'f',  g:'Tek renk kullanır'},
{k:'strobe',     n:'Flaşör',              c:'f',  g:'Tek renk kullanır'},
{k:'chasebg',    n:'Kayan Işık + Zemin',  c:'fb', g:'Işık + zemin rengi kullanır'},
{k:'cylonbg',    n:'Kara Şimşek + Zemin', c:'fb', g:'Işık + zemin rengi kullanır'},
{k:'meteor',     n:'Meteor (kuyruklu)',   c:'fb', g:'Işık + zemin rengi kullanır'},
{k:'sparkle',    n:'Yıldız Işıltısı',     c:'fb', g:'Işık + zemin rengi kullanır'},
{k:'rainbow',    n:'Gökkuşağı',           c:'',   g:'Renklerini kendi üretir'},
{k:'wave',       n:'Dalga',               c:'',   g:'Renklerini kendi üretir'},
{k:'multiwave',  n:'Hızlı Renk Akışı',    c:'',   g:'Renklerini kendi üretir'},
{k:'fire',       n:'Alev / Ateş',         c:'',   g:'Renklerini kendi üretir'},
{k:'police',     n:'Polis Çakarı',        c:'',   g:'Renklerini kendi üretir'},
{k:'rwflow',     n:'Kırmızı-Beyaz Akış',  c:'',   g:'Renklerini kendi üretir'},
{k:'triflow',    n:'RGB Üçlü Akış',       c:'',   g:'Renklerini kendi üretir'}
];
var STATICM={off:1,static:1,individual:1};   /* animasyon hızı anlamsız olanlar */
var HINT={
 '':  'Bu efekt renklerini kendisi üretir; renk seçimi gerekmez.',
 'f': 'Bu efekt tek bir renk kullanır.',
 'fb':'Bu efekt iki renk kullanır: önde hareket eden ışık, arkada duran zemin.',
 'i': 'Her LED’i tek tek ayarlayabilirsin.'
};
var PRESETS=["#FF0000","#FF6A00","#FFD400","#7CFF00","#00FF44","#00FFD0",
"#00A2FF","#2B4BFF","#8A2BFF","#FF2BD0","#FF6A9E","#FFFFFF",
"#FFB870","#FFD9A8","#C8E4FF","#6E6E80","#303040","#000000"];

/* ⚠️ Firmware'deki recalcSpeed() ile AYNI formül olmak zorunda.
   Slider sürülürken sunucudan cevap beklemeden etiketi güncelleyebilmek için
   burada da hesaplanıyor. Birini değiştirirsen diğerini de değiştir. */
function spms(v){var d=1500*Math.pow(0.8527,v-1);return Math.max(2,Math.round(d))}
function spfmt(v){var m=spms(v);return v+' <span class="ms">· '+(m>=1000?(m/1000).toFixed(1)+' sn':m+' ms')+'</span>'}

var S=null, T='fg', busy=false, pend=null;
var CUR='#000000';            /* ekranda gösterilen renk        */
var UH=200, US=100, UV=1;     /* kaydırıcıların kendi HSV hali  */
var $=function(i){return document.getElementById(i)};
function show(el,v){if(v)el.classList.remove('hide');else el.classList.add('hide')}

/* ---- renk dönüşümleri ---- */
function h2r(h){h=h.replace('#','');return[parseInt(h.substr(0,2),16),parseInt(h.substr(2,2),16),parseInt(h.substr(4,2),16)]}
function r2h(r,g,b){var f=function(v){v=Math.max(0,Math.min(255,Math.round(v))).toString(16);return v.length<2?'0'+v:v};return '#'+f(r)+f(g)+f(b)}
function rgb2hsv(r,g,b){r/=255;g/=255;b/=255;var mx=Math.max(r,g,b),mn=Math.min(r,g,b),d=mx-mn,h=0;
 if(d){if(mx==r)h=((g-b)/d+(g<b?6:0));else if(mx==g)h=(b-r)/d+2;else h=(r-g)/d+4;h*=60}
 return[h,mx?d/mx:0,mx]}
function hsv2rgb(h,s,v){h=((h%360)+360)%360/60;var i=Math.floor(h),f=h-i,
 p=v*(1-s),q=v*(1-f*s),t=v*(1-(1-f)*s),a=[[v,t,p],[q,v,p],[p,v,t],[p,q,v],[t,p,v],[v,p,q]][i%6];
 return[a[0]*255,a[1]*255,a[2]*255]}

/* ---- ağ katmanı: hızlı istekleri birleştirir, ESP8266'yı boğmaz ---- */
function send(url,full){
  if(busy){pend=[url,full];return}
  busy=true;
  fetch(url).then(function(r){return r.json()}).then(function(s){S=s;paint(!!full)})
  .catch(function(){toast()})
  .then(function(){busy=false;if(pend){var p=pend;pend=null;send(p[0],p[1])}});
}
var tt;
function toast(){$('toast').className='on';clearTimeout(tt);tt=setTimeout(function(){$('toast').className=''},2200)}

/* ---- aktif modun tanımı ---- */
function mdef(){for(var i=0;i<MODES.length;i++)if(S&&MODES[i].k===S.mode)return MODES[i];return MODES[0]}
function caps(){return mdef().c}

/* ---- hedef renk (aktif sekmeye göre) ---- */
function targetHex(){
  if(!S)return '#000000';
  if(T=='fg')return S.fg;
  if(T=='bg')return S.bg;
  return S.leds[S.sel].c;
}
function pushColor(hex){
  if(!S)return;
  if(T=='fg')      send('/api/fg?hex='+hex.substr(1));
  else if(T=='bg') send('/api/bg?hex='+hex.substr(1));
  else             send('/api/led?i='+S.sel+'&hex='+hex.substr(1));
}

/* ---- renk kutusunu çiz (kaydırıcılara DOKUNMAZ) ---- */
function drawSwatch(hex){
  CUR=hex;
  $('sw').style.background=hex;
  $('hex').textContent=hex.toUpperCase();
  $('pick').value=hex;
  $('hval').textContent=Math.round($('hue').value)+'°';
  $('sval').textContent=Math.round($('sat').value)+'%';
  var p=hsv2rgb(+$('hue').value,1,1);
  $('sat').style.background='linear-gradient(90deg,#fff,'+r2h(p[0],p[1],p[2])+')';
  /* Doygunluk 0 iken renk beyazdır ve ton görünmez olur. Ton kaydırıcısı yine
     de bir değeri tutar; doygunluğu artırınca O renk gelir. Kullanıcı bunu
     bilsin diye ton kaydırıcısını soluklaştırıp "beklemede" diye işaretliyoruz. */
  var idle=Math.round($('sat').value)===0;
  $('hue').className=idle?'idle':'';
  $('hlab').innerHTML=idle?'Renk tonu <span class="park">— beklemede</span>':'Renk tonu';
}

/* ---- kaydırıcıları verilen renge göre yeniden konumlandır ---- */
function syncFromHex(hex){
  var c=h2r(hex),v=rgb2hsv(c[0],c[1],c[2]);
  if(v[1]>0.004) UH=v[0];       /* gri/beyazda ton bilgisi yok -> eskisini koru */
  if(v[2]>0.004) US=v[1]*100;   /* siyahta doygunluk bilgisi yok -> koru        */
  UV=v[2];
  $('hue').value=Math.round(UH);
  $('sat').value=Math.round(US);
  drawSwatch(hex);
}

/* ---- kaydırıcıdan renk üret ---- */
function emit(){
  if(UV<0.02)UV=1;              /* siyahtan çıkarken görünür bir renk ver */
  var n=hsv2rgb(UH,US/100,UV);
  var hex=r2h(n[0],n[1],n[2]);
  drawSwatch(hex);
  pushColor(hex);
}

/* ---- seçilen efekte göre hangi panellerin görüneceği ---- */
function applyLayout(){
  var m=mdef(), c=m.c, ind=(c==='i');
  show($('cLed'),   ind);
  show($('cColor'), c!=='');
  show($('tabs'),   c==='fb');
  show($('spwrap'), !STATICM[m.k]);
  if(ind)                        T='led';
  else if(c==='f')               T='fg';
  else if(c==='fb'&&T!=='bg')    T='fg';
  setTab();
  $('mode').value=m.k;
  $('mhint').textContent = (m.k==='off') ? 'Tüm LED’ler kapalı.' : HINT[c];
  $('ctitle').textContent = ind ? ('LED '+(S.sel+1)+' Rengi')
                                : (c==='fb' ? 'Renkler' : 'Işık Rengi');
  $('ptip').textContent = ind ? ('Sadece LED '+(S.sel+1)+' için')
                              : (T==='bg' ? 'Zemin (arka plan) rengi' : 'Ön plan / ışık rengi');
  $('selinfo').textContent = ind ? ('LED '+(S.sel+1)+' seçili') : m.n;
  return ind;
}

/* ---- genel çizim ---- */
function paint(full){
  if(!S)return;
  $('ip').textContent=S.ip;
  var ind=applyLayout();
  $('strip').className='strip'+(ind?'':' ro');
  var i,c,f,st=$('strip').children;
  var own=(mdef().c==='');   /* renklerini kendi üreten mod -> fg'yi gösterme */
  for(i=0;i<st.length;i++){
    var col='#000000';
    if(S.mode=='individual'){c=h2r(S.leds[i].c);f=S.leds[i].b/100*(S.br/100);col=r2h(c[0]*f,c[1]*f,c[2]*f)}
    else if(S.mode=='off'){col='#000000'}
    else if(own){f=S.br/100;col=r2h(90*f,90*f,96*f)}   /* nötr: gerçek renk değişken */
    else{c=h2r(S.fg);f=S.br/100;col=r2h(c[0]*f,c[1]*f,c[2]*f)}
    st[i].style.background=col;
    st[i].className='dot'+((ind&&i==S.sel)?' sel':'');
  }
  $('ledno').textContent='LED '+(S.sel+1);
  $('lbrval').textContent=S.leds[S.sel].b+'%';
  $('brval').textContent=S.br+'%';
  $('spval').innerHTML=spfmt(S.sp);
  if(full){
    $('br').value=S.br; $('sp').value=S.sp; $('lbr').value=S.leds[S.sel].b;
    syncFromHex(targetHex());
  }
}

/* ---- arayüz kurulumu ---- */
(function(){
  var sel=$('mode'),groups={},order=[];
  MODES.forEach(function(m){if(!groups[m.g]){groups[m.g]=[];order.push(m.g)}groups[m.g].push(m)});
  order.forEach(function(g){
    var og=document.createElement('optgroup');og.label=g;
    groups[g].forEach(function(m){
      var o=document.createElement('option');o.value=m.k;o.textContent=m.n;og.appendChild(o)});
    sel.appendChild(og);
  });
  sel.onchange=function(){send('/api/mode?m='+this.value,true)};

  var s=$('strip');
  for(var i=0;i<10;i++){(function(k){
    var d=document.createElement('div');
    d.className='dot';d.innerHTML='<b>'+(k+1)+'</b>';
    d.onclick=function(){if(caps()!=='i')return;send('/api/sel?i='+k,true)};
    s.appendChild(d)})(i)}

  var p=$('presets');
  PRESETS.forEach(function(c){
    var e=document.createElement('i');
    e.style.background=c;
    e.onclick=function(){syncFromHex(c);pushColor(c)};
    p.appendChild(e);
  });
})();

function setTab(){
  var t=document.querySelectorAll('.tab');
  for(var i=0;i<t.length;i++)t[i].className='tab'+(t[i].dataset.t==T?' on':'');
}
var tabs=document.querySelectorAll('.tab');
for(var i=0;i<tabs.length;i++)tabs[i].onclick=function(){
  T=this.dataset.t;setTab();syncFromHex(targetHex());paint(false);
};

$('pick').oninput=function(){syncFromHex(this.value);pushColor(this.value)};
$('hue').oninput =function(){UH=+this.value;emit()};
$('sat').oninput =function(){US=+this.value;emit()};
$('br').oninput  =function(){$('brval').textContent=this.value+'%';send('/api/cfg?b='+this.value)};
$('sp').oninput  =function(){$('spval').innerHTML=spfmt(+this.value); send('/api/cfg?s='+this.value)};
$('lbr').oninput =function(){if(!S)return;$('lbrval').textContent=this.value+'%';send('/api/led?i='+S.sel+'&br='+this.value)};

$('bAll').onclick  =function(){send('/api/ledall?hex='+CUR.substr(1)+'&br='+$('lbr').value,true)};
$('bClear').onclick=function(){send('/api/ledclear',true)};
$('rf').onclick    =function(e){e.preventDefault();boot()};

function boot(){fetch('/api/state').then(function(r){return r.json()})
  .then(function(s){S=s;setTab();paint(true)}).catch(function(){toast()})}
boot();
</script></body></html>
)HTMLPAGE";

// =========================================================================
// JSON DURUM ÇIKTISI
// =========================================================================

const char *modeKey(Mode m) {
  for (uint8_t i = 0; i < MODE_COUNT; i++) if (MODE_TABLE[i].mode == m) return MODE_TABLE[i].key;
  return "static";
}

String stateJson() {
  String j;
  j.reserve(440);
  j  = "{\"mode\":\""; j += modeKey(currentMode);
  j += "\",\"fg\":\"";  j += rgbToHex(currentR, currentG, currentB);
  j += "\",\"bg\":\"";  j += rgbToHex(bgR, bgG, bgB);
  j += "\",\"br\":";    j += globalBrightness;
  j += ",\"sp\":";      j += animSpeed;
  j += ",\"spd\":";     j += speedDelayMs;
  j += ",\"sel\":";     j += selectedLedIndex;
  j += ",\"ip\":\"";    j += WiFi.localIP().toString();
  j += "\",\"leds\":[";
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    if (i) j += ',';
    j += "{\"c\":\""; j += rgbToHex(indR[i], indG[i], indB[i]);
    j += "\",\"b\":";  j += indBr[i];
    j += '}';
  }
  j += "]}";
  return j;
}

void sendState() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", stateJson());
}

int argInt(const char *name, int def, int lo, int hi) {
  if (!server.hasArg(name)) return def;
  int v = server.arg(name).toInt();
  return constrain(v, lo, hi);
}

// =========================================================================
// SETUP
// =========================================================================

void setup() {
  Serial.begin(115200);
  Serial.println(F("\n[RGB Panel] Basliyor..."));

  Wire.begin(D2, D1);        // SDA = D2, SCL = D1
  Wire.setClock(I2C_HZ);

  pwm1.begin(); pwm1.setPWMFreq(PWM_FREQ);
  pwm2.begin(); pwm2.setPWMFreq(PWM_FREQ);

  setAllLEDs(0, 0, 0);
  fbFlush();                 // her şey kapalı başlasın

  recalcSpeed();

  // Açılış animasyonu WiFi'dan ÖNCE: güç gelir gelmez görünür.
  // WiFi bağlantısı saniyeler sürebiliyor, arkasına alınırsa karanlık boşluk olurdu.
  runBootAnimation();

  WiFiManager wifiManager;
  wifiManager.setSTAStaticIPConfig(local_IP, gateway, subnet);
  wifiManager.autoConnect(AP_NAME);

  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  // OTA'yi parolasiz birakma: agdaki herkes cihaza firmware yukleyebilir.
  // httpUpdater.setup(&server, "/update", "admin", "PAROLANI-YAZ");
  httpUpdater.setup(&server);

  // --- Sayfa ---
  server.on("/", []() { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); });
  server.on("/favicon.ico", []() { server.send(204); });

  // --- Durum ---
  server.on("/api/state", []() { sendState(); });

  // --- Ön plan rengi ---
  server.on("/api/fg", []() {
    if (server.hasArg("hex")) {
      hexToRGB(server.arg("hex"), currentR, currentG, currentB);
      if (currentMode == MODE_OFF || currentMode == MODE_STATIC || currentMode == MODE_INDIVIDUAL)
        setMode((currentR || currentG || currentB) ? MODE_STATIC : MODE_OFF);
      ledsDirty = true;
    }
    sendState();
  });

  // --- Zemin rengi ---
  server.on("/api/bg", []() {
    if (server.hasArg("hex")) { hexToRGB(server.arg("hex"), bgR, bgG, bgB); ledsDirty = true; }
    sendState();
  });

  // --- Efekt modu ---
  server.on("/api/mode", []() {
    if (server.hasArg("m")) {
      String m = server.arg("m");
      for (uint8_t i = 0; i < MODE_COUNT; i++) {
        if (m == MODE_TABLE[i].key) {
          if (MODE_TABLE[i].mode == MODE_INDIVIDUAL) enterIndividual();
          else                                       setMode(MODE_TABLE[i].mode);
          break;
        }
      }
    }
    sendState();
  });

  // --- Parlaklık / hız ---
  server.on("/api/cfg", []() {
    if (server.hasArg("b")) { globalBrightness = argInt("b", globalBrightness, 1, 100); ledsDirty = true; }
    if (server.hasArg("s")) { animSpeed = argInt("s", animSpeed, 1, SPEED_STEPS); recalcSpeed(); }
    sendState();
  });

  // --- Sadece seçimi değiştir (modu bozmaz) ---
  server.on("/api/sel", []() {
    selectedLedIndex = argInt("i", selectedLedIndex, 0, NUM_LEDS - 1);
    sendState();
  });

  // --- Tekli LED: renk ve/veya parlaklık ---
  server.on("/api/led", []() {
    selectedLedIndex = argInt("i", selectedLedIndex, 0, NUM_LEDS - 1);
    uint8_t i = selectedLedIndex;
    if (server.hasArg("hex")) {
      enterIndividual();                                   // diğerlerini kapat
      hexToRGB(server.arg("hex"), indR[i], indG[i], indB[i]);
    }
    if (server.hasArg("br")) {
      enterIndividual();
      indBr[i] = argInt("br", indBr[i], 0, 100);
    }
    ledsDirty = true;
    sendState();
  });

  // --- Seçili rengi tüm LED'lere uygula ---
  server.on("/api/ledall", []() {
    uint8_t r = 0, g = 0, b = 0;
    uint8_t br = (uint8_t)argInt("br", 100, 0, 100);
    if (server.hasArg("hex")) hexToRGB(server.arg("hex"), r, g, b);
    enterIndividual();
    for (uint8_t i = 0; i < NUM_LEDS; i++) { indR[i] = r; indG[i] = g; indB[i] = b; indBr[i] = br; }
    ledsDirty = true;
    sendState();
  });

  // --- Tüm tekli LED'leri kapat ---
  server.on("/api/ledclear", []() {
    enterIndividual();
    for (uint8_t i = 0; i < NUM_LEDS; i++) { indR[i] = 0; indG[i] = 0; indB[i] = 0; indBr[i] = 100; }
    ledsDirty = true;
    sendState();
  });

  server.onNotFound([]() {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });

  server.begin();
  Serial.print(F("[RGB Panel] Hazir -> http://"));
  Serial.print(WiFi.localIP());
  Serial.print(F("   veya   http://"));
  Serial.print(F(HOSTNAME));
  Serial.println(F(".local"));
}

// =========================================================================
// LOOP
// =========================================================================

void loop() {
  server.handleClient();
  MDNS.update();
  handleAnimations();   // RAM'deki framebuffer'a çizer
  fbFlush();            // sadece değişen kanalları I2C'ye gönderir
}
