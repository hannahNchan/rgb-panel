# Matrix Studio 64 — documentación técnica completa

> **Firmware documentado:** `MatrixStudio_WROVER_v5_3.ino`  
> **Controlador:** ESP32-WROVER-E  
> **Panel:** P3 RGB SMD 2121, `P3(2121)64X64-32S-6.0`, 64×64 píxeles, multiplexado 1/32  
> **Interfaz del panel:** HUB75/HUB75E adaptada a PxMatrix  
> **Almacenamiento:** microSD de 16 GB por SD_MMC en modo 1 bit  
> **Acceso de usuario:** `http://matrix.local` o la IP DHCP asignada por el router  
> **Biblioteca principal del display:** PxMatrix 1.8.2

---

## 1. Qué es este proyecto

Matrix Studio 64 convierte un ESP32-WROVER-E y un panel LED RGB HUB75 P3 de 64×64 en un dispositivo autónomo parecido conceptualmente a un pequeño display inteligente tipo Pixoo, pero completamente controlado por firmware propio.

El ESP32 no se limita a recibir una imagen. El firmware implementa simultáneamente:

- control físico y refresco continuo del panel HUB75;
- servidor HTTP local;
- interfaz web responsive almacenada en microSD;
- conversión y recepción de frames RGB565;
- editor de imágenes y pixel art en el navegador;
- texto enriquecido rasterizado en el navegador;
- galería persistente;
- reproducción de animaciones desde la microSD;
- administrador de archivos;
- actualización OTA del firmware desde el navegador;
- mDNS mediante `matrix.local`;
- reconexión Wi-Fi;
- reloj NTP;
- reloj digital, analógico e híbrido;
- temperatura, humedad aparente y estado meteorológico para Puebla;
- descarga experimental de imágenes públicas de Pixilart mediante `og:image`;
- doble buffer para evitar dibujar directamente sobre el frame visible.

La arquitectura está deliberadamente dividida en dos lados:

```text
┌──────────────────────── NAVEGADOR ────────────────────────┐
│                                                          │
│  Imagen / Pixel Art / Texto / Reloj / Admin / Firmware   │
│                                                          │
│  Canvas 64×64 → RGB565 → HTTP                            │
│                                                          │
└───────────────────────────┬──────────────────────────────┘
                            │ Wi-Fi
                            ▼
┌────────────────────── ESP32-WROVER-E ────────────────────┐
│ WebServer                                                 │
│ API REST local                                            │
│ mDNS                                                      │
│ microSD                                                   │
│ NTP                                                       │
│ Open-Meteo                                                │
│ OTA                                                       │
│ frameBuffer[8192]                                         │
│ PxMatrix + doble buffer                                   │
└───────────────────────────┬──────────────────────────────┘
                            │ SPI + A/B/C/D/E + LAT/OE
                            ▼
                    PANEL P3 64×64 HUB75
```

La decisión fundamental es que el navegador realiza el trabajo pesado de edición de imágenes. Una fotografía de varios megabytes no se manda completa al ESP32 para ser decodificada. El navegador la abre, la escala a 64×64, la rota si hace falta, aplica o no suavizado y finalmente genera sólo 8192 bytes RGB565.

---

# 2. Hardware utilizado

## 2.1 ESP32-WROVER-E

El módulo es un **ESP32-WROVER-E**. Espressif documenta esta familia como módulos basados en ESP32 de doble núcleo Xtensa LX6, con Wi-Fi 802.11 b/g/n, Bluetooth y PSRAM.

Dependiendo del sufijo exacto del módulo, existen variantes WROVER-E con:

| Variante | Flash | PSRAM |
|---|---:|---:|
| N4R8 | 4 MB | 8 MB |
| N8R8 | 8 MB | 8 MB |
| N16R8 | 16 MB | 8 MB |
| variantes R2 antiguas | 4/8/16 MB | 2 MB |

El firmware actual **no reserva explícitamente buffers en PSRAM**. La presencia de PSRAM es valiosa para futuras extensiones —por ejemplo animaciones precargadas, caché de previews o decodificación local—, pero en esta versión el frame principal es un array global de 8192 bytes y los archivos grandes permanecen en la microSD.

La tensión de operación del **módulo WROVER-E propiamente dicho es 3.3 V**. Si la placa de desarrollo expone una entrada `5V`, `VIN` o USB, esa placa incorpora su propia regulación hacia 3.3 V. Nunca se debe conectar 5 V directamente al pin `3V3` del módulo.

Documentación oficial:

- Espressif ESP32-WROVER-E / WROVER-IE Datasheet:  
  https://documentation.espressif.com/esp32-wrover-e_esp32-wrover-ie_datasheet_en.html
- Página de documentos oficiales de Espressif:  
  https://www.espressif.com/en/support/download/documents

---

## 2.2 Panel LED

El panel utilizado se identificó físicamente como:

```text
P3(2121)64X64-32S-6.0
```

Interpretación práctica:

- **P3**: pitch nominal de aproximadamente 3 mm entre píxeles.
- **2121**: encapsulado SMD 2121 de los LED RGB.
- **64X64**: 4096 píxeles físicos.
- **32S**: escaneo 1/32.
- **6.0**: parte de la identificación/revisión del fabricante; no se utiliza en el firmware.

En las fotografías de la PCB también se observó el controlador `DP5125D`. Para el firmware no es necesario controlar ese integrado directamente: se trabaja a través de la interfaz HUB75 que expone el panel.

PxMatrix documenta oficialmente paneles P3 de 64×64 como paneles típicos de **1/32 scan**.

Referencia:

https://github.com/2dom/PxMatrix/blob/master/README.md

---

# 3. Por qué este panel requirió una investigación especial

## 3.1 El pin serigrafiado como GND que en realidad es D

El conector del panel presenta una rareza importante.

Visto desde atrás, el conector JIN tiene aproximadamente este esquema:

```text
R1    G1
B1    GND
R2    G2
B2    E
A     B
C     GND*     ← NO ES GND: se usa como D
CLK   LAT
OE    GND
```

Ese `GND*` situado enfrente de `C` **no tiene continuidad con tierra**.

Se comprobó con multímetro.

Además, existe un caso documentado prácticamente idéntico en el issue #214 de PxMatrix:

> “64x64 P3 1/32 panel ABCE wiring”

En ese caso, el usuario descubrió que un pin marcado `GND` en la serigrafía estaba realmente conectado al buffer 74HC245 y funcionaba como la línea **D**. Al usarlo como D, el panel 64×64 empezó a funcionar correctamente.

Referencia:

https://github.com/2dom/PxMatrix/issues/214

Por eso, en este proyecto:

```text
pin que la PCB llama GND frente a C
               │
               └────────────► D
                               │
                               └── GPIO5
```

No debe conectarse a tierra.

---

# 4. JIN y JOUT

Viendo el panel desde atrás como se montó durante el desarrollo:

```text
                     flujo lógico del panel
                ─────────────────────────────►

      IZQUIERDA                              DERECHA

   ┌───────────┐                          ┌───────────┐
   │   JIN     │                          │   JOUT    │
   │  entrada  │                          │  salida   │
   └───────────┘                          └───────────┘
```

**JIN** es el conector de entrada. Ahí se conectan:

- ESP32;
- CLK;
- LAT;
- OE;
- A/B/C/D/E;
- R1;
- GND.

**JOUT** es la salida de los registros de desplazamiento del panel y normalmente sirve para encadenar otro panel.

En este proyecto se utiliza JOUT de otra forma: para crear la cadena serie que requiere PxMatrix.

---

# 5. Por qué PxMatrix necesita los puentes JOUT → JIN

Un HUB75 convencional presenta seis líneas de datos de color en paralelo:

```text
R1 G1 B1
R2 G2 B2
```

Una librería DMA típica usaría seis GPIO independientes.

PxMatrix adopta otra estrategia para ahorrar pines: convierte los seis registros de color en **una única cadena serie** y utiliza el SPI hardware del microcontrolador.

El README oficial explica que, cuando sólo se manejan pocos paneles, se pueden puentear las salidas de un registro hacia la entrada del siguiente.

Para nuestro panel, cuyos conectores tienen `R1` arriba a la izquierda, los puentes son:

```text
JOUT R1 ─────────────► JIN R2
JOUT R2 ─────────────► JIN G1
JOUT G1 ─────────────► JIN G2
JOUT G2 ─────────────► JIN B1
JOUT B1 ─────────────► JIN B2
```

`JOUT B2` queda libre.

El resultado conceptual es:

```text
ESP32 GPIO25 / MOSI
        │
        ▼
      JIN R1
        │
        ▼
      R1 shift register
        │
      JOUT R1
        │ jumper
        ▼
      JIN R2
        │
        ▼
      R2 shift register
        │
      JOUT R2
        │ jumper
        ▼
      JIN G1
        │
        ▼
      G1
        │
        ▼
      G2
        │
        ▼
      B1
        │
        ▼
      B2
```

Así sólo necesitamos **una señal de datos SPI (`MOSI`)** para alimentar los seis canales de color.

Esta es la razón de ser de los cinco jumpers. No son un workaround arbitrario: forman parte del método de cableado documentado por PxMatrix.

---

# 6. Cableado completo Panel ↔ ESP32

## 6.1 Señales de datos y control

El firmware define:

```cpp
#define P_LAT   4
#define P_OE    22
#define P_A     23
#define P_B     19
#define P_C     21
#define P_D     5
#define P_E     33
#define P_CLK   18
#define P_MOSI  25
#define P_MISO  34
#define P_SS    26
```

El cableado real es:

| Panel JIN | ESP32 | Función |
|---|---:|---|
| R1 | GPIO25 | Datos serie RGB / MOSI |
| A | GPIO23 | Bit de dirección de fila A |
| B | GPIO19 | Bit de dirección de fila B |
| C | GPIO21 | Bit de dirección de fila C |
| D* | GPIO5 | Bit de dirección D |
| E | GPIO33 | Bit de dirección E |
| CLK | GPIO18 | Reloj SPI / shift clock |
| LAT | GPIO4 | Latch / STB |
| OE | GPIO22 | Output Enable |
| GND real | GND | Referencia lógica común |

`D*` es el falso GND ya descrito.

## 6.2 Los pines P_MISO y P_SS

El firmware también define:

```cpp
#define P_MISO 34
#define P_SS   26
```

Estos **no se conectan físicamente al panel**.

PxMatrix permite pasar pines personalizados a la inicialización del SPI:

```cpp
display.begin(
    32,
    P_CLK,
    P_MOSI,
    P_MISO,
    P_SS
);
```

El panel sólo necesita de ese bus:

- CLK;
- MOSI/R1.

No devuelve datos al ESP32, por lo que MISO no forma parte de la interfaz física HUB75 de este proyecto.

---

# 7. Qué hacen A, B, C, D y E

El panel es 1/32 scan.

Cinco bits permiten representar:

```text
2^5 = 32 direcciones
```

Por eso se necesitan:

```text
A B C D E
```

Cada dirección selecciona un grupo de filas.

En un HUB75 64×64 clásico, los canales `1` y `2` permiten manejar simultáneamente las dos mitades:

```text
R1/G1/B1 → mitad superior
R2/G2/B2 → mitad inferior
```

Así, una dirección de fila puede corresponder conceptualmente a:

```text
fila n
fila n + 32
```

y después se pasa a la siguiente de las 32 direcciones.

El firmware especifica:

```cpp
display.begin(32, ...);
display.setMuxPattern(BINARY);
display.setScanPattern(LINE);
```

Esto significa:

- **32**: escaneo 1/32;
- **BINARY**: A-E representan un número binario de fila;
- **LINE**: el orden físico de los bloques coincide con el patrón lineal normal de PxMatrix.

---

# 8. Alimentación eléctrica

## 8.1 Panel

El panel debe alimentarse mediante una fuente externa regulada de **5 V DC**.

No se alimenta desde el pin 5 V del ESP32.

El panel puede consumir varios amperios dependiendo de:

- brillo;
- cantidad de píxeles encendidos;
- color;
- contenido;
- duty cycle de multiplexado.

La documentación de PxMatrix recomienda una fuente capaz de entregar corriente suficiente y enfatiza la importancia de una buena conexión de GND.

## 8.2 Tierra común

Aunque panel y ESP32 tengan fuentes distintas:

```text
GND fuente panel ────────┐
                         ├──── GND común
GND ESP32 ───────────────┘
```

Esto es obligatorio porque las señales GPIO necesitan la misma referencia de 0 V.

Sin GND común pueden aparecer:

- ghosting;
- colores incorrectos;
- píxeles aleatorios;
- datos desplazados;
- comportamiento errático.

## 8.3 ESP32

El módulo ESP32-WROVER-E funciona internamente a 3.3 V.

Si se alimenta mediante:

- USB;
- entrada 5V/VIN de la placa portadora;

el regulador de la placa genera los 3.3 V.

**No introducir 5 V directamente a `3V3`.**

---

# 9. microSD de 16 GB

El firmware usa:

```cpp
SD_MMC.begin("/sdcard", true)
```

El segundo argumento `true` selecciona **modo de 1 bit**.

En el ESP32 clásico, el slot SDMMC utilizado de esta forma emplea normalmente:

```text
CLK → GPIO14
CMD → GPIO15
D0  → GPIO2
```

La documentación oficial de Arduino-ESP32 muestra para el slot SD/MMC del ESP32:

```text
CLK = GPIO14
CMD = GPIO15
D0  = GPIO2
D1  = GPIO4
D2  = GPIO12
D3  = GPIO13
```

## 9.1 Por qué se usa 1 bit

Es una decisión muy importante.

En modo 4 bits, la microSD necesitaría `D1 = GPIO4`.

Pero nosotros usamos:

```text
GPIO4 → LAT del HUB75
```

Por eso el firmware monta la SD en modo 1 bit y únicamente necesita:

```text
GPIO14
GPIO15
GPIO2
```

De esa manera evitamos el conflicto con `LAT`.

Documentación oficial:

https://docs.espressif.com/projects/arduino-esp32/en/latest/api/sdmmc.html

## 9.2 Consideraciones de hardware

Espressif recomienda pull-ups adecuados en las líneas de SD.

GPIO2 es además un **strapping pin**, por lo que su nivel durante el arranque puede influir en el boot. En una placa WROVER con lector microSD integrado normalmente el fabricante ya diseñó estas resistencias y conexiones.

Si se construyera el lector desde cero, conviene seguir el esquema oficial de Espressif.

---

# 10. Sistema de archivos de la microSD

El firmware crea esta estructura:

```text
/
├── www/
│   ├── index.html
│   ├── tailwind.css
│   ├── app.js
│   └── .version
│
└── gallery/
    ├── images/
    │   └── *.rgb565
    │
    ├── animations/
    │   └── *.pma
    │
    ├── gifs/
    │   ├── *.gif   (original, para preview)
    │   └── *.pma   (PMA2 convertido, para el panel)
    │
    └── remote/
        ├── *.jpg
        ├── *.png
        └── *.webp
```

### `/www`

Contiene la aplicación web.

### `/gallery/images`

Contiene frames estáticos ya convertidos a RGB565.

### `/gallery/animations`

Contiene animaciones en el formato binario propio PMA.

### `/gallery/gifs`

Contiene los GIFs subidos desde la sección **GIF's** de la interfaz. Por cada GIF se guardan dos archivos con el mismo nombre base:

- `<nombre>.gif`: el GIF original, servido en `/gifs?name=<nombre>.gif` para el preview animado del historial.
- `<nombre>.pma`: la conversión a 64×64 en formato `PMA2` (delay por frame) que reproduce el panel.

El GIF se decodifica y convierte **en el navegador** con un decodificador GIF89a propio en JavaScript (LZW + disposal); el ESP32 nunca decodifica GIFs.

### `/gallery/remote`

Contiene imágenes descargadas desde fuentes remotas, actualmente la integración experimental de Pixilart.

---

# 11. La web está embebida en el `.ino`, pero se sirve desde la SD

El firmware contiene tres strings grandes en PROGMEM:

```cpp
INDEX_HTML
TAILWIND_CSS
APP_JS
```

Esto cumple dos objetivos.

Primero, el firmware siempre lleva una copia de recuperación de la aplicación.

Segundo, el navegador no necesita recibir el HTML directamente desde Flash en cada petición. Durante el arranque, `provisionWeb()` copia esos assets a:

```text
/www/
```

Después `WebServer` los sirve desde la microSD.

Arquitectura:

```text
Firmware Flash
  │
  │ primera instalación / cambio de versión
  ▼
microSD /www
  │
  │ HTTP
  ▼
Browser
```

Esto permite que, una vez operativo el Admin SD, los assets puedan modificarse sin sacar físicamente la tarjeta.

---

# 12. `WEB_ASSET_VERSION`

El firmware documentado contiene:

```cpp
const char* WEB_ASSET_VERSION = "5.5";
```

Aunque el archivo de firmware se llame `v5_3`, este valor **no es necesariamente la versión semántica completa del firmware**.

Su función es únicamente controlar si se debe reprovisionar `/www`.

`provisionWeb()` lee:

```text
/www/.version
```

Si no coincide con `WEB_ASSET_VERSION`, elimina los assets antiguos y vuelve a escribirlos.

Esto solucionó un problema importante de versiones anteriores, donde una web antigua podía permanecer en la SD aunque se cargara firmware nuevo.

---

# 13. RGB565

El panel trabaja internamente con color de 16 bits RGB565.

Distribución:

```text
rrrrr gggggg bbbbb
 5       6      5 bits
```

Total:

```text
16 bits = 2 bytes por píxel
```

Para 64×64:

```text
64 × 64 × 2 = 8192 bytes
```

Por eso existe:

```cpp
constexpr size_t FRAME_BYTES = MATRIX_W * MATRIX_H * 2;
uint8_t frameBuffer[FRAME_BYTES];
```

`frameBuffer` ocupa exactamente:

```text
8192 bytes
```

El navegador almacena primero el byte alto y luego el byte bajo:

```text
byte 0 = RGB565 bits 15..8
byte 1 = RGB565 bits 7..0
```

`applyFrame()` reconstruye:

```cpp
uint16_t c =
    ((uint16_t)frameBuffer[i] << 8)
    | frameBuffer[i + 1];
```

---

# 14. Doble buffering

Antes de incluir PxMatrix se define:

```cpp
#define PxMATRIX_DOUBLE_BUFFER true
```

Esto activa el doble buffer de PxMatrix.

Conceptualmente:

```text
Buffer A → visible
Buffer B → se está dibujando
```

Cuando termina el dibujo:

```cpp
display.showBuffer();
```

los buffers intercambian su papel.

Ventaja:

```text
sin doble buffer:
píxeles aparecen mientras se dibuja el frame

con doble buffer:
se construye frame completo fuera de pantalla
              ↓
         showBuffer()
              ↓
aparece completo de una sola vez
```

Es especialmente importante para:

- fotografías;
- reloj;
- animaciones;
- texto;
- transiciones de galería.

---

# 15. Refresco del panel y FreeRTOS

PxMatrix necesita llamar continuamente a:

```cpp
display.display(...)
```

Esto no significa “dibujar una imagen nueva”. Significa **refrescar físicamente el panel multiplexado**.

Sin estas llamadas, los LEDs dejan de recibir el ciclo de escaneo correcto.

## 15.1 Tarea dedicada

La versión documentada usa:

```cpp
TaskHandle_t matrixRefreshTaskHandle = nullptr;

constexpr uint32_t MATRIX_REFRESH_MS = 2;
constexpr uint8_t MATRIX_DRAW_TIME = 20;
```

Y crea una tarea:

```cpp
xTaskCreatePinnedToCore(
    matrixRefreshTask,
    "matrixRefresh",
    4096,
    nullptr,
    3,
    &matrixRefreshTaskHandle,
    1
);
```

Por tanto:

- stack: 4096 bytes;
- prioridad: 3;
- core: 1;
- intervalo nominal entre activaciones: 2 ms.

## 15.2 Por qué una tarea y no una ISR

Se experimentó con una ISR de hardware, pero PxMatrix ejecuta operaciones SPI dentro de `display.display()`.

Ejecutar ese camino desde una ISR en Arduino-ESP32 puede interferir con componentes que esperan contexto normal de tarea.

Por ello se usa una tarea FreeRTOS dedicada.

La tarea:

```cpp
void matrixRefreshTask(void* parameter)
```

ejecuta:

```cpp
display.display(MATRIX_DRAW_TIME);
```

y después:

```cpp
vTaskDelayUntil(...)
```

La aplicación HTTP, la SD, el reloj y la lógica de usuario quedan fuera de esa función.

---

# 16. `display.setFastUpdate(false)`

En `setupDisplay()` se utiliza:

```cpp
display.setFastUpdate(false);
```

PxMatrix dispone de un modo `FastUpdate`, pero puede ser sensible al timing dependiendo del tipo de panel.

En este proyecto se prioriza:

```text
estabilidad visual > máxima velocidad experimental
```

La estrategia es:

- `FastUpdate` desactivado;
- refresco periódico mediante tarea FreeRTOS;
- doble buffer.

---

# 17. Inicialización de PxMatrix

La instancia se crea así:

```cpp
PxMATRIX display(
    MATRIX_W,
    MATRIX_H,
    P_LAT,
    P_OE,
    P_A,
    P_B,
    P_C,
    P_D,
    P_E
);
```

Es importante observar que el constructor recibe las señales de control de filas y no los seis RGB tradicionales.

Después:

```cpp
display.begin(
    32,
    P_CLK,
    P_MOSI,
    P_MISO,
    P_SS
);
```

`32` indica **1/32 scan**.

Posteriormente:

```cpp
display.setMuxPattern(BINARY);
display.setScanPattern(LINE);
```

---

# 18. Dependencias

## 18.1 PxMatrix

Proyecto:

https://github.com/2dom/PxMatrix

`library.properties` del repositorio identifica la versión:

```text
PxMatrix LED MATRIX library
version 1.8.2
```

PxMatrix es compatible con Adafruit GFX, por eso el firmware utiliza funciones como:

```cpp
drawPixel()
drawLine()
drawCircle()
fillCircle()
fillRect()
setCursor()
setTextColor()
print()
```

## 18.2 Adafruit GFX

Es dependencia funcional de PxMatrix y aporta la API gráfica de alto nivel.

## 18.3 Arduino-ESP32

El desarrollo se realizó alrededor de Arduino-ESP32 3.x; durante las pruebas se utilizó 3.3.11.

Incluye:

- WiFi;
- WebServer;
- ESPmDNS;
- SD_MMC;
- Update;
- HTTPClient;
- WiFiClientSecure;
- FreeRTOS.

## 18.4 `hal/gpio_ll.h`

Se incluye:

```cpp
#include "hal/gpio_ll.h"
```

por compatibilidad con PxMatrix y versiones modernas del core ESP32.

---

# 19. Descripción función por función — firmware C++

A continuación se documenta cada función nombrada del firmware.

---

## 19.1 `matrixRefreshTask(void* parameter)`

### Responsabilidad

Mantener vivo el escaneo físico del HUB75.

### Operación

1. obtiene el tick actual;
2. calcula un periodo equivalente a 2 ms;
3. entra en un bucle infinito;
4. llama `display.display(20)`;
5. duerme hasta el siguiente periodo con `vTaskDelayUntil()`.

### Por qué existe

Sin esta tarea, `loop()` tendría que refrescar constantemente PxMatrix. Cualquier operación lenta —HTTP, SD, clima— podría alterar la regularidad del panel.

### Efectos laterales

Accede permanentemente al objeto global `display`.

---

## 19.2 `startMatrixRefreshTask()`

Crea una sola instancia de la tarea de refresco.

Comprueba primero:

```cpp
matrixRefreshTaskHandle != nullptr
```

para evitar duplicarla.

Devuelve:

```text
true  → tarea creada o ya existente
false → FreeRTOS no pudo crearla
```

---

## 19.3 `safePath(String p)`

Normaliza una ruta para el administrador de archivos.

Hace:

1. `trim()`;
2. fuerza `/` al inicio;
3. elimina secuencias `..`.

Ejemplo:

```text
www/app.js
↓
/www/app.js
```

Es una protección básica contra directory traversal.

No debe considerarse un sandbox de seguridad completo.

---

## 19.4 `safeName(String s)`

Sanitiza nombres de archivos.

Conserva:

```text
A-Z
a-z
0-9
_
-
.
```

Convierte espacios en `_`.

El resto se descarta.

Si queda vacío:

```text
item
```

---

## 19.5 `ensureDir(const char* p)`

Devuelve `true` si:

- el directorio ya existe;
- o `SD_MMC.mkdir()` consigue crearlo.

Se utiliza en el provisioning.

---

## 19.6 `writeText(const char* path, const char* src)`

Copia un string de PROGMEM hacia un archivo de la microSD.

No intenta copiar todo el asset de una sola vez.

Usa chunks de aproximadamente 511 bytes:

```cpp
char buffer[512];
```

y:

```cpp
memcpy_P(...)
```

Esto reduce la presión sobre RAM.

Se utiliza para:

- `index.html`;
- `tailwind.css`;
- `app.js`.

---

## 19.7 `provisionWeb()`

Es el instalador interno de la aplicación web.

### Secuencia

Crea:

```text
/www
/gallery
/gallery/images
/gallery/animations
/gallery/remote
```

Lee:

```text
/www/.version
```

Compara contra:

```cpp
WEB_ASSET_VERSION
```

Si faltan archivos o cambia la versión:

1. elimina los assets anteriores;
2. vuelve a escribir HTML/CSS/JS;
3. crea `.version`.

Esta función evita tener que retirar físicamente la SD cada vez que cambia la interfaz.

---

## 19.8 `serveFile(const String& p)`

Sirve un archivo desde SD a través de `WebServer`.

Detecta content type básico:

```text
.html → text/html
.css  → text/css
.js   → application/javascript
.json → application/json
resto → application/octet-stream
```

Después usa:

```cpp
server.streamFile(...)
```

---

## 19.9 Prototipos explícitos

El firmware declara:

```cpp
void stopClock();
void serviceClock();
void renderClock();
```

antes de ciertas funciones.

Arduino intenta generar automáticamente prototipos para `.ino`, pero archivos muy grandes que contienen raw strings HTML/CSS/JS pueden confundir ese preprocesado.

Los prototipos explícitos evitan errores como:

```text
'stopClock' was not declared in this scope
```

---

## 19.10 `stopAnim()`

Detiene la reproducción de una animación:

```cpp
animationPlaying = false;
```

y cierra:

```cpp
animFile
```

si está abierto.

---

## 19.11 `applyFrame()`

Convierte el array de 8192 bytes en píxeles de PxMatrix.

Flujo:

```text
frameBuffer
    ↓
4096 lecturas RGB565
    ↓
drawPixelRGB565()
    ↓
back buffer
    ↓
showBuffer()
```

También mide:

```cpp
lastRenderMs
```

para saber cuánto tarda la conversión completa.

---

## 19.12 `read16(File& f)`

Lee un entero `uint16_t` little-endian desde un archivo.

```text
primer byte  → bits bajos
segundo byte → bits altos
```

Se utiliza para el header de animaciones PMA.

---

## 19.13 `playAnim(const String& p)`

Abre una animación almacenada en SD.

Primero:

```cpp
stopClock();
stopAnim();
```

Por tanto, reproducir una animación reemplaza cualquier reloj activo.

Valida:

```text
magic = PMA1
width = 64
height = 64
frames > 0
```

Después:

```cpp
animationPlaying = true;
nextAnimAt = millis();
```

---

## 19.14 `serviceAnim()`

Es el scheduler de animaciones.

No hace nada si:

- no hay animación activa;
- no hay archivo;
- todavía no toca el siguiente frame.

Calcula:

```cpp
offset = 12 + frameIndex * 8192
```

lee el frame, llama `applyFrame()`, incrementa el índice y programa:

```cpp
nextAnimAt = now + animDelay;
```

Al llegar al último frame vuelve a cero.

---

# 20. Formato de animación PMA

Existen dos variantes. El firmware detecta cuál usar por el _magic_ de la cabecera y ambas se reproducen con `playAnim()` / `serviceAnim()`.

## 20.1 PMA1 (delay global, legado)

```text
Offset  Tamaño    Contenido
0       4 bytes   "PMA1"
4       2 bytes   width, little-endian
6       2 bytes   height
8       2 bytes   frame count
10      2 bytes   delay por frame en ms (uno solo, global)
12      ...       frames RGB565 consecutivos (8192 bytes cada uno)
```

Una animación de 100 frames ocupa aproximadamente:

```text
12 + 100 × 8192 ≈ 800 KB
```

## 20.2 PMA2 (delay por frame)

Usado por la sección **GIF's**. Conserva el timing individual de cada frame del GIF original.

```text
Cabecera (10 bytes):
0       4 bytes   "PMA2"
4       2 bytes   width, little-endian (64)
6       2 bytes   height (64)
8       2 bytes   frame count

Por cada frame (8194 bytes):
+0      2 bytes   delay de ESE frame en ms, little-endian
+2      8192 bytes frame RGB565
```

Todos los campos `u16` son little-endian, igual que PMA1. Los frames RGB565 usan byte alto primero por píxel, consistente con el helper `to565()` de la web y con `applyFrame()`.

Compatibilidad: las animaciones `PMA1` existentes siguen funcionando sin cambios; `playAnim()` distingue el formato por el _magic_.

## 20.3 Sección GIF's (interfaz y endpoints)

Flujo de uso:

1. El usuario elige un GIF y un modo de ajuste a 64×64: **Contener** (mantiene proporción con bordes negros), **Recortar** (llena y recorta) o **Estirar**.
2. El navegador decodifica el GIF con `ImageDecoder`, ajusta cada frame a un canvas 64×64, lo convierte a RGB565 y arma un blob `PMA2`. Se muestra un preview animado, **sin enviar nada al panel**.
3. Al **Agregar al historial** se suben dos archivos a `/gallery/gifs/` vía `/api/fs/upload`: el `.gif` original y el `.pma`. Sigue sin reproducirse en el panel.
4. El historial se muestra en un grid responsivo con el preview animado de cada GIF.
5. Al seleccionar una tarjeta se activan **Cargar GIF al panel** y **Borrar GIF**.

Endpoints añadidos (no alteran el resto de la API ni el OTA):

```text
GET    /api/gifs                 Lista los GIFs ({name, size} por cada <base>.gif)
GET    /gifs?name=<base>.gif     Sirve el GIF original (image/gif) para el preview
POST   /api/gifs/play?name=<base> Reproduce /gallery/gifs/<base>.pma en el panel
DELETE /api/gifs/delete?name=<base> Borra <base>.gif y <base>.pma de la microSD
```

La subida reutiliza `/api/fs/upload?path=/gallery/gifs/<base>.<ext>` (handler `genericUpload`).

### Búsqueda en Giphy

La tarjeta **Nuevo GIF** tiene dos pestañas: **Subir GIF** (archivo local) y **Buscar en Giphy**.

- La consulta la hace el **navegador** directamente contra `https://api.giphy.com/v1/gifs/search` (o `trending` al abrir la pestaña sin texto). El ESP32 no participa; el cliente necesita Internet.
- La API key está en `app.js` como `GIPHY_API_KEY`. Es una *beta key*: 100 llamadas por hora y hasta 50 resultados por petición.
- El grid usa la versión `fixed_width_small` (ligera). Al elegir un GIF se descarga la versión `fixed_height` como Blob y entra al mismo flujo que un archivo subido: decodificación, ajuste 64×64, PMA2 y **Agregar al historial**.
- La pestaña muestra "Powered by GIPHY", requerido por los términos de la API.

Límite: se procesan hasta `GIF_MAX_FRAMES` (300) frames por GIF para acotar memoria y espacio en la SD.

Decodificación: se hace con un decodificador GIF89a propio en JavaScript puro (incluido en `app.js`, servido desde la SD). No depende de `ImageDecoder`/WebCodecs ni de contexto seguro, por lo que funciona sobre HTTP plano en la LAN. No requiere internet ni CDN externo.

---

# 21. `parseHex565(String hex)`

Convierte:

```text
#RRGGBB
```

a RGB565.

Ejemplo conceptual:

```text
#FF0000
↓
R=255 G=0 B=0
↓
color565
```

Se utiliza para colores configurables del reloj.

---

# 22. `jsonNumber(...)`

Es un parser extremadamente ligero.

Busca literalmente:

```text
"clave":
```

y después consume caracteres numéricos.

Acepta:

```text
0-9
-
+
.
e
E
```

No es un parser JSON completo.

### Limitación importante

Si la misma clave aparece antes con un valor no numérico, por ejemplo en una sección de unidades, puede devolver el fallback.

Para robustez futura sería mejor:

- ArduinoJson;
- o localizar específicamente el objeto `"current"` antes de buscar las variables.

---

# 23. `weatherShort(int code)`

Reduce códigos meteorológicos WMO a texto de tres caracteres:

| Código | Texto |
|---|---|
| 0 | SOL |
| 1–3 | NUB |
| 45/48 | NIE |
| lluvia | LLU |
| nieve | NVE |
| tormenta | TOR |
| otros | CLM |

Esto cabe mejor en 64×64.

---

# 24. `fetchWeather()`

Obtiene clima actual desde Open-Meteo.

Endpoint actual:

```text
https://api.open-meteo.com/v1/forecast
```

Coordenadas:

```text
19.0414
-98.2063
```

correspondientes a Puebla.

Solicita:

```text
temperature_2m
relative_humidity_2m
apparent_temperature
weather_code
```

y:

```text
timezone=America/Mexico_City
```

Open-Meteo documenta oficialmente estas variables de condiciones actuales:

https://open-meteo.com/en/docs

### TLS

Se usa:

```cpp
client.setInsecure();
```

Eso cifra HTTPS, pero **no valida el certificado del servidor**.

Es cómodo para un proyecto local, pero no es la configuración más segura.

---

# 25. `weatherTask(void* parameter)`

Tarea FreeRTOS independiente para clima.

En `setup()` se crea en:

```text
Core 0
stack 8192
priority 1
```

Comprueba cada segundo si debe descargar clima.

Refresco normal:

```cpp
900000 ms
```

es decir:

```text
15 minutos
```

También puede forzarse mediante:

```cpp
forceWeatherRefresh = true;
```

---

# 26. `drawCenteredText(...)`

Centra texto según la fuente clásica de Adafruit GFX.

Asume aproximadamente:

```text
6 × textSize píxeles por carácter
```

Calcula:

```cpp
x = (64 - width) / 2;
```

No usa medición tipográfica compleja.

---

# 27. `drawWeatherIcon(...)`

Dibuja iconografía mínima directamente con primitivas GFX.

Para sol:

- círculo;
- cuatro rayos.

Para nube:

- dos círculos rellenos;
- rectángulo.

Para lluvia/tormenta:

- agrega tres píxeles bajo la nube.

Todo se dibuja sin recursos bitmap externos.

---

# 28. `dateShort(struct tm& ti)`

Convierte la fecha a:

```text
28 SEP
```

Los meses están definidos manualmente en español:

```text
ENE FEB MAR ABR MAY JUN JUL AGO SEP OCT NOV DIC
```

---

# 29. `renderClock()`

Es la función central del modo reloj.

## 29.1 Obtención de hora

Usa:

```cpp
getLocalTime(&ti, 50)
```

Si no hay hora válida, sale sin dibujar.

## 29.2 Configuración

Lee el objeto global:

```cpp
clockCfg
```

que contiene:

- enabled;
- mode;
- 12/24 h;
- segundos;
- fecha;
- temperatura;
- humedad;
- clima;
- brillo;
- colores.

## 29.3 Modos

La UI define:

```text
0 → digital
1 → digital + fecha
2 → dashboard clima
3 → analógico
4 → híbrido
```

## 29.4 Analógico

Calcula ángulos con:

```text
2π
```

y usa:

```cpp
cosf()
sinf()
```

para:

- marcas de hora;
- aguja horaria;
- minutero;
- segundero opcional.

## 29.5 Híbrido

En modo híbrido:

- reloj analógico a la izquierda;
- hora digital a la derecha;
- temperatura;
- humedad;
- estado;
- fecha.

## 29.6 Digital

Utiliza `drawCenteredText()` y primitivas GFX.

Al terminar:

```cpp
display.showBuffer();
```

---

# 30. `serviceClock()`

Decide cuándo llamar `renderClock()`.

Si los segundos están visibles:

```text
cada 200 ms
```

Si no:

```text
cada 1000 ms
```

El render cinco veces por segundo con segundos visibles es más frecuente de lo estrictamente necesario, pero reduce la posibilidad de que un cambio de segundo se perciba tarde.

---

# 31. `stopClock()`

Sólo hace:

```cpp
clockCfg.enabled = false;
```

No borra inmediatamente la pantalla.

La próxima imagen/animación sustituirá el contenido visible.

---

# 32. Hora por Internet / NTP

`connectWiFi()` llama:

```cpp
configTime(
    -21600,
    0,
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com"
);
```

`-21600` segundos equivalen a:

```text
UTC - 6 horas
```

Se eligió para Puebla.

### Limitación

Es un offset fijo.

No utiliza reglas dinámicas de zona horaria POSIX.

Si el dispositivo se mueve a otra región, el offset debe cambiarse.

---

# 33. `startMDNS()`

Crea:

```text
matrix.local
```

mediante:

```cpp
MDNS.begin("matrix");
MDNS.addService("http", "tcp", 80);
```

Si mDNS ya estaba activo:

```cpp
MDNS.end();
```

y lo reinicia.

En macOS/iOS, mDNS/Bonjour tiene soporte integrado.

---

# 34. `connectWiFi()`

Configura:

```cpp
WiFi.mode(WIFI_STA);
WiFi.setSleep(false);
WiFi.begin(...);
```

`WIFI_STA` significa que el ESP32 se conecta al router existente.

No crea su propio AP.

`WiFi.setSleep(false)` evita el power-save Wi-Fi y puede reducir latencias/jitter, a cambio de mayor consumo.

La función espera indefinidamente:

```cpp
while(WiFi.status()!=WL_CONNECTED)
```

Por tanto, si SSID o contraseña son incorrectos, el boot no continúa hasta iniciar el servidor.

---

# 35. `serviceWiFi()`

Cada 5 segundos revisa la conexión.

Si sigue conectado:

- garantiza que mDNS esté activo.

Si está desconectado:

```cpp
WiFi.disconnect();
WiFi.begin(...);
```

No reinicia el ESP32.

---

# 36. Upload de frames

## `frameUpload()`

Maneja el upload multipart del frame 64×64.

### `UPLOAD_FILE_START`

- `uploadBytes = 0`;
- `uploadOK = true`;
- detiene animación;
- detiene reloj.

### `UPLOAD_FILE_WRITE`

Copia los chunks HTTP al `frameBuffer`.

No permite exceder:

```text
8192 bytes
```

### `UPLOAD_FILE_END`

Comprueba que llegaron exactamente 8192 bytes.

### `UPLOAD_FILE_ABORTED`

Marca el upload como inválido.

---

# 37. `genericUpload()`

Backend del Admin SD.

Al inicio:

1. sanitiza ruta;
2. elimina archivo existente;
3. abre archivo nuevo.

Mientras llegan chunks:

```cpp
uploadFile.write(...)
```

Al terminar cierra el archivo.

Si se interrumpe un upload, puede quedar un archivo parcial.

---

# 38. `firmwareUpload()`

Implementa OTA.

Usa la API oficial:

```cpp
Update.begin(UPDATE_SIZE_UNKNOWN)
Update.write(...)
Update.end(true)
```

Si todo termina correctamente:

```cpp
otaSuccess = true;
```

La ruta HTTP luego responde y llama:

```cpp
ESP.restart();
```

Documentación de OTA Web Update de Arduino-ESP32:

https://docs.espressif.com/projects/arduino-esp32/en/latest/ota_web_update.html

La documentación oficial también explica que el `.bin` de OTA se obtiene mediante:

```text
Sketch → Export Compiled Binary
```

y se genera junto al sketch.

---

# 39. `listJson(String path)`

Convierte el contenido de una carpeta de SD a JSON:

```json
{
  "items": [
    {
      "name": "app.js",
      "path": "/www/app.js",
      "dir": false,
      "size": 12345
    }
  ]
}
```

La UI Admin SD consume esta respuesta.

---

# 40. `galleryJson()`

Genera:

```json
{
  "images": [],
  "animations": []
}
```

leyendo:

```text
/gallery/images
/gallery/animations
```

La UI usa esta información para poblar Galería.

---

# 41. `setupServer()`

Esta función registra todo el API HTTP.

## Assets web

| Método | Ruta | Función |
|---|---|---|
| GET | `/` | `/www/index.html` |
| GET | `/tailwind.css` | CSS local |
| GET | `/app.css` | alias de compatibilidad |
| GET | `/app.js` | aplicación JavaScript |

Se envía:

```text
Cache-Control: no-store, no-cache...
```

para evitar que una versión vieja permanezca en el navegador.

---

## Frame

### `POST /api/frame`

Upload multipart:

```text
field = frame
size = 8192 bytes
```

Si es correcto:

```json
{
  "ok": true,
  "renderMs": 123
}
```

---

## Brillo

### `POST /api/brightness?v=80`

Rango:

```text
1..255
```

Actualiza:

```cpp
display.setBrightness(...)
clockCfg.brightness
```

---

# 42. API del reloj

## `POST /api/clock/config`

Parámetros:

```text
mode
h24
seconds
date
temp
humidity
weather
brightness
bg
primary
secondary
accent
weatherColor
```

Activa el reloj y fuerza actualización meteorológica.

## `POST /api/clock/stop`

Desactiva modo reloj.

## `POST /api/clock/weather`

Solicita actualización meteorológica.

No hace la descarga dentro de la petición HTTP: sólo activa:

```cpp
forceWeatherRefresh = true;
```

La tarea de clima hará el trabajo.

## `GET /api/clock/status`

Devuelve:

```json
{
  "enabled": true,
  "mode": 2,
  "brightness": 80,
  "weatherValid": true,
  "temp": 21.3,
  "humidity": 55,
  "feels": 21.0,
  "weatherCode": 1,
  "weatherText": "NUB"
}
```

---

# 43. API de galería

## `POST /api/gallery/save-image?name=...`

Guarda el `frameBuffer` actual como:

```text
/gallery/images/nombre.rgb565
```

## `GET /api/gallery`

Lista imágenes y animaciones.

## `POST /api/gallery/show-image?name=...`

Lee un archivo de exactamente 8192 bytes.

Detiene:

- reloj;
- animación.

Después lo muestra.

## `POST /api/gallery/play-animation?name=...`

Abre `.pma` con `playAnim()`.

## `DELETE /api/gallery/delete?type=image&name=...`

Elimina imagen o animación.

---

# 44. API del administrador de SD

## `GET /api/fs/list?path=/www`

Lista archivos.

## `POST /api/fs/mkdir?path=/carpeta`

Crea carpeta.

## `DELETE /api/fs/delete?path=/archivo`

Intenta:

```cpp
SD_MMC.remove()
```

y si no:

```cpp
SD_MMC.rmdir()
```

## `POST /api/fs/upload?path=/www/app.js`

Escribe archivo mediante `genericUpload()`.

---

# 45. API OTA

## `POST /api/firmware`

Campo multipart:

```text
firmware
```

El servidor:

1. recibe el `.bin`;
2. lo escribe en la partición OTA;
3. finaliza `Update`;
4. responde;
5. reinicia.

---

# 46. Pixilart

## 46.1 No se usa una API oficial

El firmware **no utiliza una API pública oficial de Pixilart**, porque durante el desarrollo no se encontró documentación estable de una API de galería para terceros.

La integración es experimental.

## 46.2 `POST /api/pixilart/import?url=...`

Sólo acepta URLs que empiecen con:

```text
https://www.pixilart.com/
https://es.pixilart.com/
```

Después:

1. descarga el HTML;
2. localiza `property="og:image"`;
3. obtiene el `content=...`;
4. descarga esa imagen;
5. detecta `.jpg`, `.png` o `.webp`;
6. guarda en `/gallery/remote`;
7. devuelve una URL local.

Respuesta:

```json
{
  "ok": true,
  "localUrl": "/remote?name=pixilart_12345.png"
}
```

## 46.3 `/remote`

Sirve esas imágenes al navegador.

El navegador es quien posteriormente las escala a 64×64.

## 46.4 Limitaciones

Esta integración puede romperse si Pixilart modifica:

- HTML;
- metatags;
- restricciones;
- URLs CDN.

Además, que una imagen sea pública no significa que sea libre de derechos. Debe respetarse la licencia del autor.

---

# 47. `/api/status`

Entrega estado general:

```json
{
  "host": "matrix.local",
  "ip": "192.168.x.x",
  "rssi": -45,
  "sdTotalMB": 15000,
  "sdUsedMB": 25,
  "lastRenderMs": 123,
  "clockEnabled": false
}
```

La página Panel lo utiliza para diagnóstico.

---

# 48. `setupDisplay()`

Inicializa el panel.

Orden:

```cpp
display.begin(32,...)
display.setMuxPattern(BINARY)
display.setScanPattern(LINE)
display.setFastUpdate(false)
display.setBrightness(80)
display.clearDisplay(false)
display.clearDisplay(true)
startMatrixRefreshTask()
```

Los dos `clearDisplay(...)` preparan los dos buffers.

---

# 49. `setupSD()`

Ejecuta:

```cpp
SD_MMC.begin("/sdcard", true)
```

Si la SD no monta:

```cpp
while(true) {
    delay(1000);
}
```

Es decir: el boot se detiene.

Después:

```cpp
provisionWeb();
```

---

# 50. `setup()`

El boot se divide en cinco fases.

```text
[1/5] Display
[2/5] microSD
[3/5] Wi-Fi
[4/5] HTTP
[5/5] clima
```

Esto es deliberado: si el sistema no inicia, el Serial Monitor indica exactamente qué etapa alcanzó.

Al terminar imprime:

```text
mDNS: http://matrix.local
IP: http://192.168.x.x
```

---

# 51. `loop()`

El loop principal es muy pequeño:

```cpp
serviceAnim();
serviceClock();

if (WiFi.status() == WL_CONNECTED)
    server.handleClient();

serviceWiFi();
```

El refresco HUB75 no aparece aquí porque está delegado a la tarea FreeRTOS.

El clima tampoco, porque tiene otra tarea.

---

# 52. Frontend: arquitectura

El navegador recibe tres archivos:

```text
index.html
tailwind.css
app.js
```

El archivo llamado `tailwind.css` es actualmente un **bundle CSS local de utilidades y componentes diseñado para esta aplicación**.

No ejecuta el CDN de Tailwind y no depende de Internet para estilizar Matrix Studio.

La ventaja es que `matrix.local` sigue siendo utilizable aunque el navegador no tenga salida a Internet.

---

# 53. Funciones JavaScript

## `toast(msg, type, ms)`

Muestra una notificación no bloqueante.

No usa:

```text
alert()
```

Crea un elemento temporal en `#toastHost`.

---

## `showLoader()`, `updateLoader()`, `hideLoader()`

Controlan el overlay de transferencia.

Se usan para mostrar:

- progreso de upload;
- render;
- firmware;
- importación;
- guardado.

---

## `modal(...)`

Construye un `<dialog>` HTML estilizado.

Sustituye:

- `prompt()`;
- `confirm()`;
- diálogos nativos.

Devuelve una `Promise`.

---

## `black(ctx)`

Limpia un Canvas 64×64 a negro.

---

## `to565(canvas)`

Lee:

```cpp
getImageData(0,0,64,64)
```

y convierte RGBA 8/8/8/8 a RGB565 5/6/5.

El resultado es:

```text
Uint8Array(8192)
```

---

## `uploadXHR(...)`

Utiliza `XMLHttpRequest` en lugar de `fetch()` para uploads porque XHR expone:

```javascript
xhr.upload.onprogress
```

Eso permite una barra de progreso real.

---

## `sendCanvas(canvas)`

Cadena:

```text
Canvas
  ↓
to565()
  ↓
Blob
  ↓
POST /api/frame
```

Después muestra `renderMs` si el servidor lo devuelve.

---

## `sendRawFrame(bytes)`

Manda un frame ya convertido sin volver a rasterizar.

---

## `saveFrame(bytes)`

Primero muestra/envía el frame y luego abre un dialog para elegir nombre.

Finalmente llama:

```text
/api/gallery/save-image
```

---

## `setBrightness(v)`

Sincroniza:

- slider global;
- slider de Panel;
- slider del reloj.

Usa un pequeño debounce de 100 ms para evitar cientos de peticiones mientras se arrastra.

---

# 54. Editor de imagen

## `drawImage()`

El navegador calcula:

- ancho/alto original;
- rotación;
- `cover` o `contain`;
- escala;
- centrado.

Luego usa:

```javascript
ctx.drawImage(...)
```

### Suavizado

La opción:

```text
Suavizado al reducir
```

controla:

```javascript
imageSmoothingEnabled
```

Activado:

- fotografías;
- antialiasing.

Desactivado:

- pixel art;
- bordes duros;
- nearest-neighbor visual.

---

# 55. Editor Pixel Art

## `ppos(e)`

Convierte coordenadas de ratón/touch desde el tamaño visual del Canvas a coordenadas lógicas:

```text
0..63
```

## `pushHist()`

Guarda hasta 30 estados para Undo.

Cada estado es un:

```javascript
ImageData
```

de 64×64.

## `paintAt(x,y)`

Pinta:

- color seleccionado;
- o negro en modo goma.

Tamaños:

```text
1×1
2×2
3×3
```

---

# 56. Texto enriquecido

El editor no es un `<textarea>` convencional.

Es:

```html
<div contenteditable="true">
```

Esto permite que diferentes fragmentos del mismo texto tengan estilos distintos.

---

## `selectionInsideEditor()`

Comprueba que la selección actual del navegador pertenece realmente al editor.

---

## `applySelectionStyle()`

Guarda el `Range` seleccionado.

Después crea:

```html
<span style="
  color: ...;
  font-size: ...;
  font-family: ...;
">
```

y mueve **sólo el contenido seleccionado** dentro de ese span.

Esto permite:

```text
HOLA MUNDO
^^^^       rojo 10px
     ^^^^^ azul 14px
```

sin aplicar el estilo a todo el texto.

---

## `collectRuns(...)`

Recorre recursivamente el DOM del editor.

Transforma el HTML enriquecido en segmentos:

```javascript
{
  text,
  style: {
    color,
    size,
    font
  }
}
```

También convierte `<br>`/bloques en saltos de línea.

---

## `renderRichText()`

Rasteriza esos segmentos en un Canvas de 64×64.

Antes de cada carácter:

1. mide su ancho con `measureText()`;
2. comprueba si rebasa x=63;
3. si rebasa, hace wrap;
4. aumenta `y`;
5. aplica color/fuente/tamaño.

El texto deja de dibujarse cuando rebasa 64 píxeles verticales.

### Texto nítido (sin suavizado)

El switch **Texto nítido** (activado por defecto) evita los píxeles grises del antialiasing, que en un panel RGB se ven como sombra alrededor de las letras.

En ese modo cada carácter se dibuja como máscara blanca en un canvas auxiliar, se umbraliza el alfa (≥120 encendido, resto apagado) y los píxeles encendidos se pintan con el **color exacto** del segmento. El resultado tiene solo los colores elegidos, sin tonos intermedios. Con el switch apagado se usa el texto suavizado normal del navegador.

El color por defecto de un texto sin estilo es `#ffffff`. `collectRuns()` toma el color del `style` inline de cada elemento (los `<span>` que crea **Aplicar a selección**) y no del color computado de la página.

El ESP32 **no interpreta fuentes ni spans**: recibe el resultado final rasterizado RGB565.

---

# 57. Preview del reloj en JavaScript

## `clockCfg()`

Recolecta todos los controles de la UI y genera un objeto de configuración.

## `renderClockPreview()`

Dibuja una aproximación del reloj en Canvas antes de enviarla al ESP32.

Usa la hora del navegador para el preview.

El reloj físico, en cambio, usa NTP desde el ESP32.

## `loadClockStatus()`

Consulta:

```text
GET /api/clock/status
```

y actualiza:

- estado activo/inactivo;
- temperatura;
- humedad;
- texto del clima;
- brillo.

---

# 58. Biblioteca local

`presets` define varias piezas originales:

- Corazón;
- Carita;
- Estrella;
- Nebulosa;
- Flor;
- Robot.

Cada una se genera con primitivas Canvas.

## `loadPreset(p)`

Dibuja el preset, copia sus píxeles al editor Pixel Art y cambia a esa pestaña.

---

# 59. `loadGallery()`

Consulta `/api/gallery`.

Genera cards para:

- imágenes;
- animaciones.

Cada card puede:

- mostrar/reproducir;
- eliminar.

La confirmación de borrado utiliza `<dialog>`.

---

# 60. `listFiles()`

Consulta el administrador de SD y genera una tabla con:

```text
nombre
tipo
tamaño
eliminar
```

---

# 61. `loadStatus()`

Consulta periódicamente `/api/status`.

Actualiza:

- IP;
- RSSI Wi-Fi;
- espacio SD;
- último render.

Se ejecuta al cargar y luego cada 10 segundos.

---

# 62. Flujo completo al mandar una fotografía

```text
Usuario
 │
 ├─ selecciona JPG/PNG/WebP
 │
 ▼
Browser Image()
 │
 ▼
drawImage()
 │
 ├─ crop/contain
 ├─ rotation
 └─ smoothing
 │
 ▼
Canvas 64×64 RGBA
 │
 ▼
to565()
 │
 ▼
8192 bytes
 │
 ▼
XMLHttpRequest
 │
 ▼
POST /api/frame
 │
 ▼
frameUpload()
 │
 ▼
frameBuffer
 │
 ▼
applyFrame()
 │
 ▼
back buffer PxMatrix
 │
 ▼
showBuffer()
 │
 ▼
panel HUB75
```

---

# 63. Flujo completo del reloj

```text
boot
 │
 ├─ Wi-Fi
 │
 ├─ configTime/NTP
 │
 └─ weatherTask
      │
      ▼
   Open-Meteo
      │
      ▼
 weather variables

Usuario → /api/clock/config
             │
             ▼
         clockCfg
             │
             ▼
       serviceClock()
             │
             ▼
        renderClock()
             │
             ▼
       display buffer
             │
             ▼
          HUB75
```

---

# 64. Flujo OTA

```text
Arduino IDE
 │
 └─ Sketch → Export Compiled Binary
               │
               ▼
          firmware.bin
               │
               ▼
 Browser Matrix Studio
               │
               ▼
 POST /api/firmware
               │
               ▼
        Update.write()
               │
               ▼
        Update.end(true)
               │
               ▼
          ESP.restart()
```

---

# 65. Seguridad

El proyecto está diseñado como dispositivo local de laboratorio/hogar.

Actualmente **no existe autenticación**.

Cualquier dispositivo que pueda entrar a la misma LAN y acceder a `matrix.local` podría potencialmente:

- mostrar contenido;
- cambiar brillo;
- administrar archivos;
- borrar archivos;
- subir firmware OTA.

Para un despliegue comercial debería añadirse al menos:

- autenticación;
- protección de Admin/OTA;
- CSRF protection;
- firma/verificación del firmware;
- TLS o segmentación de red.

---

# 66. TLS

Tanto Open-Meteo como Pixilart utilizan:

```cpp
WiFiClientSecure
```

pero también:

```cpp
setInsecure()
```

Esto significa:

- tráfico cifrado;
- certificado remoto no validado.

Un atacante con capacidad de MITM podría sustituir respuestas.

Para producto final convendría cargar certificados CA o usar certificate bundle.

---

# 67. Rendimiento y memoria

## Frame estático

```text
8192 B
```

## 100 imágenes RGB565

```text
≈ 800 KB
```

## 1000 imágenes

```text
≈ 8 MB
```

Una microSD de 16 GB es enorme en relación con frames 64×64.

Teóricamente podría almacenar del orden de millones de frames estáticos, aunque en la práctica:

- filesystem;
- nombres;
- previews;
- animaciones;
- otros assets;

reducirán esa cifra.

---

# 68. Qué papel juega la PSRAM

El WROVER tiene PSRAM, pero el código actual no hace:

```cpp
ps_malloc()
heap_caps_malloc(...SPIRAM...)
```

Por tanto la PSRAM todavía no es un elemento central de esta arquitectura.

Podría aprovecharse posteriormente para:

- cachear animaciones;
- buffer de GIF;
- thumbnails;
- transcodificación;
- playlists;
- canvas adicionales;
- precarga de frames desde SD.

---

# 69. Razón de que el navegador haga el procesamiento

Decodificar JPEG, PNG, WebP o GIF dentro del ESP32 es posible, pero consume:

- RAM;
- CPU;
- tiempo;
- complejidad.

Un teléfono o navegador ya dispone de decodificadores optimizados.

Por eso:

```text
archivo pesado → Browser
frame ligero   → ESP32
```

es una arquitectura más eficiente.

---

# 70. Consideraciones sobre el parpadeo

PxMatrix advierte que el refresco y número de niveles de color afectan la tasa de actualización visible.

La documentación de PxMatrix menciona:

- más niveles de color → menor margen temporal;
- revisar alimentación;
- revisar GND;
- aumentar rendimiento si aparece flicker;
- `setMuxDelay()` para multiplexores lentos.

En este firmware el enfoque elegido es:

```text
doble buffer
+ FastUpdate false
+ tarea dedicada de refresh
+ WiFi sleep false
```

Si todavía hubiera flicker, los parámetros a revisar incluyen:

- `MATRIX_REFRESH_MS`;
- `MATRIX_DRAW_TIME`;
- brillo;
- calidad de 5 V;
- GND;
- longitud de cable;
- nivel lógico 3.3 V → panel;
- versión de PxMatrix.

---

# 71. Posible level shifter

El ESP32 entrega lógica de aproximadamente 3.3 V.

El panel se alimenta a 5 V.

En este montaje funciona directamente, pero algunos HUB75 son menos tolerantes.

Para un producto más robusto puede utilizarse:

```text
74AHCT245
74HCT245
```

para convertir las señales de 3.3 V a niveles compatibles con lógica 5 V.

---

# 72. Problemas conocidos / deuda técnica

## 72.1 Parser meteorológico mínimo

`jsonNumber()` no reemplaza a un parser JSON real.

## 72.2 Timezone fija

`configTime(-21600,...)` está codificado para UTC-6.

## 72.3 Pixilart por scraping

No es API contractual.

## 72.4 OTA sin autenticación

Debe protegerse en despliegue real.

## 72.5 Administrador SD sin sandbox fuerte

`safePath()` reduce `..`, pero no pretende ser un sistema de permisos.

## 72.6 String dinámico

El uso intensivo de `String` puede fragmentar heap en ejecuciones extremadamente prolongadas.

## 72.7 Coordenadas duplicadas

Existen constantes:

```cpp
PUEBLA_LAT
PUEBLA_LON
```

pero `fetchWeather()` actualmente usa las coordenadas directamente dentro de la URL.

Sería preferible construir la URL con las constantes.

## 72.8 Comentario antiguo en `setupDisplay()`

El comentario menciona “timer hardware”, pero la versión v5_3 real utiliza una **tarea FreeRTOS**, no una ISR/timer de hardware.

El comportamiento del código tiene prioridad sobre ese comentario.

---

# 73. Diagnóstico por Serial Monitor

Velocidad:

```text
115200
```

Arranque esperado:

```text
=== Matrix Studio boot ===
[1/5] Inicializando display...
Refresh HUB75: tarea FreeRTOS cada 2 ms
[1/5] Display OK
[2/5] Montando microSD...
Web assets OK: v...
[2/5] microSD OK
[3/5] Conectando Wi-Fi...
[3/5] Wi-Fi OK: 192.168.x.x
[4/5] Iniciando servidor HTTP...
[4/5] Servidor HTTP OK
[5/5] Iniciando tarea de clima...
[5/5] Clima OK
=== Matrix Studio listo ===
mDNS: http://matrix.local
IP: http://192.168.x.x
```

Si se detiene después de una etapa, esa etapa indica dónde investigar.

---

# 74. Si `matrix.local` no abre

Primero probar la IP que aparece en Serial Monitor.

Ejemplo:

```text
http://192.168.1.74
```

Si IP funciona pero `matrix.local` no:

- problema de mDNS/Bonjour;
- red con aislamiento multicast;
- cliente sin soporte mDNS.

Si tampoco abre la IP:

- verificar que aparece `[4/5] Servidor HTTP OK`;
- verificar misma LAN;
- verificar firewall/router;
- verificar que el ESP32 no se reinicia.

---

# 75. Si la web aparece sin CSS

Comprobar:

```text
http://matrix.local/tailwind.css
```

y:

```text
http://matrix.local/app.js
```

La web utiliza cache busting y `Cache-Control: no-store`.

`/app.css` existe como alias de compatibilidad hacia `tailwind.css`.

---

# 76. Si el panel sólo muestra rojo o media pantalla

Eso fue exactamente lo que ocurrió antes de instalar los puentes PxMatrix.

Revisar:

```text
GPIO25 → JIN R1

JOUT R1 → JIN R2
JOUT R2 → JIN G1
JOUT G1 → JIN G2
JOUT G2 → JIN B1
JOUT B1 → JIN B2
```

Con multímetro y alimentación apagada, comprobar continuidad.

---

# 77. Si faltan filas

Verificar:

```text
A → GPIO23
B → GPIO19
C → GPIO21
D → GPIO5
E → GPIO33
```

Recordar que D es el pin mal etiquetado como GND.

---

# 78. Si los colores o píxeles aparecen corruptos

Revisar:

- GND común;
- fuente 5 V;
- cables CLK/LAT/OE;
- puentes JOUT/JIN;
- calidad física de Dupont;
- frecuencia/timing;
- posible level shifter.

---

# 79. Referencias técnicas

## Espressif

ESP32-WROVER-E / WROVER-IE Datasheet:

https://documentation.espressif.com/esp32-wrover-e_esp32-wrover-ie_datasheet_en.html

Arduino-ESP32 SD_MMC:

https://docs.espressif.com/projects/arduino-esp32/en/latest/api/sdmmc.html

OTA Web Update:

https://docs.espressif.com/projects/arduino-esp32/en/latest/ota_web_update.html

ESP-IDF SDMMC host:

https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-reference/peripherals/sdmmc_host.html

## PxMatrix

Repositorio:

https://github.com/2dom/PxMatrix

README / cableado / scan / mux:

https://github.com/2dom/PxMatrix/blob/master/README.md

Issue del falso GND → D:

https://github.com/2dom/PxMatrix/issues/214

## Open-Meteo

Documentación API:

https://open-meteo.com/en/docs

---

# 80. Resumen final de diseño

La razón de ser de este firmware puede resumirse así:

```text
1. El panel es un HUB75 P3 64×64 1/32.
2. El fabricante etiquetó mal uno de los pines: el supuesto GND es D.
3. PxMatrix serializa los seis canales RGB utilizando JOUT→JIN.
4. Sólo R1 recibe directamente MOSI desde GPIO25.
5. A-E seleccionan una de 32 direcciones de filas.
6. CLK desplaza los datos.
7. LAT los presenta a los drivers.
8. OE controla la salida/brillo.
9. Una tarea FreeRTOS mantiene el multiplexado continuamente.
10. El navegador convierte imágenes a RGB565 64×64.
11. El ESP32 recibe únicamente 8192 bytes por frame.
12. La microSD aloja web, imágenes, animaciones y archivos remotos.
13. mDNS evita depender de recordar la IP.
14. NTP proporciona la hora.
15. Open-Meteo proporciona clima para Puebla.
16. OTA permite actualizar firmware sin desmontar el equipo.
17. El doble buffer evita presentar frames parcialmente dibujados.
18. La aplicación web es la capa de creación; el ESP32 es la capa de ejecución.
```

El resultado es un sistema en el que el panel LED deja de ser simplemente una matriz conectada a un microcontrolador y se convierte en un **dispositivo de red autónomo**, con almacenamiento persistente, interfaz gráfica, API local, reloj, clima, OTA y herramientas de contenido.

---

# 81. Mapa rápido definitivo del cableado

```text
ESP32-WROVER-E                     PANEL JIN

GPIO25  MOSI  ───────────────────► R1

GPIO23        ───────────────────► A
GPIO19        ───────────────────► B
GPIO21        ───────────────────► C
GPIO5         ───────────────────► D*
GPIO33        ───────────────────► E

GPIO18  CLK   ───────────────────► CLK
GPIO4   LAT   ───────────────────► LAT
GPIO22  OE    ───────────────────► OE

GND           ───────────────────► GND REAL


PANEL JOUT                          PANEL JIN

R1  ─────────────────────────────► R2
R2  ─────────────────────────────► G1
G1  ─────────────────────────────► G2
G2  ─────────────────────────────► B1
B1  ─────────────────────────────► B2
B2  ─────────────────────────────► sin conectar


D* = pin que la PCB imprime como GND frente a C,
     verificado con multímetro como NO-GND.
```

---

# 82. Mapa de alimentación

```text
FUENTE 5 V REGULADA
      │
      ├────────────► +5 V PANEL
      │
      └────────────► opcional: entrada 5V/VIN de la placa ESP32,
                     sólo si la placa portadora lo admite

GND FUENTE
      │
      ├────────────► GND PANEL
      │
      └────────────► GND ESP32
```

Nunca:

```text
5 V ─────X────► pin 3V3 del ESP32
```

---

# 83. Estado del proyecto

Esta documentación describe el firmware `MatrixStudio_WROVER_v5_3.ino` tal como está implementado.

Si se modifica:

- pinout;
- biblioteca;
- formato PMA;
- endpoints;
- tarea de refresco;
- modo SD;
- almacenamiento;
- reloj;
- API meteorológica;

este README debe actualizarse junto con el `.ino`.

La mejor práctica es considerar ambos archivos como una sola unidad:

```text
MatrixStudio_WROVER_v5_3.ino
README.md
```

El `.ino` define el comportamiento.

El README explica por qué ese comportamiento existe.
