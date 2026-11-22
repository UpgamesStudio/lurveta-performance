# Instalación y compilación

## Instalar el APK
1. Copia `LURVETA-Performance-1.0.0.apk` al teléfono e instálalo. Puede que tengas que permitir "instalar apps desconocidas" para tu gestor de archivos.
   Por adb: `adb install -r LURVETA-Performance-1.0.0.apk`.
2. Ábrelo y espera a que termine la primera instalación. Extrae el rootfs (unos 400 MB) en `files/rtfs`, y durante la extracción se reubican las rutas internas.
3. **Convivencia:** el paquete es `com.lurveta.co`, así que puede estar instalado a la vez que Winlator (`com.winlator`). No comparten datos ni contenedores.
4. Permisos: almacenamiento para las unidades D:/E: y para guardar benchmarks y logs en `Download/LURVETA`, y notificaciones para el servicio en segundo plano.

**Firma:** el APK entregado está firmado con una clave de desarrollo, igual que el de Winlator original, que es una variante debug. Si después instalas un build firmado con **otra** clave, Android pedirá desinstalar primero, y con ello se pierden los contenedores. Antes de distribuir, crea tu clave (ver abajo).

## Requisitos de uso
- Android 8.0+ (API 26), ARM64. Objetivo: Dimensity 8350 / Mali‑G615. ADPF necesita Android 13+.
- Solo funciona en el **usuario principal** de Android. La ruta `/data/data/<paquete>` está compilada en el rootfs, igual que en Winlator, así que no funciona en perfiles de trabajo ni en usuarios secundarios.

## Dónde quedan los datos
| Qué | Ruta |
|---|---|
| Benchmarks | `Download/LURVETA/benchmarks/<fecha>_<juego>/` (`frames.csv`, `sensors.csv`, `summary.txt`) |
| Errores (crash) | `Download/LURVETA/logs/` y `files/logs/` |
| Perfiles de controles exportados | `Download/LURVETA/profiles/` |
| Caché de texturas decodificadas (Vortek) | `/data/data/com.lurveta.co/cache/vortek` (tamaño en la configuración de Vortek) |

## Compilar desde el código
Requisitos verificados: JDK 17, Android SDK (platform 35), NDK `24.0.8215888`, CMake `3.22.1`. Gradle 8.14.5 se descarga con el wrapper.

```bash
echo "sdk.dir=/ruta/al/android-sdk" > local.properties
./gradlew assembleDebug
# -> app/build/outputs/apk/debug/app-debug.apk
```

Si `repo.maven.apache.org` responde **429**, que es un límite temporal de Maven Central, usa un init script local de Gradle que redirija a `https://maven-central.storage-download.googleapis.com/maven2/`. No hace falta modificar el proyecto.

### Clave de firma propia (recomendado antes de distribuir)
```bash
keytool -genkeypair -v -keystore lurveta-release.jks -alias lurveta -keyalg RSA -keysize 4096 -validity 10000
```
Configura `signingConfigs` leyendo la ruta y las contraseñas desde `~/.gradle/gradle.properties`. **No subas nunca la clave ni las contraseñas al repositorio.** Guarda copia de la clave: sin ella no se pueden publicar actualizaciones.

## Pruebas en host (Linux)
```bash
VK_ICD_FILENAMES=/ruta/a/lvp_icd.json ./tools/host-tests/run.sh
```
Necesita gcc y el loader de Vulkan con un ICD, por ejemplo Mesa lavapipe. Crea `/data/data/com.lurveta.co/cache` para probar la caché.
