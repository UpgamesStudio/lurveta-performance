package com.winlator.core;

/**
 * LURVETA Performance — interpretation of a game launch from observed facts. No Android classes, so
 * it is unit-tested on the host (tools/host-tests/LaunchAnalysisTest.java).
 *
 * The stages form a chain: the highest one reached tells where the launch stopped. The verdict is a
 * HEURISTIC summary written for the user; the evidence is always the raw sections of the report.
 */
public final class LaunchAnalysis {
    public static final int STAGE_ENV_READY = 0;       // the app prepared the X server / Vortek / audio components
    public static final int STAGE_BOX64_STARTED = 1;   // the box64 process was created
    public static final int STAGE_WINE_RUNNING = 2;    // a wine / wineserver process exists
    public static final int STAGE_EXPLORER = 3;        // explorer.exe (Wine desktop) exists
    public static final int STAGE_GAME_PROCESS = 4;    // the game's own Windows process exists
    public static final int STAGE_VULKAN_CLIENT = 5;   // the game opened a Vulkan connection to Vortek
    public static final int STAGE_VULKAN_INSTANCE = 6; // vkCreateInstance succeeded (DXVK/vkd3d initialising)
    public static final int STAGE_VULKAN_DEVICE = 7;   // vkCreateDevice succeeded
    public static final int STAGE_FIRST_PRESENT = 8;   // the first frame was presented

    private static final String[] STAGE_NAMES = {
        "Entorno de la app listo (X server, Vortek, audio)",
        "Box64 lanzado",
        "Wine / wineserver en ejecución",
        "Escritorio de Wine (explorer.exe)",
        "Proceso Windows del juego creado",
        "Vortek: el juego abrió una conexión Vulkan",
        "Vulkan: instancia creada (inicio de DXVK/vkd3d)",
        "Vulkan: dispositivo creado",
        "Primer fotograma presentado"
    };

    private LaunchAnalysis() {}

    public static String stageName(int stage) {
        return stage >= 0 && stage < STAGE_NAMES.length ? STAGE_NAMES[stage] : "?";
    }

    public static int stageCount() {
        return STAGE_NAMES.length;
    }

    public static String signalName(int signal) {
        switch (signal) {
            case 1: return "SIGHUP";
            case 2: return "SIGINT";
            case 3: return "SIGQUIT";
            case 4: return "SIGILL (instrucción ilegal)";
            case 6: return "SIGABRT (abortado)";
            case 7: return "SIGBUS";
            case 8: return "SIGFPE";
            case 9: return "SIGKILL (matado; p. ej. falta de memoria o cierre de la app)";
            case 11: return "SIGSEGV (violación de memoria)";
            case 13: return "SIGPIPE";
            case 15: return "SIGTERM";
            default: return "señal " + signal;
        }
    }

    /** Java reports "killed by signal N" as 128+N. */
    public static String describeExit(int status) {
        if (status > 128 && status < 128 + 65) return "terminado por " + signalName(status - 128) + " (código " + status + ")";
        return status == 0 ? "código 0 (salida normal)" : "código de salida " + status;
    }

    /** Highest stage whose flag is set, or -1. */
    public static int lastStage(boolean[] reached) {
        int last = -1;
        for (int i = 0; i < reached.length && i < STAGE_NAMES.length; i++) if (reached[i]) last = i;
        return last;
    }

    /**
     * @param execError   reason box64 could not even be started (null if it started)
     * @param reached     per-stage flags (length >= stageCount())
     * @param exited      the guest process already ended
     * @param status      exit status when exited
     * @param elapsedMs   time from launch to exit (or to now)
     */
    public static String verdict(String execError, boolean[] reached, boolean exited, int status, long elapsedMs) {
        if (execError != null) {
            return "No se pudo lanzar Box64 (" + execError + "). Ningún componente invitado llegó a iniciarse: revisa que el sistema de archivos "
                 + "(rootfs) esté instalado y que el binario box64 exista y sea ejecutable.";
        }
        int last = lastStage(reached);
        String where = last >= 0 ? "Último punto alcanzado: «" + stageName(last) + "»." : "No se observó ningún componente invitado.";
        if (!exited) {
            if (last >= STAGE_FIRST_PRESENT) return "En ejecución: se están presentando fotogramas. " + where;
            return "Todavía sin terminar. " + where + " Si el juego parece congelado, este es el punto donde se quedó.";
        }

        String how = describeExit(status) + " tras " + (elapsedMs / 1000) + " s.";
        if (last < STAGE_WINE_RUNNING) {
            return "Box64 terminó " + (elapsedMs < 3000 ? "casi al instante " : "") + "sin llegar a ejecutar Wine (" + how + "). "
                 + "La causa suele estar en la salida de Box64 (biblioteca que no carga, binario incompatible, falta de memoria). " + where;
        }
        if (last < STAGE_GAME_PROCESS) {
            return "Wine arrancó pero el proceso Windows del juego nunca llegó a crearse (" + how + "). Mira las líneas «err:» de Wine "
                 + "(DLL que falta, ejecutable no encontrado, error del prefijo). " + where;
        }
        if (last < STAGE_VULKAN_CLIENT) {
            return "El juego se creó pero nunca abrió Vulkan/Vortek (" + how + "). Puede ser un fallo antes de iniciar DXVK, que use "
                 + "otro renderizador o que Wine/DXVK no cargara. Mira el log de DXVK y las líneas «err:» de Wine. " + where;
        }
        if (last < STAGE_VULKAN_DEVICE) {
            return "Vulkan se conectó pero no se creó el dispositivo (" + how + "). Mira «vulkan_call_failures» y el log de DXVK. " + where;
        }
        if (last < STAGE_FIRST_PRESENT) {
            return "Se creó el dispositivo Vulkan pero no se presentó ningún fotograma (" + how + "): fallo al cargar recursos, shaders o "
                 + "pipelines, o cuelgue. Mira los fallos Vulkan y las líneas de DXVK. " + where;
        }
        return "El juego presentó fotogramas y luego terminó (" + how + "). Si no lo cerraste tú, es un cierre inesperado. " + where;
    }

    /** True when the user should be told something after returning to the menu. */
    public static boolean isAbnormalEnd(String execError, boolean[] reached, int status, long elapsedMs) {
        if (execError != null) return true;
        if (status != 0) return true;
        return lastStage(reached) < STAGE_FIRST_PRESENT && elapsedMs < 120_000;
    }
}
