package com.winlator.container;

import android.content.Context;

import com.winlator.core.KeyValueSet;

public abstract class GraphicsDrivers {
    /**
     * LURVETA: Mesa Turnip only works on Qualcomm Adreno and was removed from LURVETA Performance
     * (MediaTek / ARM Mali build). The identifier is kept only to migrate existing settings:
     * any "turnip" selection is transparently mapped to Vortek, which uses the system Vulkan driver.
     */
    public static final String LEGACY_TURNIP = "turnip";
    public static final String VORTEK = "vortek";
    public static final String ZINK = "zink";
    public static final String VIRGL = "virgl";
    public static final String GLADIO = "gladio";
    public static final String DEFAULT_VULKAN_DRIVER = VORTEK;
    public static final String DEFAULT_OPENGL_DRIVER = GLADIO;

    public static String getName(String identifier) {
        switch (identifier) {
            case VORTEK: return "Vortek";
            case ZINK: return "Zink";
            case VIRGL: return "VirGL";
            case GLADIO: return "Gladio";
            default: return "None";
        }
    }

    public static boolean isVulkanDriver(String identifier) {
        return identifier != null && (identifier.equals(VORTEK) || identifier.equals(LEGACY_TURNIP));
    }

    public static boolean isOpenGLDriver(String identifier) {
        return identifier != null && (identifier.equals(ZINK) || identifier.equals(VIRGL) || identifier.equals(GLADIO));
    }

    public static String[] getItems(String apiName) {
        if (apiName.equalsIgnoreCase("VULKAN")) {
            return new String[]{getName(VORTEK)};
        }
        else if (apiName.equalsIgnoreCase("OPENGL")) {
            return new String[]{getName(ZINK), getName(VIRGL), getName(GLADIO)};
        }
        else return new String[0];
    }

    private static String migrate(String identifier) {
        return LEGACY_TURNIP.equals(identifier) ? VORTEK : identifier;
    }

    private static boolean isLegacyTurnip(String graphicsDriver) {
        return graphicsDriver != null && (graphicsDriver.equals(LEGACY_TURNIP) || graphicsDriver.startsWith(LEGACY_TURNIP + ","));
    }

    public static String[] parseIdentifiers(String graphicsDriver) {
        if (graphicsDriver == null || graphicsDriver.isEmpty()) return new String[]{DEFAULT_VULKAN_DRIVER, DEFAULT_OPENGL_DRIVER};
        if (graphicsDriver.contains(",")) {
            String[] identifiers = graphicsDriver.split(",");
            identifiers[0] = migrate(identifiers[0]);
            return identifiers;
        }
        else {
            if (isVulkanDriver(graphicsDriver)) {
                return new String[]{migrate(graphicsDriver), DEFAULT_OPENGL_DRIVER};
            }
            else if (isOpenGLDriver(graphicsDriver)) {
                return new String[]{DEFAULT_VULKAN_DRIVER, graphicsDriver};
            }
            else return new String[]{DEFAULT_VULKAN_DRIVER, DEFAULT_OPENGL_DRIVER};
        }
    }

    public static KeyValueSet[] parseConfigs(String graphicsDriver, String graphicsDriverConfig) {
        if (graphicsDriverConfig == null || graphicsDriverConfig.isEmpty()) return new KeyValueSet[]{new KeyValueSet(), new KeyValueSet()};
        KeyValueSet[] configs;
        int separator = graphicsDriverConfig.indexOf("|");
        if (separator != -1) {
            String first = graphicsDriverConfig.substring(0, separator);
            String second = graphicsDriverConfig.substring(separator + 1);
            configs = new KeyValueSet[]{new KeyValueSet(first), new KeyValueSet(second)};
        }
        else {
            if (isVulkanDriver(graphicsDriver)) {
                configs = new KeyValueSet[]{new KeyValueSet(graphicsDriverConfig), new KeyValueSet()};
            }
            else if (isOpenGLDriver(graphicsDriver)) {
                configs = new KeyValueSet[]{new KeyValueSet(), new KeyValueSet(graphicsDriverConfig)};
            }
            else configs = new KeyValueSet[]{new KeyValueSet(), new KeyValueSet()};
        }

        // A Turnip configuration means nothing to Vortek: start from Vortek defaults.
        if (isLegacyTurnip(graphicsDriver)) configs[0] = new KeyValueSet();
        return configs;
    }

    public static String getDefaultDriver(Context context) {
        return DEFAULT_VULKAN_DRIVER+","+DEFAULT_OPENGL_DRIVER;
    }
}
