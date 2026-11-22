package com.winlator.contentdialog;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.text.Html;
import android.text.method.LinkMovementMethod;
import android.view.View;
import android.widget.TextView;

import com.winlator.BuildConfig;
import com.winlator.R;

public class AboutDialog extends ContentDialog {
    public AboutDialog(Context context) {
        super(context, R.layout.about_dialog);
        findViewById(R.id.LLBottomBar).setVisibility(View.GONE);

        try {
            final PackageInfo pInfo = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);

            // LGPL-2.1: tell users where the corresponding source code is available.
            TextView tvWebpage = findViewById(R.id.TVWebpage);
            String sourceUrl = BuildConfig.SOURCE_URL;
            tvWebpage.setText(Html.fromHtml("<a href=\""+sourceUrl+"\">"+sourceUrl.replace("https://", "")+"</a>", Html.FROM_HTML_MODE_LEGACY));
            tvWebpage.setMovementMethod(LinkMovementMethod.getInstance());

            ((TextView)findViewById(R.id.TVAppVersion)).setText(context.getString(R.string.version)+" "+pInfo.versionName+" · "+BuildConfig.UPSTREAM_BASE);

            String creditsAndThirdPartyAppsHTML = String.join("<br />",
                "<b>LURVETA Performance is based on Winlator</b> by BrunoSX (<a href=\"https://github.com/brunodev85/winlator\">github.com/brunodev85/winlator</a>), licensed under the GNU LGPL-2.1. Copyright notices of the original authors are retained.",
                "",
                "Vortek and Gladio by BrunoSX (LGPL-2.1)",
                "GLIBC Patches by (<a href=\"https://github.com/termux-pacman/glibc-packages\">Termux Pacman</a>)",
                "Wine (<a href=\"https://www.winehq.org\">winehq.org</a>)",
                "Box86/Box64 by <a href=\"https://github.com/ptitSeb\">ptitseb</a>",
                "Mesa (Zink/VirGL) (<a href=\"https://www.mesa3d.org\">mesa3d.org</a>)",
                "DXVK (<a href=\"https://github.com/doitsujin/dxvk\">github.com/doitsujin/dxvk</a>)",
                "VKD3D (<a href=\"https://gitlab.winehq.org/wine/vkd3d\">gitlab.winehq.org/wine/vkd3d</a>)",
                "CNC DDraw (<a href=\"https://github.com/FunkyFr3sh/cnc-ddraw\">github.com/FunkyFr3sh/cnc-ddraw</a>)",
                "FluidSynth, PulseAudio, ALSA (LGPL)",
                "zstd-jni (BSD), Apache Commons Compress, AndroidX (Apache-2.0), XZ for Java (public domain)",
                "",
                "Full license texts: licenses/ inside the APK and the source repository."
            );

            TextView tvCreditsAndThirdPartyApps = findViewById(R.id.TVCreditsAndThirdPartyApps);
            tvCreditsAndThirdPartyApps.setText(Html.fromHtml(creditsAndThirdPartyAppsHTML, Html.FROM_HTML_MODE_LEGACY));
            tvCreditsAndThirdPartyApps.setMovementMethod(LinkMovementMethod.getInstance());
        }
        catch (PackageManager.NameNotFoundException e) {}
    }
}
