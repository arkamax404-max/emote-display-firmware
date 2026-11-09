# Preparar la actualización integrada desde macOS

**El candidato está congelado; el preflight ROM de solo lectura está autorizado y pendiente de resultado. Todavía NO está listo para escribir.**
Esta preparación combina reloj autónomo y contadores de emisores. El usuario autorizó abrir
el puerto confirmado, entrar/resetear a ROM y leer; también aceptó la recuperación local sin
cifrar con permisos privados. No instala herramientas, no cambia credenciales ni autoriza
cargar un stub, flashear o restaurar. La revisión de solo lectura anterior cubrió la preparación
del plan cifrado, no la ejecución actual ni un nuevo dictamen. No reintenta ni traslada el
verificador Windows que terminó sin dictamen.

## 1. Qué está preparado

| Evidencia local | Resultado |
|---|---|
| Rama / HEAD de preparación | `feat/clock-sender-integration` / `26f3a07a9f87c49207453f247f6c3afef8aeb24a` |
| Integración de código | `a7e4a5687483812b768c4bb40723eed18c2e18f2` |
| Binario congelado | `.tools/firmware-upgrade/clock-sender-26f3a07/firmware.bin` |
| Tamaño | **873968 bytes** |
| SHA-256 del archivo completo | `0d5226bf876ed9f7d1f049f301aa41a6c44202864336d3768a376b139b716642` |
| SHA-256 de `src/main.cpp` | `b65b57f41be5b00fc18033c286b39bacff56a6252224f1d0a0a04e9ad41dad8f` |
| esptool oficial existente | **4.9.0**, paquete PlatformIO `2.40900.250804` |
| `image_info` offline | 5 segmentos; checksum `e1` válido; hash interno válido |

El hash interno de imagen es `d55965d391a81f4a7633bbf663c1077965e82aad8d442340e6950718b811eca8`;
no es el SHA-256 del archivo completo. `manifest.json` e `image-info.txt`, junto al binario,
registran procedencia y comprobaciones reales. `.tools/` está ignorado por Git: estos archivos
no viajan con el commit; conservar esta copia local sin reemplazarla por una reconstrucción.

La compilación y las pruebas previas constan en
[el registro de integración](../odd/tasks/clock-sender-integration.md): SUCCESS,
20 pruebas aprobadas y 1 omisión explícita del checkout hermano. No se recompiló ni se
repitió la suite en esta preparación. No hay validación física de reloj/NTP.

### Repetir solo las comprobaciones offline

Desde la raíz del proyecto, con la `.venv` existente. Cada comando es independiente,
no abre puertos y debe terminar antes de 30 segundos; no continuar si cambia la identidad.

```sh
.venv/bin/python -B /Users/santiagoperez/.platformio/packages/tool-esptoolpy/esptool.py version
```

```sh
.venv/bin/python -B /Users/santiagoperez/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 image_info .tools/firmware-upgrade/clock-sender-26f3a07/firmware.bin
```

```sh
shasum -a 256 .tools/firmware-upgrade/clock-sender-26f3a07/firmware.bin
```

## 2. Herramientas oficiales y límite OTA

Se comprobaron `--help`, `image_info --help`, `read_flash --help`, `write_flash --help`
y `verify_flash --help` del esptool existente, sin operaciones sobre el dispositivo.
La identidad del paquete remite a [Espressif/esptool](https://github.com/espressif/esptool).
No hace falta un flasher propio ni instalar/actualizar dependencias.

Dentro de `/Users/santiagoperez/.platformio/packages/framework-arduinoespressif32/`:

- `tools/gen_esp32part.py` (versión fuente 1.2): conversor oficial CSV/binario;
  se verificaron ayuda y fuente, incluidas comprobación MD5 y detección de solapamientos.
  No determina qué aplicación está ejecutándose ni interpreta la selección OTA.
- `variants/adafruit_matrixportal_esp32s3/partitions-8MB-tinyuf2.csv`:
  **referencia de BUILD, NO tabla viva**. No se congela una tabla como prueba del dispositivo.
- `tools/sdk/esp32s3/include/app_update/include/esp_ota_ops.h`:
  documenta que partición configurada y ejecutada pueden diferir por fallback.
  Un nombre `ota_0` o una secuencia aislada no demuestra la aplicación activa.

No se encontró `otatool.py`, `parttool.py` ni `esp_ota_ops.c` en los paquetes inspeccionados.
La cabecera no sustituye el algoritmo de boot del SDK/bootloader exacto.
**HOLD OTA:** falta una interpretación oficial compatible de los metadatos vivos,
su integridad/estado y el fallback, correlacionada con la aplicación ejecutada.
No inventar un decodificador ni asumir `ota_0`; si no puede probarse, no escribir.

## 3. Recuperación macOS local sin cifrar — copia aún pendiente

La copia histórica usa DPAPI ligada al perfil Windows: conservarla, pero **no cuenta como
recuperación utilizable desde este Mac**. Véase la [guía anterior](continue-on-macos.md).
El usuario aceptó que la nueva copia local sea **sin cifrar y pueda contener credenciales Wi-Fi**.

El directorio `.tools/firmware-upgrade/clock-sender-26f3a07/recovery-local` ya existe con
permisos **0700**, solo para el usuario local. Está ignorado por Git y queda fuera de commits;
los archivos de recuperación deben tener permisos **0600**. Ignorar la ruta no cifra su contenido:
no publicar ni imprimir dumps, metadatos crudos o credenciales en terminal/chat. Dirigir allí
las lecturas, comparaciones y temporales, con espacio suficiente para dos lecturas completas
y sus comprobaciones según la capacidad flash viva. Todavía no hay una copia validada.

**Nota histórica:** la imagen cifrada existente y `recovery-mount` se conservan intactos.
El flujo de cifrado fallido no se reintenta ni se diagnostica como requisito de esta recuperación;
no crear, desbloquear, montar, reemplazar ni borrar la imagen para continuar.

### Prueba de recuperación pendiente, dentro de las lecturas ya autorizadas

1. Capturar la flash completa de tamaño probado y una segunda lectura independiente, con
   operaciones acotadas y sin arrancar la aplicación entre ellas. Comparar bytes, tamaños y
   SHA-256 dentro de `recovery-local`; no volcar contenido ni secretos en terminal/chat.
2. Releer los archivos y comprobar identidad y hashes, tabla/imagen antigua y cobertura del
   intervalo recuperable. Registrar solo un resumen no sensible. Fallo de lectura, espacio
   o comparación implica **HOLD**, no una copia válida.

Una copia válida requiere lecturas consistentes y recuperación utilizable, **no cifrado ni
roundtrip de contraseña**. La mera legibilidad no garantiza restauración física: se requiere
un plan oficial compatible con la seguridad y el rango reales, revisado, sin restauración de
prueba destructiva. No hay dirección de restauración aprobada.

## 4. Preflight vivo ya autorizado; próxima autorización solo para efectos adicionales

El usuario ya autorizó **abrir el puerto confirmado de esta placa**, identificarla, entrar/resetear
a ROM y leer identidad ESP32-S3, flash y seguridad, tabla de particiones y otadata. El intento
actual de lectura ROM está pendiente de resultado; no se afirma éxito ni hace falta pedir otra
vez esos permisos. Confirmar puerto vigente y respetar los resets autorizados y sus efectos;
`/dev/cu.usbmodem14101` y USB `239A:8125` son solo la enumeración pasiva previa.

La autorización distingue lectura de flash de ejecución en RAM: usar ROM sin stub;
si la lectura requiere cargar/ejecutar el stub oficial, detenerse y obtener permiso específico
antes de hacerlo. Las lecturas que creen archivos usan el directorio privado de la sección 3.
No publicar datos crudos de seguridad, NVS ni credenciales. No hay autorización de flash ni
restauración.

Mantener fijados puerto y opciones de reset, comprobar la ayuda oficial y revisar los efectos.
No hay aquí un script activo para copiar y pegar.

**HOLD de escritura** mientras falte cualquiera de estos puntos:

- Identidad viva, capacidad flash, seguridad (secure boot, cifrado, restricciones de descarga)
  y compatibilidad de la imagen. Una restricción o resultado desconocido no se salta con `--force`.
- Tabla viva validada, ubicación real de tabla/otadata y prueba oficial OTA/aplicación activa;
  no imponer los offsets históricos ni deducirlos del CSV local.
- Copia actual local privada sin cifrar, doble lectura consistente y recuperación revisada utilizable en Mac.
- Rango exacto de aplicación y borrado contenido en su partición, preservación de **todo** lo ajeno,
  revisión independiente favorable de este procedimiento nuevo y aprobación posterior de escritura.

## 5. Plan posterior de escritura solo de aplicación — NO ejecutar

Con todas las pruebas anteriores, pedir **otra aprobación**, que nombre binario/hash, puerto,
partición, intervalo, resets, stub si procede y lecturas posteriores. Usar únicamente esptool
oficial con una sola pareja dirección/imagen aprobada; nunca el upload genérico de PlatformIO.
No reescribir bootloader, tabla, NVS, otadata, otra aplicación, TinyUF2, ffat ni huecos.
Prohibidos erase total, `--force`, eFuses, reprovisión y cambios de seguridad.

Para inicio probado `A`, longitud `N` y sector `S` confirmado, el intervalo afectado es
`[floor(A/S)*S, ceil((A+N)/S)*S)`. Verificar que cabe íntegramente en la partición aprobada.
**Ejemplo HIPOTÉTICO, no dirección autorizada:** si `A=0x10000`, `N=873968` y `S=4096`,
la imagen termina en `0xe55f0` y el borrado en `0xe6000` exclusivo:
`[0x10000, 0xe6000)`, 876544 bytes. Los 2576 bytes finales del último sector también
quedan afectados y deben figurar en el plan de recuperación, no considerarse preservados.

Antes de arrancar la aplicación, mantener ROM y usar lectura oficial independiente:

1. Comparar los `N` bytes leídos con el archivo congelado, byte a byte y SHA-256;
   no aceptar solo el mensaje de éxito de escritura o `verify_flash`.
2. Comparar con el snapshot previo **todos los bytes fuera del intervalo sector-redondeado**,
   desde cero hasta la capacidad flash probada: incluye huecos, cola de aplicación,
   otras aplicaciones y todos los datos, no solo tabla/NVS/otadata. Trabajar dentro de `recovery-local`.
3. Ante cualquier diferencia inesperada, detenerse antes de arrancar: conservar pruebas protegidas,
   no reintentar, no restaurar automáticamente; solicitar diagnóstico y autorización delimitada.

Solo tras esas comprobaciones y el reset/arranque aprobado: verificar arranque, Wi-Fi y
calibración conservados, `/health`, recepción PNG, contadores de emisores y rechazos sin
incrementar frames. Comprobar NTP real, hora de Madrid, reloj tras 10 segundos sin PNG válido,
retorno inmediato al PNG, segundos/caminantes, rotación, margen y prioridad de la pantalla Wi-Fi.
La aplicación puede modificar datos al arrancar: separar esa fase del cotejo previo en ROM.
Registrar fallos y pendientes sin convertir pruebas host en evidencia física.
