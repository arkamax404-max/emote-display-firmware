# Continuar el reloj autónomo en macOS

La rama `feat/autonomous-idle-clock` contiene el reloj autónomo y sus pruebas. **El firmware nuevo todavía no está instalado en la placa.** Esta guía permite recuperar el trabajo y preparar la continuación en el Mac, no dar por aprobada una carga pendiente.

## Estado que recibes

| Elemento | Estado |
|---|---|
| Implementación del reloj | Commit `68852a72f8719c64fa3ea9cadff2b79e1e5992ef` |
| Pruebas locales e independientes del firmware | 18/18; evidencia en [el registro de implementación](../odd/tasks/autonomous-idle-clock.md) |
| Compilación ESP32-S3 en Windows | Correcta; esptool 4.9 confirmó checksum y hash internos válidos |
| Escrituras en la placa durante este trabajo | Ninguna; conserva el firmware anterior |
| Revisión independiente del procedimiento de carga Windows | No disponible: el proceso terminó sin dictamen; no es un rechazo del firmware |
| NTP, visibilidad y estabilidad del reloj en hardware | Pendientes, solo comprobables después de una carga válida |

**La revisión pendiente no se convierte en aprobación al cambiar de equipo.** No relanzar ni trasladar el verificador fallido como atajo, ni eliminar sus límites. Antes de cualquier escritura hace falta una revisión técnica independiente válida del procedimiento que se vaya a ejecutar. Si esa prueba no está disponible, detenerse sin modificar la placa.

## 1. Recuperar la rama, sin reemplazar trabajo local

Para una copia nueva, ejecutar en un directorio libre:

```sh
git clone --branch feat/autonomous-idle-clock --single-branch https://github.com/arkamax404-max/emote-display-firmware.git
cd emote-display-firmware
git status --short --branch
git merge-base --is-ancestor 68852a72f8719c64fa3ea9cadff2b79e1e5992ef HEAD
```

El último comando debe terminar con código 0. Si ya existe un checkout en el Mac, revisar primero sus cambios y su rama; no copiar encima, borrar archivos, usar `reset --hard` o `clean`, ni sobrescribir su entorno.

## 2. Reutilizar el entorno macOS

El entorno anterior era Mac Intel, macOS 14.8.7, Apple Command Line Tools y Python 3.13 de Homebrew. Reutilizar la `.venv` del Mac si es compatible; **no copiar `.venv`, `.tools` ni `.pio` desde Windows**, ni modificar el Python del sistema.

Solo si no existe una `.venv` válida, crear una local con el Python 3.13 ya disponible:

```sh
python3.13 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
```

No actualizar dependencias ni cambiar pines para resolver un fallo sin diagnosticarlo. El proyecto fija `espressif32@6.12.0`; la compilación verificada usó Arduino ESP32 2.0.17 y Protomatter 1.7.1. Las dependencias restantes están en `platformio.ini`.

## 3. Compilar y comprobar sin cargar

Estos comandos no escriben en la placa:

```sh
.venv/bin/python -m platformio run -e matrixportal_s3
.venv/bin/python -m unittest discover -s tests -v
shasum -a 256 .pio/build/matrixportal_s3/firmware.bin
.venv/bin/python -m serial.tools.list_ports -v
```

Registrar resultados, omisiones y el hash del binario realmente elegido. La suite completa tiene 18 pruebas; su integración con el emisor necesita Node.js compatible con sus imports TypeScript y el checkout hermano `../gentle-emote`. Si falta `../gentle-emote/src/display_image.ts`, esa prueba se omite: no informar 18/18 sin omisiones en ese caso. Una omisión no equivale a una prueba superada.

Referencia del binario de Windows: **871488 bytes**, SHA-256 `38b7f5939e2c63e276489494393575461c2e0e394b4a331ee0d383ee36aa7f37`. Ese binario no se publica: `.pio` es un directorio de compilación local. Una reconstrucción en otro entorno puede variar; no confundir el hash interno que muestra esptool con el SHA-256 del archivo completo, ni afirmar que un binario nuevo es el anterior.

En macOS el puerto será `/dev/cu.…`, no `COM9` ni `COM10`. El último puerto normal visto en Windows fue un candidato Adafruit `239A:8125`; el modo ROM anterior era `303A:1001`. Enumerar de nuevo y confirmar la identidad de la placa antes de abrir un puerto. No pulsar BOOT ni RESET solo para seguir esta guía mientras falte el preflight.

## 4. Resolver el preflight antes de escribir

El procedimiento mínimo debe usar las herramientas oficiales existentes, sin depender de los auxiliares Windows privados. Su revisión tiene que comprobar:

- Identidad ESP32-S3, seguridad, tamaño de flash y particiones reales del dispositivo conectado; las observaciones anteriores no son una autorización vigente.
- Selección OTA activa mediante metadatos válidos y correspondencia con la aplicación que se pretende reemplazar.
- Binario exacto, compatibilidad, tamaño y rango de borrado; lectura posterior comparada con ese mismo binario y preservación del resto de la flash.
- Recuperación disponible y comprobada, sin divulgar ni persistir credenciales en archivos sin protección.

La tabla observada anteriormente tenía flash de 8 MiB, NVS `0x9000/0x5000`, otadata `0xe000/0x2000`, `ota_0` en `0x10000/0x200000`, `ota_1` en `0x210000/0x200000`, TinyUF2 en `0x410000` y ffat en `0x450000`. La selección validada entonces era `ota_0`. Son referencias, no valores que deban imponerse a ciegas a otro estado o dispositivo.

Con el binario de Windows, la escritura prevista empezaba en `0x10000` y el borrado terminaba en `0xe5000` (límite exclusivo). Recalcular para el binario seleccionado: **solo aplicación**, nunca borrado total, `--force`, cambio de eFuses ni reescritura de NVS, bootloader, tabla, otadata, TinyUF2 o ffat. No ejecutar automáticamente el upload genérico de PlatformIO del README: puede incluir otras imágenes además de la aplicación.

### Copia y recuperación

Existe una copia completa cifrada y validada en el PC, bajo `.tools/clock-upload/backup-before-clock-20261109`. **No está en Git y no se puede descifrar directamente en el Mac:** usa DPAPI ligada al perfil de Windows que la creó. Conservar ese perfil y los archivos; no dar por disponible una recuperación macOS por tener esta referencia.

Antes de escribir desde el Mac, establecer una recuperación utilizable y validarla con herramientas oficiales en almacenamiento protegido. No subir dumps, credenciales ni copias privadas al repositorio. No repetir automáticamente la captura completa de Windows; tampoco asumir que su snapshot anterior al RESET sigue coincidiendo con el dispositivo actual. Una restauración necesita autorización explícita y no debe sobrescribir datos ajenos.

## 5. Después de una carga válida

- Comparar la aplicación leída con el binario elegido y comprobar la conservación de las regiones ajenas antes del arranque.
- Confirmar arranque, Wi-Fi existente y contrato PNG/HTTP; no reprovisionar ni cambiar calibración por defecto.
- Comprobar NTP, fecha `dd/mm/yyyy`, hora de Madrid de 24 horas y cambio estacional; el primer arranque necesita hora válida antes de mostrar el reloj.
- Sin PNG previo, el reloj debe aparecer al obtener hora válida. Un PNG válido lo reemplaza inmediatamente; tras 10000 ms sin otro PNG válido vuelve el reloj. Un PNG rechazado no renueva el plazo.
- Confirmar dos caminantes, segundos legibles, orientación, margen superior de tres filas y prioridad de la información Wi-Fi. Tras sincronizar, comprobar avance sin red mientras conserva alimentación.

Registrar lo observado y lo pendiente por separado. No presentar la compilación, las pruebas de host, la enumeración USB o esta guía como validación física del reloj.
