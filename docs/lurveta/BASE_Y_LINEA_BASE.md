# LURVETA Performance 2.x sobre Winlator Mali: registro de la base y línea base

## Base importada
- Origen: `github.com/GunaCharanTeja/WinlatorMali`, commit `0411a4b602ed00d71bef52e6a7ccb82a8e709b1a`.
  - Árbol importado **idéntico**: hash de árbol `1f14ed1f62301919afe8b2e80e038b7e40666abe` en los dos repositorios.
  - Submódulo `app/src/main/cpp/adrenotools` en `8483dfd`, con el submódulo anidado `lib/linkernsbypass` en `b10d485`.
- Licencia: MIT (BrunoSX) y las de cada componente; ver `docs/AUDITORIA_WINLATOR_MALI.md` en la rama de trabajo LURVETA.
- Recurso externo: `imagefs.tzst` se descarga al compilar. SHA-256 comprobado: idéntico al del APK 1.2.1 publicado (`0c8c6c1c76537663…`).

## Reproducibilidad (comprobado el 2026-11-23)
Comparación del APK compilado desde el código (variante `standard`, sin cambios) con el APK publicado `Winlator_Mali_1.2.1_Bionic_Ludashi.apk`:
- **Assets:** 109 de 112 idénticos byte a byte. Difieren `dexopt/baseline.prof(m)` y la licencia de la fuente Inter, esta solo en los saltos de línea.
- **Bibliotecas nativas:** misma sección `.text` y mismas funciones exportadas. Solo cambian 152 bytes de metadatos de compilación.
- **Java:** `classes2.dex` idéntico; `classes.dex` difiere en unos 1,8 KB (variante `standard` frente a `ludashi`).

Conclusión: el código fuente reproduce el motor del APK que funciona en el dispositivo.

## Cambios de identidad (commit de identidad LURVETA)
- `applicationId com.lurveta.co`, `versionCode 100`, `versionName 2.0.0-dev1`, nombre "LURVETA Performance".
- Una sola variante: sin `com.winlator.mali`, `com.ludashi.benchmark` ni `com.antutu.ABenchMark`.
- Icono de LURVETA (`lurveta_mark`), y nombre en la cabecera del menú, en "Acerca de" y en el asistente de Wine.
  - Los créditos de Winlator Mali, Bionic, Winlator, leegao y el resto se conservan.
- Firma: la clave de LURVETA (certificado SHA-256 `1a7a4800…bfb167e`).
- **Sin cambios** en el motor: Wine/Proton, FEX, WOWBox64, Box64, Wrapper, capa BCn, DXVK, audio y descargadores.

Conservado a propósito:
- El paquete Java `com.winlator.cmod`, para poder seguir comparando con el original.
- La marca "Winlator Mali" dentro de las copias de seguridad de partidas (`GameSaveBackup`), para que sigan siendo compatibles con las que hizo Winlator Mali.

## Etapa A: línea base de GTA V (pendiente de datos del dispositivo)
Lo que **no** se puede medir desde aquí: FPS, frametime, temperatura y consumo solo existen en el teléfono. No se inventan cifras.

Configuración a registrar (contenedor y acceso directo de GTA V en Winlator Mali Ludashi):

| Dato | Valor |
|---|---|
| Wine/Proton (Ajustes del contenedor → Versión de Wine) | |
| Emulador (FEXCore / WOWBox64 / Box64) y versión | |
| Preset FEX / Box64 | |
| Driver gráfico (Wrapper / variante) y versión | |
| Emulación BCn (auto/full/none), transcodificación y caché | |
| DXVK (versión, async) / VKD3D | |
| Resolución del contenedor y ajustes gráficos del juego | |
| Modo de presentación, Max image count, Disable present wait | |
| Variables de entorno | |
| Audio (ALSA / PulseAudio / PulseAudio-GN) | |
| CPU list / afinidad, Sustained performance, RAM Booster | |

Mediciones, con el mismo punto de guardado, sin cargador y a temperatura ambiente anotada:

| Minuto | FPS medio | 1 % low | Temperatura | RAM del juego | Incidencias |
|---|---|---|---|---|---|
| 0–5 | | | | | |
| 15 | | | | | |
| 30 | | | | | |
| 45 | | | | | |

Prueba del paquete Ludashi:
- Misma configuración en Winlator Mali `standard` (`com.winlator.mali`) y en LURVETA 2.0.0-dev1 (`com.lurveta.co`).
- Si Ludashi rinde más con todo lo demás igual, la diferencia es el perfil del fabricante, no el motor.
