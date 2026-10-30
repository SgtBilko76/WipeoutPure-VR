package online.portroyale.wipeoutpurevr;

import android.os.Bundle;

import io.github.teamgdb.portablekit.GameActivity;

/**
 * The app's activity: PortableKit's (SDL's) game activity, which also starts the HD texture download on the first
 * start (HdTextures; it runs beside the game and never holds it up).
 */
public class WipeoutActivity extends GameActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        HdTextures.start(getExternalFilesDir(null));
    }
}
