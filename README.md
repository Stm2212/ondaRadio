# ONDA · Radio portátil de internet

Firmware para **Waveshare ESP32-S3-Touch-LCD-1.54**, desarrollado en C con
**ESP-IDF 6.1, FreeRTOS y LVGL 9**. Reproduce emisoras de internet por el parlante
integrado y se controla desde la pantalla táctil, botones físicos y un portal web
local.

> **Prototipo académico en validación.** No está calificado comercialmente ni
> certificado en seguridad, autonomía, temperatura o compatibilidad general.

> **Licencia pendiente.** Todavía no se definió una licencia para el firmware
> propio. La publicación del código no implica autorización para reutilizarlo o
> redistribuirlo. No publicar este repositorio hasta resolver ese punto.

## Funciones

- Reproducción de flujos MP3 y AAC/AAC+ compatibles.
- Búsqueda de emisoras por popularidad, país, género, idioma o formato; listas de
  favoritas y recientes.
- Portal local para configurar Wi-Fi de 2,4 GHz y administrar emisoras desde un
  teléfono.
- Temas, acentos, reloj, protectores de pantalla, temporizador y apagado.
- Persistencia de red, biblioteca, emisora actual, volumen y preferencias.

La disponibilidad y continuidad de las emisoras dependen de Radio Browser y de
servidores de terceros.

## Hardware y controles

| Elemento | Plataforma |
|---|---|
| MCU y memoria | ESP32-S3R8, 8 MB PSRAM, 16 MB flash |
| Pantalla y táctil | ST7789, 240 × 240; CST816S por I²C |
| Audio | Códec ES8311 y amplificador NS4150B |
| Batería/carga | Porcentaje estimado por voltaje; estado de carga ETA6098 |

- **PLUS**, GPIO4: subir volumen.
- **BOOT**, GPIO0: bajar volumen.
- Botón central, GPIO5: pausar/reanudar; mantenerlo presionado solicita apagado.
- **PWR**: encender el equipo.

El nivel de batería es una estimación, no una lectura de un medidor dedicado.

## Uso básico

1. En **Ajustes → Wi-Fi y portal**, activá el portal y escaneá el QR para conectar
   el teléfono al punto **ONDA-xxxxxx**. La pantalla también permite consultar la
   clave manualmente.
2. Abrí `http://192.168.4.1` en el navegador; el QR no abre la página
   automáticamente. Elegí una red de 2,4 GHz y enviá sus datos.
3. Para buscar y guardar emisoras, usá **Buscar y ordenar radios favoritas** o
   abrí `http://192.168.4.1/radios`.
4. En **Radios**, elegí Favoritas, Recientes o Explorar. En **Escuchar**, cambiá
   de emisora y controlá la reproducción; los botones físicos ajustan el volumen.

## Compilar

Se requiere **ESP-IDF 6.1** y acceso al registro de componentes de Espressif en
una compilación limpia. Las dependencias directas están declaradas en
`main/idf_component.yml` y fijadas en `dependencies.lock`.

Con el entorno de ESP-IDF activado:

```sh
idf.py set-target esp32s3
idf.py build
```

La configuración generada (`sdkconfig`) y los componentes descargados se omiten
del repositorio; se regeneran durante la compilación a partir de
`sdkconfig.defaults` y `dependencies.lock`.

## Carga de firmware

Para actualizar una placa que ya tiene la tabla de particiones compatible, se
puede escribir **solo la aplicación**:

```sh
idf.py -p PORT app-flash
```

En esta configuración la aplicación comienza en `0x10000`. Este procedimiento
preserva el bootloader, la tabla de particiones y NVS. Verificá el modelo de
placa y el puerto antes de cargar. No uses `erase-flash` ni `flash` como una
actualización normal: pueden borrar datos o reescribir particiones. La primera
instalación en una placa sin esta tabla requiere un procedimiento de
aprovisionamiento específico que no se describe aquí.

## Seguridad y privacidad

- El punto de acceso está protegido con WPA2. Quien conozca su clave puede
  administrar el portal mientras está activo.
- El portal usa HTTP local, sin TLS. Las credenciales Wi-Fi se almacenan en NVS
  sin cifrado.
- No se declara protección contra lectura o sustitución física de la flash.
- Antes de compartir registros, revisá que no incluyan credenciales, cookies,
  SSID ni datos privados de diagnóstico.

## Licencias de terceros

Las fuentes incluidas bajo `main/ui/fonts/` conservan los avisos y licencias SIL
Open Font License 1.1. Las dependencias descargadas por ESP-IDF conservan sus
licencias originales. La licencia del firmware propio sigue pendiente.

## Referencias

- [Documentación de la placa Waveshare](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54)
- [Documentación de ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/)
- [API de Radio Browser](https://docs.radio-browser.info/)
