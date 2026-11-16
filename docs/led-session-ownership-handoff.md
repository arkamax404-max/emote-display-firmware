# Reserva del display LED por sesion: nota para retomar

**Estado: acuerdo funcional; pendiente de implementar y verificar.** Esta publicacion solo guarda la idea para continuar desde otro equipo. No modifica codigo ni autoriza grabar o actualizar la placa.

## Objetivo

Al ejecutar /emote displayOn, el emisor debe enviar una orden especial al firmware para que el LED atienda las imagenes de esa sesion y no permita que otras sesiones las sobrescriban.

Repositorios implicados:
- Emisor y comando: https://github.com/arkamax404-max/gentle-emotex
- Receptor LED: https://github.com/arkamax404-max/emote-display-firmware

## Comportamiento acordado

1. La ultima orden explicita /emote displayOn toma el control, aunque otra sesion ya lo tuviera. No se debe confundir una imagen ordinaria con una nueva orden de toma de control.
2. Mientras existe un propietario, las imagenes de otras sesiones se rechazan o ignoran sin reemplazar lo mostrado ni renovar la reserva.
3. /emote displayOff libera la reserva de la sesion propietaria. Una sesion antigua no debe poder liberar la reserva de la que le quito el control.
4. La reserva vence tras 30 segundos sin senales de vida del propietario. La sesion puede estar inactiva y conservar el control si sigue enviando dichas senales.
5. La identidad debe distinguir sesiones, no solo equipos o IP: dos sesiones en el mismo ordenador deben poder disputarse el LED mediante displayOn.

Estas reglas fueron elegidas por el usuario: ultima orden gana; liberacion mediante displayOff o perdida de conexion con vencimiento de 30 segundos.

## Estado actual y limites

- En el emisor, displayOn/displayOff habilitan o pausan los destinos no loopback; todavia no negocian la propiedad con el firmware.
- El emisor dispone de identidad de sesion en X-Emote-Session-Id cuando el host la proporciona, y de streams y secuencias. Su existencia no implica que el firmware aplique exclusividad.
- El contrato publicado del firmware indica que el ultimo PNG valido sustituye al anterior y que no ordena ni descarta por stream/sequence. No hay exclusividad por sesion verificada.
- El retorno autonomo del firmware al reloj tras 10 segundos sin PNG valido es un temporizador distinto. No debe confundirse con la reserva propuesta de 30 segundos.
- Este filtro es coordinacion entre sesiones de confianza en la LAN, no autenticacion: no hay una promesa de proteccion frente a clientes maliciosos que suplanten identificadores.

## Trabajo para la proxima sesion

- Revisar las versiones actuales de ambos repositorios antes de editar. Los cambios locales pendientes del emisor NO se publican mediante esta nota; no asumir que ya estan en su rama remota.
- Definir las peticiones y respuestas de toma de control, liberacion y senales de vida. No se han elegido rutas HTTP ni codigos de respuesta.
- Resolver antes de implementar que ocurre sin propietario o tras vencer la reserva: aceptar imagenes libremente o exigir otra orden displayOn. Esa politica no quedo decidida.
- Resolver compatibilidad con emisores antiguos o sin identidad de sesion, firmware antiguo, reinicio de la placa y preferencias guardadas. Una restauracion automatica no deberia suplantar sin mas una nueva orden explicita.
- Implementar emisor y receptor sin cambiar el comportamiento de escritorio, simuladores ni D200. Renovar la reserva tambien durante inactividad y reloj, sin depender de subir PNGs repetidos.
- Guardar el estado de propiedad de forma coherente con la cola de peticiones y los cambios de sesion; preservar la imagen y la reserva ante peticiones invalidas.
- Actualizar ayuda, documentacion y pruebas del protocolo. Compilar y ejecutar las comprobaciones aplicables antes de pedir una prueba fisica.

## Escenarios de aceptacion propuestos

- A ejecuta displayOn: sus imagenes se muestran; una imagen ordinaria de B no las sustituye.
- B ejecuta displayOn: toma el control inmediatamente; A ya no puede sobrescribirlo con imagenes ordinarias.
- A ejecuta displayOff despues de la transferencia: no libera la reserva de B.
- B ejecuta displayOff: libera su reserva y pausa su envio al LED.
- B no envia imagenes pero mantiene las senales de vida: conserva el control, incluido mientras el reloj esta activo.
- B desaparece: la reserva vence tras 30 segundos sin senales de vida; las peticiones de otra sesion no prolongan ese plazo.
- Repetir con A y B en el mismo equipo, con datos invalidos y con firmware o emisor antiguos segun la compatibilidad que se decida.

## Limite de autorizacion

La instruccion de cierre fue dejar unicamente la anotacion y subirla al repositorio para retomarla desde otro equipo. No se implemento esta funcionalidad ni se grabo la placa. Una futura carga de firmware necesita autorizacion explicita independiente.
