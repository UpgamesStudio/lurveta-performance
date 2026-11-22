package com.winlator.contentdialog;

import android.content.Context;
import android.view.View;
import android.widget.Spinner;

import com.winlator.R;
import com.winlator.core.AppUtils;
import com.winlator.core.GPUHelper;
import com.winlator.core.KeyValueSet;
import com.winlator.core.StringUtils;
import com.winlator.widget.MultiSelectionComboBox;
import com.winlator.xenvironment.components.VortekRendererComponent;

public class VortekConfigDialog extends ContentDialog {
    public static final String DEFAULT_VK_MAX_VERSION = GPUHelper.vkVersionMajor(VortekRendererComponent.VK_MAX_VERSION)+"."+GPUHelper.vkVersionMinor(VortekRendererComponent.VK_MAX_VERSION);

    public VortekConfigDialog(final View anchor) {
        super(anchor.getContext(), R.layout.vortek_config_dialog);
        Context context = anchor.getContext();
        setIcon(R.drawable.icon_display_settings);
        setTitle("Vortek "+context.getString(R.string.configuration));

        final Spinner sVkMaxVersion = findViewById(R.id.SVkMaxVersion);
        final Spinner sMaxDeviceMemory = findViewById(R.id.SMaxDeviceMemory);
        final Spinner sImageCacheSize = findViewById(R.id.SImageCacheSize);
        final Spinner sResourceMemoryType = findViewById(R.id.SResourceMemoryType);
        final MultiSelectionComboBox mscbExposedExtensions = findViewById(R.id.MSCBExposedExtensions);
        final android.widget.CheckBox cbPerformanceHints = findViewById(R.id.CBPerformanceHints);
        final Spinner sTextureUploadMode = findViewById(R.id.STextureUploadMode);
        final android.widget.CheckBox cbPipelineCache = findViewById(R.id.CBPipelineCache);
        final android.widget.CheckBox cbBcHigh = findViewById(R.id.CBBcHighFormats);

        final String[] deviceExtensions = GPUHelper.vkGetDeviceExtensions();
        mscbExposedExtensions.setPopupWindowWidth(360);
        mscbExposedExtensions.setDisplayText(context.getString(R.string.multiselection_combobox_display_text));
        mscbExposedExtensions.setItems(deviceExtensions);

        KeyValueSet config = new KeyValueSet(anchor.getTag());

        String exposedDeviceExtensionsVal = config.get("exposedDeviceExtensions", "all");
        if (exposedDeviceExtensionsVal.contains("|")) exposedDeviceExtensionsVal = "all";

        if (exposedDeviceExtensionsVal.equals("all")) {
            mscbExposedExtensions.setSelectedItems(deviceExtensions);
        }
        else if (!exposedDeviceExtensionsVal.isEmpty()) {
            String[] selectedItems = exposedDeviceExtensionsVal.split(":");
            mscbExposedExtensions.setSelectedItems(selectedItems.length > 1 ? selectedItems : deviceExtensions);
        }

        AppUtils.setSpinnerSelectionFromValue(sVkMaxVersion, config.get("vkMaxVersion", DEFAULT_VK_MAX_VERSION));
        AppUtils.setSpinnerSelectionFromMemorySize(sMaxDeviceMemory, config.get("maxDeviceMemory", "0"));
        AppUtils.setSpinnerSelectionFromNumber(sImageCacheSize, config.get("imageCacheSize", String.valueOf(VortekRendererComponent.IMAGE_CACHE_SIZE)));
        sResourceMemoryType.setSelection(config.getInt("resourceMemoryType"));
        cbPerformanceHints.setChecked(config.getBoolean("performanceHints", true));
        cbPipelineCache.setChecked(config.getBoolean("pipelineCache", true));
        cbBcHigh.setChecked(config.getBoolean("bcHighFormats", true));
        int textureUploadMode = config.getInt("textureUploadMode", 0);
        sTextureUploadMode.setSelection(textureUploadMode >= 0 && textureUploadMode < sTextureUploadMode.getCount() ? textureUploadMode : 0);

        setOnConfirmCallback(() -> {
            KeyValueSet newConfig = new KeyValueSet();
            newConfig.put("vkMaxVersion", StringUtils.parseNumber(sVkMaxVersion.getSelectedItem(), "0"));
            newConfig.put("maxDeviceMemory", StringUtils.parseMemorySize(sMaxDeviceMemory.getSelectedItem()));
            newConfig.put("imageCacheSize", StringUtils.parseNumber(sImageCacheSize.getSelectedItem()));
            newConfig.put("resourceMemoryType", sResourceMemoryType.getSelectedItemPosition());
            newConfig.put("performanceHints", cbPerformanceHints.isChecked() ? "1" : "0");
            newConfig.put("textureUploadMode", sTextureUploadMode.getSelectedItemPosition());
            newConfig.put("pipelineCache", cbPipelineCache.isChecked() ? "1" : "0");
            newConfig.put("bcHighFormats", cbBcHigh.isChecked() ? "1" : "0");

            String[] selectedItems = mscbExposedExtensions.getSelectedItems();
            if (selectedItems.length > 0) {
                if (selectedItems.length == deviceExtensions.length) {
                    newConfig.put("exposedDeviceExtensions", "all");
                }
                else newConfig.put("exposedDeviceExtensions", String.join(":", selectedItems));
            }
            anchor.setTag(newConfig.toString());
        });
    }

    public static boolean isRequireRestart(String oldGraphicsDriverConfig, String newGraphicsDriverConfig) {
        // LURVETA: custom (adrenotools) driver loading was removed, so a Vortek configuration
        // change never requires reloading the Vulkan library.
        return false;
    }
}