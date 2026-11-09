# Preparar la actualización integrada desde macOS

**El candidato está congelado y comprobado sin conectar con la placa; todavía NO está listo para escribir.**
Esta preparación combina reloj autónomo y contadores de emisores. No instala herramientas,
no cambia credenciales y no autoriza abrir el puerto, pulsar BOOT/RESET, cargar un stub ni flashear.
Es un procedimiento nuevo: requiere su propia revisión independiente de solo lectura;
no reintenta ni traslada el verificador Windows que terminó sin dictamen.

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

## 3. Recuperación macOS protegida — aún no disponible

La copia histórica usa DPAPI ligada al perfil Windows: conservarla, pero **no cuenta como
recuperación utilizable desde este Mac**. Véase la [guía anterior](continue-on-macos.md).
No se ha creado imagen cifrada, montado volumen, pedido contraseña ni leído flash.

### Preparación humana después de revisar y aprobar esta fase

Se comprobaron las ayudas locales de `hdiutil create`, `attach` e `imageinfo` y el manual:
`-agentpass` fuerza el prompt de contraseña. **Sintaxis revisada, NO probada funcionalmente.**
Los siguientes pasos los ejecuta el humano, uno por uno, en su propio Terminal privado;
el agente no ejecuta creación/montaje ni recibe la contraseña. No usar grabación de sesión,
compartir pantalla ni guardar la clave en el llavero: desmarcar cualquier opción de recordarla.
Introducirla solo en el prompt privado; nunca en argumentos, variables, stdin automatizado,
archivos, chat o registros. Si no aparece el prompt esperado, cancelar y detenerse.

La propuesta es un sparsebundle APFS de **128 MiB**, no una copia de flash todavía.
Antes de crearlo, comprobar espacio libre del Mac; después, espacio útil del volumen.
Su capacidad debe cubrir dos lecturas completas, comprobaciones y margen de filesystem,
según la capacidad flash viva que aún falta probar. Si no basta, detenerse y revisar el tamaño;
no ampliar ni sustituir automáticamente una imagen existente.

**Paso A — rutas nuevas y creación, sin montaje implícito.** No ejecutar en paralelo con
otros procesos que modifiquen estas rutas. Cualquier fallo exige parar, no borrar ni reusar
lo encontrado. La comprobación física rechaza ancestros que redirijan fuera del staging.

```sh
cd /Users/santiagoperez/Development/emote-display-firmware/.tools/firmware-upgrade/clock-sender-26f3a07 || exit 1
test "$(pwd -P)" = /Users/santiagoperez/Development/emote-display-firmware/.tools/firmware-upgrade/clock-sender-26f3a07 || exit 1
umask 077
test ! -e recovery.sparsebundle && test ! -L recovery.sparsebundle || exit 1
test ! -e recovery-mount && test ! -L recovery-mount || exit 1
mkdir -m 700 recovery-mount || exit 1
hdiutil create -size 128m -type SPARSEBUNDLE -fs APFS -volname FirmwareRecovery -nospotlight -encryption AES-256 -agentpass recovery.sparsebundle || exit 1
```

**Paso B — inspeccionar antes de montar.** En ese mismo Terminal y directorio:

```sh
hdiutil imageinfo -plist -agentpass recovery.sparsebundle
```

Confirmar en la información de la imagen el cifrado **AES-256**; si falta, es ambiguo o
hay error, **HOLD**. No interpretar el mero éxito de creación como prueba de cifrado.

**Paso C — montaje explícito dentro del repositorio.** Rechazar enlaces simbólicos,
rutas no vacías o ya montadas; revisar que el directorio sigue siendo el recién creado.
No continuar si cambió el directorio de trabajo desde A.

```sh
test -d recovery-mount && test ! -L recovery-mount || exit 1
test -z "$(ls -A recovery-mount)" || exit 1
hdiutil attach -agentpass -mountpoint "$PWD/recovery-mount" recovery.sparsebundle
```

Comprobar en el resultado que el volumen desbloqueado corresponde a esta imagen y está
montado exactamente en el `recovery-mount` local, no en `/Volumes` ni en otra ruta.
**No crear ningún dump hasta confirmar cifrado, montaje real y espacio suficiente.**
Si attach falla, el directorio vacío NO es almacenamiento protegido; no escribir en él.
Dirigir todas las lecturas de flash, metadatos crudos, comparaciones y temporales al volumen,
nunca a un archivo sin protección para cifrarlo después. Un volumen montado expone datos al
usuario local: limitar acceso y desmontarlo al terminar. Ignorar una ruta en Git no la cifra.

### Prueba de recuperación pendiente, con autorización de lectura separada

1. Capturar la flash completa de tamaño probado y una segunda lectura independiente, sin
   arrancar la aplicación entre ellas. Comparar bytes, tamaños y SHA-256 dentro del volumen;
   no volcar contenido ni secretos en terminal/chat.
2. Desmontar, volver a desbloquear con la contraseña y releer los archivos; comprobar de
   nuevo identidad y hashes, tabla/imagen antigua y cobertura del intervalo recuperable.
   Registrar solo un resumen no sensible fuera del volumen. Fallo de lectura, desbloqueo,
   espacio o comparación implica **HOLD**, no una copia válida.

Crear/montar la imagen no demuestra recuperación utilizable. Ni una copia legible ni el
roundtrip garantizan por sí solos restauración física: se requiere un plan oficial compatible
con la seguridad y el rango reales, revisado, sin restauración de prueba destructiva.
No hay dirección de restauración aprobada.

## 4. Próxima autorización: preflight vivo, sin escritura de flash

Solicitar aprobación explícita para **abrir el puerto confirmado de esta placa**, identificarla,
entrar/resetear a ROM y leer identidad ESP32-S3, flash y seguridad, tabla de particiones y otadata.
Nombrar los resets previstos y sus efectos sobre la aplicación. Confirmar puerto vigente;
`/dev/cu.usbmodem14101` y USB `239A:8125` son solo la enumeración pasiva previa.

La aprobación debe distinguir lectura de flash de ejecución en RAM: intentar ROM sin stub;
si la lectura requiere cargar/ejecutar el stub oficial, detenerse y obtener permiso específico
antes de hacerlo. Las lecturas que creen archivos esperan al almacenamiento protegido validado.
No publicar datos crudos de seguridad, NVS ni credenciales.

Antes de redactar cualquier comando activo, fijar puerto, opciones de reset y si habrá stub,
comprobar su ayuda y revisar los efectos. No hay aquí un script activo para copiar y pegar.

**HOLD de escritura** mientras falte cualquiera de estos puntos:

- Identidad viva, capacidad flash, seguridad (secure boot, cifrado, restricciones de descarga)
  y compatibilidad de la imagen. Una restricción o resultado desconocido no se salta con `--force`.
- Tabla viva validada, ubicación real de tabla/otadata y prueba oficial OTA/aplicación activa;
  no imponer los offsets históricos ni deducirlos del CSV local.
- Copia actual protegida, doble lectura consistente, roundtrip y recuperación revisada utilizable en Mac.
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
   otras aplicaciones y todos los datos, no solo tabla/NVS/otadata. Trabajar dentro del volumen cifrado.
3. Ante cualquier diferencia inesperada, detenerse antes de arrancar: conservar pruebas protegidas,
   no reintentar, no restaurar automáticamente; solicitar diagnóstico y autorización delimitada.

Solo tras esas comprobaciones y el reset/arranque aprobado: verificar arranque, Wi-Fi y
calibración conservados, `/health`, recepción PNG, contadores de emisores y rechazos sin
incrementar frames. Comprobar NTP real, hora de Madrid, reloj tras 10 segundos sin PNG válido,
retorno inmediato al PNG, segundos/caminantes, rotación, margen y prioridad de la pantalla Wi-Fi.
La aplicación puede modificar datos al arrancar: separar esa fase del cotejo previo en ROM.
Registrar fallos y pendientes sin convertir pruebas host en evidencia física.
