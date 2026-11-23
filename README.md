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

## Versión publicada

La release [**v1.1.0**](https://github.com/arkamax404-max/emote-display-firmware/releases/tag/v1.1.0) publica el firmware que corre hoy la placa: reserva del display por sesión (`/emote displayOn` / `displayOff`, rutas `claim`/`heartbeat`/`release`) y título de la sesión en la banda superior del reloj autónomo, alternando con la fecha cada 5 s.

| Asset | Contenido |
| --- | --- |
| [`firmware-panel-title.uf2`](https://github.com/arkamax404-max/emote-display-firmware/releases/download/v1.1.0/firmware-panel-title.uf2) | Imagen de aplicación para TinyUF2, OTA0, base `0x0`. 1 759 744 bytes, SHA256 `3e13d4bd14cc63368f2956aaf3d6643d8a8f01c2d1acfea42b7fe05bf2dd6b9a` |
| [`manifest.json`](https://github.com/arkamax404-max/emote-display-firmware/releases/download/v1.1.0/manifest.json) | Procedencia reproducible: binario de origen y su hash, imágenes de entrada, comandos exactos, herramienta, política de padding y `device_write_performed: false` |

Para grabarla: doble reset para entrar en TinyUF2, copia el `.uf2` al volumen `MATRXS3BOOT` y espera a que el bootloader reinicie. No hay que borrar nada ni tocar `CURRENT.UF2`. La imagen no contiene credenciales: el Wi-Fi y el resto de la configuración viven en la flash del dispositivo.

Verificación de esta versión: 53/53 pruebas anfitrionas, verificación independiente sin divergencias en 207 casos de validación frente al receptor de escritorio, 162/162 operaciones no modificadas idénticas, y comportamiento visible validado por el usuario en la placa.

## Reloj autónomo (v1.0.0 grabado; validación física pendiente)

**Grabación verificada:** el firmware `v1.0.0` quedó en OTA0 mediante una única carga TinyUF2 autorizada. La copia del Mac devolvió un error de E/S; no se reenvió. Dos lecturas posteriores de `CURRENT.UF2`, su decodificación oficial y una revisión independiente confirmaron los 2 MiB completos: imagen nueva exacta más la cola anterior preservada. El error de copia permanece registrado; esta comprobación no demuestra la conservación de toda la flash ni el funcionamiento físico del reloj. **Nota:** la placa se actualizó después a `v1.1.0` (ver [Versión publicada](#versión-publicada)); este apartado queda como historial de aquella grabación.

El panel puede mostrar la hora sin el agente ni el PC. Usa NTP sobre el Wi-Fi ya configurado y la zona **Europe/Madrid**, con cambio automático de horario de verano/invierno; no cambia credenciales ni añade comandos USB o campos HTTP.

- Al arrancar, conserva los diagnósticos hasta disponer de una hora válida. Sin un PNG previo, muestra entonces el reloj.
- La pantalla de espera `READY / SEND PNG` añade la versión compilada debajo de `SEND PNG` (**v1.1.0** en el build actual). La versión solo aparece en esa pantalla, no en los otros diagnósticos, el reloj, los PNG ni la información Wi-Fi. Conserva el brillo, el margen y la rotación actuales; la visibilidad física de esa versión sigue pendiente.
- Un PNG válido ocupa la pantalla durante **10 segundos** desde su recepción. Al vencer ese plazo aparece el reloj si hay hora válida; si no, conserva el PNG. Otro PNG válido recupera la pantalla inmediatamente.
- Un PNG rechazado no reemplaza la imagen guardada, no incrementa el contador ni reinicia esos 10 segundos. Si no llega tráfico por pausa del emisor, suspensión del PC o pérdida de red, también puede activarse el reloj.
- Tras sincronizar, sigue contando sin Wi-Fi mientras tenga alimentación. Una pérdida completa de alimentación requiere sincronización de nuevo. NTP y las lecturas de hora no bloquean el bucle esperando a la red.
- Formato `dd/mm/yyyy`, hora de 24 h, horas/minutos grandes, segundos mayores y dos personajes caminando con las **nueve posturas de lemmings** extraídas de la hoja de referencia `lemmings_reference.png` (362×545 RGBA, 15 px por píxel lógico, origen de cuadrícula (2,3), muestreo por voto en el centro de celda; recortadas a 8×10 px sobre las filas 53–62, sin recortar pies ni antenas, sin tocar el texto ni el camino): pelo verde saturado (`rgb(0,179,0)`), mono azul royal (`rgb(64,64,224)`) y piel rosada (`rgb(240,208,208)`), paleta muestreada de bloques interiores puros. Las nueve posturas ciclan cada **500 ms** desde un tick de animación monotónico (`esp_timer_get_time()`, 1 tick = 0,5 s) y la pantalla del reloj se redibuja también cada 500 ms: cada walker avanza 1 px por tick (2 px/s, velocidad y recorrido de 40 s sin cambios) mientras la hora mostrada sigue cambiando solo una vez por segundo. Se conservan brillo y rotación; el reloj se dibuja sin desplazamiento vertical sobre las 64 filas completas (camino en la fila 63 y pies en las filas 53–62), sin otro framebuffer.
- **Banda superior alternante (S5):** la banda de las filas 1–5 alterna cada **5 segundos** (según `floor(epoch_millis / 5000) mod 2`, derivado del reloj absoluto, así que todas las pantallas cambian a la vez): en la fase par mantiene la fecha centrada `dd/mm/yyyy` habitual y en la fase impar muestra el **título de la sesión** recibido en `X-Emote-Project-Name`, en mayúsculas ASCII, con glifos 3×5 a escala 1, tinta apagada e `y=1`, centrado con `(64 - (celdas * 4 - 1)) / 2`; a partir de 17 celdas se recorta a 13 más `...`, y los caracteres sin glifo reservan celdas en blanco. Un nombre ausente o vacío muestra siempre la fecha en ambas fases. Todo píxel fuera de las filas 1–5 conserva el contenido canónico: dígitos, `/` y `:` mantienen sus mapas de bits, y horas, minutos, segundos y walkers no se mueven.
- **Inactividad del título (S5):** si pasan **más de 15 segundos** sin una imagen aceptada o una llamada de control exitosa, la fase del título muestra `*NO SESSION*`; la siguiente imagen aceptada o control exitoso restaura el nombre guardado. El tráfico rechazado nunca refresca ese plazo.
- La pantalla informativa de Wi-Fi tiene prioridad. Al cerrarla muestra el PNG todavía vigente o el reloj, según corresponda.

**Estado de aceptación:** el usuario ha confirmado que la animación ha mejorado y ha dado por buenos los cambios para integrarlos en `main`. El firmware se ha compilado y su UF2 se ha validado frente al binario generado. Esta aceptación visual no equivale a comprobar todos los escenarios de NTP, cambio de horario o pérdida de red; las pruebas locales tampoco sustituyen esas comprobaciones físicas. La hoja `lemmings_reference.png` es una referencia local no distribuida en el repositorio.

**La grabación y las pruebas locales no equivalen a una validación física.** Tras la lectura de comprobación, la placa permanece en TinyUF2: falta arrancar la aplicación y comprobar la versión visible, Wi-Fi/NTP real, recepción PNG, retorno al reloj tras 10 segundos sin PNG válido y estabilidad. Pulsar RESET una sola vez permite intentar el arranque normal; el resultado aún no está confirmado.

## Continuar el reloj desde el Mac

El [registro de actualización v1.0.0](odd/tasks/versioned-clock-upload.md) contiene la autorización exacta, las comprobaciones y la validación física pendiente. Los binarios y las copias de recuperación están en `.tools/firmware-upgrade/`, ignorados por Git y con permisos privados; la copia completa de flash puede contener credenciales Wi-Fi. Se conservaron la imagen anterior y las evidencias del error de copia.

La [guía de continuación en macOS](docs/continue-on-macos.md) reúne la rama, el estado verificado, la compilación sin carga y los requisitos pendientes antes de escribir. La guía conserva la evidencia y los pendientes de aquella sesión; para el estado actual de aceptación, consultar la sección anterior.

La [preparación ROM anterior](docs/macos-firmware-upgrade.md) documenta el candidato previo y sus bloqueos: no describe una carga ROM aprobada. La autorización TinyUF2 cubrió una sola copia de un archivo concreto, no cargas futuras ni restauraciones. **No ejecutar automáticamente el upload genérico siguiente para repetir esta actualización.**

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

Tras conectar aparece `READY / SEND PNG`; el monitor imprime la IP. Reintenta la conexión si se pierde. Publica `gentle-matrix.local` por mDNS; no resolvió en la prueba de este Mac, así que se utilizó la IP mostrada (`192.168.1.227`). Conviene reservar esa IP en DHCP o actualizar la configuración si cambia. El firmware también incluye un **portal de configuración sin ordenador**: si al arrancar no puede conectar a la red guardada (4 intentos de 10 s con 5 s de pausa) y **no hay ningún equipo enumerado por USB**, abre un SoftAP WPA2 `gentle-matrix` con portal cautivo; la contraseña del AP se muestra en el panel y es nueva en cada arranque, y la página ofrece un **selector de las redes detectadas** (escaneo nuevo en cada carga, ordenado por señal y sin campo manual: las redes ocultas solo se configuran por USB). Además, **mantener pulsado DOWN 5 segundos** borra las credenciales guardadas (`ssid`, `password`, `updir`, con verificación de relectura) y reinicia la placa, en cualquier modo y sin mensaje en el panel. Detalles, condiciones y recuperación: [docs/wifi-captive-portal.md](docs/wifi-captive-portal.md).

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

- `GET /health` → JSON con `ok`, `width`, `height`, contador `frames`, `brightness` (0–255), `topOffset` (px), `bitDepth`, `wifiInfo` (pantalla informativa visible), `accelerometer` (detectado), `rotation` (0/90/180/270) y `orientationCalibrated`. Añade `sessionOwnership` (siempre `true`), `ownershipActive` y `ownershipRemainingMs` (ms restantes de la reserva; nunca revela la identidad del propietario). Añade además los contadores saturantes del ingreso serie: `serialRxImages` (imágenes binarias realmente presentadas), `serialRxControls` (claim/heartbeat/release aceptados) y `serialRxRejected` (rechazos de parseo/decodificación/admisión y timeouts de recepción). Añade también el campo aditivo de identidad `portal` (`false` en el receptor Wi-Fi habitual; el modo portal sirve su propio `/health` con `true`, sin ningún secreto).
- `POST /emote`: `Content-Type: image/png`, `X-Emote-Version: 1`, `Content-Length` obligatorio. La cabecera opcional `X-Emote-State: clock` marca un frame de reloj del emisor; su ausencia o cualquier otro valor mantienen el avatar. La cabecera opcional `X-Emote-Session-Id` identifica la sesión emisora (valores válidos: 1–128 caracteres de `[A-Za-z0-9._:-]`; valores duplicados o malformados rechazan la petición). La cabecera opcional `X-Emote-Project-Name` transporta el título de la sesión con las mismas reglas que el receptor de escritorio: se decodifica porcentaje **una sola vez** (`+` es literal, `%` exige dos dígitos hexadecimales), con límite de 1152 bytes codificados, 384 bytes UTF-8 decodificados y 128 unidades UTF-16; se rechazan con **400** y el cuerpo `invalid project name\n` (sin cambiar ningún estado) un valor vacío tras recortar espacios, exactamente `.` o `..`, cualquier carácter `<= U+001F`, `U+007F`, `/` o `\`, escapes malformados y bytes decodificados que no sean UTF-8 válido; una segunda cabecera `X-Emote-Project-Name` rechaza también la petición completa. El nombre solo se guarda cuando una imagen es admitida por la reserva y presentada con éxito, por lo que una imagen ajena, sin sesión o inválida nunca renombra la pantalla; una imagen aceptada posterior **sin** la cabecera borra el nombre guardado. `claim`, `heartbeat` y `release` son rutas de ciclo de vida e ignoran la cabecera por completo, igual que el receptor de escritorio: un valor duplicado o inválido en ellas no rechaza la orden ni cambia el título guardado.
- Control por sesión (coordinación en LAN de confianza, no autenticación): `POST /emote/claim`, `POST /emote/heartbeat` y `POST /emote/release`, sin cuerpo (`Content-Length: 0`), con `X-Emote-Version: 1` y exactamente un `X-Emote-Session-Id` válido. Una respuesta 200 lleva la cabecera `X-Emote-Ownership: 1`; un firmware antiguo (404/405) o una respuesta sin ese marcador se tratan como envío legado con aviso. La última orden `claim` explícita gana aunque otro propietario estuviera activo. `heartbeat` renueva la reserva (30 s) sin reclamar; `release` del propietario libera y es idempotente sin propietario. Un `heartbeat` o `release` ajeno, o un PNG de otra sesión o sin sesión mientras hay propietario, responde 409 sin tocar la imagen mostrada, su clasificación ni el reloj autónomo. Sin propietario (arranque, tras `release` o tras vencer la reserva) los PNG se aceptan libremente como siempre. La reserva nunca persiste: el arranque empieza sin propietario.
- PNG **64×64, RGBA de 8 bits, sin entrelazado**, chunks IHDR/IDAT/IEND (formato exacto producido por gentle-emote). No admite PNG arbitrarios con paleta, RGB, metadatos o chunks auxiliares.
- Máximo **32 KiB** de cuerpo, **4 KiB** de cabeceras y **2,5 s** totales por petición. No admite `Transfer-Encoding`, `Expect`, keep-alive ni pipelining. Conexión cerrada tras responder.
- Responde 200 tras decodificar y presentar el frame; errores 4xx conservan la imagen anterior. Verifica CRC de chunks, dimensiones y decodificación completa a un buffer temporal antes de presentar.
- Un cliente activo y un buffer de imagen, sin cola de frames de aplicación. La última recepción válida sustituye la anterior. No ordena ni descarta por stream/sequence; otros metadatos no se redibujan ni almacenan.
- El PNG ya contiene avatar, status, contador y phase. Brillo limitado en firmware a ~25% (`panel::BRIGHTNESS = 64`), profundidad de 4 bits/canal y doble buffer en la corrección preparada. Los mensajes de inicio usan el mismo escalado de brillo. Ajustes en `include/display_settings.h`.
- **Sin autenticación ni TLS**: usar únicamente en una LAN de confianza. No abrir puertos en el router. No enviar credenciales Wi-Fi al endpoint HTTP.

## Ingreso serie USB (compilado; sin grabar)

Alternativa al HTTP para enviar actualizaciones del display por el puerto USB CDC, con las mismas operaciones (`image`/`claim`/`heartbeat`/`release`), las mismas reglas de sesión (`session::Owner`), el mismo decodificador de título y la misma garantía de "rechazo no cambia estado". Documento de cable exacto: [docs/serial-transport.md](docs/serial-transport.md); protocolo en `include/serial_protocol.h`, multiplexado en `include/serial_ingress.h`, reglas compartidas en `include/image_ingress.h`.

- La consola de provisión sigue siendo el modo por defecto y su protocolo de líneas no cambia (`WIFI`/`ORIENT`, incluidos SSID o contraseñas que empiecen por `EMTS` o valgan `EMOTE`). El modo binario se entra con la línea exacta `EMOTE` (ack `EMOTE_READY`) y se sale con `EMTE
` (ack `EMOTE_EXIT`), reconocidas solo con el parser inactivo.
- Imágenes en PNG crudo (sin base64) hasta 32 KiB en un buffer estático propio; HTTP y serie pueden operar simultáneamente sin compartir buffer. Stop-and-wait con ACK/NACK de 12 bytes correlacionado por `requestId` que nunca contiene identidades; respuesta escrita de forma no bloqueante.
- Timeout en sesión binaria: 1 s de inactividad o 10 s totales por frame descartan el frame y resincronizan; el timeout HTTP (2,5 s) no cambia. Rendimiento CDC nativo: medición de hardware pendiente, no una promesa.
- **Indicador diagnóstico**: triángulo rectángulo de 4 px pegado a la esquina superior derecha del canvas lógico rotado (filas de 4/3/2/1 px, todas terminando en x63), dibujado como superposición después de cualquier contenido (avatar/reloj/mensajes/info) y antes de cada show, así el frame nuevo nunca lo borra. **Amarillo** mientras haya bytes pendientes (el primer byte recibido arma la ventana: bytes de consola o magia parcial incluidos; abrir el puerto o arrancar no marca nada); **verde** solo tras una operación binaria realmente aceptada (imagen presentada o claim/heartbeat/release admitido — un verde por el claim inicial no prueba que llegó un frame; los contadores lo distinguen); **rojo** ante rechazo de parseo/decodificación/admisión o timeout (1 s de inactividad, 10 s totales desde el primer byte). Tráfico de consola sin marco (WIFI/ORIENT) que nunca completa una operación binaria muestra rojo tras 1 s, comportamiento documentado. El handshake `EMOTE` es un límite de protocolo, nunca una operación aceptada: deja el indicador pendiente (amarillo, sin timeout) hasta la operación binaria; `EMTE` limpia el amarillo pendiente. El verde/rojo se mantiene al menos 750 ms antes de que el amarillo pueda sustituirlo; un resultado terminal sustituye a otro terminal de inmediato y persiste si no llegan bytes nuevos (tras completar un frame no se arma ningún timeout). Contadores en `/health`: `serialRxImages`, `serialRxControls`, `serialRxRejected` (uint32 saturantes, campos aditivos).
- Estado: compilado con `python -m platformio run` (sin grabar); 60 pruebas anfitrionas nuevas (protocolo + multiplexado/admisión) en verde y las 49 pruebas preexistentes sin cambios de resultado. La validación física corresponde a T3.
- El usuario confirmó la recepción de imágenes por el puerto serie sobre la placa ("ya recibimos imagenes"): evidencia externa registrada sin acceso al dispositivo desde esta tarea; no se investiga ni se atribuye causa raíz del fallo previo.
- Identidad candidata de depuración `v1.2.0-dbg` (10 celdas, caben en los 64 px; la pantalla READY la mostrará en la siguiente compilación; artefacto pendiente, sin grabar).
- Identidad candidata de desarrollo `v1.2.0-dev` (no publicada, distinta de la versión liberada v1.1.0): la pantalla READY muestra ahora esta identidad, 10 celdas de 6 px en x=2, dentro de los 64 px del panel. Artefacto TinyUF2 solo-aplicación preparado sin grabar en `.tools/firmware-upgrade/usb-serial-v1.2.0-dev/firmware-usb-serial-v1.2.0-dev.uf2` (1 768 960 bytes, SHA256 `34f8cffc79dac5e55d5f5c604d895b54a4799cf00edd767e15c3105d98be777f`, 3455 bloques de 512, familia `0xc47e5767`, base `0x0`); `manifest.json` en el mismo directorio registra hashes, controles negativos y lo que queda sin probar sin placa. La escritura en la placa sigue requiriendo autorización explícita aparte.

## Configurar Wi-Fi desde un móvil (portal cautivo, compilado; sin grabar)

Cuando la placa **no está conectada a un ordenador por USB** y no puede entrar en su red guardada al arrancar (o no tiene credenciales guardadas), abre un punto de acceso **WPA2** `gentle-matrix` con portal cautivo en `http://192.168.4.1`; la contraseña aleatoria de 8 caracteres del AP se muestra en el panel (rotando con el mensaje `PORTAL` y la dirección del AP), nunca se guarda ni aparece en HTML, registros o `/health`.

La página del portal ofrece un **selector de redes** (S5): cada carga de la página ejecuta un **escaneo nuevo** — el portal mantiene la radio en `WIFI_AP_STA` para que la estación pueda escuchar — y muestra las redes visibles como **botones de opción** de un único campo `ssid`, deduplicadas por nombre (sobrevive la señal más fuerte), ordenadas por señal descendente y limitadas a 12 entradas; cada SSID se muestra escapado. No hay campo manual de SSID (decisión del usuario): una red oculta o fuera de rango solo puede configurarse por USB. Antes de guardar, la placa **valida** los credenciales ofrecidos contra la red real (hasta 20 s en `WIFI_AP_STA`; la validación nunca escanea); si conecta los guarda con la misma verificación de relectura del cable USB y reinicia; si no, deja las credenciales guardadas intactas, cero los buffer con la contraseña ofrecida y vuelve a mostrar el formulario con el error y la lista recién escaneada.

**Reinicio de credenciales (S6):** mantener pulsado **DOWN (GPIO 7, activo bajo, con antirrebote de 50 ms) 5 segundos continuos** borra exactamente `ssid`, `password` y `updir` del espacio `gentle` con verificación de relectura y reinicia; el brillo y todo el resto de ajustes siguen intactos y no hay mensaje en el panel (el reinicio es la única señal). El sondeo funciona en todos los modos, no lee el puerto serie y no interfiere con el botón UP ni la recalibración de orientación. Si un borrado no se puede verificar, la placa **no** reinicia y se puede reintentar.

Solo ocurre en el arranque; una pérdida de red en marcha sigue reintentando sin portal. USB enumerado (`tud_mounted()`) tiene prioridad: con cable, la provisión serie sigue siendo la vía autorizada. Flujo completo, condicionado por USB y recuperación: [docs/wifi-captive-portal.md](docs/wifi-captive-portal.md). Verificación anfitriona: 42 pruebas del portal (`tests/test_portal.py`) y 14 nuevas del reset por DOWN (`tests/test_reset.py`) en verde, 199 en total de la batería; el selector y el reset reales en placa, el escaneo con el AP activo y la detección del portal en móviles aún sin validar.

**Riesgo residual:** la contraseña de la red se envía por un formulario HTTP **en claro** (sin TLS). El enlace es WPA2, pero la PSK que lo protege es la que muestra el panel, así que quien esté a distancia de radio y pueda leer el panel puede unirse al AP y descifrarla. El token CSRF no aporta confidencialidad. Configura la placa solo donde el panel no sea visible para otras personas (o tápalo mientras escribes); si no es posible, provisiona por **USB** (`scripts/provision_wifi.py`), que sigue siendo la alternativa privada y nunca pone la contraseña en el aire.

## Margen y colores

El firmware dibuja los avatares **sin desplazamiento vertical**, también al girar: conserva las 64 filas del PNG, sin añadir margen superior ni recortar la última fila. Se elimina el margen de 1 px solo para los avatares; la posición del reloj interno no cambia. La aceptación de la animación no constituye una comprobación específica de los bordes del avatar. Un frame enviado con `X-Emote-State: clock` se dibuja sin desplazamiento y usando las 64 filas; esa clasificación se guarda con el último frame válido y un PNG rechazado no la altera. Los relojes, autónomo y recibido, no sufren desplazamiento. El simulador y el PNG enviado no cambian.

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

Otra pulsación de UP cierra la pantalla antes del timeout. Al terminar vuelve al **último frame recibido** si sigue vigente; si han pasado 10 segundos y hay hora válida, muestra el reloj. Los POST, el contador y la autorrotación siguen activos mientras se muestra la información. No muestra ni registra la contraseña. Si no hay conexión indica `NO WIFI / NO IP`. El botón **DOWN (GPIO 7, activo bajo)** ya no queda libre: desde esta compilación, mantenerlo pulsado **5 segundos** borra las credenciales Wi-Fi guardadas y reinicia (sección del portal cautivo y [docs/wifi-captive-portal.md](docs/wifi-captive-portal.md)); una pulsación corta no hace nada y una pulsación larga no interfiere con la consulta UP. BOOT/RESET no se modifican. `/health` añade `wifiInfo` para indicar si la pantalla informativa está visible.

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
