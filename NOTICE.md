# Avisos legales y componentes de terceros

**LURVETA Performance** es una obra derivada de **Winlator**:
Copyright © BrunoSX y colaboradores. Licencia GNU Lesser General Public License v2.1 (texto completo en [`LICENSE`](LICENSE); también se incluye en el APK como `assets/licenses/LGPL-2.1.txt`).
Repositorios originales: <https://github.com/brunodev85/winlator> y <https://github.com/brunodev85/winlator-app>.

Las modificaciones de LURVETA (ver [`CHANGELOG.md`](CHANGELOG.md)) se distribuyen bajo la misma licencia LGPL‑2.1.

**Obligación de distribución:** si distribuyes el APK, debes ofrecer a los destinatarios el código fuente correspondiente (este repositorio, en la versión exacta del APK). Mientras el repositorio sea privado, tienes que proporcionar ese código por otro medio. El enlace "Acerca de" de la app apunta a `BuildConfig.SOURCE_URL`.

## Componentes incluidos (se conservan sus licencias y avisos)

| Componente | Autor / origen | Licencia | Forma en el APK |
|---|---|---|---|
| Winlator (app, servidor X, Vortek y Gladio del lado servidor) | BrunoSX | LGPL‑2.1 | código fuente en este repo |
| Vortek (ICD del lado del juego) | BrunoSX, <https://github.com/brunodev85/vortek> | LGPL‑2.1 | binario `graphics_driver/vortek-2.1.tzst` |
| Gladio (lado del juego) | BrunoSX, <https://github.com/brunodev85/gladio> | LGPL‑2.1 | binario `graphics_driver/gladio-1.1.tzst` |
| Wine | winehq.org | LGPL‑2.1 | binario en el rootfs |
| Box64 | ptitSeb, <https://github.com/ptitSeb/box64> | MIT | binario `box64/box64-0.4.4.tzst` |
| GLIBC patches | Termux Pacman | LGPL‑2.1 (glibc) | rootfs |
| Mesa (Zink, VirGL) | mesa3d.org | MIT | binarios |
| virglrenderer | freedesktop.org | MIT | código fuente (`app/src/main/cpp/virglrenderer`) |
| DXVK, D8VK, D7VK | doitsujin y colaboradores | zlib | binarios |
| VKD3D(-Proton) | WineHQ / Hans‑Kristian Arntzen | LGPL‑2.1 | binario |
| cnc‑ddraw | FunkyFr3sh | MIT | binario |
| SwiftShader BC decoder (base de `bc_decoder.h`) | Google | Apache‑2.0 | código fuente adaptado |
| stb_dxt | Sean Barrett | dominio público / MIT | código fuente |
| FluidSynth, PulseAudio, ALSA, glib, libsndfile, libogg/vorbis/opus/FLAC | sus autores | LGPL / BSD | bibliotecas precompiladas |
| zstd‑jni | Luben Karavelov | BSD | dependencia Gradle |
| Apache Commons Compress, AndroidX, Material Components | Apache / Google | Apache‑2.0 | dependencias Gradle |
| XZ for Java | Tukaani | dominio público | dependencia Gradle |

**Componentes retirados en LURVETA** (ya no se distribuyen): Mesa Turnip (MIT) y libadrenotools (BSD‑2‑Clause, Billy Laws).

Los componentes descargables (versiones adicionales de Box64, DXVK, VKD3D o WineD3D) se obtienen del repositorio público de Winlator y conservan sus propias licencias.
