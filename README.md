# emote-display-firmware

Receptor de PNG para **Adafruit MatrixPortal ESP32-S3 + HUB75 de 64×64**, compatible con gentle-emote.

## Estado de verificación

- Prueba inicial de texto, barras RGB y animación cargada y confirmada visualmente por el usuario.
- Receptor Wi-Fi/HTTP/PNG implementado y compilado. Tests nativos del parser y del decodificador PNG, incluido un PNG real del emisor.
- Receptor inicial cargado, flash verificada y Wi-Fi configurado por USB. Actualización con brillo reducido y autorrotación cargada y flash verificada. `/health` confirma `brightness: 45`, `accelerometer: true`, `orientationCalibrated: true` y recepción de nuevos frames. Detección de giro y autorrotación confirmadas visualmente por el usuario. Endpoint comprobado: `http://192.168.1.227/emote` (IP DHCP, puede cambiar).
- Prueba simultánea desde `ExternalDisplays`: 11 respuestas HTTP 200 del simulador y 11 de la placa; `/health` confirmó 11 frames. Pendiente de confirmación visual del avatar por el usuario.
- Rechazos comprobados sobre la placa: PNG inválido y dimensiones incorrectas → 422, cuerpo excesivo → 413, versión incorrecta → 400; contador de frames sin cambios.
- Placa detectada como USB `239A:8125`, puerto `/dev/cu.usbmodem14101` (puede cambiar). En modo de descarga aparece `303A:1001`.

### Ajustes actuales

Actualización cargada y flash verificada: brillo al **25%** (`64/255`), margen superior de **3 px** y profundidad de color de **6 bits/canal** en Protomatter. `/health` confirma los tres valores, acelerómetro activo, referencia conservada y recepción de frames tras reiniciar. El usuario observó temblor/parpadeo tras esta actualización. Se ha cargado y verificado la vuelta a **4 bits/canal**, conservando brillo 25% y margen 3 px, para aislar la profundidad de color como posible causa. `/health` confirma `bitDepth: 4`, `brightness: 64`, `topOffset: 3`, acelerómetro activo y recepción de frames; pendiente de confirmación visual de estabilidad.

## Reloj autónomo (actualización local, aún no cargada)

El panel puede mostrar la hora sin el agente ni el PC. Usa NTP sobre el Wi-Fi ya configurado y la zona **Europe/Madrid**, con cambio automático de horario de verano/invierno; no cambia credenciales ni añade comandos USB o campos HTTP.

- Al arrancar, conserva los diagnósticos hasta disponer de una hora válida. Sin un PNG previo, muestra entonces el reloj.
- Un PNG válido ocupa la pantalla durante **10 segundos** desde su recepción. Al vencer ese plazo aparece el reloj si hay hora válida; si no, conserva el PNG. Otro PNG válido recupera la pantalla inmediatamente.
- Un PNG rechazado no reemplaza la imagen guardada, no incrementa el contador ni reinicia esos 10 segundos. Si no llega tráfico por pausa del emisor, suspensión del PC o pérdida de red, también puede activarse el reloj.
- Tras sincronizar, sigue contando sin Wi-Fi mientras tenga alimentación. Una pérdida completa de alimentación requiere sincronización de nuevo. NTP y las lecturas de hora no bloquean el bucle esperando a la red.
- Formato `dd/mm/yyyy`, hora de 24 h, horas/minutos grandes, segundos mayores y dos personajes caminando, con refresco nominal de 1 Hz. Se conservan brillo, rotación y margen superior; los pies y el camino caben en las 61 filas lógicas visibles, sin otro framebuffer.
- La pantalla informativa de Wi-Fi tiene prioridad. Al cerrarla muestra el PNG todavía vigente o el reloj, según corresponda.

**La implementación y sus pruebas locales no equivalen a una validación física.** Esta actualización aún no se ha cargado: quedan por comprobar NTP real, visibilidad y estabilidad en la placa. Entrar en bootloader o subir firmware es un paso separado.

## Continuar el reloj desde el Mac

La [guía de continuación en macOS](docs/continue-on-macos.md) reúne la rama, el estado verificado, la compilación sin carga y los requisitos pendientes antes de escribir. Para el candidato que integra reloj y contadores de emisores, seguir la [preparación de actualización en macOS](docs/macos-firmware-upgrade.md): binario congelado, herramientas oficiales y recuperación local sin cifrar aceptada, ignorada por Git y con permisos privados (puede contener credenciales Wi-Fi). El preflight ROM de lectura y resets ya está autorizado, con resultado pendiente; la copia consistente y utilizable aún debe validarse. No hay autorización de stub, carga ni restauración. El firmware del reloj aún no está instalado; no aplicar automáticamente el upload genérico de la sección siguiente a esta actualización.

## Compilar y cargar

Mac Intel con macOS 14.8.7, Apple Command Line Tools y Python 3.13 de Homebrew. No modificar el Python 3.9 del sistema.

```sh
cd /Users/santiagoperez/Development/emote-display-firmware
/usr/local/bin/python3.13 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
.venv/bin/pio run
.venv/bin/pio device list
.venv/bin/pio run -t upload --upload-port /dev/cu.usbmodem14101
```

Si falla la entrada automática: desconectar USB, mantener **BOOT** al reconectar y soltar **BOOT**. Enumerar puertos de nuevo y usar el de `USB JTAG/serial debug unit`, VID:PID `303A:1001`. Cerrar el monitor serie antes de cargar o provisionar.

Si tras cargar permanece en modo de descarga, pulsar RESET sin BOOT. En esta sesión fue necesario un reset watchdog mediante esptool para arrancar la aplicación; no se reescribió flash con ese reset.

PlatformIO gestiona las herramientas en `~/.platformio`, sin Arduino IDE ni ESP-IDF separado. Versiones fijadas: PlatformIO 6.1.19, Espressif32 6.12.0 / Arduino ESP32 2.0.17, Protomatter 1.7.1, GFX 1.13.0, LIS3DH 1.3.0, PNGdec 1.1.7 (commit fijado en `platformio.ini`). Memoria estática aproximada: 154 KB RAM y 849 KB flash; no incluye memoria asignada en ejecución.

## Configurar Wi-Fi sin guardar secretos en el repo

**Cambiar el SSID o la contraseña no requiere recompilar ni actualizar el firmware.** Conectar la placa al equipo con un cable USB de datos, ejecutando normalmente este firmware: **no pulsar BOOT ni entrar en modo de carga**. Cerrar antes cualquier monitor serie u otro programa que use el puerto.

El panel muestra `SET WIFI / VIA USB` si aún no hay credenciales, pero el asistente también sirve para sustituir una red ya guardada, incluso sin conexión a la red anterior.

### macOS

Desde la raíz del repositorio, con el entorno `.venv` preparado:

```sh
# Identificar el puerto de la MatrixPortal:
.venv/bin/python -m serial.tools.list_ports -v

# Sustituir el puerto si es diferente:
.venv/bin/python scripts/provision_wifi.py --port /dev/cu.usbmodem14101
```

### Windows (PowerShell)

Instalar Python 3 y abrir una terminal en la raíz del repositorio. Si solo se desea cambiar el Wi-Fi, basta con Python y pyserial; no hace falta instalar PlatformIO ni el compilador:

```powershell
# Solo si aún no existe el entorno:
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install pyserial==3.5

# Identificar el puerto de la MatrixPortal:
.\.venv\Scripts\python.exe -m serial.tools.list_ports -v

# Sustituir COM3 por el puerto detectado:
.\.venv\Scripts\python.exe scripts\provision_wifi.py --port COM3
```

También se puede consultar el puerto en el Administrador de dispositivos, sección **Puertos (COM y LPT)**. No hay que activar el entorno virtual para usar estos comandos. La conexión USB y el asistente se han probado en macOS; Windows queda pendiente de validación física.

### Guardado y nueva IP

1. El asistente pide el **SSID** y la **contraseña**, que se introduce oculta.
2. Envía los datos por USB, los guarda en **NVS en la placa** y la reinicia. Persisten al desconectar la alimentación.
3. Usar una red **2,4 GHz**. Solo se admiten redes personales/abiertas, no Wi-Fi Enterprise ni portales cautivos.
4. Tras conectar, pulsar **UP** para consultar la nueva IP. La dirección se muestra en dos líneas que deben unirse.
5. Actualizar la URL del destino ESP32 en `.pi/extensions/gentle-emote/config.json`, por ejemplo `http://192.168.1.227/emote`, y ejecutar **`/reload`** en el plugin. El equipo y la placa deben estar en la misma red accesible.

No se guardan las credenciales en archivos del proyecto ni argumentos de shell. NVS no está cifrada: quien tenga acceso físico a la placa puede extraerlas. Para cambiar de red otra vez, repetir el asistente.

```sh
.venv/bin/pio device monitor --port /dev/cu.usbmodem14101 --baud 115200
```

Tras conectar aparece `READY / SEND PNG`; el monitor imprime la IP. Reintenta la conexión si se pierde. Publica `gentle-matrix.local` por mDNS; no resolvió en la prueba de este Mac, así que se utilizó la IP mostrada (`192.168.1.227`). Conviene reservar esa IP en DHCP o actualizar la configuración si cambia. No hay portal ni AP de configuración.

## Mantener el simulador y la placa a la vez

En `.pi/extensions/gentle-emote/config.json` del proyecto emisor:

```json
{
  "externalDisplay": {
    "enabled": true,
    "timeoutMs": 1000,
    "targets": [
      { "enabled": true, "url": "http://127.0.0.1:8000/emote" },
      { "enabled": true, "url": "http://192.168.1.227/emote" }
    ]
  }
}
```

Recargar gentle-emote con `/reload` **después de configurar el Wi-Fi** y probar `/emote demo`. Ambos destinos tienen colas y timeouts independientes; uno desconectado no detiene al otro. El emisor también envía PNGs si el terminal solo admite ASCII: el texto se queda en el terminal y la selección gráfica sigue en paralelo. Si se selecciona un set exclusivamente ASCII, los POST usan el set gráfico `gentleman` incluido; no requiere cambiar este firmware. Más adelante, poner `enabled: false` solo al destino localhost para reservarlo a depuración. `targets` sustituye a la URL única antigua; sus arrays se reemplazan completos entre capas de configuración.

## API y límites

- `GET /health` → JSON con `ok`, `width`, `height`, contador `frames`, `brightness` (0–255), `topOffset` (px), `bitDepth`, `wifiInfo` (pantalla informativa visible), `accelerometer` (detectado), `rotation` (0/90/180/270) y `orientationCalibrated`.
- `POST /emote`: `Content-Type: image/png`, `X-Emote-Version: 1`, `Content-Length` obligatorio.
- PNG **64×64, RGBA de 8 bits, sin entrelazado**, chunks IHDR/IDAT/IEND (formato exacto producido por gentle-emote). No admite PNG arbitrarios con paleta, RGB, metadatos o chunks auxiliares.
- Máximo **32 KiB** de cuerpo, **4 KiB** de cabeceras y **2,5 s** totales por petición. No admite `Transfer-Encoding`, `Expect`, keep-alive ni pipelining. Conexión cerrada tras responder.
- Responde 200 tras decodificar y presentar el frame; errores 4xx conservan la imagen anterior. Verifica CRC de chunks, dimensiones y decodificación completa a un buffer temporal antes de presentar.
- Un cliente activo y un buffer de imagen, sin cola de frames de aplicación. La última recepción válida sustituye la anterior. No ordena ni descarta por stream/sequence; otros metadatos no se redibujan ni almacenan.
- El PNG ya contiene avatar, status, contador y phase. Brillo limitado en firmware a ~25% (`panel::BRIGHTNESS = 64`), profundidad de 4 bits/canal y doble buffer en la corrección preparada. Los mensajes de inicio usan el mismo escalado de brillo. Ajustes en `include/display_settings.h`.
- **Sin autenticación ni TLS**: usar únicamente en una LAN de confianza. No abrir puertos en el router. No enviar credenciales Wi-Fi al endpoint HTTP.

## Margen y colores

El firmware desplaza todo el contenido **3 px hacia abajo** en coordenadas lógicas, también al girar. Las filas superiores 0–2 quedan negras. Descarta las tres últimas filas del PNG, que en el emisor actual solo contienen el color de fondo: el status queda en 47–51 y phase en 57–61, ambos dentro de los 64 px. Un PNG arbitrario con contenido en las últimas tres filas sí se recortará. El simulador y el PNG enviado no cambian.

Se mantiene la paleta original y el orden RGB. Se probó subir Protomatter de 4 a 6 bits/canal para reducir cuantización, pero el usuario observó temblor/parpadeo. La corrección preparada vuelve a 4 bits/canal para priorizar el refresco estable; la fuente sigue siendo RGB565 (5/6/5 bits). El escalado de brillo redondea en lugar de truncar para conservar mejor los tonos oscuros. La profundidad adicional puede afectar refresco y RAM de ejecución: comprobar estabilidad y colores en la placa. No se ha aplicado gamma ni balance RGB sin una referencia visual del panel.

**Fondo gris descartado por el usuario.** Se retiraron `background` y `backgroundCoverage` de la configuración local del emisor para restaurar el fondo negro predeterminado; aplicar con `/reload`, sin cambiar firmware. Se conserva el soporte opcional y sus tests. Durante la prueba, `#303030` se veía rosa por pérdida del verde al cuantizar; se probaron `#383838` y tramado estático al 75%, pero finalmente se decidió volver al negro.

El emisor local tiene ahora `externalDisplay.outline: "#808080"`: contorno externo blanco apagado de 1 px; se conserva la paleta del sprite sobre la silueta, sin afectar a las etiquetas ni al firmware. Aplicar con `/reload`. En sprites opacos se infiere la silueta usando negro puro como fondo; en sprites con transparencia se usa alfa. Los huecos internos cerrados no se contornean. El borde puede recortarse si la figura toca la primera fila del PNG.

## Botón UP: información Wi-Fi y emisores

El botón **UP (GPIO6, activo bajo)** muestra estas páginas en orden, **5 segundos por página**:

- Estado de conexión, **IP asignada** y señal RSSI. La IP se reparte en dos líneas para que quepa completa: `192.168.` + `1.227` = `192.168.1.227`.
- SSID completo (hasta 32 bytes) y estado. Caracteres no soportados por la fuente ASCII se muestran como `?`.
- Emisores: una página por IP de origen, con IP completa en dos líneas y contador `REQUESTS`. Si aún no hay emisores indica `SENDERS / NONE YET`. La duración total es de **15 a 50 segundos**, según los emisores registrados; si aparece uno nuevo durante la consulta se amplía hasta incluirlo.

Se conservan las **primeras 8 IPv4 distintas** en RAM, en orden de llegada, sin expulsar ninguna. `MORE IPS!` indica que hubo intentos de otras IP que no se almacenaron. Cada contador se satura en **4294967295**, sin volver a cero. Los datos se borran al reiniciar; no se guardan en NVS. La IP procede exclusivamente de la conexión TCP, no de cabeceras enviadas por el cliente.

Se cuenta **una vez por conexión**, en cuanto llega la primera línea completa y exacta `POST /emote HTTP/1.0\r\n` o `POST /emote HTTP/1.1\r\n` (los caracteres `\r\n` representan CRLF). Cuenta todos esos intentos, aunque después fallen las cabeceras —incluidos caracteres inválidos—, el PNG, el cuerpo incompleto o el timeout. Una línea inicial incompleta, no identificable o no soportada no cuenta. Tampoco cuentan `/health`, otras rutas ni otros métodos. El contador `frames` de `/health` sigue contando solo imágenes aceptadas. Firmware cargado por USB con hash de escritura verificado; funcionamiento de la nueva sección de emisores confirmado visualmente por el usuario sobre la placa.

Otra pulsación de UP cierra la pantalla antes del timeout. Al terminar vuelve al **último frame recibido** si sigue vigente; si han pasado 10 segundos y hay hora válida, muestra el reloj. Los POST, el contador y la autorrotación siguen activos mientras se muestra la información. No muestra ni registra la contraseña. Si no hay conexión indica `NO WIFI / NO IP`. El botón DOWN queda libre y BOOT/RESET no se modifican. `/health` añade `wifiInfo` para indicar si la pantalla informativa está visible.

## Rotación automática

Usa el **LIS3DH** integrado, I²C `0x19`, SDA GPIO16 / SCL GPIO17. Muestrea cada 50 ms y exige 350 ms de dirección estable antes de cambiar entre 0°, 90°, 180° y 270°. Mantiene la orientación si el panel está casi plano, en diagonal o se mueve bruscamente. Si no detecta el sensor, el receptor sigue funcionando sin autorrotación.

**En el primer arranque de esta actualización**, sostener el panel vertical y con su posición habitual correcta: la primera postura estable se guarda como referencia en NVS. La referencia persiste entre reinicios, también si se arranca con el panel girado. El sensor ya se ha detectado sobre hardware y se ha guardado una referencia estable. El usuario ha confirmado que la detección de giro funciona correctamente.

Para recalibrar, colocar el panel en la orientación de referencia deseada y enviar `ORIENT` seguido de salto de línea por USB. Después mantenerlo quieto. Los cambios aparecen en el monitor serie y `/health`. Se redibuja el contenido activo (PNG o reloj) incluso sin nuevos POST; PNGs rechazados no alteran la copia del último frame válido. La rotación y el brillo se aplican solo en la placa: no afectan al simulador.

## Panel y pinout

Escaneo estándar **1/32** con cinco líneas de dirección. La prueba inicial se vio correctamente. No se ha verificado otro modelo de panel/controlador.

| Señal | GPIO |
|---|---|
| R1, G1, B1 | 42, 41, 40 |
| R2, G2, B2 | 38, 39, 37 |
| A, B, C, D, E | 45, 36, 48, 35, 21 |
| CLK, LAT, OE | 2, 47, 14 |

Usar alimentación adecuada al panel y masa común. No asumir que el USB del Mac puede alimentar la matriz.

## Pruebas

```sh
# Compilar primero para descargar las dependencias fijadas:
.venv/bin/pio run
.venv/bin/python -m unittest discover -s tests -v
```

Los tests compilan código C++ nativo con clang y la misma versión de PNGdec que el firmware. Cubren límites HTTP, CRC/chunks, formato y dimensiones, deflate inválido, filtros inválidos, transparencia, extremos del framebuffer, calibración, cuatro orientaciones, estabilidad, desbordamiento de `millis()`, escalado RGB y límites del margen superior. La prueba de PNG real se omite si no existe el checkout hermano `gentle-emote`. No sustituyen pruebas de Wi-Fi, tiempo de respuesta, alimentación o visualización sobre la placa.

Referencias:
- https://www.adafruit.com/product/5778
- https://docs.platformio.org/en/latest/boards/espressif32/adafruit_matrixportal_esp32s3.html
- https://github.com/adafruit/Adafruit_Protomatter/blob/master/examples/simple/simple.ino
- `../emote-display-simulator`: referencia del protocolo, no dependencia del firmware.
